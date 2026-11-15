// Current weather at the companion's location (companion_settings) from
// Open-Meteo (no key), every 10 minutes, in its own task so the UI never
// waits on the network. Shown in the page header.
#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include <Arduino.h>

namespace weather {

struct Data {
  bool ok = false;       // a reading is available
  bool failing = false;  // the last request failed (no internet, API down)
  float temp = 0;        // °C
  int humidity = 0;      // %
  int code = 0;          // WMO weather code
  bool isDay = true;
  uint32_t seq = 0;      // changes with every reading or failure
};

void begin();
// Ask again now (location changed).
void refreshSoon();
Data get();
// Spanish condition name for a WMO code ("Despejado", "Lluvia"...).
const char* condition(int code);
// MDI icon name for the reading ("weather-sunny", "weather-night"...).
const char* mdiIcon(const Data& d);

}  // namespace weather

#endif  // WEATHER_SERVICE_H
