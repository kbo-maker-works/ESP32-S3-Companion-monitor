#include "hardware/power.h"
#include <Arduino.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <string.h>
#include "config.h"
#include "hardware/battery.h"
#include "hardware/led.h"
#include "ui/deck_ui.h"
#include "ui/display_ui.h"

namespace {

constexpr gpio_num_t BL_PIN = (gpio_num_t)BACKLIGHT_PIN;
constexpr gpio_num_t TOUCH_INT = GPIO_NUM_3;  // AXS15231B touch INT (deck_touch), low on a touch
constexpr uint32_t NOTICE_MS = 2500;          // the toast stays this long before the sleep

bool s_quiet = false;         // timer wake: screen dark, one short WiFi try
uint32_t s_offlineSince = 0;  // on battery without WiFi since (0: not)
uint32_t s_lowSince = 0;      // on battery at or below POWER_LOW_PCT since (0: not)
uint32_t s_sleepAt = 0;       // a sleep is decided: at this time (0: none)
bool s_sleepTimer = false;    // ...and it wakes on the timer too, not only a touch

void sleepNow(bool timer) {
  Serial.printf("Power: deep sleep, %s\n", timer ? "no WiFi (timer or touch wakes it)" : "battery low (a touch wakes it)");
  shutdownLed();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // The backlight stays off through the sleep, held low in the RTC domain: a
  // floating pin could light it. powerBootBegin() releases it.
  analogWrite(BACKLIGHT_PIN, 0);
  rtc_gpio_init(BL_PIN);
  rtc_gpio_set_direction(BL_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level(BL_PIN, 0);
  rtc_gpio_hold_en(BL_PIN);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // A touch wakes it, unless the INT line is already low (a finger still on
  // the glass, or a stuck line): that would wake it straight back up.
  const uint32_t t0 = millis();
  while (!digitalRead(TOUCH_INT) && millis() - t0 < 1000) delay(10);
  if (digitalRead(TOUCH_INT)) {
    rtc_gpio_pullup_en(TOUCH_INT);
    rtc_gpio_pulldown_dis(TOUCH_INT);
    esp_sleep_enable_ext0_wakeup(TOUCH_INT, 0);
  } else {
    Serial.println("Power: touch INT held low, no touch wake");
    timer = true;  // never a sleep nothing can end
  }
  if (timer) esp_sleep_enable_timer_wakeup((uint64_t)POWER_RETRY_EVERY_S * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// Shows why on a lit screen first; a dark one sleeps at once.
void decideSleep(const char* text, bool timer) {
  s_sleepTimer = timer;
  if (s_quiet || displaySleepingForTouch()) {
    sleepNow(timer);
    return;
  }
  deckToast(text);
  s_sleepAt = millis() + NOTICE_MS;
  if (!s_sleepAt) s_sleepAt = 1;
}

}  // namespace

void powerBootBegin() {
  // Held low through the last deep sleep: give the pin back to the backlight PWM.
  rtc_gpio_hold_dis(BL_PIN);
  rtc_gpio_deinit(BL_PIN);
  s_quiet = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  if (s_quiet) {
    setManualDisplayOff(true);  // dark until the WiFi comes back or a touch
    Serial.println("Power: timer wake, trying the WiFi with the screen off");
  }
}

bool powerQuietBoot() { return s_quiet; }

void powerTick() {
  const uint32_t now = millis();
  if (s_sleepAt) {
    if ((int32_t)(now - s_sleepAt) >= 0) sleepNow(s_sleepTimer);
    return;
  }
  batteryUpdate();  // also while the deck's header isn't polling it (setup AP screen)
  const bool online = WiFi.status() == WL_CONNECTED;
  if (s_quiet && (online || !manualDisplayOff())) {
    // The WiFi is back (and the power, most likely) or someone touched it: a
    // normal boot from here on, lit.
    s_quiet = false;
    if (online) setManualDisplayOff(false);
    s_offlineSince = 0;
  }
  const bool onBattery = !strcmp(batteryPowerSource(), "battery");
  const int pct = batteryPercent();

  // Nearly flat, WiFi or not: sleep before the cell runs out. Only a touch
  // wakes it (the timer would just find it flat again).
  if (onBattery && pct >= 0 && pct <= POWER_LOW_PCT) {
    if (!s_lowSince) s_lowSince = now ? now : 1;
    if (s_quiet || now - s_lowSince >= POWER_LOW_GRACE_MS) {
      decideSleep("Battery low, going to sleep. Tap to wake", false);
      return;
    }
  } else {
    s_lowSince = 0;
  }

  if (!onBattery || online) {
    s_offlineSince = 0;
    return;
  }
  if (!s_offlineSince) s_offlineSince = now ? now : 1;
  // A timer wake gets its short try, counted from boot; otherwise the full wait.
  if (s_quiet ? now >= POWER_RETRY_WINDOW_MS : now - s_offlineSince >= POWER_NOWIFI_SLEEP_MS)
    decideSleep("No WiFi, going to sleep. Tap to wake", true);
}
