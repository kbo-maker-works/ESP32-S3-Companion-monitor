// Deck UI: tiles bound to HTTP sensors and the PC; tile actions.
#include "deck_ui_int.h"

namespace deckui {

// --- Tile events ------------------------------------------------------------------------

const char* pcCmdLabel(const String& cmd);

uint32_t tileTextRt(const TileRt& r) { return r.hasAuto ? r.autoText : tileText(*r.def); }

// onTile: the reading on the tile itself, which follows Glass auto text.
uint32_t sensorColour(const TileRt& r, bool onTile) {
  return r.threshold == 2 ? C_CRIT : r.threshold == 1 ? C_WARN : onTile ? tileTextRt(r) : tileText(*r.def);
}

uint32_t sensorLine(const TileRt& r) {
  const TileDef& d = *r.def;
  return r.threshold ? sensorColour(r) : d.hasChartColor ? d.chartColor : d.hasColor ? d.color : C_ACCENT;
}

// 0 = fine, 1 = warn, 2 = crit; prev is the level shown now (hysteresis).
uint8_t thresholdLevel(const TileDef& d, float value, uint8_t prev) {
  const bool low = d.hasWarn && d.hasCrit && d.crit < d.warn;
  auto active = [&](float threshold, bool held) {
    const float margin = held ? max(0.01f, fabsf(threshold) * 0.01f) : 0;
    return low ? (held ? value < threshold + margin : value <= threshold) :
                 (held ? value > threshold - margin : value >= threshold);
  };
  if (d.hasCrit && active(d.crit, prev == 2)) return 2;
  if (d.hasWarn && active(d.warn, prev >= 1)) return 1;
  return 0;
}

void sensorThreshold(TileRt& r, bool numeric, float value) {
  r.threshold = numeric ? thresholdLevel(*r.def, value, r.threshold) : 0;
  lv_obj_set_style_text_color(r.sub, lv_color_hex(sensorColour(r, true)), 0);
  if (r.viz && r.ser) lv_chart_set_series_color(r.viz, r.ser, lv_color_hex(sensorLine(r)));
}

// Leave room for axis padding and the LVGL missing-point sentinel.
int32_t historyPoint(float value) {
  return (int32_t)lround(constrain((double)value * 10, -1000000000.0, 1000000000.0));
}

void plotHistory(lv_obj_t* chart, int32_t* points, const float* values, int count) {
  // Scale to the plotted readings, with padding above and below.
  int32_t low = INT32_MAX, high = INT32_MIN;
  for (int i = 0; i < count; i++) {
    points[i] = isfinite(values[i]) ? historyPoint(values[i]) : LV_CHART_POINT_NONE;
    if (points[i] == LV_CHART_POINT_NONE) continue;
    low = min(low, points[i]);
    high = max(high, points[i]);
  }
  if (low <= high) {
    const int32_t pad = (int32_t)((int64_t)(high - low) * 15 / 100) + 5;
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, low - pad, high + pad);
  }
  lv_chart_refresh(chart);
}

// A reading with at most one decimal.
void formatReading(float v, char* out, size_t n) {
  snprintf(out, n, fabsf(v - roundf(v)) < 0.05f ? "%.0f" : "%.1f", v);
}

// A number in s / min / h / d as "45s", "31m", "1h 49m", "3d 2h"; false for
// other units.
bool formatDuration(float v, const String& unit, char* out, size_t n) {
  const double scale = unit == "ms" ? 0.001 : unit == "s" ? 1 : unit == "min" ? 60 :
                       unit == "h" ? 3600 : unit == "d" ? 86400 : 0;
  if (!scale || !isfinite(v)) return false;
  const double secs = fmin(fabs(v * scale), 4e9);
  const char* sign = v < 0 ? "-" : "";
  const uint32_t t = (uint32_t)secs, d = t / 86400, h = t % 86400 / 3600, m = t % 3600 / 60;
  if (t < 60 && scale < 60) snprintf(out, n, "%s%lus", sign, (unsigned long)t);
  else if (t < 3600) snprintf(out, n, "%s%lum", sign, (unsigned long)m);
  else if (t < 86400) snprintf(out, n, m ? "%s%luh %lum" : "%s%luh", sign, (unsigned long)h, (unsigned long)m);
  else snprintf(out, n, h ? "%s%lud %luh" : "%s%lud", sign, (unsigned long)d, (unsigned long)h);
  return true;
}

void setTileOn(TileRt& r, bool on) {
  const bool was = lv_obj_has_state(r.obj, LV_STATE_CHECKED);
  r.on = on;
  if (was != on) fxBeforeTileOn(r, on);
  if (on) lv_obj_add_state(r.obj, LV_STATE_CHECKED);
  else lv_obj_remove_state(r.obj, LV_STATE_CHECKED);
  if (was != on && r.def->onIcon.length() && r.icon)
    r.iconCp = showIcon(r.icon, *r.def, on, r.iconPx, r.iconCp);
  tileIconColor(r);
  fxTileOn(r, was);
}

// --- Slider tiles ------------------------------------------------------------------------
void setSliderFill(TileRt& r, float frac) {
  r.frac = frac < 0 ? 0 : frac;
  const int w = lv_obj_get_width(r.obj), h = lv_obj_get_height(r.obj);
  if (w <= 0) return;  // not laid out yet: SIZE_CHANGED comes back here
  // The fill starts at the tile edge, outside the padding.
  const int pad = lv_obj_get_style_pad_left(r.obj, LV_PART_MAIN);
  lv_obj_set_pos(r.fill, -pad, -pad);
  lv_obj_set_size(r.fill, (int)lroundf(w * r.frac), h);
  if (r.frac <= 0) lv_obj_add_flag(r.fill, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(r.fill, LV_OBJ_FLAG_HIDDEN);
}

// Finished PC commands: a short toast either way.
void pcCmdSync() {
  PcCmdResult o;
  while (pcTakeCmdResult(o)) {
    if (o.ok && o.cmd.startsWith("volume_set:")) continue;  // the slider is the feedback
    const String t = o.pcName + ": " + pcCmdLabel(o.cmd) + (o.ok ? "" : String(" - ") + (o.msg.length() ? o.msg : "failed"));
    deckToastShow(t.c_str(), o.ok ? 1200 : 2500);
  }
}

uint32_t s_httpSeen = 0;
uint32_t s_prnSeen = 0;  // printersVersion() last applied
bool s_prnRebuild = false;
String s_keepPage;  // buildPages: show this page again (set by rebuildPages)

// "when" tiles. A PC counts as off only after this long offline, on top of
// PC_STALE_TIMEOUT_MS, so a companion restart does not reshuffle the page.
constexpr uint32_t WHEN_PC_GRACE_MS = 45000;
struct WhenPc {
  String id;  // "" = the first known PC
  bool on = false;
  uint32_t offSince = 0;
};
std::vector<WhenPc> s_whenPcs;

const String& tilePcId(const TileDef& d) { return d.tap.kind == ACT_PC ? d.tap.pc : d.pc; }

bool pcIsOnline(const String& id) {
  const PcSource* src = pcSource(id);
  return src && src->data.online;
}

// pcId: the PC a pc-on / pc-off condition follows ("" = the first known one).
bool whenWanted(const WhenDef& w, const String& pcId) {
  if (w.kind == WHEN_PC_ON || w.kind == WHEN_PC_OFF) {
    bool on = false;
    for (const WhenPc& p : s_whenPcs)
      if (p.id == pcId) on = p.on;
    return on == (w.kind == WHEN_PC_ON);
  }
  return true;
}

bool whenWanted(const TileDef& t) { return whenWanted(t.when, tilePcId(t)); }
bool whenWanted(const PageDef& p) { return whenWanted(p.when, String()); }

void whenWatch(const WhenDef& w, const String& pcId, std::vector<WhenPc>& pcs) {
  if (w.kind != WHEN_ALWAYS) {
    for (const WhenPc& p : pcs)
      if (p.id == pcId) return;
    WhenPc n;
    n.id = pcId;
    n.on = pcIsOnline(pcId);
    for (const WhenPc& p : s_whenPcs)
      if (p.id == pcId) n = p;
    pcs.push_back(n);
  }
}

// The PCs the "when" tiles and pages watch, keeping what is already known.
void whenCollect() {
  std::vector<WhenPc> pcs;
  for (const PageDef& pd : s_cfg.pages) {
    whenWatch(pd.when, String(), pcs);
    for (const TileDef& t : pd.tiles) whenWatch(t.when, tilePcId(t), pcs);
  }
  s_whenPcs.swap(pcs);
}

void whenTrack() {
  const uint32_t now = millis();
  for (WhenPc& p : s_whenPcs) {
    if (pcIsOnline(p.id)) {
      p.on = true;
      p.offSince = 0;
    } else if (p.on) {
      if (!p.offSince) p.offSince = now | 1;
      else if (now - p.offSince >= WHEN_PC_GRACE_MS) p.on = false;
    }
  }
}

// The layout follows the conditions as they are now.
void whenApply() {
  whenCollect();
  for (PageDef& pd : s_cfg.pages) {
    pd.shown = whenWanted(pd);
    for (TileDef& t : pd.tiles) t.shown = whenWanted(t);
  }
}

// A template (detail line, row value, name): literal text and fields from the
// same reply ("--" while missing).
String httpTemplate(const TileDef& d, const String& tmpl) {
  String out;
  for (const DetailPart& part : httpParseDetail(tmpl)) {
    if (!part.field) { out += part.text; continue; }
    HttpSensorValue v;
    char buf[48];
    if (!httpSensorRead(d.http, part.text, v) || v.offline || !v.present) out += "--";
    else if (!v.numeric) out += v.text ? v.text.get() : "--";
    else {
      if (!part.unit.length() || !formatDuration(v.number, part.unit, buf, sizeof(buf)))
        formatReading(v.number, buf, sizeof(buf));
      out += buf;
    }
  }
  return out;
}

// Rows put "%" and "°" right after the number ("8%").
String rowUnit(const TileDef& d) {
  if (!d.unit.length()) return "";
  return d.unit.startsWith("%") || d.unit.startsWith("\xC2\xB0") ? d.unit : " " + d.unit;
}

// Row i's value text: its "value" template when asked for, else the reading.
String rowText(const TileDef& d, size_t i, bool useTemplate) {
  const SensorRowDef& row = d.rows[i];
  if (useTemplate && row.value.length()) return httpTemplate(d, row.value);
  HttpSensorValue v;
  if (!httpSensorRead(d.http, row.path, v) || v.offline || !v.present) return "--";
  if (!v.numeric) return v.text ? v.text.get() : "--";
  char number[48];
  if (d.duration && formatDuration(v.number, d.unit, number, sizeof(number))) return number;
  formatReading(v.number, number, sizeof(number));
  return String(number) + rowUnit(d);
}

// Each row box holds label, bar, value; its user data is the threshold level.
// layoutRows sets the value texts.
void applyHttpRows(TileRt& r) {
  const TileDef& d = *r.def;
  const float full = d.scaleMax ? d.scaleMax : 100;
  const uint32_t line = d.hasChartColor ? d.chartColor : d.hasColor ? d.color : C_ACCENT;
  bool dim = true;
  for (size_t i = 0; i < d.rows.size() && i < lv_obj_get_child_count(r.rows); i++) {
    lv_obj_t* box = lv_obj_get_child(r.rows, i);
    lv_obj_t* value = lv_obj_get_child(box, 2);
    HttpSensorValue v;
    const bool found = httpSensorRead(d.http, d.rows[i].path, v);
    dim = dim && (!found || v.offline);
    const bool numeric = found && !v.offline && v.present && v.numeric;
    const uint8_t level = numeric ? thresholdLevel(d, v.number, (uint8_t)(uintptr_t)lv_obj_get_user_data(box)) : 0;
    lv_obj_set_user_data(box, (void*)(uintptr_t)level);
    // Level 0 inherits the tile text colour, so Glass auto text reaches it.
    if (level) lv_obj_set_style_text_color(value, lv_color_hex(level == 2 ? C_CRIT : C_WARN), 0);
    else lv_obj_remove_local_style_prop(value, LV_STYLE_TEXT_COLOR, 0);
    lv_obj_t* bar = lv_obj_get_child(box, 1);
    lv_bar_set_value(bar, numeric ? (int32_t)lround(constrain(v.number * 100 / full, 0.0f, 100.0f)) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(level == 2 ? C_CRIT : level ? C_WARN : line), LV_PART_INDICATOR);
  }
  lv_obj_set_style_opa(r.obj, dim ? LV_OPA_50 : LV_OPA_COVER, 0);
  layoutRows(r);
  markPageStale(r.page);
}

void applyHttpToTile(TileRt& r) {
  const TileDef& d = *r.def;
  if (!d.http.enabled) return;
  if (d.label.indexOf('{') >= 0) setText(r.name, httpTemplate(d, d.label).c_str());
  if (r.rows) {
    applyHttpRows(r);
    return;
  }
  HttpSensorValue value;
  const bool found = httpSensorRead(d.http, d.http.path, value);
  const bool numeric = found && !value.offline && value.present && value.numeric;
  String text = "--";
  char number[48];
  const bool dur = numeric && d.duration && formatDuration(value.number, d.unit, number, sizeof(number));
  if (dur) text = number;
  else if (numeric) {
    formatReading(value.number, number, sizeof(number));
    text = number;
    if (d.style != METRIC_CHART && d.unit.length())
      text += (d.unit.startsWith("\xC2\xB0") ? "" : " ") + d.unit;
  }
  const char* shown = found && !value.offline && value.present && !value.numeric && value.text
                          ? value.text.get() : text.c_str();
  lv_label_set_text(r.sub, shown);
  if (r.unit) lv_label_set_text(r.unit, numeric && !dur ? d.unit.c_str() : "");
  if (r.detail) lv_label_set_text(r.detail, httpTemplate(d, d.detail).c_str());
  lv_obj_set_style_opa(r.obj, !found || value.offline ? LV_OPA_50 : LV_OPA_COVER, 0);
  sensorThreshold(r, numeric, value.number);
  if (found && r.viz && r.pts) plotHistory(r.viz, r.pts, value.history, HTTP_HISTORY_POINTS);
  fitValue(r);
  markPageStale(r.page);
}

// Finished HTTP tile actions: a short toast either way.
void httpSync() {
  const uint32_t version = httpSensorVersion();
  if (version != s_httpSeen) {
    s_httpSeen = version;
    for (TileRt& r : s_tiles) applyHttpToTile(r);
  }
  HttpOutcome o;
  while (httpTakeResult(o)) {
    const bool ok = o.code >= 200 && o.code < 300;
    const String t = o.label + ": " + (ok ? String("OK") : o.code > 0 ? "HTTP " + String(o.code) : o.err);
    deckToastShow(t.c_str(), ok ? 1200 : 2500);
  }
}

// New pages: printers and HTTP sensors follow the new tiles.
void bindTiles() {
  printersApply(s_cfg);
  s_prnSeen = 0;
  httpSetSensors(s_cfg);
  s_httpSeen = 0;
  for (TileRt& r : s_tiles) applyHttpToTile(r);
}

void runAction(TileRt& r, const TileAction& a) {
  switch (a.kind) {
    case ACT_PAGE:
      goToPage(a.page);
      break;
    case ACT_DETAIL:
      openModal((int)(&r - &s_tiles[0]));
      break;
    case ACT_HTTP:
      if (!httpSend(a, tileLabel(*r.def))) deckToastShow("Busy, try again", 1500);
      break;
    case ACT_PC: {
      String err;
      if (!pcSendCommand(a.pc, a.cmd.c_str(), err)) deckToastShow(err.c_str(), 2200);
      break;
    }
    case ACT_APP:
      openApp((int)(&r - &s_tiles[0]), a.app);
      break;
    default:
      deckToastShow("No action set for this tile", 1500);
      break;
  }
}

const char* pcCmdLabel(const String& cmd) {
  static const char* const kLabels[][2] = {
      {"media_play_pause", "Play / pause"}, {"media_next", "Next track"}, {"media_prev", "Previous track"},
      {"media_stop", "Stop"},               {"volume_up", "Volume up"},   {"volume_down", "Volume down"},
      {"volume_mute", "Mute"},              {"lock", "Lock"},             {"sleep", "Sleep"},
      {"restart", "Restart"},               {"shutdown", "Shut down"}};
  for (const auto& l : kLabels)
    if (cmd == l[0]) return l[1];
  if (cmd.startsWith("hotkey:")) return "Hotkey";
  if (cmd.startsWith("volume_set:")) return "Volume";
  return "Run";
}

// Tile setting "confirm": "always" asks before any action that does something.
// "off" asked before turning a tile off; no action does that any more.
bool needsConfirm(const TileRt& r, const TileAction& a) {
  if (a.kind == ACT_PAGE || a.kind == ACT_DETAIL || a.kind == ACT_APP) return false;
  return r.def->confirm == CONFIRM_ALWAYS;
}

// Button text for what the action is about to do.
const char* actionVerb(const TileRt&, const TileAction& a) {
  if (a.kind == ACT_HTTP) return "Send";
  if (a.kind == ACT_PC) return pcCmdLabel(a.cmd);
  if (a.kind == ACT_APP) return "Abrir";
  return "Run";
}

void requestAction(TileRt& r, bool hold) {
  if (r.def->type == TILE_PRINTER) {
    if (!hold) printerTap(r);
    return;
  }
  const TileAction& a = hold ? r.def->hold : r.def->tap;
  if (r.def->http.enabled && a.kind == ACT_NONE) return;
  if (needsConfirm(r, a)) openConfirm((int)(&r - &s_tiles[0]), hold);
  else runAction(r, a);
}

void tileEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  // SHORT_CLICKED, not CLICKED: CLICKED also fires on the release after a
  // long press, which would run the tap action as well as the hold action.
  if (r.sliding) return;  // the press was a slider drag
  hapticTap();
  audioClick();
  if (code == LV_EVENT_SHORT_CLICKED) requestAction(r, false);
  else if (code == LV_EVENT_LONG_PRESSED) requestAction(r, true);
}

// PC volume slider: the level the finger asks for goes out at most every
// LEVEL_GAP_MS (each command spends a companion nonce); the tile shows it
// until PC_VOL_HOLD_MS after the last move, then follows the PC again.
constexpr uint32_t LEVEL_GAP_MS = 300;
constexpr uint32_t PC_VOL_HOLD_MS = 1500;
struct PcVolume {
  String pc;
  int want = -1, sent = -1;
  uint32_t sentMs = 0, holdUntil = 0;
  bool reapply = false;
};
PcVolume s_pcVol;

bool pcVolHeld() { return (int32_t)(millis() - s_pcVol.holdUntil) < 0; }

void pcVolumeTick() {
  const uint32_t now = millis();
  if (s_pcVol.want >= 0 && s_pcVol.want != s_pcVol.sent && now - s_pcVol.sentMs >= LEVEL_GAP_MS) {
    String err;
    s_pcVol.sentMs = now;
    if (pcSendCommand(s_pcVol.pc, ("volume_set:" + String(s_pcVol.want)).c_str(), err)) s_pcVol.sent = s_pcVol.want;
    else if (!pcVolHeld()) {
      s_pcVol.want = -1;
      deckToastShow(err.c_str(), 2200);
    }
  }
  if (s_pcVol.reapply && !pcVolHeld()) {
    s_pcVol.reapply = false;
    s_pcVol.want = s_pcVol.sent = -1;
    for (TileRt& r : s_tiles)
      if (r.def->pcVolume && !r.sliding) applyPcStateToTile(r);
  }
}

void pcVolumeDrag(TileRt& r, lv_event_code_t code, const lv_point_t& p) {
  if (code == LV_EVENT_PRESSING) {
    if (!r.sliding) {
      if (!pcIsOnline(tilePcId(*r.def)) || modalShown() || abs(p.x - r.slX0) < 8) return;
      r.sliding = true;
      r.slX0 = p.x;
      r.slFrac0 = r.slValue = r.frac;
    }
    const int w = lv_obj_get_width(r.obj);
    const float f = constrain(r.slFrac0 + (p.x - r.slX0) / (float)(w > 0 ? w : 1), 0.0f, 1.0f);
    if (fabsf(f - r.slValue) < 0.005f) return;
    r.slValue = f;
    setSliderFill(r, f);
    s_pcVol.pc = tilePcId(*r.def);
    s_pcVol.want = (int)lroundf(f * 100);
    if (!r.def->sub.length()) lv_label_set_text(r.sub, (String(s_pcVol.want) + "%").c_str());
  } else if (code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
    return;
  }
  if (!r.sliding) return;
  s_pcVol.holdUntil = millis() + PC_VOL_HOLD_MS;
  s_pcVol.reapply = true;
  markPageStale(r.page);
}

// Slider tile: a sideways drag moves the level from where it was (no jump to
// the finger).
void sliderEvent(lv_event_t* e) {
  const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_tiles.size()) return;
  TileRt& r = s_tiles[idx];
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_SIZE_CHANGED) {
    setSliderFill(r, r.frac);
    return;
  }
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (code == LV_EVENT_PRESSED) {
    r.sliding = false;
    r.slX0 = p.x;
    return;
  }
  pcVolumeDrag(r, code, p);
}

}  // namespace deckui
