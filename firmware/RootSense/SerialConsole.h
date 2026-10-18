#pragma once
// =============================================================================
//  SerialConsole — control everything from the Arduino Serial Monitor
//  (115200 baud). Type  help  and press Enter.
// =============================================================================

namespace console {
void begin();
void poll();         // call every loop
void printStatus();  // also printed when you short-press the BOOT button
}  // namespace console
