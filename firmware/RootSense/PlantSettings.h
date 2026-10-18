#pragma once
// =============================================================================
//  Settings — everything you change from the dashboard / Serial Monitor,
//  saved in the ESP32's flash (NVS) so it survives power cuts and re-uploads.
// =============================================================================
#include <stdint.h>

#include "PlantConfig.h"

struct ZoneConfig {
  uint8_t enabled;
  char plantKey[16];      // which PlantProfile (see PlantDatabase.cpp)
  uint16_t potMl;         // soil volume of the pot
  uint16_t dryMv;         // sensor voltage in dry air        (0 %)
  uint16_t wetMv;         // sensor voltage in watered soil   (100 %)
  uint8_t dryCalibrated;  // 1 once the user saved a real DRY reading
  uint8_t wetCalibrated;  // 1 once the user saved a real WET reading
  float flowMlPerSec;     // measured pump flow
  float mlPerPercent;     // learned by the controller
  uint8_t soakMin;        // wait after each pulse
  uint8_t maxPulses;      // pulses per watering cycle
  uint16_t cooldownMin;   // rest after a finished cycle
  uint16_t dailyLimitMl;  // max water per 24 h
};

struct AppSettings {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  ZoneConfig zones[4];
  char wifiSsid[33];
  char wifiPass[65];
  uint8_t quietEnabled;    // don't START watering during quiet hours
  uint8_t quietStartHour;  // 0..23
  uint8_t quietEndHour;    // 0..23
};

void zoneDefaults(ZoneConfig& z, uint8_t index);
void settingsDefaults(AppSettings& s);
void settingsSanitize(AppSettings& s);  // fixes out-of-range values
bool settingsLoad(AppSettings& s);      // false -> nothing saved yet, defaults used
bool settingsSave(const AppSettings& s);
void settingsErase();
