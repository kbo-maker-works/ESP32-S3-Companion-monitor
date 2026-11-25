import { useEffect, useRef, useState } from "react";
import { ago, api, STOCKS_MAX, type PinnedStock, type State, type StockHit, type StockQuote, type StocksState } from "../api";
import { Card, Dot, type Tone } from "./ui";

export function stocksView(s: State): { tone: Tone; title: string; detail: string } {
  const st = s.stocks;
  if (!st) return { tone: "dim", title: "Off", detail: "See the agent's log." };
  if (!st.symbols.length) return { tone: "dim", title: "Nothing pinned", detail: "" };
  if (st.err) return { tone: "warn", title: st.err, detail: "" };
  return { tone: "ok", title: "Updating", detail: `${st.symbols.length} pinned` };
}

const STATE: Record<string, { tone: Tone; text: string }> = {
  o: { tone: "ok", text: "Open" },
  p: { tone: "warn", text: "Pre-market" },
  a: { tone: "warn", text: "After hours" },
  c: { tone: "dim", text: "Closed" },
};

function fmt(v: number, dec: number) {
  return v.toLocaleString("en-US", { minimumFractionDigits: dec, maximumFractionDigits: dec });
}

function Quote({ q }: { q: StockQuote }) {
  if (q.price === null) return <span className="muted">waiting for the first quote…</span>;
  const pct = q.prev ? ((q.price - q.prev) / q.prev) * 100 : null;
  return (
    <span>
      <strong>{fmt(q.price, q.dec)}</strong> <span className="muted">{q.cur}</span>
      {pct !== null && (
        <span className={pct >= 0 ? "t-ok" : "t-bad"}> {pct >= 0 ? "+" : ""}{pct.toFixed(2)}%</span>
      )}
    </span>
  );
}

export function StocksPage({ state, onStocks }: { state: State; onStocks: (s: StocksState) => void }) {
  const st = state.stocks;
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");
  const [q, setQ] = useState("");
  const [hits, setHits] = useState<StockHit[] | null>(null);
  const [searching, setSearching] = useState(false);
  const seq = useRef(0);

  // Search as you type, a moment after the last key.
  useEffect(() => {
    const text = q.trim();
    if (!text) {
      setHits(null);
      return;
    }
    const n = ++seq.current;
    const t = setTimeout(async () => {
      setSearching(true);
      try {
        const { results } = await api.stocks.search(text);
        if (n === seq.current) setHits(results);
      } catch (e) {
        if (n === seq.current) {
          setHits([]);
          setErr((e as Error).message);
        }
      } finally {
        if (n === seq.current) setSearching(false);
      }
    }, 350);
    return () => clearTimeout(t);
  }, [q]);

  if (!st) {
    return <div className="banner bad">Stocks are off on this agent: see the Logs page.</div>;
  }
  const pinned: PinnedStock[] = st.symbols.map(({ sym, name }) => ({ sym, name }));
  const isPinned = new Set(pinned.map((p) => p.sym));

  const save = async (list: PinnedStock[]) => {
    setBusy(true);
    setErr("");
    try {
      onStocks(await api.stocks.pin(list));
    } catch (e) {
      setErr((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const move = (i: number, d: number) => {
    const list = [...pinned];
    const [x] = list.splice(i, 1);
    list.splice(i + d, 0, x);
    save(list);
  };

  return (
    <>
      <Card title={`Pinned on the deck (${pinned.length}/${STOCKS_MAX})`} aside={<Dot tone={stocksView(state).tone} />}>
        <p className="muted">
          The deck's Stocks app and widget show these, in this order. A tap on one there shows its chart. Quotes come
          from Yahoo Finance, every {st.every_s} s while a market is open (one request for all of them); only the
          symbols are sent there. Yahoo gives some exchanges' quotes with a delay.
        </p>
        {pinned.length === 0 ? (
          <p className="muted">Nothing pinned yet: search below.</p>
        ) : (
          <div className="picks">
            {st.symbols.map((s, i) => {
              const m = STATE[s.state];
              return (
                <div className="pick" key={s.sym}>
                  {m ? <Dot tone={m.tone} /> : <Dot tone="dim" />}
                  <div style={{ minWidth: 0 }}>
                    <div className="pick-name">{s.sym} <span className="muted small">{s.name}</span></div>
                    <div className="pick-sub"><Quote q={s} />{m && <span className="muted"> · {m.text}</span>}</div>
                  </div>
                  <div className="row-actions" style={{ marginLeft: "auto", flexWrap: "nowrap" }}>
                    <button type="button" className="icon-btn" disabled={busy || i === 0} aria-label="Move up"
                      onClick={() => move(i, -1)}>↑</button>
                    <button type="button" className="icon-btn" disabled={busy || i === pinned.length - 1}
                      aria-label="Move down" onClick={() => move(i, 1)}>↓</button>
                    <button type="button" className="icon-btn danger" disabled={busy} aria-label={`Unpin ${s.sym}`}
                      onClick={() => save(pinned.filter((p) => p.sym !== s.sym))}>✕</button>
                  </div>
                </div>
              );
            })}
          </div>
        )}
        <p className="small muted">
          {st.polled ? `Updated ${ago(st.polled, state.now)}` : "Not updated yet"}
          {" · "}
          {st.push.ok === null ? "nothing sent to the deck yet"
            : st.push.ok ? `sent to the deck ${ago(st.push.at, state.now)}` : `deck not reachable (${st.push.addr})`}
        </p>
        {st.err && <p className="t-warn">{st.err}</p>}
        {err && <p className="t-bad">✕ {err}</p>}
      </Card>

      <Card title="Add a stock">
        <input type="search" value={q} placeholder="Name or symbol: Santander, AAPL, ^IBEX, BTC-USD…" spellCheck={false}
          onChange={(e) => { setQ(e.target.value); setErr(""); }} aria-label="Search stocks" />
        {searching && <p className="muted"><span className="spinner" /> Searching…</p>}
        {hits && !searching && hits.length === 0 && <p className="muted">Nothing found.</p>}
        {hits && hits.length > 0 && (
          <div className="picks">
            {hits.map((h) => (
              <div className="pick" key={h.sym}>
                <div style={{ minWidth: 0 }}>
                  <div className="pick-name">{h.sym} <span className="muted small">{h.name}</span></div>
                  <div className="pick-sub">{[h.exch, h.type].filter(Boolean).join(" · ")}</div>
                </div>
                {isPinned.has(h.sym) ? (
                  <span className="badge ok">pinned</span>
                ) : (
                  <button type="button" className="btn btn-sm" style={{ marginLeft: "auto" }}
                    disabled={busy || pinned.length >= STOCKS_MAX}
                    onClick={() => save([...pinned, { sym: h.sym, name: h.name }])}>
                    Pin
                  </button>
                )}
              </div>
            ))}
          </div>
        )}
        <p className="field-hint">Yahoo Finance symbols: a suffix says the market (SAN.MC Madrid, BMW.DE Xetra).</p>
      </Card>
    </>
  );
}
