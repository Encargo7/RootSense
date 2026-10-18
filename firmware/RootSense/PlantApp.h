#pragma once
// =============================================================================
//  App — ties the pieces together: reads the sensors, runs one
//  WateringController per plant, drives the pumps, and offers "actions" that
//  the web dashboard and the Serial Monitor both use.
// =============================================================================
#include <Arduino.h>

#include "PlantConfig.h"
#include "EventLog.h"
#include "PlantHardware.h"
#include "MoistureMath.h"
#include "PlantDatabase.h"
#include "PlantSettings.h"
#include "WateringController.h"

// Live data for one plant/zone.
struct ZoneRuntime {
  WateringController ctrl;
  PumpRequest request = {false, 0.0f, false};
  float rawMv = 0.0f;     // last trimmed sensor voltage
  float spreadMv = 0.0f;  // last sample spread
  float moisture = 0.0f;  // smoothed moisture, %
  bool moistureValid = false;
  moisture::SensorHealth health = moisture::SensorHealth::Ok;  // debounced
  moisture::SensorHealth pendingHealth = moisture::SensorHealth::Ok;
  uint8_t pendingCount = 0;
  bool firstReading = true;
  MoistureHistory history;
};

struct AppState {
  AppSettings cfg;
  ZoneRuntime zone[ZONE_COUNT];
  hw::PumpDriver pump;
  hw::TankMonitor tank;
  hw::Button button;
  EventLog events;
  uint32_t bootMs = 0;
  uint32_t lastSensorMs = 0;
  uint32_t lastHistoryMs = 0;
  bool sensorsReadOnce = false;
  bool lastTankHasWater = true;
  uint8_t nextPumpZone = 0;
  bool settingsDirty = false;
  uint32_t settingsDirtyMs = 0;
  bool restartPending = false;
  uint32_t restartAtMs = 0;
  int resetReason = 0;
};

extern AppState app;

// ---- called from RootSense.ino ----------------------------------------
void appSetup();
void updateInputs(uint32_t nowMs);    // button, tank switch, soil sensors
void updateWatering(uint32_t nowMs);  // each plant's controller decides
void updatePumps(uint32_t nowMs);     // start/stop pumps safely
void updateStatusLed(uint32_t nowMs);
void maybeRestart(uint32_t nowMs);

// ---- actions used by the web dashboard and the Serial Monitor --------------
// Zones are numbered from 0 here (the dashboard shows them as 1, 2, ...).
// Each returns true on success and puts a human-readable message in msg.
bool actionSetPlant(uint8_t z, const char* key, String& msg);
bool actionSetEnabled(uint8_t z, bool enabled, String& msg);
bool actionSetPot(uint8_t z, long potMl, String& msg);
bool actionSetAdvanced(uint8_t z, long soakMin, long cooldownMin, long dailyLimitMl, long maxPulses,
                       String& msg);
bool actionWaterNow(uint8_t z, long ml, String& msg);
bool actionPumpTest(uint8_t z, long seconds, String& msg);
bool actionSetFlow(uint8_t z, float measuredMl, float seconds, String& msg);
bool actionCalibrate(uint8_t z, bool dryPoint, String& msg);
bool actionClear(int z, String& msg);  // z = -1 clears every zone
bool actionStopPumps(String& msg);
bool actionSetQuietHours(bool enabled, long startHour, long endHour, String& msg);
bool actionSetWifi(const char* ssid, const char* password, String& msg);
bool actionFactoryReset(String& msg);
void actionRestart(uint32_t delayMs);

// ---- helpers ----------------------------------------------------------------
void logEvent(const char* fmt, ...);
bool quietHoursNow();
bool zoneSensorOk(uint8_t z);
// Plain-English status for a zone. level is set to "ok", "info", "water", "warn" or "bad".
String zoneStatusText(uint8_t z, const char*& level);
String formatEventTime(const EventLog::Entry& e);
