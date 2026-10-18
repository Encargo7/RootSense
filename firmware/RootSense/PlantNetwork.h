#pragma once
// =============================================================================
//  Network — Wi-Fi without blocking the watering logic.
//
//  * Saved home Wi-Fi?  -> connect, then the dashboard is at http://rootsense.local
//  * No Wi-Fi saved, or it can't connect within 20 s?
//      -> the ESP32 also opens its own hotspot "RootSense-XXXX"
//         (password in PlantConfig.h). Join it and open http://192.168.4.1
//  The plants are watered either way - Wi-Fi is only for the dashboard + clock.
// =============================================================================
#include <Arduino.h>
#include <time.h>

namespace net {

void begin(const char* ssid, const char* password);
void update(uint32_t nowMs);

bool connected();     // joined the home Wi-Fi
bool apActive();      // setup hotspot is on
String ip();          // address to open in a browser
String apName();
String ssid();
int rssi();

bool timeValid();               // clock set from the internet
bool localTime(struct tm& out); // false if the clock isn't set yet

}  // namespace net
