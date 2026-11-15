// Widget registry and the widgets: weather, Claude mascot and usage, Work, PC stats. See widget.h.
#include "widgets/widget.h"
#include "ui/deck_ui_int.h"
#include "core/companion_settings.h"
#include "services/weather_service.h"
#include "services/pc_metrics.h"
#include "services/claude_fmt.h"
#include "services/work_data.h"
#include "apps/app_work.h"
#include <src/misc/lv_anim_private.h>  // lv_anim_t::var in the mascot's anim callback

namespace widget {
namespace {

using namespace deckui;

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_opa(l, opa, 0);
  lv_label_set_text(l, "");
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

void show(lv_obj_t* o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// --- Tiempo -----------------------------------------------------------------
// compacto: icon and temperature. detalle: plus condition, humidity and city,
// dropped one by one when the tile is too small for them.
struct Weather {
  bool detail;
  lv_obj_t *top, *icon, *temp, *cond, *extra, *city;
  uint32_t key = UINT32_MAX;
  int w = 0, h = 0;
};

void* weatherCreate(lv_obj_t* t, const char* variant, const char*) {
  Weather* s = new (std::nothrow) Weather();
  if (!s) return nullptr;
  s->detail = !strcmp(variant, "detalle");
  // Icon and temperature side by side, the rest below
  s->top = lv_obj_create(t);
  lv_obj_remove_style_all(s->top);
  lv_obj_set_size(s->top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s->top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s->top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s->top, 6, 0);
  lv_obj_remove_flag(s->top, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  s->icon = label(s->top, &deck_font_28);
  s->temp = label(s->top, &deck_font_28);
  s->cond = label(t, &deck_font_14);
  s->extra = label(t, &deck_font_12, LV_OPA_70);
  s->city = label(t, &deck_font_12, LV_OPA_60);
  for (lv_obj_t* l : {s->cond, s->extra, s->city}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
  }
  return s;
}

bool weatherUpdate(void* p) {
  Weather* s = (Weather*)p;
  if (!s || !s->w) return false;
  const weather::Data d = weather::get();
  const bool icons = iconsReady();
  const uint32_t key = d.seq * 4 + (icons ? 2 : 0) + (d.ok ? 1 : 0);
  if (key == s->key) return false;
  s->key = key;

  // Room decides what fits: the icon follows the tile's height
  const int big = s->w >= 120 && s->h >= 70;
  const lv_font_t* tempFont = big ? &deck_font_28 : &deck_font_20;
  lv_obj_set_style_text_font(s->temp, tempFont, 0);
  int iconPx = s->detail ? (s->h >= 110 ? 40 : 28) : std::min(s->h * 45 / 100, 56);
  if (iconPx < 20) iconPx = 20;
  const uint32_t cp = d.ok && icons ? iconCodepoint(weather::mdiIcon(d)) : 0;
  const lv_font_t* f = cp ? iconFont(iconPx, cp) : nullptr;
  show(s->icon, f != nullptr);
  if (f) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(s->icon, f, 0);
    lv_label_set_text(s->icon, u);
  }
  char buf[32];
  if (d.ok) snprintf(buf, sizeof(buf), "%d°", (int)lroundf(d.temp));
  else snprintf(buf, sizeof(buf), "--°");
  lv_label_set_text(s->temp, buf);

  const int lines = s->detail ? (s->h >= 120 ? 3 : s->h >= 90 ? 2 : s->h >= 60 ? 1 : 0) : 0;
  // Without the icon font the condition says what the icon would
  const bool condLine = lines >= 1 || (!f && s->h >= 60);
  show(s->cond, condLine && d.ok);
  lv_label_set_text(s->cond, d.ok ? weather::condition(d.code) : "");
  show(s->extra, lines >= 2 && d.ok);
  snprintf(buf, sizeof(buf), "Humidity %d%%", d.humidity);
  lv_label_set_text(s->extra, buf);
  show(s->city, lines >= 3);
  lv_label_set_text(s->city, companion::settings().city.c_str());
  return true;
}

void weatherResize(void* p, int w, int h) {
  Weather* s = (Weather*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  s->key = UINT32_MAX;  // relayout on the next update
}

void weatherDestroy(void* p) { delete (Weather*)p; }

const Variant kWeatherVariants[] = {{"compacto", "Compact"}, {"detalle", "Detail"}};

// --- Mascota ------------------------------------------------------------------
// The Claude Code retro robot, built from plain boxes so it scales to any tile.
// It follows "claude" from the PC companion (pc_metrics.h): its mood while you
// work, and the hook events (waiting for you, turn done, failure) for a while.
// compacto: the robot and one line. detalle: plus a second line under it.
namespace mascot {

// Design grid (the C3 mascot): 130 x 141 units, origin top-left.
constexpr int kW = 130, kH = 141, kHead = 60, kCx = 65;
constexpr uint32_t kFrameMs = 600;
constexpr uint32_t kEventMs = 20000;  // how long an event face stays
constexpr uint32_t kBodyHex = 0xE58C8A, kHatHex = 0xA4B2E6, kStemHex = 0xD0CCD8,
                   kBlushHex = 0xFF7A8A, kAngryHex = 0xEC5F5A;  // red with rage
constexpr uint32_t kRageShakeMs = 4000;  // a failure: it shakes again this often

enum Face : uint8_t {
  F_NORMAL, F_BLINK, F_SLEEP, F_HAPPY, F_FAIL, F_LOOK, F_WINK, F_WIDE, F_JOY, F_GRUMPY, F_DROWSY,
  F_ANGRY
};

// A tap makes it react for a moment (see tap()): its face and a pill caption
// replace the mood, then it goes back to what it was doing.
struct Reaction {
  Face face;
  const char* text;
  uint32_t bg, fg;
  uint32_t until = 0;
};

struct State {
  bool detail;
  lv_obj_t *robot, *eye[2], *cheek[2], *leg[4], *line[4];
  lv_point_precise_t pts[4][3];
  lv_obj_t *title, *sub;
  int w = 0, h = 0;
  float u = 1;  // pixels per design unit
  uint32_t lastFrame = 0, nextBlink = 0;
  uint8_t frame = 0;
  uint32_t key = UINT32_MAX;
  Reaction react = {};
  uint8_t taps = 0;       // quick taps in a row (5 make it dizzy)
  uint32_t firstTap = 0;
  uint32_t ackSeq = 0;    // event dismissed with a tap
  uint32_t shownSeq = 0;  // event on screen now, 0 = none
  bool asleep = false;    // the mood is sleeping (or no PC)
  bool working = false;   // coding or thinking
  uint32_t pokedAt = 0;   // asleep: first tap (grumpy); a second one wakes it
  lv_obj_t *bubble, *say;  // comic speech bubble over its right shoulder
  int sayMaxW = 0;         // widest its text may be, px
  int robotExt = 0;        // how far the bubble may stick out of the robot, px
  uint32_t failSeq = 0;    // the failure it last shook at
  uint32_t shookAt = 0;    // and when
  bool angry = false;      // red, fists up (F_ANGRY)
};

// What it says when a tool fails: comic swearing, a new one every 450 ms.
const char* const kCurses[] = {"#@$%&!", "%&#@!!", "@#$%!*", "&%$#@!", "*#@&%!", "$%@#!!"};
constexpr int kTail = 8;  // the bubble's pointer, px below it

lv_obj_t* box(lv_obj_t* parent, uint32_t hex) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(hex), 0);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

// Place a box in design units.
void put(State* s, lv_obj_t* o, int x, int y, int w, int h, int r = 0) {
  const float u = s->u;
  lv_obj_set_pos(o, (int)lroundf(x * u), (int)lroundf(y * u));
  lv_obj_set_size(o, std::max(1, (int)lroundf(w * u)), std::max(1, (int)lroundf(h * u)));
  lv_obj_set_style_radius(o, (int)lroundf(r * u), 0);
}

void setLine(State* s, int i, int x0, int y0, int x1, int y1, int x2, int y2) {
  const float u = s->u;
  lv_point_precise_t* p = s->pts[i];
  p[0] = {(lv_value_precise_t)(x0 * u), (lv_value_precise_t)(y0 * u)};
  p[1] = {(lv_value_precise_t)(x1 * u), (lv_value_precise_t)(y1 * u)};
  p[2] = {(lv_value_precise_t)(x2 * u), (lv_value_precise_t)(y2 * u)};
  lv_line_set_points(s->line[i], p, 3);
  lv_obj_set_style_line_width(s->line[i], std::max(2, (int)lroundf(3 * u)), 0);
  show(s->line[i], true);
}

// The bubble's pointer: a white wedge from its bottom edge down to the left,
// towards the robot's head, with the two black sides of the outline.
void bubbleTail(lv_event_t* e) {
  if (lv_event_get_code(e) == LV_EVENT_REFR_EXT_DRAW_SIZE) {
    lv_event_set_ext_draw_size(e, kTail + 2);
    return;
  }
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  lv_layer_t* layer = lv_event_get_layer(e);
  const int32_t y = a.y2 - 1, x0 = a.x1 + 9, x1 = a.x1 + 19;
  const lv_point_precise_t tip = {(lv_value_precise_t)(a.x1 + 2), (lv_value_precise_t)(a.y2 + kTail)};
  lv_draw_triangle_dsc_t t;
  lv_draw_triangle_dsc_init(&t);
  t.p[0] = {(lv_value_precise_t)x0, (lv_value_precise_t)(y - 1)};
  t.p[1] = {(lv_value_precise_t)x1, (lv_value_precise_t)(y - 1)};
  t.p[2] = tip;
  t.color = lv_color_white();
  t.opa = LV_OPA_COVER;
  lv_draw_triangle(layer, &t);
  lv_draw_line_dsc_t l;
  lv_draw_line_dsc_init(&l);
  l.color = lv_color_black();
  l.width = 2;
  l.round_start = l.round_end = 1;
  l.p1 = {(lv_value_precise_t)x0, (lv_value_precise_t)y};
  l.p2 = tip;
  lv_draw_line(layer, &l);
  l.p1 = {(lv_value_precise_t)x1, (lv_value_precise_t)y};
  lv_draw_line(layer, &l);
}

// Puts `text` in the bubble (nullptr hides it), one line or two, as wide as
// it needs up to sayMaxW. Curses go big and red. True when that changed anything.
bool say(State* s, const char* text, bool curse) {
  const bool shown = !lv_obj_has_flag(s->bubble, LV_OBJ_FLAG_HIDDEN);
  if (!text) {
    show(s->bubble, false);
    return shown;
  }
  const lv_font_t* f = curse ? &deck_font_14 : &deck_font_12;
  const bool sameFont = lv_obj_get_style_text_font(s->say, LV_PART_MAIN) == f;
  if (shown && sameFont && !strcmp(lv_label_get_text(s->say), text)) return false;
  const int maxW = std::max(20, s->sayMaxW);
  const int tw = textW(text, f) + 1;
  lv_obj_set_style_text_font(s->say, f, 0);
  lv_obj_set_style_text_color(s->say, curse ? lv_color_hex(0xD12F2F) : lv_color_black(), 0);
  lv_label_set_text(s->say, text);
  lv_obj_set_size(s->say, std::min(tw, maxW), (tw > maxW ? 2 : 1) * lineH(f));
  show(s->bubble, true);
  return true;
}

void wiggle(State* s, bool shake);

void* create(lv_obj_t* t, const char* variant, const char*) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  s->detail = !strcmp(variant, "detalle");
  s->robot = lv_obj_create(t);
  lv_obj_remove_style_all(s->robot);
  lv_obj_remove_flag(s->robot, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  // Static parts, then the ones the faces move (they keep these handles)
  // children 0..6: hat top, hat brim, stem, antenna tip, body, two ears (see layout)
  for (uint32_t hex : {kHatHex, kHatHex, kStemHex, kHatHex, kBodyHex, kBodyHex, kBodyHex})
    box(s->robot, hex);
  for (int i = 0; i < 4; i++) s->leg[i] = box(s->robot, kBodyHex);
  for (int i = 0; i < 2; i++) s->cheek[i] = box(s->robot, kBlushHex);
  for (int i = 0; i < 2; i++) s->eye[i] = box(s->robot, 0x000000);
  for (int i = 0; i < 4; i++) {
    s->line[i] = lv_line_create(s->robot);
    lv_obj_set_style_line_color(s->line[i], lv_color_black(), 0);
    lv_obj_set_style_line_rounded(s->line[i], true, 0);
    lv_obj_remove_flag(s->line[i], LV_OBJ_FLAG_CLICKABLE);
  }
  // The speech bubble, last so it draws over the hat; it may stick out of the robot.
  lv_obj_add_flag(s->robot, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  s->bubble = lv_obj_create(s->robot);
  lv_obj_remove_style_all(s->bubble);
  lv_obj_remove_flag(s->bubble, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_size(s->bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s->bubble, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(s->bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(s->bubble, lv_color_black(), 0);
  lv_obj_set_style_border_width(s->bubble, 2, 0);
  lv_obj_set_style_radius(s->bubble, 8, 0);
  lv_obj_set_style_pad_hor(s->bubble, 6, 0);
  lv_obj_set_style_pad_ver(s->bubble, 3, 0);
  lv_obj_add_event_cb(s->bubble, bubbleTail, LV_EVENT_DRAW_POST, nullptr);
  lv_obj_add_event_cb(s->bubble, bubbleTail, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
  s->say = lv_label_create(s->bubble);
  lv_label_set_text(s->say, "");
  lv_label_set_long_mode(s->say, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_remove_flag(s->say, LV_OBJ_FLAG_CLICKABLE);
  show(s->bubble, false);
  // LVGL only redraws a child where its parent (plus its extra draw size) is:
  // the robot's reaches the tile's edge so all of the bubble gets drawn.
  lv_obj_add_event_cb(s->robot, [](lv_event_t* e) {
    const State* st = (const State*)lv_event_get_user_data(e);
    lv_event_set_ext_draw_size(e, st->robotExt);
  }, LV_EVENT_REFR_EXT_DRAW_SIZE, s);
  s->title = label(t, &deck_font_14);
  s->sub = label(t, &deck_font_12, LV_OPA_70);
  for (lv_obj_t* l : {s->title, s->sub}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
    lv_obj_set_style_radius(l, 6, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
  }
  return s;
}

// Lays out the static parts at the current scale.
void layout(State* s) {
  lv_obj_set_size(s->robot, (int)lroundf(kW * s->u), (int)lroundf(kH * s->u));
  lv_obj_t* r = s->robot;
  put(s, lv_obj_get_child(r, 0), kCx - 12, kHead - 33, 24, 10);  // hat, top step
  put(s, lv_obj_get_child(r, 1), kCx - 24, kHead - 23, 48, 12);  // hat, brim
  put(s, lv_obj_get_child(r, 2), kCx - 1, kHead - 47, 3, 14);    // antenna stem
  put(s, lv_obj_get_child(r, 3), kCx - 5, kHead - 58, 10, 10, 5);  // antenna tip
  put(s, lv_obj_get_child(r, 4), kCx - 55, kHead, 110, 65);      // body
  put(s, lv_obj_get_child(r, 5), kCx - 65, kHead + 22, 10, 18);  // ears
  put(s, lv_obj_get_child(r, 6), kCx + 55, kHead + 22, 10, 18);
}

void restArms(State* s);

void drawFace(State* s, Face f) {
  for (int i = 0; i < 4; i++) show(s->line[i], false);
  for (int i = 0; i < 2; i++) show(s->cheek[i], f == F_LOOK || f == F_JOY);
  const bool boxes = f == F_NORMAL || f == F_BLINK || f == F_SLEEP || f == F_LOOK ||
                     f == F_WINK || f == F_WIDE || f == F_DROWSY || f == F_ANGRY;
  for (int i = 0; i < 2; i++) show(s->eye[i], boxes);
  // Angry: the whole robot goes red and both fists go up; anything else, back to normal.
  const bool angry = f == F_ANGRY;
  if (angry != s->angry) {
    s->angry = angry;
    const lv_color_t c = lv_color_hex(angry ? kAngryHex : kBodyHex);
    for (int i = 4; i <= 6; i++) lv_obj_set_style_bg_color(lv_obj_get_child(s->robot, i), c, 0);
    for (lv_obj_t* l : s->leg) lv_obj_set_style_bg_color(l, c, 0);
    restArms(s);
    if (angry)
      for (int i = 5; i <= 6; i++)
        lv_obj_set_style_translate_y(lv_obj_get_child(s->robot, i), -(int)lroundf(14 * s->u), 0);
  }
  const int y = kHead;
  switch (f) {
    case F_ANGRY:  // furious: brows down to the middle over narrowed eyes, and a frown
      setLine(s, 0, kCx - 31, y + 12, kCx - 21, y + 16, kCx - 11, y + 20);
      setLine(s, 1, kCx + 11, y + 20, kCx + 21, y + 16, kCx + 31, y + 12);
      setLine(s, 2, kCx - 11, y + 55, kCx, y + 49, kCx + 11, y + 55);
      put(s, s->eye[0], kCx - 26, y + 24, 10, 13);
      put(s, s->eye[1], kCx + 16, y + 24, 10, 13);
      break;
    case F_GRUMPY:  // woken up and not happy: eyes shut, slanting in
      setLine(s, 0, kCx - 29, y + 24, kCx - 20, y + 27, kCx - 11, y + 30);
      setLine(s, 1, kCx + 11, y + 30, kCx + 20, y + 27, kCx + 29, y + 24);
      break;
    case F_DROWSY:  // half-open eyes
      put(s, s->eye[0], kCx - 26, y + 31, 10, 9);
      put(s, s->eye[1], kCx + 16, y + 31, 10, 9);
      break;
    case F_WINK:  // left eye open, right one shut
      put(s, s->eye[0], kCx - 26, y + 20, 10, 22);
      put(s, s->eye[1], kCx + 14, y + 29, 14, 4);
      break;
    case F_WIDE:  // startled: big round eyes
      put(s, s->eye[0], kCx - 29, y + 13, 14, 26, 7);
      put(s, s->eye[1], kCx + 15, y + 13, 14, 26, 7);
      break;
    case F_NORMAL:
      put(s, s->eye[0], kCx - 26, y + 20, 10, 22);
      put(s, s->eye[1], kCx + 16, y + 20, 10, 22);
      break;
    case F_BLINK:
      put(s, s->eye[0], kCx - 26, y + 29, 10, 4);
      put(s, s->eye[1], kCx + 16, y + 29, 10, 4);
      break;
    case F_SLEEP:
      put(s, s->eye[0], kCx - 28, y + 26, 16, 4);
      put(s, s->eye[1], kCx + 12, y + 26, 16, 4);
      break;
    case F_LOOK:  // waiting for you: eyes up, rosy cheeks
      put(s, s->eye[0], kCx - 26, y + 14, 10, 18, 3);
      put(s, s->eye[1], kCx + 16, y + 14, 10, 18, 3);
      put(s, s->cheek[0], kCx - 33, y + 38, 14, 6, 3);
      put(s, s->cheek[1], kCx + 19, y + 38, 14, 6, 3);
      break;
    case F_HAPPY:  // ^ ^
    case F_JOY:    // ^ ^ and a blush (tapped)
      setLine(s, 0, kCx - 28, y + 32, kCx - 20, y + 20, kCx - 12, y + 32);
      setLine(s, 1, kCx + 12, y + 32, kCx + 20, y + 20, kCx + 28, y + 32);
      if (f == F_JOY) {
        put(s, s->cheek[0], kCx - 35, y + 40, 14, 6, 3);
        put(s, s->cheek[1], kCx + 21, y + 40, 14, 6, 3);
      }
      break;
    case F_FAIL:  // X X
      setLine(s, 0, kCx - 28, y + 20, kCx - 20, y + 28, kCx - 12, y + 36);
      setLine(s, 1, kCx - 28, y + 36, kCx - 20, y + 28, kCx - 12, y + 20);
      setLine(s, 2, kCx + 12, y + 20, kCx + 20, y + 28, kCx + 28, y + 36);
      setLine(s, 3, kCx + 12, y + 36, kCx + 20, y + 28, kCx + 28, y + 20);
      break;
  }
}

void drawLegs(State* s) {
  static const int xs[4] = {kCx - 35, kCx - 22, kCx + 14, kCx + 27};
  for (int i = 0; i < 4; i++) {
    const bool up = (i == 0 || i == 3) == (s->frame == 1);
    put(s, s->leg[i], xs[i], kHead + 65, 8, up ? 13 : 16);
  }
}

void caption(lv_obj_t* l, const char* text, bool pill, uint32_t bg, uint32_t fg) {
  lv_label_set_text(l, text);
  lv_obj_set_style_bg_opa(l, pill ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  if (pill) {
    lv_obj_set_style_bg_color(l, lv_color_hex(bg), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
  } else {
    lv_obj_remove_local_style_prop(l, LV_STYLE_TEXT_COLOR, 0);
  }
}

bool update(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return false;
  const uint32_t now = millis();
  const ClaudeLive& c = pcClaude();
  const bool online = pcClaudeOnline();
  const bool event = c.evSeq && c.evSeq != s->ackSeq && now - c.evRxMs < kEventMs && c.ev[0];

  // What to show: an event first, then the mood
  Face face = F_NORMAL;
  const char *title = "Hi Alejo!", *sub = "";
  uint32_t bg = 0, fg = 0;
  bool pill = false;
  if (event) {
    pill = true;
    title = c.evTxt[0] ? c.evTxt : c.ev;
    sub = c.evSub;
    if (!strcmp(c.ev, "input")) face = F_LOOK, bg = 0xFACC15, fg = 0x000000;
    else if (!strcmp(c.ev, "fail")) face = F_ANGRY, bg = 0xEF4444, fg = 0xFFFFFF;
    else face = F_HAPPY, bg = 0x22C55E, fg = 0x000000;
  } else if (!online) {
    face = F_SLEEP, title = "zZz...", sub = "No PC data";
  } else if (!strcmp(c.mood, "sleeping")) {
    face = F_SLEEP, title = "zZz...";
  } else if (!strcmp(c.mood, "hyped")) {
    face = F_HAPPY, title = "LET'S BUILD!";
  } else if (!strcmp(c.mood, "coding")) {
    title = "Ship code!!", sub = c.status;
  } else if (!strcmp(c.mood, "thinking")) {
    title = "Thinking...", sub = c.status;
  }
  s->shownSeq = event ? c.evSeq : 0;
  s->asleep = !event && face == F_SLEEP;
  s->working = !event && online && (!strcmp(c.mood, "coding") || !strcmp(c.mood, "thinking"));

  // The speech bubble: what it is doing while it works, curses after a failure.
  const bool failing = event && !strcmp(c.ev, "fail");
  const bool busy = !event && online && (!strcmp(c.mood, "coding") || !strcmp(c.mood, "thinking") ||
                                         !strcmp(c.mood, "hyped"));
  char dots[4] = "";
  const char* line = nullptr;
  if (failing) {
    line = kCurses[(now / 450) % (sizeof(kCurses) / sizeof(kCurses[0]))];
    if (c.evSeq != s->failSeq || now - s->shookAt >= kRageShakeMs) {  // shaking with rage
      s->failSeq = c.evSeq;
      s->shookAt = now;
      wiggle(s, true);
    }
  } else if (busy) {
    if (c.status[0] && strcmp(c.status, "Thinking...")) {
      // "Bash: pio run" says "pio run"; a bare tool name stays as it is
      const char* colon = strstr(c.status, ": ");
      line = colon && colon[2] ? colon + 2 : c.status;
    } else {  // just thinking: "." ".." "..."
      const int n = 1 + (now / 400) % 3;
      memset(dots, '.', n);
      dots[n] = 0;
      line = dots;
    }
  }
  if (line && sub == c.status) sub = "";  // the bubble says it already

  // A tap reaction covers the mood for a moment
  if (s->react.until) {
    if ((int32_t)(now - s->react.until) >= 0) {
      s->react.until = 0;
    } else {
      face = s->react.face;
      title = s->react.text;
      pill = true, bg = s->react.bg, fg = s->react.fg;
    }
  }

  // Blink now and then (open eyes only)
  if (face == F_NORMAL) {
    if (!s->nextBlink) s->nextBlink = now + 3000 + esp_random() % 3000;
    if ((int32_t)(now - s->nextBlink) >= 0) {
      face = F_BLINK;
      if (now - s->nextBlink > 150) s->nextBlink = 0;
    }
  }

  bool changed = say(s, line, failing);
  if (now - s->lastFrame >= kFrameMs) {
    s->lastFrame = now;
    s->frame ^= 1;
    drawLegs(s);
    changed = true;
  }
  // FNV-1a over everything drawn below
  uint32_t key = 2166136261u;
  auto mix = [&key](uint32_t v) { key = (key ^ v) * 16777619u; };
  mix(face);
  mix(bg);
  for (const char* t : {title, sub})
    for (const char* q = t; *q; q++) mix((uint8_t)*q);
  if (key != s->key) {
    s->key = key;
    drawFace(s, face);
    caption(s->title, title, pill, bg, fg);
    show(s->sub, s->detail && sub[0]);
    caption(s->sub, sub, false, 0, 0);
    changed = true;
  }
  return changed;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  // Room for the captions under the robot
  const int text = s->detail ? 40 : 22;
  const float u = std::min((float)w / kW, (float)std::max(h - text, 20) / kH);
  s->u = std::max(u, 0.15f);
  layout(s);
  drawLegs(s);
  s->key = UINT32_MAX;
  // The bubble starts right of the antenna and may reach the tile's edge.
  const int rw = (int)lroundf(kW * s->u);
  const int right = rw + std::max(0, (w - rw) / 2) - 2;
  int left = (int)lroundf(0.56f * rw);
  if (right - left < 76) left = std::max(0, right - 76);
  s->sayMaxW = std::min(right - left, 160) - 2 * 6 - 2 * 2;  // less its padding and border
  lv_obj_set_pos(s->bubble, left, -2);
  s->robotExt = std::max(0, right - rw) + kTail + 4;
  lv_obj_refresh_ext_draw_size(s->robot);
  lv_label_set_text(s->say, "");  // re-fit on the next update
}

// The robot's animations only touch the robot, and LVGL ends them when the
// tile (and so the robot) is deleted.
void destroy(void* p) { delete (State*)p; }

// --- Tap reactions --------------------------------------------------------------
void moveY(void* o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t*)o, v, 0); }
void moveX(void* o, int32_t v) { lv_obj_set_style_translate_x((lv_obj_t*)o, v, 0); }

// A hop up and back (sideways shakes when `shake`), kept within the tile's
// padding so the antenna is not clipped.
void wiggle(State* s, bool shake) {
  lv_anim_delete(s->robot, moveY);
  lv_anim_delete(s->robot, moveX);
  lv_obj_set_style_translate_x(s->robot, 0, 0);
  lv_obj_set_style_translate_y(s->robot, 0, 0);
  const int px = std::max(3, std::min(8, (int)lroundf((shake ? 5 : 9) * s->u)));
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s->robot);
  if (shake) {
    lv_anim_set_exec_cb(&a, moveX);
    lv_anim_set_values(&a, -px, px);
    lv_anim_set_duration(&a, 70);
    lv_anim_set_reverse_duration(&a, 70);
    lv_anim_set_repeat_count(&a, 5);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, [](lv_anim_t* an) { moveX(an->var, 0); });
  } else {
    lv_anim_set_exec_cb(&a, moveY);
    lv_anim_set_values(&a, 0, -px);
    lv_anim_set_duration(&a, 130);
    lv_anim_set_reverse_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  }
  lv_anim_start(&a);
}

// The side bits are its arms: children 5 (left) and 6 (right) of the robot.
lv_obj_t* arm(State* s, int i) { return lv_obj_get_child(s->robot, 5 + i); }

void restArms(State* s) {
  for (int i = 0; i < 2; i++) {
    lv_anim_delete(arm(s, i), nullptr);
    lv_obj_set_style_translate_x(arm(s, i), 0, 0);
    lv_obj_set_style_translate_y(arm(s, i), 0, 0);
  }
}

// "Go away": the right arm goes up and flaps outwards a few times.
void shoo(State* s) {
  restArms(s);
  lv_obj_t* a = arm(s, 1);
  lv_obj_set_style_translate_y(a, -(int)lroundf(14 * s->u), 0);
  lv_anim_t an;
  lv_anim_init(&an);
  lv_anim_set_var(&an, a);
  lv_anim_set_exec_cb(&an, moveX);
  lv_anim_set_values(&an, 0, std::max(2, (int)lroundf(6 * s->u)));
  lv_anim_set_delay(&an, 150);
  lv_anim_set_duration(&an, 120);
  lv_anim_set_reverse_duration(&an, 120);
  lv_anim_set_repeat_count(&an, 4);
  lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&an, [](lv_anim_t* x) { moveY(x->var, 0); });
  lv_anim_start(&an);
}

// Waking up: both arms go up slowly, stay a moment, come down.
void stretch(State* s) {
  restArms(s);
  for (int i = 0; i < 2; i++) {
    lv_anim_t an;
    lv_anim_init(&an);
    lv_anim_set_var(&an, arm(s, i));
    lv_anim_set_exec_cb(&an, moveY);
    lv_anim_set_values(&an, 0, -(int)lroundf(16 * s->u));
    lv_anim_set_duration(&an, 450);
    lv_anim_set_reverse_delay(&an, 900);
    lv_anim_set_reverse_duration(&an, 450);
    lv_anim_set_path_cb(&an, lv_anim_path_ease_in_out);
    lv_anim_start(&an);
  }
}

template <size_t N>
const char* pick(const char* const (&lines)[N]) { return lines[esp_random() % N]; }

void react(State* s, Face face, const char* text, uint32_t bg, uint32_t fg, uint32_t ms) {
  s->react.face = face;
  s->react.text = text;
  s->react.bg = bg;
  s->react.fg = fg;
  s->react.until = millis() + ms;
  s->nextBlink = 0;
}

void tap(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return;
  const uint32_t now = millis();
  const bool reacting = s->react.until && (int32_t)(s->react.until - now) > 0;
  if (reacting && s->react.face == F_FAIL) {
    wiggle(s, true);  // still dizzy
    return;
  }
  if (!s->taps || now - s->firstTap > 2500) s->firstTap = now, s->taps = 0;
  if (++s->taps >= 5) {  // too much poking
    s->taps = 0;
    restArms(s);
    react(s, F_FAIL, "I'm dizzy!", 0xEF4444, 0xFFFFFF, 3000);
    wiggle(s, true);
    return;
  }
  // Asleep (and not just woken up): the first tap annoys it, a second one
  // within a few seconds wakes it up, still sleepy. No hop for either.
  if (!s->shownSeq && s->asleep && !(reacting && s->react.face == F_DROWSY)) {
    if (s->pokedAt && now - s->pokedAt < 8000) {
      static const char* const kWake[] = {"*yawn*", "Mmh... I'm up", "Wha...?"};
      s->pokedAt = 0;
      react(s, F_DROWSY, pick(kWake), 0x64748B, 0xFFFFFF, 4000);
      stretch(s);
    } else {
      static const char* const kGrumpy[] = {"Go away...", "5 more minutes...", "Shhh..."};
      s->pokedAt = now;
      react(s, F_GRUMPY, pick(kGrumpy), 0x475569, 0xFFFFFF, 2500);
      shoo(s);
    }
    return;
  }
  restArms(s);
  if (s->shownSeq) {  // an event on screen: the tap dismisses it
    s->ackSeq = s->shownSeq;
    react(s, F_HAPPY, "Got it!", 0x22C55E, 0x000000, 1500);
  } else if (s->working) {  // busy: it did not see you coming
    react(s, F_WIDE, "Huh?!", 0xFACC15, 0x000000, 2000);
  } else {
    static const char* const kLines[] = {"Boop!", "Hehe!", "Beep boop!", "That tickles!",
                                         "Hi Alejo!", "Let's ship it!", "*happy beeps*"};
    react(s, esp_random() % 4 == 0 ? F_WINK : F_JOY, pick(kLines), kHatHex, 0x000000, 2200);
  }
  wiggle(s, false);
}

const Variant kVariants[] = {{"compacto", "Compact"}, {"detalle", "Detail"}};

}  // namespace mascot

// --- Claude: plan usage and context -------------------------------------------
// One meter (5 horas, semana, contexto: name, big %, bar, a detail line when
// there is room) or all three as rows ("todo"). Tap opens the IA app.
namespace quota {

using claudefmt::Kind;

struct Row {
  lv_obj_t *box, *name, *pct, *bar;
};

struct State {
  bool all;
  Kind kind;
  lv_obj_t *name, *pct, *bar, *sub;  // single meter
  Row rows[3];                       // "todo"
  uint32_t key = UINT32_MAX;
  int w = 0, h = 0;
};

const char* kNames[] = {"5 HOURS", "WEEK", "CONTEXT"};

lv_obj_t* makeBar(lv_obj_t* parent) {
  lv_obj_t* b = lv_bar_create(parent);
  lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_bar_set_range(b, 0, 1000);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 4, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 4, LV_PART_INDICATOR);
  return b;
}

void setBar(lv_obj_t* bar, lv_obj_t* pctLabel, float pct) {
  if (pct < 0) {
    lv_label_set_text(pctLabel, "--");
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return;
  }
  lv_label_set_text_fmt(pctLabel, "%d%%", (int)(pct + 0.5f));
  lv_obj_set_style_bg_color(bar, lv_color_hex(claudefmt::level(pct, C_ACCENT, C_WARN, C_CRIT)),
                            LV_PART_INDICATOR);
  lv_bar_set_value(bar, (int32_t)(LV_MIN(pct, 100.0f) * 10), LV_ANIM_OFF);
}

void* create(lv_obj_t* t, const char* variant, const char*) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  s->all = !strcmp(variant, "todo");
  s->kind = !strcmp(variant, "semana") ? claudefmt::K_7D
            : !strcmp(variant, "contexto") ? claudefmt::K_CTX : claudefmt::K_5H;
  if (!s->all) {
    s->name = label(t, &deck_font_12, LV_OPA_70);
    lv_label_set_text(s->name, kNames[s->kind]);
    s->pct = label(t, &deck_font_28);
    s->bar = makeBar(t);
    lv_obj_set_size(s->bar, LV_PCT(86), 6);
    lv_obj_set_style_margin_top(s->bar, 2, 0);
    s->sub = label(t, &deck_font_12, LV_OPA_60);
    lv_label_set_long_mode(s->sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(s->sub, LV_PCT(100), 0);
    return s;
  }
  for (int i = 0; i < 3; i++) {
    Row& r = s->rows[i];
    r.box = lv_obj_create(t);
    lv_obj_remove_style_all(r.box);
    lv_obj_remove_flag(r.box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_set_size(r.box, LV_PCT(100), LV_SIZE_CONTENT);
    r.name = label(r.box, &deck_font_12, LV_OPA_70);
    lv_label_set_text(r.name, kNames[i]);
    lv_obj_align(r.name, LV_ALIGN_TOP_LEFT, 0, 0);
    r.pct = label(r.box, &deck_font_12);
    lv_obj_align(r.pct, LV_ALIGN_TOP_RIGHT, 0, 0);
    r.bar = makeBar(r.box);
    lv_obj_set_size(r.bar, LV_PCT(100), 5);
    lv_obj_align(r.bar, LV_ALIGN_TOP_LEFT, 0, 17);
    lv_obj_set_height(r.box, 22);
  }
  return s;
}

bool update(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return false;
  // New data, or every 30 s for the countdowns.
  const bool online = pcClaudeOnline();
  const uint32_t key = pcVersion() * 4 + (millis() / 30000 % 2) * 2 + (online ? 1 : 0);
  if (key == s->key) return false;
  s->key = key;
  const ClaudeUsage& u = pcClaude().usage;

  if (s->all) {
    const int rowH = s->h / 3;
    for (int i = 0; i < 3; i++) {
      Row& r = s->rows[i];
      show(r.box, rowH >= 18);
      setBar(r.bar, r.pct, claudefmt::pct(u, online, (Kind)i));
    }
    return true;
  }
  const float pct = claudefmt::pct(u, online, s->kind);
  // The big number follows the tile; the detail line only where it fits.
  lv_obj_set_style_text_font(s->pct, s->h >= 80 && s->w >= 90 ? &deck_font_28 : &deck_font_20, 0);
  show(s->name, s->h >= 50);
  setBar(s->bar, s->pct, pct);
  char buf[40];
  claudefmt::detail(buf, sizeof(buf), u, s->kind);
  show(s->sub, s->h >= 90 && pct >= 0 && buf[0]);
  lv_label_set_text(s->sub, buf);
  return true;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  s->key = UINT32_MAX;  // relayout on the next update
}

void destroy(void* p) { delete (State*)p; }

// The first is the default: all three, the one most dashboards want.
const Variant kVariants[] = {
    {"todo", "All"}, {"5h", "5 hours"}, {"semana", "Week"}, {"contexto", "Context"}};

}  // namespace quota

// --- Work: GitLab merge requests and Jira filters ---------------------------
// The tile shows one section of the Work app (its variant): the section's
// name, its count and, where there is room, its first items. A tap opens the
// Work app on that section. Fed by the PC agent's pushes (work_data.h).
namespace work {

constexpr int MAX_LINES = 5;
constexpr int V_ACTIVE = -1, V_PIPES = WORK_SECTIONS;  // GitLab variants besides the sections

struct State {
  bool jira;
  int which;  // GitLab: V_ACTIVE, a WorkSection or V_PIPES; Jira: the filter's index
  lv_obj_t *name, *count, *lines[MAX_LINES];
  uint32_t key = UINT32_MAX;
  int w = 0, h = 0;
};

uint32_t toneColor(char t) {
  switch (t) {
    case 'o': return C_OK;
    case 'w': return C_WARN;
    case 'b': return C_CRIT;
    case 'i': return 0x60A5FA;
    default: return 0xA9B1BD;
  }
}

uint32_t pipeColor(char st) {
  return st == 's' ? C_OK : st == 'f' ? C_CRIT : st == 'r' ? 0x60A5FA : st == 'p' ? C_WARN : 0xA9B1BD;
}

// GitLab variant ids, in the order of the sections (V_ACTIVE first, V_PIPES last).
const Variant kGitlabVariants[] = {
    {"active", "Active"},
    {"returned", "Returned to you"},
    {"review", "Review requested"},
    {"mine", "Your merge requests"},
    {"author", "Waiting for author"},
    {"approvals", "Waiting for approvals"},
    {"by_me", "Approved by you"},
    {"by_others", "Approved by others"},
    {"pipelines", "Pipelines"}};
const Variant kJiraVariants[] = {{"f1", "Filter 1"}, {"f2", "Filter 2"}, {"f3", "Filter 3"},
                                 {"f4", "Filter 4"}, {"f5", "Filter 5"}, {"f6", "Filter 6"}};

void* create(lv_obj_t* t, const char* variant, bool jira) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  s->jira = jira;
  if (jira) {
    s->which = (variant[0] == 'f' && variant[1] >= '1' && variant[1] <= '6') ? variant[1] - '1' : 0;
  } else {
    s->which = V_ACTIVE;
    for (int i = 0; i < (int)(sizeof(kGitlabVariants) / sizeof(kGitlabVariants[0])); i++)
      if (!strcmp(variant, kGitlabVariants[i].id)) s->which = i - 1;  // "active" is -1
  }
  s->name = label(t, &deck_font_12, LV_OPA_70);
  lv_label_set_long_mode(s->name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_max_width(s->name, LV_PCT(100), 0);
  s->count = label(t, &deck_font_28);
  for (lv_obj_t*& l : s->lines) {
    l = label(t, &deck_font_12);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    show(l, false);
  }
  return s;
}
void* createGitlab(lv_obj_t* t, const char* v, const char*) { return create(t, v, false); }
void* createJira(lv_obj_t* t, const char* v, const char*) { return create(t, v, true); }

bool update(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return false;
  const WorkData& d = workData();
  const bool fresh = workFresh();
  const uint32_t key = d.seq * 2 + (fresh ? 1 : 0);
  if (key == s->key) return false;
  s->key = key;

  // How many lines fit under the name and the number.
  const bool big = s->h >= 80 && s->w >= 90;
  lv_obj_set_style_text_font(s->count, big ? &deck_font_28 : &deck_font_20, 0);
  const int room = s->w >= 120 ? LV_MIN(MAX_LINES, (s->h - (big ? 62 : 50)) / 16) : 0;

  char name[40] = "", buf[96];
  int n = -1, shown = 0;
  uint32_t numColor = 0xFFFFFF;
  if (s->jira) {
    const bool has = d.jira && s->which < d.colCount;
    if (has) {
      const WorkCol& c = d.cols[s->which];
      strlcpy(name, c.label, sizeof(name));
      n = c.n;
      numColor = c.color;
      for (int i = 0; i < d.ticketCount && shown < room; i++) {
        const WorkTicket& k = d.tickets[i];
        if (k.col != s->which) continue;
        snprintf(buf, sizeof(buf), "%s  %s", k.key, k.title);
        lv_label_set_text(s->lines[shown], buf);
        lv_obj_set_style_text_color(s->lines[shown], lv_color_hex(toneColor(k.tone)), 0);
        shown++;
      }
    } else {
      snprintf(name, sizeof(name), "Jira filter %d", s->which + 1);
    }
  } else if (s->which == V_PIPES) {
    strlcpy(name, d.ko ? "Pipelines failing" : "Pipelines", sizeof(name));
    if (d.gitlab) {
      n = d.ko ? d.ko : d.pipelineCount;
      numColor = d.ko ? C_CRIT : C_OK;
      for (int i = 0; i < d.pipelineCount && shown < room; i++) {
        const WorkPipeline& pl = d.pipelines[i];
        snprintf(buf, sizeof(buf), "%s  %s", pl.name, pl.ref);
        lv_label_set_text(s->lines[shown], buf);
        lv_obj_set_style_text_color(s->lines[shown], lv_color_hex(pipeColor(pl.status)), 0);
        shown++;
      }
    }
  } else {
    strlcpy(name, s->which == V_ACTIVE ? "Active MRs" : workSectionName(s->which), sizeof(name));
    if (d.gitlab) {
      n = s->which == V_ACTIVE ? d.active : d.sec[s->which];
      for (int i = 0; i < d.mrCount && shown < room; i++) {
        const WorkMr& m = d.mrs[i];
        if (s->which == V_ACTIVE ? m.sec >= WORK_ACTIVE_SECTIONS : m.sec != s->which) continue;
        snprintf(buf, sizeof(buf), "%s  %s", m.chip, m.title);
        lv_label_set_text(s->lines[shown], buf);
        lv_obj_set_style_text_color(s->lines[shown], lv_color_hex(toneColor(m.tone)), 0);
        shown++;
      }
    }
  }
  lv_label_set_text(s->name, name);
  show(s->name, s->h >= 44);
  if (n < 0) lv_label_set_text(s->count, "--");
  else lv_label_set_text_fmt(s->count, "%d", n);
  lv_obj_set_style_text_color(s->count, lv_color_hex(numColor), 0);
  lv_obj_set_style_opa(s->count, fresh ? LV_OPA_COVER : LV_OPA_50, 0);  // old data: dimmed
  for (int i = 0; i < MAX_LINES; i++) show(s->lines[i], i < shown);
  return true;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  s->key = UINT32_MAX;  // relayout on the next update
}

void destroy(void* p) { delete (State*)p; }

// Opens the Work app on this tile's section.
void tap(void* p) {
  State* s = (State*)p;
  if (!s) return;
  if (s->jira) app::workFocus(1, s->which);
  else app::workFocus(0, s->which == V_ACTIVE ? WORK_SEC_REVIEW : s->which);
  openApp(-1, "work");
}

}  // namespace work

// --- PC: its load or its batteries as rings, like macOS's Batteries widget -----
// "rings": rings with the item's icon inside and its percentage under it, in
// the row or grid that makes them biggest (fewer when the tile is small): the
// PC's first four of CPU, RAM, GPU, disk and battery. "list": a row each,
// icon, name, percentage and a bar, the PC's name on top when there is room.
// "batteries" / "batteries-list": the same for the PC's own battery and its
// wireless accessories' (keyboards, mice, AirPods and their case...), a bolt
// on what is charging. From the tile's PC ("pc"), else the first PC online; an
// offline PC keeps its last values, dimmed.
namespace pcstats {

struct Kind {
  uint8_t id;        // the agent's metric id (pc-agent/touchdeck_agent.py Metrics)
  const char* name;
  const char* icon;
  bool charge;       // a level that is good when high (battery); else a load
};
const Kind kKinds[] = {{1, "CPU", "mdi:cpu-64-bit", false},     {2, "RAM", "mdi:memory", false},
                       {4, "GPU", "mdi:expansion-card", false}, {8, "Disk", "mdi:harddisk", false},
                       {11, "Battery", "mdi:battery", true}};
constexpr int KINDS = sizeof(kKinds) / sizeof(kKinds[0]);
constexpr int MAX = PC_MAX_BATTERIES;  // items shown at most
constexpr int MAX_LOAD = 4;            // load rings, as macOS's widget
constexpr int LABEL_H = 18;            // the percentage under a ring
constexpr int MIN_RING = 28;

// What one ring or row shows.
struct Entry {
  char name[24];
  char icon[28];
  float v;          // NAN = unknown
  bool charge;      // battery: good when high
  bool charging;
};

struct Item {
  lv_obj_t *box, *arc, *icon, *pct, *name, *bar, *bolt;
  uint32_t cp = 0;  // icon codepoint shown, 0 = none, UINT32_MAX = the name as text
  int iconPx = 0;
  bool boltSet = false;
};

struct State {
  bool list, batteries;
  String pc;
  lv_obj_t *root, *title;
  Item items[MAX];
  int n = -1;                  // items laid out (-1 = not yet)
  uint32_t key = UINT32_MAX;
  int w = 0, h = 0;
  bool relayout = true;
};

// The tile's PC, else the first one online; for batteries, the first online
// one that reports any (a desktop has none of its own).
const PcSource* pick(const State* s) {
  if (s->pc.length()) return pcSource(s->pc);
  if (!pcSources) return nullptr;
  for (int pass = s->batteries ? 0 : 1; pass < 2; pass++)
    for (int i = 0; i < MAX_PCS; i++) {
      const PcSource& c = pcSources[i];
      if (c.used && c.data.online && (pass || c.state.battCount)) return &c;
    }
  return pcSources[0].used ? &pcSources[0] : nullptr;
}

// The PC's last value of a metric, online or not; NAN when it never sent it.
float value(const PcSource* pc, uint8_t id) {
  if (!pc) return NAN;
  for (uint8_t i = 0; i < pc->data.count; i++)
    if (pc->data.metrics[i].id == id) return pc->data.metrics[i].value;
  return NAN;
}

uint32_t levelColor(const Entry& e) {
  const float v = isnan(e.v) ? 0 : e.v;
  if (e.charge) return e.charging || v > 20 ? C_OK : v > 10 ? C_WARN : C_CRIT;
  return v < 70 ? C_OK : v < 90 ? C_WARN : C_CRIT;
}

const char* deviceIcon(char kind) {
  switch (kind) {
    case 'l': return "mdi:laptop";
    case 'k': return "mdi:keyboard";
    case 'm': return "mdi:mouse";
    case 't': return "mdi:gesture-tap";
    case 'h': return "mdi:headphones";
    case 'c': return "mdi:earbuds";
    case 'g': return "mdi:gamepad-variant";
    case 's': return "mdi:speaker";
    case 'p': return "mdi:cellphone";
    case 'w': return "mdi:watch";
    default: return "mdi:bluetooth";
  }
}

// The items to show, in order; returns how many.
int entries(const State* s, const PcSource* pc, Entry* out) {
  int n = 0;
  if (s->batteries) {
    const PcState* st = pc ? &pc->state : nullptr;  // the last list, online or not
    for (int i = 0; st && i < st->battCount && n < MAX; i++) {
      const PcBattery& b = st->batt[i];
      Entry& e = out[n++];
      strlcpy(e.name, b.name, sizeof(e.name));
      strlcpy(e.icon, deviceIcon(b.kind), sizeof(e.icon));
      e.v = b.pct;
      e.charge = true;
      e.charging = b.charging;
    }
    if (!n) {  // nothing reports a battery (yet)
      Entry& e = out[n++];
      strlcpy(e.name, "No batteries", sizeof(e.name));
      strlcpy(e.icon, "mdi:battery-unknown", sizeof(e.icon));
      e.v = NAN;
      e.charge = true;
      e.charging = false;
    }
    return n;
  }
  for (int k = 0; k < KINDS && n < MAX_LOAD; k++) {
    const float v = value(pc, kKinds[k].id);
    if (isnan(v)) continue;
    Entry& e = out[n++];
    strlcpy(e.name, kKinds[k].name, sizeof(e.name));
    e.v = v;
    e.charge = kKinds[k].charge;
    e.charging = false;
    // The battery's icon follows its level, as macOS's does.
    const int tens = (int)(v + 5) / 10;
    if (!e.charge) strlcpy(e.icon, kKinds[k].icon, sizeof(e.icon));
    else if (tens >= 10) strlcpy(e.icon, "mdi:battery", sizeof(e.icon));
    else if (tens <= 0) strlcpy(e.icon, "mdi:battery-outline", sizeof(e.icon));
    else snprintf(e.icon, sizeof(e.icon), "mdi:battery-%d0", tens);
  }
  if (!n) {  // nothing yet: the usual three, empty
    for (int k : {0, 1, 3}) {
      Entry& e = out[n++];
      strlcpy(e.name, kKinds[k].name, sizeof(e.name));
      strlcpy(e.icon, kKinds[k].icon, sizeof(e.icon));
      e.v = NAN;
      e.charge = e.charging = false;
    }
  }
  return n;
}

lv_obj_t* plainBox(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

void* create(lv_obj_t* t, const char* variant, const char* pc) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  s->list = !strcmp(variant, "list") || !strcmp(variant, "batteries-list");
  s->batteries = !strncmp(variant, "batteries", 9);
  s->pc = pc ? pc : "";
  s->root = plainBox(t);
  lv_obj_set_size(s->root, LV_PCT(100), LV_PCT(100));
  s->title = label(s->root, &deck_font_12, LV_OPA_60);
  lv_label_set_long_mode(s->title, LV_LABEL_LONG_MODE_DOTS);
  show(s->title, false);
  for (Item& it : s->items) {
    it.box = plainBox(s->root);
    it.icon = label(it.box, &deck_font_12);
    it.pct = label(it.box, s->list ? &deck_font_12 : &deck_font_14);
    it.bolt = label(it.box, &deck_font_12);
    lv_obj_set_style_text_color(it.bolt, lv_color_hex(C_OK), 0);
    show(it.bolt, false);
    if (s->list) {
      it.name = label(it.box, &deck_font_12, LV_OPA_70);
      lv_label_set_long_mode(it.name, LV_LABEL_LONG_MODE_DOTS);
      it.bar = quota::makeBar(it.box);
      it.arc = nullptr;
    } else {
      lv_obj_t* a = it.arc = lv_arc_create(it.box);
      lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
      lv_arc_set_rotation(a, 270);
      lv_arc_set_bg_angles(a, 0, 360);
      lv_arc_set_range(a, 0, 1000);
      lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(a, 0, 0);
      lv_obj_set_style_arc_opa(a, LV_OPA_30, LV_PART_MAIN);  // the track: the ring's colour, faint
      lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
      lv_obj_set_parent(it.icon, a);
      lv_obj_center(it.icon);
      it.name = it.bar = nullptr;
    }
    show(it.box, false);
  }
  return s;
}

// Icon sizes the deck already draws elsewhere: each new size takes one of the
// icon font's few slots (deck_icons.cpp MAX_FONTS), so the rings snap to these.
int iconSize(int want) {
  static const int kSizes[] = {40, 28, 20, 16, 14};
  for (int px : kSizes)
    if (want >= px) return px;
  return 14;
}

// Rings in `cols` x `rows` cells: the ring diameter that fits.
int ringFit(int w, int h, int cols, int rows) {
  return LV_MIN(w / cols - 6, h / rows - LABEL_H - 2);
}

// Places the first n items.
void layout(State* s, int n) {
  for (int i = 0; i < MAX; i++) show(s->items[i].box, i < n);
  if (s->list) {
    const bool title = s->h >= n * 22 + 16;
    show(s->title, title);
    lv_obj_set_size(s->title, s->w, 15);  // one line, dots at the end
    lv_obj_set_pos(s->title, 0, 0);
    const int top = title ? 16 : 0;
    const int rowH = n ? LV_MIN(30, (s->h - top) / n) : 0;
    const int y0 = top + ((s->h - top) - rowH * n) / 2;
    for (int i = 0; i < n; i++) {
      Item& it = s->items[i];
      lv_obj_set_pos(it.box, 0, y0 + i * rowH);
      lv_obj_set_size(it.box, s->w, rowH);
      lv_obj_align(it.icon, LV_ALIGN_TOP_LEFT, 0, 0);
      lv_obj_set_height(it.name, 15);  // one line; its width follows the bolt (update)
      lv_obj_align(it.name, LV_ALIGN_TOP_LEFT, 18, 0);
      lv_obj_align(it.pct, LV_ALIGN_TOP_RIGHT, 0, 0);
      lv_obj_align(it.bolt, LV_ALIGN_TOP_RIGHT, -32, 0);
      lv_obj_set_size(it.bar, s->w, 5);
      lv_obj_align(it.bar, LV_ALIGN_TOP_LEFT, 0, 17);
      it.iconPx = 14;
      it.cp = 0;
      it.boltSet = false;
    }
    return;
  }
  show(s->title, false);
  int best = 0, cols = 1, rows = 1;
  for (int r = 1; r <= n; r++) {
    const int c = (n + r - 1) / r;
    const int d = ringFit(s->w, s->h, c, r);
    if (d > best) best = d, cols = c, rows = r;
  }
  const int d = LV_MIN(best, 96);
  const int cw = s->w / cols, ch = s->h / rows;
  for (int i = 0; i < n; i++) {
    Item& it = s->items[i];
    const int row = i / cols, col = i % cols;
    const int inRow = LV_MIN(cols, n - row * cols);
    lv_obj_set_pos(it.box, (s->w - inRow * cw) / 2 + col * cw, row * ch);
    lv_obj_set_size(it.box, cw, ch);
    const int top = (ch - d - LABEL_H) / 2;
    lv_obj_set_size(it.arc, d, d);
    lv_obj_align(it.arc, LV_ALIGN_TOP_MID, 0, top);
    const int width = LV_MAX(3, d / 9);
    lv_obj_set_style_arc_width(it.arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(it.arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(it.pct, d >= 50 ? &deck_font_14 : &deck_font_12, 0);
    lv_obj_align(it.pct, LV_ALIGN_TOP_MID, 0, top + d + 2);  // kept centred as the text changes
    lv_obj_align(it.bolt, LV_ALIGN_TOP_MID, d / 2, top - 2);  // a badge at the ring's top right
    it.iconPx = iconSize(d * 44 / 100);
    it.cp = 0;
    it.boltSet = false;
  }
}

// How many items fit: rings of MIN_RING or more, list rows of 20 px.
int fitting(const State* s, int want) {
  for (int n = want; n > 1; n--) {
    if (s->list) {
      if (s->h >= n * 20) return n;
      continue;
    }
    for (int r = 1; r <= n; r++)
      if (ringFit(s->w, s->h, (n + r - 1) / r, r) >= MIN_RING) return n;
  }
  return want ? 1 : 0;
}

// The icon, else (no icon font, or this font lacks it) a battery, else the name.
void setIcon(Item& it, const Entry& e, bool text) {
  uint32_t cp = iconsReady() ? iconCodepoint(e.icon) : 0;
  if (!cp && iconsReady()) cp = iconCodepoint("mdi:battery");
  const lv_font_t* f = cp ? iconFont(it.iconPx, cp) : nullptr;
  if (f && cp != it.cp) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(it.icon, f, 0);
    lv_label_set_text(it.icon, u);
    it.cp = cp;
  } else if (!f && it.cp != UINT32_MAX) {
    lv_obj_set_style_text_font(it.icon, &deck_font_12, 0);
    lv_label_set_text(it.icon, text ? e.name : "");
    it.cp = UINT32_MAX;
  }
  if (it.arc) lv_obj_center(it.icon);
  // The charging bolt, from the icon font (none without it).
  if (!it.boltSet) {
    const uint32_t bcp = iconsReady() ? iconCodepoint("mdi:lightning-bolt") : 0;
    const lv_font_t* bf = bcp ? iconFont(14, bcp) : nullptr;
    if (bf) {
      char u[5];
      iconUtf8(bcp, u);
      lv_obj_set_style_text_font(it.bolt, bf, 0);
      lv_label_set_text(it.bolt, u);
      it.boltSet = true;
    }
  }
  show(it.bolt, e.charging && it.boltSet);
}

bool update(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return false;
  const uint32_t key = pcVersion() * 2 + (iconsReady() ? 1 : 0);
  if (key == s->key && !s->relayout) return false;
  s->key = key;

  const PcSource* pc = pick(s);
  Entry e[MAX];
  const int n = fitting(s, entries(s, pc, e));
  if (s->relayout || n != s->n) {
    s->n = n;
    s->relayout = false;
    layout(s, n);
  }
  lv_label_set_text(s->title, pc ? pc->name : "No PC yet");
  lv_obj_set_style_opa(s->root, pc && pc->data.online ? LV_OPA_COVER : LV_OPA_50, 0);
  for (int i = 0; i < n; i++) {
    Item& it = s->items[i];
    const uint32_t c = levelColor(e[i]);
    if (isnan(e[i].v)) lv_label_set_text(it.pct, "--");
    else lv_label_set_text_fmt(it.pct, "%d%%", (int)lroundf(e[i].v));
    const int32_t at = isnan(e[i].v) ? 0 : (int32_t)(constrain(e[i].v, 0.0f, 100.0f) * 10);
    if (it.arc) {
      lv_obj_set_style_arc_color(it.arc, lv_color_hex(c), LV_PART_MAIN);
      lv_obj_set_style_arc_color(it.arc, lv_color_hex(c), LV_PART_INDICATOR);
      lv_arc_set_value(it.arc, at);
    } else {
      lv_label_set_text(it.name, e[i].name);
      // Icon 18 px, "100%" 30, the bolt 16 while charging: the name gets the rest.
      lv_obj_set_width(it.name, LV_MAX(10, s->w - 18 - 30 - (e[i].charging ? 16 : 0)));
      lv_obj_set_style_bg_color(it.bar, lv_color_hex(c), LV_PART_INDICATOR);
      lv_bar_set_value(it.bar, at, LV_ANIM_OFF);
    }
    setIcon(it, e[i], it.arc ? lv_obj_get_width(it.arc) >= 36 : false);
  }
  return true;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  s->relayout = true;
}

void destroy(void* p) { delete (State*)p; }

const Variant kVariants[] = {
    {"rings", "Rings"}, {"list", "List"}, {"batteries", "Batteries"}, {"batteries-list", "Batteries list"}};

}  // namespace pcstats

// --- Registry -----------------------------------------------------------------
const Def kWeather = {"tiempo", "Weather", nullptr, kWeatherVariants, 2,
                      weatherCreate, weatherUpdate, weatherResize, weatherDestroy};

const Def kMascot = {"mascota", "Claude mascot", nullptr, mascot::kVariants, 2,
                     mascot::create, mascot::update, mascot::resize, mascot::destroy, mascot::tap};

const Def kQuota = {"claude", "Claude usage", "ia", quota::kVariants, 4,
                    quota::create, quota::update, quota::resize, quota::destroy};

// No "app": the tap goes to the widget, which opens the Work app on its section.
const Def kGitlab = {"gitlab", "GitLab MRs", nullptr, work::kGitlabVariants,
                     sizeof(work::kGitlabVariants) / sizeof(work::kGitlabVariants[0]),
                     work::createGitlab, work::update, work::resize, work::destroy, work::tap};

const Def kJira = {"jira", "Jira filter", nullptr, work::kJiraVariants,
                   sizeof(work::kJiraVariants) / sizeof(work::kJiraVariants[0]),
                   work::createJira, work::update, work::resize, work::destroy, work::tap};

// No app; "pc": the editors offer the tile's PC (else the first one online).
const Def kPc = {"pc", "PC stats", nullptr, pcstats::kVariants, 4,
                 pcstats::create, pcstats::update, pcstats::resize, pcstats::destroy, nullptr, true};

const Def* const kWidgets[] = {&kWeather, &kMascot, &kQuota, &kGitlab, &kJira, &kPc};
constexpr size_t kCount = sizeof(kWidgets) / sizeof(kWidgets[0]);

}  // namespace

const Def* find(const char* id) {
  for (const Def* d : kWidgets)
    if (!strcmp(d->id, id)) return d;
  return nullptr;
}

size_t count() { return kCount; }
const Def* get(size_t i) { return i < kCount ? kWidgets[i] : nullptr; }

const char* variantOr(const Def* d, const char* variant) {
  for (uint8_t i = 0; i < d->variantCount; i++)
    if (!strcmp(d->variants[i].id, variant)) return d->variants[i].id;
  return d->variants[0].id;
}

}  // namespace widget
