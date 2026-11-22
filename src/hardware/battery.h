#ifndef BATTERY_H
#define BATTERY_H

#include <stdint.h>

// LiPo on the board's battery connector, sensed on GPIO5 through the board's
// divider (factor calibrated in battery.cpp). Read every couple of seconds and smoothed; the percentage comes
// from a resting LiPo discharge curve, so it reads high while charging.

// Samples when due. True when the percentage to show changed.
bool batteryUpdate();

// Bumps on every change batteryUpdate() reports: several callers (the deck's
// header, hardware/power) can poll without taking a change from each other.
uint32_t batterySeq();

// 0..100, or -1 when no battery is connected.
int batteryPercent();

// External power charges the battery: a PC on USB, or other power (a wall
// charger, a charge-only cable) seen on VBUS_SENSE_PIN when it is wired, on the
// cell's voltage otherwise. False without a battery.
bool batteryCharging();

// "usb" (a PC on the data lines), "charger" (other external power),
// "battery", or "none" (no battery and no PC).
const char* batteryPowerSource();

// Smoothed battery voltage in mV (0 before the first reading).
int batteryMillivolts();

#endif
