/*
  Unified SmartCart Checkout System
  - ESP32 + MFRC522 RFID (HTTP POST to Web Dashboard)
  - HX711 Load Cell + Screaming Siren Buzzer
  - 16x2 I2C LCD (Item Scan, Total Cost, Live Weight)
*/

#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "HX711.h"

// ================= PIN DEFINITIONS =================
#define SCK_PIN           18
#define MISO_PIN          19
#define MOSI_PIN          23
#define SS_PIN            5
#define RST_PIN           22

#define LOADCELL_DOUT_PIN 35
#define LOADCELL_SCK_PIN  32

#define BUZZER_PIN        4

#define I2C_SDA_PIN       2
#define I2C_SCL_PIN       15

// ================= SYSTEM OBJECTS =================
MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

HX711 scale;
float calibration_factor = 420.0;

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= NETWORK CONFIGURATION =================
const char* ssid      = "Mich";
const char* password  = "00000000";
const char* serverApi = "http://192.168.43.103:3000/api/esp/scan";

const byte targetBlock = 4;

// ================= STATE & TIMING VARIABLES =================
float totalCost = 0.0;
float currentWeightKg = 0.0;

unsigned long messageEndTime = 0;
bool showingItemMessage = false;

float lastDisplayedWeight = -1.0;
float lastDisplayedCost = -1.0;

unsigned long lastRfidScanTime = 0;
const unsigned long rfidCooldown = 1200;

unsigned long lastScaleReadTime = 0;
const unsigned long scaleInterval = 300;

// Siren modulation state
unsigned long lastSirenToggle = 0;
bool sirenState = false;

// ================= BUZZER FUNCTIONS =================
void playTone(int frequency) {
  if (frequency <= 0) {
    noTone(BUZZER_PIN);
  } else {
    tone(BUZZER_PIN, frequency);
  }
}

// Alternates rapidly between 2800 Hz and 4200 Hz for an ear-piercing scream
void playScreamingSiren() {
  if (millis() - lastSirenToggle > 50) {
    lastSirenToggle = millis();
    sirenState = !sirenState;
    playTone(sirenState ? 4200 : 2800);
  }
}

// ================= HTTP TRANSMITTER =================
void sendPayloadToWeb(String payload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected. Scan dropped.");
    return;
  }

  Serial.println("[HTTP] Sending to server: " + payload);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(serverApi);
  http.addHeader("Content-Type", "application/json");

  String requestBody = "{\"rfidTag\":\"" + payload + "\"}";
  int httpCode = http.POST(requestBody);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("Server Response (HTTP " + String(httpCode) + "): " + response);
  } else {
    Serial.println("POST Error: " + http.errorToString(httpCode));
  }
  http.end();
}

// ================= LCD FUNCTIONS =================
void addItem(String itemName, float itemPrice) {
  totalCost += itemPrice;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("+ " + itemName.substring(0, 14));
  lcd.setCursor(0, 1);
  lcd.print("+GHS " + String(itemPrice, 2));

  messageEndTime = millis() + 3000;
  showingItemMessage = true;
}

void removeItem(String itemName, float itemPrice) {
  totalCost -= itemPrice;
  if (totalCost < 0) totalCost = 0.0;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("- " + itemName.substring(0, 14));
  lcd.setCursor(0, 1);
  lcd.print("-GHS " + String(itemPrice, 2));

  messageEndTime = millis() + 3000;
  showingItemMessage = true;
}

void updateDefaultDisplay() {
  if (abs(currentWeightKg - lastDisplayedWeight) > 0.01 || abs(totalCost - lastDisplayedCost) > 0.01) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Total: GHS ");
    lcd.print(totalCost, 2);

    lcd.setCursor(0, 1);
    lcd.print("Weight: ");
    lcd.print(currentWeightKg, 2);
    lcd.print(" kg");

    lastDisplayedWeight = currentWeightKg;
    lastDisplayedCost = totalCost;
  }
}

// ================= RFID DATA PARSER (4 FOOD ITEMS) =================
void processCardPayload(String payload) {
  payload.trim();

  if (payload.indexOf("Water") >= 0) {
    addItem("Water", 5.00);
  } else if (payload.indexOf("CocaCola") >= 0 || payload.indexOf("Coke") >= 0) {
    addItem("CocaCola", 10.00);
  } else if (payload.indexOf("Kalyppo") >= 0) {
    addItem("Kalyppo", 10.00);
  } else if (payload.indexOf("Biscuit") >= 0) {
    addItem("Biscuit", 7.00);
  } else {
    addItem(payload.substring(0, 10), 0.00);
  }
}

String readCardDataString(byte blockAddr) {
  MFRC522::StatusCode status;
  byte buffer[18];
  byte size = sizeof(buffer);

  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) return "";

  status = rfid.MIFARE_Read(blockAddr, buffer, &size);
  if (status != MFRC522::STATUS_OK) return "";

  String str = "";
  for (byte i = 0; i < 16; i++) {
    if (buffer[i] >= 32 && buffer[i] <= 126) {
      str += (char)buffer[i];
    }
  }
  str.trim();
  return str;
}

// ================= SETUP =================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);

  // 1. Buzzer Init
  pinMode(BUZZER_PIN, OUTPUT);
  playTone(0);

  // 2. LCD Init
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Cart Ready");

  // 3. Load Cell Init
  Serial.println("Initializing Load Cell...");
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  if (scale.wait_ready_timeout(1000)) {
    scale.set_scale();
    scale.tare();
    scale.set_scale(calibration_factor);
    Serial.println("Load Cell Ready!");
  } else {
    Serial.println("HX711 not found (check GPIO 35 / 32).");
  }

  // 4. RFID Init
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();

  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  // 5. WiFi Connect
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected! Ready for Smart Cart card scans.");
  Serial.println("Commands: '1'=Water(5), '2'=CocaCola(10), '3'=Kalyppo(10), '4'=Biscuit(7)");

  delay(1000);
  lcd.clear();
}

// ================= MAIN LOOP =================
void loop() {
  // 1. NON-BLOCKING LOAD CELL READ
  if (millis() - lastScaleReadTime > scaleInterval) {
    lastScaleReadTime = millis();
    if (scale.is_ready()) {
      float reading = scale.get_units(1);
      currentWeightKg = reading / 1000.0;
      if (currentWeightKg < 0) currentWeightKg = 0.0;
    }
  }

  // 2. ALARM LOGIC (Level 2: Screaming siren | Level 1: Fast warning chirp)
  if (currentWeightKg >= 1.50) {
    playScreamingSiren();
  } else if (currentWeightKg >= 1.20) {
    playTone(3200);
  } else {
    playTone(0);
  }

  // 3. RFID SCANNING
  if (millis() - lastRfidScanTime > rfidCooldown) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      String cardData = readCardDataString(targetBlock);

      String tagUid = "";
      for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) tagUid += "0";
        tagUid += String(rfid.uid.uidByte[i], HEX);
      }
      tagUid.toUpperCase();

      String payloadTag = (cardData.length() > 0) ? cardData : tagUid;
      Serial.println("Scanned Card Payload: " + payloadTag);

      processCardPayload(payloadTag);
      sendPayloadToWeb(payloadTag);

      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
      lastRfidScanTime = millis();
    }
  }

  // 4. LCD TIMING
  if (showingItemMessage) {
    if (millis() >= messageEndTime) {
      showingItemMessage = false;
      lastDisplayedWeight = -1.0;
      lastDisplayedCost = -1.0;
    }
  } else {
    updateDefaultDisplay();
  }

  // 5. KEYBOARD SIMULATION CONTROLS ('1' - '4')
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '1') {
      addItem("Water", 5.00);
      sendPayloadToWeb("Water5");
    } else if (cmd == '2') {
      addItem("CocaCola", 10.00);
      sendPayloadToWeb("CocaCola10");
    } else if (cmd == '3') {
      addItem("Kalyppo", 10.00);
      sendPayloadToWeb("Kalyppo10");
    } else if (cmd == '4') {
      addItem("Biscuit", 7.00);
      sendPayloadToWeb("Biscuit7");
    }
  }

  delay(10);
}