#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include "GxEPD2_290_Custom.h"  // Custom driver with white border
#include <time.h>
#include <sys/time.h>
#include <cstring>
#include "credentials.h"  // WiFi credentials
#include <esp_sleep.h>

// Pin definitions for ESP32 SPI connection
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4
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

// Graph positioning and size
const int GRAPH_X = 0;              // Graph left margin
const int GRAPH_Y = 5;              // Graph top margin
const int GRAPH_HEIGHT = 110;       // Graph height in pixels

// Label positioning
const int Y_LABEL_OFFSET = 2;               // Gap between graph and price labels (right side)
const int Y_LABEL_VERTICAL_OFFSET = -3;     // Vertical adjustment for price labels (adjust to align with grid lines)
const int HOUR_LABEL_CENTER_OFFSET = 1;     // Fine-tune horizontal centering of hour labels (adjust to align with bars)
const int HOUR_LABEL_Y_OFFSET = 5;          // Gap between graph and hour labels (bottom)

// Time constants (seconds)
const time_t SECONDS_PER_DAY = 86400;
const time_t RATE_SLOT_DURATION = 1800;  // 30 minutes
const time_t WAKE_INTERVAL_S = 900;      // Wake every 15 minutes (quarter-hour)
const time_t PRICE_FETCH_INTERVAL_S = 6 * 60 * 60; // Fetch prices every 6 hours
const time_t TIME_SYNC_INTERVAL_S = 24 * 60 * 60;  // Re-sync time every 24 hours

// RTC memory variables (persist across deep sleep)
RTC_DATA_ATTR time_t rtcLastPriceFetch = 0;
RTC_DATA_ATTR time_t rtcLastTimeSync = 0;
RTC_DATA_ATTR int rtcBootCount = 0;

// Graph display constants
const double PRICE_GRID_INTERVAL = 5.0;                   // Grid line every 5 pence
const int HOUR_LABEL_INTERVAL = 2;                        // Show hour label every 2 hours
const int MAX_RATES = 60;                                 // Buffer size for rate data

// Bar sizing (adjust these to change appearance)
const int EXPECTED_SLOTS = 46;                            // Expected number of 30-min slots (0:00 to 23:00)
const int BAR_WIDTH = 5;                                  // Bar width in pixels (odd number for centering)
const int BAR_GAP = 1;                                    // Total gap per slot (1px each side of bar)

// Calculated dimensions (auto-update based on above)
const int SLOT_WIDTH = BAR_WIDTH + BAR_GAP;               // Width of each time slot (currently 5px)
const int GRAPH_WIDTH = EXPECTED_SLOTS * SLOT_WIDTH;      // Total graph width (currently 230px)

// Store rate data for graphing (in RTC memory to persist across deep sleep)
struct RateData {
  time_t validFrom;
  double price;
};
RTC_DATA_ATTR RateData rates[MAX_RATES];
RTC_DATA_ATTR int rateCount = 0;
RTC_DATA_ATTR int currentRateIndex = -1;
RTC_DATA_ATTR time_t graphStartTime = 0;  // Store the start time of the graph for axis labels

bool timeToUtcStruct(time_t timestamp, struct tm& out);

void logWithTimestamp(const char* message) {
  time_t now = time(nullptr);
  if (now >= 1000000000) {
    struct tm info;
    if (timeToUtcStruct(now, info)) {
      char ts[25];
      strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &info);
      Serial.print("[");
      Serial.print(ts);
      Serial.print("] ");
      Serial.println(message);
      return;
    }
  }
  Serial.println(message);
}

bool waitForTimeSync() {
  time_t now = time(nullptr);
  int attempts = 0;
  while ((now < 1000000000) && (attempts < 40)) {  // wait until reasonable epoch
    delay(500);
    now = time(nullptr);
    attempts++;
  }

  if (now < 1000000000) {
    logWithTimestamp("Unable to get valid time from NTP.");
    return false;
  }

  logWithTimestamp("Time synchronized via NTP.");
  return true;
}

bool syncTimeFromHttp() {
  const char* timeUrl = "https://worldtimeapi.org/api/timezone/Europe/London";
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, timeUrl)) {
    logWithTimestamp("Failed to init HTTP client for time API.");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("Time API HTTP Error: ");
    Serial.println(httpCode);
    logWithTimestamp("Time API HTTP error.");
    http.end();
    return false;
  }

  JsonDocument timeDoc;
  DeserializationError error = deserializeJson(timeDoc, http.getString());
  http.end();
  if (error) {
    Serial.print("Time JSON parse failed: ");
    Serial.println(error.f_str());
    logWithTimestamp("Time JSON parse failed.");
    return false;
  }

  long unixTime = timeDoc["unixtime"] | 0;
  if (unixTime <= 0) {
    logWithTimestamp("Invalid time received from API.");
    return false;
  }

  struct timeval tv;
  tv.tv_sec = unixTime;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.print("Time synced via HTTP API: ");
  Serial.println(unixTime);
  logWithTimestamp("Time synchronized via HTTP.");

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

struct PriceStats {
  double minPrice;
  double maxPrice;
  double medianPrice;
};

PriceStats calculatePriceStats() {
  PriceStats stats;

  // Find min/max prices
  stats.minPrice = rates[0].price;
  stats.maxPrice = rates[0].price;
  for (int i = 1; i < rateCount; i++) {
    if (rates[i].price < stats.minPrice) stats.minPrice = rates[i].price;
    if (rates[i].price > stats.maxPrice) stats.maxPrice = rates[i].price;
  }

  // Round to nice values for axis (multiples of 5)
  stats.minPrice = floor(stats.minPrice / PRICE_GRID_INTERVAL) * PRICE_GRID_INTERVAL;
  stats.maxPrice = ceil(stats.maxPrice / PRICE_GRID_INTERVAL) * PRICE_GRID_INTERVAL;
  if (stats.maxPrice - stats.minPrice < 10) {
    stats.maxPrice = stats.minPrice + 10;
  }

  // Calculate median price for color threshold
  double sortedPrices[MAX_RATES];
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
  stats.medianPrice = sortedPrices[rateCount / 2];

  return stats;
}

void drawGridLinesAndLabels(int x, int y, int width, int height, double minPrice, double maxPrice, double priceRange) {
  display.setFont();
  for (double price = minPrice; price <= maxPrice; price += PRICE_GRID_INTERVAL) {
    int gridY = y + height - (int)((price - minPrice) / priceRange * height);

    // Draw grid line (solid for bottom line, dashed for others)
    if (price == minPrice) {
      // Solid line for bottom of graph
      display.drawLine(x, gridY, x + width, gridY, GxEPD_BLACK);
    } else {
      // Dashed horizontal grid line (lighter appearance)
      const int dashLength = 1;
      const int gapLength = 2;
      for (int xx = x; xx <= x + width; xx += dashLength + gapLength) {
        int xEnd = min(xx + dashLength, x + width);
        display.drawLine(xx, gridY, xEnd, gridY, GxEPD_BLACK);
      }
    }

    // Draw Y-axis label on the right
    char label[10];
    snprintf(label, sizeof(label), "%.0fp", price);
    display.setCursor(x + width + Y_LABEL_OFFSET, gridY + Y_LABEL_VERTICAL_OFFSET);
    display.print(label);
  }
}

void drawPriceBars(int x, int y, int width, int height, double minPrice, double priceRange, double medianPrice) {
  for (int i = 0; i < rateCount; i++) {
    // Calculate slot position (each slot is SLOT_WIDTH pixels)
    int slotStartX = x + (i * SLOT_WIDTH);

    // Center the fixed-width bar within the slot
    // BAR_GAP pixels distributed around bar: gap/2 offset from slot start
    int barX = slotStartX + (BAR_GAP / 2);

    // Calculate bar height
    int barHeight = (int)((rates[i].price - minPrice) / priceRange * height);
    int barY = y + height - barHeight;

    // Ensure bar is within bounds
    barX = constrain(barX, x, x + width);
    barY = constrain(barY, y, y + height);
    barHeight = constrain(barHeight, 0, height);

    // Fill bar based on price
    if (rates[i].price > medianPrice) {
      // High price - draw as filled black (expensive)
      display.fillRect(barX, barY, BAR_WIDTH, barHeight, GxEPD_BLACK);
    } else {
      // Low price - draw as white with black border (cheaper)
      display.fillRect(barX, barY, BAR_WIDTH, barHeight, GxEPD_WHITE);
    }

    // Draw black border on three sides (left, right, top - no bottom as bars sit on baseline)
    display.drawLine(barX, barY, barX, barY + barHeight - 1, GxEPD_BLACK);              // Left edge
    display.drawLine(barX + BAR_WIDTH - 1, barY, barX + BAR_WIDTH - 1, barY + barHeight - 1, GxEPD_BLACK);  // Right edge
    display.drawLine(barX, barY, barX + BAR_WIDTH - 1, barY, GxEPD_BLACK);              // Top edge
  }
}

void drawTimeLabels(int x, int y, int width, int height) {
  struct tm timeInfo;
  for (int i = 0; i < rateCount; i++) {
    if (timeToUtcStruct(rates[i].validFrom, timeInfo)) {
      // Show label at regular hour intervals
      if (timeInfo.tm_min == 0 && timeInfo.tm_hour % HOUR_LABEL_INTERVAL == 0) {
        // Calculate center of the bar (not slot) for proper alignment
        int slotStartX = x + (i * SLOT_WIDTH);
        int barX = slotStartX + (BAR_GAP / 2);
        int barCenterX = barX + (BAR_WIDTH / 2);

        // Draw alignment notch at bar center
        display.drawLine(barCenterX, y + height, barCenterX, y + height + 2, GxEPD_BLACK);

        char timeLabel[4];
        snprintf(timeLabel, sizeof(timeLabel), "%d", timeInfo.tm_hour);

        // Center the text by calculating its width (default font is 6px per character)
        int textWidth = strlen(timeLabel) * 6;
        int centeredX = barCenterX - (textWidth / 2) + HOUR_LABEL_CENTER_OFFSET;

        display.setCursor(centeredX, y + height + HOUR_LABEL_Y_OFFSET);
        display.print(timeLabel);
      }
    }
  }
}

void drawCurrentTimeSlot(int x, int y, int width, int height) {
  if (currentRateIndex >= 0 && currentRateIndex < rateCount) {
    // Calculate the center of the current slot's bar
    int slotStartX = x + (currentRateIndex * SLOT_WIDTH);
    int barX = slotStartX + (BAR_GAP / 2);
    int currentX = barX + (BAR_WIDTH / 2);

    // Solid vertical line to indicate current slot, centered in the bar
    display.drawLine(currentX, y, currentX, y + height, GxEPD_BLACK);
  }
}

void updateCurrentRateIndexFromNow() {
  if (rateCount <= 0) return;

  time_t now = time(nullptr);
  if (now < 1000000000) return;  // time not set

  currentRateIndex = -1;
  for (int i = 0; i < rateCount; i++) {
    time_t slotStart = rates[i].validFrom;
    time_t slotEnd = (i < rateCount - 1)
        ? rates[i + 1].validFrom
        : rates[i].validFrom + RATE_SLOT_DURATION;

    if (now >= slotStart && now < slotEnd) {
      currentRateIndex = i;
      break;
    }
  }
}

bool fetchCurrentPrice() {
  if (WiFi.status() != WL_CONNECTED) {
    logWithTimestamp("Price fetch skipped: WiFi disconnected.");
    return false;
  }

  logWithTimestamp("Price fetch start.");
  time_t now = time(nullptr);
  if (now < 1000000000) {
    logWithTimestamp("Time not set - attempting to sync again.");
    if (!waitForTimeSync() && !syncTimeFromHttp()) {
      return false;
    }
    now = time(nullptr);
  }

  struct tm currentInfo;
  if (!timeToUtcStruct(now, currentInfo)) {
    logWithTimestamp("Time conversion failed.");
    return false;
  }

  // Get prices for full current day (midnight to midnight)
  // Calculate seconds since midnight
  int secondsSinceMidnight = currentInfo.tm_hour * 3600 + currentInfo.tm_min * 60 + currentInfo.tm_sec;
  time_t periodStart = now - secondsSinceMidnight;     // Start of today (midnight)
  time_t periodEnd = periodStart + SECONDS_PER_DAY;   // End of today (next midnight)
  graphStartTime = periodStart;                        // Store for axis labels

  struct tm periodStartInfo, periodEndInfo;
  if (!timeToUtcStruct(periodStart, periodStartInfo)) {
    return false;
  }
  if (!timeToUtcStruct(periodEnd, periodEndInfo)) {
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
  logWithTimestamp("Price fetch HTTP request created.");

  WiFiClientSecure client;
  client.setInsecure();  // Octopus Energy uses a trusted certificate; skip CA check for simplicity

  HTTPClient http;
  if (!http.begin(client, url)) {
    logWithTimestamp("Failed to initialize HTTP client.");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP Error: ");
    Serial.println(httpCode);
    logWithTimestamp("Price fetch HTTP error.");
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
    logWithTimestamp("Price JSON parse failed.");
    return false;
  }

  JsonArray results = doc["results"];
  if (!results || results.size() == 0) {
    logWithTimestamp("No rate data returned.");
    return false;
  }

  Serial.print("Found ");
  Serial.print(results.size());
  Serial.println(" rate periods");
  Serial.print("Current time (UTC): ");
  Serial.println(currentIso);

  // Store all rates for graphing
  rateCount = 0;
  for (JsonObject rate : results) {
    // Store rate data for graphing with actual timestamps
    if (rateCount < MAX_RATES) {
      const char* validFromStr = rate["valid_from"] | "";
      double priceVal = rate["value_inc_vat"] | 0.0;

      if (strlen(validFromStr) > 0) {
        rates[rateCount].price = priceVal;
        rates[rateCount].validFrom = parseISOTimestamp(validFromStr);
        rateCount++;
      }
    }
  }

  Serial.print("Stored ");
  Serial.print(rateCount);
  Serial.println(" rates for graphing.");

  // Reverse the rates array to get chronological order (API returns newest first)
  for (int i = 0; i < rateCount / 2; i++) {
    RateData temp = rates[i];
    rates[i] = rates[rateCount - 1 - i];
    rates[rateCount - 1 - i] = temp;
  }

  // Cap to expected slots (46 slots from 0:00 to 23:00)
  if (rateCount > EXPECTED_SLOTS) {
    rateCount = EXPECTED_SLOTS;
  }

  logWithTimestamp("Price fetch complete.");
  return true;
}

void drawPriceGraph(int x, int y, int width, int height) {
  if (rateCount < 2 || graphStartTime == 0) return;

  // Calculate price statistics
  PriceStats stats = calculatePriceStats();
  double priceRange = stats.maxPrice - stats.minPrice;

  // Draw all graph components
  drawGridLinesAndLabels(x, y, width, height, stats.minPrice, stats.maxPrice, priceRange);
  drawCurrentTimeSlot(x, y, width, height);
  drawPriceBars(x, y, width, height, stats.minPrice, priceRange, stats.medianPrice);
  drawTimeLabels(x, y, width, height);
}

void enterDeepSleep(time_t sleepSeconds) {
  // Ensure WiFi is off before sleeping
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.print("Entering deep sleep for ");
  Serial.print(sleepSeconds);
  Serial.println(" seconds");
  Serial.flush();  // Ensure serial output completes

  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);  // Convert to microseconds
  esp_deep_sleep_start();
}

void updateDisplay() {
  logWithTimestamp("Display update start.");

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Draw price graph with Y-axis labels on right, time labels at bottom
    if (rateCount > 0) {
      logWithTimestamp("Graph render start.");
      drawPriceGraph(GRAPH_X, GRAPH_Y, GRAPH_WIDTH, GRAPH_HEIGHT);
      logWithTimestamp("Graph render complete.");
    }

  } while (display.nextPage());

  logWithTimestamp("Display update complete.");
  display.hibernate();
}

void setup() {
  Serial.begin(115200);
  delay(100);  // Allow serial to initialize

  // Increment boot counter
  rtcBootCount++;
  Serial.print("Boot #");
  Serial.print(rtcBootCount);
  Serial.print(" - Wake reason: ");
  Serial.println(esp_sleep_get_wakeup_cause());
  logWithTimestamp("ESP32 Octopus Price Display starting.");

  // Initialize the display
  display.init(115200, true, 2, false);
  display.setRotation(1);

  // Determine if we need WiFi this wake cycle
  time_t now = time(nullptr);
  bool needTimeSync = (rtcBootCount == 1) || (now < 1000000000) ||
      (now >= 1000000000 && rtcLastTimeSync > 0 && (now - rtcLastTimeSync) >= TIME_SYNC_INTERVAL_S);
  bool needPriceFetch = (rtcBootCount == 1) || (rateCount == 0) ||
      (now >= 1000000000 && rtcLastPriceFetch > 0 && (now - rtcLastPriceFetch) >= PRICE_FETCH_INTERVAL_S);
  bool needWiFi = needTimeSync || needPriceFetch;

  if (needWiFi) {
    // Connect to WiFi
    logWithTimestamp("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\nWiFi connection failed!");
      logWithTimestamp("WiFi connection failed.");
      // If we have rate data, update display anyway and sleep
      if (rateCount > 0) {
        updateCurrentRateIndexFromNow();
        updateDisplay();
      }
      enterDeepSleep(WAKE_INTERVAL_S);
      return;
    }

    Serial.println("\nWiFi connected!");
    logWithTimestamp("WiFi connected.");

    // Sync time if needed
    if (needTimeSync) {
      configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
      logWithTimestamp("Time sync starting.");
      if (!waitForTimeSync()) {
        syncTimeFromHttp();
      }
      now = time(nullptr);
      if (now >= 1000000000) {
        rtcLastTimeSync = now;
      }
    }

    // Fetch prices if needed
    if (needPriceFetch) {
      logWithTimestamp("Fetching prices.");
      if (fetchCurrentPrice()) {
        now = time(nullptr);
        rtcLastPriceFetch = now;
      }
    }

    // Disconnect WiFi immediately after use
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    logWithTimestamp("WiFi disconnected.");
  } else {
    logWithTimestamp("Display-only wake - skipping WiFi.");
  }

  // Update current rate index based on current time
  updateCurrentRateIndexFromNow();

  // Update display if we have rate data
  if (rateCount > 0) {
    logWithTimestamp("Updating display.");
    updateDisplay();
  } else {
    logWithTimestamp("No rate data - skipping display update.");
  }

  // Calculate time until next quarter-hour boundary
  now = time(nullptr);
  if (now >= 1000000000) {
    time_t secondsIntoQuarter = now % WAKE_INTERVAL_S;
    time_t sleepSeconds = WAKE_INTERVAL_S - secondsIntoQuarter;

    // Add a small buffer to ensure we're past the boundary
    sleepSeconds += 5;

    logWithTimestamp("Entering deep sleep mode.");
    enterDeepSleep(sleepSeconds);
  } else {
    // Fallback if time sync failed
    logWithTimestamp("Time not set - sleeping for default interval.");
    enterDeepSleep(WAKE_INTERVAL_S);
  }
}

void loop() {
  // Not used - setup() handles everything and enters deep sleep.
  // If we somehow reach here, sleep immediately.
  enterDeepSleep(WAKE_INTERVAL_S);
}
