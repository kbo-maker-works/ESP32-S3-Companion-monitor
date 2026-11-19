#include "hardware/battery.h"
#include <Arduino.h>
#include <stdlib.h>

namespace {

constexpr int PIN = 5;
// Battery -> GPIO5 divider, not the nominal 1:2. Calibrated on the charger's
// full-charge hold: after a full charge the reading sat flat for an hour, which
// is the charger holding the cell at ~4.18 V (ADC 2404 mV at the pin).
constexpr float DIVIDER = 1.739f;
constexpr uint32_t PERIOD_MS = 2000;
constexpr int PRESENT_MV = 2500;       // below this nothing is on the connector
// While the charger pushes current the cell reads above its resting voltage.
constexpr int CHARGE_RISE_MV = 80;
// The charger holds a full cell here: 100 % while plugged in.
constexpr int CHARGE_FULL_MV = 4150;
// Power without USB data (a wall charger, a charge-only cable) has no signal of
// its own: it shows on the cell. Plugging in lifts the reading at once and
// unplugging drops it (two samples in a row this far from the smoothed value,
// so a WiFi burst does not count); between steps, a cell on the charger only
// rises or stays flat and a cell feeding the board only falls.
constexpr int STEP_MV = 70;
constexpr uint32_t TREND_MS = 300000;  // charging lifts the cell ~8 mV/min
constexpr int TREND_MV = 25;
// Before any step was seen (boot), only the charger holds the cell this high
// under the board's load: on battery it is below 4.02 V within a minute.
constexpr int BOOT_SAMPLES = 10;

struct Point { uint16_t mv; uint8_t pct; };
// Under load (screen + WiFi), from a measured discharge of the 750 mAh 403450
// at maximum brightness (2026-11-13): 4.02 V just after unplugging to 3.50 V
// took 91 min, % by time left. Lighter loads read a little higher.
const Point kCurve[] = {
  {3480, 0},  {3500, 2},  {3550, 9},  {3600, 17}, {3650, 22}, {3700, 30},
  {3750, 36}, {3800, 46}, {3850, 55}, {3900, 61}, {3950, 67}, {4000, 75},
  {4020, 88}, {4100, 95}, {4150, 100},
};

bool s_started = false;
bool s_charging = false;
bool s_usb = false;  // a PC on the USB data lines
bool s_ext = false;  // power seen on the cell, without USB data
int s_upRun = 0, s_downRun = 0;
float s_trendMv = 0;
uint32_t s_trendAt = 0;
int s_samples = 0;
uint32_t s_last = 0;
float s_mv = 0;
int s_pct = -1;

int sampleMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN);
  return (int)(sum / 8 * DIVIDER);
}

int pctFor(float mv) {
  const size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
  if (mv <= kCurve[0].mv) return 0;
  if (mv >= kCurve[n - 1].mv) return 100;
  for (size_t i = 1; i < n; i++) {
    if (mv > kCurve[i].mv) continue;
    const Point a = kCurve[i - 1], b = kCurve[i];
    return (int)(a.pct + (b.pct - a.pct) * (mv - a.mv) / (float)(b.mv - a.mv) + 0.5f);
  }
  return 100;
}

// USB power from a PC: the host sends start-of-frame packets. A wall charger
// does not, so charging from one is not seen.
bool usbPowered() {
#if ARDUINO_USB_MODE && ARDUINO_USB_CDC_ON_BOOT
  return HWCDC::isPlugged();
#else
  return false;
#endif
}

}  // namespace

bool batteryUpdate() {
  const uint32_t now = millis();
  if (s_started && now - s_last < PERIOD_MS) return false;
  s_last = now;
  const int mv = sampleMv();
  const bool usb = usbPowered();
  if (s_usb && !usb) s_ext = false;  // the PC's cable came out: its power went with it
  s_usb = usb;

  // Steps against the smoothed reading, before it takes this sample in.
  bool step = false;
  if (s_started && !usb && mv >= PRESENT_MV) {
    const int d = mv - (int)s_mv;
    s_upRun = d >= STEP_MV ? s_upRun + 1 : 0;
    s_downRun = d <= -STEP_MV ? s_downRun + 1 : 0;
    // A confirmed step re-bases the reading even when the state already matches.
    if (s_upRun >= 2) s_ext = step = true;
    if (s_downRun >= 2) { s_ext = false; step = true; }
    if (step) s_upRun = s_downRun = 0;
  } else {
    s_upRun = s_downRun = 0;
  }
  if (mv < PRESENT_MV) s_ext = false;

  const bool charging = usb || s_ext;
  const bool plugChange = charging != s_charging;
  s_charging = charging;
  // Start (or restart after a plug/unplug jump) from the raw reading, then smooth;
  // while a step waits for its second sample, the reference holds still.
  if (!s_started || plugChange || step) s_mv = mv;
  else if (s_upRun || s_downRun) {}
  else if (abs(mv - (int)s_mv) > 300) s_mv = mv;
  else s_mv = s_mv * 0.8f + mv * 0.2f;
  s_started = true;

  if (plugChange || step || !s_trendAt) {
    s_trendMv = s_mv;
    s_trendAt = now;
  } else if (now - s_trendAt >= TREND_MS) {
    if (!usb && s_mv >= PRESENT_MV) {
      if (s_mv - s_trendMv >= TREND_MV) s_ext = true;
      else if (s_trendMv - s_mv >= TREND_MV) s_ext = false;
    }
    s_trendMv = s_mv;
    s_trendAt = now;
  }
  if (s_samples < BOOT_SAMPLES && ++s_samples == BOOT_SAMPLES && !usb && s_mv >= CHARGE_FULL_MV) s_ext = true;

  int pct = s_mv >= PRESENT_MV ? pctFor(s_mv - (charging ? CHARGE_RISE_MV : 0)) : -1;
  if (pct >= 0 && charging && s_mv >= CHARGE_FULL_MV) pct = 100;
  // Hold the shown value against 1 % jitter, but never across the 20 % warning.
  if (!plugChange && pct >= 0 && s_pct >= 0 && abs(pct - s_pct) < 2 && (pct < 20) == (s_pct < 20))
    pct = s_pct;
  if (pct == s_pct && !plugChange) return false;
  s_pct = pct;
  return true;
}

int batteryPercent() { return s_pct; }

bool batteryCharging() { return s_charging && s_pct >= 0; }

int batteryMillivolts() { return (int)(s_mv + 0.5f); }

const char* batteryPowerSource() {
  if (s_pct < 0) return s_usb ? "usb" : "none";
  return s_usb ? "usb" : s_ext ? "charger" : "battery";
}
