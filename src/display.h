#pragma once
#include <GxEPD2_BW.h>
#include "GxEPD2_290_Custom.h"

// Pin definitions for ESP32 SPI connection to e-paper display
#define EPD_CS    5
#define EPD_DC    17
#define EPD_RST   16
#define EPD_BUSY  4

// Display object (defined in display.cpp, used in main.cpp for init/rotation)
extern GxEPD2_BW<GxEPD2_290_Custom, GxEPD2_290_Custom::HEIGHT> display;

// Update currentRateIndex to reflect the current time slot within rates[]
void updateCurrentRateIndexFromNow();

// Select which EXPECTED_SLOTS-wide window of fetchedRates[] to copy into rates[]
void selectDisplayWindow();

// Perform a full e-ink refresh with the current price graph
void updateDisplay();
