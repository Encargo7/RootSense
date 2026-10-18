#include "PlantNetwork.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "PlantApp.h"
#include "PlantConfig.h"

namespace net {

static bool s_haveCreds = false;
static bool s_apOn = false;
static bool s_wasConnected = false;
static bool s_servicesStarted = false;
static uint32_t s_beginMs = 0;
static uint32_t s_connectedAtMs = 0;
static uint32_t s_lastRetryMs = 0;
static String s_apName;
static String s_ssid;
static String s_pass;
static DNSServer s_dns;  // answers every name with our own address while the hotspot is on,
                         // so phones pop up the dashboard by themselves ("captive portal")

static void startAccessPoint() {
  WiFi.mode(s_haveCreds ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(s_apName.c_str(), SETUP_AP_PASSWORD);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_apOn = true;
  logEvent("Setup hotspot \"%s\" is on (password \"%s\"). Join it and open http://%s",
           s_apName.c_str(), SETUP_AP_PASSWORD, WiFi.softAPIP().toString().c_str());
}

void begin(const char* ssid, const char* password) {
  // Name the hotspot after the end of this board's MAC address, e.g. RootSense-3F2A.
  const uint64_t mac = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "RootSense-%02X%02X", (unsigned)((mac >> 32) & 0xFF),
           (unsigned)((mac >> 40) & 0xFF));
  s_apName = name;
  s_ssid = ssid ? ssid : "";
  s_pass = password ? password : "";
  s_haveCreds = s_ssid.length() > 0;

  WiFi.persistent(false);  // we keep the credentials in our own settings
  WiFi.setHostname(DEVICE_HOSTNAME);
  if (s_haveCreds) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    s_beginMs = millis();
    s_lastRetryMs = s_beginMs;
    logEvent("Connecting to Wi-Fi \"%s\"...", s_ssid.c_str());
  } else {
    logEvent("No Wi-Fi saved yet.");
    startAccessPoint();
  }
}

void update(uint32_t nowMs) {
  if (s_apOn) s_dns.processNextRequest();
  const bool isConnected = s_haveCreds && WiFi.status() == WL_CONNECTED;

  if (isConnected && !s_wasConnected) {
    s_wasConnected = true;
    s_connectedAtMs = nowMs;
    logEvent("Wi-Fi connected. Dashboard: http://%s  or  http://%s.local",
             WiFi.localIP().toString().c_str(), DEVICE_HOSTNAME);
    if (!s_servicesStarted) {
      s_servicesStarted = true;
      if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
      configTzTime(TIMEZONE_POSIX, NTP_SERVER_1, NTP_SERVER_2);  // sets the clock in the background
    }
  } else if (!isConnected && s_wasConnected) {
    s_wasConnected = false;
    logEvent("Wi-Fi connection lost - reconnecting automatically.");
  }

  if (s_haveCreds && !isConnected) {
    // Couldn't connect in time: open the setup hotspot so the user can fix it.
    if (!s_apOn && nowMs - s_beginMs >= WIFI_CONNECT_TIMEOUT_MS) {
      logEvent("Could not join \"%s\" (wrong password or out of range?).", s_ssid.c_str());
      startAccessPoint();
    }
    // Keep retrying every 2 minutes (the router may simply have been off).
    if (nowMs - s_lastRetryMs >= 120000UL) {
      s_lastRetryMs = nowMs;
      WiFi.begin(s_ssid.c_str(), s_pass.c_str());
    }
  }

  // Connected for 3 minutes and nobody is using the hotspot -> switch it off.
  if (s_apOn && isConnected && nowMs - s_connectedAtMs >= 180000UL && WiFi.softAPgetStationNum() == 0) {
    s_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_apOn = false;
    logEvent("Setup hotspot switched off (home Wi-Fi is working).");
  }
}

bool connected() { return s_haveCreds && WiFi.status() == WL_CONNECTED; }
bool apActive() { return s_apOn; }
String apName() { return s_apName; }
String ssid() { return s_ssid; }
int rssi() { return connected() ? WiFi.RSSI() : 0; }

String ip() {
  if (connected()) return WiFi.localIP().toString();
  if (s_apOn) return WiFi.softAPIP().toString();
  return String("");
}

bool timeValid() { return time(nullptr) > 1700000000; }  // later than Nov 2023 = really set

bool localTime(struct tm& out) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  localtime_r(&now, &out);
  return true;
}

}  // namespace net
