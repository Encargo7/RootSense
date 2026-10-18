#include "WateringController.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

const uint32_t DAY_MS = 24UL * 60UL * 60UL * 1000UL;
const float MIN_RISE_PCT = 1.5f;             // smallest rise that proves water arrived
const float EXPECTED_RISE_FRACTION = 0.25f;  // ...or a quarter of the rise we expected
const float LEARN_RATE = 0.3f;               // how fast the ml-per-% estimate adapts
const uint8_t NO_RESPONSE_LIMIT = 2;         // pulses without effect before stopping

// Wrap-safe "time since": works even when millis() rolls over after ~49 days.
inline uint32_t since(uint32_t now, uint32_t then) { return now - then; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline float maxf(float a, float b) { return a > b ? a : b; }

}  // namespace

float baselineMlPerPercent(float potMl) {
  // Potting mix holds roughly 40-50 % of its volume as water when "just
  // watered", but the sensor mostly sees the top half of the pot. 0.4 % of the
  // pot volume per 1 % on our scale is a cautious first guess; it is refined
  // after every watering.
  return clampf(potMl * 0.004f, 0.5f, 200.0f);
}

ZoneParams defaultZoneParams(const PlantProfile* plant, float potMl) {
  ZoneParams p;
  p.plant = plant;
  p.potMl = potMl;
  p.mlPerPercent = baselineMlPerPercent(potMl);
  p.maxPulseMl = clampf(potMl * 0.08f, 30.0f, 250.0f);
  p.minPulseMl = 20.0f;
  p.dailyLimitMl = clampf(potMl * 0.4f, 100.0f, 3000.0f);
  p.maxPulses = 5;
  p.soakMs = 10UL * 60UL * 1000UL;
  p.cooldownMs = 60UL * 60UL * 1000UL;
  return p;
}

WateringController::WateringController()
    : zone_(0),
      enabled_(false),
      state_(ZoneState::Disabled),
      fault_(ZoneFault::None),
      wait_(WaitReason::None),
      lastResult_(CycleResult::None),
      haveInputs_(false),
      pulses_(0),
      cycleMl_(0),
      pulseMl_(0),
      lastPulseMl_(0),
      pulseStartMoisture_(0),
      soakStartMs_(0),
      respondedThisCycle_(false),
      noResponseCount_(0),
      hasCycleEnded_(false),
      cycleEndMs_(0),
      hasWatered_(false),
      lastWateredMs_(0),
      pumpRunning_(false),
      requestIsManual_(false),
      runIsManual_(false),
      manualPendingMl_(0),
      learnedChanged_(false),
      doseCount_(0),
      logFn_(nullptr),
      logCtx_(nullptr) {
  p_ = defaultZoneParams(&plants::fallback(), 1500.0f);
  memset(&last_, 0, sizeof(last_));
  memset(doses_, 0, sizeof(doses_));
}

void WateringController::begin(uint8_t zoneIndex, const ZoneParams& params, bool enabled) {
  zone_ = zoneIndex;
  setParams(params);
  enabled_ = enabled;
  state_ = enabled ? ZoneState::Idle : ZoneState::Disabled;
}

void WateringController::setParams(const ZoneParams& params) {
  p_ = params;
  if (p_.plant == nullptr) p_.plant = &plants::fallback();
  if (p_.potMl < 50.0f) p_.potMl = 50.0f;
  if (p_.mlPerPercent <= 0.0f) p_.mlPerPercent = baselineMlPerPercent(p_.potMl);
  if (p_.minPulseMl < 5.0f) p_.minPulseMl = 5.0f;
  if (p_.maxPulseMl < p_.minPulseMl) p_.maxPulseMl = p_.minPulseMl;
  if (p_.maxPulses == 0) p_.maxPulses = 1;
}

void WateringController::setEnabled(bool enabled, uint32_t nowMs) {
  if (enabled == enabled_) return;
  enabled_ = enabled;
  if (!enabled) {
    if (state_ == ZoneState::Watering || state_ == ZoneState::Soaking) {
      endCycle(CycleResult::Interrupted, nowMs, "zone switched off");
    }
    state_ = ZoneState::Disabled;
    wait_ = WaitReason::None;
    report("Automatic watering switched OFF.");
  } else {
    state_ = (fault_ != ZoneFault::None) ? ZoneState::Fault : ZoneState::Idle;
    report("Automatic watering switched ON.");
  }
}

float WateringController::targetPct() const { return (float)p_.plant->targetPct; }

PumpRequest WateringController::update(const ZoneInputs& in) {
  last_ = in;
  haveInputs_ = true;
  pruneDoses(in.nowMs);

  PumpRequest req = {false, 0.0f, false};
  if (pumpRunning_) return req;  // nothing to decide while the pump runs

  // 1) A person asked for water: allowed whenever no automatic cycle is busy.
  if (manualPendingMl_ > 0.0f && state_ != ZoneState::Watering && state_ != ZoneState::Soaking) {
    if (!in.tankOk) {
      report("Manual watering cancelled: the water tank is empty.");
      manualPendingMl_ = 0.0f;
    } else {
      requestIsManual_ = true;
      req.wanted = true;
      req.ml = manualPendingMl_;
      req.manual = true;
      return req;
    }
  }
  requestIsManual_ = false;

  switch (state_) {
    case ZoneState::Disabled:
    case ZoneState::Fault:
      wait_ = WaitReason::None;
      return req;

    case ZoneState::Watering:
      // A pulse is planned but the pump has not started yet (e.g. the other
      // zone's pump is busy). Give up if conditions changed meanwhile.
      if (!in.tankOk) {
        endCycle(CycleResult::Interrupted, in.nowMs, "the water tank is empty");
        return req;
      }
      if (!in.sensorOk) {
        endCycle(CycleResult::Interrupted, in.nowMs, "sensor problem");
        return req;
      }
      req.wanted = true;
      req.ml = pulseMl_;
      return req;

    case ZoneState::Soaking:
      if (since(in.nowMs, soakStartMs_) < p_.soakMs) return req;
      evaluateAfterSoak(in);
      if (state_ == ZoneState::Watering) {
        req.wanted = true;
        req.ml = pulseMl_;
      }
      return req;

    case ZoneState::Idle:
      wait_ = whyNotStart(in);
      if (wait_ != WaitReason::None) return req;
      startCycle(in);
      req.wanted = true;
      req.ml = pulseMl_;
      return req;
  }
  return req;
}

WaitReason WateringController::whyNotStart(const ZoneInputs& in) const {
  if (!in.bootSettled) return WaitReason::BootSettling;
  if (!in.sensorOk) return WaitReason::SensorProblem;
  if (in.moisture >= (float)p_.plant->waterBelowPct) return WaitReason::MoistEnough;
  if (hasCycleEnded_ && since(in.nowMs, cycleEndMs_) < p_.cooldownMs) return WaitReason::Cooldown;
  if (!in.tankOk) return WaitReason::TankEmpty;
  if (in.quietHours) return WaitReason::QuietHours;
  if (planPulse(in.moisture, in.nowMs) < p_.minPulseMl) return WaitReason::DailyLimit;
  return WaitReason::None;
}

float WateringController::planPulse(float moisture, uint32_t nowMs) const {
  float deficit = targetPct() - moisture;
  if (deficit < 1.0f) deficit = 1.0f;
  float ml = clampf(deficit * p_.mlPerPercent, p_.minPulseMl, p_.maxPulseMl);
  float remaining = p_.dailyLimitMl - mlInLast24h(nowMs);
  if (ml > remaining) ml = remaining;  // may drop below minPulseMl -> caller stops
  return ml;
}

void WateringController::startCycle(const ZoneInputs& in) {
  pulses_ = 0;
  cycleMl_ = 0.0f;
  respondedThisCycle_ = false;
  noResponseCount_ = 0;
  pulseMl_ = planPulse(in.moisture, in.nowMs);
  state_ = ZoneState::Watering;
  wait_ = WaitReason::None;
  lastResult_ = CycleResult::None;
  report("Soil is down to %.1f%% (waters below %u%%) -> watering up to %u%%.",
         floorf(in.moisture * 10.0f) / 10.0f, (unsigned)p_.plant->waterBelowPct,
         (unsigned)p_.plant->targetPct);
}

void WateringController::onPumpStarted(uint32_t nowMs) {
  (void)nowMs;
  pumpRunning_ = true;
  runIsManual_ = requestIsManual_;
  pulseStartMoisture_ = last_.moisture;
}

void WateringController::onPumpStopped(uint32_t nowMs, float mlDelivered, bool completed) {
  pumpRunning_ = false;
  if (mlDelivered < 0.0f) mlDelivered = 0.0f;
  recordDose(nowMs, mlDelivered);
  if (mlDelivered >= 1.0f) {
    hasWatered_ = true;
    lastWateredMs_ = nowMs;
  }

  if (runIsManual_) {
    runIsManual_ = false;
    requestIsManual_ = false;
    manualPendingMl_ = 0.0f;
    report("Manual watering: %.0f ml%s.", mlDelivered, completed ? "" : " (stopped early)");
    return;
  }
  if (state_ != ZoneState::Watering) return;  // cycle was cancelled while pumping

  pulses_++;
  cycleMl_ += mlDelivered;
  lastPulseMl_ = mlDelivered;
  if (!completed) {
    endCycle(CycleResult::Interrupted, nowMs, "the pump was stopped");
    return;
  }
  state_ = ZoneState::Soaking;
  soakStartMs_ = nowMs;
  report("Pulse %u: %.0f ml. Re-checking in %lu min.", (unsigned)pulses_, mlDelivered,
       (unsigned long)(p_.soakMs / 60000UL));
}

void WateringController::evaluateAfterSoak(const ZoneInputs& in) {
  if (!in.sensorOk) {
    endCycle(CycleResult::Interrupted, in.nowMs, "sensor problem");
    return;
  }
  const float rise = in.moisture - pulseStartMoisture_;
  const float expected = lastPulseMl_ / p_.mlPerPercent;
  const bool responded = rise >= maxf(MIN_RISE_PCT, EXPECTED_RISE_FRACTION * expected);

  if (responded) {
    respondedThisCycle_ = true;
    noResponseCount_ = 0;
    learn(lastPulseMl_, rise);
  } else {
    noResponseCount_++;
  }

  if (in.moisture >= targetPct()) {
    endCycle(CycleResult::ReachedTarget, in.nowMs, nullptr);
  } else if (!responded && respondedThisCycle_) {
    endCycle(CycleResult::SoilFull, in.nowMs, nullptr);
  } else if (noResponseCount_ >= NO_RESPONSE_LIMIT) {
    raiseFault(ZoneFault::NoResponse, in.nowMs);
  } else if (pulses_ >= p_.maxPulses) {
    endCycle(CycleResult::MaxPulses, in.nowMs, nullptr);
  } else if (!in.tankOk) {
    endCycle(CycleResult::Interrupted, in.nowMs, "the water tank is empty");
  } else if (in.quietHours) {
    endCycle(CycleResult::Interrupted, in.nowMs, "quiet hours started");
  } else {
    const float ml = planPulse(in.moisture, in.nowMs);
    if (ml < p_.minPulseMl) {
      endCycle(CycleResult::DailyLimit, in.nowMs, nullptr);
    } else {
      pulseMl_ = ml;
      state_ = ZoneState::Watering;
    }
  }
}

void WateringController::learn(float ml, float rise) {
  if (rise <= 0.0f || ml <= 0.0f) return;
  const float base = baselineMlPerPercent(p_.potMl);
  const float observed = clampf(ml / rise, base * 0.25f, base * 4.0f);
  p_.mlPerPercent = p_.mlPerPercent + LEARN_RATE * (observed - p_.mlPerPercent);
  learnedChanged_ = true;
}

void WateringController::endCycle(CycleResult result, uint32_t nowMs, const char* detail) {
  lastResult_ = result;
  hasCycleEnded_ = true;
  cycleEndMs_ = nowMs;
  state_ = enabled_ ? ZoneState::Idle : ZoneState::Disabled;
  const float soil = last_.moisture;
  const char* plural = pulses_ == 1 ? "" : "s";
  switch (result) {
    case CycleResult::ReachedTarget:
      report("Done: soil is back to %.0f%% (%u pulse%s, %.0f ml).", soil, (unsigned)pulses_, plural, cycleMl_);
      break;
    case CycleResult::SoilFull:
      report("Done: the soil stopped absorbing at %.0f%% (%u pulse%s, %.0f ml). If this keeps "
           "happening, lower the target or redo the WET calibration.",
           soil, (unsigned)pulses_, plural, cycleMl_);
      break;
    case CycleResult::MaxPulses:
      report("Paused after the maximum %u pulses (%.0f ml); soil at %.0f%%. Will check again after "
           "the rest period.",
           (unsigned)pulses_, cycleMl_, soil);
      break;
    case CycleResult::DailyLimit:
      report("Paused: daily water limit (%.0f ml) reached.", p_.dailyLimitMl);
      break;
    case CycleResult::Interrupted:
      report("Watering stopped early: %s (%.0f ml given).", detail ? detail : "interrupted", cycleMl_);
      break;
    case CycleResult::None:
      break;
  }
}

void WateringController::raiseFault(ZoneFault fault, uint32_t nowMs) {
  if (state_ == ZoneState::Watering || state_ == ZoneState::Soaking) {
    lastResult_ = CycleResult::Interrupted;
    hasCycleEnded_ = true;
    cycleEndMs_ = nowMs;
  }
  fault_ = fault;
  if (enabled_) state_ = ZoneState::Fault;
  wait_ = WaitReason::None;
  switch (fault) {
    case ZoneFault::NoResponse:
      report("STOPPED: watered %u times but the moisture reading did not rise. Check the tank, the "
           "tubing, and that the sensor is pushed into the soil near the tube, then press Clear.",
           (unsigned)pulses_);
      break;
    case ZoneFault::ResetWhilePumping:
      report("STOPPED: the board restarted while this pump was running (often a weak power supply). "
           "Check the wiring/power, then press Clear.");
      break;
    case ZoneFault::None:
      break;
  }
}

void WateringController::clearFault() {
  if (fault_ == ZoneFault::None) return;
  fault_ = ZoneFault::None;
  noResponseCount_ = 0;
  if (state_ == ZoneState::Fault) state_ = enabled_ ? ZoneState::Idle : ZoneState::Disabled;
  report("Alert cleared.");
}

void WateringController::stopCycle(uint32_t nowMs) {
  if (state_ == ZoneState::Watering || state_ == ZoneState::Soaking) {
    endCycle(CycleResult::Interrupted, nowMs, "stopped by the user");
  }
}

bool WateringController::requestManual(float ml) {
  if (state_ == ZoneState::Watering || state_ == ZoneState::Soaking || pumpRunning_) return false;
  if (ml < 1.0f) return false;
  manualPendingMl_ = ml;
  return true;
}

uint32_t WateringController::soakRemainingMs(uint32_t nowMs) const {
  if (state_ != ZoneState::Soaking) return 0;
  uint32_t e = since(nowMs, soakStartMs_);
  return e >= p_.soakMs ? 0 : p_.soakMs - e;
}

uint32_t WateringController::cooldownRemainingMs(uint32_t nowMs) const {
  if (!hasCycleEnded_) return 0;
  uint32_t e = since(nowMs, cycleEndMs_);
  return e >= p_.cooldownMs ? 0 : p_.cooldownMs - e;
}

float WateringController::mlInLast24h(uint32_t nowMs) const {
  float total = 0.0f;
  for (uint8_t i = 0; i < doseCount_; i++) {
    if (since(nowMs, doses_[i].atMs) < DAY_MS) total += doses_[i].ml;
  }
  return total;
}

void WateringController::recordDose(uint32_t nowMs, float ml) {
  if (ml <= 0.0f) return;
  pruneDoses(nowMs);
  if (doseCount_ == DOSE_LOG_SIZE) {
    // Log full: fold the oldest entry into the next one. Keeping the newer
    // timestamp means that water is counted for longer - never less.
    doses_[1].ml += doses_[0].ml;
    memmove(&doses_[0], &doses_[1], sizeof(Dose) * (DOSE_LOG_SIZE - 1));
    doseCount_--;
  }
  doses_[doseCount_].atMs = nowMs;
  doses_[doseCount_].ml = ml;
  doseCount_++;
}

void WateringController::pruneDoses(uint32_t nowMs) {
  uint8_t drop = 0;
  while (drop < doseCount_ && since(nowMs, doses_[drop].atMs) >= DAY_MS) drop++;
  if (drop == 0) return;
  memmove(&doses_[0], &doses_[drop], sizeof(Dose) * (doseCount_ - drop));
  doseCount_ -= drop;
}

bool WateringController::takeLearnedUpdate() {
  bool changed = learnedChanged_;
  learnedChanged_ = false;
  return changed;
}

void WateringController::report(const char* fmt, ...) {
  if (logFn_ == nullptr) return;
  char buf[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logFn_(logCtx_, zone_, buf);
}

const char* zoneStateName(ZoneState s) {
  switch (s) {
    case ZoneState::Disabled: return "off";
    case ZoneState::Idle: return "idle";
    case ZoneState::Watering: return "watering";
    case ZoneState::Soaking: return "soaking";
    case ZoneState::Fault: return "fault";
  }
  return "?";
}

const char* zoneFaultName(ZoneFault f) {
  switch (f) {
    case ZoneFault::None: return "none";
    case ZoneFault::NoResponse: return "no-response";
    case ZoneFault::ResetWhilePumping: return "reset-while-pumping";
  }
  return "?";
}

const char* waitReasonName(WaitReason w) {
  switch (w) {
    case WaitReason::None: return "none";
    case WaitReason::MoistEnough: return "moist-enough";
    case WaitReason::BootSettling: return "starting-up";
    case WaitReason::SensorProblem: return "sensor-problem";
    case WaitReason::TankEmpty: return "tank-empty";
    case WaitReason::QuietHours: return "quiet-hours";
    case WaitReason::Cooldown: return "resting";
    case WaitReason::DailyLimit: return "daily-limit";
  }
  return "?";
}

const char* cycleResultName(CycleResult r) {
  switch (r) {
    case CycleResult::None: return "none";
    case CycleResult::ReachedTarget: return "reached-target";
    case CycleResult::SoilFull: return "soil-full";
    case CycleResult::MaxPulses: return "max-pulses";
    case CycleResult::DailyLimit: return "daily-limit";
    case CycleResult::Interrupted: return "interrupted";
  }
  return "?";
}
