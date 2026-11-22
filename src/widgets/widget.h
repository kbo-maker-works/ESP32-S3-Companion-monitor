// Widgets: small live views that sit in a home tile
// ({"type": "widget", "widget": "<id>", "variant": "<variant>"}).
//
// The tile is a normal deck tile (background, radius, theme, glass); a widget
// builds its content inside it, refreshes it, and adapts it to the tile's
// size. A tap opens the widget's app when it has one (the tile's "tap"
// action, filled in by the config parser); without one, a widget can take
// the tap itself (Def::tap).
//
// To add a widget: write its create/update/resize/destroy (see widget.cpp),
// then add it to the registry there. It shows up in the portal's tile editor
// and in the on-device "+" picker on its own.
#ifndef WIDGET_H
#define WIDGET_H

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

namespace widget {

struct Variant {
  const char* id;    // deck.json "variant"
  const char* name;  // shown in the editors
};

struct Def {
  const char* id;    // deck.json "widget"
  const char* name;  // shown in the editors
  const char* app;   // app opened by a tap (app.h), nullptr = none
  const Variant* variants;  // the first one is the default
  uint8_t variantCount;
  // Builds the content in `tile` (a flex column, centred) and returns the
  // widget's own state, passed to the calls below. pc: the tile's "pc"
  // (a widget with Def::pc), "" = its own default.
  void* (*create)(lv_obj_t* tile, const char* variant, const char* pc);
  // Every loop: refresh, animate. True when it changed what it shows.
  bool (*update)(void* state);
  // The tile's content box changed size (also once after create).
  void (*resize)(void* state, int w, int h);
  void (*destroy)(void* state);
  // Optional: a tap on the tile when its config gives it no tap action (the
  // mascot reacts). Long presses still reach the page (edit mode).
  void (*tap)(void* state) = nullptr;
  // Shows a PC's data: the editors offer the tile's "pc".
  bool pc = false;
  // Optional: an app is about to open; let go of big buffers (the widget
  // rebuilds them in update() once no app is shown).
  void (*release)(void* state) = nullptr;
};

const Def* find(const char* id);
size_t count();
const Def* get(size_t i);
// The variant id if the widget has it, else the default one.
const char* variantOr(const Def* d, const char* variant);

}  // namespace widget

#endif  // WIDGET_H
