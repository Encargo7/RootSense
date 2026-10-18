// =============================================================================
//  RootSense - automatic watering with an ESP32
//  https://github.com/Encargo7/RootSense
//
//  What it does, per plant ("zone"):
//    capacitive soil sensor  ->  moisture %  ->  plant database decides
//    ->  MOSFET trigger switch  ->  pump gives small pulses of water
//
//  Files in this sketch (each tab in the Arduino IDE):
//    PlantConfig.h         pins and hardware options  <- edit this one
//    PlantDatabase.cpp     how wet each plant likes it <- add your plants here
//    WateringController.*  the decision logic (pulse-and-soak, safety rules)
//    MoistureMath.h        sensor voltage -> moisture %, fault detection
//    PlantHardware.*       sensors, pumps (soft start), float switch, LED, button
//    PlantSettings.*       saves your settings in flash
//    PlantNetwork.*        Wi-Fi + setup hotspot + internet clock
//    WebDashboard.*        the web page and its API
//    SerialConsole.*       commands in the Serial Monitor (type "help")
//    PlantApp.*            glues everything together
//
//  Board: "ESP32 Dev Module" (Tools > Board > esp32). Serial Monitor: 115200.
// =============================================================================
#include <Arduino.h>

#include "PlantApp.h"
#include "PlantNetwork.h"
#include "SerialConsole.h"
#include "WebDashboard.h"

void setup() {
  appSetup();       // pumps off, load settings, start Wi-Fi
  web::begin();     // dashboard at http://rootsense.local
  console::begin(); // Serial Monitor commands
  enableLoopWDT();  // if loop() ever freezes for 5 s, the ESP32 restarts itself
}

void loop() {
  const uint32_t now = millis();
  updateInputs(now);     // BOOT button, tank float switch, soil sensors (every 2 s)
  updateWatering(now);   // each plant's controller decides whether it needs water
  updatePumps(now);      // start/stop the pumps: one at a time, soft start, time limit
  updateStatusLed(now);  // blink = OK, solid = pumping, fast blink = needs attention
  net::update(now);      // Wi-Fi, setup hotspot, clock
  web::handle();         // answer the dashboard
  console::poll();       // read Serial Monitor commands
  maybeRestart(now);     // after a Wi-Fi change or a "restart" command
}
