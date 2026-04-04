#include "time_utils.h"
#include "globals.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <sys/time.h>
#include <esp_sntp.h>

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

void applyDriftCorrection() {
  time_t now = time(nullptr);
  if (now < 1000000000 || rtcLastCorrectionTime == 0 || rtcDriftPerHour == 0.0) return;

  double elapsedHours = (double)(now - rtcLastCorrectionTime) / 3600.0;
  rtcAccumulatedDrift += rtcDriftPerHour * elapsedHours;

  long correctionSeconds = (long)rtcAccumulatedDrift;  // truncate toward zero
  if (correctionSeconds == 0) {
    // No integer correction yet, but update baseline so elapsed tracking stays correct
    rtcLastCorrectionTime = now;
    return;
  }

  struct timeval tv;
  tv.tv_sec = now - correctionSeconds;  // Subtract drift (positive drift = clock ahead)
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  rtcLastCorrectionTime = tv.tv_sec;

  // Keep the fractional remainder for next wake
  rtcAccumulatedDrift -= (double)correctionSeconds;

  Serial.print("Drift correction applied: ");
  Serial.print(-correctionSeconds);
  Serial.print("s (remainder: ");
  Serial.print(rtcAccumulatedDrift, 3);
  Serial.println("s)");
}

bool waitForNtpSync() {
  // Wait for SNTP to report a completed sync — not just "clock is already set".
  // sntp_get_sync_status() returns SNTP_SYNC_STATUS_COMPLETED once the NTP
  // response arrives and the system clock is updated with that value.
  for (int i = 0; i < 40; i++) {
    delay(500);
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      logWithTimestamp("Time synchronized via NTP.");
      return true;
    }
  }
  logWithTimestamp("NTP sync timed out.");
  return false;
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

  return time(nullptr) >= 1000000000;
}

bool timeToUtcStruct(time_t timestamp, struct tm& out) {
  struct tm* tmp = gmtime(&timestamp);
  if (!tmp) return false;
  memcpy(&out, tmp, sizeof(struct tm));
  return true;
}

bool timeToLocalStruct(time_t timestamp, struct tm& out) {
  struct tm* tmp = localtime(&timestamp);
  if (!tmp) return false;
  memcpy(&out, tmp, sizeof(struct tm));
  return true;
}

time_t utcStructToEpoch(const struct tm& utcTime) {
  static const int daysBeforeMonth[] = {
      0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
  };

  int year = utcTime.tm_year + 1900;
  if (year < 1970) return 0;

  auto isLeapYear = [](int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  };

  long days = 0;
  for (int y = 1970; y < year; ++y) {
    days += isLeapYear(y) ? 366 : 365;
  }

  days += daysBeforeMonth[utcTime.tm_mon];
  if (utcTime.tm_mon >= 2 && isLeapYear(year)) {
    days += 1;
  }
  days += utcTime.tm_mday - 1;

  return (time_t)days * SECONDS_PER_DAY +
      utcTime.tm_hour * 3600L +
      utcTime.tm_min * 60L +
      utcTime.tm_sec;
}

void configureLocalTimezone() {
  setenv("TZ", LOCAL_TIMEZONE, 1);
  tzset();
}

time_t parseISOTimestamp(const char* isoTime) {
  if (!isoTime || strlen(isoTime) < 19) return 0;

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

    // Octopus timestamps are UTC (`...Z`), so convert explicitly instead of
    // relying on the process timezone or non-portable libc helpers.
    return utcStructToEpoch(tm);
  }
  return 0;
}
