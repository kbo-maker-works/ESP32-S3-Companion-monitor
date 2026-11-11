// Deck UI: page edit mode on the device.
#include "deck_ui_int.h"

namespace deckui {

// --- Edit mode ------------------------------------------------------------------------
// Long press on an empty part of a page, or Edit in the quick panel: a sketch
// of the active page on the top layer. Drag a tile to a free place or onto a
// tile (that one moves to its old place, edCheck), resize it from the corner,
// remove it with x. Done writes the page's positions into the stored
// deck.json; Cancel drops them.
constexpr int ED_BAR_H = 48;
constexpr int ED_PAD = 8;
constexpr int ED_GAP = 7;
constexpr int ED_SLOP = 6;
constexpr uint32_t C_ED_OK = 0x5CC9A7;
constexpr uint32_t C_ED_BAD = 0xE5534B;

EditState s_ed;

void edPlace(lv_obj_t* o, const Cell& c) {
  lv_obj_set_pos(o, ED_PAD + c.x * (s_ed.cw + ED_GAP), ED_BAR_H + c.y * (s_ed.ch + ED_GAP));
  lv_obj_set_size(o, c.w * s_ed.cw + (c.w - 1) * ED_GAP, c.h * s_ed.ch + (c.h - 1) * ED_GAP);
}

int overlapArea(const Cell& a, const Cell& b) {
  const int w = min(a.x + a.w, b.x + b.w) - max(a.x, b.x);
  const int h = min(a.y + a.h, b.y + b.h) - max(a.y, b.y);
  return w > 0 && h > 0 ? w * h : 0;
}

// c is on the grid and no placed tile but a and b is on it.
bool edFree(const Cell& c, int a, int b) {
  if (c.x < 0 || c.y < 0 || c.x + c.w > s_ed.cols || c.y + c.h > s_ed.rows) return false;
  for (int j = 0; j < (int)s_ed.tiles.size(); j++) {
    const EdTile& o = s_ed.tiles[j];
    if (j != a && j != b && o.c.placed && !o.removed && overlapArea(o.c, c)) return false;
  }
  return true;
}

// Can tile k go to c? Free cells, or (a move of a placed tile) onto a tile that
// then takes k's old place (*swap, *swapTo). A same-size tile covered at least
// half snaps c onto it. The portal mirrors this (swapPlan() in web/deck.js).
bool edCheck(int k, Cell& c, bool sizing, int* swap, Cell* swapTo) {
  *swap = -1;
  if (c.x < 0 || c.y < 0 || c.x + c.w > s_ed.cols || c.y + c.h > s_ed.rows) return false;
  const Cell& me = s_ed.tiles[k].c;
  int hit = -1, best = 0, hits = 0;
  for (int j = 0; j < (int)s_ed.tiles.size(); j++) {
    const EdTile& o = s_ed.tiles[j];
    if (j == k || !o.c.placed || o.removed) continue;
    const int a = overlapArea(o.c, c);
    if (!a) continue;
    hits++;
    if (a > best) {
      best = a;
      hit = j;
    }
  }
  if (!hits) return true;
  if (sizing || !me.placed) return false;
  const Cell& o = s_ed.tiles[hit].c;
  if (o.w == me.w && o.h == me.h && best * 2 >= me.w * me.h) {
    c.x = o.x;
    c.y = o.y;
    *swap = hit;
    *swapTo = o;
    swapTo->x = me.x;
    swapTo->y = me.y;
    return true;
  }
  if (hits > 1) return false;
  // Anchored at a corner of k's old place, so a bigger tile can grow into free cells.
  const int xs[2] = {me.x, me.x + me.w - o.w}, ys[2] = {me.y, me.y + me.h - o.h};
  for (int yi = 0; yi < 2; yi++)
    for (int xi = 0; xi < 2; xi++) {
      Cell to = o;
      to.x = xs[xi];
      to.y = ys[yi];
      if (overlapArea(to, c) || !edFree(to, k, hit)) continue;
      *swap = hit;
      *swapTo = to;
      return true;
    }
  return false;
}

bool edApply(int k, Cell c, bool sizing) {
  int swap;
  Cell swapTo;
  if (!edCheck(k, c, sizing, &swap, &swapTo)) return false;
  EdTile& t = s_ed.tiles[k];
  if (swap >= 0) s_ed.tiles[swap].c = swapTo;
  t.c = c;
  t.c.placed = true;
  s_ed.dirty = true;
  return true;
}

void edLayout() {
  for (EdTile& t : s_ed.tiles)
    if (t.box) edPlace(t.box, t.c);
}

void edDragEvent(lv_event_t* e) {
  const uint32_t tag = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  const int k = tag & 0xFF;
  const bool sizing = tag & 0x100;
  if (k >= (int)s_ed.tiles.size() || !s_ed.tiles[k].box) return;
  EdTile& t = s_ed.tiles[k];
  const lv_event_code_t code = lv_event_get_code(e);
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) {
    s_ed.k = k;
    s_ed.sizing = sizing;
    s_ed.moved = false;
    s_ed.ok = false;
    s_ed.p0 = p;
    s_ed.bx0 = lv_obj_get_x(t.box);
    s_ed.by0 = lv_obj_get_y(t.box);
    return;
  }
  if (s_ed.k != k) return;
  if (code == LV_EVENT_PRESSING) {
    const int dx = p.x - s_ed.p0.x, dy = p.y - s_ed.p0.y;
    if (!s_ed.moved && abs(dx) + abs(dy) < ED_SLOP) return;
    if (!s_ed.moved) {
      s_ed.moved = true;
      // A moved tile floats over the target frame, a resized one under it.
      lv_obj_move_foreground(sizing ? s_ed.ghost : t.box);
      lv_obj_set_style_opa(t.box, LV_OPA_80, 0);
    }
    const float sx = s_ed.cw + ED_GAP, sy = s_ed.ch + ED_GAP;
    Cell c = t.c;
    if (sizing) {
      c.w = constrain((int)lroundf((p.x - ED_PAD) / sx) - c.x, 1, s_ed.cols - c.x);
      c.h = constrain((int)lroundf((p.y - ED_BAR_H) / sy) - c.y, 1, s_ed.rows - c.y);
    } else {
      lv_obj_set_pos(t.box, s_ed.bx0 + dx, s_ed.by0 + dy);
      c.x = constrain((int)lroundf((s_ed.bx0 + dx - ED_PAD) / sx), 0, s_ed.cols - c.w);
      c.y = constrain((int)lroundf((s_ed.by0 + dy - ED_BAR_H) / sy), 0, s_ed.rows - c.h);
    }
    s_ed.ok = edCheck(k, c, sizing, &s_ed.swap, &s_ed.swapTo);
    s_ed.to = c;
    for (int j = 0; j < (int)s_ed.tiles.size(); j++) {
      const EdTile& o = s_ed.tiles[j];
      if (j != k && o.box) edPlace(o.box, s_ed.ok && j == s_ed.swap ? s_ed.swapTo : o.c);
    }
    const lv_color_t col = lv_color_hex(s_ed.ok ? C_ED_OK : C_ED_BAD);
    lv_obj_set_style_border_color(s_ed.ghost, col, 0);
    lv_obj_set_style_bg_color(s_ed.ghost, col, 0);
    edPlace(s_ed.ghost, c);
    lv_obj_remove_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // RELEASED or PRESS_LOST
  if (s_ed.moved && s_ed.ok && code == LV_EVENT_RELEASED) edApply(k, s_ed.to, sizing);
  s_ed.k = -1;
  lv_obj_add_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(t.box, LV_OPA_COVER, 0);
  edLayout();
}

void edRemove(int k) {
  if (k < 0 || k >= (int)s_ed.tiles.size() || !s_ed.tiles[k].box) return;
  s_ed.tiles[k].removed = true;
  // Hidden, not deleted: this runs in an event of the box's own child.
  lv_obj_add_flag(s_ed.tiles[k].box, LV_OBJ_FLAG_HIDDEN);
  s_ed.dirty = true;
}

void edRemoveEvent(lv_event_t* e) { edRemove((int)(uintptr_t)lv_event_get_user_data(e)); }
void edDoneEvent(lv_event_t*) { s_ed.pending = EditState::P_SAVE; }
void edCancelEvent(lv_event_t*) { s_ed.pending = EditState::P_CANCEL; }

// The name the tile shows (metric name, HTTP template), else from the config.
String edTileName(const TileDef& d) {
  for (const TileRt& r : s_tiles)
    if (r.def == &d && r.name && *lv_label_get_text(r.name)) return lv_label_get_text(r.name);
  if (d.type != TILE_METRIC || d.label.length()) return tileLabel(d);
  const PcMetric* m = pcMetricFind(pcSource(d.pc), d.metric);
  return m ? String(m->name) : "PC metric " + String(d.metric);
}

void edMakeBox(int k, const TileDef& d) {
  const uint32_t accent = d.type == TILE_METRIC ? metricAccent(d) : d.hasColor ? d.color : C_ACCENT;
  lv_obj_t* b = lv_obj_create(s_ed.root);
  lv_obj_remove_style_all(b);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK));
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
  lv_obj_set_style_pad_all(b, 8, 0);
  s_ed.tiles[k].box = b;
  const Cell& c = s_ed.tiles[k].c;
  edPlace(b, c);
  // Half-cell boxes: a smaller x and grip, so part of the box is left to drag.
  const bool small = c.w * s_ed.cw < 70 || c.h * s_ed.ch < 50;
  const int xs = small ? 20 : 30, gs = small ? 22 : 34;

  lv_obj_t* name = lv_label_create(b);
  lv_label_set_text(name, edTileName(d).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  // Full width, mid height: below the x and above the grip.
  lv_obj_set_width(name, lv_pct(100));
  if (small) lv_obj_set_style_text_font(name, &deck_font_12, 0);
  lv_obj_set_style_max_height(name, small ? lineH(&deck_font_12) : 2 * lineH(&deck_font_14), 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, small ? 0 : 6);

  lv_obj_t* x = lv_obj_create(b);
  lv_obj_remove_style_all(x);
  lv_obj_set_size(x, xs, xs);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 8, -8);
  lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(x, lv_color_hex(0x3A2226), 0);
  lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(x, edRemoveEvent, LV_EVENT_CLICKED, (void*)(uintptr_t)k);
  lv_obj_t* xl = lv_label_create(x);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(xl, lv_color_hex(C_ED_BAD), 0);
  lv_obj_center(xl);

  // Resize grip: an L-shaped corner mark with a finger-sized hit area.
  lv_obj_t* g = lv_obj_create(b);
  lv_obj_remove_style_all(g);
  lv_obj_set_size(g, gs, gs);
  lv_obj_align(g, LV_ALIGN_BOTTOM_RIGHT, 8, 8);
  lv_obj_add_flag(g, LV_OBJ_FLAG_PRESS_LOCK);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* bar = lv_obj_create(g);
    lv_obj_remove_style_all(bar);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, i ? 3 : gs * 14 / 34, i ? gs * 14 / 34 : 3);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_CLOCK), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_RIGHT, -7, -7);
  }
  // Not LV_EVENT_ALL: the handler reads the touch point, which draw events lack.
  for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(b, edDragEvent, c, (void*)(uintptr_t)k);
    lv_obj_add_event_cb(g, edDragEvent, c, (void*)(uintptr_t)(k | 0x100));
  }
}

void edOpen(int page) {
  if (s_ed.root || page < 0 || page >= s_count || !s_pages[page].def) return;
  hidePanelNow();
  closeModal();
  const PageDef& def = *s_pages[page].def;
  const int w = logicalW(), h = logicalH();
  s_ed = EditState();
  s_ed.cfgPage = (int)(&def - &s_cfg.pages[0]);
  s_ed.name = def.name.length() ? def.name : def.id;
  s_ed.orient = w > h ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT;
  s_ed.cols = def.cols[s_ed.orient] * 2;  // half cells
  s_ed.rows = def.rows[s_ed.orient] * 2;
  s_ed.cw = (w - 2 * ED_PAD - (s_ed.cols - 1) * ED_GAP) / s_ed.cols;
  s_ed.ch = (h - ED_BAR_H - ED_PAD - (s_ed.rows - 1) * ED_GAP) / s_ed.rows;

  lv_obj_t* root = lv_obj_create(lv_layer_top());
  s_ed.root = root;
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, w, h);
  lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(root, lv_color_hex(C_TEXT), 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);  // taps never reach the page

  constexpr int BW = 84;
  s_ed.title = lv_label_create(root);
  lv_obj_set_style_text_font(s_ed.title, &deck_font_20, 0);
  lv_label_set_long_mode(s_ed.title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s_ed.title, w - 2 * BW - 40);
  lv_obj_set_pos(s_ed.title, 12, 13);
  lv_obj_t* cancel = qpButton(root, w - 2 * BW - 16, 4, BW, "Cancel", false);
  lv_obj_add_event_cb(cancel, edCancelEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* done = qpButton(root, w - BW - 8, 4, BW, LV_SYMBOL_OK " Done", false);
  lv_obj_set_style_bg_color(done, lv_color_hex(C_OK), 0);
  lv_obj_set_style_text_color(done, lv_color_hex(C_TEXT_ON), 0);
  lv_obj_add_event_cb(done, edDoneEvent, LV_EVENT_CLICKED, nullptr);

  for (int y = 0; y < s_ed.rows; y += 2)
    for (int x = 0; x < s_ed.cols; x += 2) {
      lv_obj_t* o = lv_obj_create(root);
      lv_obj_remove_style_all(o);
      lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_radius(o, 12, 0);
      lv_obj_set_style_border_width(o, 1, 0);
      lv_obj_set_style_border_color(o, lv_color_hex(C_TRACK), 0);
      edPlace(o, Cell{true, x, y, 2, 2});
    }

  std::vector<Cell> cells;
  placeTiles(def, s_ed.orient, cells);
  int hidden = 0;
  for (const Cell& c : cells) {
    s_ed.tiles.push_back({c, false, nullptr});
    if (!c.placed && !c.off) hidden++;
  }
  for (size_t i = 0; i < cells.size(); i++)
    if (cells[i].placed) edMakeBox((int)i, def.tiles[i]);

  s_ed.ghost = lv_obj_create(root);
  lv_obj_remove_style_all(s_ed.ghost);
  lv_obj_remove_flag(s_ed.ghost, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(s_ed.ghost, 12, 0);
  lv_obj_set_style_border_width(s_ed.ghost, 2, 0);
  lv_obj_set_style_bg_opa(s_ed.ghost, LV_OPA_20, 0);
  lv_obj_add_flag(s_ed.ghost, LV_OBJ_FLAG_HIDDEN);

  String t = "Edit " + s_ed.name;
  if (hidden) t += " (" + String(hidden) + " not shown)";
  lv_label_set_text(s_ed.title, t.c_str());

  setModalOpen(true);  // no swipes or quick panel under the editor
  panelSetPager(1, 0, 1, 0);  // and no page dots over it
  Serial.printf("Deck: edit page '%s'\n", def.id.c_str());
}

void edClose() {
  if (!s_ed.root) return;
  lv_obj_delete(s_ed.root);
  s_ed = EditState();
  setModalOpen(false);
  if (s_count) setPager(sh.active);
}

// The stored deck.json with this page's positions (pinned for every placed
// tile, in the current orientation) and removals. Tiles match by index: the
// parser keeps every tile of a page in file order. Other fields stay as they are.
// A cell back into deck.json units: whole numbers stay integers, w/h 1 left out.
void putHalf(JsonObject q, const char* key, int v) {
  if (v % 2) q[key] = v / 2.0f;
  else q[key] = v / 2;
}

void putHalves(JsonObject q, const Cell& c) {
  putHalf(q, "x", c.x);
  putHalf(q, "y", c.y);
  if (c.w != 2) putHalf(q, "w", c.w);
  if (c.h != 2) putHalf(q, "h", c.h);
}

bool edBuild(String& out, String& err) {
  String text;
  deckConfigText(text);
  JsonDocument doc;
  if (deserializeJson(doc, text)) {
    err = "stored deck.json unreadable";
    return false;
  }
  JsonObject pv;
  int n = 0;
  for (JsonObject p : doc["pages"].as<JsonArray>()) {
    if (!strcmp(p["type"] | "tiles", "metrics")) continue;  // skipped by the parser
    if (n++ == s_ed.cfgPage) {
      pv = p;
      break;
    }
  }
  JsonArray tiles = pv["tiles"].as<JsonArray>();
  const size_t own = s_ed.tiles.size();
  if (pv.isNull() || tiles.size() != own) {
    err = "page changed meanwhile";
    return false;
  }
  const char* key = s_ed.orient == ORIENT_LANDSCAPE ? "landscape" : "portrait";
  for (size_t i = 0; i < own; i++) {
    const EdTile& t = s_ed.tiles[i];
    if (t.removed || t.c.off) continue;
    JsonObject tv = tiles[i];
    if (!t.c.placed) {
      // A hidden tile loses its old cell, or it could claim space before a pinned one.
      JsonObject old = tv["pos"][key].as<JsonObject>();
      if (!old.isNull()) {
        old.remove("x");
        old.remove("y");
      }
      continue;
    }
    JsonObject pos = tv["pos"].is<JsonObject>() ? tv["pos"].as<JsonObject>() : tv["pos"].to<JsonObject>();
    JsonObject q = pos[key].to<JsonObject>();
    putHalves(q, t.c);
  }
  for (size_t i = own; i-- > 0;)
    if (s_ed.tiles[i].removed) tiles.remove(i);
  out = "";
  serializeJson(doc, out);
  return true;
}

// From deckTick, outside the LVGL event that asked for it: applying the
// config rebuilds the pages, and the editor goes with them (destroyPages).
// A failed save keeps the editor and its changes.
void edFinish() {
  const bool save = s_ed.pending == EditState::P_SAVE;
  s_ed.pending = EditState::P_NONE;
  if (!save || !s_ed.dirty) {
    edClose();
    return;
  }
  String text, err;
  if (edBuild(text, err) && deckApplyConfig(text, err)) {
    deckToastShow("Page saved", 1500);
    return;
  }
  Serial.printf("Deck: edit not saved: %s\n", err.c_str());
  deckToastShow("Not saved", 1800);
}

}  // namespace deckui
