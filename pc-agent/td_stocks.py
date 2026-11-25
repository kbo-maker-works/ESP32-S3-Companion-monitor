#!/usr/bin/env python3
"""
Stock quotes for the deck's Stocks app and widget: the symbols you pin on the
settings page (Stocks), from Yahoo Finance's public API (no account, no key),
pushed to the deck:

    POST http://<deck>/api/pc/stocks         {"pc_id", "pc_name", "err", "rows": [...]}

The prices of every pinned symbol come in ONE request (Yahoo's "spark"), every
15 s while a market is open, every 2 minutes while all are closed. Names,
currencies and trading hours come from each symbol's chart, every 30 minutes.

A tap on a row asks for its chart (the signed deck command "stock:<sym>:<range>"),
fetched here and pushed back:

    POST http://<deck>/api/pc/stocks/chart   {"pc_id", "sym", "range", "p": [...], "labels": [...], ...}

Standard library only. touchdeck_agent.py starts it; with nothing pinned it
only waits. ~/.touchdeck/stocks.json (the settings page writes it):

{
  "symbols": [{"sym": "SAN.MC", "name": "Banco Santander"}, {"sym": "^IBEX", "name": "IBEX 35"}],
  "every_s": 15                 # the prices while a market is open (10 .. 300)
}

Symbols are Yahoo Finance's: AAPL, SAN.MC (Madrid), ^IBEX, BTC-USD, EURUSD=X...
Yahoo gives some exchanges' quotes with a delay (its own rules, per exchange).

Try it without the deck:   python td_stocks.py --once SAN.MC AAPL
                           python td_stocks.py --chart SAN.MC 1mo
                           python td_stocks.py --search santander
"""
import argparse
import datetime
import json
import os
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

CONFIG_FILE = os.path.join(os.path.expanduser("~"), ".touchdeck", "stocks.json")
PUSH_EVERY_S = 60     # also when nothing changed, so the deck knows the data is current
FAST_S = 15           # the prices while a market is open (stocks.json "every_s")
IDLE_S = 120          # ...and while every market is closed
META_S = 1800         # a symbol's name, currency and trading hours
TIMEOUT_S = 15
UA = "Mozilla/5.0 (compatible; CompaDeck agent)"
CHART_URL = "https://query1.finance.yahoo.com/v8/finance/chart/%s"
SPARK_URL = "https://query1.finance.yahoo.com/v8/finance/spark"
SEARCH_URL = "https://query2.finance.yahoo.com/v1/finance/search"

# Deck field sizes in bytes, terminator included (src/services/stock_data.h).
MAX_SYMBOLS, MAX_SYM, MAX_NAME, MAX_CUR, MAX_WHEN, MAX_ERR, MAX_LABEL = 12, 15, 39, 3, 23, 39, 7
SPARK, CHART_MAX, MAX_LABELS = 48, 160, 6
SYMBOL = re.compile(r"^[A-Za-z0-9.^=\-]{1,15}$")

# Deck range -> Yahoo's range and interval.
RANGES = {"1d": ("1d", "5m"), "5d": ("5d", "15m"), "1mo": ("1mo", "60m"),
          "6mo": ("6mo", "1d"), "1y": ("1y", "1d"), "5y": ("5y", "1wk")}
SEARCH_TYPES = {"EQUITY", "ETF", "INDEX", "CRYPTOCURRENCY", "MUTUALFUND", "CURRENCY", "FUTURE"}


def clip(text, max_bytes):
    b = str(text or "").encode("utf-8")[:max_bytes]
    return b.decode("utf-8", "ignore")


def http_error(e):
    if isinstance(e, urllib.error.HTTPError):
        return "Yahoo: HTTP %d" % e.code
    if isinstance(e, urllib.error.URLError):
        return "Yahoo: %s" % clip(e.reason, 30)
    return clip(str(e), MAX_ERR)


def get_json(url, params):
    req = urllib.request.Request(url + "?" + urllib.parse.urlencode(params), headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
        return json.loads(r.read().decode("utf-8"))


def chart(sym, rng, interval):
    """Yahoo's chart for sym: (meta, [(t, close)]) with the gaps left out."""
    data = get_json(CHART_URL % urllib.parse.quote(sym, safe=""), {"range": rng, "interval": interval})
    res = (data.get("chart") or {}).get("result") or []
    if not res:
        err = (data.get("chart") or {}).get("error") or {}
        raise ValueError(clip(err.get("description") or "unknown symbol", MAX_ERR))
    r = res[0]
    closes = (((r.get("indicators") or {}).get("quote") or [{}])[0]).get("close") or []
    pts = [(t, c) for t, c in zip(r.get("timestamp") or [], closes) if c is not None]
    return r.get("meta") or {}, pts


def chart_series(meta, pts):
    """A chart's day as spark_series gives it."""
    reg = (meta.get("currentTradingPeriod") or {}).get("regular") or {}
    return {"pts": pts, "prev": meta.get("chartPreviousClose") or meta.get("previousClose"),
            "start": reg.get("start") or 0, "end": reg.get("end") or 0}


def spark_series(symbols):
    """Today's 5 minute closes of every symbol in one request:
    {sym: {pts: [(t, close)], prev, start, end}} (start/end: the regular session)."""
    data = get_json(SPARK_URL, {"symbols": ",".join(symbols), "range": "1d", "interval": "5m"})
    out = {}
    for sym in symbols:
        x = data.get(sym)
        if not isinstance(x, dict):
            continue
        pts = [(t, c) for t, c in zip(x.get("timestamp") or [], x.get("close") or []) if c is not None]
        out[sym] = {"pts": pts, "prev": x.get("chartPreviousClose") or x.get("previousClose"),
                    "start": x.get("start") or 0, "end": x.get("end") or 0}
    return out


def search(q):
    """Yahoo's symbol search: [{sym, name, exch, type}], quotes only."""
    data = get_json(SEARCH_URL, {"q": q, "quotesCount": 10, "newsCount": 0, "listsCount": 0})
    out = []
    for x in data.get("quotes") or []:
        sym = x.get("symbol") or ""
        if x.get("quoteType") not in SEARCH_TYPES or not SYMBOL.match(sym):
            continue
        out.append({"sym": sym, "name": x.get("longname") or x.get("shortname") or sym,
                    "exch": x.get("exchDisp") or x.get("exchange") or "", "type": x.get("typeDisp") or ""})
    return out


def pick(points, n):
    """At most n of the points, evenly spread, the first and last kept."""
    if len(points) <= n:
        return list(points)
    return [points[round(i * (len(points) - 1) / (n - 1))] for i in range(n)]


def decimals(meta, price):
    hint = meta.get("priceHint")
    if isinstance(hint, int) and 0 <= hint <= 6:
        return hint
    return 4 if price and abs(price) < 1 else 2


def local(t, meta):
    """The exchange's wall clock at epoch t."""
    return datetime.datetime.fromtimestamp(t + int(meta.get("gmtoffset") or 0), datetime.timezone.utc)


def market_state(meta, ser, now):
    """'o' open, 'p' pre-market, 'a' after hours, 'c' closed: from the trading
    hours when they are today's, else from the day's session the prices came with."""
    tp = meta.get("currentTradingPeriod") or {}
    for key, st in (("regular", "o"), ("pre", "p"), ("post", "a")):
        p = tp.get(key) or {}
        if p.get("start") and p.get("end") and p["start"] <= now < p["end"]:
            return st
    if ser and ser.get("start") and ser.get("end") and ser["start"] <= now < ser["end"]:
        return "o"
    return "c"


def when_text(state, meta, t):
    if not t:
        return ""
    d = local(t, meta)
    tz = meta.get("timezone") or ""
    if state == "o":
        text = "Open · %s %s" % (d.strftime("%H:%M"), tz)
    elif state == "p":
        text = "Pre-market · %s" % d.strftime("%H:%M")
    elif state == "a":
        text = "After hours · %s" % d.strftime("%H:%M")
    else:
        text = "Closed · %s %s" % (d.strftime("%a %H:%M"), tz)
    return clip(text.strip(), MAX_WHEN)


def build_row(sym, name, meta, ser, now, stale=False):
    """One row for the deck: [sym, name, cur, state, dec, when, price, prev, lo, hi, [spark], err, span]."""
    ser = ser or {}
    pts = ser.get("pts") or []
    state = market_state(meta, ser, now)
    # Open: the last trade. Otherwise the official close, once the chart has it.
    price = pts[-1][1] if state == "o" and pts else meta.get("regularMarketPrice")
    if price is None and pts:
        price = pts[-1][1]
    prev = ser.get("prev") or meta.get("chartPreviousClose") or meta.get("previousClose")
    dec = decimals(meta, price)
    line = [c for _, c in pick(pts, SPARK)]
    vals = line + ([prev] if prev else [])
    lo, hi = (min(vals), max(vals)) if vals else (0.0, 0.0)
    spark = [round((c - lo) / (hi - lo) * 255) for c in line] if hi > lo else [128] * len(line)
    span = 1.0
    start, end = ser.get("start") or 0, ser.get("end") or 0
    if state == "o" and pts and end > start:  # the line covers the session so far
        span = min(1.0, max(0.05, (pts[-1][0] - start) / (end - start)))
    t = pts[-1][0] if state == "o" and pts else meta.get("regularMarketTime") or (pts[-1][0] if pts else 0)
    full = name or meta.get("longName") or meta.get("shortName") or sym
    r = lambda v: round(v, dec + 2) if v is not None else None  # noqa: E731
    return [sym, clip(full, MAX_NAME), clip(meta.get("currency") or "", MAX_CUR), state, dec,
            when_text(state, meta, t), r(price), r(prev), r(lo), r(hi), spark, 1 if stale else 0, round(span, 3)]


def quote_row(sym, name=""):
    meta, pts = chart(sym, "1d", "5m")
    return build_row(sym, name, meta, chart_series(meta, pts), time.time())


def label_key(rng, d):
    """When the label under a chart changes, and its text."""
    if rng == "1d":
        return d.hour, d.strftime("%H:00")
    if rng == "5d":
        return d.date(), d.strftime("%a %d")
    if rng == "1mo":
        return d.date(), d.strftime("%b %d").replace(" 0", " ")
    if rng in ("6mo", "1y"):
        return (d.year, d.month), d.strftime("%b")
    return d.year, d.strftime("%Y")


def chart_payload(sym, rng):
    """The deck's chart: {sym, range, dec, base, prev_close, p, labels, err}."""
    yr, interval = RANGES[rng]
    meta, pts = chart(sym, yr, interval)
    pts = pick(pts, CHART_MAX)
    if len(pts) < 2:
        return {"sym": sym, "range": rng, "err": "No data for this range"}
    price = meta.get("regularMarketPrice") or pts[-1][1]
    dec = decimals(meta, price)
    prev = meta.get("chartPreviousClose") or meta.get("previousClose")
    one_day = rng == "1d" and prev
    labels, last_key, last_at = [], None, -1.0
    for i, (t, _) in enumerate(pts):
        key, text = label_key(rng, local(t, meta))
        if key == last_key:
            continue
        first = last_key is None
        last_key = key
        at = i / (len(pts) - 1)
        if first and at < 0.02 and rng != "1d":  # the start of the range is no boundary
            continue
        if at - last_at < 0.16 or at > 0.97 or len(labels) >= MAX_LABELS:
            continue
        labels.append([round(at, 3), clip(text, MAX_LABEL)])
        last_at = at
    return {"sym": sym, "range": rng, "dec": dec, "base": round(prev if one_day else pts[0][1], dec + 2),
            "prev_close": bool(one_day), "p": [round(c, dec + 2) for _, c in pts], "labels": labels, "err": ""}


# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
def clean_symbols(items):
    """[{sym, name}] valid and without repeats, MAX_SYMBOLS at most."""
    out, seen = [], set()
    for x in items or []:
        if isinstance(x, str):
            x = {"sym": x}
        sym = str((x or {}).get("sym") or "").strip().upper()
        if not SYMBOL.match(sym) or sym in seen:
            continue
        seen.add(sym)
        out.append({"sym": sym, "name": clip(str(x.get("name") or "").strip(), MAX_NAME)})
    return out[:MAX_SYMBOLS]


def load_config(path=CONFIG_FILE):
    try:
        with open(path, encoding="utf-8") as f:
            cfg = json.load(f)
        return cfg if isinstance(cfg, dict) else {}
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        print("stocks: can't read %s: %s" % (path, e), flush=True)
        return {}


def save_config(cfg, path=CONFIG_FILE):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2, ensure_ascii=False)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# The worker
# ---------------------------------------------------------------------------
class Stocks(threading.Thread):
    """Polls the pinned symbols' prices in one request (every `fast` s while a
    market is open, IDLE_S while all are closed), their names and hours one
    symbol at a time (META_S), and pushes the rows to the deck on a change or
    every minute. The deck's "stock:<sym>:<range>" fetches a chart in its own thread."""

    def __init__(self, deck_addr, pc_id, pc_name, verbose=False, path=CONFIG_FILE):
        super().__init__(daemon=True, name="td_stocks")
        self.deck_addr, self.pc_id, self.pc_name, self.verbose, self.path = deck_addr, pc_id, pc_name, verbose, path
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.meta = {}       # sym -> the chart's meta (name, currency, hours)
        self.series = {}     # sym -> the day's prices (spark_series)
        self.meta_due = {}   # sym -> monotonic time to fetch its meta again
        self.states = {}     # sym -> its market's state at the last push
        self.next_fast = 0.0
        self.err = ""
        self.pushed, self.pushed_at = None, 0.0
        self.last_push = {"ok": None, "at": 0.0, "addr": ""}
        self.polled = 0.0
        self.charts = 0      # in flight
        self.configure(load_config(path))

    def configure(self, cfg):
        with self.lock:
            self.symbols = clean_symbols(cfg.get("symbols"))
            every = cfg.get("every_s") or FAST_S
            self.fast = max(10, min(300, int(every))) if isinstance(every, (int, float)) else FAST_S
            keep = {s["sym"] for s in self.symbols}
            for d in (self.meta, self.series, self.states):
                for k in [k for k in d if k not in keep]:
                    del d[k]
            self.meta_due = {s["sym"]: self.meta_due.get(s["sym"], 0.0) for s in self.symbols}
            self.next_fast = 0.0  # the new list's prices now
            self.pushed = None    # and the deck gets it at once
        self.wake.set()

    def set_symbols(self, items):
        """The settings page: saves the pinned list and polls the new ones now."""
        cfg = load_config(self.path)
        cfg["symbols"] = clean_symbols(items)
        save_config(cfg, self.path)
        self.configure(cfg)
        return self.status()

    def run(self):
        failing = None
        while True:
            try:
                self._tick()
            except Exception as e:  # one bad poll must not stop the quotes for good
                if str(e) != failing:
                    print("stocks: %s (logged once until it changes)" % e, flush=True)
                    failing = str(e)
            else:
                failing = None
            self.wake.wait(2)
            self.wake.clear()

    def _tick(self):
        with self.lock:
            syms = [s["sym"] for s in self.symbols]
            fast_due = time.monotonic() >= self.next_fast
        if not syms:
            self.push()
            return
        if fast_due:
            try:
                got, err = spark_series(syms), ""
            except Exception as e:
                got, err = {}, http_error(e)
                if self.verbose:
                    print("stocks: prices: %s" % err, flush=True)
            now = time.time()
            with self.lock:
                for sym, ser in got.items():
                    if sym in self.meta_due:
                        self.series[sym] = ser
                for sym in syms:  # unknown to spark: its chart brings the prices
                    if not err and sym not in got and sym in self.meta_due:
                        self.meta_due[sym] = 0.0
                self.err = err
                if got:
                    self.polled = now
                live = any(market_state(self.meta.get(s, {}), self.series.get(s), now) in "opa" for s in syms)
                self.next_fast = time.monotonic() + (self.fast if live or err else IDLE_S)
        # Names, currencies and hours: one symbol per tick, the missing ones first.
        with self.lock:
            due = sorted((t, s) for s, t in self.meta_due.items() if time.monotonic() >= t)
        if due:
            sym = due[0][1]
            try:
                meta, pts = chart(sym, "1d", "5m")
            except Exception as e:
                if self.verbose:
                    print("stocks: %s: %s" % (sym, http_error(e)), flush=True)
                with self.lock:
                    if sym in self.meta_due:
                        self.meta_due[sym] = time.monotonic() + 300
            else:
                with self.lock:
                    if sym in self.meta_due:
                        self.meta[sym] = meta
                        if not (self.series.get(sym) or {}).get("pts"):
                            self.series[sym] = chart_series(meta, pts)
                        self.meta_due[sym] = time.monotonic() + META_S
        self.push()

    def _rows(self):
        """The deck's rows, in the pinned order. Lock held."""
        now = time.time()
        rows = []
        for s in self.symbols:
            sym = s["sym"]
            meta, ser = self.meta.get(sym), self.series.get(sym)
            if not meta and not ser:
                continue
            row = build_row(sym, s["name"], meta or {}, ser, now, stale=bool(self.err))
            # The market just closed: fetch its official close soon.
            if self.states.get(sym) == "o" and row[3] != "o":
                self.meta_due[sym] = 0.0
            self.states[sym] = row[3]
            rows.append(row)
        return rows

    def payload(self):
        with self.lock:
            return {"pc_id": self.pc_id, "pc_name": self.pc_name, "err": clip(self.err, MAX_ERR),
                    "rows": self._rows()}

    def status(self):
        """For the settings page: the pinned list with the last quotes, and the last push."""
        with self.lock:
            got = {r[0]: r for r in self._rows()}
            rows = []
            for s in self.symbols:
                r = got.get(s["sym"])
                rows.append({"sym": s["sym"], "name": s["name"] or (r[1] if r else ""),
                             "price": r[6] if r else None, "prev": r[7] if r else None,
                             "cur": r[2] if r else "", "state": r[3] if r else "", "dec": r[4] if r else 2})
            return {"symbols": rows, "err": self.err, "polled": self.polled, "every_s": self.fast,
                    "push": dict(self.last_push)}

    def push(self):
        with self.lock:
            if not self.symbols and not self.last_push["at"]:
                return  # nothing pinned and nothing ever sent: nothing to say
        body = json.dumps(self.payload(), separators=(",", ":"), ensure_ascii=False)
        if body == self.pushed and time.monotonic() - self.pushed_at < PUSH_EVERY_S:
            return
        addr = self.deck_addr()
        if addr == "255.255.255.255":  # the deck's name doesn't resolve yet
            return
        ok = post(addr, "/api/pc/stocks", body, self.verbose)
        with self.lock:
            self.last_push = {"ok": ok, "at": time.time(), "addr": addr}
            if ok:
                self.pushed, self.pushed_at = body, time.monotonic()

    def command(self, rest):
        """The deck's "stock:<sym>:<range>": fetch that chart and push it."""
        sym, _, rng = (rest or "").rpartition(":")
        if not SYMBOL.match(sym) or rng not in RANGES:
            return False, "bad chart request"
        with self.lock:
            if self.charts >= 3:
                return False, "busy, try again"
            self.charts += 1
        threading.Thread(target=self._chart, args=(sym, rng), daemon=True, name="td_stock_chart").start()
        return True, ""

    def _chart(self, sym, rng):
        try:
            try:
                out = chart_payload(sym, rng)
            except Exception as e:
                out = {"sym": sym, "range": rng, "err": http_error(e)}
            out = dict({"pc_id": self.pc_id}, **out)
            addr = self.deck_addr()
            if addr != "255.255.255.255":
                post(addr, "/api/pc/stocks/chart", json.dumps(out, separators=(",", ":")), self.verbose)
        finally:
            with self.lock:
                self.charts -= 1


def post(addr, path, body, verbose=False):
    req = urllib.request.Request("http://%s%s" % (addr, path), data=body.encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            r.read()
        if verbose:
            print("stocks: pushed %d B to %s%s" % (len(body), addr, path), flush=True)
        return True
    except (urllib.error.URLError, OSError) as e:
        if verbose:
            print("stocks: push to %s%s failed: %s" % (addr, path, e), flush=True)
        return False


def main():
    ap = argparse.ArgumentParser(description="CompaDeck stock quotes (Yahoo Finance)")
    ap.add_argument("--once", nargs="*", metavar="SYM", help="print the rows of these symbols (or the pinned ones)")
    ap.add_argument("--chart", nargs=2, metavar=("SYM", "RANGE"), help="print a chart: RANGE = " + ", ".join(RANGES))
    ap.add_argument("--search", metavar="TEXT", help="search Yahoo Finance's symbols")
    args = ap.parse_args()
    if args.search:
        for x in search(args.search):
            print("%-12s %-40s %s %s" % (x["sym"], x["name"][:40], x["exch"], x["type"]))
        return
    if args.chart:
        print(json.dumps(chart_payload(args.chart[0].upper(), args.chart[1]), ensure_ascii=False))
        return
    syms = clean_symbols(args.once) if args.once else clean_symbols(load_config().get("symbols"))
    if not syms:
        raise SystemExit("nothing pinned: python td_stocks.py --once SAN.MC AAPL")
    got = spark_series([s["sym"] for s in syms])
    for s in syms:
        meta, pts = chart(s["sym"], "1d", "5m")
        print(json.dumps(build_row(s["sym"], s["name"], meta, got.get(s["sym"]) or chart_series(meta, pts),
                                   time.time()), ensure_ascii=False))


if __name__ == "__main__":
    main()
