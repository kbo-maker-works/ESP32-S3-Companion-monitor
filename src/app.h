// Apps: full-screen views opened from a tile ("tap": {"action": "app", "app": "<id>"}).
//
// An app gets an empty LVGL container below the header ("‹ Inicio" + its name)
// and builds whatever it wants in it. The deck opens and closes it like a
// detail card, with the same animation from and back into its tile.
//
// To add an app: write its open/tick/close (see app.cpp), then add a line to
// the registry there. It shows up in the portal's tile editor ("On tap > App")
// on its own.
#ifndef APP_H
#define APP_H

#include <lvgl.h>
#include <stddef.h>

namespace app {

struct Def {
  const char* id;    // used in deck.json: "app": "radar"
  const char* name;  // header title and default tile label
  const char* icon;  // default tile icon, "mdi:<name>"
  bool ready;        // false: shows "Próximamente"
  // Build the view in `body` (w x h, below the header). Called once per open.
  void (*open)(lv_obj_t* body, int w, int h);
  // Every loop while open (optional): refresh data, animate.
  void (*tick)();
  // On close, before the view is deleted (optional): stop work, free memory.
  void (*close)();
};

const Def* find(const char* id);
const Def* all(size_t& count);

}  // namespace app

#endif  // APP_H
