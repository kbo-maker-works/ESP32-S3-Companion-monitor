#ifndef WIFI_KNOWN_H
#define WIFI_KNOWN_H

#include <Arduino.h>

// Saved WiFi networks (home, office...): up to WIFI_KNOWN_MAX, the one joined
// last first. Kept in NVS "wifinets"; the passwords never leave the device.
// wifiSSID / wifiPass (settings.h) hold the network being joined now.
constexpr int WIFI_KNOWN_MAX = 5;

// Load the list (once); the network in wifiSSID joins it if it is not there
// yet (devices saved before the list existed).
void wifiKnownLoad();
int wifiKnownCount();
const char* wifiKnownSsid(int i);
// Add or update a network, first in the list.
void wifiKnownRemember(const char* ssid, const char* pass);
// The saved password of ssid; false when the network is not saved.
bool wifiKnownPass(const char* ssid, String& pass);
// Drop a network. The last one cannot go (the deck would have none).
bool wifiKnownForget(const char* ssid);
// The network just joined goes first (saved only when the order changes).
void wifiKnownJoined(const char* ssid);

// Put the network to join next in wifiSSID / wifiPass: of the saved ones
// seen in the scan results (n networks), the strongest; with none seen (or
// no scan, n <= 0), the next one in turn.
void wifiKnownPick(int16_t n);

#endif  // WIFI_KNOWN_H
