// Stocks widget ({"type": "widget", "widget": "stocks", "variant": "list" | "s1".."s12"}),
// as Apple's: the pinned stocks of the PC agent (stock_data.h).
//
// "list": a row per pinned stock, as many as the tile holds: the ticker (its
// name under it when the rows are tall), a triangle up in green or down in red
// against the previous close, the price per share, and the day's change when
// the tile is wide. "s1".."s12": one pinned stock, big: the ticker, the price,
// the triangle and change, and the day's line when the tile is tall. "tape": a
// ticker tape, Wall Street's: every pinned stock (ticker, triangle, price,
// change) scrolling right to left, drawn in one object, moving only while its
// page is on screen. A tap opens the Stocks app (on that stock's chart for a
// single one). Old data (no push for 5 minutes) is dimmed.
#include "widgets/widget.h"
#include "ui/deck_ui_int.h"
#include "apps/app_stocks.h"
#include "services/stock_data.h"
#include <math.h>

namespace widget {
namespace {

using namespace deckui;

constexpr int MAX_ROWS = 8;
constexpr int TRI_W = 10, TRI_H = 8;
constexpr int V_LIST = -1, V_TAPE = -2;  // State::which besides a pinned stock's index
constexpr int TAPE_SPEED = 40;           // px a second
constexpr uint32_t TAPE_FRAME_MS = 40;   // 25 fps
constexpr int TAPE_GAP = 26;             // between two stocks

struct Row { lv_obj_t *box, *left, *sym, *name, *tri, *price, *pct; };

// A stock on the tape, measured once per push.
struct TapeItem {
  char sym[16], price[24], pct[12];
  int dir;          // the triangle: 1 up, -1 down, 0 none
  uint32_t color;   // the triangle's and the change's
  int16_t wSym, wPrice, wPct, width;  // width: the whole item, gap included
};

struct State {
  int which = V_LIST;  // V_LIST, V_TAPE or the pinned stock's index
  lv_obj_t *root = nullptr, *title = nullptr;
  // The tape
  lv_obj_t* tape = nullptr;
  TapeItem items[STOCK_MAX] = {};
  int itemCount = 0, tapeLen = 0;  // tapeLen: one turn of the tape, in px
  bool tapeFresh = true, tapeBig = false;
  uint32_t frameAt = 0;
  // Where the tape is: the item at the left edge and how far it has gone past
  // it. Kept across pushes, so new prices (other text widths) don't make it jump.
  int head = 0;
  float headOff = 0;
  Row rows[MAX_ROWS] = {};
  // A single stock
  lv_obj_t *sym = nullptr, *name = nullptr, *price = nullptr, *chgRow = nullptr, *tri = nullptr, *chg = nullptr,
           *line = nullptr;  // between the name and the price: the day's line, or just room
  bool drawLine = false;
  int w = 0, h = 0;
  uint32_t key = UINT32_MAX;
};

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return o;
}

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

// The triangle: up (user data 1) or down (-1) in the object's bg colour; 0 = none.
void triDraw(lv_event_t* e) {
  lv_obj_t* o = lv_event_get_target_obj(e);
  const intptr_t dir = (intptr_t)lv_obj_get_user_data(o);
  if (!dir) return;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.color = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
  d.opa = LV_OPA_COVER;
  const int32_t cx = (a.x1 + a.x2 + 1) / 2;
  if (dir > 0) {
    d.p[0] = {a.x1, a.y2};
    d.p[1] = {a.x2 + 1, a.y2};
    d.p[2] = {cx, a.y1};
  } else {
    d.p[0] = {a.x1, a.y1};
    d.p[1] = {a.x2 + 1, a.y1};
    d.p[2] = {cx, a.y2};
  }
  lv_draw_triangle(lv_event_get_layer(e), &d);
}

lv_obj_t* triangle(lv_obj_t* parent, int w, int h) {
  lv_obj_t* t = plain(parent);
  lv_obj_set_size(t, w, h);
  lv_obj_add_event_cb(t, triDraw, LV_EVENT_DRAW_MAIN, nullptr);
  return t;
}

void setTriangle(lv_obj_t* t, int dir, uint32_t color) {
  if ((intptr_t)lv_obj_get_user_data(t) == dir &&
      lv_color_to_u32(lv_obj_get_style_bg_color(t, LV_PART_MAIN)) == lv_color_to_u32(lv_color_hex(color)))
    return;
  lv_obj_set_user_data(t, (void*)(intptr_t)dir);
  lv_obj_set_style_bg_color(t, lv_color_hex(color), 0);
  lv_obj_invalidate(t);
}

// The day's line of the single stock shown.
void lineDraw(lv_event_t* e) {
  const State* s = (const State*)lv_event_get_user_data(e);
  const StockData& d = stockData();
  if (!s || !s->drawLine || s->which < 0 || s->which >= d.count) return;
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  app::stocksDrawDay(lv_event_get_layer(e), a, d.rows[s->which]);
}

// 12,345.67 (dec decimals); sign: "+" in front of a positive value.
void fmtNum(char* b, size_t n, double v, int dec, bool sign) {
  if (!isfinite(v)) {
    strlcpy(b, "--", n);
    return;
  }
  char raw[32];
  snprintf(raw, sizeof(raw), "%.*f", dec, fabs(v));
  const bool zero = strspn(raw, "0.") == strlen(raw);
  const char* dot = strchr(raw, '.');
  const int intLen = dot ? (int)(dot - raw) : (int)strlen(raw);
  char out[40];
  int o = 0;
  if (!zero && v < 0) out[o++] = '-';
  else if (!zero && sign) out[o++] = '+';
  for (int i = 0; i < intLen && o < (int)sizeof(out) - 2; i++) {
    if (i && (intLen - i) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
  strlcat(out, raw + intLen, sizeof(out));
  strlcpy(b, out, n);
}

// The change against the previous close: its direction, colour and "+1.25%".
int direction(const StockRow& r, uint32_t& color, char* pct, size_t n) {
  color = app::stocksMoveColor(r.price, r.prev);
  if (!isfinite(r.price) || !isfinite(r.prev) || r.prev == 0) {
    strlcpy(pct, "", n);
    return 0;
  }
  fmtNum(pct, n - 1, (r.price - r.prev) / r.prev * 100.0, 2, true);
  strlcat(pct, "%", n);
  return r.price > r.prev ? 1 : r.price < r.prev ? -1 : 0;
}

// --- The tape -------------------------------------------------------------------

const lv_font_t* tapeFont(const State* s) { return s->tapeBig ? &deck_font_20 : &deck_font_14; }
const lv_font_t* tapePctFont(const State* s) { return s->tapeBig ? &deck_font_14 : &deck_font_12; }
int tapeTriW(const State* s) { return s->tapeBig ? 12 : TRI_W; }
int tapeTriH(const State* s) { return s->tapeBig ? 10 : TRI_H; }

void drawText(lv_layer_t* layer, const char* t, const lv_font_t* f, lv_color_t c, lv_opa_t opa, int x, int cy, int w) {
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = t;  // State's own buffers: they hold until the next update, after this frame
  d.font = f;
  d.color = c;
  d.opa = opa;
  const int h = lineH(f);
  lv_area_t a = {x, cy - h / 2, x + w + 1, cy - h / 2 + h - 1};
  lv_draw_label(layer, &d, &a);
}

// The items from the head on, again and again until the right edge.
void tapeDraw(lv_event_t* e) {
  const State* s = (const State*)lv_event_get_user_data(e);
  if (!s || !s->itemCount || s->tapeLen <= 0) return;
  lv_obj_t* o = lv_event_get_target_obj(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_layer_t* layer = lv_event_get_layer(e);
  const lv_font_t *f = tapeFont(s), *fp = tapePctFont(s);
  const int triW = tapeTriW(s), triH = tapeTriH(s);
  const lv_color_t text = lv_obj_get_style_text_color(o, LV_PART_MAIN);
  const lv_opa_t opa = s->tapeFresh ? LV_OPA_COVER : LV_OPA_50;
  const int cy = (a.y1 + a.y2) / 2;
  int x = a.x1 - (int)lroundf(s->headOff);
  for (int i = s->head % s->itemCount; x <= a.x2; i = (i + 1) % s->itemCount) {
    const TapeItem& it = s->items[i];
    if (x + it.width > a.x1) {
      int cx = x;
      drawText(layer, it.sym, f, text, opa, cx, cy, it.wSym);
      cx += it.wSym + 5;
      if (it.dir) {
        lv_draw_triangle_dsc_t t;
        lv_draw_triangle_dsc_init(&t);
        t.color = lv_color_hex(it.color);
        t.opa = opa;
        const int32_t top = cy - triH / 2, bot = top + triH;
        if (it.dir > 0) {
          t.p[0] = {cx, bot};
          t.p[1] = {cx + triW, bot};
          t.p[2] = {cx + triW / 2, top};
        } else {
          t.p[0] = {cx, top};
          t.p[1] = {cx + triW, top};
          t.p[2] = {cx + triW / 2, bot};
        }
        lv_draw_triangle(layer, &t);
      }
      cx += triW + 5;
      drawText(layer, it.price, f, text, opa, cx, cy, it.wPrice);
      cx += it.wPrice + 6;
      drawText(layer, it.pct, fp, lv_color_hex(it.color), opa, cx, cy, it.wPct);
    }
    x += it.width;
  }
}

void fillTape(State* s, const StockData& d, bool fresh) {
  s->tapeBig = s->h >= 44;
  s->tapeFresh = fresh;
  const lv_font_t *f = tapeFont(s), *fp = tapePctFont(s);
  s->itemCount = 0;
  s->tapeLen = 0;
  for (int i = 0; i < d.count; i++) {
    const StockRow& q = d.rows[i];
    TapeItem& it = s->items[s->itemCount++];
    strlcpy(it.sym, q.sym, sizeof(it.sym));
    fmtNum(it.price, sizeof(it.price), q.price, q.dec, false);
    it.dir = direction(q, it.color, it.pct, sizeof(it.pct));
    it.wSym = textW(it.sym, f);
    it.wPrice = textW(it.price, f);
    it.wPct = textW(it.pct, fp);
    it.width = it.wSym + 5 + tapeTriW(s) + 5 + it.wPrice + 6 + it.wPct + TAPE_GAP;
    s->tapeLen += it.width;
    if (q.err) s->tapeFresh = false;
  }
  if (s->itemCount) {  // the same place: the same item, at most its new width in
    s->head %= s->itemCount;
    s->headOff = std::min(s->headOff, (float)s->items[s->head].width);
  }
  show(s->tape, s->itemCount > 0);
  show(s->title, s->itemCount == 0);
  if (!s->itemCount) lv_label_set_text(s->title, d.rxMs ? "Pin stocks on the agent" : "Stocks: no data yet");
  lv_obj_invalidate(s->tape);
}

// --- Building -----------------------------------------------------------------

void buildList(State* s) {
  for (int i = 0; i < MAX_ROWS; i++) {
    Row& r = s->rows[i];
    r.box = plain(s->root);
    lv_obj_set_width(r.box, LV_PCT(100));
    lv_obj_set_flex_grow(r.box, 1);
    lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r.box, 6, 0);
    r.left = plain(r.box);
    lv_obj_set_size(r.left, 10, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(r.left, 1);
    lv_obj_set_flex_flow(r.left, LV_FLEX_FLOW_COLUMN);
    r.sym = label(r.left, &deck_font_14);
    lv_label_set_long_mode(r.sym, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r.sym, LV_PCT(100));
    r.name = label(r.left, &deck_font_12, LV_OPA_60);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r.name, LV_PCT(100));
    r.tri = triangle(r.box, TRI_W, TRI_H);
    r.price = label(r.box, &deck_font_14);
    r.pct = label(r.box, &deck_font_12);
    lv_obj_set_style_text_align(r.pct, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(r.pct, 58);
    show(r.box, false);
  }
}

void buildSingle(State* s) {
  lv_obj_t* top = plain(s->root);
  lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
  s->sym = label(top, &deck_font_14);
  lv_label_set_long_mode(s->sym, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s->sym, LV_PCT(100));
  s->name = label(top, &deck_font_12, LV_OPA_60);
  lv_label_set_long_mode(s->name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(s->name, LV_PCT(100));
  s->line = plain(s->root);
  lv_obj_set_width(s->line, LV_PCT(100));
  lv_obj_set_flex_grow(s->line, 1);
  lv_obj_add_event_cb(s->line, lineDraw, LV_EVENT_DRAW_MAIN, s);
  s->price = label(s->root, &deck_font_28);
  s->chgRow = plain(s->root);
  lv_obj_set_size(s->chgRow, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s->chgRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s->chgRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s->chgRow, 5, 0);
  s->tri = triangle(s->chgRow, TRI_W, TRI_H);
  s->chg = label(s->chgRow, &deck_font_14);
}

void* create(lv_obj_t* tile, const char* variant, const char*) {
  State* s = new (std::nothrow) State();
  if (!s) return nullptr;
  if (!strcmp(variant, "tape")) {
    s->which = V_TAPE;
  } else if (variant[0] == 's' && isdigit((unsigned char)variant[1])) {
    const int n = atoi(variant + 1);
    if (n >= 1 && n <= STOCK_MAX) s->which = n - 1;
  }
  s->root = plain(tile);
  lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s->root, 2, 0);
  s->title = label(s->root, &deck_font_12, LV_OPA_70);
  show(s->title, false);
  if (s->which == V_TAPE) {
    s->tape = plain(s->root);
    lv_obj_set_width(s->tape, LV_PCT(100));
    lv_obj_set_flex_grow(s->tape, 1);
    lv_obj_add_event_cb(s->tape, tapeDraw, LV_EVENT_DRAW_MAIN, s);
  } else if (s->which == V_LIST) {
    buildList(s);
  } else {
    buildSingle(s);
  }
  return s;
}

// --- Refreshing -----------------------------------------------------------------

void fillList(State* s, const StockData& d, bool fresh) {
  // As many rows as fit: taller ones show the name too.
  const int fit = std::max(1, std::min<int>(MAX_ROWS, s->h / 22));
  const int n = std::min<int>(d.count, fit);
  const bool names = n > 0 && s->h / n >= 38 && s->w >= 120;
  const bool pct = s->w >= 190;
  for (int i = 0; i < MAX_ROWS; i++) {
    Row& r = s->rows[i];
    show(r.box, i < n);
    if (i >= n) continue;
    const StockRow& q = d.rows[i];
    lv_label_set_text(r.sym, q.sym);
    lv_label_set_text(r.name, q.name);
    show(r.name, names);
    char b[32], p[16];
    uint32_t color;
    const int dir = direction(q, color, p, sizeof(p));
    setTriangle(r.tri, dir, color);
    fmtNum(b, sizeof(b), q.price, q.dec, false);
    lv_label_set_text(r.price, b);
    lv_obj_set_style_opa(r.price, fresh && !q.err ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_label_set_text(r.pct, p);
    lv_obj_set_style_text_color(r.pct, lv_color_hex(color), 0);
    show(r.pct, pct);
  }
  // Nothing pinned yet: say where to pin.
  show(s->title, n == 0);
  if (n == 0) lv_label_set_text(s->title, d.rxMs ? "Pin stocks on the agent" : "Stocks: no data yet");
}

void fillSingle(State* s, const StockData& d, bool fresh) {
  const bool has = s->which < d.count;
  const StockRow* q = has ? &d.rows[s->which] : nullptr;
  const bool big = s->w >= 110 && s->h >= 80;
  lv_obj_set_style_text_font(s->price, big ? &deck_font_28 : &deck_font_20, 0);
  show(s->name, has && s->h >= 96);
  s->drawLine = has && s->h >= 120;
  show(s->chgRow, has);
  if (!q) {
    char b[24];
    snprintf(b, sizeof(b), "Stock %d", s->which + 1);
    lv_label_set_text(s->sym, b);
    lv_label_set_text(s->price, "--");
    lv_obj_set_style_opa(s->price, LV_OPA_50, 0);
    return;
  }
  lv_label_set_text(s->sym, q->sym);
  lv_label_set_text(s->name, q->name);
  char b[32], p[16];
  fmtNum(b, sizeof(b), q->price, q->dec, false);
  lv_label_set_text(s->price, b);
  lv_obj_set_style_opa(s->price, fresh && !q->err ? LV_OPA_COVER : LV_OPA_50, 0);
  uint32_t color;
  const int dir = direction(*q, color, p, sizeof(p));
  setTriangle(s->tri, dir, color);
  lv_label_set_text(s->chg, p);
  lv_obj_set_style_text_color(s->chg, lv_color_hex(color), 0);
  lv_obj_invalidate(s->line);
}

bool update(void* p) {
  State* s = (State*)p;
  if (!s || !s->w) return false;
  const StockData& d = stockData();
  const bool fresh = stockFresh();
  const uint32_t key = d.seq * 2 + (fresh ? 1 : 0);
  if (s->which == V_TAPE) {
    const bool changed = key != s->key;
    if (changed) {
      s->key = key;
      fillTape(s, d, fresh);
    }
    // It moves only on screen; a snapshot of its page keeps the last place.
    const uint32_t now = millis();
    if (s->itemCount && now - s->frameAt >= TAPE_FRAME_MS && widgetOnScreen(s)) {
      // Back on screen after a while: carry on from where it stopped
      const uint32_t dt = now - s->frameAt > 4 * TAPE_FRAME_MS ? TAPE_FRAME_MS : now - s->frameAt;
      s->frameAt = now;
      s->headOff += dt * TAPE_SPEED / 1000.0f;
      while (s->headOff >= s->items[s->head].width) {
        s->headOff -= s->items[s->head].width;
        s->head = (s->head + 1) % s->itemCount;
      }
      lv_obj_invalidate(s->tape);
      s_sparkPaintMs = now;  // like a chart glide: other pages snapshot rarely meanwhile
    }
    return changed;
  }
  if (key == s->key) return false;
  s->key = key;
  if (s->which == V_LIST) fillList(s, d, fresh);
  else fillSingle(s, d, fresh);
  return true;
}

void resize(void* p, int w, int h) {
  State* s = (State*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  lv_obj_set_size(s->root, w, h);
  s->key = UINT32_MAX;  // relayout on the next update
}

void destroy(void* p) { delete (State*)p; }

// Opens the Stocks app, on this stock's chart for a single one.
void tap(void* p) {
  State* s = (State*)p;
  if (!s) return;
  const StockData& d = stockData();
  app::stocksFocus(s->which >= 0 && s->which < d.count ? d.rows[s->which].sym : "");
  openApp(-1, "stocks");
}

const Variant kVariants[] = {
    {"list", "Watchlist"}, {"tape", "Ticker tape"},
    {"s1", "Stock 1"},     {"s2", "Stock 2"},  {"s3", "Stock 3"},  {"s4", "Stock 4"},
    {"s5", "Stock 5"},     {"s6", "Stock 6"},  {"s7", "Stock 7"},   {"s8", "Stock 8"},  {"s9", "Stock 9"},
    {"s10", "Stock 10"},   {"s11", "Stock 11"}, {"s12", "Stock 12"}};

}  // namespace

// No "app": the tap goes to the widget, which opens the Stocks app (on its stock).
extern const Def kStocksWidget = {"stocks", "Stocks", nullptr, kVariants, sizeof(kVariants) / sizeof(kVariants[0]),
                                  create, update, resize, destroy, tap};

}  // namespace widget
