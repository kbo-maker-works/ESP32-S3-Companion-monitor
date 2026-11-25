// Stocks app (app_stocks.cpp): the symbols pinned on the PC agent's settings
// page, one row each with its price, the day's change and line; a tap shows
// its chart over a range picked under the price (stock_data.h,
// pc-agent/td_stocks.py).
#ifndef APP_STOCKS_H
#define APP_STOCKS_H

#include "apps/app.h"
#include "services/stock_data.h"

namespace app {
extern const Def kStocks;

// The next open of the Stocks app (or the open one) shows this symbol's chart;
// "" = the list.
void stocksFocus(const char* sym);

// A row's day line in a: the previous close dashed, the line across the part
// of the session gone, green or red against the previous close (also the
// Stocks widget's).
void stocksDrawDay(lv_layer_t* layer, const lv_area_t& a, const StockRow& r);
// Green when now >= base, red below, dim when either is unknown.
uint32_t stocksMoveColor(double now, double base);
}  // namespace app

#endif  // APP_STOCKS_H
