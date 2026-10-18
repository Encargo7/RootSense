#include "PlantApp.h"

#include <esp_system.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "PlantNetwork.h"
#include "SerialConsole.h"

AppState app;

// These two survive a reset (but not a power cut). If the board resets while a
// pump is running - usually a power-supply dip - the next boot finds out and
// stops that zone instead of starting the pump again and again.
RTC_NOINIT_ATTR uint32_t g_rtcMagic;
RTC_NOINIT_ATTR int32_t g_rtcPumpZone;
static const uint32_t kRtcMagic = 0x50554D50UL;  // "PUMP"

static const float MOISTURE_SMOOTHING = 0.25f;   // 0..1, lower = smoother
static const uint8_t HEALTH_CONFIRM_READINGS = 3;

// ================================================================ logging ===
String formatEventTime(const EventLog::Entry& e) {
  char buf[24];
  if (e.epoch > 0) {
    time_t t = (time_t)e.epoch;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%b %d %H:%M", &tmv);
  } else {
    const unsigned long s = e.uptimeMs / 1000UL;
    snprintf(buf, sizeof(buf), "+%luh%02lum%02lus", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
  }
  return String(buf);
}

void logEvent(const char* fmt, ...) {
  char buf[EventLog::TEXT_LEN];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  const int64_t epoch = net::timeValid() ? (int64_t)time(nullptr) : 0;
  app.events.add(millis(), epoch, buf);
  Serial.print('[');
  Serial.print(formatEventTime(app.events.get(app.events.size() - 1)));
  Serial.print("] ");
  Serial.println(buf);
}

static void controllerLog(void* ctx, uint8_t zone, const char* message) {
  (void)ctx;
  logEvent("Zone %u: %s", (unsigned)(zone + 1), message);
}

// =============================================================== settings ===
static ZoneParams paramsFor(const ZoneConfig& c) {
  const PlantProfile* plant = plants::find(c.plantKey);
  if (plant == nullptr) plant = &plants::fallback();
  ZoneParams p = defaultZoneParams(plant, (float)c.potMl);
  p.mlPerPercent = c.mlPerPercent;
  p.dailyLimitMl = (float)c.dailyLimitMl;
  p.maxPulses = c.maxPulses;
  p.soakMs = (uint32_t)c.soakMin * 60000UL;
  p.cooldownMs = (uint32_t)c.cooldownMin * 60000UL;
  return p;
}

static void applyZone(uint8_t z) { app.zone[z].ctrl.setParams(paramsFor(app.cfg.zones[z])); }

static bool saveNow() {
  app.settingsDirty = false;
  const bool ok = settingsSave(app.cfg);
  if (!ok) logEvent("WARNING: could not save the settings to flash.");
  return ok;
}

static void saveSoon(uint32_t nowMs) {
  app.settingsDirty = true;
  app.settingsDirtyMs = nowMs;
}

static const char* resetReasonText(int r) {
  switch ((esp_reset_reason_t)r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset button";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog - the program froze";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout - the supply voltage dipped";
    default: return "other";
  }
}

// ================================================================== setup ===
void appSetup() {
  hw::begin();  // pumps OFF before anything else
  Serial.begin(115200);
  delay(300);  // give the USB-serial chip a moment
  app.bootMs = millis();
  Serial.println();
  Serial.println(F("============== RootSense ==============="));

  const bool loaded = settingsLoad(app.cfg);
  app.resetReason = (int)esp_reset_reason();
  logEvent("Started (%s). %s", resetReasonText(app.resetReason),
           loaded ? "Settings loaded from flash." : "No saved settings yet - using defaults.");
  if (app.resetReason == ESP_RST_BROWNOUT) {
    logEvent("WARNING: the last restart was caused by a voltage dip. If it happens when a pump "
             "starts, use a stronger 5 V supply and add the 470 uF capacitor (see README).");
  }

  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    app.zone[z].ctrl.setLogger(controllerLog, nullptr);
    app.zone[z].ctrl.begin(z, paramsFor(app.cfg.zones[z]), app.cfg.zones[z].enabled != 0);
  }

  // Did we reset while a pump was running?
  if (g_rtcMagic == kRtcMagic && g_rtcPumpZone >= 0 && g_rtcPumpZone < ZONE_COUNT &&
      app.resetReason != ESP_RST_POWERON) {
    app.zone[g_rtcPumpZone].ctrl.raiseFault(ZoneFault::ResetWhilePumping, millis());
  }
  g_rtcMagic = kRtcMagic;
  g_rtcPumpZone = -1;

  app.tank.begin();
  app.lastTankHasWater = app.tank.hasWater();
  if (!app.tank.configured()) {
    logEvent("No tank float switch configured - the pump cannot tell when the tank is empty!");
  } else if (!app.lastTankHasWater) {
    logEvent("The water tank is EMPTY - refill it. (Or check the float switch wiring.)");
  }
  app.pump.begin();
  app.button.begin();

  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    const ZoneConfig& c = app.cfg.zones[z];
    logEvent("Zone %u: %s, automatic watering %s%s", (unsigned)(z + 1), app.zone[z].ctrl.params().plant->name,
             c.enabled ? "ON" : "OFF",
             (c.dryCalibrated && c.wetCalibrated) ? "" : " (sensor not calibrated yet)");
  }

  net::begin(app.cfg.wifiSsid, app.cfg.wifiPass);
}

// ================================================================= inputs ===
bool zoneSensorOk(uint8_t z) {
  return app.zone[z].health == moisture::SensorHealth::Ok && app.zone[z].moistureValid;
}

static void logHealthChange(uint8_t z, moisture::SensorHealth h) {
  if (!app.cfg.zones[z].enabled) return;  // unplugged sensors on unused zones are expected
  if (h == moisture::SensorHealth::Ok) {
    logEvent("Zone %u: sensor reading normally again.", (unsigned)(z + 1));
  } else {
    logEvent("Zone %u: sensor problem (%s, %.0f mV on GPIO %u). Check VCC->3V3, GND->GND, AOUT->GPIO %u.",
             (unsigned)(z + 1), moisture::healthName(h), app.zone[z].rawMv, (unsigned)SENSOR_PINS[z],
             (unsigned)SENSOR_PINS[z]);
  }
}

static void readSensors() {
  hw::sensorPower(true);
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    ZoneRuntime& r = app.zone[z];
    const ZoneConfig& c = app.cfg.zones[z];
    const hw::SensorSample s = hw::readSensor(z);
    r.rawMv = s.meanMv;
    r.spreadMv = s.spreadMv;
    const moisture::SensorHealth h = moisture::classify(s.meanMv, s.spreadMv);

    // Only believe a change of health after it shows up 3 readings in a row.
    if (r.firstReading) {
      r.firstReading = false;
      r.health = h;
      r.pendingCount = 0;
      if (h != moisture::SensorHealth::Ok) logHealthChange(z, h);
    } else if (h == r.health) {
      r.pendingCount = 0;
    } else if (h == r.pendingHealth && r.pendingCount > 0) {
      if (++r.pendingCount >= HEALTH_CONFIRM_READINGS) {
        r.health = h;
        r.pendingCount = 0;
        logHealthChange(z, h);
      }
    } else {
      r.pendingHealth = h;
      r.pendingCount = 1;
    }

    if (h == moisture::SensorHealth::Ok) {
      const float pct = moisture::toPercent(s.meanMv, (float)c.dryMv, (float)c.wetMv);
      r.moisture = r.moistureValid ? moisture::smooth(r.moisture, pct, MOISTURE_SMOOTHING) : pct;
      r.moistureValid = true;
    } else if (r.health != moisture::SensorHealth::Ok) {
      r.moistureValid = false;
    }
  }
  hw::sensorPower(false);
}

static void recordHistory() {
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    uint8_t v = MoistureHistory::NO_DATA;
    if (zoneSensorOk(z)) {
      float m = app.zone[z].moisture;
      if (m < 0.0f) m = 0.0f;
      if (m > 150.0f) m = 150.0f;
      v = (uint8_t)(m + 0.5f);
    }
    app.zone[z].history.push(v);
  }
}

void updateInputs(uint32_t nowMs) {
  app.button.update(nowMs);
  if (app.button.takeLongPress()) {
    String m;
    actionClear(-1, m);
    logEvent("BOOT button held: %s", m.c_str());
  }
  if (app.button.takeShortPress()) console::printStatus();

  app.tank.update(nowMs);
  const bool water = app.tank.hasWater();
  if (water != app.lastTankHasWater) {
    app.lastTankHasWater = water;
    logEvent(water ? "Water tank refilled." : "The water tank is EMPTY - refill it. Watering is paused.");
  }

  if (!app.sensorsReadOnce || nowMs - app.lastSensorMs >= SENSOR_READ_INTERVAL_MS) {
    app.lastSensorMs = nowMs;
    readSensors();
    if (!app.sensorsReadOnce) {
      app.sensorsReadOnce = true;
      app.lastHistoryMs = nowMs;
      recordHistory();
    }
  }
  if (nowMs - app.lastHistoryMs >= HISTORY_INTERVAL_MS) {
    app.lastHistoryMs = nowMs;
    recordHistory();
  }
}

// =============================================================== watering ===
bool quietHoursNow() {
  if (!app.cfg.quietEnabled) return false;
  struct tm t;
  if (!net::localTime(t)) return false;  // no clock yet -> no quiet hours
  const int h = t.tm_hour;
  const int s = app.cfg.quietStartHour;
  const int e = app.cfg.quietEndHour;
  if (s == e) return false;
  return s < e ? (h >= s && h < e) : (h >= s || h < e);  // e.g. 22 -> 7 wraps past midnight
}

void updateWatering(uint32_t nowMs) {
  const bool settled = nowMs - app.bootMs >= BOOT_SETTLE_MS;
  const bool quiet = quietHoursNow();
  const bool water = app.tank.hasWater();
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    ZoneRuntime& r = app.zone[z];
    ZoneInputs in;
    in.nowMs = nowMs;
    in.moisture = r.moisture;
    in.sensorOk = zoneSensorOk(z);
    in.tankOk = water;
    in.quietHours = quiet;
    in.bootSettled = settled;
    r.request = r.ctrl.update(in);
    if (r.ctrl.takeLearnedUpdate()) {
      app.cfg.zones[z].mlPerPercent = r.ctrl.params().mlPerPercent;
      saveSoon(nowMs);
    }
  }
  if (app.settingsDirty && nowMs - app.settingsDirtyMs >= 5000UL) saveNow();
}

// ================================================================== pumps ===
void updatePumps(uint32_t nowMs) {
  app.pump.update(nowMs);

  if (app.pump.running() && !app.tank.hasWater()) {
    logEvent("Zone %u: pump stopped - the water tank is empty.", (unsigned)(app.pump.zone() + 1));
    app.pump.stop(nowMs);
  }

  hw::PumpRun run;
  if (app.pump.takeFinished(run)) {
    g_rtcPumpZone = -1;
    const float ml = hw::pumpMlForMs(run.ranMs, app.cfg.zones[run.zone].flowMlPerSec);
    if (run.owner == hw::PumpOwner::Plant) {
      app.zone[run.zone].ctrl.onPumpStopped(nowMs, ml, run.completed);
    } else if (run.owner == hw::PumpOwner::Test) {
      logEvent("Zone %u: pump test finished (%.1f s). Measure the water in the cup and enter it "
               "under 'Measure pump flow'.",
               (unsigned)(run.zone + 1), (float)run.ranMs / 1000.0f);
    }
  }

  if (app.pump.running() || !app.tank.hasWater()) return;
  // Round-robin, so one thirsty plant can't starve the other.
  for (uint8_t k = 0; k < ZONE_COUNT; k++) {
    const uint8_t z = (uint8_t)((app.nextPumpZone + k) % ZONE_COUNT);
    ZoneRuntime& r = app.zone[z];
    if (!r.request.wanted) continue;
    const uint32_t ms = hw::pumpMsForMl(r.request.ml, app.cfg.zones[z].flowMlPerSec);
    if (app.pump.start(z, ms, hw::PumpOwner::Plant, nowMs)) {
      g_rtcPumpZone = z;
      r.ctrl.onPumpStarted(nowMs);
      r.request.wanted = false;
      app.nextPumpZone = (uint8_t)((z + 1) % ZONE_COUNT);
    }
    break;
  }
}

// ============================================================= status LED ===
void updateStatusLed(uint32_t nowMs) {
  bool alert = app.tank.configured() && !app.tank.hasWater();
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    if (!app.cfg.zones[z].enabled) continue;
    if (app.zone[z].ctrl.state() == ZoneState::Fault || app.zone[z].health != moisture::SensorHealth::Ok) alert = true;
  }
  hw::LedPattern p = hw::LedPattern::Heartbeat;
  if (app.pump.running()) {
    p = hw::LedPattern::Pumping;
  } else if (alert) {
    p = hw::LedPattern::Alert;
  } else if (net::apActive() && !net::connected()) {
    p = hw::LedPattern::SetupMode;
  }
  hw::ledShow(nowMs, p);
}

void maybeRestart(uint32_t nowMs) {
  if (!app.restartPending || (int32_t)(nowMs - app.restartAtMs) < 0) return;
  app.pump.stop(nowMs);
  g_rtcPumpZone = -1;
  if (app.settingsDirty) saveNow();
  Serial.println(F("Restarting..."));
  Serial.flush();
  delay(100);
  ESP.restart();
}

// ============================================================ status text ===
static String minutesText(uint32_t ms) {
  const unsigned long m = (ms + 59999UL) / 60000UL;
  return String(m) + " min";
}

String zoneStatusText(uint8_t z, const char*& level) {
  const uint32_t now = millis();
  const ZoneRuntime& r = app.zone[z];
  const WateringController& c = r.ctrl;
  const PlantProfile& p = *c.params().plant;
  const bool myPumpRunning = app.pump.running() && app.pump.zone() == z;

  switch (c.state()) {
    case ZoneState::Disabled:
      if (myPumpRunning) break;
      level = "info";
      return F("Automatic watering is off for this zone.");
    case ZoneState::Fault:
      if (myPumpRunning) break;
      level = "bad";
      if (c.fault() == ZoneFault::NoResponse) {
        return F("Stopped for safety: two pulses of water did not raise the moisture. Check the tank, "
                 "the tubing and that the sensor is in the soil near the tube, then press Clear alert.");
      }
      return F("Stopped for safety: the board restarted while this pump was running. Check the power "
               "supply and wiring, then press Clear alert.");
    case ZoneState::Watering:
      level = "water";
      if (myPumpRunning) return String("Watering now (pulse ") + String(c.pulsesThisCycle() + 1) + ")...";
      return F("Ready to water - waiting for the other pump to finish...");
    case ZoneState::Soaking:
      level = "water";
      return String("Letting the water soak in - re-checking in ") + minutesText(c.soakRemainingMs(now)) +
             " (pulse " + String(c.pulsesThisCycle()) + " of max " + String(c.params().maxPulses) + ").";
    case ZoneState::Idle:
      break;
  }

  if (myPumpRunning) {
    level = "water";
    if (app.pump.owner() == hw::PumpOwner::Test) return F("Pump test running - catch the water in a cup...");
    return F("Manual watering...");
  }

  switch (c.waitReason()) {
    case WaitReason::BootSettling: {
      const uint32_t e = now - app.bootMs;
      const unsigned long left = e < BOOT_SETTLE_MS ? (BOOT_SETTLE_MS - e) / 1000UL + 1 : 0;
      level = "info";
      return String("Starting up - automatic watering begins in ") + String(left) + " s.";
    }
    case WaitReason::SensorProblem:
      level = "bad";
      return String("Sensor problem (") + moisture::healthName(r.health) +
             ") - not watering until the readings look normal again.";
    case WaitReason::TankEmpty:
      level = "bad";
      return F("The soil is dry but the water tank is empty - refill it.");
    case WaitReason::QuietHours:
      level = "info";
      return String("Dry, but waiting for quiet hours to end (") + String(app.cfg.quietEndHour) + ":00).";
    case WaitReason::Cooldown:
      level = "info";
      return String("Resting after the last watering - next check in ") + minutesText(c.cooldownRemainingMs(now)) + ".";
    case WaitReason::DailyLimit:
      level = "warn";
      return String("Daily water limit reached (") + String((int)c.mlInLast24h(now)) + " ml in 24 h) - waiting.";
    case WaitReason::MoistEnough:
      if (r.moisture > (float)p.soggyAbovePct) {
        level = "warn";
        return String("Wetter than this plant likes (above ") + String(p.soggyAbovePct) +
               "%). Make sure the pot can drain.";
      }
      level = "ok";
      return String("Moist enough - will water when it drops below ") + String(p.waterBelowPct) + "%.";
    case WaitReason::None:
      break;
  }
  level = "ok";
  return F("Checking...");
}

// ================================================================ actions ===
static bool validZone(uint8_t z, String& msg) {
  if (z < ZONE_COUNT) return true;
  msg = String("There is no zone ") + String(z + 1) + " (ZONE_COUNT is " + String(ZONE_COUNT) + " in PlantConfig.h).";
  return false;
}

static String zoneName(uint8_t z) { return String("Zone ") + String(z + 1); }

bool actionSetPlant(uint8_t z, const char* key, String& msg) {
  if (!validZone(z, msg)) return false;
  const PlantProfile* p = plants::find(key);
  if (p == nullptr) {
    msg = String("Unknown plant '") + key + "'. Type 'plants' to see the list.";
    return false;
  }
  ZoneConfig& c = app.cfg.zones[z];
  strncpy(c.plantKey, p->key, sizeof(c.plantKey) - 1);
  c.plantKey[sizeof(c.plantKey) - 1] = '\0';
  applyZone(z);
  saveNow();
  msg = zoneName(z) + ": " + p->name + " - waters below " + String(p->waterBelowPct) + "%, up to " +
        String(p->targetPct) + "%.";
  logEvent("%s", msg.c_str());
  return true;
}

bool actionSetEnabled(uint8_t z, bool enabled, String& msg) {
  if (!validZone(z, msg)) return false;
  const uint32_t now = millis();
  app.cfg.zones[z].enabled = enabled ? 1 : 0;
  if (!enabled && app.pump.running() && app.pump.zone() == z) app.pump.stop(now);
  app.zone[z].ctrl.setEnabled(enabled, now);
  saveNow();
  msg = zoneName(z) + (enabled ? ": automatic watering ON." : ": automatic watering OFF.");
  return true;
}

bool actionSetPot(uint8_t z, long potMl, String& msg) {
  if (!validZone(z, msg)) return false;
  if (potMl < 100 || potMl > 20000) {
    msg = "The pot size must be between 100 and 20000 ml.";
    return false;
  }
  ZoneConfig& c = app.cfg.zones[z];
  c.potMl = (uint16_t)potMl;
  c.mlPerPercent = baselineMlPerPercent((float)potMl);  // start learning again
  long limit = potMl * 4 / 10;
  if (limit < 20) limit = 20;
  if (limit > 5000) limit = 5000;
  c.dailyLimitMl = (uint16_t)limit;
  applyZone(z);
  saveNow();
  msg = zoneName(z) + ": pot set to " + String(potMl) + " ml (daily limit now " + String(limit) + " ml).";
  logEvent("%s", msg.c_str());
  return true;
}

bool actionSetAdvanced(uint8_t z, long soakMin, long cooldownMin, long dailyLimitMl, long maxPulses,
                       String& msg) {
  if (!validZone(z, msg)) return false;
  if (soakMin >= 0 && (soakMin < 2 || soakMin > 120)) {
    msg = "Soak time must be 2-120 minutes.";
    return false;
  }
  if (cooldownMin >= 0 && (cooldownMin < 10 || cooldownMin > 1440)) {
    msg = "Rest time must be 10-1440 minutes.";
    return false;
  }
  if (dailyLimitMl >= 0 && (dailyLimitMl < 20 || dailyLimitMl > 5000)) {
    msg = "Daily limit must be 20-5000 ml.";
    return false;
  }
  if (maxPulses >= 0 && (maxPulses < 1 || maxPulses > 10)) {
    msg = "Max pulses must be 1-10.";
    return false;
  }
  ZoneConfig& c = app.cfg.zones[z];
  if (soakMin >= 0) c.soakMin = (uint8_t)soakMin;
  if (cooldownMin >= 0) c.cooldownMin = (uint16_t)cooldownMin;
  if (dailyLimitMl >= 0) c.dailyLimitMl = (uint16_t)dailyLimitMl;
  if (maxPulses >= 0) c.maxPulses = (uint8_t)maxPulses;
  applyZone(z);
  saveNow();
  msg = zoneName(z) + ": soak " + String(c.soakMin) + " min, rest " + String(c.cooldownMin) + " min, limit " +
        String(c.dailyLimitMl) + " ml/day, max " + String(c.maxPulses) + " pulses.";
  return true;
}

bool actionWaterNow(uint8_t z, long ml, String& msg) {
  if (!validZone(z, msg)) return false;
  if (ml < 5 || ml > 500) {
    msg = "Choose between 5 and 500 ml.";
    return false;
  }
  if (!app.tank.hasWater()) {
    msg = "The water tank is empty - refill it first.";
    return false;
  }
  if (app.pump.running()) {
    msg = "A pump is already running - try again in a moment.";
    return false;
  }
  if (!app.zone[z].ctrl.requestManual((float)ml)) {
    msg = zoneName(z) + " is in the middle of a watering cycle - try again when it finishes.";
    return false;
  }
  msg = zoneName(z) + ": watering " + String(ml) + " ml.";
  return true;
}

bool actionPumpTest(uint8_t z, long seconds, String& msg) {
  if (!validZone(z, msg)) return false;
  if (seconds < 1 || seconds > 30) {
    msg = "The test can run 1-30 seconds.";
    return false;
  }
  if (!app.tank.hasWater()) {
    msg = "The water tank is empty - refill it first.";
    return false;
  }
  if (app.pump.running()) {
    msg = "A pump is already running - try again in a moment.";
    return false;
  }
  if (!app.pump.start(z, (uint32_t)seconds * 1000UL, hw::PumpOwner::Test, millis())) {
    msg = "Could not start the pump.";
    return false;
  }
  g_rtcPumpZone = z;
  msg = zoneName(z) + ": pump running for " + String(seconds) + " s - catch the water in a measuring cup.";
  logEvent("%s", msg.c_str());
  return true;
}

bool actionSetFlow(uint8_t z, float measuredMl, float seconds, String& msg) {
  if (!validZone(z, msg)) return false;
  if (!(seconds >= 1.0f && seconds <= 30.0f)) {
    msg = "The test length must be 1-30 seconds.";
    return false;
  }
  if (!(measuredMl >= 1.0f && measuredMl <= 3000.0f)) {
    msg = "Enter the measured amount in ml (1-3000).";
    return false;
  }
  const float mlAtOneMlPerSec = hw::pumpMlForMs((uint32_t)(seconds * 1000.0f), 1.0f);
  float flow = measuredMl / mlAtOneMlPerSec;
  if (flow < 1.0f) flow = 1.0f;
  if (flow > 200.0f) flow = 200.0f;
  app.cfg.zones[z].flowMlPerSec = flow;
  saveNow();
  msg = zoneName(z) + ": pump flow set to " + String(flow, 1) + " ml/s (" + String((int)(flow * 60.0f)) +
        " ml per minute).";
  logEvent("%s", msg.c_str());
  return true;
}

bool actionCalibrate(uint8_t z, bool dryPoint, String& msg) {
  if (!validZone(z, msg)) return false;
  ZoneRuntime& r = app.zone[z];
  if (r.health == moisture::SensorHealth::Disconnected || r.health == moisture::SensorHealth::OverRange) {
    msg = String("The sensor reading looks wrong (") + moisture::healthName(r.health) + ", " + String((int)r.rawMv) +
          " mV) - check its wiring first.";
    return false;
  }
  const uint16_t mv = (uint16_t)(r.rawMv + 0.5f);
  ZoneConfig& c = app.cfg.zones[z];
  if (dryPoint) {
    if (c.wetCalibrated && mv < c.wetMv + 300) {
      msg = String("That reading (") + String(mv) + " mV) is hardly drier than the WET point (" + String(c.wetMv) +
            " mV). Is the sensor clean, dry and held in the air?";
      return false;
    }
    c.dryMv = mv;
    c.dryCalibrated = 1;
  } else {
    if (mv + 300 > c.dryMv) {
      msg = String("That reading (") + String(mv) + " mV) is hardly wetter than the DRY point (" + String(c.dryMv) +
            " mV). Is the sensor pushed into freshly watered soil up to the line?";
      return false;
    }
    c.wetMv = mv;
    c.wetCalibrated = 1;
  }
  r.moistureValid = false;  // restart the smoothing on the new scale
  saveNow();
  msg = zoneName(z) + (dryPoint ? ": DRY point saved (" : ": WET point saved (") + String(mv) + " mV).";
  if (c.dryCalibrated && c.wetCalibrated) {
    msg += " Calibration complete.";
  } else {
    msg += dryPoint ? " Next: the WET point." : " Next: the DRY point.";
  }
  logEvent("%s", msg.c_str());
  return true;
}

bool actionClear(int z, String& msg) {
  if (z < 0) {
    for (uint8_t i = 0; i < ZONE_COUNT; i++) app.zone[i].ctrl.clearFault();
    msg = "All alerts cleared.";
    return true;
  }
  if (!validZone((uint8_t)z, msg)) return false;
  app.zone[z].ctrl.clearFault();
  msg = zoneName((uint8_t)z) + ": alert cleared.";
  return true;
}

bool actionStopPumps(String& msg) {
  const uint32_t now = millis();
  const bool wasRunning = app.pump.running();
  app.pump.stop(now);
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    app.zone[z].ctrl.cancelManual();
    app.zone[z].ctrl.stopCycle(now);
  }
  msg = wasRunning ? "Pump stopped." : "No pump was running (pending watering cancelled).";
  logEvent("Stop pressed: %s", msg.c_str());
  return true;
}

bool actionSetQuietHours(bool enabled, long startHour, long endHour, String& msg) {
  if (startHour < 0 || startHour > 23 || endHour < 0 || endHour > 23) {
    msg = "Hours must be 0-23.";
    return false;
  }
  app.cfg.quietEnabled = enabled ? 1 : 0;
  app.cfg.quietStartHour = (uint8_t)startHour;
  app.cfg.quietEndHour = (uint8_t)endHour;
  saveNow();
  if (enabled) {
    msg = String("Quiet hours ON: no watering starts between ") + String(startHour) + ":00 and " +
          String(endHour) + ":00.";
    if (!net::timeValid()) msg += " (Needs the internet clock - only works once Wi-Fi is connected.)";
  } else {
    msg = "Quiet hours OFF.";
  }
  return true;
}

bool actionSetWifi(const char* ssid, const char* password, String& msg) {
  const size_t sl = ssid ? strlen(ssid) : 0;
  const size_t pl = password ? strlen(password) : 0;
  if (sl < 1 || sl > 32) {
    msg = "The Wi-Fi name must be 1-32 characters.";
    return false;
  }
  if (pl != 0 && (pl < 8 || pl > 63)) {
    msg = "The Wi-Fi password must be 8-63 characters (or empty for an open network).";
    return false;
  }
  strncpy(app.cfg.wifiSsid, ssid, sizeof(app.cfg.wifiSsid) - 1);
  app.cfg.wifiSsid[sizeof(app.cfg.wifiSsid) - 1] = '\0';
  strncpy(app.cfg.wifiPass, password ? password : "", sizeof(app.cfg.wifiPass) - 1);
  app.cfg.wifiPass[sizeof(app.cfg.wifiPass) - 1] = '\0';
  saveNow();
  actionRestart(1500);
  msg = String("Saved. Restarting to join \"") + ssid + "\". If it works the dashboard will be at http://" +
        DEVICE_HOSTNAME + ".local (or check the Serial Monitor for the IP address).";
  return true;
}

bool actionFactoryReset(String& msg) {
  settingsErase();
  settingsDefaults(app.cfg);
  app.settingsDirty = false;
  actionRestart(1000);
  msg = "All settings erased. Restarting...";
  return true;
}

void actionRestart(uint32_t delayMs) {
  app.restartPending = true;
  app.restartAtMs = millis() + delayMs;
}
