/*
  SmartCart Checkout - ESP32 + MFRC522 RFID Reader
  Reads [price][item] (e.g., "10apples") from Mifare Block 4
  and posts locally via plain HTTP.
*/

// Brownout bypass registers
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>

// ESP32 VSPI Pin Configuration
#define SCK_PIN   18
#define MISO_PIN  19
#define MOSI_PIN  23
#define SS_PIN     5
#define RST_PIN   22

MFRC522 rfid(SS_PIN, RST_PIN);
MFRC522::MIFARE_Key key;

// 1. Enter your local WiFi network details:
const char* ssid     = "WiFi-Name";  // Replace with your WiFi SSID
const char* password = "WiFi-Password";  // Replace with your WiFi password

// 2. Local computer IP (replace with your PC's actual local IPv4 address):
const char* serverApi = "http://IP_ADDRESS:3000/api/esp/scan";

// Target block for RFID data
const byte targetBlock = 4;

void setup() {
  // Disable brownout detector to prevent current-spike reboot loops
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);

  // Initialize SPI bus on specific VSPI lines
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();

  // Set default factory key (0xFF)
  for (byte i = 0; i < 6; i++) {
    key.keyByte[i] = 0xFF;
  }

  // Connect to Wi-Fi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected! Ready for Smart Cart card scans.");
}

// Reads saved text string from Mifare Classic Data Block
String readCardDataString(byte blockAddr) {
  MFRC522::StatusCode status;
  byte buffer[18];
  byte size = sizeof(buffer);

  // Authenticate sector using Key A
  status = rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, blockAddr, &key, &(rfid.uid));
  if (status != MFRC522::STATUS_OK) return "";

  // Read 16 bytes from block
  status = rfid.MIFARE_Read(blockAddr, buffer, &size);
  if (status != MFRC522::STATUS_OK) return "";

  String str = "";
  for (byte i = 0; i < 16; i++) {
    if (buffer[i] >= 32 && buffer[i] <= 126) { // ASCII printable range
      str += (char)buffer[i];
    }
  }
  str.trim();
  return str;
}

void loop() {
  // Check for new card tap
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(50);
    return;
  }

  // 1. Read stored payload from Block 4
  String cardData = readCardDataString(targetBlock);

  // 2. Fallback to UID if Block 4 cannot be read or is empty
  String tagUid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) tagUid += "0";
    tagUid += String(rfid.uid.uidByte[i], HEX);
  }
  tagUid.toUpperCase();

  String payloadTag = (cardData.length() > 0) ? cardData : tagUid;
  Serial.println("Scanned Card Payload: " + payloadTag);

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.begin(serverApi);
    http.addHeader("Content-Type", "application/json");

    // Manually constructed JSON: {"rfidTag":"..."}
    String requestBody = "{\"rfidTag\":\"" + payloadTag + "\"}";

    int httpCode = http.POST(requestBody);
    if (httpCode > 0) {
      String response = http.getString();
      Serial.println("Server Response (HTTP " + String(httpCode) + "): " + response);
    } else {
      Serial.println("POST Error: " + http.errorToString(httpCode));
    }
    http.end();
  } else {
    Serial.println("WiFi not connected. Scan dropped.");
  }

  // Release card communication
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(1200); // 1.2-second debounce
}