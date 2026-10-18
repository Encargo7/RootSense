#pragma once
// =============================================================================
//  WateringController — the "brain" for ONE plant.
//
//  It decides WHEN to water and HOW MUCH, using "pulse and soak":
//
//    1. Soil drops below the plant's waterBelow%  ->  start a watering cycle.
//    2. Give one small pulse of water (sized from how dry the soil is and how
//       much water this pot needed last time).
//    3. Wait (soak) so the water can spread to the sensor, then re-measure.
//    4. Reached the target?  -> done, rest for a while.
//       Not yet?             -> another pulse (up to a maximum).
//
//  Watering in pulses avoids the classic beginner mistake of running the pump
//  until the sensor reads wet: water takes minutes to reach the sensor, so that
//  approach floods the pot every time.
//
//  Safety rules built in:
//    - never water if the sensor looks broken, the tank is empty, or during
//      the first minutes after power-up;
//    - a daily water limit and a maximum number of pulses per cycle;
//    - if two pulses in a row make no difference to the reading, STOP and wait
//      for a human (empty tank, tube fell out of the pot, sensor out of soil...).
//
//  This file has no pins, delays or Arduino calls, so it is unit-tested on a PC.
// =============================================================================
#include <stdint.h>

#include "PlantDatabase.h"

enum class ZoneState : uint8_t {
  Disabled,  // zone switched off (manual watering still possible)
  Idle,      // watching the soil
  Watering,  // waiting for, or running, the pump for one pulse
  Soaking,   // pulse done; letting the water spread before re-measuring
  Fault      // stopped for safety until someone presses "Clear"
};

enum class ZoneFault : uint8_t {
  None,
  NoResponse,        // two pulses but the moisture reading did not rise
  ResetWhilePumping  // the board restarted while this pump was running
};

// Why an idle zone is not watering right now (shown on the dashboard).
enum class WaitReason : uint8_t {
  None,
  MoistEnough,
  BootSettling,
  SensorProblem,
  TankEmpty,
  QuietHours,
  Cooldown,
  DailyLimit
};

enum class CycleResult : uint8_t {
  None,
  ReachedTarget,  // soil got back up to the target
  SoilFull,       // the reading stopped rising: soil cannot hold more
  MaxPulses,      // gave the maximum number of pulses
  DailyLimit,     // hit the daily water limit
  Interrupted     // tank empty, sensor problem, zone switched off...
};

struct ZoneParams {
  const PlantProfile* plant;
  float potMl;         // soil volume in the pot, ml (1 L = 1000 ml)
  float mlPerPercent;  // water needed to raise the reading by 1 % (learned)
  float maxPulseMl;    // biggest single pulse
  float minPulseMl;    // smallest pulse worth running the pump for
  float dailyLimitMl;  // never give more than this in any 24 h
  uint8_t maxPulses;   // pulses per watering cycle
  uint32_t soakMs;     // wait after each pulse before re-measuring
  uint32_t cooldownMs; // rest after a finished cycle
};

// Sensible starting parameters for a plant in a pot of the given size.
ZoneParams defaultZoneParams(const PlantProfile* plant, float potMl);
// First guess of "ml per 1 %" for a pot size; the controller refines it.
float baselineMlPerPercent(float potMl);

struct ZoneInputs {
  uint32_t nowMs;
  float moisture;    // filtered reading, % (see PlantDatabase.h for the scale)
  bool sensorOk;
  bool tankOk;
  bool quietHours;
  bool bootSettled;
};

struct PumpRequest {
  bool wanted;  // true = please run this zone's pump
  float ml;     // how much water
  bool manual;  // requested by a person, not by the automatic logic
};

class WateringController {
 public:
  typedef void (*LogFn)(void* ctx, uint8_t zoneIndex, const char* message);

  WateringController();
  void begin(uint8_t zoneIndex, const ZoneParams& params, bool enabled);
  void setParams(const ZoneParams& params);
  const ZoneParams& params() const { return p_; }
  void setEnabled(bool enabled, uint32_t nowMs);
  bool enabled() const { return enabled_; }
  void setLogger(LogFn fn, void* ctx) {
    logFn_ = fn;
    logCtx_ = ctx;
  }

  // Call often (every loop). Returns whether this zone wants its pump on.
  PumpRequest update(const ZoneInputs& in);
  // The application tells the controller what the pump actually did.
  void onPumpStarted(uint32_t nowMs);
  void onPumpStopped(uint32_t nowMs, float mlDelivered, bool completed);

  bool requestManual(float ml);  // false if a watering cycle is in progress
  void cancelManual() { manualPendingMl_ = 0.0f; }
  void stopCycle(uint32_t nowMs);  // abandon a cycle in progress (the Stop button)
  void clearFault();
  void raiseFault(ZoneFault fault, uint32_t nowMs);

  ZoneState state() const { return state_; }
  ZoneFault fault() const { return fault_; }
  WaitReason waitReason() const { return wait_; }
  CycleResult lastResult() const { return lastResult_; }
  bool pumpRunning() const { return pumpRunning_; }
  uint8_t pulsesThisCycle() const { return pulses_; }
  float cycleMl() const { return cycleMl_; }
  uint32_t soakRemainingMs(uint32_t nowMs) const;
  uint32_t cooldownRemainingMs(uint32_t nowMs) const;
  float mlInLast24h(uint32_t nowMs) const;
  bool hasWatered() const { return hasWatered_; }
  uint32_t lastWateredMs() const { return lastWateredMs_; }
  // True once after the learned ml-per-% changed (so the app can save it).
  bool takeLearnedUpdate();

 private:
  struct Dose {
    uint32_t atMs;
    float ml;
  };
  static const uint8_t DOSE_LOG_SIZE = 48;

  WaitReason whyNotStart(const ZoneInputs& in) const;
  float planPulse(float moisture, uint32_t nowMs) const;
  void startCycle(const ZoneInputs& in);
  void evaluateAfterSoak(const ZoneInputs& in);
  void endCycle(CycleResult result, uint32_t nowMs, const char* detail);
  void learn(float ml, float rise);
  void recordDose(uint32_t nowMs, float ml);
  void pruneDoses(uint32_t nowMs);
  float targetPct() const;
  void report(const char* fmt, ...);

  uint8_t zone_;
  ZoneParams p_;
  bool enabled_;
  ZoneState state_;
  ZoneFault fault_;
  WaitReason wait_;
  CycleResult lastResult_;

  ZoneInputs last_;
  bool haveInputs_;

  // current cycle
  uint8_t pulses_;
  float cycleMl_;
  float pulseMl_;           // size of the next / current pulse
  float lastPulseMl_;       // water actually delivered by the last pulse
  float pulseStartMoisture_;
  uint32_t soakStartMs_;
  bool respondedThisCycle_;
  uint8_t noResponseCount_;

  // between cycles
  bool hasCycleEnded_;
  uint32_t cycleEndMs_;
  bool hasWatered_;
  uint32_t lastWateredMs_;

  // pump bookkeeping
  bool pumpRunning_;
  bool requestIsManual_;
  bool runIsManual_;
  float manualPendingMl_;

  bool learnedChanged_;

  Dose doses_[DOSE_LOG_SIZE];
  uint8_t doseCount_;

  LogFn logFn_;
  void* logCtx_;
};

const char* zoneStateName(ZoneState s);
const char* zoneFaultName(ZoneFault f);
const char* waitReasonName(WaitReason w);
const char* cycleResultName(CycleResult r);
