#include <Arduino.h>

/*
 * STEG 1: HÅRDVARUTEST - SMART PARKING SYSTEM
 * Hårdvara: ESP32 DevKitC V4 (38-pin)
 */

#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <LiquidCrystal_I2C.h>
#include <Redis.h>

// --- PIN DEFINITIONER ---

// RFID (SPI)
#define RST_PIN 27
#define SS_IN_PIN 5
#define SS_OUT_PIN 4

// Servomotorer
#define SERVO_IN_PIN 13
#define SERVO_OUT_PIN 14

// Ultraljudssensorer (För testet använder vi "Före bom"-sensorerna)
#define TRIG_IN 32
#define ECHO_IN 34
#define TRIG_OUT 25
#define ECHO_OUT 36

// --- OBJEKT ---

// LCD: Byt ut 0x26 mot den adress du faktiskt har på skärm 2
LiquidCrystal_I2C lcdIn(0x27, 16, 2); 
LiquidCrystal_I2C lcdOut(0x26, 16, 2); 

MFRC522 rfidIn(SS_IN_PIN, RST_PIN);
MFRC522 rfidOut(SS_OUT_PIN, RST_PIN);

Servo servoIn;
Servo servoOut;

// Variabel för att inte spamma Serial Monitor med sensordata
unsigned long lastSensorRead = 0;

// --- FUNKTIONSDEKLARATIONER ---
// Berättar för C++ att funktionen finns längre ner i koden
long measureDistance(int trigPin, int echoPin);

void setup() {
  Serial.begin(115200);
  Serial.println("Startar Hårdvarutest...");

  // 1. Initiera I2C & Skärmar
  Wire.begin(21, 22); // SDA, SCL
  lcdIn.init();
  lcdIn.backlight();
  lcdIn.setCursor(0, 0);
  lcdIn.print("INGANG REDO");

  lcdOut.init();
  lcdOut.backlight();
  lcdOut.setCursor(0, 0);
  lcdOut.print("UTGANG REDO");

  // 2. Initiera SPI & RFID
  SPI.begin(18, 19, 23, 27); // SCK, MISO, MOSI, SS (RST-pin används som referens här)
  rfidIn.PCD_Init();
  rfidOut.PCD_Init();
  Serial.println("RFID-läsare initierade.");

  // 3. Initiera Servomotorer
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoIn.setPeriodHertz(50);
  servoOut.setPeriodHertz(50);
  servoIn.attach(SERVO_IN_PIN, 500, 2400);
  servoOut.attach(SERVO_OUT_PIN, 500, 2400);
  
  servoIn.write(0);  // Ställ i stängt läge
  servoOut.write(0); // Ställ i stängt läge

  // 4. Initiera Ultraljud
  pinMode(TRIG_IN, OUTPUT);
  pinMode(ECHO_IN, INPUT);
  pinMode(TRIG_OUT, OUTPUT);
  pinMode(ECHO_OUT, INPUT);

  Serial.println("Setup klar! Systemet körs.");
}

void loop() {
  // --- TEST 1: LÄS ULTRALJUD VARJE SEKUND ---
  if (millis() - lastSensorRead > 1000) {
    long distIn = measureDistance(TRIG_IN, ECHO_IN);
    long distOut = measureDistance(TRIG_OUT, ECHO_OUT);
    
    Serial.print("Avstånd Ingång: "); Serial.print(distIn); Serial.print(" cm | ");
    Serial.print("Avstånd Utgång: "); Serial.print(distOut); Serial.println(" cm");
    
    lastSensorRead = millis();
  }

  // --- TEST 2: KOLLA EFTER RFID VID INGÅNG ---
  if (rfidIn.PICC_IsNewCardPresent() && rfidIn.PICC_ReadCardSerial()) {
    Serial.println("KORT LÄST VID INGÅNG!");
    lcdIn.setCursor(0, 1);
    lcdIn.print("Kort Godkant!   ");
    
    servoIn.write(90); // Öppna
    delay(2000);       // Vänta 2 sekunder
    servoIn.write(0);  // Stäng
    
    lcdIn.setCursor(0, 1);
    lcdIn.print("                "); // Rensa raden
    
    rfidIn.PICC_HaltA(); // Stoppa läsning av detta kort
  }

  // --- TEST 3: KOLLA EFTER RFID VID UTGÅNG ---
  if (rfidOut.PICC_IsNewCardPresent() && rfidOut.PICC_ReadCardSerial()) {
    Serial.println("KORT LÄST VID UTGÅNG!");
    lcdOut.setCursor(0, 1);
    lcdOut.print("Kort Godkant!   ");
    
    servoOut.write(90); 
    delay(2000);       
    servoOut.write(0);  
    
    lcdOut.setCursor(0, 1);
    lcdOut.print("                "); 
    
    rfidOut.PICC_HaltA(); 
  }
}

// Hjälpfunktion för att mäta avstånd med ultraljudssensor
long measureDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  
  long duration = pulseIn(echoPin, HIGH, 30000); // Timeout efter 30ms (för att inte frysa koden)
  if (duration == 0) return -1; // Fel eller inget svar
  
  return duration * 0.034 / 2; // Konvertera till centimeter
}