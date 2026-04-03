#include "octopus.h"
#include "globals.h"
#include "time_utils.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>

bool fetchCurrentPrice() {
  if (WiFi.status() != WL_CONNECTED) {
    logWithTimestamp("Price fetch skipped: WiFi disconnected.");
    return false;
  }

  logWithTimestamp("Price fetch start.");
  time_t now = time(nullptr);
  if (now < 1000000000) {
    logWithTimestamp("Time not set - attempting to sync again.");
    if (!waitForNtpSync() && !syncTimeFromHttp()) {
      return false;
    }
    now = time(nullptr);
  }

  // Fetch a 48-hour window centred on now (rounded to slot boundaries).
  // This ensures today's prices are always present, and tomorrow's are included
  // as soon as Octopus releases them (~16:00 the day before).
  time_t slotNow    = (now / RATE_SLOT_DURATION) * RATE_SLOT_DURATION;
  time_t periodStart = slotNow - 24 * 3600;
  time_t periodEnd   = slotNow + 24 * 3600;

  struct tm periodStartInfo, periodEndInfo;
  if (!timeToUtcStruct(periodStart, periodStartInfo)) return false;
  if (!timeToUtcStruct(periodEnd, periodEndInfo))   return false;

  char nowIso[25], periodStartIso[25], periodEndIso[25];
  struct tm nowInfo;
  timeToUtcStruct(now, nowInfo);
  strftime(nowIso,         sizeof(nowIso),         "%Y-%m-%dT%H:%M:%SZ", &nowInfo);
  strftime(periodStartIso, sizeof(periodStartIso), "%Y-%m-%dT%H:%M:%SZ", &periodStartInfo);
  strftime(periodEndIso,   sizeof(periodEndIso),   "%Y-%m-%dT%H:%M:%SZ", &periodEndInfo);

  String url = "https://api.octopus.energy/v1/products/";
  url += OCTOPUS_PRODUCT_CODE;
  url += "/electricity-tariffs/";
  url += OCTOPUS_TARIFF_CODE;
  url += "/standard-unit-rates/?page_size=100&period_from=";
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
  Serial.println(nowIso);

  // Store into fetchedRates[] (full 48-hr buffer; display window selected separately)
  fetchedRateCount = 0;
  for (JsonObject rate : results) {
    if (fetchedRateCount < FETCH_MAX_RATES) {
      const char* validFromStr = rate["valid_from"] | "";
      double priceVal = rate["value_inc_vat"] | 0.0;

      if (strlen(validFromStr) > 0) {
        fetchedRates[fetchedRateCount].price     = priceVal;
        fetchedRates[fetchedRateCount].validFrom = parseISOTimestamp(validFromStr);
        fetchedRateCount++;
      }
    }
  }

  Serial.print("Fetched ");
  Serial.print(fetchedRateCount);
  Serial.println(" rates.");

  // Reverse to chronological order (API returns newest first)
  for (int i = 0; i < fetchedRateCount / 2; i++) {
    RateData temp                            = fetchedRates[i];
    fetchedRates[i]                          = fetchedRates[fetchedRateCount - 1 - i];
    fetchedRates[fetchedRateCount - 1 - i]  = temp;
  }

  logWithTimestamp("Price fetch complete.");
  return true;
}
