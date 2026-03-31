# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

An ESP32-based e-paper display that shows Octopus Energy Agile tariff electricity prices as a bar chart. The device wakes from deep sleep every 15 minutes, fetches prices from the Octopus Energy API every 6 hours, and displays a full-day price graph on a 2.9" Waveshare e-paper screen (296×128px).

## Build & Flash Commands

This is a PlatformIO project. Use the PlatformIO CLI (`pio`) or the VSCode PlatformIO extension.

```bash
# Build (default environment)
pio run

# Build debug variant (enables DEBUG_DISPLAY flag — shows current time on screen)
pio run -e debug

# Upload to connected ESP32
pio run -t upload

# Upload debug build
pio run -e debug -t upload

# Monitor serial output (115200 baud)
pio device monitor

# Build + upload + monitor in one step
pio run -t upload && pio device monitor
```

There are no automated tests — the `test/` directory contains only the PlatformIO placeholder README.

## Setup

Before building, copy `src/credentials.h.example` to `src/credentials.h` and fill in:
- WiFi SSID and password
- Octopus Energy product code and region code (defaults to `AGILE-24-10-01`, region `E` / West Midlands)

`src/credentials.h` is git-ignored. Never commit it.

## Architecture

**Single-file firmware** — all logic lives in `src/main.cpp`. There is no RTOS, no task system; everything runs in `setup()` and the device enters deep sleep before `loop()` is ever called.

### Deep Sleep / Wake Cycle

The core execution model is: wake → do work → sleep. RTC memory (`RTC_DATA_ATTR`) persists all state across sleep cycles:
- `rates[]` / `rateCount` — cached price data (up to 60 slots)
- `rtcLastPriceFetch` — timestamp of last API call (throttled to every 6 hours)
- `rtcBootCount` — used to schedule early drift calibration syncs (boots 2, 5)
- `rtcDriftPerHour` / `rtcLastCorrectionTime` / `rtcAccumulatedDrift` — RTC drift compensation values (including sub-second accumulator)
- `rtcFetchedAfter4pm` — tracks whether prices have been fetched during today's 16:00 window
- `rtcLastDisplayedRateIndex` / `rtcRefreshCounter` — display update throttling

The device sleeps until the next 15-minute boundary (`:00`, `:15`, `:30`, `:45`) plus a 5-second buffer. The sleep duration is pre-compensated for measured RTC oscillator drift to prevent progressive lag.

### WiFi & Time Sync Strategy

WiFi is only enabled when needed:
1. First boot or no rate data
2. 6-hour price fetch interval elapsed
3. Smart fetch at 16:00 local time to capture next-day prices (Octopus publishes ~16:00)
4. Early drift calibration syncs (boots 2, 5 to rapidly characterise RTC drift)
5. Time not set

When WiFi is active, time is always re-synced (NTP via `pool.ntp.org`, fallback to `worldtimeapi.org` HTTP API). After NTP sync, the measured drift between the previous RTC time and NTP time is recorded and smoothed (50/50 EWMA) into `rtcDriftPerHour`. Between syncs, `applyDriftCorrection()` adjusts `settimeofday()` based on elapsed time × drift rate, with a sub-second accumulator to prevent truncation losses.

All timestamps are UTC internally. The Octopus API returns ISO 8601 UTC strings; `parseISOTimestamp()` converts these to `time_t`.

### Display Update Logic

`updateDisplay()` always does a full refresh (no partial update) — this prevents ghosting. It's skipped when the current 30-minute rate slot hasn't changed, unless:
- New price data was just fetched, or
- `rtcRefreshCounter` reaches 4 (~1 hour) for mandatory anti-ghosting refresh

The graph draws 46 bars (00:00–23:00), each 6px wide (5px bar + 1px gap). Bars above the median price are filled black (expensive), below are white with black border (cheap). Y-axis labels appear on the right; hour labels appear at the bottom at 2-hour intervals.

### Custom e-Paper Driver

`lib/GxEPD2_290_Custom/` contains a copy of the GxEPD2 `GxEPD2_290` driver with one change: the `_Update_Full()` command byte is `0xc4` instead of `0xc7`. The standard value causes busy-wait timeouts on this specific hardware (GDEH029A1 / IL3820). This cannot be fixed by subclassing because `_Update_Full()` is private and non-virtual.

### Hardware

- Board: AZ-Delivery ESP32 DevKit V4 (`az-delivery-devkit-v4`)
- Display: Waveshare 2.9" e-paper, 296×128px, connected via SPI
  - CS=5, DC=17, RST=16, BUSY=4
- Display is landscape (rotated 90°): logical width=296, height=128

### Key Constants (all in `main.cpp`)

| Constant | Value | Purpose |
|---|---|---|
| `WAKE_INTERVAL_S` | 900s | Deep sleep wake period |
| `PRICE_FETCH_INTERVAL_S` | 21600s | API poll interval |
| `RATE_SLOT_DURATION` | 1800s | Octopus slot length |
| `EXPECTED_SLOTS` | 46 | Bars on graph (00:00–23:00) |
| `BAR_WIDTH` / `BAR_GAP` | 5 / 1 | Graph bar sizing |
| `MAX_RATES` | 60 | RTC rate buffer size |
