#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include "credentials.h"  // WiFi credentials

// Pin definitions for ESP32 SPI connection
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4
#define BUTTON_PIN 0  // BOOT button on most ESP32 boards

// Initialize display (296x128, 2.9" Waveshare)
GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> display(GxEPD2_290(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

String publicIP = "Fetching...";
bool lastButtonState = HIGH;

String getPublicIP() {
  HTTPClient http;
  String ip = "Error";

  Serial.println("Fetching public IP...");
  http.begin("http://api.ipify.org");

  int httpCode = http.GET();
  if (httpCode == 200) {
    ip = http.getString();
    Serial.print("Public IP: ");
    Serial.println(ip);
  } else {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
  }

  http.end();
  return ip;
}

void updateDisplay() {
  Serial.println("Updating display...");

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Title
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(10, 25);
    display.println("Public IP Address");

    // Draw line
    display.drawLine(10, 30, 286, 30, GxEPD_BLACK);

    // IP Address
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 60);
    display.println(publicIP);

    // WiFi status
    display.setCursor(10, 90);
    display.print("WiFi: ");
    display.println(WiFi.SSID());

    // Instructions
    display.setCursor(10, 115);
    display.setFont();  // Default small font
    display.println("Press BOOT to refresh");

  } while (display.nextPage());

  Serial.println("Display update complete!");
  display.hibernate();
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Public IP Display");

  // Setup button pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Initialize the display
  display.init(115200);
  display.setRotation(1);

  // Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("Local IP: ");
    Serial.println(WiFi.localIP());

    // Get public IP
    publicIP = getPublicIP();
  } else {
    Serial.println("\nWiFi connection failed!");
    publicIP = "WiFi Failed";
  }

  // Initial display update
  updateDisplay();
}

void loop() {
  bool buttonState = digitalRead(BUTTON_PIN);

  // Detect button press (LOW = pressed on BOOT button)
  if (buttonState == LOW && lastButtonState == HIGH) {
    Serial.println("Button pressed!");
    delay(50); // Debounce

    if (WiFi.status() == WL_CONNECTED) {
      publicIP = getPublicIP();
      updateDisplay();
    } else {
      Serial.println("WiFi not connected!");
    }

    delay(200); // Prevent multiple triggers
  }

  lastButtonState = buttonState;
  delay(10);
}
