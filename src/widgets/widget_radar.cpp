// Radar widget: the flight radar's disc in a tile ({"type": "widget", "widget":
// "radar"}), at the app's radius; a tap opens the Radar app.
//
// The same phosphor disc as the app (apps/app_radar.cpp), sized to the tile:
// a base picture with the disc and its rings, and every frame the sweep (in
// step with the app's and the LED strip's, all from millis()) and the planes,
// which light up as the beam passes and fade over a turn. It animates and
// keeps the flight data coming only while its tile is on screen, and lets go
// of its pictures while an app is open (the Radar app needs ~580 KB).
#include "widgets/widget.h"
#include "ui/deck_ui_int.h"
#include "core/companion_settings.h"
#include "services/flight_service.h"
#include <math.h>

namespace widget {
namespace {

using namespace deckui;
using flights::Plane;
using flights::Snapshot;

constexpr int GLOW = 3;
constexpr int MAX_SIDE = 208;      // canvas side cap: 2 x 173 KB of PSRAM plus the angle table
constexpr int MIN_SIDE = 48;
constexpr uint32_t FRAME_MS = 50;  // 20 fps
constexpr uint32_t SWEEP_MS = LED_RADAR_SWEEP_MS;
constexpr int ANG = 1024;          // angle units per turn
constexpr int TRAIL = ANG * 70 / 360;
constexpr uint16_t OUT = 0xFFFF;
constexpr uint32_t GREEN = 0x34F5A0, DISC = 0x021410, DIM = 0x3E8A6A;
constexpr uint32_t KIND_COL[3] = {0x7DFFC8, 0xFF5A5A, 0x58A6FF};  // commercial, military, cargo

struct State {
  lv_obj_t* tile = nullptr;
  lv_obj_t* canvas = nullptr;
  lv_obj_t* note = nullptr;     // "25 km", or what is missing
  uint32_t *base = nullptr, *px = nullptr;
  uint16_t* lut = nullptr;
  int side = 0, r = 0, c = 0;   // canvas side, disc radius, centre
  int w = 0, h = 0;
  Snapshot* snap = nullptr;     // PSRAM: 30 planes
  uint32_t seq = 0xFFFFFFFF, frameAt = 0;
  uint32_t litAt[flights::MAX] = {};
  int prevSweep = 0;
  uint8_t km = 0;
  uint32_t buildTry = 0;        // last attempt to rebuild after an app (no retry storm)
  bool holding = false;         // holds the flight data (flights::HOLD_WIDGET)
};

int s_holders = 0;  // radar widgets on screen

void hold(State* s, bool on) {
  if (s->holding == on) return;
  s->holding = on;
  s_holders += on ? 1 : -1;
  flights::setActive(s_holders > 0, flights::HOLD_WIDGET);
}

inline uint32_t argb(uint32_t rgb, uint8_t a) { return (uint32_t)a << 24 | (rgb & 0xFFFFFF); }

inline void blend(uint32_t& p, uint32_t rgb, uint8_t alpha) {
  if (!alpha) return;
  const uint32_t inv = 255 - alpha;
  const uint32_t r = (((rgb >> 16) & 0xFF) * alpha + ((p >> 16) & 0xFF) * inv) / 255;
  const uint32_t g = (((rgb >> 8) & 0xFF) * alpha + ((p >> 8) & 0xFF) * inv) / 255;
  const uint32_t b = ((rgb & 0xFF) * alpha + (p & 0xFF) * inv) / 255;
  uint32_t a = p >> 24;
  if (alpha > a) a = alpha;
  p = a << 24 | r << 16 | g << 8 | b;
}

void blendAt(State* s, uint32_t* buf, int x, int y, uint32_t rgb, uint8_t a) {
  if (x < 0 || y < 0 || x >= s->side || y >= s->side) return;
  blend(buf[y * s->side + x], rgb, a);
}

void fillRect(State* s, int x, int y, int w, int h, uint32_t rgb, uint8_t a) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) blendAt(s, s->px, i, j, rgb, a);
}

void line(State* s, uint32_t* buf, int x0, int y0, int x1, int y1, uint32_t rgb, uint8_t a) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    blendAt(s, buf, x0, y0, rgb, a);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void ring(State* s, int rr, uint8_t a) {
  int x = rr, y = 0, e = 1 - rr;
  while (x >= y) {
    const int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
    for (auto& p : pts) blendAt(s, s->base, s->c + p[0], s->c + p[1], GREEN, a);
    y++;
    if (e < 0) e += 2 * y + 1;
    else {
      x--;
      e += 2 * (y - x) + 1;
    }
  }
}

void freeBuffers(State* s) {
  if (s->canvas) {
    lv_obj_delete(s->canvas);
    s->canvas = nullptr;
  }
  heap_caps_free(s->base);
  heap_caps_free(s->px);
  heap_caps_free(s->lut);
  s->base = s->px = nullptr;
  s->lut = nullptr;
  s->side = 0;
}

// The disc for a canvas side: angle table and base picture.
bool build(State* s, int side) {
  freeBuffers(s);
  const size_t n = (size_t)side * side;
  s->base = (uint32_t*)heap_caps_aligned_alloc(64, n * 4, MALLOC_CAP_SPIRAM);
  s->px = (uint32_t*)heap_caps_aligned_alloc(64, n * 4, MALLOC_CAP_SPIRAM);
  s->lut = (uint16_t*)heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
  if (!s->base || !s->px || !s->lut) {
    freeBuffers(s);
    return false;
  }
  s->side = side;
  s->c = side / 2;
  s->r = s->c - GLOW;
  const int R = s->r, C = s->c;
  for (int j = 0; j < side; j++)
    for (int i = 0; i < side; i++) {
      const int dx = i - C, dy = j - C, d2 = dx * dx + dy * dy;
      uint16_t v = OUT;
      if (d2 <= R * R) {
        float a = atan2f((float)dx, (float)-dy);  // 0 = north, clockwise
        if (a < 0) a += 2 * PI;
        v = (uint16_t)(a / (2 * PI) * ANG) % ANG;
      }
      s->lut[j * side + i] = v;
      uint32_t& p = s->base[j * side + i];
      p = d2 <= R * R ? argb(DISC, 235) : 0;
      const float e = fabsf(sqrtf((float)d2) - R);
      if (e < GLOW) blend(p, GREEN, (uint8_t)(e < 1 ? 230 : 120 / (e * e)));
    }
  ring(s, R / 3, 70);
  ring(s, 2 * R / 3, 70);
  line(s, s->base, C - R + 2, C, C + R - 2, C, GREEN, 40);
  line(s, s->base, C, C - R + 2, C, C + R - 2, GREEN, 40);
  for (int j = -1; j <= 1; j++)
    for (int i = -1; i <= 1; i++) blendAt(s, s->base, C + i, C + j, GREEN, 255);
  memcpy(s->px, s->base, n * 4);
  s->canvas = lv_canvas_create(s->tile);
  lv_canvas_set_buffer(s->canvas, s->px, side, side, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_remove_flag(s->canvas, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_move_to_index(s->canvas, 0);  // above the note
  return true;
}

int sweepAngle(uint32_t now) { return (int)((now % SWEEP_MS) * ANG / SWEEP_MS); }

void planeXY(State* s, const Plane& p, uint32_t now, int& x, int& y) {
  const float b = p.bearing * DEG_TO_RAD;
  float ex = p.distKm * sinf(b), ny = p.distKm * cosf(b);
  if (p.track >= 0 && p.gsKt > 0 && s->snap->at) {
    const float t = std::min(30.0f, (now - s->snap->at) / 1000.0f);
    const float km = p.gsKt * 1.852f / 3600.0f * t;
    ex += km * sinf(p.track * DEG_TO_RAD);
    ny += km * cosf(p.track * DEG_TO_RAD);
  }
  const float k = (s->r - 4) / (float)companion::settings().radarKm;
  x = s->c + (int)lroundf(ex * k);
  y = s->c - (int)lroundf(ny * k);
}

void drawFrame(State* s, uint32_t now) {
  const int side = s->side, sw = sweepAngle(now);
  memcpy(s->px, s->base, (size_t)side * side * 4);
  for (int j = 0; j < side; j++) {
    const uint16_t* row = s->lut + j * side;
    uint32_t* out = s->px + j * side;
    for (int i = 0; i < side; i++) {
      const uint16_t a = row[i];
      if (a == OUT) continue;
      const int diff = (sw - a) & (ANG - 1);
      if (diff >= TRAIL) continue;
      blend(out[i], GREEN, diff < 3 ? 210 : 120 * (TRAIL - diff) / TRAIL);
    }
  }
  // Planes: a dot each (a small tile has no room for headings or names)
  const int span = (sw - s->prevSweep) & (ANG - 1);
  const bool big = side >= 120;
  for (int i = 0; i < s->snap->count; i++) {
    const Plane& p = s->snap->planes[i];
    int x, y;
    planeXY(s, p, now, x, y);
    const int dx = x - s->c, dy = y - s->c;
    if (dx * dx + dy * dy > (s->r - 2) * (s->r - 2)) continue;
    float a = atan2f((float)dx, (float)-dy);
    if (a < 0) a += 2 * PI;
    const int ang = (int)(a / (2 * PI) * ANG) % ANG;
    if (((ang - s->prevSweep) & (ANG - 1)) <= span) s->litAt[i] = now;
    float b = s->litAt[i] ? 1.0f - (now - s->litAt[i]) / (float)(SWEEP_MS - 300) : 0.0f;
    if (b < 0.18f) b = 0.18f;
    const uint32_t col = KIND_COL[p.kind % 3];
    const int hr = big ? 2 : 1;
    fillRect(s, x - hr - 3, y - hr - 3, 2 * hr + 7, 2 * hr + 7, col, (uint8_t)(50 * b));
    fillRect(s, x - hr, y - hr, 2 * hr + 1, 2 * hr + 1, col, (uint8_t)(255 * b));
    if (big && !p.ground && p.track >= 0) {
      const float t = p.track * DEG_TO_RAD;
      line(s, s->px, x, y, x + (int)lroundf(8 * sinf(t)), y - (int)lroundf(8 * cosf(t)), col, (uint8_t)(255 * b));
    }
  }
  s->prevSweep = sw;
  lv_image_cache_drop(lv_image_get_src(s->canvas));
  lv_obj_invalidate(s->canvas);
}

void noteText(State* s) {
  const Snapshot& sn = *s->snap;
  char b[24];
  if (sn.state == flights::State::NoNet) snprintf(b, sizeof(b), "No network");
  else if (!sn.at && sn.state == flights::State::Error) snprintf(b, sizeof(b), "API error");
  else if (!sn.at) snprintf(b, sizeof(b), "%u km", (unsigned)companion::settings().radarKm);
  else snprintf(b, sizeof(b), "%u km · %d", (unsigned)companion::settings().radarKm, sn.count);
  lv_label_set_text(s->note, b);
}

// A canvas as big as the tile allows, with room under it for the note.
void layout(State* s) {
  const bool withNote = s->h >= 90;
  lv_obj_remove_flag(s->note, LV_OBJ_FLAG_HIDDEN);
  if (!withNote) lv_obj_add_flag(s->note, LV_OBJ_FLAG_HIDDEN);
  int side = std::min(s->w, s->h - (withNote ? 16 : 0));
  side = std::min(side, MAX_SIDE) & ~1;
  if (side < MIN_SIDE || appShown()) {  // too small, or an app needs the memory
    freeBuffers(s);
    return;
  }
  if (side != s->side && !build(s, side)) lv_label_set_text(s->note, "No memory");
  s->km = 0;
}

void* create(lv_obj_t* t, const char*, const char*) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  s->snap = (Snapshot*)heap_caps_malloc(sizeof(Snapshot), MALLOC_CAP_SPIRAM);
  if (!s->snap) {
    delete s;
    return nullptr;
  }
  new (s->snap) Snapshot();
  s->tile = t;
  lv_obj_set_style_pad_row(t, 2, 0);
  s->note = lv_label_create(t);
  lv_obj_set_style_text_font(s->note, &deck_font_12, 0);
  lv_obj_set_style_text_color(s->note, lv_color_hex(DIM), 0);
  lv_obj_remove_flag(s->note, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(s->note, "");
  return s;
}

bool update(void* p) {
  State* s = (State*)p;
  if (!s) return false;
  if (appShown()) {
    hold(s, false);
    if (s->side) freeBuffers(s);
    return false;
  }
  if (!s->side && s->w && millis() - s->buildTry >= 2000) {  // back from an app
    s->buildTry = millis();
    layout(s);
  }
  const bool seen = s->side && widgetOnScreen(s);
  hold(s, seen);
  if (!seen) return false;
  const uint32_t now = millis();
  if (now - s->frameAt < FRAME_MS) return false;
  s->frameAt = now;
  // New data: a plane keeps its glow by callsign, not by its place in the list
  char prevCall[flights::MAX][10];
  uint32_t prevLit[flights::MAX];
  const int prevN = s->snap->count;
  for (int i = 0; i < prevN; i++) {
    strlcpy(prevCall[i], s->snap->planes[i].call, 10);
    prevLit[i] = s->litAt[i];
  }
  flights::snapshot(*s->snap, companion::settings().radarGnd);
  if (s->snap->seq != s->seq || s->km != companion::settings().radarKm) {
    for (int i = 0; i < s->snap->count; i++) {
      s->litAt[i] = 0;
      for (int k = 0; k < prevN; k++)
        if (!strcmp(prevCall[k], s->snap->planes[i].call)) {
          s->litAt[i] = prevLit[k];
          break;
        }
    }
    s->seq = s->snap->seq;
    s->km = companion::settings().radarKm;
    noteText(s);
  }
  drawFrame(s, now);
  s_sparkPaintMs = now;  // like a chart glide: other pages snapshot rarely meanwhile
  return false;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  layout(s);
}

void release(void* p) {
  State* s = (State*)p;
  if (!s) return;
  hold(s, false);
  freeBuffers(s);
}

void destroy(void* p) {
  State* s = (State*)p;
  if (!s) return;
  hold(s, false);
  heap_caps_free(s->base);
  heap_caps_free(s->px);
  heap_caps_free(s->lut);
  heap_caps_free(s->snap);
  delete s;  // the canvas goes with the tile
}

const Variant kVariants[] = {{"disco", "Disc"}};

}  // namespace

// A tap opens the Radar app (the config parser gives the tile that action).
extern const Def kRadarWidget = {"radar", "Flight radar", "radar", kVariants, 1,
                                 create, update, resize, destroy, nullptr, false, release};

}  // namespace widget
