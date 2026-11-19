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

// External power charges the battery: a PC on USB, or power seen on the cell
// (a wall charger, a charge-only cable). False without a battery.
bool batteryCharging();

// "usb" (a PC on the data lines), "charger" (power seen on the cell),
// "battery", or "none" (no battery and no PC).
const char* batteryPowerSource();

// Smoothed battery voltage in mV (0 before the first reading).
int batteryMillivolts();

#endif
