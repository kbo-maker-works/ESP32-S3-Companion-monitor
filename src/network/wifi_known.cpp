// Saved WiFi networks: see wifi_known.h.
#include "network/wifi_known.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include "settings.h"

namespace {

struct Net {
  char ssid[33];
  char pass[65];
};

Net s_nets[WIFI_KNOWN_MAX];
int s_count = 0;
bool s_loaded = false;
int s_turn = 0;  // wifiKnownPick without a match: the next one to try

void save() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < s_count; i++) {
    JsonObject o = a.add<JsonObject>();
    o["s"] = s_nets[i].ssid;
    o["p"] = s_nets[i].pass;
  }
  String out;
  serializeJson(doc, out);
  Preferences p;
  if (p.begin("wifinets", false)) {
    p.putString("list", out);
    p.end();
  }
}

int find(const char* ssid) {
  for (int i = 0; i < s_count; i++)
    if (!strcmp(s_nets[i].ssid, ssid)) return i;
  return -1;
}

// Move entry i to the front.
void toFront(int i) {
  if (i <= 0) return;
  const Net n = s_nets[i];
  memmove(&s_nets[1], &s_nets[0], i * sizeof(Net));
  s_nets[0] = n;
}

void use(int i) {
  strlcpy(wifiSSID, s_nets[i].ssid, sizeof(wifiSSID));
  strlcpy(wifiPass, s_nets[i].pass, sizeof(wifiPass));
}

}  // namespace

void wifiKnownLoad() {
  if (s_loaded) return;
  s_loaded = true;
  Preferences p;
  String s;
  if (p.begin("wifinets", true)) {
    s = p.getString("list", "");
    p.end();
  }
  JsonDocument doc;
  if (s.length() && !deserializeJson(doc, s)) {
    for (JsonObject o : doc.as<JsonArray>()) {
      const char* ssid = o["s"] | "";
      if (!ssid[0] || s_count == WIFI_KNOWN_MAX) continue;
      strlcpy(s_nets[s_count].ssid, ssid, sizeof(s_nets[0].ssid));
      strlcpy(s_nets[s_count].pass, o["p"] | "", sizeof(s_nets[0].pass));
      s_count++;
    }
  }
  if (wifiSSID[0] && find(wifiSSID) < 0) wifiKnownRemember(wifiSSID, wifiPass);
}

int wifiKnownCount() {
  wifiKnownLoad();
  return s_count;
}

const char* wifiKnownSsid(int i) { return i >= 0 && i < s_count ? s_nets[i].ssid : ""; }

void wifiKnownRemember(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return;
  s_loaded = true;  // called from wifiKnownLoad too
  int i = find(ssid);
  if (i < 0) {
    // Full: the one joined longest ago (the last) makes room.
    i = s_count < WIFI_KNOWN_MAX ? s_count++ : WIFI_KNOWN_MAX - 1;
    strlcpy(s_nets[i].ssid, ssid, sizeof(s_nets[0].ssid));
  }
  strlcpy(s_nets[i].pass, pass ? pass : "", sizeof(s_nets[0].pass));
  toFront(i);
  save();
}

bool wifiKnownPass(const char* ssid, String& pass) {
  wifiKnownLoad();
  const int i = find(ssid);
  if (i < 0) return false;
  pass = s_nets[i].pass;
  return true;
}

bool wifiKnownForget(const char* ssid) {
  wifiKnownLoad();
  const int i = find(ssid);
  if (i < 0 || s_count <= 1) return false;
  memmove(&s_nets[i], &s_nets[i + 1], (s_count - i - 1) * sizeof(Net));
  s_count--;
  save();
  // The network the deck joins at boot (wifiSSID in the settings) must stay a saved one.
  if (!strcmp(wifiSSID, ssid)) {
    use(0);
    saveSettings();
  }
  return true;
}

void wifiKnownJoined(const char* ssid) {
  wifiKnownLoad();
  const int i = find(ssid);
  if (i > 0) {
    toFront(i);
    save();
  }
  s_turn = 0;
}

void wifiKnownPick(int16_t n) {
  wifiKnownLoad();
  if (!s_count) return;
  int best = -1, bestRssi = -1000;
  for (int16_t k = 0; k < n; k++) {
    const int i = find(WiFi.SSID(k).c_str());
    if (i >= 0 && WiFi.RSSI(k) > bestRssi) {
      best = i;
      bestRssi = WiFi.RSSI(k);
    }
  }
  if (best < 0) best = s_turn++ % s_count;  // none seen (hidden, or no scan): take turns
  use(best);
  if (s_count > 1)
    Serial.printf("WiFi: joining %s (%s)\n", wifiSSID, bestRssi > -1000 ? "strongest saved one around" : "next saved one");
}
