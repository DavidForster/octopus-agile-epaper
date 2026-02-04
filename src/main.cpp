#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include "GxEPD2_290_Custom.h"  // Custom driver with white border
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

// Initialize display (296x128, 2.9" Waveshare) with custom driver
GxEPD2_BW<GxEPD2_290_Custom, GxEPD2_290_Custom::HEIGHT> display(GxEPD2_290_Custom(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

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

// Store rate data for graphing
struct RateData {
  time_t validFrom;
  double price;
};
RateData rates[60];  // 27 hours * 2 slots per hour = 54, with some buffer
int rateCount = 0;
int currentRateIndex = -1;
time_t graphStartTime = 0;  // Store the start time of the graph for axis labels

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

  JsonDocument timeDoc;
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

time_t parseISOTimestamp(const char* isoTime) {
  if (!isoTime || strlen(isoTime) < 19) {
    return 0;
  }

  struct tm tm = {0};
  int year, month, day, hour, min, sec;

  if (sscanf(isoTime, "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &min, &sec) == 6) {
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    tm.tm_isdst = 0;

    // Convert to time_t - since we configured ESP32 with UTC (configTime(0, 0, ...)),
    // mktime treats this as UTC
    return mktime(&tm);
  }
  return 0;
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

  // Get prices for full current day (midnight to midnight)
  // Calculate seconds since midnight
  int secondsSinceMidnight = currentInfo.tm_hour * 3600 + currentInfo.tm_min * 60 + currentInfo.tm_sec;
  time_t periodStart = now - secondsSinceMidnight;  // Start of today (midnight)
  time_t periodEnd = periodStart + 86400;           // End of today (next midnight)
  graphStartTime = periodStart;                     // Store for axis labels

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

  JsonDocument doc;
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

  // Store all rates for graphing and find current rate
  JsonObject selectedRate;
  bool hasSelectedRate = false;
  JsonObject fallbackRate;
  bool hasFallbackRate = false;
  rateCount = 0;
  currentRateIndex = -1;

  int loopIndex = 0;
  for (JsonObject rate : results) {
    // Store rate data for graphing with actual timestamps
    if (rateCount < 60) {
      const char* validFromStr = rate["valid_from"] | "";
      double priceVal = rate["value_inc_vat"] | 0.0;

      if (priceVal > 0 && strlen(validFromStr) > 0) {
        rates[rateCount].price = priceVal;
        rates[rateCount].validFrom = parseISOTimestamp(validFromStr);
        rateCount++;
      }
    }

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
    if (!hasSelectedRate && (strncmp(validFromCandidate, currentIso, 19) <= 0) && (strncmp(currentIso, validToCandidate, 19) < 0)) {
      selectedRate = rate;
      hasSelectedRate = true;
      currentRateIndex = rateCount - 1;  // Current rate is the last one we stored
      Serial.print("Matched current slot: ");
      Serial.print(validFromCandidate);
      Serial.print(" to ");
      Serial.println(validToCandidate);
      // Don't break - continue storing all rates for full day view
    }

    loopIndex++;
  }

  Serial.print("Stored ");
  Serial.print(rateCount);
  Serial.print(" rates for graphing, current index: ");
  Serial.println(currentRateIndex);

  // Reverse the rates array to get chronological order (API returns newest first)
  for (int i = 0; i < rateCount / 2; i++) {
    RateData temp = rates[i];
    rates[i] = rates[rateCount - 1 - i];
    rates[rateCount - 1 - i] = temp;
  }
  // Update current rate index after reversal
  if (currentRateIndex >= 0) {
    currentRateIndex = rateCount - 1 - currentRateIndex;
  }

  // Keep graphStartTime as midnight for full 24-hour day view

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

void drawPriceGraph(int x, int y, int width, int height) {
  if (rateCount < 2 || graphStartTime == 0) return;

  // Find min/max prices for scaling
  double minPrice = rates[0].price;
  double maxPrice = rates[0].price;
  for (int i = 1; i < rateCount; i++) {
    if (rates[i].price < minPrice) minPrice = rates[i].price;
    if (rates[i].price > maxPrice) maxPrice = rates[i].price;
  }

  // Round to nice values for axis (multiples of 5)
  minPrice = floor(minPrice / 5.0) * 5.0;
  maxPrice = ceil(maxPrice / 5.0) * 5.0;
  if (maxPrice - minPrice < 10) maxPrice = minPrice + 10;
  double priceRange = maxPrice - minPrice;

  // Calculate median price for color threshold
  double sortedPrices[60];
  for (int i = 0; i < rateCount; i++) {
    sortedPrices[i] = rates[i].price;
  }
  for (int i = 0; i < rateCount - 1; i++) {
    for (int j = i + 1; j < rateCount; j++) {
      if (sortedPrices[i] > sortedPrices[j]) {
        double temp = sortedPrices[i];
        sortedPrices[i] = sortedPrices[j];
        sortedPrices[j] = temp;
      }
    }
  }
  double medianPrice = sortedPrices[rateCount / 2];

  // Use fixed 24-hour time range (midnight to midnight)
  time_t timeRange = 86400;  // 24 hours in seconds

  // Draw horizontal grid lines at 5p intervals
  display.setFont();
  for (double price = minPrice; price <= maxPrice; price += 5.0) {
    int gridY = y + height - (int)((price - minPrice) / priceRange * height);
    display.drawLine(x, gridY, x + width, gridY, GxEPD_BLACK);

    // Draw Y-axis label
    char label[10];
    snprintf(label, sizeof(label), "%.1fp", price);
    display.setCursor(2, gridY + 3);
    display.print(label);
  }

  // Draw bars for each rate
  for (int i = 0; i < rateCount; i++) {
    // Calculate x position and width based on time
    int barX = x + ((rates[i].validFrom - graphStartTime) * width) / timeRange;
    int nextX;
    if (i < rateCount - 1) {
      nextX = x + ((rates[i + 1].validFrom - graphStartTime) * width) / timeRange;
    } else {
      // For the last bar, assume 30-minute slot (1800 seconds)
      nextX = x + ((rates[i].validFrom + 1800 - graphStartTime) * width) / timeRange;
    }
    int barWidth = nextX - barX - 1;  // -1 for spacing between bars

    // Calculate bar height
    int barHeight = (int)((rates[i].price - minPrice) / priceRange * height);
    int barY = y + height - barHeight;

    // Ensure bar is within bounds
    barX = constrain(barX, x, x + width);
    barWidth = constrain(barWidth, 1, width);
    barY = constrain(barY, y, y + height);
    barHeight = constrain(barHeight, 0, height);

    // Fill bar with color (green for low, red for high)
    // E-paper displays: GxEPD_BLACK for filled, GxEPD_WHITE for outline
    // We'll use filled rectangles with borders
    if (rates[i].price > medianPrice) {
      // High price - draw as filled black (will appear dark)
      display.fillRect(barX, barY, barWidth, barHeight, GxEPD_BLACK);
    } else {
      // Low price - draw as white with black border
      display.fillRect(barX, barY, barWidth, barHeight, GxEPD_WHITE);
    }

    // Draw black border around bar
    display.drawRect(barX, barY, barWidth, barHeight, GxEPD_BLACK);
  }

  // Draw X-axis time labels
  struct tm timeInfo;
  for (int i = 0; i < rateCount; i++) {
    if (timeToUtcStruct(rates[i].validFrom, timeInfo)) {
      // Show label every 2 hours on the hour
      if (timeInfo.tm_min == 0 && timeInfo.tm_hour % 2 == 0) {
        int labelX = x + ((rates[i].validFrom - graphStartTime) * width) / timeRange;
        char timeLabel[4];
        snprintf(timeLabel, sizeof(timeLabel), "%d", timeInfo.tm_hour);
        display.setCursor(labelX - 3, y + height + 5);
        display.print(timeLabel);
      }
    }
  }
}

void clearDisplay() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
  } while (display.nextPage());
  display.hibernate();
}

void updateDisplay() {
  Serial.println("Updating display...");

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Draw price graph with room for time labels at bottom
    // x=35 (room for Y labels), y=5, width=255, height=110 (leaves room for hour labels)
    if (rateCount > 0) {
      drawPriceGraph(35, 5, 255, 110);
    }

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
  display.init(115200, true, 2, false);  // Last param false = no border
  display.setRotation(1);
  // Skip initial clearDisplay() to avoid timeout with custom driver
  // Display will be updated with content immediately after data fetch

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
