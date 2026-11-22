// Bench app: see app_bench.h. A text sheet is one step per line; "# Title"
// lines are section headings, and list markers ("- ", "1. ", "[ ] ") are
// dropped. Colour words (red, rojo...) get a swatch, "->" becomes an arrow.
// The sheet lives on the SD card (or LittleFS) as sheet.txt or sheet.jpg plus
// meta.json, and the current step in "step", so a reboot keeps it.
#include "apps/app_bench.h"
#include "ui/deck_ui_int.h"
#include "ui/deck_config.h"
#include "ui/display_ui.h"
#include "services/deck_wall.h"
#include "hardware/deck_sd.h"
#include "hardware/haptic.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <src/libs/tjpgd/tjpgd.h>
#include <src/misc/cache/instance/lv_image_cache.h>

namespace app {
namespace {

using namespace deckui;

constexpr int MAX_STEPS = 200;
constexpr int MAX_CHIPS = 3;
constexpr uint32_t STEP_SAVE_MS = 1500;  // the step is written this long after the last tap
#define ARROW LV_SYMBOL_RIGHT

// --- The sheet ------------------------------------------------------------------
struct Sheet {
  bool loaded;
  char kind;  // 't' text, 'i' image, 0 none
  char title[BENCH_MAX_TITLE + 1];
  char* text;  // PSRAM, kind 't'
  int step;    // text: the current step; == the step count = all done
  int imgW, imgH;
  uint32_t seq;  // bumps with every new sheet
};
Sheet s_sheet = {};
bool s_pendingOpen = false;
uint32_t s_stepDirtyAt = 0;

String dir() { return deck::wallOnSd() ? String(SD_ROOT "/bench") : String("/bench"); }
String file(const char* name) { return dir() + "/" + name; }
fs::FS& fsys() { return deck::wallFs(); }

void ensureDir() {
  if (deck::wallOnSd()) fsys().mkdir(SD_ROOT);
  fsys().mkdir(dir());
}

bool writeFile(const String& path, const char* data, size_t len) {
  File f = fsys().open(path, "w");
  if (!f) return false;
  const bool ok = f.write((const uint8_t*)data, len) == len;
  f.close();
  return ok;
}

void writeMeta() {
  JsonDocument doc;
  doc["kind"] = s_sheet.kind == 'i' ? "image" : "text";
  doc["title"] = s_sheet.title;
  String out;
  serializeJson(doc, out);
  writeFile(file("meta.json"), out.c_str(), out.length());
}

void writeStep() {
  char buf[8];
  const int n = snprintf(buf, sizeof(buf), "%d", s_sheet.step);
  writeFile(file("step"), buf, n);
  s_stepDirtyAt = 0;
}

void setSheetText(const char* text, size_t len) {
  free(s_sheet.text);
  s_sheet.text = nullptr;
  if (!text) return;
  s_sheet.text = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
  if (!s_sheet.text) return;
  memcpy(s_sheet.text, text, len);
  s_sheet.text[len] = 0;
}

// --- JPEG ------------------------------------------------------------------------
struct JpegIn {
  File f;
  uint16_t* px;
  int w;
};

size_t jpegRead(JDEC* jd, uint8_t* buf, size_t n) {
  JpegIn& in = *(JpegIn*)jd->device;
  if (buf) return in.f.read(buf, n);
  const size_t pos = in.f.position();
  const size_t count = std::min(n, (size_t)in.f.size() - pos);
  return in.f.seek(pos + count) ? count : 0;
}

int jpegWrite(JDEC* jd, void* bmp, JRECT* r) {
  JpegIn& in = *(JpegIn*)jd->device;
  const uint8_t* src = (const uint8_t*)bmp;
  for (int y = r->top; y <= r->bottom; y++)
    for (int x = r->left; x <= r->right; x++, src += 3)  // LVGL's tjpgd gives B, G, R
      in.px[y * in.w + x] = deck::wallDither565(src[2], src[1], src[0], x, y);
  return 1;
}

// Reads the header (and decodes into px when given, w * h RGB565). err says why not.
bool jpegOpen(const String& path, int& w, int& h, uint16_t* px, String& err) {
  JpegIn in;
  in.f = fsys().open(path, "r");
  if (!in.f) {
    err = "the picture is missing";
    return false;
  }
  if (in.f.size() > BENCH_MAX_JPEG) {
    err = "the JPEG is larger than 400 KB";
    return false;
  }
  if (in.f.read() != 0xFF || in.f.read() != 0xD8 || !in.f.seek(0)) {
    err = "not a JPEG";
    return false;
  }
  if (in.f.size() > 8192) in.f.setBufferSize(8192);  // PSRAM stdio buffer
  constexpr size_t workSize = 4096;
  void* work = heap_caps_malloc(workSize, MALLOC_CAP_SPIRAM);
  if (!work) {
    err = "out of memory";
    return false;
  }
  JDEC jd;
  const JRESULT r = jd_prepare(&jd, jpegRead, work, workSize, &in);
  bool ok = r == JDR_OK;
  if (!ok) err = r == JDR_FMT3 ? "progressive JPEG: send a baseline one" : "cannot read the JPEG";
  if (ok) {
    w = jd.width;
    h = jd.height;
    if (w > BENCH_MAX_SIDE || h > BENCH_MAX_SIDE || (uint32_t)w * h > BENCH_MAX_PIXELS) {
      err = "the picture is too large (at most 800 x 600 pixels)";
      ok = false;
    }
  }
  if (ok && px) {
    in.px = px;
    in.w = w;
    ok = jd_decomp(&jd, jpegWrite, 0) == JDR_OK;
    if (!ok) err = "the JPEG did not decode";
  }
  free(work);
  return ok;
}

void ensureLoaded() {
  if (s_sheet.loaded) return;
  s_sheet.loaded = true;
  if (!benchStorageReady()) return;
  File f = fsys().open(file("meta.json"), "r");
  if (!f) return;
  JsonDocument doc;
  const bool bad = deserializeJson(doc, f) != DeserializationError::Ok;
  f.close();
  if (bad) return;
  strlcpy(s_sheet.title, doc["title"] | "", sizeof(s_sheet.title));
  const char* kind = doc["kind"] | "";
  if (!strcmp(kind, "text")) {
    File t = fsys().open(file("sheet.txt"), "r");
    if (!t) return;
    const size_t len = std::min((size_t)t.size(), BENCH_MAX_TEXT);
    char* buf = (char*)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    const size_t got = t.read((uint8_t*)buf, len);
    t.close();
    setSheetText(buf, got);
    free(buf);
    if (!s_sheet.text) return;
    s_sheet.kind = 't';
    File st = fsys().open(file("step"), "r");
    if (st) {
      s_sheet.step = st.readString().toInt();
      st.close();
    }
  } else if (!strcmp(kind, "image")) {
    String err;
    if (jpegOpen(file("sheet.jpg"), s_sheet.imgW, s_sheet.imgH, nullptr, err)) s_sheet.kind = 'i';
  }
}

// --- Text: steps ------------------------------------------------------------------
struct Step {
  const char* text;  // cleaned, in s_clean
  int heading;       // index into s_heads, -1 = none
};
Step s_steps[MAX_STEPS];
const char* s_heads[MAX_STEPS];
int s_stepCount = 0, s_headCount = 0;
char* s_clean = nullptr;  // PSRAM: the cleaned lines, NUL-separated

bool startsWith(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }

// Drops a list marker: "- ", "* ", "+ ", "• ", "[ ] ", "[x] ", "12. ", "12) ".
const char* skipMarker(const char* s) {
  for (const char* m : {"- ", "* ", "+ ", "\xE2\x80\xA2 "})
    if (startsWith(s, m)) {
      s += strlen(m);
      break;
    }
  for (const char* m : {"[ ] ", "[x] ", "[X] "})
    if (startsWith(s, m)) {
      s += strlen(m);
      break;
    }
  const char* d = s;
  while (*d >= '0' && *d <= '9') d++;
  if (d > s && d - s <= 3 && (*d == '.' || *d == ')') && d[1] == ' ') s = d + 2;
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

// Copies [s, e) into out with "->", "=>" and "→" as the font's arrow; returns the new end.
char* copyLine(char* out, const char* s, const char* e) {
  while (s < e) {
    if (e - s >= 2 && (startsWith(s, "->") || startsWith(s, "=>"))) {
      memcpy(out, ARROW, 3);
      out += 3;
      s += 2;
    } else if (e - s >= 3 && startsWith(s, "\xE2\x86\x92")) {
      memcpy(out, ARROW, 3);
      out += 3;
      s += 3;
    } else {
      *out++ = *s++;
    }
  }
  *out++ = 0;
  return out;
}

void parseSteps() {
  free(s_clean);
  s_clean = nullptr;
  s_stepCount = s_headCount = 0;
  if (!s_sheet.text) return;
  const size_t len = strlen(s_sheet.text);
  s_clean = (char*)heap_caps_malloc(len * 2 + 16, MALLOC_CAP_SPIRAM);
  if (!s_clean) return;
  char* out = s_clean;
  int heading = -1;
  const char* p = s_sheet.text;
  while (*p && s_stepCount < MAX_STEPS) {
    const char* nl = strchr(p, '\n');
    const char* end = nl ? nl : p + strlen(p);
    const char* next = nl ? nl + 1 : end;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    while (end > p && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    if (p < end && *p == '#') {
      while (p < end && (*p == '#' || *p == ' ')) p++;
      if (p < end && s_headCount < MAX_STEPS) {
        s_heads[s_headCount] = out;
        out = copyLine(out, p, end);
        heading = s_headCount++;
      }
    } else if (p < end) {
      const char* s = skipMarker(p);
      if (s < end) {
        s_steps[s_stepCount++] = {out, heading};
        out = copyLine(out, s, end);
      }
    }
    p = next;
  }
}

// Wire colours: a swatch before the step for each colour word in it.
struct Colour {
  const char* word;
  uint32_t rgb;
};
const Colour kColours[] = {
    {"red", 0xE5484D},     {"rojo", 0xE5484D},     {"roja", 0xE5484D},    {"black", 0x111111},
    {"negro", 0x111111},   {"negra", 0x111111},    {"white", 0xF5F5F5},   {"blanco", 0xF5F5F5},
    {"blanca", 0xF5F5F5},  {"green", 0x3FB950},    {"verde", 0x3FB950},   {"blue", 0x3B82F6},
    {"azul", 0x3B82F6},    {"yellow", 0xFACC15},   {"amarillo", 0xFACC15}, {"amarilla", 0xFACC15},
    {"orange", 0xF97316},  {"naranja", 0xF97316},  {"brown", 0x8B5A2B},   {"marron", 0x8B5A2B},
    {"marr\xC3\xB3n", 0x8B5A2B}, {"purple", 0xA855F7}, {"violet", 0xA855F7}, {"violeta", 0xA855F7},
    {"morado", 0xA855F7},  {"morada", 0xA855F7},   {"lila", 0xA855F7},    {"grey", 0x9CA3AF},
    {"gray", 0x9CA3AF},    {"gris", 0x9CA3AF},     {"pink", 0xEC4899},    {"rosa", 0xEC4899},
};

bool wordChar(char c) { return isalnum((unsigned char)c) || (unsigned char)c >= 0x80; }

int findColours(const char* s, uint32_t* out) {
  int n = 0;
  for (const char* p = s; *p && n < MAX_CHIPS; p++) {
    if (p > s && wordChar(p[-1])) continue;
    for (const Colour& c : kColours) {
      const size_t l = strlen(c.word);
      if (!strncasecmp(p, c.word, l) && !wordChar(p[l])) {
        out[n++] = c.rgb;
        p += l - 1;
        break;
      }
    }
  }
  return n;
}

// --- Image ------------------------------------------------------------------------
// Only the fitted copy stays in memory (~280 KB at most); the full picture is
// decoded while zoomed (and for a moment to make the fitted copy). A picture no
// larger than the body is its own fitted copy (s_small) and zooms 2x.
lv_obj_t* s_body = nullptr;  // the open app's view
int s_w = 0, s_h = 0;
uint32_t s_shownSeq = 0;
uint16_t *s_full = nullptr, *s_fit = nullptr;
lv_image_dsc_t s_fullDsc = {}, s_fitDsc = {};
int s_fitW = 0, s_fitH = 0;
bool s_small = false, s_zoom = false;
int s_zoomX = 0, s_zoomY = 0;  // the picture's point at the centre of the zoomed view

void dsc(lv_image_dsc_t& d, uint16_t* px, int w, int h) {
  lv_image_cache_drop(&d);
  d = {};
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_RGB565;
  d.header.w = w;
  d.header.h = h;
  d.header.stride = w * 2;
  d.data_size = (uint32_t)w * h * 2;
  d.data = (const uint8_t*)px;
}

void freeBuf(uint16_t*& px, lv_image_dsc_t& d) {
  lv_image_cache_drop(&d);
  d = {};
  free(px);
  px = nullptr;
}

void freeImage() {
  freeBuf(s_full, s_fullDsc);
  freeBuf(s_fit, s_fitDsc);
}

uint16_t* decode(String& err) {
  const int iw = s_sheet.imgW, ih = s_sheet.imgH;
  uint16_t* px = iw > 0 && ih > 0 ? (uint16_t*)heap_caps_malloc((size_t)iw * ih * 2, MALLOC_CAP_SPIRAM) : nullptr;
  if (!px) {
    err = "not enough memory for this picture";
    return nullptr;
  }
  int w, h;
  if (!jpegOpen(file("sheet.jpg"), w, h, px, err) || w != iw || h != ih) {
    if (!err.length()) err = "the picture changed";
    free(px);
    return nullptr;
  }
  return px;
}

// Box filter from the full picture to fw x fh.
void shrink(const uint16_t* src, int sw, int sh, uint16_t* dst, int dw, int dh) {
  for (int dy = 0; dy < dh; dy++) {
    const int y0 = dy * sh / dh, y1 = std::max(y0 + 1, (dy + 1) * sh / dh);
    for (int dx = 0; dx < dw; dx++) {
      const int x0 = dx * sw / dw, x1 = std::max(x0 + 1, (dx + 1) * sw / dw);
      uint32_t r = 0, g = 0, b = 0;
      for (int y = y0; y < y1; y++) {
        const uint16_t* row = src + y * sw;
        for (int x = x0; x < x1; x++) {
          const uint16_t c = row[x];
          r += c >> 11;
          g += (c >> 5) & 0x3F;
          b += c & 0x1F;
        }
      }
      const uint32_t n = (uint32_t)(y1 - y0) * (x1 - x0);
      dst[dy * dw + dx] = deck::wallDither565(r * 255 / (31 * n), g * 255 / (63 * n), b * 255 / (31 * n), dx, dy);
    }
  }
}

// The fitted copy: inside the body, never larger than the picture.
bool makeFit(String& err) {
  if (s_fit) return true;
  const int iw = s_sheet.imgW, ih = s_sheet.imgH;
  const float k = std::min(1.0f, std::min((float)s_w / iw, (float)s_h / ih));
  s_fitW = std::max(1, (int)(iw * k + 0.5f));
  s_fitH = std::max(1, (int)(ih * k + 0.5f));
  s_small = s_fitW == iw && s_fitH == ih;
  if (s_small) {
    s_fit = decode(err);
  } else {
    const bool had = s_full != nullptr;  // zoomed: the full picture is there already
    if (!had && !(s_full = decode(err))) return false;
    s_fit = (uint16_t*)heap_caps_malloc((size_t)s_fitW * s_fitH * 2, MALLOC_CAP_SPIRAM);
    if (s_fit) shrink(s_full, iw, ih, s_fit, s_fitW, s_fitH);
    else err = "not enough memory for this picture";
    if (!had) freeBuf(s_full, s_fullDsc);
  }
  if (s_fit) dsc(s_fitDsc, s_fit, s_fitW, s_fitH);
  return s_fit != nullptr;
}

// Zoom: the full picture in place of the fitted copy (a small one keeps its copy).
bool zoomIn(String& err) {
  if (s_small) return s_fit != nullptr;
  if (!s_full && !(s_full = decode(err))) return false;
  dsc(s_fullDsc, s_full, s_sheet.imgW, s_sheet.imgH);
  freeBuf(s_fit, s_fitDsc);
  return true;
}

bool zoomOut(String& err) {
  const bool ok = makeFit(err);
  if (ok && !s_small) freeBuf(s_full, s_fullDsc);
  return ok;
}

// --- View -------------------------------------------------------------------------

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t* box(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  return o;
}

lv_obj_t* oneLine(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text, int w) {
  lv_obj_t* l = label(parent, font, color, text);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(l, w, lineH(font));
  return l;
}

void build();

void buildEmpty() {
  lv_obj_t* col = box(s_body);
  lv_obj_set_size(col, s_w - 48, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 10, 0);
  lv_obj_center(col);
  label(col, &deck_font_20, C_TEXT, "Nothing on the bench");
  lv_obj_t* hint = label(col, &deck_font_14, C_DIM,
                         "Send a list of steps or a picture from the PC:\npython td_bench.py <file>");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

void buildText() {
  parseSteps();
  const int pad = 16, iw = s_w - 2 * pad;
  if (!s_stepCount) {
    buildEmpty();
    return;
  }
  int& step = s_sheet.step;
  if (step < 0) step = 0;
  if (step > s_stepCount) step = s_stepCount;
  const bool done = step == s_stepCount;

  // Title and progress
  char buf[24];
  lv_obj_set_pos(oneLine(s_body, &deck_font_14, C_DIM, s_sheet.title[0] ? s_sheet.title : "Bench", iw - 80), pad, 8);
  snprintf(buf, sizeof(buf), "%d / %d", done ? s_stepCount : step + 1, s_stepCount);
  lv_obj_t* prog = label(s_body, &deck_font_14, C_DIM, buf);
  lv_obj_align(prog, LV_ALIGN_TOP_RIGHT, -pad, 8);
  lv_obj_t* track = box(s_body);
  lv_obj_set_style_bg_color(track, lv_color_hex(C_TRACK), 0);
  lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(track, 2, 0);
  lv_obj_set_size(track, iw, 4);
  lv_obj_set_pos(track, pad, 30);
  lv_obj_t* fill = box(track);
  lv_obj_set_style_bg_color(fill, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(fill, 2, 0);
  lv_obj_set_size(fill, std::max(4, iw * step / s_stepCount), 4);

  // Hints at the bottom: the two halves of the screen
  const int hintY = s_h - lineH(&deck_font_14) - 6;
  lv_obj_t* back = label(s_body, &deck_font_14, C_DIM, LV_SYMBOL_LEFT "  Back");
  lv_obj_set_pos(back, pad, hintY);
  if (!step) lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t* fwd = label(s_body, &deck_font_14, C_DIM, done ? "Start again  " LV_SYMBOL_RIGHT : "Next  " LV_SYMBOL_RIGHT);
  lv_obj_align(fwd, LV_ALIGN_BOTTOM_RIGHT, -pad, -6);

  lv_obj_t* col = box(s_body);
  const int top = 44, colH = hintY - 8 - top;
  lv_obj_set_pos(col, pad, top);
  lv_obj_set_size(col, iw, colH);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 8, 0);

  if (done) {
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(col, &deck_font_28, C_OK, LV_SYMBOL_OK);
    label(col, &deck_font_28, C_TEXT, "All done");
    snprintf(buf, sizeof(buf), "%d steps", s_stepCount);
    label(col, &deck_font_14, C_DIM, buf);
    return;
  }

  const Step& cur = s_steps[step];
  if (cur.heading >= 0) oneLine(col, &deck_font_14, C_ACCENT, s_heads[cur.heading], iw);
  if (step > 0) {
    String prev = String(LV_SYMBOL_OK "  ") + s_steps[step - 1].text;
    lv_obj_t* l = oneLine(col, &deck_font_14, C_DIM, prev.c_str(), iw);
    lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_STRIKETHROUGH, 0);
  }

  // The current step: number, swatches, text (28 px, 20 when long)
  lv_obj_t* row = box(col);
  lv_obj_set_width(row, iw);
  lv_obj_set_flex_grow(row, 1);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_t* badge = box(row);
  lv_obj_set_size(badge, 38, 38);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  snprintf(buf, sizeof(buf), "%d", step + 1);
  lv_obj_center(label(badge, &deck_font_20, C_TEXT_ON, buf));
  lv_obj_t* right = box(row);
  const int textW = iw - 38 - 12;
  lv_obj_set_size(right, textW, LV_PCT(100));
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(right, 6, 0);
  uint32_t colours[MAX_CHIPS];
  const int chips = findColours(cur.text, colours);
  if (chips) {
    lv_obj_t* sw = box(right);
    lv_obj_set_size(sw, LV_SIZE_CONTENT, 22);
    lv_obj_set_flex_flow(sw, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(sw, 8, 0);
    for (int i = 0; i < chips; i++) {
      lv_obj_t* c = box(sw);
      lv_obj_set_size(c, 40, 22);
      lv_obj_set_style_radius(c, 11, 0);
      lv_obj_set_style_bg_color(c, lv_color_hex(colours[i]), 0);
      lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(c, lv_color_hex(C_DIM), 0);
      lv_obj_set_style_border_width(c, 1, 0);
    }
  }
  // The rows under the current one take what the text leaves.
  const int nextMax = s_h > s_w ? 4 : 2;
  const int nextN = std::min(nextMax, s_stepCount - step - 1);
  int room = colH - (cur.heading >= 0 ? lineH(&deck_font_14) + 8 : 0) - (step > 0 ? lineH(&deck_font_14) + 8 : 0) -
             nextN * (lineH(&deck_font_20) + 8) - (chips ? 28 : 0);
  lv_point_t sz;
  const lv_font_t* font = &deck_font_28;
  lv_text_get_size(&sz, cur.text, font, 0, 0, textW, LV_TEXT_FLAG_NONE);
  if (sz.y > room) font = &deck_font_20;
  lv_obj_t* t = label(right, font, C_TEXT, cur.text);
  lv_obj_set_width(t, textW);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);

  for (int i = 1; i <= nextN; i++) {
    snprintf(buf, sizeof(buf), "%d   ", step + 1 + i);
    String line = String(buf) + s_steps[step + i].text;
    oneLine(col, &deck_font_20, C_DIM, line.c_str(), iw);
  }
}

void buildImage() {
  String err;
  const int iw = s_sheet.imgW, ih = s_sheet.imgH;
  if (s_zoom) {
    // 1:1 (2x for a small picture) in a view that pans.
    lv_obj_t* view = lv_obj_create(s_body);
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, s_w, s_h);
    // Pressed itself so it can pan; a tap without a drag bubbles up to the body.
    lv_obj_add_flag(view, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE));
    lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t* img = lv_image_create(view);
    const int z = s_small ? 2 : 1;
    lv_image_set_src(img, s_small ? &s_fitDsc : &s_fullDsc);
    lv_obj_set_size(img, iw * z, ih * z);
    if (z > 1) lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    // Smaller than the view along a side: centred on that side.
    lv_obj_set_pos(img, std::max(0, (s_w - iw * z) / 2), std::max(0, (s_h - ih * z) / 2));
    lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(view);
    lv_obj_scroll_to(view, s_zoomX * z - s_w / 2, s_zoomY * z - s_h / 2, LV_ANIM_OFF);
    return;
  }
  if (!makeFit(err)) {
    lv_obj_t* l = label(s_body, &deck_font_20, C_DIM, err.c_str());
    lv_obj_center(l);
    return;
  }
  lv_obj_t* img = lv_image_create(s_body);
  lv_image_set_src(img, &s_fitDsc);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(img);
  if (s_sheet.title[0]) {
    lv_obj_t* chip = label(s_body, &deck_font_14, C_TEXT, s_sheet.title);
    lv_obj_set_style_bg_color(chip, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_70, 0);
    lv_obj_set_style_radius(chip, 8, 0);
    lv_obj_set_style_pad_hor(chip, 8, 0);
    lv_obj_set_style_pad_ver(chip, 3, 0);
    lv_obj_set_pos(chip, 36, 8);
  }
}

void build() {
  if (!s_body) return;
  lv_obj_clean(s_body);  // before the buffers its images show go
  if (s_shownSeq != s_sheet.seq) {  // a new sheet: its own buffers
    freeImage();
    s_zoom = false;
    s_shownSeq = s_sheet.seq;
  }
  if (s_sheet.kind == 't') buildText();
  else if (s_sheet.kind == 'i') buildImage();
  else buildEmpty();
}

// --- Taps --------------------------------------------------------------------------
void stepBy(int d) {
  const int n = s_stepCount, before = s_sheet.step;
  if (s_sheet.step >= n && d > 0) s_sheet.step = 0;  // "Start again"
  else s_sheet.step = std::max(0, std::min(n, s_sheet.step + d));
  if (s_sheet.step == before) return;
  s_stepDirtyAt = millis() | 1;
  build();
}

// x, y in body coordinates.
bool tapAt(int x, int y) {
  if (s_sheet.kind == 't' && s_stepCount) {
    stepBy(x < s_w / 2 ? -1 : 1);
    return true;
  }
  if (s_sheet.kind != 'i' || !(s_fit || s_full)) return false;
  String err;
  if (s_zoom) {
    lv_obj_clean(s_body);  // the full picture goes: nothing may show it
    if (!zoomOut(err)) {
      build();
      return false;
    }
    s_zoom = false;
  } else {
    // The tapped point of the fitted picture, in full-picture pixels.
    const int ox = (s_w - s_fitW) / 2, oy = (s_h - s_fitH) / 2;
    s_zoomX = std::max(0, std::min(s_sheet.imgW, (x - ox) * s_sheet.imgW / s_fitW));
    s_zoomY = std::max(0, std::min(s_sheet.imgH, (y - oy) * s_sheet.imgH / s_fitH));
    lv_obj_clean(s_body);  // the fitted copy goes: nothing may show it
    if (!zoomIn(err)) {
      deckToastShow(err.c_str(), 2000);
      build();
      return false;
    }
    s_zoom = true;
  }
  build();
  return true;
}

void onTap(lv_event_t*) {
  lv_point_t p;
  lv_indev_get_point(lv_indev_active(), &p);
  lv_area_t a;
  lv_obj_get_coords(s_body, &a);
  if (tapAt(p.x - a.x1, p.y - a.y1)) hapticTap();
}

// --- App -----------------------------------------------------------------------------
void openBench(lv_obj_t* body, int w, int h) {
  ensureLoaded();
  s_body = body;
  s_w = w;
  s_h = h;
  s_zoom = false;
  lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(body, onTap, LV_EVENT_SHORT_CLICKED, nullptr);
  build();
}

void tickBench() {
  if (s_sheet.seq != s_shownSeq) {
    s_zoom = false;
    build();
  }
  if (s_stepDirtyAt && millis() - s_stepDirtyAt >= STEP_SAVE_MS) writeStep();
}

void closeBench() {
  if (s_stepDirtyAt) writeStep();
  freeImage();  // the card goes right after this, with no draw in between
  free(s_clean);
  s_clean = nullptr;
  s_body = nullptr;
}

}  // namespace

// Keeps the screen on while open (Def::awake): it is read across the bench.
const Def kBench = {"bench", "Bench", "mdi:soldering-iron", true, openBench, tickBench, closeBench, false, true};

}  // namespace app

using namespace app;
using namespace deckui;

bool benchStorageReady() { return deck::wallOnSd() || deck::deckFsReady(); }

String benchUploadPath() {
  ensureDir();
  return file("upload.up");
}

bool benchPushText(const char* title, const char* text, String& err) {
  const size_t len = text ? strlen(text) : 0;
  if (!len) {
    err = "empty text";
    return false;
  }
  if (len > BENCH_MAX_TEXT) {
    err = "the text is larger than 8 KB";
    return false;
  }
  ensureLoaded();
  setSheetText(text, len);
  if (!s_sheet.text) {
    err = "out of memory";
    return false;
  }
  strlcpy(s_sheet.title, title ? title : "", sizeof(s_sheet.title));
  s_sheet.kind = 't';
  s_sheet.step = 0;
  s_sheet.seq++;
  s_pendingOpen = true;
  if (benchStorageReady()) {  // kept in RAM even when it cannot be stored
    ensureDir();
    fsys().remove(file("sheet.jpg"));
    writeFile(file("sheet.txt"), text, len);
    writeMeta();
    writeStep();
  }
  return true;
}

bool benchPushImage(const char* title, String& err) {
  const String up = file("upload.up");
  int w = 0, h = 0;
  if (!jpegOpen(up, w, h, nullptr, err)) {
    fsys().remove(up);
    return false;
  }
  ensureLoaded();
  fsys().remove(file("sheet.jpg"));
  if (!fsys().rename(up, file("sheet.jpg"))) {
    fsys().remove(up);
    err = "could not store the picture";
    return false;
  }
  fsys().remove(file("sheet.txt"));
  setSheetText(nullptr, 0);
  strlcpy(s_sheet.title, title ? title : "", sizeof(s_sheet.title));
  s_sheet.kind = 'i';
  s_sheet.imgW = w;
  s_sheet.imgH = h;
  s_sheet.step = 0;
  s_sheet.seq++;
  s_pendingOpen = true;
  writeMeta();
  return true;
}

void benchJson(String& out) {
  ensureLoaded();
  JsonDocument doc;
  doc["kind"] = s_sheet.kind == 't' ? "text" : s_sheet.kind == 'i' ? "image" : "none";
  doc["title"] = s_sheet.title;
  doc["open"] = s_body != nullptr;
  if (s_sheet.kind == 't') {
    if (!s_body) parseSteps();  // the open view keeps its own parse current
    doc["steps"] = s_stepCount;
    doc["step"] = s_sheet.step;
    doc["done"] = s_stepCount && s_sheet.step >= s_stepCount;
  } else if (s_sheet.kind == 'i') {
    doc["w"] = s_sheet.imgW;
    doc["h"] = s_sheet.imgH;
    doc["zoom"] = s_zoom && s_body;
  }
  serializeJson(doc, out);
}

void benchDeckTick() {
  if (!s_pendingOpen) return;
  if (s_body) {  // open: its tick shows the new sheet
    s_pendingOpen = false;
    presenceWake();
    return;
  }
  presenceWake();  // out of idle sleep and the picture frame
  if (nightScreenOff()) wakeDisplayFromTouch();
  if (getScreenState() != SCREEN_DECK || frameActive() || s_ed.root) return;  // next tick
  s_pendingOpen = false;
  openApp(-1, "bench");
}

bool benchTestOp(const String& op, int x, int y) {
  if (!s_body) return false;
  if (op == "next" || op == "back") {
    if (s_sheet.kind != 't' || !s_stepCount) return false;
    stepBy(op == "next" ? 1 : -1);
    return true;
  }
  if (op == "zoom") return s_sheet.kind == 'i' && !s_zoom && tapAt(x, y);
  if (op == "fit") return s_sheet.kind == 'i' && s_zoom && tapAt(0, 0);
  return false;
}
