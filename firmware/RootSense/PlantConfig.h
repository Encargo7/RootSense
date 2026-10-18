#pragma once
// =============================================================================
//  PlantConfig.h — the hardware settings you are most likely to change.
//
//  Everything else (which plant, pot size, sensor calibration, pump flow,
//  Wi-Fi) is set at runtime from the web dashboard or the Serial Monitor and
//  saved in the ESP32's flash, so you never have to re-upload for those.
//
//  (The #ifndef lines let PlatformIO/CI override a value with -D flags;
//   just edit the number after #define.)
// =============================================================================
#include <stdint.h>

// ---------------------------------------------------------------- Zones -----
// One "zone" = one plant = one soil sensor + one trigger switch + one pump.
#ifndef ZONE_COUNT
#define ZONE_COUNT 2  // 1 to 4. Extra zones start switched OFF until you enable them.
#endif

// Soil-sensor signal (AOUT) pins. These MUST be ADC1 pins (32, 33, 34, 35, 36, 39):
// the ESP32's ADC2 pins stop working whenever Wi-Fi is on.
static const uint8_t SENSOR_PINS[4] = {34, 35, 32, 33};

// Trigger-switch signal (TRIG / PWM / IN) pins, one per pump. These avoid the
// ESP32 "strapping" pins (0, 2, 5, 12, 15) and pins that pulse at boot (1, 3, 14),
// so a pump never twitches while the board is starting up.
static const uint8_t PUMP_PINS[4] = {25, 26, 27, 13};

// ------------------------------------------------------- Water tank ---------
// Float switch in the water tank. Wire it between this pin and GND (no resistor
// needed - the ESP32's internal pull-up is used). Set to -1 if you don't have one
// (NOT recommended: nothing will then stop the pump from running dry).
#ifndef TANK_SWITCH_PIN
#define TANK_SWITCH_PIN 23
#endif
// true  = the switch is CLOSED when there IS water  <- recommended: a broken or
//         unplugged wire then reads as "tank empty", which is the safe failure.
// false = the switch is CLOSED when the tank is EMPTY.
#ifndef TANK_SWITCH_CLOSED_MEANS_WATER
#define TANK_SWITCH_CLOSED_MEANS_WATER true
#endif

// ------------------------------------------------- Other pins ---------------
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 2      // blue on-board LED on most ESP32 dev boards (-1 = none)
#endif
#ifndef BUTTON_PIN
#define BUTTON_PIN 0          // the on-board BOOT button: hold 3 s to clear alerts (-1 = none)
#endif
// Optional: feed the sensors' VCC from this pin so they are only powered while
// measuring (-1 = sensors on 3V3 all the time, which is what the wiring diagram shows).
#ifndef SENSOR_POWER_PIN
#define SENSOR_POWER_PIN -1
#endif

// ------------------------------------------------- Pump driver --------------
#define PUMP_DRIVER_MOSFET 1  // MOSFET "trigger switch" module (D4184 / AOD4184 type)
#define PUMP_DRIVER_RELAY 2   // relay module: plain on/off, no soft start
#ifndef PUMP_DRIVER
#define PUMP_DRIVER PUMP_DRIVER_MOSFET
#endif

#ifndef PUMP_ACTIVE_HIGH
#define PUMP_ACTIVE_HIGH true         // MOSFET modules: true. Most relay boards: false.
#endif
#define PUMP_PWM_FREQ_HZ 1000         // 1 kHz suits both opto-isolated and direct-gate modules
#define PUMP_PWM_BITS 10              // duty resolution (0..1023)
#define PUMP_SOFT_START_MS 300        // ramp the pump up gently -> smaller current spike,
                                      // so the ESP32 doesn't reset from a voltage dip
#ifndef PUMP_MAX_DUTY_PERCENT
#define PUMP_MAX_DUTY_PERCENT 100     // e.g. 85 runs a "3-4.5 V" pump gently from 5 V
#endif
#define PUMP_MAX_RUN_MS 30000UL       // hard safety limit for ONE pump run (30 s)

// ------------------------------------------------- Measuring ----------------
#define SENSOR_READ_INTERVAL_MS 2000UL   // read every sensor every 2 s
#define SENSOR_SAMPLES 32                // ADC samples per reading (spikes are trimmed off)
#define SENSOR_WARMUP_MS 150UL           // only used with SENSOR_POWER_PIN
#define BOOT_SETTLE_MS 120000UL          // no automatic watering for 2 min after power-up
#define HISTORY_INTERVAL_MS 600000UL     // store one moisture point every 10 min (24 h chart)

// ------------------------------------------------- Network ------------------
#define DEVICE_HOSTNAME "rootsense"      // dashboard at http://rootsense.local
#define SETUP_AP_PASSWORD "rootsense"    // password of the "RootSense-XXXX" setup Wi-Fi (8+ chars)
#define WIFI_CONNECT_TIMEOUT_MS 20000UL  // then the setup hotspot turns on
// Your time zone as a POSIX TZ string (used for quiet hours and log timestamps).
// US Eastern: "EST5EDT,M3.2.0,M11.1.0"   US Central: "CST6CDT,M3.2.0,M11.1.0"
// US Pacific: "PST8PDT,M3.2.0,M11.1.0"   UK: "GMT0BST,M3.5.0/1,M10.5.0"
#ifndef TIMEZONE_POSIX
#define TIMEZONE_POSIX "EST5EDT,M3.2.0,M11.1.0"
#endif
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

static_assert(ZONE_COUNT >= 1 && ZONE_COUNT <= 4, "ZONE_COUNT must be 1..4");
