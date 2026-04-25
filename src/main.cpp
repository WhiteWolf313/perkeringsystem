/*
 * SMART PARKING SYSTEM - PROFESSIONELL MED TIDSBERÄKNING
 * Hårdvara: ESP32 DevKitC V4 (38-pin), 2x RFID, 2x Servo, 2x LCD, 4x HC-SR04, 1x Buzzer (Active LOW)
 * Databas: Redis via Wi-Fi (med LittleFS Offline-stöd)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Redis.h>
#include <map> // <-- NYTT BIBLIOTEK FÖR ATT SPARA TIDER!

// =======================================================
// 1. INSTÄLLNINGAR & VARIABLER
// =======================================================
const char* ssid = "TN_wifi_A28B37";
const char* password = "5D35C2E6D2";
const char* redis_host = "192.168.10.199";
const int redis_port = 6379;
const char* queueFile = "/queue.txt";

int freeSpots = 50; 
WiFiClient redisClient;

// Skapa en "ordlista" i minnet för att spara bilarnas starttid
std::map<String, unsigned long> parkeringstider;

// =======================================================
// 2. PIN-DEFINITIONER
// =======================================================
#define BUZZER_PIN 2

#define RST_PIN 27
#define SS_IN_PIN 5
#define SS_OUT_PIN 4

#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 14

#define TRIG_IN_BEFORE 32
#define ECHO_IN_BEFORE 34
#define TRIG_IN_AFTER 33
#define ECHO_IN_AFTER 35

#define TRIG_OUT_BEFORE 25
#define ECHO_OUT_BEFORE 36
#define TRIG_OUT_AFTER 26
#define ECHO_OUT_AFTER 39

// =======================================================
// 3. OBJEKT & TILLSTÅND
// =======================================================
LiquidCrystal_I2C lcdIn(0x27, 16, 2); 
LiquidCrystal_I2C lcdOut(0x26, 16, 2); 

MFRC522 rfidIn(SS_IN_PIN, RST_PIN);
MFRC522 rfidOut(SS_OUT_PIN, RST_PIN);

Servo servoIn;
Servo servoOut;

bool gateInOpen = false;
unsigned long gateInTimer = 0;

bool gateOutOpen = false;
unsigned long gateOutTimer = 0;

bool carAtInBefore = false;
bool carAtOutBefore = false;
unsigned long lastSensorCheck = 0;


// =======================================================
// 4. HJÄLPFUNKTIONER FÖR HÅRDVARA
// =======================================================

long getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW); delayMicroseconds(2);
  digitalWrite(trigPin, HIGH); delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 20000); 
  if (duration == 0) return 999;
  return duration * 0.034 / 2;
}

void beep(int duration, int times = 1) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, LOW); // LOW = PIPER
    delay(duration);
    digitalWrite(BUZZER_PIN, HIGH); // HIGH = TYST
    if (times > 1) delay(100); 
  }
}

void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH); 
}

void initLCD() {
  Wire.begin(21, 22);
  lcdIn.init(); lcdIn.backlight();
  lcdOut.init(); lcdOut.backlight();
}

void updateDisplays() {
  lcdIn.setCursor(0, 0);  lcdIn.print("Platser: " + String(freeSpots) + "    ");
  lcdIn.setCursor(0, 1);  lcdIn.print("Scanna kort...  ");
  
  lcdOut.setCursor(0, 0); lcdOut.print("Platser: " + String(freeSpots) + "    ");
  lcdOut.setCursor(0, 1); lcdOut.print("Scanna kort...  ");
}

String getUID(MFRC522 &rfid) {
  String uidStr = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uidStr += String(rfid.uid.uidByte[i], HEX);
  }
  return uidStr;
}

// =======================================================
// 5. NÄTVERK OCH DATABAS
// =======================================================

void connectWiFi() {
  lcdIn.clear(); 
  lcdIn.print("WiFi: Ansluter..");
  Serial.print("Ansluter till Wi-Fi...");
  
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500); 
    Serial.print(".");
    attempts++;
  }
  
  lcdIn.clear();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK! IP: " + WiFi.localIP().toString());
    lcdIn.print("WiFi: OK!");
    lcdOut.clear(); lcdOut.print("IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(" OFFLINE! Fortsätter utan internet.");
    lcdIn.print("WiFi: OFFLINE");
    lcdOut.clear(); lcdOut.print("Lokal Lagring");
  }
  delay(3000); 
}

bool checkAccess(String uid) {
  if (WiFi.status() == WL_CONNECTED) {
    if (redisClient.connect(redis_host, redis_port)) {
      Redis redis(redisClient); 
      String access = redis.get(("access:" + uid).c_str());
      redisClient.stop(); 
      return (access == "1"); 
    }
  }
  Serial.println("Databas ej nåbar. Offline-fallback.");
  return true; 
}

void logPassage(String uid, String direction) {
  String logData = uid + "," + direction + "," + String(millis());
  if (WiFi.status() == WL_CONNECTED) {
    if (redisClient.connect(redis_host, redis_port)) {
      Redis redis(redisClient);
      redis.rpush("parking_logs", logData.c_str());
      redisClient.stop();
      return; 
    }
  }
  
  File file = LittleFS.open(queueFile, FILE_APPEND);
  if (file) {
    file.println(logData);
    file.close();
  }
}

void syncOfflineData() {
  if (WiFi.status() == WL_CONNECTED && LittleFS.exists(queueFile)) {
    if (redisClient.connect(redis_host, redis_port)) {
      Redis redis(redisClient);
      File file = LittleFS.open(queueFile, FILE_READ);
      while (file.available()) {
        String logData = file.readStringUntil('\n');
        logData.trim();
        if (logData.length() > 0) {
          redis.rpush("parking_logs", logData.c_str());
        }
      }
      file.close(); 
      redisClient.stop();
      LittleFS.remove(queueFile); 
      Serial.println("Offline-data uppladdad!");
    }
  }
}

// =======================================================
// 6. SETUP 
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  initBuzzer();
  initLCD();
  lcdIn.clear(); lcdIn.print("Laddar System...");
  delay(1000);

  Serial.println("\n=== SYSTEM BOOT ===");

  pinMode(SS_IN_PIN, OUTPUT); digitalWrite(SS_IN_PIN, HIGH);
  pinMode(SS_OUT_PIN, OUTPUT); digitalWrite(SS_OUT_PIN, HIGH);
  SPI.begin(18, 19, 23, 27);
  rfidIn.PCD_Init();
  rfidOut.PCD_Init();

  ESP32PWM::allocateTimer(0); ESP32PWM::allocateTimer(1);
  servoIn.setPeriodHertz(50); servoOut.setPeriodHertz(50);
  servoIn.attach(SERVO_IN_PIN, 500, 2400); 
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);
  servoIn.write(0); servoOut.write(0); 

  pinMode(TRIG_IN_BEFORE, OUTPUT); pinMode(ECHO_IN_BEFORE, INPUT);
  pinMode(TRIG_IN_AFTER, OUTPUT); pinMode(ECHO_IN_AFTER, INPUT);
  pinMode(TRIG_OUT_BEFORE, OUTPUT); pinMode(ECHO_OUT_BEFORE, INPUT);
  pinMode(TRIG_OUT_AFTER, OUTPUT); pinMode(ECHO_OUT_AFTER, INPUT);

  LittleFS.begin(true);
  connectWiFi(); 
  syncOfflineData();

  updateDisplays();
  beep(100, 3); 
  Serial.println("\n=== SYSTEM REDO OCH KÖRS! ===");
}


// =======================================================
// 7. HUVUDLOOP 
// =======================================================
void loop() {
  
  // --- KOLLA BILAR (Sensorer) ---
  if (millis() - lastSensorCheck > 300) {
    long distIn = getDistance(TRIG_IN_BEFORE, ECHO_IN_BEFORE);
    long distOut = getDistance(TRIG_OUT_BEFORE, ECHO_OUT_BEFORE);

    // ÄNDRAT: Känner nu bara av bil om avståndet är upp till 5 cm!
    if (distIn <= 5 && !carAtInBefore) { carAtInBefore = true; beep(100); } 
    else if (distIn > 5) { carAtInBefore = false; }

    if (distOut <= 5 && !carAtOutBefore) { carAtOutBefore = true; beep(100); } 
    else if (distOut > 5) { carAtOutBefore = false; }
    
    lastSensorCheck = millis();
  }

  // --- HANTERA INGÅNG (Kortläsare) ---
  if (!gateInOpen && rfidIn.PICC_IsNewCardPresent() && rfidIn.PICC_ReadCardSerial()) {
    beep(50);
    String uid = getUID(rfidIn);
    Serial.println("Läst in: " + uid);
    
    if (checkAccess(uid)) {
      freeSpots--;
      logPassage(uid, "IN");

      // NYTT: Spara tiden då bilen körde in!
      parkeringstider[uid] = millis();

      lcdIn.clear(); lcdIn.print("Valkommen!");
      
      beep(100, 2);
      servoIn.write(90); 
      gateInOpen = true;
      gateInTimer = millis(); 
    } else {
      lcdIn.clear(); lcdIn.print("Nekad Access!");
      beep(500); 
    }
    rfidIn.PICC_HaltA();
  }

  // --- HANTERA INGÅNGENS BOM ---
  if (gateInOpen && (millis() - gateInTimer > 3000)) { 
    long carPassedDist = getDistance(TRIG_IN_AFTER, ECHO_IN_AFTER);
    // ÄNDRAT: Väntar tills bilen är mer än 5 cm bort innan bommen stängs
    if (carPassedDist > 5) {
      servoIn.write(0); 
      gateInOpen = false;
      updateDisplays();
    } else {
      gateInTimer = millis(); 
    }
  }

  // --- HANTERA UTGÅNG (Kortläsare) ---
  if (!gateOutOpen && rfidOut.PICC_IsNewCardPresent() && rfidOut.PICC_ReadCardSerial()) {
    beep(50);
    String uid = getUID(rfidOut);
    Serial.println("Läst ut: " + uid);
    
    if (checkAccess(uid)) {
      freeSpots++;
      logPassage(uid, "OUT");

      // NYTT: Räkna ut hur länge bilen var parkerad
      String timeMsg = "Tid okand";
      if (parkeringstider.count(uid)) { // Kolla om bilen finns i vår lista
        unsigned long parkedTimeMs = millis() - parkeringstider[uid];
        unsigned long totalSecs = parkedTimeMs / 1000;
        unsigned long mins = totalSecs / 60;
        unsigned long secs = totalSecs % 60;

        // Formatera snyggt för skärmen
        if (mins > 0) {
          timeMsg = "Tid: " + String(mins) + "m " + String(secs) + "s";
        } else {
          timeMsg = "Tid: " + String(secs) + " sekunder";
        }
        // Ta bort bilen från "parkerade bilar"-listan
        parkeringstider.erase(uid); 
      }

      // Visa tiden på LCD-skärmen
      lcdOut.clear(); 
      lcdOut.setCursor(0, 0); lcdOut.print("Tack for besoket");
      lcdOut.setCursor(0, 1); lcdOut.print(timeMsg);
      
      beep(100, 2);
      servoOut.write(90); 
      gateOutOpen = true;
      gateOutTimer = millis();
    } else {
      lcdOut.clear(); lcdOut.print("Nekad Access!");
      beep(500); 
    }
    rfidOut.PICC_HaltA();
  }

  // --- HANTERA UTGÅNGENS BOM ---
  if (gateOutOpen && (millis() - gateOutTimer > 3000)) {
    long carPassedDist = getDistance(TRIG_OUT_AFTER, ECHO_OUT_AFTER);
    // ÄNDRAT: Väntar tills bilen är mer än 5 cm bort innan bommen stängs
    if (carPassedDist > 5) {
      servoOut.write(0); 
      gateOutOpen = false;
      updateDisplays();
    } else {
      gateOutTimer = millis(); 
    }
  }
}