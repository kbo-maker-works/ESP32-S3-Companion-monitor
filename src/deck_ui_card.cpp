// Deck UI: toast, detail card, confirm card and the PC pairing card.
#include "deck_ui_int.h"
#include "deck_daily.h"
#include "app.h"
#include <src/misc/cache/instance/lv_image_cache.h>

namespace deckui {

// --- Toast and detail card ------------------------------------------------------------
lv_obj_t* s_toast = nullptr;
lv_obj_t* s_modalScrim = nullptr;
lv_obj_t* s_modalCard = nullptr;
lv_draw_buf_t* s_modalGlass = nullptr;  // Glass theme: the card's frosted background
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
// A detail card the size of the screen: header with "‹ Inicio" and the app's
// name, the app's own view below. Same opening and closing animation as a card.
constexpr int APP_HEADER_H = 48;
void cleanPage();

void onAppBack(lv_event_t*) { fxCardClose(s_modalCard, s_modalTile); }

void openApp(int tileIdx, const String& id) {
  const app::Def* a = app::find(id.c_str());
  if (!a) {
    deckToastShow("App desconocida", 1500);
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
  if (s_theme == THEME_GLASS && (s_modalGlass = frostedCopy(0, 0, w, h, 0))) frostStyle(c, s_modalGlass);
  lv_obj_set_style_text_color(c, lv_color_hex(C_TEXT), 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back = qpButton(c, 8, 6, 118, LV_SYMBOL_LEFT "  Inicio", false);
  lv_obj_set_height(back, APP_HEADER_H - 12);
  lv_obj_add_event_cb(back, onAppBack, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* title = lv_label_create(c);
  lv_label_set_text(title, a->name);
  lv_obj_set_style_text_font(title, &deck_font_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, (APP_HEADER_H - lv_font_get_line_height(&deck_font_20)) / 2);

  lv_obj_t* body = lv_obj_create(c);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, 0, APP_HEADER_H);
  lv_obj_set_size(body, w, h - APP_HEADER_H);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  s_app = a;
  setModalOpen(true);
  a->open(body, w, h - APP_HEADER_H);
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
  if (s_modalScrim) lv_obj_delete(s_modalScrim);
  if (s_modalUnder) lv_obj_delete(s_modalUnder);
  s_modalCard = s_modalScrim = s_modalUnder = nullptr;
  if (s_modalGlass) lv_draw_buf_destroy(s_modalGlass);
  s_modalGlass = nullptr;
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

void pairClose() {
  if (s_pairCard) lv_obj_delete(s_pairCard);
  if (s_pairScrim) lv_obj_delete(s_pairScrim);
  s_pairCard = s_pairScrim = nullptr;
  s_pairName = s_pairCode = "";
  setModalOpen(s_modalCard != nullptr);
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

}  // namespace deckui
