// Deck UI: toast, detail card, confirm card and the PC pairing card.
#include "ui/deck_ui_int.h"
#include "services/deck_daily.h"
#include "apps/app.h"
#include <src/misc/cache/instance/lv_image_cache.h>

namespace deckui {

// --- Toast and detail card ------------------------------------------------------------
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_modalScrim = nullptr;
lv_obj_t* s_modalCard = nullptr;
lv_draw_buf_t* s_modalGlass = nullptr;  // Glass theme: the card's frosted background
FrostBands s_appGlass;                  // the same for an app, screen sized
int s_modalTile = -1;
lv_obj_t* s_modalState = nullptr;
lv_obj_t* s_modalIcon = nullptr;
uint32_t s_modalIconCp = 0;
// Confirm dialog (shares the card slot): which action waits, the tile state it
// was asked for, and the auto-cancel timer.
int8_t s_confirmHold = -1;  // -1 = the card is a detail card
bool s_confirmOn = false;
uint32_t s_confirmMs = 0;
lv_timer_t* s_confirmTimer = nullptr;
constexpr uint32_t CONFIRM_TIMEOUT_MS = 8000;
constexpr uint32_t CONFIRM_ARM_MS = 400;  // a stray second tap must not confirm

bool modalShown() { return s_modalCard != nullptr; }

// The app open in the card slot, if any (see openApp).
const app::Def* s_app = nullptr;
bool appShown() { return s_app != nullptr; }
bool appKeepsAwake() { return s_app && s_app->awake; }
const char* openAppId() { return s_app ? s_app->id : ""; }

// The page under a card, dimmed like the scrim, as an opaque picture on top of
// the screen: LVGL skips the page under it while the card is open.
lv_obj_t* s_modalUnder = nullptr;
lv_draw_buf_t* s_underBuf = nullptr;  // kept once used

// The cover's picture: the last frame at half brightness, as the compositor dims.
void coverFill() {
  if (!s_underBuf) return;
  const int w = logicalW(), h = logicalH();
  const uint16_t* fb = lastFrame();
  for (int y = 0; y < h; y++) {
    uint16_t* d = (uint16_t*)(s_underBuf->data + y * s_underBuf->header.stride);
    const uint16_t* s = fb + y * w;
    for (int x = 0; x < w; x++) d[x] = (s[x] >> 1) & 0x7BEF;
  }
}

// Only with no card open; the next card allocates it again.
void cardUnderFree() {
  if (s_modalUnder || !s_underBuf) return;
  lv_image_cache_drop(s_underBuf);
  lv_draw_buf_destroy(s_underBuf);
  s_underBuf = nullptr;
}

// fill false: the caller runs coverFill before LVGL draws again.
bool coverPage(bool fill) {
  const int w = logicalW(), h = logicalH();
  if (s_underBuf && (s_underBuf->header.w != w || s_underBuf->header.h != h)) {
    lv_draw_buf_destroy(s_underBuf);
    s_underBuf = nullptr;
  }
  if (!s_underBuf) s_underBuf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, 0);
  if (!s_underBuf) return false;
  lv_image_cache_drop(s_underBuf);
  if (fill) coverFill();
  s_modalUnder = lv_image_create(lv_screen_active());
  lv_obj_add_flag(s_modalUnder, (lv_obj_flag_t)(LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_FLOATING));
  lv_obj_remove_flag(s_modalUnder, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(s_modalUnder, 0, 0);
  lv_image_set_src(s_modalUnder, s_underBuf);
  // An image alone never reports "covers": the opaque background does.
  lv_obj_set_style_bg_color(s_modalUnder, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_modalUnder, LV_OPA_COVER, 0);
  return true;
}

void onModalScrim(lv_event_t*);

// The scrim for a card: transparent over the covered page, else half black.
lv_obj_t* makeScrim(bool fill = true) {
  lv_obj_t* s = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s);
  lv_obj_set_size(s, logicalW(), logicalH());
  lv_obj_set_style_bg_color(s, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s, coverPage(fill) ? LV_OPA_TRANSP : LV_OPA_50, 0);
  lv_obj_add_event_cb(s, onModalScrim, LV_EVENT_CLICKED, nullptr);
  return s;
}

// Thin line on the top edge while daily pictures download: swipes may hitch then.
lv_obj_t* s_dailyBar = nullptr;
lv_obj_t* s_dailyPill = nullptr;  // "Daily pictures 3 of 20, tap to skip"

void dailySkipTap(lv_event_t*) {
  deck::dailySkip();
  hapticTap();
}

void dailyBarSync() {
  int done = 0, total = 0;
  if (!deck::dailyProgress(done, total) || !total) {
    if (s_dailyBar) lv_obj_add_flag(s_dailyBar, LV_OBJ_FLAG_HIDDEN);
    if (s_dailyPill) lv_obj_add_flag(s_dailyPill, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // The PC link and the printers are paused meanwhile: say why, and offer a way out.
  if (!s_dailyPill) {
    s_dailyPill = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dailyPill);
    lv_obj_set_size(s_dailyPill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_dailyPill, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s_dailyPill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_dailyPill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_dailyPill, 14, 0);
    lv_obj_set_style_pad_ver(s_dailyPill, 7, 0);
    lv_obj_set_style_text_color(s_dailyPill, lv_color_hex(0xE6E8EC), 0);
    lv_obj_add_event_cb(s_dailyPill, dailySkipTap, LV_EVENT_CLICKED, nullptr);
    lv_label_create(s_dailyPill);
  }
  char text[64];
  snprintf(text, sizeof(text), "Daily pictures %d of %d, tap to skip", std::min(done + 1, total), total);
  lv_obj_t* label = lv_obj_get_child(s_dailyPill, 0);
  if (strcmp(lv_label_get_text(label), text)) lv_label_set_text(label, text);
  lv_obj_align(s_dailyPill, LV_ALIGN_TOP_MID, 0, 8);
  if (lv_obj_has_flag(s_dailyPill, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_remove_flag(s_dailyPill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_dailyPill);
  }
  if (!s_dailyBar) {
    s_dailyBar = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dailyBar);
    lv_obj_set_style_bg_color(s_dailyBar, lv_color_hex(C_OK), 0);
    lv_obj_set_style_bg_opa(s_dailyBar, LV_OPA_80, 0);
    lv_obj_remove_flag(s_dailyBar, LV_OBJ_FLAG_CLICKABLE);
  }
  // Half a step for the picture in progress, so the line moves from the start.
  lv_obj_set_size(s_dailyBar, std::max(6, logicalW() * (2 * done + 1) / (2 * total)), 3);
  lv_obj_set_pos(s_dailyBar, 0, 0);
  lv_obj_remove_flag(s_dailyBar, LV_OBJ_FLAG_HIDDEN);
}

void setModalOpen(bool open) {
  portENTER_CRITICAL(&s_mux);
  sh.modalOpen = open;
  portEXIT_CRITICAL(&s_mux);
}

void toastOpa(void* obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }
void toastDone(lv_anim_t* a) {
  if (lv_obj_get_style_opa((lv_obj_t*)a->var, LV_PART_MAIN) == LV_OPA_TRANSP)
    lv_obj_add_flag((lv_obj_t*)a->var, LV_OBJ_FLAG_HIDDEN);
}

void deckToastShow(const char* text, uint32_t holdMs) {
  if (!s_toast) {
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 9, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_hex(0xE6E8EC), 0);  // dark pill on both themes
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_label_create(s_toast);
  }
  lv_obj_t* label = lv_obj_get_child(s_toast, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_max_width(label, logicalW() - 40 - 32, 0);
  lv_label_set_text(label, text);
  // Above the page dots, which the flush task draws at the very bottom.
  lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_toast);  // above cards and the editor made after it
  lv_anim_delete(s_toast, toastOpa);
  lv_anim_t in;
  lv_anim_init(&in);
  lv_anim_set_var(&in, s_toast);
  lv_anim_set_exec_cb(&in, toastOpa);
  lv_anim_set_values(&in, lv_obj_get_style_opa(s_toast, LV_PART_MAIN), LV_OPA_COVER);
  lv_anim_set_duration(&in, 120);
  lv_anim_start(&in);
  lv_anim_t out;
  lv_anim_init(&out);
  lv_anim_set_var(&out, s_toast);
  lv_anim_set_exec_cb(&out, toastOpa);
  lv_anim_set_values(&out, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_delay(&out, 120 + holdMs);
  lv_anim_set_duration(&out, 250);
  lv_anim_set_completed_cb(&out, toastDone);
  lv_anim_start(&out);
}

// --- Apps ------------------------------------------------------------------------
// A detail card the size of the screen: the deck's top bar, the app's own view
// below it. Same opening and closing animation as a card. Back is a swipe in
// from the left edge, as on a phone.
constexpr int APP_EDGE_W = 28;     // touch must start this close to the left edge
constexpr int APP_BACK_DX = 60;    // and travel this far right
void cleanPage();

lv_point_t s_edgeStart;
bool s_edgeDone = false;

void onAppEdge(lv_event_t* e) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
    s_edgeStart = p;
    s_edgeDone = false;
    return;
  }
  const int dx = p.x - s_edgeStart.x, dy = p.y - s_edgeStart.y;
  if (s_edgeDone || dx < APP_BACK_DX || abs(dy) > dx) return;
  s_edgeDone = true;
  hapticTap();
  fxCardClose(s_modalCard, s_modalTile);
}

void openApp(int tileIdx, const String& id) {
  const app::Def* a = app::find(id.c_str());
  if (!a) {
    deckToastShow("Unknown app", 1500);
    return;
  }
  closeModal();
  cleanPage();
  const int w = logicalW(), h = logicalH();
  s_modalTile = tileIdx;
  fxCardOpenBegin(tileIdx, {0, 0, (int16_t)w, (int16_t)h}, 0);

  s_modalScrim = makeScrim(false);
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, w, h);
  lv_obj_set_pos(c, 0, 0);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  if (s_theme == THEME_GLASS && frostedBands(s_appGlass, 0, 0, w, h)) frostBandsStyle(c, &s_appGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  // The deck's top bar stays on screen above the app (when it is on).
  const int top = s_header ? HEADER_H : 0;
  s_appHdr = {};
  if (s_header) {
    s_appHdr.root = c;
    buildHeader(s_appHdr, c);
  }

  const int bodyH = h - top;
  lv_obj_t* body = lv_obj_create(c);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, 0, top);
  lv_obj_set_size(body, w, bodyH);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  widgetsRelease();  // the memory the app is about to take (the radar widget's pictures)
  s_app = a;
  setModalOpen(true);
  a->open(body, w, bodyH);

  // Invisible strip on the left edge, above the app's view: swipe right = back.
  lv_obj_t* edge = lv_obj_create(c);
  lv_obj_remove_style_all(edge);
  lv_obj_set_pos(edge, 0, top);
  lv_obj_set_size(edge, APP_EDGE_W, bodyH);
  lv_obj_add_flag(edge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(edge, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE |
                                           LV_OBJ_FLAG_EVENT_BUBBLE));
  lv_obj_add_event_cb(edge, onAppEdge, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(edge, onAppEdge, LV_EVENT_PRESSING, nullptr);
  fxCardOpen(c, tileIdx);
  coverFill();
}

void appTick() {
  if (s_app && s_app->tick && s_modalCard) s_app->tick();
}

void closeModal() {
  if (s_app) {
    if (s_app->close) s_app->close();
    s_app = nullptr;
  }
  if (s_modalCard) lv_obj_delete(s_modalCard);
  s_appHdr = {};  // its objects went with the card
  if (s_modalScrim) lv_obj_delete(s_modalScrim);
  if (s_modalUnder) lv_obj_delete(s_modalUnder);
  s_modalCard = s_modalScrim = s_modalUnder = nullptr;
  if (s_modalGlass) lv_draw_buf_destroy(s_modalGlass);
  s_modalGlass = nullptr;
  frostBandsFree(s_appGlass);
  s_modalIcon = nullptr;
  s_modalTile = -1;
  s_confirmHold = -1;
  if (s_confirmTimer) lv_timer_delete(s_confirmTimer);
  s_confirmTimer = nullptr;
  setModalOpen(false);
}

// Calm: the detail card shrinks back into its tile; confirm cards just close.
void onModalScrim(lv_event_t*) {
  if (s_confirmHold < 0) fxCardClose(s_modalCard, s_modalTile);
  else closeModal();
}

void onModalPrimary(lv_event_t*) {
  if (s_modalTile < 0 || s_modalTile >= (int)s_tiles.size()) return;
  TileRt& r = s_tiles[s_modalTile];
  if (needsConfirm(r, r.def->tap)) {
    openConfirm(s_modalTile, false);  // replaces this card
    return;
  }
  runAction(r, r.def->tap);
  refreshModal();
}

void refreshModal() {
  if (s_modalTile < 0 || !s_modalCard || s_app) return;
  const TileRt& r = s_tiles[s_modalTile];
  if (s_confirmHold >= 0) {
    // The state changed under the question: ask again.
    if (r.on != s_confirmOn) closeModal();
    return;
  }
  lv_label_set_text(s_modalState, tileSub(r).c_str());
  s_modalIconCp = showIcon(s_modalIcon, *r.def, r.on, 28, s_modalIconCp);
}

// Detail card for a tile (hold action "more-info"): name, state and the tap
// action as a button.
// Touch effects stop, and LVGL draws the page once without them: the card's
// background and animation take their picture of the page from that frame.
void cleanPage() {
  if (fxCancelRipples()) lv_refr_now(s_disp);
}

void openModal(int tileIdx) {
  closeModal();
  cleanPage();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : C_ACCENT;
  s_modalTile = tileIdx;

  const int cw = w - 40 < 380 ? w - 40 : 380;
  const int ch = 196;
  fxCardOpenBegin(tileIdx, {(int16_t)((w - cw) / 2), (int16_t)((h - ch) / 2), (int16_t)cw, (int16_t)ch}, 22);

  s_modalScrim = makeScrim(false);  // filled below, while the animation runs
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS &&
      (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);  // taps on the card stay on the card
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 44, 44);
  lv_obj_set_pos(chip, 16, 16);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  styleIconChip(chip, accent);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  s_modalIcon = lv_label_create(chip);
  s_modalIconCp = 0;
  lv_label_set_text(s_modalIcon, "");
  lv_obj_center(s_modalIcon);

  // The name on one line, cut with dots.
  lv_obj_t* name = lv_label_create(c);
  lv_label_set_text(name, tileLabel(d).c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(name, cw - 88, lv_font_get_line_height(&deck_font_20));
  lv_obj_set_pos(name, 72, 26);

  s_modalState = lv_label_create(c);
  lv_obj_set_style_text_font(s_modalState, &deck_font_28, 0);
  lv_label_set_long_mode(s_modalState, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(s_modalState, cw - 32, lv_font_get_line_height(&deck_font_28));
  lv_obj_set_pos(s_modalState, 16, 74);

  const int bw = (cw - 16 * 2 - 10) / 2;
  const int by = ch - 16 - 44;
  lv_obj_t* primary = qpButton(c, 16, by, bw, "", false);
  lv_obj_set_height(primary, 44);
  lv_obj_set_style_bg_color(primary, lv_color_hex(accent), 0);
  lv_obj_set_style_text_color(primary, lv_color_hex(C_TEXT_ON), 0);
  lv_label_set_text(lv_obj_get_child(primary, 0), actionVerb(r, d.tap));
  lv_obj_add_event_cb(primary, onModalPrimary, LV_EVENT_CLICKED, nullptr);
  // Nothing to run when the tap does nothing or only opens this card.
  const bool noPrimary = d.tap.kind == ACT_NONE || d.tap.kind == ACT_DETAIL;
  if (noPrimary) lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* close = qpButton(c, noPrimary ? 16 : 16 + bw + 10, by, noPrimary ? cw - 32 : bw, "Close", false);
  lv_obj_set_height(close, 44);
  lv_obj_add_event_cb(close, onModalScrim, LV_EVENT_CLICKED, nullptr);

  setModalOpen(true);
  refreshModal();
  fxCardOpen(c, tileIdx);
  coverFill();
}

void onConfirmOk(lv_event_t*) {
  if (s_modalTile < 0 || s_confirmHold < 0 || millis() - s_confirmMs < CONFIRM_ARM_MS) return;
  TileRt& r = s_tiles[s_modalTile];
  const TileAction& a = s_confirmHold ? r.def->hold : r.def->tap;
  closeModal();
  runAction(r, a);
}

void onConfirmTimeout(lv_timer_t*) {
  s_confirmTimer = nullptr;  // a one-shot timer deletes itself
  closeModal();
}

// "Turn off Printer?" card for tiles with "confirm". Cancel, the scrim, the
// timeout or a state change close it without running anything.
void openConfirm(int tileIdx, bool hold) {
  closeModal();
  cleanPage();
  const TileRt& r = s_tiles[tileIdx];
  const TileDef& d = *r.def;
  const TileAction& a = hold ? d.hold : d.tap;
  const int w = logicalW(), h = logicalH();
  const uint32_t accent = d.hasColor ? d.color : C_ACCENT;
  constexpr uint32_t C_DANGER = 0xE5534B;
  s_modalTile = tileIdx;
  s_confirmHold = hold ? 1 : 0;
  s_confirmOn = r.on;
  s_confirmMs = millis();

  s_modalScrim = makeScrim();

  const int cw = w - 40 < 360 ? w - 40 : 360;
  const int ch = 188;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS &&
      (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* chip = lv_obj_create(c);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, 44, 44);
  lv_obj_set_pos(chip, 16, 16);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  styleIconChip(chip, accent);
  lv_obj_set_style_text_font(chip, &deck_font_20, 0);
  lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  s_modalIcon = lv_label_create(chip);
  lv_label_set_text(s_modalIcon, "");
  lv_obj_center(s_modalIcon);
  s_modalIconCp = showIcon(s_modalIcon, d, r.on, 28, 0);

  lv_obj_t* name = lv_label_create(c);
  lv_label_set_text(name, tileLabel(d).c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(name, cw - 88, lv_font_get_line_height(&deck_font_20));
  const String state = tileSub(r);
  lv_obj_set_pos(name, 72, state.length() ? 15 : 26);
  if (state.length()) {
    lv_obj_t* st = lv_label_create(c);
    lv_label_set_text(st, state.c_str());
    lv_obj_set_style_text_color(st, lv_color_hex(C_DIM), 0);
    lv_label_set_long_mode(st, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(st, cw - 88, lv_font_get_line_height(&deck_font_14));
    lv_obj_set_pos(st, 72, 41);
  }

  const char* verb = actionVerb(r, a);
  lv_obj_t* q = lv_label_create(c);
  lv_label_set_text_fmt(q, "%s?", verb);
  lv_obj_set_style_text_font(q, &deck_font_28, 0);
  lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(q, cw - 32, lv_font_get_line_height(&deck_font_28));
  lv_obj_set_pos(q, 16, 74);

  const int bw = (cw - 16 * 2 - 10) / 2;
  const int by = ch - 16 - 44;
  lv_obj_t* cancel = qpButton(c, 16, by, bw, "Cancel", false);
  lv_obj_set_height(cancel, 44);
  lv_obj_add_event_cb(cancel, onModalScrim, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* ok = qpButton(c, 16 + bw + 10, by, bw, verb, false);
  lv_obj_set_height(ok, 44);
  lv_obj_set_style_bg_color(ok, lv_color_hex(C_DANGER), 0);
  lv_obj_set_style_text_color(ok, lv_color_white(), 0);
  lv_obj_add_event_cb(ok, onConfirmOk, LV_EVENT_CLICKED, nullptr);

  s_confirmTimer = lv_timer_create(onConfirmTimeout, CONFIRM_TIMEOUT_MS, nullptr);
  lv_timer_set_repeat_count(s_confirmTimer, 1);
  setModalOpen(true);
}

// Printer errors: print_error first, then the HMS codes worst first. A tap
// anywhere closes it.
void onErrorsClose(lv_event_t*) { closeModal(); }

void openPrinterErrors(const String& title, const PrinterState& s) {
  closeModal();
  cleanPage();
  const int w = logicalW(), h = logicalH();
  s_modalScrim = makeScrim();
  const int cw = w - 32 < 440 ? w - 32 : 440;
  const int ch = h - 40 < 340 ? h - 40 : 340;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_modalCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, ch);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  if (s_theme == THEME_GLASS &&
      (s_modalGlass = frostedCopy((w - cw) / 2, (h - ch) / 2, cw, ch, 22)))
    frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_pad_all(c, 16, 0);
  lv_obj_set_style_pad_row(c, 10, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c, onErrorsClose, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* name = lv_label_create(c);
  lv_label_set_text(name, title.c_str());
  lv_obj_set_style_text_font(name, &deck_font_20, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(name, lv_pct(100));

  auto entry = [&](uint8_t sev, const String& head, const char* code, const String& text) {
    const uint32_t color = hmsSeverityColor(sev);
    lv_obj_t* box = lv_obj_create(c);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(box, 10, 0);
    lv_obj_set_style_pad_row(box, 2, 0);
    lv_obj_set_style_border_side(box, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(color), 0);
    lv_obj_t* hl = lv_label_create(box);
    lv_label_set_text(hl, head.c_str());
    lv_obj_set_style_text_font(hl, &deck_font_14, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(color), 0);
    lv_obj_t* tl = lv_label_create(box);
    lv_label_set_text(tl, text.c_str());
    lv_obj_set_style_text_font(tl, &deck_font_14, 0);
    lv_obj_set_width(tl, lv_pct(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t* cl = lv_label_create(box);
    lv_label_set_text(cl, code);
    lv_obj_set_style_text_font(cl, &deck_font_12, 0);
    lv_obj_set_style_text_opa(cl, LV_OPA_60, 0);
  };
  char code[32];
  if (s.printError && !printErrorIsCancel(s.printError)) {
    printErrorFormatCode(s.printError, code, sizeof(code));
    const char* t = printErrorText(s.printError);
    entry(1, "Print error", code, t ? t : "No description published for this code");
  }
  for (uint8_t i = 0; i < s.hmsCount; i++) {
    const PrinterHms& e = s.hms[i];
    const uint8_t sev = hmsSeverity(e.code);
    hmsFormatCode(e.attr, e.code, code, sizeof(code));
    const char* t = hmsText(e.attr, e.code);
    String head = String(hmsSeverityLabel(sev)) + " - " + hmsModuleLabel(e.attr);
    if (e.quiet) head += " (handled)";
    entry(sev >= 1 && sev <= 3 ? sev : 0, head, code, t ? t : "No description published for this code");
  }
  if (s.hmsTotal > s.hmsCount) {
    lv_obj_t* more = lv_label_create(c);
    lv_label_set_text_fmt(more, "+%u more on the printer", (unsigned)(s.hmsTotal - s.hmsCount));
    lv_obj_set_style_text_font(more, &deck_font_12, 0);
    lv_obj_set_style_text_opa(more, LV_OPA_60, 0);
  }
  setModalOpen(true);
}

// The pairing card: Allow / Deny for a companion's request, or the code an
// older companion asked for, until answered, cancelled or timed out
// (pc_metrics owns the state).
lv_obj_t* s_pairCard = nullptr;
lv_obj_t* s_pairScrim = nullptr;
uint32_t s_pairChecked = 0;
String s_pairName, s_pairCode;
lv_obj_t* s_askCard = nullptr;  // Claude Code asking, below

void pairClose() {
  if (s_pairCard) lv_obj_delete(s_pairCard);
  if (s_pairScrim) lv_obj_delete(s_pairScrim);
  s_pairCard = s_pairScrim = nullptr;
  s_pairName = s_pairCode = "";
  setModalOpen(s_modalCard != nullptr || s_askCard != nullptr);
}

void onPairCancel(lv_event_t*) {
  pcPairCancel();
  lv_async_call([](void*) { pairClose(); }, nullptr);  // runs in the button's own event
}

void onPairAllow(lv_event_t*) {
  pcPairAllow();
  lv_async_call([](void*) { pairClose(); }, nullptr);
}

void pairSync() {
  const uint32_t now = millis();
  if (now - s_pairChecked < 250) return;
  s_pairChecked = now;
  String name, code;
  const bool pending = pcPairPending(name, code);
  if (!pending) {
    if (s_pairCard) pairClose();
    return;
  }
  if (s_pairCard && name == s_pairName && code == s_pairCode) return;
  if (s_pairCard) pairClose();
  s_pairName = name;
  s_pairCode = code;
  const int w = logicalW(), h = logicalH();
  s_pairScrim = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_pairScrim);
  lv_obj_set_size(s_pairScrim, w, h);
  lv_obj_set_style_bg_color(s_pairScrim, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_pairScrim, LV_OPA_50, 0);
  lv_obj_add_flag(s_pairScrim, LV_OBJ_FLAG_CLICKABLE);
  const int cw = w - 40 < 340 ? w - 40 : 340;
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_pairCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, cw, 196);
  lv_obj_center(c);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 22, 0);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t = lv_label_create(c);
  lv_label_set_text_fmt(t, "Pair %s", name.c_str());
  lv_obj_set_style_text_font(t, &deck_font_20, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(t, cw - 32);
  lv_obj_set_pos(t, 16, 14);
  if (!code.length()) {
    // Tap mode: the companion waits for this answer.
    lv_obj_t* q = lv_label_create(c);
    lv_label_set_text_fmt(q, "Let this deck control %s: media, volume, lock, power and hotkeys?", name.c_str());
    lv_label_set_long_mode(q, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(q, cw - 32);
    lv_obj_set_style_text_color(q, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(q, 16, 52);
    const int bw = (cw - 40) / 2;
    lv_obj_t* deny = qpButton(c, 16, 196 - 16 - 44, bw, "Deny", false);
    lv_obj_set_height(deny, 44);
    lv_obj_add_event_cb(deny, onPairCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* allow = qpButton(c, 24 + bw, 196 - 16 - 44, bw, "Allow", false);
    lv_obj_set_height(allow, 44);
    lv_obj_add_state(allow, LV_STATE_CHECKED);  // the accent look
    lv_obj_add_event_cb(allow, onPairAllow, LV_EVENT_CLICKED, nullptr);
    setModalOpen(true);
    return;
  }
  lv_obj_t* k = lv_label_create(c);
  String spaced = code.substring(0, 3) + " " + code.substring(3);
  lv_label_set_text(k, spaced.c_str());
  lv_obj_set_style_text_font(k, &deck_font_28, 0);
  lv_obj_set_style_text_letter_space(k, 4, 0);
  lv_obj_align(k, LV_ALIGN_TOP_MID, 0, 50);
  lv_obj_t* hint = lv_label_create(c);
  lv_label_set_text(hint, "Enter this code in TouchDeck Companion");
  lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), 0);
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 94);
  lv_obj_t* cancel = qpButton(c, 16, 196 - 16 - 44, cw - 32, "Cancel", false);
  lv_obj_set_height(cancel, 44);
  lv_obj_add_event_cb(cancel, onPairCancel, LV_EVENT_CLICKED, nullptr);
  setModalOpen(true);  // no swipes under it
}

// --- Claude Code asking -----------------------------------------------------------------
// A permission prompt or a question a Claude Code session waits on (pcClaudeAsk),
// with its options as buttons. The terminal shows the same dialog meanwhile:
// whichever answers first wins, and the card goes once the PC stops sending it.
// A tap closes it at once; it comes back if the PC still asks a while later
// (the answer got lost). The x hides it until another prompt comes.
lv_obj_t* s_askScrim = nullptr;
lv_obj_t* s_askBtn[CLAUDE_ASK_OPTS] = {};
lv_obj_t* s_askBadge[CLAUDE_ASK_OPTS] = {};  // a question's option numbers (a check once picked)
String s_askPc;              // whose prompt (pc_id)
char s_askId[12] = "";
uint8_t s_askQi = 0, s_askQn = 0;
char s_askKind = 0;
bool s_askMulti = false;
bool s_askLocked = false;    // answered or hidden: no more taps until it goes or is asked again
uint32_t s_askSerial = 0;    // bumps per card shown, for the delayed close
lv_obj_t* s_askDot = nullptr;  // the session's dot: where the card's effect starts
int s_askW = 0, s_askH = 0;  // the screen it was laid out for
uint32_t s_askShownMs = 0;
uint32_t s_askChecked = 0;
uint32_t s_askWoke = 0;      // seq of the last prompt that woke the screen
// The last one answered or hidden, and when.
char s_askDoneId[12] = "";
uint8_t s_askDoneQi = 0;
uint32_t s_askDoneMs = 0;
bool s_askHidden = false;
constexpr uint32_t ASK_ARM_MS = 700;     // a tap under way when it opened must not answer it
constexpr uint32_t ASK_RETRY_MS = 6000;  // still asked this long after a tap: shown again
constexpr uint32_t ASK_CLOSE_MS = 220;   // after a tap, so its touch effect shows before the card goes
constexpr uint32_t C_CLAUDE = 0xD97757;  // Claude's orange
// The accent, the PC agent portal's green (styles.css --accent, --on-accent), and
// the text on it; its light variant on Daylight.
uint32_t askAccent() { return s_theme == THEME_DAYLIGHT ? 0x1F8A5B : 0x36B478; }
uint32_t askOnAccent() { return s_theme == THEME_DAYLIGHT ? 0xFFFFFF : 0x0C1A12; }
FrostBands s_askGlass;  // Glass theme: the card's frosted background

// keepGlass: another prompt replaces this one at once, over the same screen;
// a fresh copy now would frost this card instead of the page under it.
void askClose(bool keepGlass = false) {
  if (s_askCard) lv_obj_delete(s_askCard);
  if (s_askScrim) lv_obj_delete(s_askScrim);
  s_askCard = s_askScrim = nullptr;
  if (!keepGlass) frostBandsFree(s_askGlass);
  memset(s_askBtn, 0, sizeof(s_askBtn));
  memset(s_askBadge, 0, sizeof(s_askBadge));
  s_askDot = nullptr;
  s_askId[0] = 0;
  setModalOpen(s_modalCard != nullptr || s_pairCard != nullptr);
}

// The card goes with the display's card effect backwards.
void askCloseFx() {
  if (s_askCard) fxPromptClose(s_askCard, [] { askClose(); });
}

void askDone(bool hidden) {
  strlcpy(s_askDoneId, s_askId, sizeof(s_askDoneId));
  s_askDoneQi = s_askQi;
  s_askDoneMs = millis();
  s_askHidden = hidden;
  s_askLocked = true;
  // Another question of the same prompt comes in its place: the card stays.
  if (!hidden && s_askKind == 'q' && s_askQi + 1 < s_askQn) return;
  if (hidden) {
    lv_async_call([](void*) { askCloseFx(); }, nullptr);  // runs in the button's own event
    return;
  }
  lv_timer_t* t = lv_timer_create(
      [](lv_timer_t* t) {
        if ((uint32_t)(uintptr_t)lv_timer_get_user_data(t) == s_askSerial) askCloseFx();
      },
      ASK_CLOSE_MS, (void*)(uintptr_t)s_askSerial);
  lv_timer_set_repeat_count(t, 1);
}

bool askArmed() { return !s_askLocked && millis() - s_askShownMs >= ASK_ARM_MS; }

void askSend(const char* choice) {
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "claude:%s:%u:%s", s_askId, (unsigned)s_askQi, choice);
  String err;
  hapticTap();
  if (!pcSendCommand(s_askPc, cmd, err)) {
    deckToastShow(("Can't answer: " + err).c_str(), 2500);
    return;  // the card stays for another try
  }
  askDone(false);
}

void onAskOption(lv_event_t* e) {
  if (!askArmed()) return;
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (s_askMulti) {  // toggled here, not by LVGL: an early tap must not flip it
    lv_obj_t* b = s_askBtn[i];
    const bool checked = !lv_obj_has_state(b, LV_STATE_CHECKED);
    if (checked) lv_obj_add_state(b, LV_STATE_CHECKED);
    else lv_obj_remove_state(b, LV_STATE_CHECKED);
    if (s_askBadge[i]) {
      if (checked) lv_label_set_text(s_askBadge[i], LV_SYMBOL_OK);
      else lv_label_set_text_fmt(s_askBadge[i], "%d", i + 1);
    }
    hapticTap();
    return;
  }
  char choice[4];
  snprintf(choice, sizeof(choice), "%d", i);
  if (s_askBtn[i]) lv_obj_add_state(s_askBtn[i], LV_STATE_CHECKED);  // the pick, in the accent
  askSend(choice);
  if (!s_askLocked && s_askBtn[i]) lv_obj_remove_state(s_askBtn[i], LV_STATE_CHECKED);  // not sent
}

void onAskSend(lv_event_t*) {
  if (!askArmed()) return;
  char choice[16] = "";
  for (int i = 0; i < CLAUDE_ASK_OPTS; i++) {
    if (!s_askBtn[i] || !lv_obj_has_state(s_askBtn[i], LV_STATE_CHECKED)) continue;
    const size_t n = strlen(choice);
    snprintf(choice + n, sizeof(choice) - n, n ? ",%d" : "%d", i);
  }
  if (!choice[0]) {
    deckToastShow("Pick at least one", 1500);
    return;
  }
  askSend(choice);
}

void onAskHide(lv_event_t*) {
  if (askArmed()) askDone(true);
}

lv_obj_t* askLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

// Glass: the cards' light edge (portal Display, Claude cards), none at 0 %.
void askFrameStyle(lv_obj_t* o) {
  lv_obj_set_style_border_width(o, dispSettings.askFrame ? GLASS_BORDER_WIDTH : 0, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(dispSettings.askFrameColor), 0);
  lv_obj_set_style_border_opa(o, (lv_opa_t)(dispSettings.askFrame * 255 / 100), 0);
}

// Glass: a button's glass, as dark as the tiles' but never under 40 %, so
// light text reads on any picture.
lv_opa_t askButtonOpa() { return (lv_opa_t)(std::max<int>(dispSettings.glassTint, 40) * 255 / 100); }

// A button's colours. Glass: as the theme's tiles, tinted glass with a light
// edge, and the accent (picked, or the main one) at the "on" opacity.
void askFace(lv_obj_t* b, bool main) {
  const bool glass = (bool)s_askGlass;
  lv_obj_set_style_bg_color(b, lv_color_hex(main ? askAccent() : glass ? C_TILE : C_BUTTON), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(askAccent()), LV_STATE_CHECKED);
  lv_obj_set_style_text_color(b, lv_color_hex(main ? askOnAccent() : C_TEXT), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(askOnAccent()), LV_STATE_CHECKED);
  if (!glass) return;
  const lv_opa_t on = (lv_opa_t)(dispSettings.glassOn * 255 / 100);
  lv_obj_set_style_bg_opa(b, main ? on : askButtonOpa(), 0);
  lv_obj_set_style_bg_opa(b, on, LV_STATE_CHECKED);
  askFrameStyle(b);
}

// A button as tall as its text: the label, and a dimmer line under it.
lv_obj_t* askButton(lv_obj_t* parent, const char* text, const char* sub, bool main) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_hor(b, 10, 0);
  lv_obj_set_style_pad_ver(b, 10, 0);
  lv_obj_set_style_pad_row(b, 2, 0);
  lv_obj_set_style_min_height(b, 44, 0);
  lv_obj_set_height(b, LV_SIZE_CONTENT);
  askFace(b, main);
  lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  if (sub && sub[0]) {
    lv_obj_t* s = lv_label_create(b);
    lv_label_set_text(s, sub);
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s, &deck_font_12, 0);
    lv_obj_set_style_text_opa(s, LV_OPA_70, 0);
  }
  return b;
}

// A header in capitals: ASCII, and the Latin-1 letters the deck's fonts have
// ("Elección" -> "ELECCIÓN").
String askUpper(const char* s) {
  String out;
  for (const uint8_t* p = (const uint8_t*)s; *p; p++) {
    out += (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    if (*p == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) {  // U+00E0..00FE but the ÷
      out += (char)(p[1] - 0x20);
      p++;
    }
  }
  return out;
}

// A big choice: an optional chip (a question's number), the label and a
// dimmer line under it. Side by side in a row; stacked in a grid cell.
lv_obj_t* askChoice(lv_obj_t* parent, const char* text, const char* sub, const char* chip, bool cell, int width,
                    int minH, const lv_font_t* font, bool main, lv_obj_t** chipLabel = nullptr) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_radius(b, 14, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_hor(b, 12, 0);
  lv_obj_set_style_pad_ver(b, 10, 0);
  lv_obj_set_style_pad_column(b, 12, 0);
  lv_obj_set_style_pad_row(b, 6, 0);
  lv_obj_set_size(b, width, LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(b, minH, 0);
  askFace(b, main);
  fxTouchBind(b);  // the display's touch effect, as on the tiles
  lv_obj_set_flex_flow(b, cell ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  if (cell) lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  else lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);

  if (chip) {
    lv_obj_t* c = lv_obj_create(b);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, 26, 26);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);  // the tap is the button's
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* n = lv_label_create(c);
    lv_label_set_text(n, chip);
    lv_obj_set_style_text_font(n, &deck_font_14, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(askAccent()), 0);
    lv_obj_center(n);
    if (chipLabel) *chipLabel = n;
  }

  lv_obj_t* col = lv_obj_create(b);
  lv_obj_remove_style_all(col);
  lv_obj_set_height(col, LV_SIZE_CONTENT);
  if (cell) lv_obj_set_width(col, lv_pct(100));
  else lv_obj_set_flex_grow(col, 1);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 2, 0);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* l = lv_label_create(col);
  lv_label_set_text(l, text);
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_font(l, font, 0);
  if (sub && sub[0]) {
    lv_obj_t* d = lv_label_create(col);
    lv_label_set_text(d, sub);
    lv_obj_set_width(d, lv_pct(100));
    lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(d, &deck_font_12, 0);
    lv_obj_set_style_text_opa(d, LV_OPA_70, 0);
  }
  return b;
}

// Both cards take the whole screen: on the side (on top when the screen
// stands upright) whose session and what it asks, then Hide; beside it the
// choices as big buttons. Glass: the screen under it, frosted, as an app's.
lv_obj_t* askRoot(int w, int h) {
  lv_obj_t* c = lv_obj_create(lv_layer_top());
  s_askCard = c;
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  const int bg = dispSettings.askBg == ASK_BG_TILES ? dispSettings.glassTint : dispSettings.askBg;
  if (s_theme == THEME_GLASS && (s_askGlass || frostedBands(s_askGlass, 0, 0, w, h, bg))) {
    frostBandsStyle(c, &s_askGlass);
    askFrameStyle(c);
  }
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_set_flex_flow(c, w >= h ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

// The side: the session's dot and name, then "SUB" in small capitals. On
// Glass a darker pane of the same glass.
lv_obj_t* askSide(lv_obj_t* c, const String& who, const String& sub, int sw, int sh) {
  lv_obj_t* side = lv_obj_create(c);
  lv_obj_remove_style_all(side);
  lv_obj_set_size(side, sw, sh);
  lv_obj_set_style_bg_color(side, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(side, s_askGlass ? LV_OPA_40 : LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(side, 14, 0);
  lv_obj_set_style_pad_bottom(side, 4, 0);
  lv_obj_set_style_pad_row(side, 6, 0);
  lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* top = lv_obj_create(side);
  lv_obj_remove_style_all(top);
  lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(top, 6, 0);
  lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* dot = lv_obj_create(top);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, lv_color_hex(C_CLAUDE), 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  s_askDot = dot;
  lv_obj_t* name = lv_label_create(top);
  lv_label_set_text(name, who.c_str());
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_flex_grow(name, 1);
  lv_obj_set_style_text_font(name, &deck_font_14, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(C_CLAUDE), 0);

  if (sub.length()) {
    lv_obj_t* s = askLabel(side, sub.c_str(), &deck_font_12, C_TEXT);
    lv_obj_set_style_text_opa(s, LV_OPA_60, 0);
    lv_obj_set_style_text_letter_space(s, 1, 0);
  }
  return side;
}

// What the side shows between its header and Hide; a long one scrolls.
lv_obj_t* askBody(lv_obj_t* side) {
  lv_obj_t* body = lv_obj_create(side);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_style_pad_top(body, 4, 0);
  lv_obj_set_style_pad_row(body, 6, 0);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  return body;
}

// Hide, at the bottom of the side: the terminal still has the prompt.
void askHideRow(lv_obj_t* side) {
  lv_obj_t* x = lv_button_create(side);
  lv_obj_remove_style_all(x);
  lv_obj_set_size(x, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_min_height(x, 36, 0);
  lv_obj_t* xl = askLabel(x, LV_SYMBOL_CLOSE "  Hide \xC2\xB7 answer in terminal", &deck_font_12, C_TEXT);
  lv_obj_set_style_text_opa(xl, LV_OPA_50, 0);
  lv_obj_align(xl, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_add_event_cb(x, onAskHide, LV_EVENT_CLICKED, nullptr);
}

// The choices' column, beside or under the side.
lv_obj_t* askChoices(lv_obj_t* c, int ow, int oh, bool wrap) {
  lv_obj_t* o = lv_obj_create(c);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, ow, oh);
  lv_obj_set_style_pad_all(o, 12, 0);
  lv_obj_set_style_pad_row(o, 8, 0);
  lv_obj_set_style_pad_column(o, 8, 0);
  lv_obj_set_flex_flow(o, wrap ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(o, LV_DIR_VER);  // big text past the cells scrolls
  return o;
}

// A question: which of how many (a bar each), the question, and its options
// numbered, four in a 2x2 grid. A tap answers; one that takes several has Send.
void askQuestionCard(const ClaudeAsk& a, const String& who, int w, int h) {
  const bool wide = w >= h;
  const int sw = wide ? w * 7 / 20 : w, sh = wide ? h : h * 9 / 20;
  lv_obj_t* c = askRoot(w, h);

  // "COLOR · 1 OF 2 · PICK ANY"
  String sub = a.title[0] ? askUpper(a.title) : String();
  if (a.qn > 1) sub += String(sub.length() ? " \xC2\xB7 " : "") + String(a.qi + 1) + " OF " + String(a.qn);
  if (a.multi) sub += String(sub.length() ? " \xC2\xB7 " : "") + "PICK ANY";
  lv_obj_t* side = askSide(c, who, sub, sw, sh);
  if (a.qn > 1) {
    lv_obj_t* bars = lv_obj_create(side);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bars, 4, 0);
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < a.qn && i < 8; i++) {
      lv_obj_t* bar = lv_obj_create(bars);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 18, 4);
      lv_obj_set_style_radius(bar, 2, 0);
      lv_obj_set_style_bg_color(bar, lv_color_hex(i <= a.qi ? askAccent() : C_BUTTON), 0);
      lv_obj_set_style_bg_opa(bar, i < a.qi ? LV_OPA_50 : LV_OPA_COVER, 0);  // done ones fainter
    }
  }
  const size_t len = strlen(a.text);
  askLabel(askBody(side), a.text, len <= (wide ? 70u : 120u) ? &deck_font_20 : &deck_font_14, C_TEXT);
  askHideRow(side);

  const int ow = wide ? w - sw : w, oh = wide ? h : h - sh;
  const bool grid = a.optCount == 4;
  lv_obj_t* opts = askChoices(c, ow, oh, grid);
  const int rows = grid ? 2 : std::max<int>(1, a.optCount);
  const int iw = ow - 24, ih = oh - 24 - (a.multi ? 44 + 8 : 0);
  const int cellW = grid ? (iw - 8) / 2 : iw;
  const int cellH = std::max(44, (ih - 8 * (rows - 1)) / rows);
  size_t longest = 0;
  for (int i = 0; i < a.optCount; i++) longest = std::max(longest, strlen(a.opts[i]));
  const lv_font_t* font = longest <= (grid ? 20u : 32u) ? &deck_font_20 : &deck_font_14;
  for (int i = 0; i < a.optCount; i++) {
    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    lv_obj_t* b = askChoice(opts, a.opts[i], a.optDesc[i], num, grid, cellW, cellH, font, false, &s_askBadge[i]);
    s_askBtn[i] = b;
    lv_obj_add_event_cb(b, onAskOption, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  if (a.multi) {
    lv_obj_t* send = askButton(opts, "Send", nullptr, true);
    lv_obj_set_width(send, iw);
    fxTouchBind(send);
    lv_obj_add_event_cb(send, onAskSend, LV_EVENT_CLICKED, nullptr);
  }
}

// A permission prompt: the tool, Claude's words for it and what it runs or
// touches, on a wider side; the choices stacked, the main one (the PC lists
// it last) on top, each with what it does.
void askPermissionCard(const ClaudeAsk& a, const String& who, int w, int h) {
  const bool wide = w >= h;
  const int sw = wide ? w * 7 / 12 : w, sh = wide ? h : h * 11 / 20;
  lv_obj_t* c = askRoot(w, h);

  lv_obj_t* side = askSide(c, who, askUpper(a.title) + " \xC2\xB7 PERMISSION", sw, sh);
  lv_obj_t* body = askBody(side);
  char t[48];
  snprintf(t, sizeof(t), "Allow %s?", a.title);
  askLabel(body, t, &deck_font_20, C_TEXT);
  if (a.desc[0]) lv_obj_set_style_text_opa(askLabel(body, a.desc, &deck_font_14, C_TEXT), LV_OPA_60, 0);
  if (a.text[0]) {
    // The command or file as it will run.
    lv_obj_t* box = lv_obj_create(body);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_bg_opa(box, s_askGlass ? askButtonOpa() : LV_OPA_COVER, 0);
    if (s_askGlass) askFrameStyle(box);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);  // the body scrolls
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    askLabel(box, a.text, &deck_font_14, C_TEXT);
  }
  askHideRow(side);

  const int ow = wide ? w - sw : w, oh = wide ? h : h - sh;
  lv_obj_t* opts = askChoices(c, ow, oh, false);
  const int n = std::max<int>(1, a.optCount);
  const int iw = ow - 24, cellH = std::max(44, (oh - 24 - 8 * (n - 1)) / n);
  String hint = a.hint;
  if (hint.startsWith("Always: ")) hint = hint.substring(8);  // the button says it
  for (int k = 0; k < a.optCount; k++) {
    const int i = a.optCount - 1 - k;
    const char* label = a.opts[i];
    String text = label;
    const char* sub = nullptr;
    if (!strcmp(label, "Allow")) {
      text = LV_SYMBOL_OK "  Allow";
      sub = "Just this once";
    } else if (!strcmp(label, "Deny")) {
      text = LV_SYMBOL_CLOSE "  Deny";
      sub = "Claude stops and waits";
    } else if (!strcmp(label, "Always")) {
      sub = hint.c_str();
    }
    lv_obj_t* b = askChoice(opts, text.c_str(), sub, nullptr, false, iw, cellH, &deck_font_20, i == a.optCount - 1);
    s_askBtn[i] = b;
    lv_obj_add_event_cb(b, onAskOption, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
}

// animate: the card comes over the screen with the display's card effect;
// not when it takes the place of one already shown.
void askOpen(const ClaudeAsk& a, const PcSource& pc, bool animate) {
  s_askPc = pc.id;
  strlcpy(s_askId, a.id, sizeof(s_askId));
  s_askQi = a.qi;
  s_askQn = a.qn;
  s_askKind = a.kind;
  s_askMulti = a.multi;
  s_askLocked = false;
  s_askSerial++;
  const int w = logicalW(), h = logicalH();
  s_askW = w;
  s_askH = h;
  s_askScrim = lv_obj_create(lv_layer_top());  // under the card, for askSync's restacking
  lv_obj_remove_style_all(s_askScrim);
  lv_obj_set_size(s_askScrim, w, h);
  lv_obj_add_flag(s_askScrim, LV_OBJ_FLAG_CLICKABLE);

  // Whose session, and PC with several.
  String head = a.session[0] ? a.session : "Claude Code";
  if (pcOnlineCount() > 1) head += String(" on ") + pc.name;
  if (a.kind == 'q') askQuestionCard(a, head, w, h);
  else askPermissionCard(a, head, w, h);
  s_askShownMs = millis();
  setModalOpen(true);  // no swipes under it
  if (!animate) return;
  lv_obj_update_layout(s_askCard);
  lv_point_t at = {w / 2, h / 2};
  if (s_askDot) {
    lv_area_t d;
    lv_obj_get_coords(s_askDot, &d);
    at = {(d.x1 + d.x2) / 2, (d.y1 + d.y2) / 2};
  }
  fxPromptOpen(s_askCard, at);
}

void askWake() {
  const ClaudeAsk* a = pcClaudeAsk(nullptr);
  if (!a || a->seq == s_askWoke) return;
  s_askWoke = a->seq;
  if (s_askHidden && !strcmp(a->id, s_askDoneId) && a->qi == s_askDoneQi) return;
  presenceWake();  // out of idle sleep and the picture frame, and the timer restarts
  if (nightScreenOff()) wakeDisplayFromTouch();  // a 0% night: a dim minute to answer in
}

void askSync() {
  const uint32_t now = millis();
  if (now - s_askChecked < 200) return;
  s_askChecked = now;
  const PcSource* pc = nullptr;
  const ClaudeAsk* a = getScreenState() == SCREEN_DECK && !s_pairCard ? pcClaudeAsk(&pc) : nullptr;
  if (!a) {
    // Answered in the terminal, or the session moved on: the card goes as it came.
    if (s_askCard) getScreenState() == SCREEN_DECK ? askCloseFx() : askClose();
    return;
  }
  if (s_askCard && !strcmp(a->id, s_askId) && a->qi == s_askQi && s_askW == logicalW() && s_askH == logicalH()) {
    if (s_askLocked && now - s_askDoneMs >= ASK_RETRY_MS) {  // the answer got lost: ask again
      askClose(true);
      askOpen(*a, *pc, false);
      return;
    }
    // Stays over a card or the quick panel opened since, with no swipes under it.
    const int32_t last = (int32_t)lv_obj_get_child_count(lv_layer_top()) - 1;
    if (lv_obj_get_index(s_askCard) != last) {
      lv_obj_move_foreground(s_askScrim);
      lv_obj_move_foreground(s_askCard);
    }
    setModalOpen(true);
    return;
  }
  if (!strcmp(a->id, s_askDoneId) && a->qi == s_askDoneQi && (s_askHidden || now - s_askDoneMs < ASK_RETRY_MS)) {
    if (s_askCard) askClose();
    return;
  }
  // Another prompt or the next question over this one: it takes its place at
  // once, on the same frosted page (a copy now would be of this card).
  const bool shown = s_askCard != nullptr;
  if (shown) askClose(s_askW == logicalW() && s_askH == logicalH());
  askOpen(*a, *pc, !shown);
}

}  // namespace deckui
