// Widget registry, the weather widget and the Claude mascot. See widget.h.
#include "widget.h"
#include "deck_ui_int.h"
#include "companion_settings.h"
#include "weather_service.h"
#include "pc_metrics.h"

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

void* weatherCreate(lv_obj_t* t, const char* variant) {
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
  snprintf(buf, sizeof(buf), "Humedad %d%%", d.humidity);
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

const Variant kWeatherVariants[] = {{"compacto", "Compacto"}, {"detalle", "Detalle"}};

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
                   kBlushHex = 0xFF7A8A;

enum Face : uint8_t { F_NORMAL, F_BLINK, F_SLEEP, F_HAPPY, F_FAIL, F_LOOK };

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
};

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

void* create(lv_obj_t* t, const char* variant) {
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

void drawFace(State* s, Face f) {
  for (int i = 0; i < 4; i++) show(s->line[i], false);
  for (int i = 0; i < 2; i++) show(s->cheek[i], f == F_LOOK);
  const bool boxes = f == F_NORMAL || f == F_BLINK || f == F_SLEEP || f == F_LOOK;
  for (int i = 0; i < 2; i++) show(s->eye[i], boxes);
  const int y = kHead;
  switch (f) {
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
      setLine(s, 0, kCx - 28, y + 32, kCx - 20, y + 20, kCx - 12, y + 32);
      setLine(s, 1, kCx + 12, y + 32, kCx + 20, y + 20, kCx + 28, y + 32);
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
  const bool event = c.evSeq && now - c.evRxMs < kEventMs && c.ev[0];

  // What to show: an event first, then the mood
  Face face = F_NORMAL;
  const char *title = "Hola Alejo!", *sub = "";
  uint32_t bg = 0, fg = 0;
  bool pill = false;
  if (event) {
    pill = true;
    title = c.evTxt[0] ? c.evTxt : c.ev;
    sub = c.evSub;
    if (!strcmp(c.ev, "input")) face = F_LOOK, bg = 0xFACC15, fg = 0x000000;
    else if (!strcmp(c.ev, "fail")) face = F_FAIL, bg = 0xEF4444, fg = 0xFFFFFF;
    else face = F_HAPPY, bg = 0x22C55E, fg = 0x000000;
  } else if (!online) {
    face = F_SLEEP, title = "zZz...", sub = "Sin datos del PC";
  } else if (!strcmp(c.mood, "sleeping")) {
    face = F_SLEEP, title = "zZz...";
  } else if (!strcmp(c.mood, "hyped")) {
    face = F_HAPPY, title = "LET'S BUILD!";
  } else if (!strcmp(c.mood, "coding")) {
    title = "Ship code!!", sub = c.status;
  } else if (!strcmp(c.mood, "thinking")) {
    title = "Thinking...", sub = c.status;
  }

  // Blink now and then (open eyes only)
  if (face == F_NORMAL) {
    if (!s->nextBlink) s->nextBlink = now + 3000 + esp_random() % 3000;
    if ((int32_t)(now - s->nextBlink) >= 0) {
      face = F_BLINK;
      if (now - s->nextBlink > 150) s->nextBlink = 0;
    }
  }

  bool changed = false;
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
}

void destroy(void* p) { delete (State*)p; }

const Variant kVariants[] = {{"compacto", "Compacto"}, {"detalle", "Detalle"}};

}  // namespace mascot

// --- Registry -----------------------------------------------------------------
const Def kWeather = {"tiempo", "Tiempo", nullptr, kWeatherVariants, 2,
                      weatherCreate, weatherUpdate, weatherResize, weatherDestroy};

const Def kMascot = {"mascota", "Mascota Claude", nullptr, mascot::kVariants, 2,
                     mascot::create, mascot::update, mascot::resize, mascot::destroy};

const Def* const kWidgets[] = {&kWeather, &kMascot};
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
