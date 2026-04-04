#include "display.h"
#include "globals.h"
#include "time_utils.h"
#include <Arduino.h>
#include <time.h>
#include <math.h>

// Display object (296x128, 2.9" Waveshare) with custom driver
GxEPD2_BW<GxEPD2_290_Custom, GxEPD2_290_Custom::HEIGHT> display(
    GxEPD2_290_Custom(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---- Internal types ----

struct PriceStats {
  double minPrice;
  double maxPrice;
  double medianPrice;
};

// ---- Internal helpers ----

static PriceStats calculatePriceStats() {
  PriceStats stats;

  stats.minPrice = rates[0].price;
  stats.maxPrice = rates[0].price;
  for (int i = 1; i < rateCount; i++) {
    if (rates[i].price < stats.minPrice) stats.minPrice = rates[i].price;
    if (rates[i].price > stats.maxPrice) stats.maxPrice = rates[i].price;
  }

  // Round to nice values for axis (multiples of 5)
  stats.minPrice = floor(stats.minPrice / PRICE_GRID_INTERVAL) * PRICE_GRID_INTERVAL;
  stats.maxPrice = ceil(stats.maxPrice  / PRICE_GRID_INTERVAL) * PRICE_GRID_INTERVAL;
  if (stats.maxPrice - stats.minPrice < 10) {
    stats.maxPrice = stats.minPrice + 10;
  }

  // Calculate median price for bar colour threshold
  double sortedPrices[MAX_RATES];
  for (int i = 0; i < rateCount; i++) sortedPrices[i] = rates[i].price;
  for (int i = 0; i < rateCount - 1; i++) {
    for (int j = i + 1; j < rateCount; j++) {
      if (sortedPrices[i] > sortedPrices[j]) {
        double tmp = sortedPrices[i];
        sortedPrices[i] = sortedPrices[j];
        sortedPrices[j] = tmp;
      }
    }
  }
  stats.medianPrice = sortedPrices[rateCount / 2];

  return stats;
}

static void drawGridLinesAndLabels(int x, int y, int width, int height,
                                   double minPrice, double maxPrice, double priceRange) {
  display.setFont();
  for (double price = minPrice; price <= maxPrice; price += PRICE_GRID_INTERVAL) {
    int gridY = y + height - (int)((price - minPrice) / priceRange * height);

    // Solid line at zero when range spans negative; otherwise solid at the bottom (minPrice)
    bool isSolid = (minPrice < 0) ? (fabs(price) < 0.001) : (price == minPrice);
    if (isSolid) {
      // Solid baseline
      display.drawLine(x, gridY, x + width, gridY, GxEPD_BLACK);
    } else {
      // Dashed horizontal grid line
      const int dashLength = 1;
      const int gapLength  = 2;
      for (int xx = x; xx <= x + width; xx += dashLength + gapLength) {
        int xEnd = min(xx + dashLength, x + width);
        display.drawLine(xx, gridY, xEnd, gridY, GxEPD_BLACK);
      }
    }

    // Y-axis label on the right
    char label[10];
    snprintf(label, sizeof(label), "%.0fp", price);
    display.setCursor(x + width + Y_LABEL_OFFSET, gridY + Y_LABEL_VERTICAL_OFFSET);
    display.print(label);
  }
}

static void drawPriceBars(int x, int y, int width, int height,
                          double minPrice, double priceRange, double medianPrice) {
  // Zero line Y position — clamped to chart bounds (handles all-positive case)
  int zeroY = constrain(y + height - (int)((0.0 - minPrice) / priceRange * height), y, y + height);

  for (int i = 0; i < rateCount; i++) {
    int    slotStartX = x + (i * SLOT_WIDTH);
    int    barX       = constrain(slotStartX + (BAR_GAP / 2), x, x + width);
    double price      = rates[i].price;
    bool   isExpensive = (price > medianPrice);

    if (price >= 0) {
      int barHeight = constrain((int)(price / priceRange * height), 0, height);
      if (barHeight == 0) continue;
      int barY = zeroY - barHeight;

      display.fillRect(barX, barY, BAR_WIDTH, barHeight, isExpensive ? GxEPD_BLACK : GxEPD_WHITE);
      // Three-sided border: top + sides (no bottom — bar sits on zero line)
      display.drawLine(barX,                 barY,             barX,                 barY + barHeight - 1, GxEPD_BLACK);
      display.drawLine(barX + BAR_WIDTH - 1, barY,             barX + BAR_WIDTH - 1, barY + barHeight - 1, GxEPD_BLACK);
      display.drawLine(barX,                 barY,             barX + BAR_WIDTH - 1, barY,                 GxEPD_BLACK);
    } else {
      int barHeight = constrain((int)((-price) / priceRange * height), 0, height);
      if (barHeight == 0) continue;
      int barY = zeroY;

      // Negative prices are always cheap — white with border
      display.fillRect(barX, barY, BAR_WIDTH, barHeight, GxEPD_WHITE);
      // Four-sided border — top restores the zero line overwritten by fillRect
      display.drawLine(barX,                 barY,             barX,                 barY + barHeight - 1, GxEPD_BLACK);
      display.drawLine(barX + BAR_WIDTH - 1, barY,             barX + BAR_WIDTH - 1, barY + barHeight - 1, GxEPD_BLACK);
      display.drawLine(barX,                 barY,             barX + BAR_WIDTH - 1, barY,                 GxEPD_BLACK);
      display.drawLine(barX,                 barY + barHeight - 1, barX + BAR_WIDTH - 1, barY + barHeight - 1, GxEPD_BLACK);
    }
  }
}

static void drawTimeLabels(int x, int y, int width, int height) {
  struct tm timeInfo;
  for (int i = 0; i < rateCount; i++) {
    if (timeToLocalStruct(rates[i].validFrom, timeInfo)) {
      if (timeInfo.tm_min == 0 && timeInfo.tm_hour % HOUR_LABEL_INTERVAL == 0) {
        int slotStartX  = x + (i * SLOT_WIDTH);
        int barX        = slotStartX + (BAR_GAP / 2);
        int barCenterX  = barX + (BAR_WIDTH / 2);

        // Alignment notch
        display.drawLine(barCenterX, y + height, barCenterX, y + height + 2, GxEPD_BLACK);

        char timeLabel[4];
        snprintf(timeLabel, sizeof(timeLabel), "%d", timeInfo.tm_hour);

        int textWidth  = strlen(timeLabel) * 6;  // default font is 6px per char
        int centeredX  = barCenterX - (textWidth / 2) + HOUR_LABEL_CENTER_OFFSET;
        display.setCursor(centeredX, y + height + HOUR_LABEL_Y_OFFSET);
        display.print(timeLabel);
      }
    }
  }
}

static void drawNegativeZoneShading(int x, int zeroY, int width, int bottom) {
  // Sparse dot pattern (every 3px) to shade the below-zero region
  for (int py = zeroY + 1; py <= bottom; py += 3) {
    for (int px = x; px <= x + width; px += 3) {
      display.drawPixel(px, py, GxEPD_BLACK);
    }
  }
}

static void drawCurrentTimeSlot(int x, int y, int width, int height) {
  if (currentRateIndex >= 0 && currentRateIndex < rateCount) {
    int slotStartX = x + (currentRateIndex * SLOT_WIDTH);
    int barX       = slotStartX + (BAR_GAP / 2);
    int currentX   = barX + (BAR_WIDTH / 2);

    // Solid vertical line centred in the current bar
    display.drawLine(currentX, y, currentX, y + height, GxEPD_BLACK);
  }
}

static void drawPriceGraph(int x, int y, int width, int height) {
  if (rateCount < 2 || graphStartTime == 0) return;

  PriceStats stats  = calculatePriceStats();
  double priceRange = stats.maxPrice - stats.minPrice;

  if (stats.minPrice < 0) {
    int zeroY = constrain(y + height - (int)((0.0 - stats.minPrice) / priceRange * height), y, y + height);
    drawNegativeZoneShading(x, zeroY, width, y + height);
  }
  drawGridLinesAndLabels(x, y, width, height, stats.minPrice, stats.maxPrice, priceRange);
  drawCurrentTimeSlot(x, y, width, height);
  drawPriceBars(x, y, width, height, stats.minPrice, priceRange, stats.medianPrice);
  drawTimeLabels(x, y, width, height);
}

// ---- Public interface ----

void updateCurrentRateIndexFromNow() {
  if (rateCount <= 0) return;

  time_t now = time(nullptr);
  if (now < 1000000000) return;

  currentRateIndex = -1;
  for (int i = 0; i < rateCount; i++) {
    time_t slotStart = rates[i].validFrom;
    time_t slotEnd   = (i < rateCount - 1)
        ? rates[i + 1].validFrom
        : rates[i].validFrom + RATE_SLOT_DURATION;

    if (now >= slotStart && now < slotEnd) {
      currentRateIndex = i;
      break;
    }
  }
}

void selectDisplayWindow() {
  if (fetchedRateCount <= 0) return;

  time_t now = time(nullptr);
  if (now < 1000000000) return;

  // Find the slot we're currently in
  int currentFetchedIndex = -1;
  for (int i = 0; i < fetchedRateCount; i++) {
    time_t slotEnd = (i < fetchedRateCount - 1)
        ? fetchedRates[i + 1].validFrom
        : fetchedRates[i].validFrom + RATE_SLOT_DURATION;
    if (now >= fetchedRates[i].validFrom && now < slotEnd) {
      currentFetchedIndex = i;
      break;
    }
  }

  int start;
  if (currentFetchedIndex < 0) {
    // Current time outside fetched range — show from the start
    start = 0;
  } else {
    // Ideal: MIN_PAST_SLOTS before current slot.
    // Slide back only if there isn't enough future data to fill the window.
    int desiredStart = currentFetchedIndex - MIN_PAST_SLOTS;
    int latestStart  = fetchedRateCount - EXPECTED_SLOTS;
    start = max(0, min(desiredStart, latestStart));
  }

  int count = min(EXPECTED_SLOTS, fetchedRateCount - start);
  for (int i = 0; i < count; i++) {
    rates[i] = fetchedRates[start + i];
  }
  rateCount      = count;
  graphStartTime = (rateCount > 0) ? rates[0].validFrom : 0;
  currentRateIndex = (currentFetchedIndex >= 0) ? (currentFetchedIndex - start) : -1;

  Serial.print("Display window: slots ");
  Serial.print(start);
  Serial.print(" to ");
  Serial.print(start + count - 1);
  Serial.print(", current slot index in window: ");
  Serial.println(currentRateIndex);
}

void updateDisplay() {
  logWithTimestamp("Display update start.");

  // Always do a full refresh — prevents ghosting and stuck pixels.
  // Takes ~1.6s but this function is only called on slot changes or ~hourly.
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    if (rateCount > 0) {
      logWithTimestamp("Graph render start.");
      drawPriceGraph(GRAPH_X, GRAPH_Y, GRAPH_WIDTH, GRAPH_HEIGHT);
      logWithTimestamp("Graph render complete.");
    }

#ifdef DEBUG_DISPLAY
    {
      time_t debugNow = time(nullptr);
      struct tm debugInfo;
      if (debugNow >= 1000000000 && timeToLocalStruct(debugNow, debugInfo)) {
        char debugTime[9];
        strftime(debugTime, sizeof(debugTime), "%H:%M:%S", &debugInfo);
        display.setFont();
        display.setCursor(0, 0);
        display.print(debugTime);
      }
    }
#endif

  } while (display.nextPage());

  logWithTimestamp("Display update complete.");
  display.hibernate();
}
