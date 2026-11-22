#ifndef LED_FX_H
#define LED_FX_H

#include <Arduino.h>
#include "settings.h"

// The strip's frames (led.cpp sends them): the effects a strip shows on its
// own and the events the deck shows over them. A frame is full brightness;
// led.cpp scales it by the LED's level and reverses it when asked.

// What the deck shows over the effect, worst last (deck_ui_printer.cpp
// picks one).
enum LedEvent : uint8_t {
  LED_EV_NONE = 0,
  LED_EV_PROGRESS,  // a bar of pct % in color, breathing while paused
  LED_EV_DONE,      // green breathing
  LED_EV_CLAUDE,    // amber breathing
  LED_EV_ERROR,     // red blinking
};

// One frame of the effect at time ms: c1 is the strip's color, fx the rest.
void ledFxRender(const LedFxSettings& fx, uint32_t c1, uint8_t n, uint32_t ms, uint8_t (*px)[3]);
// One frame of an event.
void ledFxEvent(uint8_t ev, uint8_t pct, uint32_t color, bool paused, uint8_t n, uint32_t ms,
                uint8_t (*px)[3]);
// The effects' own state (twinkles, flames) starts over.
void ledFxReset();

#endif  // LED_FX_H
