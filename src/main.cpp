#include <Arduino.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include "credentials.h"
#include "globals.h"
#include "time_utils.h"
#include "octopus.h"
#include "display.h"

// ---- RTC-persisted state (definitions) ----
// Declared extern in globals.h so all modules can read/write these.
RTC_DATA_ATTR time_t   rtcLastPriceFetch      = 0;
RTC_DATA_ATTR time_t   rtcLastTimeSync        = 0;
RTC_DATA_ATTR int      rtcBootCount           = 0;
RTC_DATA_ATTR time_t   rtcLastDisplayedRateStart = 0;
RTC_DATA_ATTR int      rtcRefreshCounter      = 0;
RTC_DATA_ATTR double   rtcDriftPerHour        = 0.0;
RTC_DATA_ATTR double   rtcAccumulatedDrift    = 0.0;
RTC_DATA_ATTR time_t   rtcLastCorrectionTime  = 0;
RTC_DATA_ATTR bool     rtcFetchedAfter4pm     = false;
RTC_DATA_ATTR RateData fetchedRates[FETCH_MAX_RATES];
RTC_DATA_ATTR int      fetchedRateCount       = 0;
RTC_DATA_ATTR RateData rates[MAX_RATES];
RTC_DATA_ATTR int      rateCount              = 0;
RTC_DATA_ATTR int      currentRateIndex       = -1;
RTC_DATA_ATTR time_t   graphStartTime         = 0;

// ---- Sleep ----

void enterDeepSleep(time_t sleepSeconds) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  Serial.print("Entering deep sleep for ");
  Serial.print(sleepSeconds);
  Serial.println(" seconds");
  Serial.flush();

  esp_sleep_enable_timer_wakeup(sleepSeconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---- Main ----

void setup() {
  Serial.begin(115200);
  delay(100);
  configureLocalTimezone();

  rtcBootCount++;
  Serial.print("Boot #");
  Serial.print(rtcBootCount);
  Serial.print(" - Wake reason: ");
  Serial.println(esp_sleep_get_wakeup_cause());
  logWithTimestamp("ESP32 Octopus Price Display starting.");

  display.init(115200, true, 2, false);
  display.setRotation(1);

  time_t now = time(nullptr);
  bool needPriceFetch = (rtcBootCount == 1) || (rateCount == 0) ||
      (now >= 1000000000 && rtcLastPriceFetch > 0 &&
       (now - rtcLastPriceFetch) >= PRICE_FETCH_INTERVAL_S);

  // Smart fetch: capture next-day prices released around 16:00 UK local time.
  if (!needPriceFetch && now >= 1000000000) {
    struct tm localNow;
    if (timeToLocalStruct(now, localNow)) {
      if (localNow.tm_hour == 16 && !rtcFetchedAfter4pm) {
        needPriceFetch = true;
        logWithTimestamp("Smart fetch: 16:00 window for next-day prices.");
      }
      if (localNow.tm_hour < 16) {
        rtcFetchedAfter4pm = false;
      }
    }
  }

  // Early drift calibration: sync at ~15min (boot 2) and ~1hr (boot 5)
  bool needEarlyDriftSync = (rtcBootCount == 2 || rtcBootCount == 5) &&
      now >= 1000000000 && rtcLastCorrectionTime > 0;

  bool needWiFi    = needPriceFetch || needEarlyDriftSync || (now < 1000000000);
  bool needTimeSync = needWiFi;  // Always re-sync time when WiFi is already needed

  if (needWiFi) {
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
      if (fetchedRateCount > 0) {
        selectDisplayWindow();
        updateDisplay();
      }
      enterDeepSleep(WAKE_INTERVAL_S);
      return;
    }

    Serial.println("\nWiFi connected!");
    logWithTimestamp("WiFi connected.");

    if (needTimeSync) {
      time_t rtcTimeBeforeSync = time(nullptr);

      // Reset sync status so waitForNtpSync() detects a fresh response,
      // not a cached COMPLETED status from a previous boot.
      sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
      configTzTime(LOCAL_TIMEZONE, "pool.ntp.org", "time.nist.gov", "time.google.com");
      logWithTimestamp("Time sync starting.");
      if (!waitForNtpSync()) {
        syncTimeFromHttp();
      }
      now = time(nullptr);
      if (now >= 1000000000) {
        // Measure and record drift rate.
        // Use rtcLastTimeSync (last NTP sync) for elapsed, not rtcLastCorrectionTime —
        // the latter is updated every 15-min non-WiFi wake, keeping the window too
        // short for a meaningful drift measurement.
        if (rtcTimeBeforeSync >= 1000000000 && rtcLastTimeSync > 0) {
          double elapsedHours = (double)(rtcTimeBeforeSync - rtcLastTimeSync) / 3600.0;
          if (elapsedHours >= 0.5) {
            long   driftSeconds       = rtcTimeBeforeSync - now;
            double residualDriftPerHour = (double)driftSeconds / elapsedHours;
            double trueDriftEstimate  = rtcDriftPerHour + residualDriftPerHour;
            if (rtcDriftPerHour != 0.0) {
              rtcDriftPerHour = rtcDriftPerHour * 0.5 + trueDriftEstimate * 0.5;
            } else {
              rtcDriftPerHour = trueDriftEstimate;  // first measurement: raw drift
            }
            Serial.print("RTC drift measured (residual): ");
            Serial.print(residualDriftPerHour, 4);
            Serial.print("s/hr, smoothed estimate: ");
            Serial.print(rtcDriftPerHour, 4);
            Serial.println("s/hr");
          }
        }
        rtcLastTimeSync       = now;
        rtcLastCorrectionTime = now;
        rtcAccumulatedDrift   = 0.0;  // NTP is ground truth — reset accumulator
      }
    }

    if (needPriceFetch) {
      logWithTimestamp("Fetching prices.");
      if (fetchCurrentPrice()) {
        now = time(nullptr);
        rtcLastPriceFetch = now;
        struct tm fetchLocal;
        if (timeToLocalStruct(now, fetchLocal) && fetchLocal.tm_hour >= 16) {
          rtcFetchedAfter4pm = true;
        }
      }
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    logWithTimestamp("WiFi disconnected.");
  } else {
    logWithTimestamp("Display-only wake - skipping WiFi.");
    applyDriftCorrection();
  }

  selectDisplayWindow();

  bool needDisplayUpdate = false;
  if (rateCount > 0) {
    time_t currentRateStart = (currentRateIndex >= 0 && currentRateIndex < rateCount)
        ? rates[currentRateIndex].validFrom : 0;

    if (currentRateStart != rtcLastDisplayedRateStart) {
      logWithTimestamp("Rate slot changed - updating display.");
      needDisplayUpdate = true;
    } else if (needPriceFetch && rtcLastPriceFetch == now) {
      logWithTimestamp("New price data fetched - updating display.");
      needDisplayUpdate = true;
    } else if (rtcRefreshCounter >= 4) {
      logWithTimestamp("Periodic refresh to prevent ghosting.");
      needDisplayUpdate = true;
      rtcRefreshCounter = 0;
    }

    if (needDisplayUpdate) {
      updateDisplay();
      rtcLastDisplayedRateStart = currentRateStart;
    } else {
      logWithTimestamp("Display unchanged - skipping update to save power.");
    }

    rtcRefreshCounter++;
  } else {
    logWithTimestamp("No rate data - skipping display update.");
  }

  // Calculate time until next quarter-hour boundary
  now = time(nullptr);
  if (now >= 1000000000) {
    time_t secondsIntoQuarter = now % WAKE_INTERVAL_S;
    time_t sleepSeconds       = WAKE_INTERVAL_S - secondsIntoQuarter + 5;  // +5s buffer

    // Pre-compensate sleep duration for RTC oscillator drift
    if (rtcDriftPerHour != 0.0) {
      double sleepHours = (double)sleepSeconds / 3600.0;
      long adjusted     = (long)sleepSeconds + (long)(rtcDriftPerHour * sleepHours + 0.5);
      if (adjusted < 60) adjusted = 60;
      sleepSeconds = (time_t)adjusted;
    }

    logWithTimestamp("Entering deep sleep mode.");
    enterDeepSleep(sleepSeconds);
  } else {
    logWithTimestamp("Time not set - sleeping for default interval.");
    enterDeepSleep(WAKE_INTERVAL_S);
  }
}

void loop() {
  // Not used — setup() handles everything and enters deep sleep.
  enterDeepSleep(WAKE_INTERVAL_S);
}
