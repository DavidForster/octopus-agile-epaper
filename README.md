# Octopus Display

An ESP32-based e-paper display that shows today's Octopus Energy Agile electricity prices as a bar chart. The device wakes from deep sleep every 15 minutes to update the display, fetching fresh price data from the Octopus Energy API every 6 hours.

![Display showing a day of Agile electricity prices as a bar chart with cheap slots shown as white bars and expensive slots as black bars]

## Features

- Full-day bar chart of 30-minute Agile tariff price slots
- Bars above the median price filled black (expensive), below shown as white outlines (cheap)
- Current time slot highlighted with a vertical line
- Extremely low power consumption — WiFi and display are off most of the time
- RTC drift compensation — measures and corrects ESP32 clock drift between NTP syncs

## Hardware

| Component | Details |
|---|---|
| Microcontroller | AZ-Delivery ESP32 DevKit V4 (or compatible) |
| Display | Waveshare 2.9" e-paper, 296×128px (GDEH029A1 / IL3820) |

### Wiring (SPI)

| ESP32 Pin | Display Pin |
|---|---|
| GPIO 5 | CS |
| GPIO 17 | DC |
| GPIO 16 | RST |
| GPIO 4 | BUSY |
| GPIO 23 (MOSI) | DIN |
| GPIO 18 (SCK) | CLK |
| 3.3V | VCC |
| GND | GND |

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VSCode extension)
- Octopus Energy account on the Agile tariff

### Configuration

Copy the credentials template and fill in your details:

```bash
cp src/credentials.h.example src/credentials.h
```

Edit `src/credentials.h`:

```cpp
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

#define OCTOPUS_PRODUCT_CODE "AGILE-24-10-01"  // Check Octopus API for current product code
#define OCTOPUS_REGION_CODE "E"                 // Your region — see below
```

**Region codes:** A=Eastern, B=East Midlands, C=London, D=Merseyside & N Wales, E=West Midlands, F=North East, G=North Scotland, H=North West, J=South East, K=South Scotland, L=South Wales, M=South West, N=Southern, P=Yorkshire.

The current Agile product code can be found via the [Octopus Energy API docs](https://docs.octopus.energy/rest/guides/endpoints/#api-price-endpoints).

### Build & Flash

```bash
# Build and upload
pio run -t upload

# Monitor serial output (useful for debugging)
pio device monitor

# Build the debug variant (shows current time on screen)
pio run -e debug -t upload
```

## How It Works

The device spends almost all its time in deep sleep. On each wake (every 15 minutes, aligned to clock boundaries):

1. **Checks if WiFi is needed** — required for first boot, every 6 hours for price data, or if the clock isn't set
2. **Syncs time** — via NTP (`pool.ntp.org`), falling back to the WorldTimeAPI HTTP endpoint
3. **Fetches prices** — from `api.octopus.energy` if the 6-hour interval has elapsed
4. **Updates the display** — only if the current 30-minute rate slot has changed, new data arrived, or ~1 hour has passed (periodic anti-ghosting refresh)
5. **Sleeps** — until the next 15-minute boundary

Price data and RTC state are stored in ESP32 RTC memory, which survives deep sleep.

## Libraries

- [GxEPD2](https://github.com/ZinggJM/GxEPD2) — e-paper display driver
- [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library) — graphics primitives
- [ArduinoJson](https://arduinojson.org/) — JSON parsing

The project includes a custom e-paper driver (`lib/GxEPD2_290_Custom/`) that fixes a busy-wait timeout issue specific to this display hardware by changing a single update command byte (`0xc7` → `0xc4`).
