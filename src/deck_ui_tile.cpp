// Deck UI: tile look and building a tile.
#include "deck_ui_int.h"

namespace deckui {

// --- Tile look ------------------------------------------------------------------------
// Material Design Icons names mapped onto the LVGL symbol font while no MDI
// font is loaded.
const char* iconSymbol(const TileDef& t) {
  struct Map { const char* name; const char* sym; };
  static const Map kMap[] = {
    {"lightbulb", LV_SYMBOL_EYE_OPEN}, {"ceiling-light", LV_SYMBOL_EYE_OPEN},
    {"television-ambient-light", LV_SYMBOL_IMAGE}, {"fan", LV_SYMBOL_REFRESH},
    {"weather-night", LV_SYMBOL_IMAGE}, {"eye", LV_SYMBOL_EYE_OPEN},
    {"printer-3d", LV_SYMBOL_CHARGE}, {"speaker", LV_SYMBOL_AUDIO},
    {"lock", LV_SYMBOL_HOME}, {"power", LV_SYMBOL_POWER}, {"power-sleep", LV_SYMBOL_POWER},
    {"chevron-right", LV_SYMBOL_RIGHT}, {"chevron-left", LV_SYMBOL_LEFT},
    {"microphone-off", LV_SYMBOL_MUTE}, {"image", LV_SYMBOL_IMAGE},
    {"skip-previous", LV_SYMBOL_PREV}, {"skip-next", LV_SYMBOL_NEXT},
    {"play", LV_SYMBOL_PLAY}, {"pause", LV_SYMBOL_PAUSE},
    {"console", LV_SYMBOL_KEYBOARD}, {"volume-high", LV_SYMBOL_VOLUME_MAX},
    {"volume-off", LV_SYMBOL_MUTE}, {"wifi", LV_SYMBOL_WIFI}, {"cog", LV_SYMBOL_SETTINGS},
    {"bell", LV_SYMBOL_BELL}, {"home", LV_SYMBOL_HOME},
    // Companion apps (app.cpp) and their usual icons
    {"radar", LV_SYMBOL_GPS}, {"map-marker", LV_SYMBOL_GPS}, {"airplane", LV_SYMBOL_GPS},
    {"lan", LV_SYMBOL_WIFI}, {"network", LV_SYMBOL_WIFI}, {"chip", LV_SYMBOL_DRIVE},
    {"memory", LV_SYMBOL_DRIVE}, {"robot-outline", LV_SYMBOL_CHARGE}, {"robot", LV_SYMBOL_CHARGE},
    {"cog-outline", LV_SYMBOL_SETTINGS}, {"tune", LV_SYMBOL_SETTINGS}, {"usb", LV_SYMBOL_USB},
    {"folder", LV_SYMBOL_DIRECTORY}, {"music", LV_SYMBOL_AUDIO}, {"cloud", LV_SYMBOL_UPLOAD},
  };
  String name = t.icon;
  if (name.startsWith("mdi:")) name = name.substring(4);
  for (const Map& m : kMap)
    if (name == m.name) return m.sym;
  // Unknown: an arrow ("opens something"), never a power symbol that reads as on/off
  return LV_SYMBOL_RIGHT;
}

// Puts a tile's icon on a label: the tile's own MDI icon, else a default; an
// LVGL symbol while no icon font is loaded.
// Returns the codepoint shown (0 = symbol); shown = what the label has now.
uint32_t showIcon(lv_obj_t* label, const TileDef& d, bool on, int px, uint32_t shown) {
  uint32_t cp = 0;
  if (on && d.onIcon.length()) cp = iconCodepoint(d.onIcon);
  if (!cp && d.icon.length()) cp = iconCodepoint(d.icon);
  if (!cp) cp = iconCodepoint("gesture-tap-button");
  const lv_font_t* f = cp ? iconFont(px, cp) : nullptr;
  if (!f) cp = 0;
  if (cp && cp == shown) return cp;
  if (cp) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(label, f, 0);
    lv_label_set_text(label, u);
  } else {
    lv_label_set_text(label, iconSymbol(d));
  }
  return cp;
}

// --- Widgets (widget.h) -----------------------------------------------------------
void widgetSizeEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  if (!r.wdef || !r.wstate) return;
  r.wdef->resize(r.wstate, lv_obj_get_content_width(r.obj), lv_obj_get_content_height(r.obj));
  if (r.wdef->update(r.wstate)) markPageStale(r.page);
}

// Every loop: widgets refresh themselves (each one throttles its own work).
void widgetsTick() {
  for (TileRt& r : s_tiles)
    if (r.wdef && r.wstate && r.wdef->update(r.wstate) && r.page != sh.active) markPageStale(r.page);
}

void widgetFree(TileRt& r) {
  if (r.wdef && r.wstate) r.wdef->destroy(r.wstate);
  r.wstate = nullptr;
}

String tileLabel(const TileDef& t) {
  if (t.label.length()) return t.label;
  if (t.type == TILE_WIDGET) {
    const widget::Def* w = widget::find(t.widget.c_str());
    return w ? String(w->name) : String("Widget");
  }
  if (t.type == TILE_CLOCK) return "Clock";
  if (t.type == TILE_MEDIA) return "Now playing";
  return t.id.length() ? t.id : String("Tile");
}

String tileSub(const TileRt& r) { return r.def->sub; }

void markPageStale(int page) {
  if (page >= 0 && page < s_count) s_pages[page].stale = true;
}

uint32_t tileAccent(const TileDef& def) {
  return def.hasColor ? def.color : def.type == TILE_PRINTER ? C_PRN_HEAT : C_ACCENT;
}

// Per-tile look from deck.json, else the theme and display settings.
uint32_t tileBg(const TileDef& d) { return d.hasBgColor ? d.bgColor : C_TILE; }
uint32_t tileOnFill(const TileDef& d) { return d.hasOnColor ? d.onColor : tileAccent(d); }
int tileTint(const TileDef& d) { return d.opacity >= 0 ? d.opacity : dispSettings.glassTint; }
int tileOnOpa(const TileDef& d) { return d.onOpacity >= 0 ? d.onOpacity : dispSettings.glassOn; }

// The icon's colour: "onIconColor" / "iconColor", else the accent; a plain icon
// on an on tile takes the text colour, as the accent would vanish on the on fill.
void tileIconColor(TileRt& r) {
  const TileDef& d = *r.def;
  if (!r.chip || (d.type != TILE_ACTION && d.type != TILE_SLIDER && d.type != TILE_SENSOR)) return;
  const bool on = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  if (on && d.hasOnIconColor) {
    lv_obj_set_style_text_color(r.chip, lv_color_hex(d.onIconColor), 0);
  } else if (on && d.iconPlain) {
    lv_obj_remove_local_style_prop(r.chip, LV_STYLE_TEXT_COLOR, 0);
  } else if (d.hasIconColor) {
    lv_obj_set_style_text_color(r.chip, lv_color_hex(d.iconColor), 0);
  } else {
    styleIconChip(r.chip, tileAccent(d));
  }
}

// Glass tiles are see-through: their glass is baked into the page
// background (bakeGlass); pressed and on states draw over it.
void glassStyle(lv_obj_t* t, const TileDef& def) {
  const int onPct = tileOnOpa(def);
  const lv_opa_t on = (lv_opa_t)(onPct * 255 / 100);
  lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_30, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(t, on, LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_border_width(t, 0, 0);
  // A faint fill alone hardly tells on from off: outline it in the accent.
  if (onPct < 50) {
    lv_obj_set_style_border_width(t, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(t, lv_color_hex(tileOnFill(def)), LV_STATE_CHECKED);
  }
}

// Text on an "on" tile: its own colour, else dark while the fill is bright
// enough, else the normal light text (Glass with a faint fill over a dark picture).
uint32_t onTextColor(const TileDef& def, bool glass) {
  if (def.hasOnTextColor) return def.onTextColor;
  if (!glass) return C_TEXT_ON;
  const uint32_t fill = tileOnFill(def);
  const uint32_t lum = ((fill >> 16 & 0xFF) * 299 + (fill >> 8 & 0xFF) * 587 + (fill & 0xFF) * 114) / 1000;
  return lum * tileOnOpa(def) / 100 >= 115 ? C_TEXT_ON : C_TEXT;
}

void makeTile(lv_obj_t* grid, const TileDef& def, int page, int col, int row, int w, int h) {
  const uint32_t accent = tileAccent(def);
  const bool glass = s_theme == THEME_GLASS && wallBlur();

  lv_obj_t* t = lv_obj_create(grid);
  lv_obj_set_size(t, 0, 0);  // the grid stretches it; LVGL's default 130 px would be fitted first
  lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, w, LV_GRID_ALIGN_STRETCH, row, h);
  lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(t, 16, 0);
  // Daylight tiles get a hairline, white on light grey is too soft alone.
  lv_obj_set_style_border_width(t, s_theme == THEME_DAYLIGHT ? 1 : 0, 0);
  lv_obj_set_style_border_color(t, lv_color_hex(C_TILE_LINE), 0);
  lv_obj_set_style_pad_all(t, 9, 0);
  const uint32_t onFill = tileOnFill(def);
  lv_obj_set_style_bg_color(t, lv_color_hex(tileBg(def)), 0);
  lv_obj_set_style_bg_color(t, lv_color_hex(onFill), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(t, lv_color_hex(tileText(def)), 0);
  lv_obj_set_style_text_color(t, lv_color_hex(onTextColor(def, glass)), LV_STATE_CHECKED);
  // Press feedback: the card darkens and shrinks a little, eased in 90 ms.
  lv_obj_set_style_bg_color(t, def.hasBgColor ? lv_color_mix(lv_color_hex(def.bgColor), lv_color_black(), 200)
                                              : lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(t, lv_color_mix(lv_color_hex(onFill), lv_color_black(), 200),
                            LV_STATE_PRESSED | LV_STATE_CHECKED);
  lv_obj_set_style_transform_width(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(t, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transition(t, &s_pressTrans, 0);
  lv_obj_set_style_transition(t, &s_pressTrans, LV_STATE_PRESSED);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(t, fxPressed, LV_EVENT_PRESS_LOST, nullptr);
  if (glass) glassStyle(t, def);

  if (def.type == TILE_PRINTER) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makePrinterContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyPrinterToTile(s_tiles.back());
    // The hero is clickable only while it has errors to list (applyPrinterToTile).
    if (def.show == PS_LIGHT || def.show == PS_SUMMARY || def.show == PS_HERO)
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
    if (def.show != PS_LIGHT && def.show != PS_SUMMARY && def.show != PS_HERO)
      lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_METRIC || (def.type == TILE_SENSOR && def.style == METRIC_CHART)) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    makeMetricContent(r, t);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    if (r.spark) {
      lv_obj_add_event_cb(r.viz, sparkDrawEvent, LV_EVENT_DRAW_MAIN, r.spark);
      lv_obj_add_event_cb(r.viz, sparkSizeEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    }
    if (def.type == TILE_METRIC) applyPcToTile(s_tiles.back());
    else {
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
    }
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  if (def.type == TILE_MEDIA) {
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    r.chip = lv_obj_create(t);
    lv_obj_remove_style_all(r.chip);
    lv_obj_remove_flag(r.chip, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(r.chip, 8, 0);
    lv_obj_set_style_clip_corner(r.chip, true, 0);
    lv_obj_set_style_bg_color(r.chip, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(r.chip, LV_OPA_COVER, 0);
    r.cover = lv_image_create(r.chip);
    lv_obj_remove_flag(r.cover, LV_OBJ_FLAG_CLICKABLE);
    showObj(r.cover, false);
    r.icon = lv_label_create(r.chip);
    mediaGlyph(r.icon, "music", "M", 32);
    lv_obj_center(r.icon);
    r.name = lv_label_create(t);
    r.sub = lv_label_create(t);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_opa(r.sub, LV_OPA_70, 0);
    r.viz = lv_bar_create(t);
    lv_obj_remove_style_all(r.viz);
    lv_obj_remove_flag(r.viz, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(r.viz, 0, 1000);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r.viz, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r.viz, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r.viz, 2, 0);
    lv_obj_set_style_radius(r.viz, 2, LV_PART_INDICATOR);
    const size_t idx = s_tiles.size();
    static const char* const icons[] = {"skip-previous", "play", "skip-next"};
    static const char* const fallback[] = {"|<", ">", ">|"};
    for (int i = 0; i < 3; i++) {
      lv_obj_t* button = r.mediaButtons[i] = lv_obj_create(t);
      lv_obj_remove_style_all(button);
      lv_obj_remove_flag(button, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
      lv_obj_set_style_radius(button, 8, 0);
      lv_obj_set_style_bg_color(button, lv_color_hex(C_TILE_PRESSED), LV_STATE_PRESSED);
      lv_obj_set_style_bg_opa(button, LV_OPA_50, LV_STATE_PRESSED);
      r.mediaIcons[i] = lv_label_create(button);
      mediaGlyph(r.mediaIcons[i], icons[i], fallback[i], 24);
      lv_obj_center(r.mediaIcons[i]);
      lv_obj_add_event_cb(button, mediaEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(idx * 4 + i));
    }
    s_tiles.push_back(r);
    applyMedia(s_tiles.back());
    lv_obj_add_event_cb(t, mediaEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)(idx * 4 + 3));
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Widget: its content, built by the widget (widget.h), in a centred column.
  if (def.type == TILE_WIDGET) {
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(t, 2, 0);
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    r.wdef = widget::find(def.widget.c_str());
    if (r.wdef) r.wstate = r.wdef->create(t, def.variant.c_str());
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    if (def.tap.kind != ACT_NONE || def.hold.kind != ACT_NONE) {
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
      lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
    } else {
      lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_add_event_cb(t, widgetSizeEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Clock: the time ("color") over the date, centred; fitClock sizes them.
  if (def.type == TILE_CLOCK) {
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);  // read-only
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    TileRt r = {};
    r.def = &def;
    r.page = page;
    r.obj = t;
    lv_obj_t* row = lv_obj_create(t);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_remove_flag(row, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t* hm = lv_obj_create(row);
    lv_obj_remove_style_all(hm);
    lv_obj_set_size(hm, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hm, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(hm, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    if (def.hasColor) lv_obj_set_style_text_color(hm, lv_color_hex(def.color), 0);
    r.sub = lv_label_create(hm);
    r.colon = lv_label_create(hm);
    lv_label_set_text(r.colon, ":");
    r.colonLit = true;
    r.mins = lv_label_create(hm);
    r.unit = lv_label_create(row);
    lv_obj_set_style_text_opa(r.unit, LV_OPA_70, 0);
    r.name = lv_label_create(t);
    lv_obj_set_style_text_opa(r.name, LV_OPA_70, 0);
    const size_t idx = s_tiles.size();
    s_tiles.push_back(r);
    applyClock(s_tiles.back());
    lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
    return;
  }

  // Slider: the level fill goes in first so it stays behind the content.
  lv_obj_t* fill = nullptr;
  if (def.type == TILE_SLIDER) {
    fill = lv_obj_create(t);
    lv_obj_remove_style_all(fill);
    lv_obj_add_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN));
    lv_obj_remove_flag(fill, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_style_radius(fill, 16, 0);
    lv_obj_set_style_bg_color(fill, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(fill, s_theme == THEME_DAYLIGHT ? LV_OPA_40 : LV_OPA_50, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_PRESS_LOCK);  // the drag may leave the tile
  }

  lv_obj_t* chip = lv_obj_create(t);
  lv_obj_remove_flag(chip, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_size(chip, 30, 30);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  styleIconChip(chip, accent);
  if (def.iconPlain) lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
  if (def.hasIconColor) lv_obj_set_style_text_color(chip, lv_color_hex(def.iconColor), 0);
  lv_obj_t* ic = lv_label_create(chip);
  lv_obj_center(ic);

  // Fonts and places come from fitTile once the tile has its size.
  lv_obj_t* name = lv_label_create(t);
  lv_label_set_text(name, def.noName ? "" : tileLabel(def).c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_t* sub = lv_label_create(t);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
  if (def.type == TILE_SENSOR) {
    lv_obj_set_style_text_opa(name, LV_OPA_70, 0);
  } else {
    lv_obj_set_style_text_opa(sub, LV_OPA_70, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_RIGHT, 0);
  }

  const int iconPx = def.type == TILE_SENSOR ? 16 : 20;
  TileRt r = {};
  r.def = &def;
  r.page = page;
  r.obj = t;
  r.name = name;
  r.sub = sub;
  r.icon = ic;
  r.chip = chip;
  r.iconPx = iconPx;
  r.iconCp = showIcon(ic, def, false, iconPx, 0);
  r.fill = fill;
  if (def.type == TILE_SENSOR) {
    r.detail = makeDetail(t, def);
    r.rows = makeRows(t, def);
  }
  lv_label_set_text(sub, tileSub(r).c_str());
  const size_t idx = s_tiles.size();
  s_tiles.push_back(r);
  if (def.tap.kind == ACT_PC) applyPcStateToTile(s_tiles.back());
  lv_obj_add_event_cb(t, fitEvent, LV_EVENT_SIZE_CHANGED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)idx);
  lv_obj_add_event_cb(t, tileEvent, LV_EVENT_LONG_PRESSED, (void*)(uintptr_t)idx);
  if (fill)
    for (lv_event_code_t c : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST,
                              LV_EVENT_SIZE_CHANGED})
      lv_obj_add_event_cb(t, sliderEvent, c, (void*)(uintptr_t)idx);
}

}  // namespace deckui
