#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

// LiPo on the board's battery connector, sensed on GPIO5 through the board's
// divider (factor calibrated in battery.cpp). Read every couple of seconds and smoothed; the percentage comes
// from a resting LiPo discharge curve, so it reads high while charging.

// Samples when due. True when the percentage to show changed.
bool batteryUpdate();

// 0..100, or -1 when no battery is connected.
int batteryPercent();

// A PC powers the board over USB (and so charges the battery).
bool batteryCharging();

// Smoothed battery voltage in mV (0 before the first reading).
int batteryMillivolts();

#endif
