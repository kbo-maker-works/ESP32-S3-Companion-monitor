#include "hardware/led.h"
#include "hardware/led_fx.h"
#include "config.h"
#include "settings.h"
#include "hardware/battery.h"
#include "ui/display_ui.h"
#include "services/pc_metrics.h"
#include "soc/soc_caps.h"
#include <esp_heap_caps.h>
#include <string.h>

// Which GPIO currently carries the signal, and as what, or -1 when nothing is
// attached. Kept separate from ledSettings so a configuration change detaches
// the OLD pin before claiming the new one.
static int8_t  attachedPin     = -1;
static uint8_t attachedType    = LED_TYPE_PWM;
static int16_t lastWrittenDuty = -1;   // -1 = unknown, forces the first write
static uint32_t lastTickMs     = 0;

// Strip: one RMT symbol per bit, 24 per LED. Internal RAM: the RMT interrupt
// copies from it while a frame goes out.
static rmt_data_t* stripSyms   = nullptr;
static uint8_t     stripCount  = 0;
static uint8_t     stripPx[LED_STRIP_MAX][3];    // the frame at full brightness
static uint8_t     stripSent[LED_STRIP_MAX][3];  // what is on the strip
static bool        stripDirty  = true;           // send the next frame whatever it is
static uint32_t    stripSentMs = 0;

// The event the deck shows over the effect, and a portal test of one.
static uint8_t  evKind = LED_EV_NONE, evPct = 0;
static uint32_t evColor = 0;
static bool     evPaused = false;
static uint8_t  testKind = LED_EV_NONE;
static uint32_t testFromMs = 0;
constexpr uint32_t LED_TEST_MS = 6000;

// The shown deck page's own effect (ledSetPageFx), over the saved one.
static bool          pageFxOn = false;
static LedFxSettings pageFx;
static uint32_t      pageColor = 0;

// The battery rule holds the LED dark (for /api/status).
static bool batteryHeld = false;

// Live preview from the portal: unsaved settings drive the LED. While set, the
// tick ignores the schedule and the events, so the 60 Hz tick does not fight a
// slider drag with the stale saved value. Cleared by initLed() (which the save
// handler calls) and by shutdownLed().
static bool          previewActive = false;
static LedSettings   previewCfg;
static LedFxSettings previewFx;

// ---------------------------------------------------------------------------
//  Pin deny-list
// ---------------------------------------------------------------------------
// Refuses a pin that would kill the display, touch, USB or boot; it does not
// try to model which pins the board physically breaks out.
bool ledPinAllowed(uint8_t pin) {
  // GPIO 0 is a strapping pin on every board here, never a valid choice.
  if (pin == 0) return false;
  if (pin == BACKLIGHT_PIN) return false;
  if (dispSettings.sdCard && pin >= 10 && pin <= 13) return false;  // microSD
  // Guition JC3248W535 (AXS15231B QSPI 320x480)
  if (pin == 21 || pin == 39 || pin == 40 ||
      pin == 45 || pin == 47 || pin == 48) return false;  // display QSPI
  if (pin == 38) return false;                // display TE
  if (pin == 4 || pin == 8) return false;     // touch I2C
  if (pin == 3) return false;                 // touch INT
  if (pin == 5) return false;                 // battery sense ADC
  if (pin == 2 || pin == 41 || pin == 42) return false;   // NS4168 I2S audio
  if (pin == 19 || pin == 20) return false;   // USB CDC D-/D+
  if (VBUS_SENSE_PIN >= 0 && pin == VBUS_SENSE_PIN) return false;  // USB 5 V sense input
  if (pin >= 26 && pin <= 37) return false;   // SPI flash + PSRAM (qio_opi)
  // Pins the chip does not have (the S3 has no GPIO 22..25).
  if (pin >= SOC_GPIO_PIN_COUNT || !((SOC_GPIO_VALID_OUTPUT_GPIO_MASK >> pin) & 1)) return false;
  return true;
}

bool ledPinValid() {
  return ledSettings.enabled && ledPinAllowed(ledSettings.pin);
}

void sanitizeLedFx(LedFxSettings& fx) {
  if (fx.effect >= LED_FX_COUNT) fx.effect = LED_FX_SOLID;
  fx.color2 &= 0xFFFFFF;
  fx.flags &= LED_FXF_REVERSE;
  fx.events &= LED_EVB_PROGRESS | LED_EVB_DONE | LED_EVB_ERROR | LED_EVB_CLAUDE;
}

void sanitizeLedPin() {
  ledSettings.enabled = ledSettings.enabled ? 1 : 0;
  ledSettings.nightEnabled = ledSettings.nightEnabled ? 1 : 0;
  ledSettings.followDisplay = ledSettings.followDisplay ? 1 : 0;
  ledSettings.offlineOff = ledSettings.offlineOff ? 1 : 0;
  ledSettings.batteryOff = ledSettings.batteryOff ? 1 : 0;
  if (ledSettings.type != LED_TYPE_STRIP) ledSettings.type = LED_TYPE_PWM;
  ledSettings.count = constrain(ledSettings.count, 1, LED_STRIP_MAX);
  ledSettings.color &= 0xFFFFFF;
  sanitizeLedFx(ledFxSettings);
  if (ledSettings.nightStartMinute >= 24 * 60) ledSettings.nightStartMinute = 22 * 60;
  if (ledSettings.nightEndMinute >= 24 * 60) ledSettings.nightEndMinute = 7 * 60;
  // Nothing is driven while the LED is off, so a stored pin is just a
  // remembered choice - policing it there would silently erase what the user
  // typed before they ticked the box. initLed() guards the park-LOW path with
  // its own ledPinAllowed() check, so an unvalidated pin can never be poked.
  if (!ledSettings.enabled) return;
  if (!ledPinAllowed(ledSettings.pin)) {
    Serial.printf("LED: GPIO %u is not allowed on this board, disabling\n",
                  (unsigned)ledSettings.pin);
    ledSettings.enabled = 0;
    ledSettings.pin = LED_DEFAULT_PIN;
  }
}

// ---------------------------------------------------------------------------
//  Strip frames
// ---------------------------------------------------------------------------
// stripPx scaled by level, reversed when asked, sent when it changed (and
// every LED_STRIP_REFRESH_MS anyway). At 10 MHz a 1 is 0.8 us high + 0.4 us
// low, a 0 is 0.4 + 0.8 (WS2812B and SK6812 both take it), GRB order. The
// line rests low after the frame, which latches it.
static void stripPush(uint8_t level, bool reverse) {
  if (!stripSyms || attachedPin < 0) return;
  uint8_t out[LED_STRIP_MAX][3];
  for (uint8_t i = 0; i < stripCount; i++) {
    const uint8_t* p = stripPx[reverse ? stripCount - 1 - i : i];
    for (int c = 0; c < 3; c++) out[i][c] = (uint8_t)((p[c] * level + 127) / 255);
  }
  const uint32_t now = millis();
  const size_t bytes = (size_t)stripCount * 3;
  if (!stripDirty && !memcmp(out, stripSent, bytes) && now - stripSentMs < LED_STRIP_REFRESH_MS) return;
  rmt_data_t* s = stripSyms;
  for (uint8_t led = 0; led < stripCount; led++) {
    const uint8_t grb[3] = {out[led][1], out[led][0], out[led][2]};
    for (int c = 0; c < 3; c++) {
      for (int bit = 7; bit >= 0; bit--, s++) {
        const bool one = (grb[c] >> bit) & 1;
        s->level0 = 1;
        s->duration0 = one ? 8 : 4;
        s->level1 = 0;
        s->duration1 = one ? 4 : 8;
      }
    }
  }
  rmtWrite(attachedPin, stripSyms, (size_t)stripCount * 24, 20);
  memcpy(stripSent, out, bytes);
  stripSentMs = now;
  stripDirty = false;
}

static void stripBlank() {
  memset(stripPx, 0, sizeof(stripPx));
  stripDirty = true;
  stripPush(0, false);
}

// One frame of the strip: a test, the deck's event, or the effect.
static void stripFrame(uint8_t level, const LedSettings& cfg, const LedFxSettings& fx, bool events) {
  const uint32_t now = millis();
  uint8_t kind = LED_EV_NONE, pct = evPct;
  uint32_t color = evColor;
  bool paused = evPaused;
  if (testKind && now - testFromMs < LED_TEST_MS) {
    kind = testKind;
    pct = (uint8_t)((now - testFromMs) * 100 / LED_TEST_MS);
    color = cfg.color;
    paused = false;
  } else {
    testKind = LED_EV_NONE;
    if (events) kind = evKind;
  }
  if (kind) ledFxEvent(kind, pct, color, paused, stripCount, now, stripPx);
  else ledFxRender(fx, cfg.color, stripCount, now, stripPx);
  stripPush(level, fx.flags & LED_FXF_REVERSE);
}

// ---------------------------------------------------------------------------
//  Output
// ---------------------------------------------------------------------------
static void writePwm(uint8_t duty) {
  if (attachedPin < 0 || (int16_t)duty == lastWrittenDuty) return;
  ledcWrite((uint8_t)attachedPin, duty);
  lastWrittenDuty = duty;
}

// The LED at `level` (0..255) for a configuration: a PWM duty, or a strip frame.
static void writeLevel(uint8_t level, const LedSettings& cfg, const LedFxSettings& fx, bool events) {
  if (attachedPin < 0) return;
  if (attachedType != LED_TYPE_STRIP) {
    writePwm(level);
    return;
  }
  stripFrame(level, cfg, fx, events);
  lastWrittenDuty = level;
}

// Park the pin as a driven LOW output rather than leaving it high-Z, so the
// transistor gate (or the strip's data line) is held off by firmware and does
// not depend on an external pulldown. pinMode() re-takes the pin from the
// LEDC / RMT peripheral.
static void detachAndForceLow() {
  if (attachedPin >= 0) {
    if (attachedType == LED_TYPE_STRIP) {
      stripBlank();  // dark before letting go: the strip keeps its last frame
      rmtDeinit(attachedPin);
      heap_caps_free(stripSyms);
      stripSyms = nullptr;
      stripCount = 0;
    } else {
      ledcWrite((uint8_t)attachedPin, 0);
      ledcDetach((uint8_t)attachedPin);
    }
    pinMode(attachedPin, OUTPUT);
    digitalWrite(attachedPin, LOW);
  }
  attachedPin = -1;
  lastWrittenDuty = -1;
}

static bool attachLed(uint8_t type, uint8_t pin, uint8_t count) {
  const bool strip = type == LED_TYPE_STRIP;
  count = constrain(count, 1, LED_STRIP_MAX);
  if (attachedPin == (int8_t)pin && attachedType == type && (!strip || stripCount == count))
    return true;
  detachAndForceLow();
  if (strip) {
    stripSyms = (rmt_data_t*)heap_caps_malloc(sizeof(rmt_data_t) * 24 * count,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Four memory blocks hold 8 LEDs' frame whole, so no refill interrupt
    // (which WiFi can delay) runs in the middle of it; one block if they are taken.
    if (!stripSyms || (!rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_4, 10000000) &&
                       !rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000))) {
      Serial.printf("LED: could not start the strip on GPIO %u\n", (unsigned)pin);
      heap_caps_free(stripSyms);
      stripSyms = nullptr;
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
      return false;
    }
    stripCount = count;
    stripDirty = true;
    ledFxReset();
  } else if (!ledcAttach(pin, LED_PWM_FREQ, LED_PWM_RES)) {
    Serial.printf("LED: could not attach PWM to GPIO %u\n", (unsigned)pin);
    return false;
  }
  attachedPin = (int8_t)pin;
  attachedType = type;
  lastWrittenDuty = -1;
  return true;
}

uint8_t ledCurrentDuty() {
  return (lastWrittenDuty < 0) ? 0 : (uint8_t)lastWrittenDuty;
}

bool ledBatteryHeld() { return attachedPin >= 0 && batteryHeld; }

uint8_t ledEventNow() {
  if (attachedPin < 0 || attachedType != LED_TYPE_STRIP) return LED_EV_NONE;
  if (testKind && millis() - testFromMs < LED_TEST_MS) return testKind;
  return previewActive ? LED_EV_NONE : evKind;
}

// The battery rule: dark on the battery alone (a PC or a charger is power).
static uint8_t batteryRule(uint8_t level, const LedSettings& cfg) {
  batteryHeld = cfg.batteryOff && level > 0 && !strcmp(batteryPowerSource(), "battery");
  return batteryHeld ? 0 : level;
}

// The saved configuration's level right now.
static uint8_t scheduledLevel() {
  uint8_t duty = ledNightActive() ? ledSettings.nightBrightness : ledSettings.brightness;
  // One test covers manual off and a night brightness of 0: whatever put the
  // panel out, the LED goes with it.
  if (ledSettings.followDisplay && currentBacklightLevel() == 0) duty = 0;
  if (ledSettings.offlineOff && !pcAnyOnline()) duty = 0;
  return batteryRule(duty, ledSettings);
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------
void initLed() {
  previewActive = false;

  if (!ledSettings.enabled) {
    // A saved-but-disabled pin still gets parked LOW - but only if it is a pin
    // we are allowed to touch, so a stale setting can never poke the display
    // bus or USB.
    const uint8_t saved = ledSettings.pin;
    detachAndForceLow();
    if (saved != LED_DEFAULT_PIN && ledPinAllowed(saved)) {
      pinMode(saved, OUTPUT);
      digitalWrite(saved, LOW);
    }
    Serial.println("LED: disabled");
    return;
  }
  if (!ledPinAllowed(ledSettings.pin)) {
    Serial.printf("LED: GPIO %u is not allowed on this board\n",
                  (unsigned)ledSettings.pin);
    detachAndForceLow();
    return;
  }
  if (!attachLed(ledSettings.type, ledSettings.pin, ledSettings.count)) return;
  stripDirty = true;
  writeLevel(scheduledLevel(), ledSettings, ledFxSettings, true);
  if (ledSettings.type == LED_TYPE_STRIP)
    Serial.printf("LED: strip of %u on GPIO %u, effect %u, brightness %u\n",
                  (unsigned)ledSettings.count, (unsigned)ledSettings.pin,
                  (unsigned)ledFxSettings.effect, (unsigned)ledSettings.brightness);
  else
    Serial.printf("LED: GPIO %u at brightness %u\n",
                  (unsigned)ledSettings.pin, (unsigned)ledSettings.brightness);
}

void shutdownLed() {
  previewActive = false;
  detachAndForceLow();
}

void previewLed(bool enabled, uint8_t pin, uint8_t brightness) {
  LedSettings cfg = ledSettings;
  cfg.enabled = enabled;
  cfg.pin = pin;
  cfg.brightness = brightness;
  previewLed(cfg, ledFxSettings);
}

void previewLed(const LedSettings& cfg, const LedFxSettings& fx) {
  if (!cfg.enabled || !ledPinAllowed(cfg.pin)) {
    previewActive = false;
    detachAndForceLow();
    return;
  }
  const uint8_t type = cfg.type == LED_TYPE_STRIP ? LED_TYPE_STRIP : LED_TYPE_PWM;
  if (!attachLed(type, cfg.pin, cfg.count)) {
    previewActive = false;
    return;
  }
  previewActive = true;
  previewCfg = cfg;
  previewCfg.type = type;
  previewCfg.color &= 0xFFFFFF;
  previewFx = fx;
  sanitizeLedFx(previewFx);
  writeLevel(batteryRule(previewCfg.brightness, ledSettings), previewCfg, previewFx, false);
}

// Unconditional re-init, not "only if a preview is running": previewing the
// LED as DISABLED clears previewActive on the way in, and an early return here
// would then leave the LED dark against a saved config that says it is on.
void clearLedPreview() {
  previewActive = false;
  initLed();   // back to the saved configuration
}

void ledSetPageFx(const LedFxSettings* fx, uint32_t color) {
  pageFxOn = fx != nullptr;
  if (!fx) return;
  pageFx = *fx;
  sanitizeLedFx(pageFx);
  pageColor = color & 0xFFFFFF;
}

void ledSetEvent(uint8_t kind, uint8_t pct, uint32_t color, bool paused) {
  evKind = kind;
  evPct = pct;
  evColor = color;
  evPaused = paused;
}

bool ledTestEvent(uint8_t kind) {
  if (attachedPin < 0 || attachedType != LED_TYPE_STRIP || !kind || kind > LED_EV_ERROR) return false;
  testKind = kind;
  testFromMs = millis();
  return true;
}

// ---------------------------------------------------------------------------
//  Tick
// ---------------------------------------------------------------------------
void ledTick() {
  if (attachedPin < 0) return;
  const uint32_t now = millis();
  if (now - lastTickMs < LED_TICK_MIN_INTERVAL_MS) return;
  lastTickMs = now;

  if (previewActive) {
    // The battery rule still holds: a strip without its 5 V must not be fed
    // through its data pin.
    writeLevel(batteryRule(previewCfg.brightness, ledSettings), previewCfg, previewFx, false);
    return;
  }
  if (!ledSettings.enabled) return;
  if (pageFxOn && attachedType == LED_TYPE_STRIP) {
    LedSettings cfg = ledSettings;
    cfg.color = pageColor;
    writeLevel(scheduledLevel(), cfg, pageFx, true);
    return;
  }
  writeLevel(scheduledLevel(), ledSettings, ledFxSettings, true);
}
