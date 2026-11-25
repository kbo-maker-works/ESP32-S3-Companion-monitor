#include "services/stock_data.h"
#include "services/pc_metrics.h"
#include "core/psram_json.h"
#include <ArduinoJson.h>
#include <math.h>

namespace {

// In PSRAM, allocated by the first push. Until then the empty ones (flash).
StockData* s_rows = nullptr;
StockChart* s_chart = nullptr;
const StockData kNoRows = {};
const StockChart kNoChart = {};

const char* const kRangeIds[STOCK_RANGES] = {"1d", "5d", "1mo", "6mo", "1y", "5y"};
const char* const kRangeLabels[STOCK_RANGES] = {"1D", "5D", "1M", "6M", "1Y", "5Y"};

void copy(char* out, size_t n, JsonVariantConst v) { strlcpy(out, v | "", n); }

// The PC that may push: one the deck knows, at its metrics' address, and the
// Stocks app's own PC when it has one.
const PcSource* sender(JsonDocument& doc, IPAddress from, String& err) {
  const char* id = doc["pc_id"] | "";
  const PcSource* pc = id[0] ? pcSource(id) : nullptr;
  if (!pc || !(pc->ip == from)) {
    err = "unknown PC (its agent must be sending metrics)";
    return nullptr;
  }
  const char* own = pcAppMode("stocks");
  if (own[0] && strcmp(own, pc->id) && pcSource(own)) {
    err = "the Stocks app shows another PC";
    return nullptr;
  }
  return pc;
}

template <typename T>
T* psramStruct(T*& keep) {
  if (!keep) keep = (T*)heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM);
  return keep;
}

}  // namespace

const char* stockRangeId(int i) { return i >= 0 && i < STOCK_RANGES ? kRangeIds[i] : ""; }
const char* stockRangeLabel(int i) { return i >= 0 && i < STOCK_RANGES ? kRangeLabels[i] : ""; }

const StockData& stockData() { return s_rows ? *s_rows : kNoRows; }
const StockChart& stockChart() { return s_chart ? *s_chart : kNoChart; }

bool stockFresh() { return s_rows && s_rows->rxMs && millis() - s_rows->rxMs < STOCK_STALE_MS; }

bool stockApply(const String& json, IPAddress from, String& err) {
  JsonDocument doc(psramJson());
  if (deserializeJson(doc, json)) {
    err = "bad JSON";
    return false;
  }
  const PcSource* pc = sender(doc, from, err);
  if (!pc) return false;
  // Built aside (not on the web server's stack), then copied over.
  StockData* d = (StockData*)heap_caps_calloc(1, sizeof(StockData), MALLOC_CAP_SPIRAM);
  if (!psramStruct(s_rows) || !d) {
    heap_caps_free(d);
    err = "out of memory";
    return false;
  }
  strlcpy(d->pcId, pc->id, sizeof(d->pcId));
  strlcpy(d->pcName, pc->name, sizeof(d->pcName));
  copy(d->err, sizeof(d->err), doc["err"]);
  // [sym, name, cur, state, dec, when, price, prev, lo, hi, [spark], err, span]
  for (JsonArrayConst r : doc["rows"].as<JsonArrayConst>()) {
    if (d->count >= STOCK_MAX) break;
    StockRow& s = d->rows[d->count];
    copy(s.sym, sizeof(s.sym), r[0]);
    if (!s.sym[0]) continue;
    d->count++;
    copy(s.name, sizeof(s.name), r[1]);
    copy(s.cur, sizeof(s.cur), r[2]);
    const char* st = r[3] | "c";
    s.state = strchr("opac", st[0]) ? st[0] : 'c';
    s.dec = (uint8_t)constrain(r[4] | 2, 0, 6);
    copy(s.when, sizeof(s.when), r[5]);
    s.price = r[6].is<double>() ? r[6].as<double>() : NAN;
    s.prev = r[7].is<double>() ? r[7].as<double>() : NAN;
    s.lo = r[8] | 0.0f;
    s.hi = r[9] | 0.0f;
    for (JsonVariantConst v : r[10].as<JsonArrayConst>()) {
      if (s.n >= STOCK_SPARK) break;
      s.spark[s.n++] = (uint8_t)constrain(v | 0, 0, 255);
    }
    s.err = (r[11] | 0) != 0;
    s.span = constrain(r[12] | 1.0f, 0.05f, 1.0f);
  }
  // Same content as before: only the time moves (the agent repeats it every minute).
  d->seq = s_rows->seq;
  d->rxMs = s_rows->rxMs;
  if (memcmp(d, s_rows, sizeof(StockData))) d->seq++;
  d->rxMs = millis();
  if (!d->rxMs) d->rxMs = 1;
  *s_rows = *d;
  heap_caps_free(d);
  return true;
}

bool stockChartApply(const String& json, IPAddress from, String& err) {
  JsonDocument doc(psramJson());
  if (deserializeJson(doc, json)) {
    err = "bad JSON";
    return false;
  }
  if (!sender(doc, from, err)) return false;
  if (!psramStruct(s_chart)) {
    err = "out of memory";
    return false;
  }
  StockChart c = {};
  copy(c.sym, sizeof(c.sym), doc["sym"]);
  copy(c.range, sizeof(c.range), doc["range"]);
  copy(c.err, sizeof(c.err), doc["err"]);
  c.dec = (uint8_t)constrain(doc["dec"] | 2, 0, 6);
  c.base = doc["base"].is<double>() ? doc["base"].as<double>() : NAN;
  c.prevClose = doc["prev_close"] | false;
  for (JsonVariantConst v : doc["p"].as<JsonArrayConst>()) {
    if (c.n >= STOCK_CHART_MAX) break;
    if (!v.is<float>()) continue;  // a gap: the line just joins its neighbours
    c.p[c.n++] = v.as<float>();
  }
  // [[at, "09:00"], ...]
  for (JsonArrayConst l : doc["labels"].as<JsonArrayConst>()) {
    if (c.labelCount >= STOCK_LABELS) break;
    StockLabel& sl = c.labels[c.labelCount++];
    sl.at = constrain(l[0] | 0.0f, 0.0f, 1.0f);
    copy(sl.text, sizeof(sl.text), l[1]);
  }
  c.seq = s_chart->seq + 1;
  c.rxMs = millis();
  if (!c.rxMs) c.rxMs = 1;
  *s_chart = c;
  return true;
}

bool stockAskChart(const char* sym, const char* range, String& err) {
  if (!s_rows || !s_rows->pcId[0] || !sym || !sym[0]) {
    err = "no PC for the Stocks app";
    return false;
  }
  char cmd[40];
  snprintf(cmd, sizeof(cmd), "stock:%s:%s", sym, range);
  return pcSendCommand(s_rows->pcId, cmd, err);
}
