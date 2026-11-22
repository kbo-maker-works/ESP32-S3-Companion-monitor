// CompaDeck - firmware entry point.
// Boot: settings -> display (splash) -> WiFi (STA or AP setup) -> web/OTA
// server -> UDP metric listener. PC metric, HTTP and printer tiles live on the
// deck pages (deck_ui); the PC companion pushes V2.1 JSON to UDP :4210.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "core/tls_psram.h"
#include "ui/display_ui.h"
#include "settings.h"
#include "network/wifi_manager.h"
#include "network/web_server.h"
#include "services/pc_metrics.h"
#include "hardware/led.h"
#include "hardware/haptic.h"
#include "hardware/audio.h"
#include "hardware/power.h"
#include "core/crash_log.h"
#include "services/deck_daily.h"
#include "hardware/deck_sd.h"
#include "core/companion_settings.h"
#include "services/flight_service.h"
#include "services/weather_service.h"

// Loop-task watchdog: an indefinite block in loop() panics after
// WDT_TIMEOUT_S with a backtrace and reboots (/api/status then reports
// reset_reason "task_wdt"). Every legitimate blocking path finishes far
// below this bound.
static const uint32_t WDT_TIMEOUT_S = 30;

// LVGL rendering and the Glass bake left under 1 KB of the default 8 KB.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() {
  // Companion metrics arrive over the cable as ~1 KB lines; HWCDC's default is 256.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PRODUCT_NAME " " FW_VERSION " boot");
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  crashLogBegin();
  powerBootBegin();  // before initDisplay: a timer wake keeps the screen dark
  // The core keeps mallocs up to 4 KB internal; 128 B doubles free internal RAM.
  heap_caps_malloc_extmem_enable(128);
  // mbedTLS (this build: CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC) in PSRAM for every TLS
  // client from boot: before, the radar's HTTPS took ~35 KB of internal RAM per
  // fetch until a printer or the daily download had installed it.
  tlsPsramInstall();

  loadSettings();
  companion::load();
  deck::wallStorageBegin();
  setScreenState(SCREEN_SPLASH);
  initDisplay();

  startWiFiDuringSplash();        // kick off a background STA connect
  unsigned long splashStart = millis();
  while (millis() - splashStart < 800) {
    displayTick();
    delay(20);
  }

  initWiFi();                     // blocks until STA connects or AP comes up
  initWebServer();
  pcMetricsBegin();
  flights::begin();               // idle until the radar app opens
  weather::begin();               // header weather, every 10 min
  initLed();
  initHaptic();
  audioInit();

  if (isWiFiConnected()) setScreenState(SCREEN_DECK);

  // Subscribe only after the blocking boot phases; loop() feeds it every pass.
  // The Arduino core usually arrives with the TWDT already initialized, so
  // fall back to reconfigure when init reports that state.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_init(&wdtConfig) != ESP_OK)
    esp_task_wdt_reconfigure(&wdtConfig);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  handleWiFi();
  handleWebServer();
  pcMetricsHandle();
  powerTick();  // on battery without WiFi: deep sleep

  // Finish boot/connect transitions.
  ScreenState s = getScreenState();
  if (isWiFiConnected() &&
      (s == SCREEN_SPLASH || s == SCREEN_CONNECTING_WIFI ||
       s == SCREEN_WIFI_CONNECTED)) {
    setScreenState(SCREEN_DECK);
  }

  updateBacklightControl();
  // After the backlight: the LED reads the level the panel just settled on.
  ledTick();
  displayTick();
  deck::dailyPoll();
}
