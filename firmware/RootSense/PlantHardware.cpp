#include "PlantHardware.h"

#include <esp_arduino_version.h>

#include "MoistureMath.h"

namespace hw {

// ----------------------------------------------------------------- PWM ------
// The ESP32 Arduino core changed its PWM ("LEDC") functions in version 3.
// These two helpers hide the difference so the sketch builds on 2.x and 3.x.
#if PUMP_DRIVER == PUMP_DRIVER_MOSFET
static void pwmAttach(uint8_t zone) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PUMP_PINS[zone], PUMP_PWM_FREQ_HZ, PUMP_PWM_BITS);
#else
  ledcSetup(zone, PUMP_PWM_FREQ_HZ, PUMP_PWM_BITS);
  ledcAttachPin(PUMP_PINS[zone], zone);
#endif
}

static void pwmWrite(uint8_t zone, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PUMP_PINS[zone], duty);
#else
  ledcWrite(zone, duty);
#endif
}
#endif

static const uint8_t PUMP_OFF_LEVEL = PUMP_ACTIVE_HIGH ? LOW : HIGH;

void begin() {
  // Pumps first: drive every trigger pin to "off" as early as possible.
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    pinMode(PUMP_PINS[z], OUTPUT);
    digitalWrite(PUMP_PINS[z], PUMP_OFF_LEVEL);
  }
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    analogSetPinAttenuation(SENSOR_PINS[z], ADC_11db);  // full 0-3.1 V range
  }
#if SENSOR_POWER_PIN >= 0
  pinMode(SENSOR_POWER_PIN, OUTPUT);
  digitalWrite(SENSOR_POWER_PIN, LOW);
#endif
#if STATUS_LED_PIN >= 0
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
#endif
}

// ------------------------------------------------------------ sensors -------
void sensorPower(bool on) {
#if SENSOR_POWER_PIN >= 0
  digitalWrite(SENSOR_POWER_PIN, on ? HIGH : LOW);
  if (on) delay(SENSOR_WARMUP_MS);  // the sensor's output needs a moment to settle
#else
  (void)on;
#endif
}

SensorSample readSensor(uint8_t zone) {
  uint16_t samples[SENSOR_SAMPLES];
  for (uint8_t i = 0; i < SENSOR_SAMPLES; i++) {
    samples[i] = (uint16_t)analogReadMilliVolts(SENSOR_PINS[zone]);  // factory-calibrated mV
    delayMicroseconds(200);  // spread the samples out a little
  }
  moisture::Stats st = moisture::trimmedStats(samples, SENSOR_SAMPLES);
  SensorSample out;
  out.meanMv = st.meanMv;
  out.spreadMv = st.spreadMv;
  return out;
}

// --------------------------------------------------------- tank switch ------
bool TankMonitor::readRaw() const {
#if TANK_SWITCH_PIN >= 0
  const bool closed = digitalRead(TANK_SWITCH_PIN) == LOW;  // closed switch pulls the pin to GND
  return TANK_SWITCH_CLOSED_MEANS_WATER ? closed : !closed;
#else
  return true;
#endif
}

void TankMonitor::begin() {
#if TANK_SWITCH_PIN >= 0
  pinMode(TANK_SWITCH_PIN, INPUT_PULLUP);
  delay(5);
#endif
  stable_ = readRaw();
  lastRaw_ = stable_;
  changedAtMs_ = millis();
}

void TankMonitor::update(uint32_t nowMs) {
  const bool raw = readRaw();
  if (raw != lastRaw_) {
    lastRaw_ = raw;
    changedAtMs_ = nowMs;
  }
  // "Empty" is believed after 1.5 s; "refilled" only after 30 s of steady
  // readings, so water sloshing around the float can't toggle the pump.
  const uint32_t needMs = raw ? 30000UL : 1500UL;
  if (raw != stable_ && nowMs - changedAtMs_ >= needMs) stable_ = raw;
}

// --------------------------------------------------------------- pumps ------
void PumpDriver::begin() {
#if PUMP_DRIVER == PUMP_DRIVER_MOSFET
  for (uint8_t z = 0; z < ZONE_COUNT; z++) pwmAttach(z);
#endif
  for (uint8_t z = 0; z < ZONE_COUNT; z++) output(z, 0.0f);
  running_ = false;
}

void PumpDriver::output(uint8_t zone, float level) {
#if PUMP_DRIVER == PUMP_DRIVER_MOSFET
  const uint32_t maxDuty = (1UL << PUMP_PWM_BITS) - 1;
  uint32_t duty = (uint32_t)(level * (float)maxDuty * (float)PUMP_MAX_DUTY_PERCENT / 100.0f + 0.5f);
  if (duty > maxDuty) duty = maxDuty;
  if (!PUMP_ACTIVE_HIGH) duty = maxDuty - duty;
  pwmWrite(zone, duty);
#else
  const bool on = level > 0.0f;
  digitalWrite(PUMP_PINS[zone], on ? (PUMP_ACTIVE_HIGH ? HIGH : LOW) : PUMP_OFF_LEVEL);
#endif
}

bool PumpDriver::start(uint8_t zone, uint32_t durationMs, PumpOwner owner, uint32_t nowMs) {
  if (running_ || zone >= ZONE_COUNT || durationMs == 0) return false;
  if (durationMs > PUMP_MAX_RUN_MS) durationMs = PUMP_MAX_RUN_MS;
  running_ = true;
  zone_ = zone;
  owner_ = owner;
  startMs_ = nowMs;
  durationMs_ = durationMs;
  level_ = -1.0f;  // forces the first update() to write the output
  update(nowMs);
  return true;
}

void PumpDriver::update(uint32_t nowMs) {
  if (!running_) return;
  const uint32_t elapsed = nowMs - startMs_;
  if (elapsed >= durationMs_ || elapsed >= PUMP_MAX_RUN_MS) {
    finish(nowMs, true);
    return;
  }
  float level = 1.0f;
#if PUMP_DRIVER == PUMP_DRIVER_MOSFET
  if (PUMP_SOFT_START_MS > 0 && elapsed < PUMP_SOFT_START_MS) {
    level = 0.25f + 0.75f * (float)elapsed / (float)PUMP_SOFT_START_MS;  // 25 % -> 100 %
  }
#endif
  if (level - level_ > 0.02f || level_ < 0.0f || level >= 1.0f) {
    if (level != level_) output(zone_, level);
    level_ = level;
  }
}

void PumpDriver::stop(uint32_t nowMs) {
  if (running_) finish(nowMs, false);
}

void PumpDriver::finish(uint32_t nowMs, bool completed) {
  output(zone_, 0.0f);
  running_ = false;
  finished_.zone = zone_;
  finished_.owner = owner_;
  finished_.ranMs = nowMs - startMs_;
  finished_.completed = completed;
  hasFinished_ = true;
  owner_ = PumpOwner::None;
  level_ = 0.0f;
}

bool PumpDriver::takeFinished(PumpRun& out) {
  if (!hasFinished_) return false;
  hasFinished_ = false;
  out = finished_;
  return true;
}

// The soft-start ramp delivers about half the water of full speed, so half of
// it is counted as "lost" time when converting.
static const float RAMP_LOSS_MS =
    (PUMP_DRIVER == PUMP_DRIVER_MOSFET) ? PUMP_SOFT_START_MS * 0.4f : 0.0f;

uint32_t pumpMsForMl(float ml, float flowMlPerSec) {
  if (flowMlPerSec < 0.1f) flowMlPerSec = 0.1f;
  float ms = ml / flowMlPerSec * 1000.0f + RAMP_LOSS_MS;
  if (ms < 100.0f) ms = 100.0f;
  if (ms > (float)PUMP_MAX_RUN_MS) ms = (float)PUMP_MAX_RUN_MS;
  return (uint32_t)ms;
}

float pumpMlForMs(uint32_t ms, float flowMlPerSec) {
  float effective = (float)ms - RAMP_LOSS_MS;
  if (effective < 0.0f) effective = 0.0f;
  return effective / 1000.0f * flowMlPerSec;
}

// ---------------------------------------------------------- status LED ------
void ledShow(uint32_t nowMs, LedPattern pattern) {
#if STATUS_LED_PIN >= 0
  bool on = false;
  switch (pattern) {
    case LedPattern::Off: on = false; break;
    case LedPattern::Pumping: on = true; break;
    case LedPattern::Alert: on = (nowMs / 250) % 2 == 0; break;  // fast blink
    case LedPattern::SetupMode: {                                // double blink
      const uint32_t t = nowMs % 2000;
      on = t < 100 || (t >= 250 && t < 350);
      break;
    }
    case LedPattern::Heartbeat: on = (nowMs % 3000) < 60; break;  // short blip
  }
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#else
  (void)nowMs;
  (void)pattern;
#endif
}

// -------------------------------------------------------- BOOT button -------
void Button::begin() {
#if BUTTON_PIN >= 0
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif
}

void Button::update(uint32_t nowMs) {
#if BUTTON_PIN >= 0
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed && !down_) {
    down_ = true;
    longFired_ = false;
    downAtMs_ = nowMs;
  } else if (pressed && down_ && !longFired_ && nowMs - downAtMs_ >= 3000) {
    longFired_ = true;
    longPending_ = true;
  } else if (!pressed && down_) {
    down_ = false;
    if (!longFired_ && nowMs - downAtMs_ >= 40) shortPending_ = true;
  }
#else
  (void)nowMs;
#endif
}

bool Button::takeShortPress() {
  bool p = shortPending_;
  shortPending_ = false;
  return p;
}

bool Button::takeLongPress() {
  bool p = longPending_;
  longPending_ = false;
  return p;
}

}  // namespace hw
