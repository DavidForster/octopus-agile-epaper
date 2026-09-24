# TODO

Findings from a manual code review (2026-08-20). Ranked by severity.

## 1. WiFi-failure path bypasses the display throttle (bug, meaningful)
`src/main.cpp:99-108` — when WiFi fails to connect, the code unconditionally calls
`updateDisplay()` if there's cached rate data:
```cpp
if (fetchedRateCount > 0) {
  selectDisplayWindow();
  updateDisplay();
}
enterDeepSleep(WAKE_INTERVAL_S);
```
This skips the `rtcLastDisplayedRateStart` check that the normal path uses to avoid
needless refreshes. If WiFi is down for an extended period (router outage, moved out
of range, etc.), the device will do a full e-paper refresh every 15 minutes
indefinitely — exactly the kind of panel wear/ghosting-risk behavior the rest of the
code goes out of its way to avoid. Should reuse the same "did the slot change" gate
before refreshing.

## 2. `rtcFetchedAfter4pm` reset can be skipped, delaying next-day price fetch (bug, subtle)
`src/main.cpp:66-78`:
```cpp
if (!needPriceFetch && now >= 1000000000) {
  ...
  if (localNow.tm_hour == 16 && !rtcFetchedAfter4pm) needPriceFetch = true;
  if (localNow.tm_hour < 16) rtcFetchedAfter4pm = false;
}
```
The daily reset of `rtcFetchedAfter4pm` only runs when `needPriceFetch` is already
false. But on most days, the regular 6-hour interval fetch (`PRICE_FETCH_INTERVAL_S`)
is likely to land at some hour < 16 anyway, which sets `needPriceFetch = true` for
that boot — skipping this whole block, so the flag never gets reset. If yesterday's
flag was left `true`, today's smart-fetch trigger at 16:00
(`tm_hour == 16 && !rtcFetchedAfter4pm`) silently no-ops, and next-day prices only
show up whenever the next regular 6-hour fetch happens to land (up to several hours
late). Worth decoupling the reset from `needPriceFetch`.

## 3. Dead code / duplicated logic (cleanup)
`updateCurrentRateIndexFromNow()` (`display.cpp:193`, declared in `display.h:15`) is
never called anywhere. `selectDisplayWindow()` has its own near-identical inline loop
for finding the current slot. Either wire the function in or delete it — right now
it's just confusing, unused duplication.

## 4. Y-axis label overflow fix is incomplete (minor)
`display.cpp:77-82` drops the "p" suffix once if the label would overflow, but
doesn't recheck after dropping it. A 3-digit magnitude label (e.g. `-100`, 4 chars)
can still overflow by a couple pixels since the fallback isn't re-validated. Given
the git history shows this exact overflow was already fixed twice (`9956e9e`,
`5aa0d93`), this residual edge case is worth closing off for good — e.g.
loop/truncate until it fits, rather than a single fallback.

## Battery life

Ranked by expected impact.

### 5. Hardware: onboard devkit power draw likely dominates deep sleep (biggest lever)
Deep sleep on a bare ESP32 chip is ~10µA, but generic devkits like the
AZ-Delivery DevKit V4 rarely get close to that in practice because of onboard
extras that stay powered: the USB-UART bridge chip (CP2102/CH340, often 1+ mA
continuously), the power/status LED if wired straight to 3.3V, and the onboard
3.3V regulator (commonly AMS1117-class, can have several mA of quiescent
current at no load). Not fixable in firmware — measure actual deep-sleep
current with a multimeter first; if it's in the mA range rather than µA, this
is where the battery is really going. Fix is hardware (cut the LED trace,
bypass the onboard regulator with a low-IQ one, or feed 3.3V directly).

### 6. `display.init()` runs on every wake, even when nothing will be drawn
`src/main.cpp:58-59` calls `display.init()`/`setRotation()` unconditionally at
the top of `setup()`, before it's known whether `updateDisplay()` will
actually run this cycle. On most 15-minute wakes (no rate-slot change, no
WiFi needed), this SPI/reset init work happens for nothing. Move it to just
before the `updateDisplay()` call(s) — inside the `needDisplayUpdate` branch
and the WiFi-failure fallback branch — to skip it on quiet wakes.

### 7. Wake interval vs. rate-slot granularity
`WAKE_INTERVAL_S` is 900s (15 min), but Octopus rate slots
(`RATE_SLOT_DURATION`) are 30 min — the device wakes twice per price slot for
no display-relevant reason on the "off" wake. Bumping the wake interval to
1800s would halve total wakes/day (96 → 48), each of which carries baseline
overhead (RTC boot, clock check, serial init) even when it does nothing else.
Trade-off: the "current slot" marker on the graph would only ever update on
the half-hour, and drift-correction/anti-ghosting timing would need
re-tuning since they're wake-count-based (`rtcRefreshCounter`, boot 2/5
calibration).

### 8. NTP wait timeout is generous
`waitForNtpSync()` (`time_utils.cpp:58-71`) polls for up to 20 seconds before
falling back to the HTTP time API, during which WiFi stays associated and the
radio stays active. Only bites on WiFi-needed wakes, so a smaller win, but
shortening the timeout (e.g. 10s) would trim active-radio time on those
wakes.
