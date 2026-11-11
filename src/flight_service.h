// Air traffic around the companion's location (companion_settings), from the
// free ADS-B API of adsb.lol (no key):
//   GET https://api.adsb.lol/v2/point/{lat}/{lon}/{radius_nm}
//
// The HTTPS request runs in its own FreeRTOS task on core 0, so the UI never
// blocks on it. It only fetches while the radar is open (setActive) and WiFi
// is up, every REFRESH_MS.
#ifndef FLIGHT_SERVICE_H
#define FLIGHT_SERVICE_H

#include <Arduino.h>

namespace flights {

constexpr int MAX = 30;  // what the radar gets (already filtered)
constexpr int ALL = 80;  // kept from each response, before filtering
constexpr uint32_t REFRESH_MS = 12000;

enum Kind : uint8_t { COMMERCIAL = 0, MILITARY = 1, CARGO = 2 };
enum Ground : uint8_t { AIRBORNE = 0, ON_GROUND = 1, BOTH = 2 };
enum class State : uint8_t { Idle, Loading, Ok, NoNet, Error };

struct Plane {
  char call[10];   // callsign (or registration / hex when it sends none)
  char type[6];    // ICAO type (B738, A320...)
  float distKm;    // distance from the centre
  float bearing;   // from the centre, degrees (0 = north)
  int32_t altFt;   // barometric altitude in feet (-1 = "ground", -2 = unknown)
  int16_t gsKt;    // ground speed, knots
  int16_t track;   // heading over ground, degrees (-1 = unknown)
  uint8_t kind;    // Kind
  bool ground;     // on the ground: alt_baro "ground", or < 40 kt and < 500 ft
};

struct Snapshot {
  Plane planes[MAX];
  int count = 0;
  State state = State::Idle;
  uint32_t seq = 0;  // changes with every new response or filter change
  int total = 0;     // planes in the radius, unfiltered
  uint32_t at = 0;   // millis() of the last good response
  int httpCode = 0;
};

void begin();
// The radar turns it on when it opens and off when it closes.
void setActive(bool on);
// Fetch again as soon as possible (radius changed).
void refreshSoon();
// The latest data, filtered (Ground) and cut to the MAX nearest.
void snapshot(Snapshot& out, uint8_t filter);

}  // namespace flights

#endif  // FLIGHT_SERVICE_H
