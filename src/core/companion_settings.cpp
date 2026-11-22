#include "core/companion_settings.h"
#include <Preferences.h>

namespace companion {
namespace {
constexpr const char* NS = "companion-cfg";
Settings s_settings;
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Place s_place = {40.4168f, -3.7038f, 25, 0};

void publish() {
  const Place p = {s_settings.lat.toFloat(), s_settings.lon.toFloat(), s_settings.radarKm, s_settings.radarGnd};
  portENTER_CRITICAL(&s_mux);
  s_place = p;
  portEXIT_CRITICAL(&s_mux);
}
}  // namespace

Settings& settings() { return s_settings; }

Place place() {
  portENTER_CRITICAL(&s_mux);
  const Place p = s_place;
  portEXIT_CRITICAL(&s_mux);
  return p;
}

bool validRadarKm(int km) {
  for (uint8_t k : RADAR_KMS)
    if (k == km) return true;
  return false;
}

void load() {
  Preferences p;
  Settings& s = s_settings;
  if (!p.begin(NS, true)) {  // never saved: defaults
    publish();
    return;
  }
  if (p.isKey("city")) s.city = p.getString("city", s.city);
  if (p.isKey("lat")) s.lat = p.getString("lat", s.lat);
  if (p.isKey("lon")) s.lon = p.getString("lon", s.lon);
  s.radarKm = p.getUChar("rdr_km", s.radarKm);
  s.radarGnd = p.getUChar("rdr_gnd", s.radarGnd);
  s.radarLed = p.getBool("rdr_led", s.radarLed);
  p.end();
  if (!validRadarKm(s.radarKm)) s.radarKm = 25;
  if (s.radarGnd > 2) s.radarGnd = 0;
  publish();
  Serial.printf("Companion: %s (%s, %s), radar %u km\n", s.city.c_str(), s.lat.c_str(), s.lon.c_str(),
                (unsigned)s.radarKm);
}

void save() {
  publish();
  Preferences p;
  if (!p.begin(NS, false)) return;
  const Settings& s = s_settings;
  p.putString("city", s.city);
  p.putString("lat", s.lat);
  p.putString("lon", s.lon);
  p.putUChar("rdr_km", s.radarKm);
  p.putUChar("rdr_gnd", s.radarGnd);
  p.putBool("rdr_led", s.radarLed);
  p.end();
}

}  // namespace companion
