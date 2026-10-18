#pragma once
// =============================================================================
//  WebDashboard — the web server behind http://rootsense.local
//
//  GET  /               the dashboard page
//  GET  /api/status     live readings + state of every zone (JSON)
//  GET  /api/plants     the plant database (JSON)
//  GET  /api/history    moisture every 10 min for the last 24 h (JSON)
//  POST /api/zone       z, and any of: enabled, plant, pot, soak, cooldown, limit, pulses
//  POST /api/water      z, ml          water now
//  POST /api/pumptest   z, s           run the pump for s seconds
//  POST /api/flow       z, ml, s       save the measured flow
//  POST /api/calibrate  z, point=dry|wet
//  POST /api/clear      z (omit = all) clear safety alerts
//  POST /api/stop                      stop the pump
//  POST /api/quiet      enabled, start, end
//  POST /api/wifi       ssid, pass     (restarts)
//  POST /api/restart
//
//  There is no password: only use it on a network you trust.
// =============================================================================

namespace web {
void begin();
void handle();  // call every loop
}  // namespace web
