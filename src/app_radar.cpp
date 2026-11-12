// Radar app: air traffic around the companion's location (flight_service).
//
// Phosphor-green disc with a ~4 s sweep and a 70° trail; each plane lights up
// when the sweep passes it and fades over one turn. North up, centre = us.
// Side panel with the nearest planes and the selected one's details (tap a
// plane on the disc or a row). Footer: radius (tap to change) and a legend.
//
// The disc is pixel work on an ARGB8888 canvas, ported from the companion
// firmware's RadarScreen: the parts that never move (disc, glow, rings) are
// drawn once into a base picture, and every frame copies it and adds the
// sweep and the planes. The angle of every disc pixel is precomputed (LUT).
// Everything else is LVGL widgets in the deck's style.
#include "app_radar.h"
#include "companion_settings.h"
#include "flight_service.h"
#include "deck_ui_int.h"
#include <math.h>

namespace app {
namespace {

using namespace deckui;
using flights::Plane;
using flights::Snapshot;

// --- Geometry (the app body is 480 x 272) ---------------------------------------------
constexpr int R = 116;                       // disc radius
constexpr int GLOW = 4;
constexpr int LUT_R = R + GLOW, LUT_S = 2 * LUT_R + 1;  // canvas side
constexpr int DISC_X = 12, DISC_Y = 4;       // canvas position in the body
constexpr int PX = DISC_X + LUT_S + 14;      // side panel
constexpr int PW = 480 - PX - 8;
constexpr int ROWS = 5, ROW_H = 26, LIST_Y = 44;
constexpr int DET_Y = LIST_Y + ROWS * ROW_H + 10;
constexpr uint32_t FRAME_MS = 40;            // ~25 fps
constexpr uint32_t SWEEP_MS = 4000;          // one turn
constexpr int ANG = 1024;                    // angle units per turn
constexpr int TRAIL = ANG * 70 / 360;        // 70° trail
constexpr uint16_t OUT = 0xFFFF;             // outside the disc in the LUT

// Colours (RGB888)
constexpr uint32_t GREEN = 0x34F5A0, LABEL = 0x9DF7CF, FOOT = 0x7FA89A, DISC = 0x021410;
constexpr uint32_t KIND_COL[3] = {0x7DFFC8, 0xFF5A5A, 0x58A6FF};  // commercial, military, cargo
const char* const KIND_NAME[3] = {"COMERCIAL", "MILITAR", "CARGA"};
const char* const KIND_SHORT[3] = {"COM", "MIL", "CAR"};
const char* const FILTER_NAME[3] = {"EN VUELO", "EN TIERRA", "TODOS"};

struct RowUi {
  lv_obj_t *box, *bar, *call, *alt, *dist;
};

struct State {
  uint32_t *base = nullptr, *px = nullptr;  // ARGB8888, LUT_S x LUT_S
  uint16_t* lut = nullptr;
  lv_obj_t *canvas = nullptr, *msg = nullptr, *selLabel = nullptr;
  lv_obj_t* ring[2] = {};
  lv_obj_t *radius = nullptr, *chip = nullptr, *chipText = nullptr, *count = nullptr;
  RowUi rows[ROWS] = {};
  lv_obj_t *detCall = nullptr, *detKind = nullptr, *detL1 = nullptr, *detL2 = nullptr, *detTrack = nullptr,
           *detHint = nullptr;
  Snapshot snap;
  flights::State lastState = flights::State::Idle;
  uint32_t seq = 0xFFFFFFFF, frameAt = 0;
  uint32_t litAt[flights::MAX] = {};
  int prevSweep = 0;
  char sel[10] = "";
};
State* s = nullptr;

int textW(const char* t, const lv_font_t* f) {
  lv_point_t sz;
  lv_text_get_size(&sz, t, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return sz.x;
}

// --- Pixels ------------------------------------------------------------------------
inline uint32_t argb(uint32_t rgb, uint8_t a) { return (uint32_t)a << 24 | (rgb & 0xFFFFFF); }

// Paint `rgb` over the pixel at `alpha`; the pixel's own alpha only grows.
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

inline void blendAt(uint32_t* buf, int x, int y, uint32_t rgb, uint8_t alpha) {
  if (x < 0 || y < 0 || x >= LUT_S || y >= LUT_S) return;
  blend(buf[y * LUT_S + x], rgb, alpha);
}

void fillRect(uint32_t* buf, int x, int y, int w, int h, uint32_t rgb, uint8_t alpha) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) blendAt(buf, i, j, rgb, alpha);
}

void line(uint32_t* buf, int x0, int y0, int x1, int y1, uint32_t rgb, uint8_t a) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    blendAt(buf, x0, y0, rgb, a);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void circle(uint32_t* buf, int r, uint32_t rgb, uint8_t a) {
  // Midpoint circle around the disc centre
  int x = r, y = 0, e = 1 - r;
  while (x >= y) {
    const int pts[8][2] = {{x, y}, {y, x}, {-y, x}, {-x, y}, {-x, -y}, {-y, -x}, {y, -x}, {x, -y}};
    for (auto& p : pts) blendAt(buf, LUT_R + p[0], LUT_R + p[1], rgb, a);
    y++;
    if (e < 0) e += 2 * y + 1;
    else {
      x--;
      e += 2 * (y - x) + 1;
    }
  }
}

void buildLut() {
  for (int j = 0; j < LUT_S; j++)
    for (int i = 0; i < LUT_S; i++) {
      const int dx = i - LUT_R, dy = j - LUT_R;
      uint16_t v = OUT;
      if (dx * dx + dy * dy <= R * R) {
        float a = atan2f((float)dx, (float)-dy);  // 0 = north, clockwise
        if (a < 0) a += 2 * PI;
        v = (uint16_t)(a / (2 * PI) * ANG) % ANG;
      }
      s->lut[j * LUT_S + i] = v;
    }
}

// The parts that never move: disc, edge glow, rings, cross, centre.
void buildBase() {
  uint32_t* b = s->base;
  for (int j = 0; j < LUT_S; j++)
    for (int i = 0; i < LUT_S; i++) {
      const int dx = i - LUT_R, dy = j - LUT_R, d2 = dx * dx + dy * dy;
      uint32_t& p = b[j * LUT_S + i];
      p = d2 <= R * R ? argb(DISC, 235) : 0;
      const float e = fabsf(sqrtf((float)d2) - R);  // glow both sides of the edge
      if (e < GLOW) blend(p, GREEN, (uint8_t)(e < 1 ? 230 : 120 / (e * e)));
    }
  circle(b, R / 3, GREEN, 70);
  circle(b, 2 * R / 3, GREEN, 70);
  line(b, LUT_R - R + 2, LUT_R, LUT_R + R - 2, LUT_R, GREEN, 40);
  line(b, LUT_R, LUT_R - R + 2, LUT_R, LUT_R + R - 2, GREEN, 40);
  fillRect(b, LUT_R - 1, LUT_R - 1, 3, 3, GREEN, 255);
}

int sweepAngle(uint32_t now) { return (int)((now % SWEEP_MS) * ANG / SWEEP_MS); }

int angleOf(int x, int y) {
  float a = atan2f((float)(x - LUT_R), (float)(LUT_R - y));
  if (a < 0) a += 2 * PI;
  return (int)(a / (2 * PI) * ANG) % ANG;
}

// Canvas position, extrapolated with speed and heading since the last reading.
void planeXY(const Plane& p, uint32_t now, int& x, int& y) {
  const float b = p.bearing * DEG_TO_RAD;
  float ex = p.distKm * sinf(b), ny = p.distKm * cosf(b);
  if (p.track >= 0 && p.gsKt > 0 && s->snap.at) {
    const float t = std::min(30.0f, (now - s->snap.at) / 1000.0f);
    const float km = p.gsKt * 1.852f / 3600.0f * t;
    ex += km * sinf(p.track * DEG_TO_RAD);
    ny += km * cosf(p.track * DEG_TO_RAD);
  }
  const float k = (R - 6) / (float)companion::settings().radarKm;
  x = LUT_R + (int)lroundf(ex * k);
  y = LUT_R - (int)lroundf(ny * k);
}

void drawSweep(uint32_t now) {
  const int sw = sweepAngle(now);
  for (int j = 0; j < LUT_S; j++) {
    const uint16_t* row = s->lut + j * LUT_S;
    uint32_t* out = s->px + j * LUT_S;
    for (int i = 0; i < LUT_S; i++) {
      const uint16_t a = row[i];
      if (a == OUT) continue;
      const int diff = (sw - a) & (ANG - 1);  // how long ago the sweep passed
      if (diff >= TRAIL) continue;
      blend(out[i], GREEN, diff < 3 ? 210 : 120 * (TRAIL - diff) / TRAIL);
    }
  }
}

void drawPlanes(uint32_t now) {
  const int sw = sweepAngle(now);
  const int span = (sw - s->prevSweep) & (ANG - 1);  // swept since the last frame
  bool selShown = false;
  for (int i = 0; i < s->snap.count; i++) {
    const Plane& p = s->snap.planes[i];
    int x, y;
    planeXY(p, now, x, y);
    const int dx = x - LUT_R, dy = y - LUT_R;
    if (dx * dx + dy * dy > (R - 2) * (R - 2)) continue;  // left the radius

    // Phosphor: lights when the sweep passes, fades over a turn
    if (((angleOf(x, y) - s->prevSweep) & (ANG - 1)) <= span) s->litAt[i] = now;
    const bool sel = !strcmp(p.call, s->sel);
    float b = s->litAt[i] ? 1.0f - (now - s->litAt[i]) / (float)(SWEEP_MS - 300) : 0.0f;
    if (b < 0.18f) b = 0.18f;
    if (sel && b < 0.75f) b = 0.75f;
    const uint8_t al = (uint8_t)(255 * b);
    const uint32_t col = KIND_COL[p.kind % 3];
    if (p.ground) {  // on the ground: small square, no heading
      fillRect(s->px, x - 4, y - 4, 9, 9, col, (uint8_t)(55 * b));
      fillRect(s->px, x - 2, y - 2, 5, 5, col, al);
    } else {
      fillRect(s->px, x - 5, y - 5, 11, 11, col, (uint8_t)(50 * b));  // halo
      fillRect(s->px, x - 2, y - 2, 5, 5, col, al);
      if (p.track >= 0) {
        const float t = p.track * DEG_TO_RAD;
        line(s->px, x, y, x + (int)lroundf(10 * sinf(t)), y - (int)lroundf(10 * cosf(t)), col, al);
      }
    }
    if (!sel) continue;
    // Selected: corner marks and its name next to it
    for (int k = 0; k < 4; k++) {
      const int cx = x + ((k & 1) ? 8 : -8), cy = y + ((k & 2) ? 8 : -8);
      line(s->px, (k & 1) ? cx - 3 : cx, cy, (k & 1) ? cx : cx + 3, cy, GREEN, 255);
      line(s->px, cx, (k & 2) ? cy - 3 : cy, cx, (k & 2) ? cy : cy + 3, GREEN, 255);
    }
    lv_label_set_text(s->selLabel, p.call);
    const int lw = textW(p.call, &deck_font_14);
    int lx = x + 12;
    if (lx + lw > LUT_S) lx = x - 12 - lw;
    lv_obj_set_pos(s->selLabel, DISC_X + lx, DISC_Y + y - 18);
    selShown = true;
  }
  if (selShown) lv_obj_remove_flag(s->selLabel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s->selLabel, LV_OBJ_FLAG_HIDDEN);
  s->prevSweep = sw;
}

void drawFrame(uint32_t now) {
  memcpy(s->px, s->base, LUT_S * LUT_S * 4);
  drawSweep(now);
  drawPlanes(now);
  lv_image_cache_drop(lv_image_get_src(s->canvas));
  lv_obj_invalidate(s->canvas);
}

// --- Widgets ---------------------------------------------------------------------
lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* t = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, t);
  return l;
}

void fmtAlt(int32_t ft, char* out, size_t n) {
  if (ft == -2) snprintf(out, n, "--");
  else if (ft < 0) snprintf(out, n, "tierra");
  else if (ft >= 10000) snprintf(out, n, "FL%03ld", (long)(ft / 100));
  else snprintf(out, n, "%ld ft", (long)ft);
}

void radiusText() {
  lv_label_set_text_fmt(s->radius, "RADIO %u KM", (unsigned)companion::settings().radarKm);
  const int km = companion::settings().radarKm;
  // LVGL's own printf has no floats: format with snprintf
  char b[12];
  for (int k = 0; k < 2; k++) {
    const float v = km * (k + 1) / 3.0f;
    if (v < 10) snprintf(b, sizeof(b), "%.1f", v);
    else snprintf(b, sizeof(b), "%d", (int)lroundf(v));
    lv_label_set_text(s->ring[k], b);
  }
}

void toggleSel(const char* call) {
  if (!strcmp(s->sel, call)) s->sel[0] = 0;
  else strlcpy(s->sel, call, sizeof(s->sel));
}

void refreshPanel() {
  const Snapshot& sn = s->snap;
  lv_label_set_text_fmt(s->count, "%d", sn.count);
  lv_label_set_text(s->chipText, FILTER_NAME[companion::settings().radarGnd % 3]);
  for (int i = 0; i < ROWS; i++) {
    RowUi& r = s->rows[i];
    if (i >= sn.count) {
      lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_remove_flag(r.box, LV_OBJ_FLAG_HIDDEN);
    const Plane& p = sn.planes[i];
    const bool sel = !strcmp(p.call, s->sel);
    char alt[12], d[12];
    fmtAlt(p.altFt, alt, sizeof(alt));
    snprintf(d, sizeof(d), "%.1f km", p.distKm);
    lv_obj_set_style_bg_opa(r.box, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(r.bar, lv_color_hex(KIND_COL[p.kind % 3]), 0);
    const lv_color_t c = lv_color_hex(sel ? C_TEXT_ON : C_TEXT), m = lv_color_hex(sel ? C_TEXT_ON : C_DIM);
    lv_label_set_text(r.call, p.call);
    lv_obj_set_style_text_color(r.call, c, 0);
    lv_label_set_text(r.alt, alt);
    lv_obj_set_style_text_color(r.alt, m, 0);
    lv_label_set_text(r.dist, d);
    lv_obj_set_style_text_color(r.dist, c, 0);
  }

  const Plane* sp = nullptr;
  for (int i = 0; i < sn.count; i++)
    if (s->sel[0] && !strcmp(sn.planes[i].call, s->sel)) sp = &sn.planes[i];
  lv_obj_t* det[] = {s->detCall, s->detKind, s->detL1, s->detL2, s->detTrack};
  for (lv_obj_t* o : det) {
    if (sp) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
  if (!sp) {
    lv_obj_remove_flag(s->detHint, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(s->detHint, LV_OBJ_FLAG_HIDDEN);
  char alt[12];
  fmtAlt(sp->altFt, alt, sizeof(alt));
  lv_label_set_text(s->detCall, sp->call);
  lv_label_set_text(s->detKind, KIND_NAME[sp->kind % 3]);
  lv_obj_set_style_text_color(s->detKind, lv_color_hex(KIND_COL[sp->kind % 3]), 0);
  if (sp->type[0]) lv_label_set_text_fmt(s->detL1, "%s · %s · %d kt", sp->type, alt, sp->gsKt);
  else lv_label_set_text_fmt(s->detL1, "%s · %d kt", alt, sp->gsKt);
  char l2[32];
  snprintf(l2, sizeof(l2), "%.1f km · %03d°", sp->distKm, (int)sp->bearing);
  lv_label_set_text(s->detL2, l2);
  if (sp->track >= 0) lv_label_set_text_fmt(s->detTrack, "rumbo %03d°", sp->track);
  else lv_label_set_text(s->detTrack, "");
}

void refreshMessage() {
  const char* msg = nullptr;
  char buf[32];
  const Snapshot& sn = s->snap;
  switch (sn.state) {
    case flights::State::NoNet: msg = "SIN RED"; break;
    case flights::State::Idle:
    case flights::State::Loading:
      if (!sn.at) msg = "CARGANDO...";
      break;
    case flights::State::Error:
      if (!sn.at) {
        snprintf(buf, sizeof(buf), "ERROR API %d", sn.httpCode);
        msg = buf;
      }
      break;
    case flights::State::Ok:
      if (!sn.count) {
        static const char* const NONE[3] = {"SIN VUELOS", "NADIE EN TIERRA", "SIN TRÁFICO"};
        msg = NONE[companion::settings().radarGnd % 3];
      }
      break;
  }
  if (msg) {
    lv_label_set_text(s->msg, msg);
    lv_obj_remove_flag(s->msg, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s->msg, LV_OBJ_FLAG_HIDDEN);
  }
}

// --- Events ----------------------------------------------------------------------
void onDisc(lv_event_t* e) {
  lv_point_t pt;
  lv_indev_get_point(lv_indev_active(), &pt);
  lv_area_t a;
  lv_obj_get_coords(s->canvas, &a);
  const int x = pt.x - a.x1, y = pt.y - a.y1;
  int best = -1, bestD = 24 * 24;
  for (int i = 0; i < s->snap.count; i++) {
    int px, py;
    planeXY(s->snap.planes[i], millis(), px, py);
    const int d = (px - x) * (px - x) + (py - y) * (py - y);
    if (d < bestD) {
      bestD = d;
      best = i;
    }
  }
  if (best >= 0) toggleSel(s->snap.planes[best].call);
  else s->sel[0] = 0;
  refreshPanel();
}

void onRow(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < s->snap.count) toggleSel(s->snap.planes[i].call);
  refreshPanel();
}

void onFilter(lv_event_t*) {
  companion::Settings& cs = companion::settings();
  cs.radarGnd = (cs.radarGnd + 1) % 3;  // airborne -> ground -> both
  companion::save();
  s->seq = 0xFFFFFFFF;  // the snapshot is filtered again on the next tick
}

void onRadius(lv_event_t*) {
  companion::Settings& cs = companion::settings();
  int i = 0;
  const int n = sizeof(companion::RADAR_KMS);
  while (i < n && companion::RADAR_KMS[i] != cs.radarKm) i++;
  cs.radarKm = companion::RADAR_KMS[(i + 1) % n];
  companion::save();
  for (auto& t : s->litAt) t = 0;
  flights::refreshSoon();
  radiusText();
}

// --- App -------------------------------------------------------------------------
void close() {
  flights::setActive(false);
  if (!s) return;
  heap_caps_free(s->base);
  heap_caps_free(s->px);
  heap_caps_free(s->lut);
  delete s;  // the widgets go with the app's view
  s = nullptr;
}

void open(lv_obj_t* body, int w, int h) {
  close();
  s = new (std::nothrow) State();
  if (s) {
    s->base = (uint32_t*)heap_caps_aligned_alloc(64, LUT_S * LUT_S * 4, MALLOC_CAP_SPIRAM);
    s->px = (uint32_t*)heap_caps_aligned_alloc(64, LUT_S * LUT_S * 4, MALLOC_CAP_SPIRAM);
    s->lut = (uint16_t*)heap_caps_malloc(LUT_S * LUT_S * 2, MALLOC_CAP_SPIRAM);
  }
  if (!s || !s->base || !s->px || !s->lut) {
    close();
    lv_obj_center(text(body, &deck_font_20, C_DIM, "Sin memoria para el radar"));
    return;
  }
  buildLut();
  buildBase();
  memcpy(s->px, s->base, LUT_S * LUT_S * 4);

  // Disc
  s->canvas = lv_canvas_create(body);
  lv_canvas_set_buffer(s->canvas, s->px, LUT_S, LUT_S, LV_COLOR_FORMAT_ARGB8888);
  lv_obj_set_pos(s->canvas, DISC_X, DISC_Y);
  lv_obj_add_flag(s->canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s->canvas, onDisc, LV_EVENT_CLICKED, nullptr);
  const lv_color_t dimGreen = lv_color_hex(0x3E8A6A);
  lv_obj_t* n = text(body, &deck_font_14, 0, "N");
  lv_obj_set_style_text_color(n, dimGreen, 0);
  lv_obj_set_pos(n, DISC_X + LUT_R - 5, DISC_Y + LUT_R - R + 4);
  for (int k = 0; k < 2; k++) {
    s->ring[k] = text(body, &deck_font_12, 0);
    lv_obj_set_style_text_color(s->ring[k], dimGreen, 0);
    lv_obj_set_pos(s->ring[k], DISC_X + LUT_R + 4, DISC_Y + LUT_R - R * (k + 1) / 3 + 2);
  }
  s->msg = text(body, &deck_font_14, LABEL);
  lv_obj_set_width(s->msg, LUT_S);
  lv_obj_set_style_text_align(s->msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s->msg, DISC_X, DISC_Y + LUT_R + 26);
  s->selLabel = text(body, &deck_font_14, GREEN);
  lv_obj_add_flag(s->selLabel, LV_OBJ_FLAG_HIDDEN);

  // Footer: radius (tap to change) and the legend
  s->radius = text(body, &deck_font_12, FOOT);
  lv_obj_set_pos(s->radius, DISC_X, h - 22);
  lv_obj_add_flag(s->radius, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(s->radius, 10);
  lv_obj_add_event_cb(s->radius, onRadius, LV_EVENT_CLICKED, nullptr);
  int lx = DISC_X + LUT_S;
  for (int k = 2; k >= 0; k--) {
    lv_obj_t* l = text(body, &deck_font_12, FOOT, KIND_SHORT[k]);
    lx -= textW(KIND_SHORT[k], &deck_font_12);
    lv_obj_set_pos(l, lx, h - 22);
    lx -= 10;
    lv_obj_t* sq = lv_obj_create(body);
    lv_obj_remove_style_all(sq);
    lv_obj_set_size(sq, 7, 7);
    lv_obj_set_style_bg_color(sq, lv_color_hex(KIND_COL[k]), 0);
    lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
    lv_obj_set_pos(sq, lx, h - 17);
    lx -= 14;
  }

  // Side panel
  lv_obj_t* panel = lv_obj_create(body);
  lv_obj_remove_style_all(panel);
  lv_obj_set_pos(panel, PX, 4);
  lv_obj_set_size(panel, PW, h - 8);
  lv_obj_set_style_bg_color(panel, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(panel, s_theme == THEME_GLASS ? LV_OPA_50 : LV_OPA_COVER, 0);
  lv_obj_set_style_radius(panel, 16, 0);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* hdr = text(panel, &deck_font_14, C_DIM, "CERCANOS");
  lv_obj_set_pos(hdr, 12, 14);
  s->count = text(panel, &deck_font_14, GREEN);
  lv_obj_set_pos(s->count, 12 + textW("CERCANOS", &deck_font_14) + 8, 14);
  s->chip = qpButton(panel, PW - 8 - 78, 6, 78, "", false);
  lv_obj_set_height(s->chip, 30);
  s->chipText = lv_obj_get_child(s->chip, 0);
  lv_obj_set_style_text_font(s->chipText, &deck_font_12, 0);
  lv_obj_add_event_cb(s->chip, onFilter, LV_EVENT_CLICKED, nullptr);

  for (int i = 0; i < ROWS; i++) {
    RowUi& r = s->rows[i];
    r.box = lv_obj_create(panel);
    lv_obj_remove_style_all(r.box);
    lv_obj_set_pos(r.box, 6, LIST_Y + i * ROW_H);
    lv_obj_set_size(r.box, PW - 12, ROW_H - 2);
    lv_obj_set_style_radius(r.box, 8, 0);
    lv_obj_set_style_bg_color(r.box, lv_color_hex(GREEN), 0);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(r.box, onRow, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    r.bar = lv_obj_create(r.box);
    lv_obj_remove_style_all(r.bar);
    lv_obj_set_size(r.bar, 3, ROW_H - 12);
    lv_obj_set_pos(r.bar, 4, 5);
    lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, 0);
    r.call = text(r.box, &deck_font_14, C_TEXT);
    lv_obj_set_pos(r.call, 12, 4);
    r.alt = text(r.box, &deck_font_12, C_DIM);
    lv_obj_set_pos(r.alt, 80, 6);
    r.dist = text(r.box, &deck_font_14, C_TEXT);
    lv_obj_align(r.dist, LV_ALIGN_TOP_RIGHT, -6, 4);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
  }

  lv_obj_t* sep = lv_obj_create(panel);
  lv_obj_remove_style_all(sep);
  lv_obj_set_size(sep, PW - 24, 1);
  lv_obj_set_pos(sep, 12, DET_Y - 6);
  lv_obj_set_style_bg_color(sep, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_40, 0);
  s->detHint = text(panel, &deck_font_14, C_DIM, "Toca un avión");
  lv_obj_set_pos(s->detHint, 12, DET_Y + 8);
  s->detCall = text(panel, &deck_font_20, GREEN);
  lv_obj_set_pos(s->detCall, 12, DET_Y);
  s->detKind = text(panel, &deck_font_12, C_DIM);
  lv_obj_align(s->detKind, LV_ALIGN_TOP_RIGHT, -12, DET_Y + 6);
  s->detL1 = text(panel, &deck_font_14, C_TEXT);
  lv_obj_set_pos(s->detL1, 12, DET_Y + 26);
  s->detL2 = text(panel, &deck_font_14, C_DIM);
  lv_obj_set_pos(s->detL2, 12, DET_Y + 46);
  s->detTrack = text(panel, &deck_font_14, C_DIM);
  lv_obj_align(s->detTrack, LV_ALIGN_TOP_RIGHT, -12, DET_Y + 46);

  radiusText();
  refreshPanel();
  refreshMessage();
  flights::setActive(true);
  (void)w;
}

void tick() {
  if (!s || !s->canvas) return;
  const uint32_t now = millis();
  if (now - s->frameAt < FRAME_MS) return;
  s->frameAt = now;

  // New data: brightness follows the callsign, not the position in the list
  char prevCall[flights::MAX][10];
  uint32_t prevLit[flights::MAX];
  const int prevN = s->snap.count;
  for (int i = 0; i < prevN; i++) {
    strlcpy(prevCall[i], s->snap.planes[i].call, 10);
    prevLit[i] = s->litAt[i];
  }
  flights::snapshot(s->snap, companion::settings().radarGnd);
  if (s->snap.seq != s->seq) {
    for (int i = 0; i < s->snap.count; i++) {
      s->litAt[i] = 0;
      for (int k = 0; k < prevN; k++)
        if (!strcmp(prevCall[k], s->snap.planes[i].call)) {
          s->litAt[i] = prevLit[k];
          break;
        }
    }
    s->seq = s->snap.seq;
    if (s->snap.state == flights::State::Error && s->lastState != flights::State::Error)
      deckToastShow(s->snap.httpCode >= 400 && s->snap.httpCode < 500 ? "Radar: la API rechaza la petición"
                                                                       : "Radar: la API no responde",
                    2500);
    s->lastState = s->snap.state;
    refreshPanel();
    refreshMessage();
  }
  drawFrame(now);
}

}  // namespace

const Def kRadar = {"radar", "Radar", "mdi:radar", true, open, tick, close};

}  // namespace app
