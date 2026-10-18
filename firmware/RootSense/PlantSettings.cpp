#include "PlantSettings.h"

#include <Preferences.h>
#include <string.h>

#include "PlantDatabase.h"
#include "WateringController.h"

static const uint32_t kSettingsMagic = 0x504C4E54UL;  // "PLNT"
static const uint16_t kSettingsVersion = 1;
static const char* kNvsNamespace = "rootsense";
static const char* kNvsKey = "cfg";

template <typename T>
static T clampValue(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void zoneDefaults(ZoneConfig& z, uint8_t index) {
  memset(&z, 0, sizeof(z));
  z.enabled = (index == 0) ? 1 : 0;  // only zone 1 starts switched on
  strncpy(z.plantKey, "generic", sizeof(z.plantKey) - 1);
  z.potMl = 1500;            // a 15 cm (6 inch) pot
  z.dryMv = 2600;            // typical capacitive sensor in air at 3.3 V
  z.wetMv = 1200;            // typical value in wet soil
  z.flowMlPerSec = 25.0f;    // deliberately on the high side -> less water until measured
  z.mlPerPercent = baselineMlPerPercent(z.potMl);
  z.soakMin = 10;
  z.maxPulses = 5;
  z.cooldownMin = 60;
  z.dailyLimitMl = 600;
}

void settingsDefaults(AppSettings& s) {
  memset(&s, 0, sizeof(s));
  s.magic = kSettingsMagic;
  s.version = kSettingsVersion;
  s.size = sizeof(AppSettings);
  for (uint8_t i = 0; i < 4; i++) zoneDefaults(s.zones[i], i);
  s.quietEnabled = 0;
  s.quietStartHour = 22;
  s.quietEndHour = 7;
}

void settingsSanitize(AppSettings& s) {
  for (uint8_t i = 0; i < 4; i++) {
    ZoneConfig& z = s.zones[i];
    z.enabled = z.enabled ? 1 : 0;
    z.plantKey[sizeof(z.plantKey) - 1] = '\0';
    if (plants::find(z.plantKey) == nullptr) strncpy(z.plantKey, "generic", sizeof(z.plantKey) - 1);
    z.potMl = clampValue<uint16_t>(z.potMl, 100, 20000);
    z.dryMv = clampValue<uint16_t>(z.dryMv, 500, 3300);
    z.wetMv = clampValue<uint16_t>(z.wetMv, 100, 3200);
    if (z.wetMv + 200 > z.dryMv) {  // impossible calibration -> back to defaults
      z.dryMv = 2600;
      z.wetMv = 1200;
      z.dryCalibrated = 0;
      z.wetCalibrated = 0;
    }
    if (!(z.flowMlPerSec >= 1.0f && z.flowMlPerSec <= 200.0f)) z.flowMlPerSec = 25.0f;  // also catches NaN
    const float base = baselineMlPerPercent(z.potMl);
    if (!(z.mlPerPercent >= base * 0.2f && z.mlPerPercent <= base * 5.0f)) z.mlPerPercent = base;
    z.soakMin = clampValue<uint8_t>(z.soakMin, 2, 120);
    z.maxPulses = clampValue<uint8_t>(z.maxPulses, 1, 10);
    z.cooldownMin = clampValue<uint16_t>(z.cooldownMin, 10, 1440);
    z.dailyLimitMl = clampValue<uint16_t>(z.dailyLimitMl, 20, 5000);
  }
  s.wifiSsid[sizeof(s.wifiSsid) - 1] = '\0';
  s.wifiPass[sizeof(s.wifiPass) - 1] = '\0';
  s.quietEnabled = s.quietEnabled ? 1 : 0;
  if (s.quietStartHour > 23) s.quietStartHour = 22;
  if (s.quietEndHour > 23) s.quietEndHour = 7;
}

bool settingsLoad(AppSettings& s) {
  settingsDefaults(s);
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) return false;  // first boot: namespace not created yet
  bool ok = false;
  if (prefs.isKey(kNvsKey) && prefs.getBytesLength(kNvsKey) == sizeof(AppSettings)) {
    AppSettings loaded;
    prefs.getBytes(kNvsKey, &loaded, sizeof(loaded));
    if (loaded.magic == kSettingsMagic && loaded.version == kSettingsVersion &&
        loaded.size == sizeof(AppSettings)) {
      s = loaded;
      ok = true;
    }
  }
  prefs.end();
  settingsSanitize(s);
  return ok;
}

bool settingsSave(const AppSettings& s) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) return false;
  size_t written = prefs.putBytes(kNvsKey, &s, sizeof(s));
  prefs.end();
  return written == sizeof(s);
}

void settingsErase() {
  Preferences prefs;
  if (prefs.begin(kNvsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}
