// On battery without WiFi (a power cut takes the router down too), deep sleep
// instead of draining the cell: wake every few minutes with the screen off to
// try the WiFi again, or on a touch. Also sleeps on a nearly flat battery. On USB
// or a charger nothing changes. Times in config.h (POWER_*).
#ifndef POWER_H
#define POWER_H

// First thing in setup(): frees the backlight pin held low through deep sleep
// and, after a timer wake, keeps the screen dark for the quiet WiFi try.
void powerBootBegin();

// True after a timer wake, until the WiFi or a touch makes it a normal boot.
// initWiFi() then makes one attempt and opens no setup AP.
bool powerQuietBoot();

// Every loop pass.
void powerTick();

#endif
