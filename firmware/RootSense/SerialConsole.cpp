#include "SerialConsole.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "PlantApp.h"
#include "PlantNetwork.h"

namespace console {

static char s_line[192];
static size_t s_len = 0;
static uint32_t s_lastCharMs = 0;

// Splits a line into words. "Double quotes" keep spaces together,
// e.g.  wifi "My Home WiFi" secret123
static int tokenize(char* line, char* argv[], int maxArgs) {
  int argc = 0;
  char* p = line;
  while (*p != '\0' && argc < maxArgs) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    if (*p == '"') {
      p++;
      argv[argc++] = p;
      while (*p != '\0' && *p != '"') p++;
    } else {
      argv[argc++] = p;
      while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    }
    if (*p != '\0') *p++ = '\0';
  }
  return argc;
}

static bool parseZone(const char* s, uint8_t& z) {
  const long v = strtol(s, nullptr, 10);
  if (v < 1 || v > ZONE_COUNT) {
    Serial.printf("Zone must be a number from 1 to %d.\n", ZONE_COUNT);
    return false;
  }
  z = (uint8_t)(v - 1);
  return true;
}

static void reply(bool ok, const String& msg) {
  if (ok) {
    Serial.println(msg);
  } else {
    Serial.print(F("Error: "));
    Serial.println(msg);
  }
}

static void printHelp() {
  Serial.println(F(
      "\nCommands (zone numbers start at 1):\n"
      "  status                      show every zone (or short-press BOOT)\n"
      "  plants                      list the plant database\n"
      "  plant <zone> <key>          choose the plant, e.g.  plant 1 pothos\n"
      "  on <zone> | off <zone>      automatic watering on / off\n"
      "  pot <zone> <ml>             soil volume of the pot, e.g.  pot 1 1500\n"
      "  dry <zone>                  calibrate: sensor clean and dry, held in the air\n"
      "  wet <zone>                  calibrate: sensor in freshly watered soil\n"
      "  pumptest <zone> [sec]       run the pump (default 10 s) into a measuring cup\n"
      "  flow <zone> <ml> [sec]      enter what came out during the pump test\n"
      "  water <zone> <ml>           water now, e.g.  water 1 50\n"
      "  stop                        stop the pump immediately\n"
      "  clear [zone]                clear safety alerts (or hold BOOT for 3 s)\n"
      "  soak <zone> <min>           wait after each pulse        (default 10)\n"
      "  rest <zone> <min>           rest after a watering cycle  (default 60)\n"
      "  limit <zone> <ml>           max water per 24 h\n"
      "  pulses <zone> <n>           max pulses per watering cycle (default 5)\n"
      "  quiet off | quiet <start> <end>   e.g.  quiet 22 7  (needs Wi-Fi for the clock)\n"
      "  wifi \"<name>\" \"<password>\"   join your Wi-Fi (the board restarts)\n"
      "  log                         recent events\n"
      "  restart  |  factory-reset yes\n"));
}

static void printPlants() {
  const char* group = "";
  for (size_t i = 0; i < plants::count(); i++) {
    const PlantProfile& p = plants::at(i);
    if (strcmp(p.group, group) != 0) {
      group = p.group;
      Serial.printf("\n%s\n", group);
    }
    Serial.printf("  %-14s %-30s waters below %3u%%, up to %3u%%\n", p.key, p.name, (unsigned)p.waterBelowPct,
                  (unsigned)p.targetPct);
  }
  Serial.println(F("\nChoose one with:  plant <zone> <key>"));
}

static void printLog() {
  Serial.println(F("\nRecent events:"));
  for (uint8_t i = 0; i < app.events.size(); i++) {
    const EventLog::Entry& e = app.events.get(i);
    Serial.print(F("  ["));
    Serial.print(formatEventTime(e));
    Serial.print(F("] "));
    Serial.println(e.text);
  }
}

void printStatus() {
  const uint32_t now = millis();
  Serial.println(F("\n------------------------------ status ------------------------------"));
  Serial.printf("Up %lu min | tank: %s | Wi-Fi: %s | dashboard: %s\n", (unsigned long)((now - app.bootMs) / 60000UL),
                !app.tank.configured() ? "no float switch" : (app.tank.hasWater() ? "water OK" : "EMPTY"),
                net::connected() ? net::ssid().c_str() : (net::apActive() ? "setup hotspot" : "not connected"),
                net::ip().length() ? (String("http://") + net::ip()).c_str() : "-");
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    const ZoneRuntime& r = app.zone[z];
    const ZoneConfig& c = app.cfg.zones[z];
    const PlantProfile& p = *r.ctrl.params().plant;
    const char* level = "";
    const String text = zoneStatusText(z, level);
    Serial.printf("Zone %u - %s (%s)\n", (unsigned)(z + 1), p.name, c.enabled ? "automatic" : "off");
    if (zoneSensorOk(z)) {
      Serial.printf("  moisture %.0f%%  (sensor %.0f mV)   waters below %u%%, up to %u%%\n", r.moisture, r.rawMv,
                    (unsigned)p.waterBelowPct, (unsigned)p.targetPct);
    } else {
      Serial.printf("  sensor: %s (%.0f mV on GPIO %u)\n", moisture::healthName(r.health), r.rawMv,
                    (unsigned)SENSOR_PINS[z]);
    }
    Serial.printf("  %s\n", text.c_str());
    Serial.printf("  last 24 h: %.0f of %u ml | flow %.1f ml/s | calibrated: %s\n", r.ctrl.mlInLast24h(now),
                  (unsigned)c.dailyLimitMl, c.flowMlPerSec,
                  (c.dryCalibrated && c.wetCalibrated) ? "yes" : (c.dryCalibrated || c.wetCalibrated ? "half" : "no"));
  }
  Serial.println(F("--------------------------------------------------------------------"));
}

static void execute(char* line) {
  // Echo the command (but never the Wi-Fi password).
  Serial.print(F("> "));
  Serial.println(strncmp(line, "wifi", 4) == 0 ? "wifi ********" : line);

  char* argv[6];
  const int argc = tokenize(line, argv, 6);
  if (argc == 0) return;
  String cmd(argv[0]);
  cmd.toLowerCase();
  String msg;
  uint8_t z = 0;
  const bool hasZone = argc >= 2;

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "status" || cmd == "s") {
    printStatus();
  } else if (cmd == "plants") {
    printPlants();
  } else if (cmd == "log") {
    printLog();
  } else if (cmd == "stop") {
    reply(actionStopPumps(msg), msg);
  } else if (cmd == "clear") {
    if (!hasZone) {
      reply(actionClear(-1, msg), msg);
    } else if (parseZone(argv[1], z)) {
      reply(actionClear(z, msg), msg);
    }
  } else if (cmd == "restart") {
    Serial.println(F("Restarting..."));
    actionRestart(300);
  } else if (cmd == "factory-reset") {
    if (argc >= 2 && strcmp(argv[1], "yes") == 0) {
      reply(actionFactoryReset(msg), msg);
    } else {
      Serial.println(F("This erases every setting (plants, calibration, Wi-Fi). Type:  factory-reset yes"));
    }
  } else if (cmd == "quiet") {
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
      reply(actionSetQuietHours(false, app.cfg.quietStartHour, app.cfg.quietEndHour, msg), msg);
    } else if (argc >= 3) {
      reply(actionSetQuietHours(true, atol(argv[1]), atol(argv[2]), msg), msg);
    } else {
      Serial.println(F("Usage:  quiet off   or   quiet <start hour> <end hour>   e.g.  quiet 22 7"));
    }
  } else if (cmd == "wifi") {
    if (argc < 2) {
      Serial.println(F("Usage:  wifi \"<name>\" \"<password>\""));
    } else {
      reply(actionSetWifi(argv[1], argc >= 3 ? argv[2] : "", msg), msg);
    }
  } else if (cmd == "plant" || cmd == "on" || cmd == "off" || cmd == "pot" || cmd == "dry" || cmd == "wet" ||
             cmd == "pumptest" || cmd == "flow" || cmd == "water" || cmd == "soak" || cmd == "rest" ||
             cmd == "limit" || cmd == "pulses") {
    // Every command below needs a zone number.
    if (!hasZone) {
      Serial.println(F("Which zone? e.g.  water 1 50   (type 'help' for the list)"));
      return;
    }
    if (!parseZone(argv[1], z)) return;
    const bool hasValue = argc >= 3;
    if (cmd == "on" || cmd == "off") {
      reply(actionSetEnabled(z, cmd == "on", msg), msg);
    } else if (cmd == "dry" || cmd == "wet") {
      reply(actionCalibrate(z, cmd == "dry", msg), msg);
    } else if (cmd == "pumptest") {
      reply(actionPumpTest(z, hasValue ? atol(argv[2]) : 10, msg), msg);
    } else if (!hasValue) {
      Serial.println(F("That command needs a value - type 'help'."));
    } else if (cmd == "plant") {
      reply(actionSetPlant(z, argv[2], msg), msg);
    } else if (cmd == "pot") {
      reply(actionSetPot(z, atol(argv[2]), msg), msg);
    } else if (cmd == "flow") {
      reply(actionSetFlow(z, (float)atof(argv[2]), argc >= 4 ? (float)atof(argv[3]) : 10.0f, msg), msg);
    } else if (cmd == "water") {
      reply(actionWaterNow(z, atol(argv[2]), msg), msg);
    } else if (cmd == "soak") {
      reply(actionSetAdvanced(z, atol(argv[2]), -1, -1, -1, msg), msg);
    } else if (cmd == "rest") {
      reply(actionSetAdvanced(z, -1, atol(argv[2]), -1, -1, msg), msg);
    } else if (cmd == "limit") {
      reply(actionSetAdvanced(z, -1, -1, atol(argv[2]), -1, msg), msg);
    } else if (cmd == "pulses") {
      reply(actionSetAdvanced(z, -1, -1, -1, atol(argv[2]), msg), msg);
    }
  } else {
    Serial.printf("Unknown command '%s' - type 'help'.\n", argv[0]);
  }
}

void begin() { Serial.println(F("Serial Monitor ready (115200 baud). Type 'help' and press Enter.")); }

void poll() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    s_lastCharMs = millis();
    if (c == '\r' || c == '\n') {
      if (s_len > 0) {
        s_line[s_len] = '\0';
        execute(s_line);
        s_len = 0;
      }
    } else if (s_len < sizeof(s_line) - 1) {
      s_line[s_len++] = (char)c;
    }
  }
  // Serial Monitor set to "No line ending"? Run the command after a short pause.
  if (s_len > 0 && millis() - s_lastCharMs > 1500UL) {
    s_line[s_len] = '\0';
    execute(s_line);
    s_len = 0;
  }
}

}  // namespace console
