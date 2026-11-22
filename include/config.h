#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
//  Firmware version
// =============================================================================
#define FW_VERSION          "1.2.4-beta"
#define PRODUCT_NAME        "CompaDeck"

// Board variant - reported to the portal and the OTA page.
#ifndef BOARD_VARIANT
#define BOARD_VARIANT       "jc3248w535"
#endif

// =============================================================================
//  PC stats ingest (UDP push from the StatNode Companion)
// =============================================================================
#define UDP_PORT                4210    // companion pushes V2.1 JSON here
#define PC_STALE_TIMEOUT_MS     15000   // no packet for 15s = PC offline
#define PC_PACKET_BUF_SIZE      2048    // max UDP packet (oversize discarded)
#define MAX_METRICS             32      // max metrics tracked per packet

// PC companion status codes (mirror the V2.1 protocol)
#define PC_STATUS_OK             1
#define PC_STATUS_API_ERROR      2
#define PC_STATUS_LHM_NOT_RUNNING 3
#define PC_STATUS_LHM_STARTING   4

// =============================================================================
//  WiFi
// =============================================================================
#define WIFI_AP_PREFIX      "CompaDeck-"
#define WIFI_AP_PASSWORD    "touchdeck1234"
#define WIFI_CONNECT_TIMEOUT 15000  // 15s STA connect timeout
#define WIFI_RECONNECT_TIMEOUT 60000 // 60s before re-entering AP mode
#define WIFI_BACKOFF_PHASE1_MS    10000   // 10s between attempts in phase 1
#define WIFI_BACKOFF_PHASE2_MS    30000   // 30s between attempts after phase 1
#define WIFI_BACKOFF_PHASE3_MS    60000   // 60s between attempts after phase 2
#define WIFI_BACKOFF_PHASE2_START 5       // start phase 2 after this many attempts
#define WIFI_BACKOFF_PHASE3_START 10      // start phase 3 after this many attempts
#define WIFI_STA_PROBE_INTERVAL   120000  // 2 min between STA probes while in AP mode
#define WIFI_STA_PROBE_CHECK_MS    10000  // 10s after probe start before checking result
#define WIFI_AP_FALLBACK_MS       900000  // 15 min in phase 3 before falling back to AP

// Improv WiFi serial setup window on first boot (no stored credentials).
#define IMPROV_SETUP_WINDOW_MS    180000  // 3 min window for Improv setup at first boot

// =============================================================================
//  NVS
// =============================================================================
// Kept from StatNode so a device moved over keeps its WiFi and network setup.
#define NVS_NAMESPACE       "pcmon"

// =============================================================================
//  PC metric tiles: default full scale when a tile sets none
// =============================================================================
#define GAUGE_TEMP_SCALE_DEFAULT   100   // C
#define GAUGE_POWER_SCALE_DEFAULT  300   // W
#define GAUGE_FAN_SCALE_DEFAULT   3000   // RPM and anything without a unit rule

// =============================================================================
//  External LED (optional): a PWM-dimmed LED through an NPN/MOSFET, or an
//  addressable strip (WS2812/SK6812, GRB) with its data line on the GPIO
// =============================================================================
#define LED_TYPE_PWM    0     // one single-color LED, PWM through a transistor
#define LED_TYPE_STRIP  1     // addressable strip, one color on every LED
#define LED_PWM_FREQ    5000  // PWM frequency (Hz)
#define LED_PWM_RES     8     // PWM resolution (bits) -> 0..255 duty
// The UART connector's RX pin (+5V, TX, RX, GND): the CompaDeck's strip. TX
// (GPIO 43) would flash it with the boot log.
#define LED_DEFAULT_PIN 44
#define LED_STRIP_MAX           60    // LEDs on a strip
#define LED_STRIP_DEFAULT_COUNT 6
#define LED_STRIP_DEFAULT_COLOR 0xFFFFFF
#define LED_STRIP_REFRESH_MS    2000  // the same frame again: a glitch on the line never sticks

// Strip effects (hardware/led_fx.cpp). Stored by number: append, never renumber.
#define LED_FX_SOLID          0
#define LED_FX_BREATHE        1
#define LED_FX_RAINBOW        2   // the whole strip through the hues
#define LED_FX_RAINBOW_CYCLE  3   // the hues spread along the strip, moving
#define LED_FX_WAVE           4
#define LED_FX_GRADIENT       5
#define LED_FX_GLOW           6   // each LED breathing on its own
#define LED_FX_AMBIENT        7   // each LED drifting between the two colors
#define LED_FX_TWINKLE        8
#define LED_FX_CANDLE         9
#define LED_FX_FIRE           10
#define LED_FX_SCANNER        11
#define LED_FX_COMET          12
#define LED_FX_CHASE          13
#define LED_FX_RADAR          14  // a radar's sweep passing under the screen (the radar app's)
#define LED_FX_COUNT          15
#define LED_RADAR_SWEEP_MS    4000  // one turn, as the radar app's (both from millis(): in step)
#define LED_FXF_REVERSE       0x01  // LedFxSettings.flags: the first LED is the far end
// LedFxSettings.events: what the deck shows over the effect.
#define LED_EVB_PROGRESS      0x01  // a print's progress, in its filament's color
#define LED_EVB_DONE          0x02  // a finished print, until a touch or the bed cooled
#define LED_EVB_ERROR         0x04  // a printer error
#define LED_EVB_CLAUDE        0x08  // Claude Code waits on a prompt

// LED effect tuning (ms periods)
#define LED_TICK_MIN_INTERVAL_MS    16   // throttle ledTick() to ~60Hz

// =============================================================================
//  USB 5 V sense (optional)
// =============================================================================
// A GPIO wired to the USB-C breakout's V+ through a divider, 10k from V+ and
// 15k to GND (~3.0 V at 5 V): external power from any source (a PC, a wall
// charger, a charge-only cable). Read as digital: GPIO 18 is on ADC2, which
// WiFi takes. -1 = not wired: the battery voltage tells a charger instead.
#define VBUS_SENSE_PIN  -1

// =============================================================================
//  On battery without WiFi: deep sleep (hardware/power.cpp)
// =============================================================================
// A power cut takes the router down too, and the screen would drain the cell
// waiting for it. Only on battery: on USB or a charger nothing changes. A touch
// wakes it at any time.
#define POWER_NOWIFI_SLEEP_MS  300000  // 5 min without WiFi, then deep sleep
#define POWER_RETRY_EVERY_S    600     // wake every 10 min, screen off, to try the WiFi
#define POWER_RETRY_WINDOW_MS  30000   // ...and back to sleep this long after that boot
#define POWER_LOW_PCT          5       // at or below this: sleep until touched
#define POWER_LOW_GRACE_MS     30000   // ...after this long, so a touch wake shows why

#endif // CONFIG_H
