#pragma once
#include <Arduino.h>

// Octopus API configuration (can be overridden in credentials.h or platformio.ini)
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

// UK local time with automatic DST handling (GMT in winter, BST in summer)
const char* const LOCAL_TIMEZONE = "GMT0BST,M3.5.0/1,M10.5.0/2";

// Time constants (seconds)
const time_t SECONDS_PER_DAY        = 86400;
const time_t RATE_SLOT_DURATION     = 1800;   // 30 minutes
const time_t WAKE_INTERVAL_S        = 900;    // Wake every 15 minutes
const time_t PRICE_FETCH_INTERVAL_S = 6 * 60 * 60;  // Fetch prices every 6 hours

// Buffer sizes
const int MAX_RATES       = 60;   // Current display window
const int FETCH_MAX_RATES = 100;  // Full 48-hr fetch buffer (~96 slots)
const int MIN_PAST_SLOTS  = 6;    // Minimum past slots in display window (3 hrs)

// Graph display constants
const int    EXPECTED_SLOTS        = 46;   // Display window width in 30-min slots (23 hrs)
const int    BAR_WIDTH             = 5;    // Bar width in pixels (odd number for centering)
const int    BAR_GAP               = 1;    // Total gap per slot
const int    SLOT_WIDTH            = BAR_WIDTH + BAR_GAP;
const int    GRAPH_WIDTH           = EXPECTED_SLOTS * SLOT_WIDTH;
const double PRICE_GRID_INTERVAL   = 5.0;  // Grid line every 5 pence
const int    HOUR_LABEL_INTERVAL   = 2;    // Show hour label every 2 hours

// Graph layout (pixels)
const int GRAPH_X                  = 0;
const int GRAPH_Y                  = 5;
const int GRAPH_HEIGHT             = 110;
const int Y_LABEL_OFFSET           = 2;    // Gap between graph and price labels (right side)
const int Y_LABEL_VERTICAL_OFFSET  = -3;   // Vertical adjustment for price labels
const int HOUR_LABEL_CENTER_OFFSET = 1;    // Fine-tune horizontal centering of hour labels
const int HOUR_LABEL_Y_OFFSET      = 5;    // Gap between graph and hour labels (bottom)

// Rate data structure
struct RateData {
  time_t validFrom;
  double price;
};

// RTC-persisted state — defined in main.cpp, accessible everywhere via extern.
// RTC_DATA_ATTR is only needed on the definition; the extern declarations below
// just tell the compiler these symbols exist (the linker resolves them to RTC memory).
extern RTC_DATA_ATTR time_t   rtcLastPriceFetch;
extern RTC_DATA_ATTR time_t   rtcLastTimeSync;
extern RTC_DATA_ATTR int      rtcBootCount;
extern RTC_DATA_ATTR time_t   rtcLastDisplayedRateStart;
extern RTC_DATA_ATTR int      rtcRefreshCounter;
extern RTC_DATA_ATTR double   rtcDriftPerHour;
extern RTC_DATA_ATTR double   rtcAccumulatedDrift;
extern RTC_DATA_ATTR time_t   rtcLastCorrectionTime;
extern RTC_DATA_ATTR bool     rtcFetchedAfter4pm;
extern RTC_DATA_ATTR RateData fetchedRates[];
extern RTC_DATA_ATTR int      fetchedRateCount;
extern RTC_DATA_ATTR RateData rates[];
extern RTC_DATA_ATTR int      rateCount;
extern RTC_DATA_ATTR int      currentRateIndex;
extern RTC_DATA_ATTR time_t   graphStartTime;
