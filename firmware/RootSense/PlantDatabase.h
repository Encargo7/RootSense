#pragma once
// =============================================================================
//  PlantDatabase — how wet each kind of plant likes its soil.
//
//  The moisture scale used everywhere in this project:
//      0 %  = sensor in dry air                         ("DRY" calibration)
//    100 %  = sensor in freshly watered, drained soil    ("WET" calibration)
//
//  For each plant:
//    waterBelowPct  start watering when the soil drops below this
//    targetPct      keep giving small pulses until the soil is back up to this
//    soggyAbovePct  show a "too wet" warning above this
//
//  These are starting points - every pot, soil mix and room is different.
//  Watch the dashboard for a week and nudge the numbers (PlantDatabase.cpp).
// =============================================================================
#include <stddef.h>
#include <stdint.h>

struct PlantProfile {
  const char* key;    // short id saved in flash, e.g. "pothos" (max 15 chars)
  const char* name;   // shown in the dashboard
  const char* group;  // used to group the drop-down list
  uint8_t waterBelowPct;
  uint8_t targetPct;
  uint8_t soggyAbovePct;
  const char* tip;
};

namespace plants {
size_t count();
const PlantProfile& at(size_t index);
const PlantProfile* find(const char* key);  // nullptr if the key is unknown
const PlantProfile& fallback();              // "generic" houseplant
}  // namespace plants
