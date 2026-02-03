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
#include <cstring>
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

bool timeToUtcStruct(time_t timestamp, struct tm& out) {
  struct tm* tmp = gmtime(&timestamp);
  if (!tmp) {
    return false;
  }
  memcpy(&out, tmp, sizeof(struct tm));
  return true;
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

  struct tm currentInfo;
  if (!timeToUtcStruct(now, currentInfo)) {
    statusMessage = "Time convert failed";
    return false;
  }

  // Octopus pricing periods run from 23:00 UTC to 23:00 UTC
  // Calculate the most recent 23:00 UTC (yesterday if before 23:00, today if after)
  int secondsIntoDay = currentInfo.tm_hour * 3600 + currentInfo.tm_min * 60 + currentInfo.tm_sec;
  time_t dayStart = now > secondsIntoDay ? now - secondsIntoDay : 0;

  // Adjust to start from 23:00 yesterday (subtract 1 hour from midnight)
  time_t periodStart = dayStart > 3600 ? dayStart - 3600 : 0;

  // Period end is 23:00 today (start + 24 hours)
  time_t periodEnd = periodStart + 86400;

  struct tm periodStartInfo, periodEndInfo;
  if (!timeToUtcStruct(periodStart, periodStartInfo)) {
    statusMessage = "Period start failed";
    return false;
  }
  if (!timeToUtcStruct(periodEnd, periodEndInfo)) {
    statusMessage = "Period end failed";
    return false;
  }

  char currentIso[25];
  char periodStartIso[25];
  char periodEndIso[25];
  strftime(currentIso, sizeof(currentIso), "%Y-%m-%dT%H:%M:%SZ", &currentInfo);
  strftime(periodStartIso, sizeof(periodStartIso), "%Y-%m-%dT%H:%M:%SZ", &periodStartInfo);
  strftime(periodEndIso, sizeof(periodEndIso), "%Y-%m-%dT%H:%M:%SZ", &periodEndInfo);

  String url = "https://api.octopus.energy/v1/products/";
  url += OCTOPUS_PRODUCT_CODE;
  url += "/electricity-tariffs/";
  url += OCTOPUS_TARIFF_CODE;
  url += "/standard-unit-rates/?period_from=";
  url += periodStartIso;
  url += "&period_to=";
  url += periodEndIso;

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

  Serial.print("Found ");
  Serial.print(results.size());
  Serial.println(" rate periods");
  Serial.print("Current time (UTC): ");
  Serial.println(currentIso);

  // Debug: Print first 3 rates to see what we're getting
  Serial.println("First 3 rates from API:");
  for (int i = 0; i < 3 && i < results.size(); i++) {
    JsonObject rate = results[i];
    Serial.print("  ");
    Serial.print(i);
    Serial.print(": ");
    Serial.print(rate["valid_from"].as<const char*>());
    Serial.print(" to ");
    Serial.print(rate["valid_to"].as<const char*>());
    Serial.print(" = ");
    Serial.print(rate["value_inc_vat"].as<double>());
    Serial.println(" p");
  }

  JsonObject selectedRate;
  bool hasSelectedRate = false;
  JsonObject fallbackRate;
  bool hasFallbackRate = false;

  for (JsonObject rate : results) {
    if (!hasFallbackRate) {
      fallbackRate = rate;
      hasFallbackRate = true;
    }

    const char* validFromCandidate = rate["valid_from"] | "";
    const char* validToCandidate = rate["valid_to"] | "";
    if (strlen(validFromCandidate) == 0 || strlen(validToCandidate) == 0) {
      continue;
    }

    // Compare only first 19 characters (YYYY-MM-DDTHH:MM:SS) to ignore fractional seconds
    if ((strncmp(validFromCandidate, currentIso, 19) <= 0) && (strncmp(currentIso, validToCandidate, 19) < 0)) {
      selectedRate = rate;
      hasSelectedRate = true;
      Serial.print("Matched current slot: ");
      Serial.print(validFromCandidate);
      Serial.print(" to ");
      Serial.println(validToCandidate);
      break;
    }
  }

  JsonObject rateObject;
  if (hasSelectedRate) {
    rateObject = selectedRate;
    statusMessage = "";
  } else if (hasFallbackRate) {
    rateObject = fallbackRate;
    statusMessage = "Showing next slot";
  } else {
    Serial.println("No rate data returned.");
    statusMessage = "No rate data";
    return false;
  }

  double priceValue = rateObject["value_inc_vat"] | -1.0;
  const char* validFrom = rateObject["valid_from"] | "";
  const char* validTo = rateObject["valid_to"] | "";

  if (priceValue < 0) {
    Serial.println("Invalid price value.");
    statusMessage = "Invalid price";
    return false;
  }

  currentPrice = String(priceValue, 2) + " p/kWh";
  priceWindow = extractTimeFromISO(validFrom) + " - " + extractTimeFromISO(validTo) + " UTC";
  lastUpdated = currentIso;

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
