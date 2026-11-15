// Settings of the companion's own apps (location, radar), apart from TouchDeck's.
//
// Stored in the NVS namespace "companion-cfg" with the same keys as the
// companion firmware before the move to TouchDeck, so a board that ran it
// keeps its location and radar setup.
#ifndef COMPANION_SETTINGS_H
#define COMPANION_SETTINGS_H

#include <Arduino.h>

namespace companion {

struct Settings {
  String city = "Madrid";   // shown in the portal, informative only
  String lat = "40.4168";   // decimal degrees, as text (what the user typed)
  String lon = "-3.7038";
  uint8_t radarKm = 25;     // radar radius: 10, 25, 50 or 100 km
  uint8_t radarGnd = 0;     // radar filter: 0 airborne, 1 on the ground, 2 both
};

Settings& settings();
void load();
void save();

// Radar radius steps, in order (the radar's footer cycles through them).
constexpr uint8_t RADAR_KMS[] = {10, 25, 50, 100};
bool validRadarKm(int km);

}  // namespace companion

#endif  // COMPANION_SETTINGS_H
