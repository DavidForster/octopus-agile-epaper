#pragma once

// Fetch current Agile prices from the Octopus Energy API into fetchedRates[].
// Returns true on success. Requires WiFi to be connected and time to be set.
bool fetchCurrentPrice();
