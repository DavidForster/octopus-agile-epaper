#pragma once
#include <time.h>

// Logging
void logWithTimestamp(const char* message);

// Drift compensation
void applyDriftCorrection();

// Time synchronisation
bool waitForNtpSync();
bool syncTimeFromHttp();

// Time conversion helpers
bool   timeToUtcStruct(time_t timestamp, struct tm& out);
bool   timeToLocalStruct(time_t timestamp, struct tm& out);
time_t utcStructToEpoch(const struct tm& utcTime);
void   configureLocalTimezone();
time_t parseISOTimestamp(const char* isoTime);
