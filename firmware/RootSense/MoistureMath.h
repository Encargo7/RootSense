#pragma once
// =============================================================================
//  MoistureMath — turning raw sensor voltages into a trustworthy percentage.
//  Pure functions (no hardware), so they are unit-tested on a PC.
// =============================================================================
#include <stddef.h>
#include <stdint.h>

namespace moisture {

enum class SensorHealth : uint8_t {
  Ok,
  Disconnected,  // reads ~0 V: unplugged, broken wire, or no power
  OverRange,     // reads above ~3 V: sensor powered from 5 V without its regulator?
  Unstable       // samples jump around: loose wire or floating pin
};

inline const char* healthName(SensorHealth h) {
  switch (h) {
    case SensorHealth::Ok: return "ok";
    case SensorHealth::Disconnected: return "disconnected";
    case SensorHealth::OverRange: return "over-range";
    case SensorHealth::Unstable: return "unstable";
  }
  return "?";
}

struct Stats {
  float meanMv;    // average of the middle half of the samples
  float spreadMv;  // difference between the 75th and 25th percentile
};

// Sorts the samples in place, throws away the lowest and highest quarter
// (Wi-Fi noise spikes) and averages the rest.
inline Stats trimmedStats(uint16_t* s, size_t n) {
  Stats st = {0.0f, 0.0f};
  if (n == 0) return st;
  for (size_t i = 1; i < n; i++) {  // insertion sort - n is small
    uint16_t v = s[i];
    size_t j = i;
    while (j > 0 && s[j - 1] > v) {
      s[j] = s[j - 1];
      j--;
    }
    s[j] = v;
  }
  size_t lo = n / 4;
  size_t hi = n - n / 4;  // exclusive
  if (hi <= lo) {
    lo = 0;
    hi = n;
  }
  uint32_t sum = 0;
  for (size_t i = lo; i < hi; i++) sum += s[i];
  st.meanMv = (float)sum / (float)(hi - lo);
  st.spreadMv = (float)s[hi - 1] - (float)s[lo];
  return st;
}

// Capacitive sensors read a HIGHER voltage when the soil is DRIER.
// Returns 0 % at the dry calibration voltage and 100 % at the wet one.
// Not clamped: values slightly outside 0..100 are normal and informative.
inline float toPercent(float mv, float dryMv, float wetMv) {
  float span = dryMv - wetMv;
  if (span < 100.0f) span = 100.0f;  // guards against a bad calibration
  return (dryMv - mv) * 100.0f / span;
}

inline SensorHealth classify(float meanMv, float spreadMv) {
  if (meanMv < 250.0f) return SensorHealth::Disconnected;
  if (meanMv > 3050.0f) return SensorHealth::OverRange;
  if (spreadMv > 250.0f) return SensorHealth::Unstable;
  return SensorHealth::Ok;
}

// Exponential moving average: smooths readings so one odd value never
// triggers the pump. alpha = how much of the new reading to take (0..1).
inline float smooth(float previous, float reading, float alpha) {
  return previous + alpha * (reading - previous);
}

}  // namespace moisture
