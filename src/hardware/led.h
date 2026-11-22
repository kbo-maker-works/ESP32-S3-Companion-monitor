#ifndef LED_H
#define LED_H

#include <Arduino.h>
#include "settings.h"

// Status LED, one of two kinds on a GPIO (ledSettings.type):
// - LED_TYPE_PWM: one plain single-color LED dimmed with PWM through a
//   transistor driver. No color, only brightness.
// - LED_TYPE_STRIP: an addressable strip (WS2812/SK6812, GRB order) driven
//   by the RMT peripheral at 60 frames a second: an effect (ledFxSettings,
//   hardware/led_fx.cpp) scaled by the brightness, and over it the event the
//   deck asks for (a print's progress, a finished print, a printer error,
//   Claude waiting). On the CompaDeck it is wired to the UART connector's RX.
//
// The LED has its own night interval and level, independent of the backlight
// schedule and the manual night button, and goes dark whenever the display
// itself is dark (manual off, PC-offline sleep, or a night brightness of 0),
// and on the battery when batteryOff is set (the board's 5 V may be missing).

// Lifecycle. initLed() re-reads ledSettings, so the save handler calls it to
// apply a changed pin or brightness. ledTick() must run every loop pass.
void initLed();
void ledTick();
void shutdownLed();

// Portal live preview: drive the LED from unsaved form values so the slider
// moves the real LED (cfg.brightness is the level shown, no events). NVS is
// untouched; the next initLed() clears it. The short form keeps the rest of
// the saved settings (the deck's Light slider).
void previewLed(bool enabled, uint8_t pin, uint8_t brightness);
void previewLed(const LedSettings& cfg, const LedFxSettings& fx);
void clearLedPreview();

// The event over the strip's effect (led_fx.h LedEvent), set by the deck
// every few ticks; pct and color for LED_EV_PROGRESS.
void ledSetEvent(uint8_t kind, uint8_t pct = 0, uint32_t color = 0, bool paused = false);
// A deck page's own effect while it shows (fx and the strip's color), or
// nullptr for the Hardware page's. The events still go over it.
void ledSetPageFx(const LedFxSettings* fx, uint32_t color);
// The portal's Test: shows an event for a few seconds (a progress bar fills
// in the strip's color). False when no strip is running.
bool ledTestEvent(uint8_t kind);

// Pin validation. sanitizeLedPin() runs on load and before every NVS write
// (and sanitizes ledFxSettings too).
bool ledPinAllowed(uint8_t pin);
bool ledPinValid();
void sanitizeLedPin();
void sanitizeLedFx(LedFxSettings& fx);

// Live state for /api/status: the level on the LED (0 = dark), whether the
// battery rule is what holds it dark, and the event on the strip.
uint8_t ledCurrentDuty();
bool ledBatteryHeld();
uint8_t ledEventNow();

#endif // LED_H
