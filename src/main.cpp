#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <time.h>
#include <sys/time.h>
#include "credentials.h"  // WiFi credentials

// Pin definitions for ESP32 SPI connection
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4
#define BUTTON_PIN 0  // BOOT button on most ESP32 boards

// Initialize display (296x128, 2.9" Waveshare)
GxEPD2_BW<GxEPD2_290, GxEPD2_290::HEIGHT> display(GxEPD2_290(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

#ifndef OCTOPUS_PRODUCT_CODE
#define OCTOPUS_PRODUCT_CODE "AGILE-24-10-01"
#endif

#ifndef OCTOPUS_REGION_CODE
#define OCTOPUS_REGION_CODE "E"  // West Midlands (see Octopus docs for region codes)
#endif

#ifndef OCTOPUS_TARIFF_PREFIX
#define OCTOPUS_TARIFF_PREFIX "E-1R-"  // Single-rate electricity
#endif

#ifndef OCTOPUS_TARIFF_CODE
#define OCTOPUS_TARIFF_CODE OCTOPUS_TARIFF_PREFIX OCTOPUS_PRODUCT_CODE "-" OCTOPUS_REGION_CODE
#endif

String currentPrice = "Fetching price...";
String priceWindow = "Waiting for data...";
String lastUpdated = "--";
String statusMessage = "";
bool lastButtonState = HIGH;

bool waitForTimeSync() {
  time_t now = time(nullptr);
  int attempts = 0;
  while ((now < 1000000000) && (attempts < 40)) {  // wait until reasonable epoch
    delay(500);
    now = time(nullptr);
    attempts++;
  }

  if (now < 1000000000) {
    Serial.println("Unable to get valid time from NTP.");
    statusMessage = "Time sync failed";
    return false;
  }

  Serial.println("Time synchronized.");
  return true;
}

bool syncTimeFromHttp() {
  const char* timeUrl = "https://worldtimeapi.org/api/timezone/Europe/London";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, timeUrl)) {
    Serial.println("Failed to init HTTP client for time API.");
    statusMessage = "Time API init failed";
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Time API HTTP Error: ");
    Serial.println(httpCode);
    statusMessage = "Time API error";
    http.end();
    return false;
  }

  DynamicJsonDocument timeDoc(1024);
  DeserializationError error = deserializeJson(timeDoc, http.getString());
  http.end();
  if (error) {
    Serial.print("Time JSON parse failed: ");
    Serial.println(error.f_str());
    statusMessage = "Time parse error";
    return false;
  }

  long unixTime = timeDoc["unixtime"] | 0;
  if (unixTime <= 0) {
    Serial.println("Invalid time received from API.");
    statusMessage = "Invalid time";
    return false;
  }

  struct timeval tv;
  tv.tv_sec = unixTime;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.print("Time synced via HTTP API: ");
  Serial.println(unixTime);

  return waitForTimeSync();
}

String extractTimeFromISO(const char* isoTime) {
  if (!isoTime) {
    return "--:--";
  }
  String timeString = isoTime;
  if (timeString.length() >= 16) {
    return timeString.substring(11, 16);  // HH:MM
  }
  return timeString;
}

bool fetchCurrentPrice() {
  if (WiFi.status() != WL_CONNECTED) {
    statusMessage = "WiFi disconnected";
    return false;
  }

  time_t now = time(nullptr);
  if (now < 1000000000) {
    Serial.println("Time not set - attempting to sync again.");
    if (!waitForTimeSync() && !syncTimeFromHttp()) {
      return false;
    }
    now = time(nullptr);
  }

  struct tm* timeInfo = gmtime(&now);
  char isoBuffer[25];
  strftime(isoBuffer, sizeof(isoBuffer), "%Y-%m-%dT%H:%M:%SZ", timeInfo);

  String url = "https://api.octopus.energy/v1/products/";
  url += OCTOPUS_PRODUCT_CODE;
  url += "/electricity-tariffs/";
  url += OCTOPUS_TARIFF_CODE;
  url += "/standard-unit-rates/?page_size=1&period_from=";
  url += isoBuffer;

  Serial.print("Requesting rate: ");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure();  // Octopus Energy uses a trusted certificate; skip CA check for simplicity

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("Failed to initialize HTTP client.");
    statusMessage = "HTTP init failed";
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
    statusMessage = "HTTP error " + String(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.print("JSON parse failed: ");
    Serial.println(error.f_str());
    statusMessage = "JSON parse error";
    return false;
  }

  JsonArray results = doc["results"];
  if (!results || results.size() == 0) {
    Serial.println("No rate data returned.");
    statusMessage = "No rate data";
    return false;
  }

  JsonObject rate = results[0];
  double priceValue = rate["value_inc_vat"] | -1.0;
  const char* validFrom = rate["valid_from"] | "";
  const char* validTo = rate["valid_to"] | "";

  if (priceValue < 0) {
    Serial.println("Invalid price value.");
    statusMessage = "Invalid price";
    return false;
  }

  currentPrice = String(priceValue, 2) + " p/kWh";
  priceWindow = extractTimeFromISO(validFrom) + " - " + extractTimeFromISO(validTo) + " UTC";
  lastUpdated = isoBuffer;
  statusMessage = "";

  Serial.print("Current price: ");
  Serial.println(currentPrice);
  Serial.print("Valid window: ");
  Serial.println(priceWindow);

  return true;
}

void clearDisplay() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
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
    display.println("Octopus Energy");

    // Draw line
    display.drawLine(10, 30, 286, 30, GxEPD_BLACK);

    // Price info
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 60);
    display.print("Price: ");
    display.println(currentPrice);

    display.setCursor(10, 90);
    display.print("Slot: ");
    display.println(priceWindow);

    display.setCursor(10, 115);
    display.print("Updated: ");
    display.println(lastUpdated);

    display.setFont();  // Default small font
    display.setCursor(10, 122);
    String footer = "WiFi: " + WiFi.SSID();
    if (statusMessage.length() > 0) {
      footer += " | " + statusMessage;
    } else {
      footer += " | BOOT=Refresh";
    }
    display.println(footer);

  } while (display.nextPage());

  Serial.println("Display update complete!");
  display.hibernate();
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Octopus Price Display");

  // Setup button pin
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Initialize the display
  display.init(115200);
  display.setRotation(1);
  clearDisplay();

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

    configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    if (!waitForTimeSync()) {
      syncTimeFromHttp();
    }

    // Get initial price info
    if (!fetchCurrentPrice()) {
      currentPrice = "Unable to fetch";
      priceWindow = "Check tariff settings";
    }
  } else {
    Serial.println("\nWiFi connection failed!");
    currentPrice = "WiFi Failed";
    priceWindow = "--";
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
      fetchCurrentPrice();
      updateDisplay();
    } else {
      Serial.println("WiFi not connected!");
    }

    delay(200); // Prevent multiple triggers
  }

  lastButtonState = buttonState;
  delay(10);
}
