// Bench app (app_bench.cpp): a sheet for the workbench, pushed from a PC so
// the soldering order or the wire colours are on the deck instead of on the
// phone. A sheet is either text, one step per line (tap the right half for the
// next step, the left half to go back), or a JPEG (tap to zoom, drag to pan).
// pc-agent/td_bench.py sends it; the newest sheet opens the app by itself and
// stays on storage until the next one replaces it.
#ifndef APP_BENCH_H
#define APP_BENCH_H

#include "apps/app.h"
#include <Arduino.h>

namespace app {
extern const Def kBench;
}  // namespace app

// Limits of a sheet (td_bench.py keeps to them).
constexpr size_t BENCH_MAX_TEXT = 8192;      // bytes of text
constexpr size_t BENCH_MAX_TITLE = 47;       // bytes of the title
constexpr uint32_t BENCH_MAX_JPEG = 400 * 1024;
constexpr int BENCH_MAX_SIDE = 1280;         // pixels, either side
constexpr uint32_t BENCH_MAX_PIXELS = 800 * 600;  // decoded only while zoomed: ~940 KB of PSRAM

// A text sheet ("" title = none). False with the reason when it does not fit
// or cannot be stored. plan: a read-only plan whose "[x]" / "[~]" / "[ ]" marks
// say where it is (pc-agent/td_plan.py); quiet: don't open the app for it (an
// update of the plan on show redraws in place).
bool benchPushText(const char* title, const char* text, String& err, bool plan = false, bool quiet = false);
// The web server writes an uploaded JPEG here, then calls benchPushImage,
// which checks it (baseline JPEG within the limits) and makes it the sheet.
String benchUploadPath();
bool benchStorageReady();
bool benchPushImage(const char* title, String& err);
// The current sheet for GET /api/pc/bench: kind ("text", "image", "none"),
// title, steps and the current one, image size.
void benchJson(String& out);
// Deck loop, every tick: a new sheet wakes the screen and opens the app.
void benchDeckTick();
// Bench hook (POST /api/deck/benchapp): "next", "back", "zoom" at x, y (body
// coordinates), "fit". False when the app is not open or the op does not apply.
bool benchTestOp(const String& op, int x, int y);

#endif  // APP_BENCH_H
