#include "WebDashboard.h"

#include <WebServer.h>

#include "PlantApp.h"
#include "DashboardPage.h"
#include "PlantNetwork.h"

namespace web {

static WebServer server(80);

// ------------------------------------------------------- tiny JSON writer ---
class JsonOut {
 public:
  explicit JsonOut(String& out) : s_(out), depth_(0) { first_[0] = true; }
  void openObject(const char* key = nullptr) {
    sep(key);
    s_ += '{';
    push();
  }
  void closeObject() {
    pop();
    s_ += '}';
  }
  void openArray(const char* key = nullptr) {
    sep(key);
    s_ += '[';
    push();
  }
  void closeArray() {
    pop();
    s_ += ']';
  }
  void str(const char* key, const char* v) {
    sep(key);
    quote(v);
  }
  void str(const char* key, const String& v) { str(key, v.c_str()); }
  void num(const char* key, double v, unsigned decimals = 1) {
    sep(key);
    if (!(v == v) || v > 1e300 || v < -1e300) {  // NaN or infinity -> null
      s_ += "null";
    } else {
      s_ += String(v, decimals);
    }
  }
  void integer(const char* key, long v) {
    sep(key);
    s_ += String(v);
  }
  void boolean(const char* key, bool v) {
    sep(key);
    s_ += v ? "true" : "false";
  }
  void null(const char* key) {
    sep(key);
    s_ += "null";
  }

 private:
  void sep(const char* key) {
    if (!first_[depth_]) s_ += ',';
    first_[depth_] = false;
    if (key != nullptr) {
      quote(key);
      s_ += ':';
    }
  }
  void push() {
    if (depth_ < 7) depth_++;
    first_[depth_] = true;
  }
  void pop() {
    if (depth_ > 0) depth_--;
  }
  void quote(const char* v) {
    s_ += '"';
    for (const char* p = v ? v : ""; *p != '\0'; p++) {
      const char c = *p;
      switch (c) {
        case '"': s_ += "\\\""; break;
        case '\\': s_ += "\\\\"; break;
        case '\n': s_ += "\\n"; break;
        case '\r': s_ += "\\r"; break;
        case '\t': s_ += "\\t"; break;
        default:
          if ((uint8_t)c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)(uint8_t)c);
            s_ += buf;
          } else {
            s_ += c;
          }
      }
    }
    s_ += '"';
  }
  String& s_;
  uint8_t depth_;
  bool first_[8];
};

// ---------------------------------------------------------------- helpers ---
static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static void sendResult(bool ok, const String& msg) {
  String out;
  JsonOut j(out);
  j.openObject();
  j.boolean("ok", ok);
  j.str(ok ? "msg" : "error", msg);
  j.closeObject();
  sendJson(ok ? 200 : 400, out);
}

// Reads the "z" argument (1-based on the web) into a 0-based zone index.
static bool zoneArg(uint8_t& z) {
  const long v = server.hasArg("z") ? server.arg("z").toInt() : 0;
  if (v < 1 || v > ZONE_COUNT) {
    sendResult(false, String("Unknown zone - use 1 to ") + String(ZONE_COUNT) + ".");
    return false;
  }
  z = (uint8_t)(v - 1);
  return true;
}

static long longArg(const char* name, long fallback) {
  if (!server.hasArg(name)) return fallback;
  const String v = server.arg(name);
  if (v.length() == 0) return fallback;
  return v.toInt();
}

// --------------------------------------------------------------- handlers ---
static void handleRoot() {
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

static void handleStatus() {
  const uint32_t now = millis();
  String out;
  out.reserve(1400 + 900 * ZONE_COUNT);
  JsonOut j(out);
  j.openObject();
  j.integer("uptime", (long)((now - app.bootMs) / 1000UL));
  struct tm t;
  if (net::localTime(t)) {
    char buf[24];
    strftime(buf, sizeof(buf), "%a %H:%M", &t);
    j.str("time", buf);
  } else {
    j.null("time");
  }

  j.openObject("wifi");
  j.boolean("connected", net::connected());
  j.boolean("ap", net::apActive());
  j.str("apName", net::apName());
  j.str("ssid", net::ssid());
  j.str("saved", app.cfg.wifiSsid);
  j.str("ip", net::ip());
  j.integer("rssi", net::rssi());
  j.closeObject();

  j.openObject("tank");
  j.boolean("present", app.tank.configured());
  j.boolean("water", app.tank.hasWater());
  j.closeObject();

  j.openObject("pump");
  j.boolean("running", app.pump.running());
  j.integer("zone", app.pump.running() ? app.pump.zone() + 1 : 0);
  j.str("kind", app.pump.owner() == hw::PumpOwner::Test ? "test" : "water");
  j.closeObject();

  j.openObject("quiet");
  j.boolean("enabled", app.cfg.quietEnabled != 0);
  j.integer("start", app.cfg.quietStartHour);
  j.integer("end", app.cfg.quietEndHour);
  j.boolean("now", quietHoursNow());
  j.closeObject();

  j.openArray("zones");
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    const ZoneRuntime& r = app.zone[z];
    const ZoneConfig& c = app.cfg.zones[z];
    const WateringController& w = r.ctrl;
    const PlantProfile& p = *w.params().plant;
    const char* level = "ok";
    const String status = zoneStatusText(z, level);
    j.openObject();
    j.integer("n", z + 1);
    j.boolean("enabled", c.enabled != 0);
    j.str("plant", p.key);
    j.str("plantName", p.name);
    j.str("tip", p.tip);
    j.integer("below", p.waterBelowPct);
    j.integer("target", p.targetPct);
    j.integer("soggy", p.soggyAbovePct);
    if (zoneSensorOk(z)) {
      j.num("moisture", r.moisture, 1);
    } else {
      j.null("moisture");
    }
    j.num("mv", r.rawMv, 0);
    j.num("spread", r.spreadMv, 0);
    j.str("health", moisture::healthName(r.health));
    j.boolean("calDry", c.dryCalibrated != 0);
    j.boolean("calWet", c.wetCalibrated != 0);
    j.integer("dryMv", c.dryMv);
    j.integer("wetMv", c.wetMv);
    j.str("state", zoneStateName(w.state()));
    j.str("fault", zoneFaultName(w.fault()));
    j.str("wait", waitReasonName(w.waitReason()));
    j.str("last", cycleResultName(w.lastResult()));
    j.str("status", status);
    j.str("level", level);
    j.num("ml24h", w.mlInLast24h(now), 0);
    j.integer("limit", c.dailyLimitMl);
    if (w.hasWatered()) {
      j.integer("lastWateredAgo", (long)((now - w.lastWateredMs()) / 1000UL));
    } else {
      j.null("lastWateredAgo");
    }
    j.num("flow", c.flowMlPerSec, 1);
    j.num("mlPerPct", w.params().mlPerPercent, 2);
    j.integer("pot", c.potMl);
    j.integer("soak", c.soakMin);
    j.integer("cooldown", c.cooldownMin);
    j.integer("maxPulses", c.maxPulses);
    j.integer("pulses", w.pulsesThisCycle());
    j.closeObject();
  }
  j.closeArray();

  j.openArray("events");  // newest first, at most 15
  const uint8_t n = app.events.size();
  for (uint8_t k = 0; k < n && k < 15; k++) {
    const EventLog::Entry& e = app.events.get((uint8_t)(n - 1 - k));
    j.openObject();
    j.str("t", formatEventTime(e));
    j.str("msg", e.text);
    j.closeObject();
  }
  j.closeArray();
  j.closeObject();
  sendJson(200, out);
}

static void handlePlants() {
  String out;
  out.reserve((unsigned int)(160 * plants::count()));
  JsonOut j(out);
  j.openArray();
  for (size_t i = 0; i < plants::count(); i++) {
    const PlantProfile& p = plants::at(i);
    j.openObject();
    j.str("key", p.key);
    j.str("name", p.name);
    j.str("group", p.group);
    j.integer("below", p.waterBelowPct);
    j.integer("target", p.targetPct);
    j.integer("soggy", p.soggyAbovePct);
    j.str("tip", p.tip);
    j.closeObject();
  }
  j.closeArray();
  sendJson(200, out);
}

static void handleHistory() {
  String out;
  out.reserve(64 + ZONE_COUNT * MoistureHistory::SIZE * 4);
  JsonOut j(out);
  j.openObject();
  j.integer("intervalMin", (long)(HISTORY_INTERVAL_MS / 60000UL));
  j.integer("size", MoistureHistory::SIZE);
  j.openArray("zones");
  for (uint8_t z = 0; z < ZONE_COUNT; z++) {
    const MoistureHistory& h = app.zone[z].history;
    j.openArray();
    for (uint16_t i = 0; i < h.size(); i++) {
      const uint8_t v = h.get(i);
      if (v == MoistureHistory::NO_DATA) {
        j.null(nullptr);
      } else {
        j.integer(nullptr, v);
      }
    }
    j.closeArray();
  }
  j.closeArray();
  j.closeObject();
  sendJson(200, out);
}

static void handleZone() {
  uint8_t z;
  if (!zoneArg(z)) return;
  String msg, all;
  bool ok = true;
  bool any = false;
  if (server.hasArg("enabled")) {
    any = true;
    ok = actionSetEnabled(z, server.arg("enabled").toInt() != 0, msg) && ok;
    all += msg + " ";
  }
  if (ok && server.hasArg("plant")) {
    any = true;
    ok = actionSetPlant(z, server.arg("plant").c_str(), msg) && ok;
    all += msg + " ";
  }
  if (ok && server.hasArg("pot") && longArg("pot", -1) != (long)app.cfg.zones[z].potMl) {
    any = true;
    ok = actionSetPot(z, longArg("pot", -1), msg) && ok;
    all += msg + " ";
  }
  if (ok && (server.hasArg("soak") || server.hasArg("cooldown") || server.hasArg("limit") || server.hasArg("pulses"))) {
    any = true;
    ok = actionSetAdvanced(z, longArg("soak", -1), longArg("cooldown", -1), longArg("limit", -1),
                           longArg("pulses", -1), msg) && ok;
    all += msg + " ";
  }
  if (!any) {
    sendResult(true, "Nothing changed.");
    return;
  }
  all.trim();
  sendResult(ok, ok ? all : msg);
}

static void handleWater() {
  uint8_t z;
  if (!zoneArg(z)) return;
  String msg;
  const bool ok = actionWaterNow(z, longArg("ml", 0), msg);
  sendResult(ok, msg);
}

static void handlePumpTest() {
  uint8_t z;
  if (!zoneArg(z)) return;
  String msg;
  const bool ok = actionPumpTest(z, longArg("s", 10), msg);
  sendResult(ok, msg);
}

static void handleFlow() {
  uint8_t z;
  if (!zoneArg(z)) return;
  String msg;
  const float ml = server.hasArg("ml") ? server.arg("ml").toFloat() : 0.0f;
  const float s = server.hasArg("s") ? server.arg("s").toFloat() : 10.0f;
  const bool ok = actionSetFlow(z, ml, s, msg);
  sendResult(ok, msg);
}

static void handleCalibrate() {
  uint8_t z;
  if (!zoneArg(z)) return;
  const String point = server.arg("point");
  if (point != "dry" && point != "wet") {
    sendResult(false, "point must be 'dry' or 'wet'.");
    return;
  }
  String msg;
  const bool ok = actionCalibrate(z, point == "dry", msg);
  sendResult(ok, msg);
}

static void handleClear() {
  String msg;
  bool ok;
  if (server.hasArg("z") && server.arg("z").length() > 0) {
    uint8_t z;
    if (!zoneArg(z)) return;
    ok = actionClear(z, msg);
  } else {
    ok = actionClear(-1, msg);
  }
  sendResult(ok, msg);
}

static void handleStop() {
  String msg;
  const bool ok = actionStopPumps(msg);
  sendResult(ok, msg);
}

static void handleQuiet() {
  String msg;
  const bool ok = actionSetQuietHours(longArg("enabled", 0) != 0, longArg("start", app.cfg.quietStartHour),
                                      longArg("end", app.cfg.quietEndHour), msg);
  sendResult(ok, msg);
}

static void handleWifi() {
  String msg;
  const String ssid = server.arg("ssid");
  const String pass = server.arg("pass");
  const bool ok = actionSetWifi(ssid.c_str(), pass.c_str(), msg);
  sendResult(ok, msg);
}

static void handleRestart() {
  actionRestart(1000);
  sendResult(true, "Restarting - the page will reconnect in a few seconds.");
}

static void handleNotFound() {
  // In hotspot mode phones probe random URLs to detect a "captive portal";
  // sending them to the dashboard makes the setup page pop up by itself.
  if (server.method() == HTTP_GET && !server.uri().startsWith("/api/")) {
    // client().localIP() is our address on the network the phone is using
    // (hotspot or home Wi-Fi), so the redirect always points somewhere reachable.
    server.sendHeader("Location", String("http://") + server.client().localIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  sendResult(false, String("No such page: ") + server.uri());
}

void begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/plants", HTTP_GET, handlePlants);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/zone", HTTP_POST, handleZone);
  server.on("/api/water", HTTP_POST, handleWater);
  server.on("/api/pumptest", HTTP_POST, handlePumpTest);
  server.on("/api/flow", HTTP_POST, handleFlow);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.on("/api/clear", HTTP_POST, handleClear);
  server.on("/api/stop", HTTP_POST, handleStop);
  server.on("/api/quiet", HTTP_POST, handleQuiet);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();
}

void handle() { server.handleClient(); }

}  // namespace web
