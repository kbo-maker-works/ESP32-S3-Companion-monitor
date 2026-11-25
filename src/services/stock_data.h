// Stock quotes for the Stocks app: the symbols pinned on the PC agent's
// settings page, polled from Yahoo Finance by pc-agent/td_stocks.py and pushed
// here (POST /api/pc/stocks): price, previous close and the day's line for
// each. A tap on a row asks the agent for that symbol's chart over a range
// (signed "stock:<sym>:<range>"), which comes back on POST /api/pc/stocks/chart.
// Field sizes match td_stocks.py's MAX_*.
#ifndef STOCK_DATA_H
#define STOCK_DATA_H

#include <Arduino.h>
#include <IPAddress.h>

constexpr int STOCK_MAX = 12;           // pinned symbols
constexpr int STOCK_SPARK = 48;         // points of a row's day line
constexpr int STOCK_CHART_MAX = 160;    // points of a chart
constexpr int STOCK_LABELS = 6;         // time labels under a chart
constexpr uint32_t STOCK_STALE_MS = 5 * 60 * 1000;  // nothing pushed this long: not current

// Chart ranges, in the agent's names ("1d", "5d", ...), shown as "1D", "5D"...
constexpr int STOCK_RANGES = 6;
const char* stockRangeId(int i);
const char* stockRangeLabel(int i);

struct StockRow {
  char sym[16];      // "SAN.MC", "^IBEX", "BTC-USD"
  char name[40];
  char cur[4];       // "EUR"
  char state;        // 'o' open, 'p' pre-market, 'a' after hours, 'c' closed
  uint8_t dec;       // decimals to show
  char when[24];     // "Closed · Fri 17:35 CET"
  double price, prev;  // prev: the previous close, NAN = unknown
  float lo, hi;      // the day line's range (prev included)
  uint8_t spark[STOCK_SPARK];  // 0 = lo .. 255 = hi
  uint8_t n;         // points in spark
  float span;        // the part of the trading day they cover, 0 .. 1
  bool err;          // its last poll failed: these are older values
};

struct StockData {
  char pcId[17], pcName[24];  // the PC whose agent sent it (and draws the charts)
  char err[40];               // the last poll failed: why
  StockRow rows[STOCK_MAX];
  uint8_t count;
  uint32_t rxMs;  // millis() of the last push, 0 = never
  uint32_t seq;   // bumps when the content changes
};

struct StockLabel {
  float at;  // 0 .. 1 across the chart
  char text[8];
};

struct StockChart {
  char sym[16];
  char range[4];  // as stockRangeId
  uint8_t dec;
  double base;    // the line to compare with: the previous close (1d) or the first point
  bool prevClose; // base is the previous close (drawn dashed)
  float p[STOCK_CHART_MAX];
  uint8_t n;
  StockLabel labels[STOCK_LABELS];
  uint8_t labelCount;
  char err[40];   // the agent could not get it: why
  uint32_t rxMs;  // millis() it arrived, 0 = none yet
  uint32_t seq;
};

const StockData& stockData();
// A push arrived within STOCK_STALE_MS.
bool stockFresh();
// Take a push of the rows, or of a chart. Only from a PC the deck already
// knows, at the address its metrics come from.
bool stockApply(const String& json, IPAddress from, String& err);
bool stockChartApply(const String& json, IPAddress from, String& err);
// The chart last received (its sym and range say which).
const StockChart& stockChart();
// Ask the agent that sent the rows for sym's chart over range (stockRangeId).
// False (err set) when it cannot be asked.
bool stockAskChart(const char* sym, const char* range, String& err);

#endif  // STOCK_DATA_H
