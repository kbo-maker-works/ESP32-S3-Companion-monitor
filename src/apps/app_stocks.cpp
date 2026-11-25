// Stocks app: the symbols pinned on the PC agent (stock_data.h,
// pc-agent/td_stocks.py). The list has one row per symbol: its market's state,
// the price, the day's change and line (the previous close dashed). A tap on a
// row shows its chart, over the range picked under the price; the agent sends
// it when asked, and again every minute for a 1D chart while the market is open.
//
// The rows are built once per open at their maximum count and refilled in
// place on each push (StockData::seq). The chart's fill is painted once per
// chart into an A8 buffer drawn in the line's colour; the line itself is drawn
// on top each time the chart is redrawn.
#include "apps/app_stocks.h"
#include "ui/deck_ui_int.h"
#include "services/stock_data.h"
#include <math.h>

namespace app {
namespace {

using namespace deckui;

constexpr int ROW_H = 50;
constexpr int CHART_AXIS = 58;   // right of the plot: the high and the low
constexpr int CHART_INSET = 6;   // the line stays this far from the plot's top and bottom
constexpr int FILL_TOP = 80;     // the fill's opacity at the plot's top (of 255), fading to 0 at its bottom
constexpr uint32_t ASK_TIMEOUT_MS = 12000;  // no chart by then: say so
constexpr uint32_t LIVE_EVERY_MS = 60000;   // an open market's 1D chart is asked again this often
constexpr uint32_t CACHE_MS = 60000;        // the last chart shows again without asking within this

struct Row { lv_obj_t *box, *dot, *sym, *name, *spark, *price, *chg; };

struct Ui {
  lv_obj_t *list, *summary, *err, *empty;
  Row rows[STOCK_MAX];
  lv_obj_t *detail, *dSym, *dName, *dWhen, *dPrice, *dCur, *dChg, *tab[STOCK_RANGES];
  lv_obj_t *chart, *hi, *lo, *msg, *axis, *xl[STOCK_LABELS];
  int sel;             // the row shown in detail, -1 = the list
  char sym[16];        // its symbol: rows may move between pushes
  uint32_t seq;        // stockData().seq shown
  uint32_t chartSeq;   // stockChart().seq seen
  uint32_t askedMs;    // the chart was asked at, 0 = not waiting
  bool timedOut;
  char askErr[48];     // the ask could not be sent: why
  uint32_t ageAt;
  lv_draw_buf_t* fill; // the area under the chart's line
  uint32_t fillSeq;    // the chart it was painted for
  float vLo, vHi;      // the chart's vertical range, as painted
};
Ui* s = nullptr;
int s_range = 0;      // the chart's range (stockRangeId), for the whole session
char s_focus[16] = "";  // stocksFocus: show this symbol's chart on open

// --- Small builders (as the Work app's) --------------------------------------------

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return o;
}

lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* t = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, t);
  return l;
}

// One line that ends in "..." when it does not fit, as wide as its parent.
lv_obj_t* fullLine(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = text(parent, font, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(l, LV_PCT(100), lineH(font));
  return l;
}

lv_obj_t* flexRow(lv_obj_t* parent, int gap) {
  lv_obj_t* r = plain(parent);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, gap, 0);
  return r;
}

lv_obj_t* card(lv_obj_t* parent) {
  lv_obj_t* b = plain(parent);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  return b;
}

void pressable(lv_obj_t* b, lv_event_cb_t cb, uintptr_t data) {
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE_LINE), LV_STATE_PRESSED);
  lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, (void*)data);
}

// --- Numbers ---------------------------------------------------------------------

// 12,345.67 with dec decimals; sign: "+" in front of a positive value.
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
  char out[48];
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

// "+0.12 (+1.25%)"; "" when either is unknown.
void fmtChange(char* b, size_t n, double now, double base, int dec) {
  b[0] = 0;
  if (!isfinite(now) || !isfinite(base) || base == 0) return;
  char abs_[24], pct[16];
  fmtNum(abs_, sizeof(abs_), now - base, dec, true);
  fmtNum(pct, sizeof(pct), (now - base) / base * 100.0, 2, true);
  snprintf(b, n, "%s (%s%%)", abs_, pct);
}

uint32_t moveColor(double now, double base) { return stocksMoveColor(now, base); }

uint32_t stateColor(char st) {
  switch (st) {
    case 'o': return C_OK;
    case 'p':
    case 'a': return C_WARN;
    default: return C_DIM;
  }
}

int rowOf(const char* sym) {
  const StockData& d = stockData();
  for (int i = 0; i < d.count; i++)
    if (!strcmp(d.rows[i].sym, sym)) return i;
  return -1;
}

const StockRow* selRow() {
  const int i = s && s->sel >= 0 ? rowOf(s->sym) : -1;
  return i >= 0 ? &stockData().rows[i] : nullptr;
}

// --- The list --------------------------------------------------------------------

// The day's line, its previous close dashed, across the part of the day gone.
void sparkDraw(lv_event_t* e) {
  if (!s) return;
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  const StockData& d = stockData();
  if (i >= d.count) return;
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  stocksDrawDay(lv_event_get_layer(e), a, d.rows[i]);
}

void showDetail(int i);

void onRow(lv_event_t* e) {
  if (!s) return;
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  if (i >= stockData().count) return;
  hapticTap();
  showDetail(i);
}

void buildRow(lv_obj_t* parent, Row& r, int i, int sparkW) {
  r.box = card(parent);
  lv_obj_set_size(r.box, LV_PCT(100), ROW_H);
  lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(r.box, 10, 0);
  lv_obj_set_style_pad_column(r.box, 10, 0);
  pressable(r.box, onRow, i);

  lv_obj_t* left = plain(r.box);
  lv_obj_set_size(left, 10, LV_SIZE_CONTENT);
  lv_obj_set_flex_grow(left, 1);
  lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(left, 2, 0);
  lv_obj_t* top = flexRow(left, 6);
  r.dot = plain(top);
  lv_obj_set_size(r.dot, 7, 7);
  lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
  r.sym = text(top, &deck_font_14, C_TEXT);
  r.name = fullLine(left, &deck_font_12, C_DIM);

  r.spark = plain(r.box);
  lv_obj_set_size(r.spark, sparkW, 30);
  lv_obj_add_event_cb(r.spark, sparkDraw, LV_EVENT_DRAW_MAIN, (void*)(uintptr_t)i);

  lv_obj_t* right = plain(r.box);
  lv_obj_set_size(right, 112, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_row(right, 2, 0);
  r.price = text(right, &deck_font_20, C_TEXT);
  r.chg = text(right, &deck_font_12, C_DIM);
  lv_obj_set_style_radius(r.chg, 4, 0);
  lv_obj_set_style_pad_hor(r.chg, 5, 0);
  lv_obj_set_style_pad_ver(r.chg, 1, 0);
  lv_obj_set_style_bg_opa(r.chg, LV_OPA_20, 0);
}

void fillRow(Row& r, const StockRow& d) {
  lv_obj_set_style_bg_color(r.dot, lv_color_hex(stateColor(d.state)), 0);
  lv_obj_set_style_bg_opa(r.dot, d.state == 'c' ? LV_OPA_50 : LV_OPA_COVER, 0);
  setText(r.sym, d.sym);
  setText(r.name, d.name);
  char b[32];
  fmtNum(b, sizeof(b), d.price, d.dec, false);
  setText(r.price, b);
  lv_obj_set_style_text_color(r.price, lv_color_hex(d.err ? C_DIM : C_TEXT), 0);
  const uint32_t c = moveColor(d.price, d.prev);
  if (isfinite(d.prev) && d.prev != 0 && isfinite(d.price)) {
    fmtNum(b, sizeof(b) - 1, (d.price - d.prev) / d.prev * 100.0, 2, true);
    strlcat(b, "%", sizeof(b));
  } else {
    strlcpy(b, "--", sizeof(b));
  }
  setText(r.chg, b);
  lv_obj_set_style_text_color(r.chg, lv_color_hex(c), 0);
  lv_obj_set_style_bg_color(r.chg, lv_color_hex(c), 0);
  lv_obj_invalidate(r.spark);
}

void refreshAge() {
  const StockData& d = stockData();
  char b[64] = "";
  const bool fresh = stockFresh();
  if (d.rxMs) {
    const uint32_t m = (millis() - d.rxMs) / 60000;
    if (m < 1) snprintf(b, sizeof(b), "%d stocks · from %s · just now", d.count, d.pcName);
    else snprintf(b, sizeof(b), "%d stocks · from %s · %um ago", d.count, d.pcName, (unsigned)m);
  }
  setText(s->summary, b);
  lv_obj_set_style_text_color(s->summary, lv_color_hex(fresh ? C_DIM : C_WARN), 0);
}

void refreshList() {
  const StockData& d = stockData();
  s->seq = d.seq;
  for (int i = 0; i < STOCK_MAX; i++) {
    showObj(s->rows[i].box, i < d.count);
    if (i < d.count) fillRow(s->rows[i], d.rows[i]);
  }
  setText(s->err, d.err);
  showObj(s->err, d.err[0] != 0);
  showObj(s->empty, d.count == 0);
  refreshAge();
}

// --- The chart -------------------------------------------------------------------

bool chartMatches(const StockChart& c) {
  return s && s->sel >= 0 && c.rxMs && !strcmp(c.sym, s->sym) && !strcmp(c.range, stockRangeId(s_range));
}

bool chartShown(const StockChart& c) { return chartMatches(c) && !c.err[0] && c.n >= 2; }

float plotY(float v, int h) {
  return CHART_INSET + (h - 1 - 2 * CHART_INSET) * (s->vHi - v) / (s->vHi - s->vLo);
}

// The fill under the line, opacity fading towards the plot's bottom.
void paintFill(const StockChart& c, int pw, int h) {
  if (s->fill && ((int)s->fill->header.w != pw || (int)s->fill->header.h != h)) {
    lv_image_cache_drop(s->fill);
    lv_draw_buf_destroy(s->fill);
    s->fill = nullptr;
  }
  if (!s->fill) s->fill = lv_draw_buf_create(pw, h, LV_COLOR_FORMAT_A8, 0);
  if (!s->fill) return;
  lv_image_cache_drop(s->fill);
  const uint32_t stride = s->fill->header.stride;
  for (int x = 0; x < pw; x++) {
    float t = pw > 1 ? (float)x * (c.n - 1) / (pw - 1) : 0;
    int i = (int)t;
    if (i >= c.n - 1) i = c.n - 2;
    const float v = c.p[i] + (c.p[i + 1] - c.p[i]) * (t - i);
    const float yl = plotY(v, h);
    for (int y = 0; y < h; y++) {
      const float cov = constrain(y + 1 - yl, 0.0f, 1.0f);
      s->fill->data[y * stride + x] = (uint8_t)(cov * FILL_TOP * (h - y) / h);
    }
  }
  s->fillSeq = c.seq;
}

uint32_t chartColor(const StockChart& c) {
  const StockRow* r = selRow();
  const double last = s_range == 0 && r ? r->price : c.p[c.n - 1];
  return moveColor(last, c.base);
}

void chartDraw(lv_event_t* e) {
  if (!s) return;
  const StockChart& c = stockChart();
  if (!chartShown(c) || !(s->vHi > s->vLo)) return;
  lv_area_t a;
  lv_obj_get_coords(lv_event_get_target_obj(e), &a);
  const int pw = lv_area_get_width(&a) - CHART_AXIS, h = lv_area_get_height(&a);
  if (pw < 20 || h < 20) return;
  lv_layer_t* layer = lv_event_get_layer(e);
  const lv_color_t col = lv_color_hex(chartColor(c));
  if (s->fill && s->fillSeq == c.seq && (int)s->fill->header.w == pw && (int)s->fill->header.h == h) {
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = s->fill;
    d.recolor = col;
    d.recolor_opa = LV_OPA_COVER;
    lv_area_t fa = {a.x1, a.y1, a.x1 + pw - 1, a.y2};
    lv_draw_image(layer, &d, &fa);
  }
  lv_draw_line_dsc_t dl;
  if (c.prevClose && isfinite(c.base)) {  // the previous close
    lv_draw_line_dsc_init(&dl);
    const int32_t y = a.y1 + (int32_t)plotY((float)c.base, h);
    dl.p1 = {a.x1, y};
    dl.p2 = {a.x1 + pw - 1, y};
    dl.color = lv_color_hex(C_DIM);
    dl.opa = LV_OPA_60;
    dl.dash_width = 4;
    dl.dash_gap = 4;
    lv_draw_line(layer, &dl);
  }
  lv_point_precise_t pts[STOCK_CHART_MAX];
  for (int k = 0; k < c.n; k++) {
    pts[k].x = a.x1 + (int32_t)((float)(pw - 1) * k / (c.n - 1));
    pts[k].y = a.y1 + (int32_t)plotY(c.p[k], h);
  }
  lv_draw_line_dsc_init(&dl);
  dl.points = pts;
  dl.point_cnt = c.n;
  dl.width = 2;
  dl.round_start = dl.round_end = 1;
  dl.color = col;
  lv_draw_line(layer, &dl);
}

// The price and change at the top of the detail: live for 1D, over the
// chart's range otherwise.
void refreshHead() {
  const StockRow* r = selRow();
  if (!r) return;
  setText(s->dSym, r->sym);
  setText(s->dName, r->name);
  setText(s->dWhen, r->when);
  lv_obj_set_style_text_color(s->dWhen, lv_color_hex(stateColor(r->state)), 0);
  char b[48];
  fmtNum(b, sizeof(b), r->price, r->dec, false);
  setText(s->dPrice, b);
  setText(s->dCur, r->cur);
  const StockChart& c = stockChart();
  const double base = s_range == 0 || !chartShown(c) ? r->prev : c.base;
  char ch[40];
  fmtChange(ch, sizeof(ch), r->price, base, r->dec);
  if (ch[0] && (s_range == 0 || chartShown(c))) snprintf(b, sizeof(b), "%s  %s", ch, stockRangeLabel(s_range));
  else strlcpy(b, ch, sizeof(b));
  setText(s->dChg, b);
  lv_obj_set_style_text_color(s->dChg, lv_color_hex(moveColor(r->price, base)), 0);
}

void refreshChart() {
  const StockChart& c = stockChart();
  const bool shown = chartShown(c);
  const StockRow* r = selRow();
  if (shown) {
    float lo = c.p[0], hi = c.p[0];
    for (int k = 1; k < c.n; k++) {
      lo = std::min(lo, c.p[k]);
      hi = std::max(hi, c.p[k]);
    }
    if (c.prevClose && isfinite(c.base)) {
      lo = std::min(lo, (float)c.base);
      hi = std::max(hi, (float)c.base);
    }
    if (!(hi > lo)) {  // a flat line sits in the middle
      const float pad = fabsf(hi) * 0.01f + 0.01f;
      lo -= pad;
      hi += pad;
    }
    s->vLo = lo;
    s->vHi = hi;
    lv_obj_update_layout(s->chart);
    const int pw = lv_obj_get_width(s->chart) - CHART_AXIS, h = lv_obj_get_height(s->chart);
    if (pw >= 20 && h >= 20) paintFill(c, pw, h);
    char b[24];
    fmtNum(b, sizeof(b), hi, c.dec, false);
    setText(s->hi, b);
    fmtNum(b, sizeof(b), lo, c.dec, false);
    setText(s->lo, b);
  }
  showObj(s->hi, shown);
  showObj(s->lo, shown);

  // The time labels under the plot, centred on their place, inside it.
  lv_obj_update_layout(s->axis);
  const int pw = lv_obj_get_width(s->axis) - CHART_AXIS;
  for (int i = 0; i < STOCK_LABELS; i++) {
    const bool on = shown && i < c.labelCount;
    showObj(s->xl[i], on);
    if (!on) continue;
    setText(s->xl[i], c.labels[i].text);
    const int tw = textW(c.labels[i].text, &deck_font_12);
    const int x = constrain((int)(c.labels[i].at * (pw - 1)) - tw / 2, 0, std::max(0, pw - tw));
    lv_obj_set_pos(s->xl[i], x, 0);
  }

  const char* m = "";
  char b[64];
  if (!shown) {
    if (chartMatches(c) && c.err[0]) m = c.err;
    else if (s->askErr[0]) m = s->askErr;
    else if (s->timedOut) {
      snprintf(b, sizeof(b), "No answer from %s", stockData().pcName);
      m = b;
    } else if (s->askedMs) m = "Loading...";
    else if (chartMatches(c)) m = "No data for this range";
  }
  setText(s->msg, m);
  showObj(s->msg, m[0] != 0);
  for (int i = 0; i < STOCK_RANGES; i++) {
    if (i == s_range) lv_obj_add_state(s->tab[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(s->tab[i], LV_STATE_CHECKED);
  }
  if (r) refreshHead();
  lv_obj_invalidate(s->chart);
}

// Ask the agent for the chart, unless the last one is this one and recent.
void askChart(bool force) {
  const StockChart& c = stockChart();
  s->timedOut = false;
  s->askErr[0] = 0;
  if (!force && chartShown(c) && millis() - c.rxMs < CACHE_MS) {
    s->askedMs = 0;
    refreshChart();
    return;
  }
  String err;
  if (stockAskChart(s->sym, stockRangeId(s_range), err)) {
    s->askedMs = millis() | 1;
  } else {
    s->askedMs = 0;
    strlcpy(s->askErr, err.c_str(), sizeof(s->askErr));
  }
  refreshChart();
}

void showList() {
  s->sel = -1;
  showObj(s->detail, false);
  showObj(s->list, true);
}

void showDetail(int i) {
  const StockData& d = stockData();
  if (i < 0 || i >= d.count) return;
  s->sel = i;
  strlcpy(s->sym, d.rows[i].sym, sizeof(s->sym));
  showObj(s->list, false);
  showObj(s->detail, true);
  refreshHead();
  askChart(false);
}

void onBack(lv_event_t*) {
  if (!s) return;
  hapticTap();
  showList();
}

void onRange(lv_event_t* e) {
  if (!s) return;
  const int i = (int)(uintptr_t)lv_event_get_user_data(e);
  if (i == s_range) return;
  hapticTap();
  s_range = i;
  askChart(false);
}

void buildDetail(lv_obj_t* body, int w, int h) {
  lv_obj_t* d = s->detail = plain(body);
  lv_obj_set_size(d, w, h);
  lv_obj_set_flex_flow(d, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(d, 10, 0);
  lv_obj_set_style_pad_ver(d, 6, 0);
  lv_obj_set_style_pad_row(d, 6, 0);
  showObj(d, false);

  // Back, the symbol and its name; the market's state on the right
  lv_obj_t* head = flexRow(d, 8);
  lv_obj_t* back = card(head);
  lv_obj_set_size(back, 40, 30);
  pressable(back, onBack, 0);
  lv_obj_t* arrow = text(back, &deck_font_14, C_TEXT, LV_SYMBOL_LEFT);
  lv_obj_center(arrow);
  s->dSym = text(head, &deck_font_20, C_TEXT);
  s->dName = text(head, &deck_font_12, C_DIM);
  lv_label_set_long_mode(s->dName, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(s->dName, 10, lineH(&deck_font_12));
  lv_obj_set_flex_grow(s->dName, 1);
  s->dWhen = text(head, &deck_font_12, C_DIM);

  // The price, its currency and the change over the range
  lv_obj_t* prow = flexRow(d, 8);
  lv_obj_set_flex_align(prow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  s->dPrice = text(prow, &deck_font_28, C_TEXT);
  s->dCur = text(prow, &deck_font_14, C_DIM);
  lv_obj_set_style_pad_bottom(s->dCur, 3, 0);
  s->dChg = text(prow, &deck_font_14, C_DIM);
  lv_obj_set_style_pad_bottom(s->dChg, 3, 0);
  lv_obj_set_style_pad_left(s->dChg, 6, 0);

  // The ranges
  lv_obj_t* tabs = flexRow(d, 4);
  const int tw = std::min(52, (w - 20 - 4 * (STOCK_RANGES - 1)) / STOCK_RANGES);
  for (int i = 0; i < STOCK_RANGES; i++) {
    lv_obj_t* t = s->tab[i] = card(tabs);
    lv_obj_set_size(t, tw, 26);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x0B1220), LV_STATE_CHECKED);
    pressable(t, onRange, i);
    lv_obj_t* l = lv_label_create(t);
    lv_obj_set_style_text_font(l, &deck_font_12, 0);
    lv_label_set_text(l, stockRangeLabel(i));
    lv_obj_center(l);
  }

  // The plot, the high and low on its right, the times under it
  s->chart = plain(d);
  lv_obj_set_width(s->chart, LV_PCT(100));
  lv_obj_set_flex_grow(s->chart, 1);
  lv_obj_add_event_cb(s->chart, chartDraw, LV_EVENT_DRAW_MAIN, nullptr);
  s->hi = text(s->chart, &deck_font_12, C_DIM);
  lv_obj_align(s->hi, LV_ALIGN_TOP_RIGHT, 0, 0);
  s->lo = text(s->chart, &deck_font_12, C_DIM);
  lv_obj_align(s->lo, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  s->msg = text(s->chart, &deck_font_14, C_DIM);
  lv_obj_set_style_text_align(s->msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s->msg, LV_ALIGN_CENTER, -CHART_AXIS / 2, 0);
  s->axis = plain(d);
  lv_obj_set_size(s->axis, LV_PCT(100), lineH(&deck_font_12));
  for (int i = 0; i < STOCK_LABELS; i++) {
    s->xl[i] = text(s->axis, &deck_font_12, C_DIM);
    showObj(s->xl[i], false);
  }
}

// --- App -------------------------------------------------------------------------

void open(lv_obj_t* body, int w, int h) {
  s = new (std::nothrow) Ui();
  if (!s) return;
  s->sel = -1;

  lv_obj_t* l = s->list = lv_obj_create(body);
  lv_obj_remove_style_all(l);
  lv_obj_set_size(l, w, h);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(l, 8, 0);
  lv_obj_set_style_pad_ver(l, 6, 0);
  lv_obj_set_style_pad_row(l, 4, 0);
  lv_obj_set_scroll_dir(l, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_ACTIVE);
  s->summary = fullLine(l, &deck_font_12, C_DIM);
  s->err = fullLine(l, &deck_font_12, C_CRIT);
  const int sparkW = w >= 400 ? 96 : 64;
  for (int i = 0; i < STOCK_MAX; i++) buildRow(l, s->rows[i], i, sparkW);

  buildDetail(body, w, h);

  // Over the list while there is nothing to show
  s->empty = plain(body);
  lv_obj_set_size(s->empty, w, h);
  lv_obj_t* msg = text(s->empty, &deck_font_20, C_TEXT, "No stocks yet");
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -22);
  lv_obj_t* hint = text(s->empty, &deck_font_14, C_DIM,
                        "Pin them on the agent's settings page\n(Stocks, 127.0.0.1:4213 where it runs)");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);

  s->chartSeq = stockChart().seq;
  refreshList();
  s->ageAt = millis();
  if (s_focus[0]) {
    showDetail(rowOf(s_focus));
    s_focus[0] = 0;
  }
}

void tick() {
  if (!s) return;
  if (stockData().seq != s->seq) {
    refreshList();
    if (s->sel >= 0) {
      const int i = rowOf(s->sym);
      if (i < 0) showList();  // unpinned meanwhile
      else {
        s->sel = i;
        refreshHead();
        lv_obj_invalidate(s->chart);  // a 1D line's colour follows the live price
      }
    }
  }
  const StockChart& c = stockChart();
  if (c.seq != s->chartSeq) {
    s->chartSeq = c.seq;
    if (chartMatches(c)) {
      s->askedMs = 0;
      s->timedOut = false;
    }
    if (s->sel >= 0) refreshChart();
  }
  if (millis() - s->ageAt < 1000) return;
  s->ageAt = millis();
  refreshAge();
  if (s->sel < 0) return;
  if (s->askedMs && millis() - s->askedMs >= ASK_TIMEOUT_MS) {
    s->askedMs = 0;
    s->timedOut = true;
    refreshChart();
  }
  // A 1D chart of an open market keeps up with it.
  const StockRow* r = selRow();
  if (s_range == 0 && r && r->state == 'o' && !s->askedMs && chartShown(c) && millis() - c.rxMs >= LIVE_EVERY_MS)
    askChart(true);
}

void close() {
  if (!s) return;
  if (s->fill) {
    lv_image_cache_drop(s->fill);
    lv_draw_buf_destroy(s->fill);
  }
  delete s;  // the card deletes the objects
  s = nullptr;
}

}  // namespace

uint32_t stocksMoveColor(double now, double base) {
  if (!isfinite(now) || !isfinite(base)) return C_DIM;
  return now >= base ? C_OK : C_CRIT;
}

void stocksDrawDay(lv_layer_t* layer, const lv_area_t& a, const StockRow& r) {
  if (r.n < 2 || !(r.hi > r.lo)) return;
  const int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
  if (w < 8 || h < 8) return;
  lv_draw_line_dsc_t dl;
  if (isfinite(r.prev)) {
    lv_draw_line_dsc_init(&dl);
    const int32_t y = a.y1 + 2 + (int32_t)((h - 5) * (1.0f - (float)(r.prev - r.lo) / (r.hi - r.lo)));
    dl.p1 = {a.x1, y};
    dl.p2 = {a.x2, y};
    dl.color = lv_color_hex(C_DIM);
    dl.opa = LV_OPA_50;
    dl.dash_width = 3;
    dl.dash_gap = 3;
    lv_draw_line(layer, &dl);
  }
  lv_point_precise_t pts[STOCK_SPARK];
  const float span = (w - 1) * r.span;
  for (int k = 0; k < r.n; k++) {
    pts[k].x = a.x1 + (int32_t)(span * k / (r.n - 1));
    pts[k].y = a.y1 + 2 + (int32_t)((h - 5) * (1.0f - r.spark[k] / 255.0f));
  }
  lv_draw_line_dsc_init(&dl);
  dl.points = pts;
  dl.point_cnt = r.n;
  dl.width = 2;
  dl.round_start = dl.round_end = 1;
  dl.color = lv_color_hex(stocksMoveColor(r.price, r.prev));
  lv_draw_line(layer, &dl);
}

void stocksFocus(const char* sym) {
  strlcpy(s_focus, sym ? sym : "", sizeof(s_focus));
  if (s && s_focus[0]) {  // already open: show it now
    showDetail(rowOf(s_focus));
    s_focus[0] = 0;
  }
}

const Def kStocks = {"stocks", "Stocks", "mdi:chart-line", true, open, tick, close, true};

}  // namespace app
