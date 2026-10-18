#pragma once
// =============================================================================
//  Hardware — everything that touches a pin: soil sensors, pumps, the tank
//  float switch, the status LED and the BOOT button.
// =============================================================================
#include <Arduino.h>

#include "PlantConfig.h"

namespace hw {

void begin();  // call first in setup(): makes sure every pump is OFF

// ------------------------------------------------------------ soil sensors --
struct SensorSample {
  float meanMv;    // trimmed average voltage at the sensor's AOUT pin
  float spreadMv;  // how much the samples disagreed (noise)
};
void sensorPower(bool on);         // only does something if SENSOR_POWER_PIN >= 0
SensorSample readSensor(uint8_t zone);

// -------------------------------------------------- tank float switch -------
class TankMonitor {
 public:
  void begin();
  void update(uint32_t nowMs);
  bool configured() const { return TANK_SWITCH_PIN >= 0; }
  bool hasWater() const { return stable_; }  // debounced; always true without a switch

 private:
  bool readRaw() const;
  bool stable_ = true;
  bool lastRaw_ = true;
  uint32_t changedAtMs_ = 0;
};

// --------------------------------------------------------------- pumps ------
enum class PumpOwner : uint8_t { None, Plant, Test };

struct PumpRun {
  uint8_t zone;
  PumpOwner owner;
  uint32_t ranMs;
  bool completed;  // false = stopped early
};

// Runs at most ONE pump at a time (keeps the current draw low), with a soft
// start and a hard time limit.
class PumpDriver {
 public:
  void begin();
  bool start(uint8_t zone, uint32_t durationMs, PumpOwner owner, uint32_t nowMs);
  void stop(uint32_t nowMs);    // early stop
  void update(uint32_t nowMs);  // call every loop
  bool running() const { return running_; }
  uint8_t zone() const { return zone_; }
  PumpOwner owner() const { return owner_; }
  uint32_t elapsedMs(uint32_t nowMs) const { return running_ ? nowMs - startMs_ : 0; }
  uint32_t durationMs() const { return durationMs_; }
  bool takeFinished(PumpRun& out);  // true once after each run ends

 private:
  void finish(uint32_t nowMs, bool completed);
  void output(uint8_t zone, float level);  // level 0..1
  bool running_ = false;
  uint8_t zone_ = 0;
  PumpOwner owner_ = PumpOwner::None;
  uint32_t startMs_ = 0;
  uint32_t durationMs_ = 0;
  float level_ = 0.0f;
  bool hasFinished_ = false;
  PumpRun finished_ = {0, PumpOwner::None, 0, false};
};

// Converting between water volume and pump run time (includes the soft start).
uint32_t pumpMsForMl(float ml, float flowMlPerSec);
float pumpMlForMs(uint32_t ms, float flowMlPerSec);

// ----------------------------------------------------------- status LED -----
enum class LedPattern : uint8_t { Off, Heartbeat, Pumping, Alert, SetupMode };
void ledShow(uint32_t nowMs, LedPattern pattern);

// -------------------------------------------------------- BOOT button -------
class Button {
 public:
  void begin();
  void update(uint32_t nowMs);
  bool takeShortPress();
  bool takeLongPress();  // held 3 s

 private:
  bool down_ = false;
  bool longFired_ = false;
  bool shortPending_ = false;
  bool longPending_ = false;
  uint32_t downAtMs_ = 0;
};

}  // namespace hw
