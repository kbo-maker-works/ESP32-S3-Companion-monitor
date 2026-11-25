import { useCallback, useEffect, useRef, useState } from "react";
import { api, toForm, type Form, type SourceName, type State, type TestName, type TestResult } from "./api";
import { LogsPage } from "./components/LogsPage";
import { deckView, Overview, sourceView } from "./components/Overview";
import { bambuView, PrintersPage } from "./components/PrintersPage";
import { AtlassianPage, GitLabPage } from "./components/SourcePages";
import { StocksPage, stocksView } from "./components/StocksPage";
import { Dot, type Tone } from "./components/ui";

const PAGES = [
  { id: "overview", label: "Overview", lede: "What the agent is doing right now." },
  { id: "gitlab", label: "GitLab", lede: "Merge requests, reviews, to-dos and pipelines for the Work app." },
  { id: "atlassian", label: "Atlassian", lede: "One account for Jira (board and tickets on the Work app) and Confluence." },
  { id: "printers", label: "Printers", lede: "Bambu Lab printers through Bambu Cloud, for the deck's printer pages and tiles." },
  { id: "stocks", label: "Stocks", lede: "The stocks the deck's Stocks app follows, with their charts." },
  { id: "logs", label: "Logs", lede: "The agent's output, newest at the bottom." },
] as const;
type Page = (typeof PAGES)[number]["id"];

const pageFromHash = (): Page => {
  const h = location.hash.replace("#", "").replace(/^jira$/, "atlassian");
  return (PAGES.find((p) => p.id === h)?.id ?? "overview") as Page;
};

function overallTone(s: State): { tone: Tone; text: string } {
  const views = [deckView(s), sourceView(s, "gitlab"), sourceView(s, "jira"), bambuView(s)];
  if (views.some((v) => v.tone === "bad")) return { tone: "bad", text: "Needs attention" };
  if (views.some((v) => v.tone === "warn")) return { tone: "warn", text: "Starting" };
  if (views.slice(1).every((v) => v.tone === "dim")) return { tone: "dim", text: "Not set up" };
  return { tone: "ok", text: "Running" };
}

export default function App() {
  const [page, setPage] = useState<Page>(pageFromHash);
  const [state, setState] = useState<State | null>(null);
  const [offline, setOffline] = useState("");
  const [form, setForm] = useState<Form | null>(null);
  const [baseline, setBaseline] = useState("");
  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState<SourceName | null>(null);
  const [results, setResults] = useState<Partial<Record<TestName, TestResult>>>({});
  const [toast, setToast] = useState<{ text: string; tone: Tone } | null>(null);

  const dirty = !!form && JSON.stringify(form) !== baseline;

  // The forms come from the first state that arrives, even if that is a poll
  // after a failed first load (the agent was still starting).
  const loaded = useRef(false);
  const adopt = useCallback((s: State) => {
    loaded.current = true;
    const f = toForm(s);
    setState(s);
    setForm(f);
    setBaseline(JSON.stringify(f));
  }, []);

  useEffect(() => {
    api.state().then(adopt, (e: Error) => setOffline(e.message));
    const t = setInterval(() => {
      api.state().then(
        (s) => {
          if (loaded.current) setState(s);
          else adopt(s);
          setOffline("");
        },
        (e: Error) => setOffline(e.message),
      );
    }, 4000);
    return () => clearInterval(t);
  }, [adopt]);

  useEffect(() => {
    const onHash = () => setPage(pageFromHash());
    window.addEventListener("hashchange", onHash);
    return () => window.removeEventListener("hashchange", onHash);
  }, []);

  useEffect(() => {
    const warn = (e: BeforeUnloadEvent) => {
      if (dirty) e.preventDefault();
    };
    window.addEventListener("beforeunload", warn);
    return () => window.removeEventListener("beforeunload", warn);
  }, [dirty]);

  useEffect(() => {
    if (!toast) return;
    const t = setTimeout(() => setToast(null), 3500);
    return () => clearTimeout(t);
  }, [toast]);

  const go = (p: string) => {
    location.hash = p;
  };

  const save = async () => {
    if (!form) return;
    setSaving(true);
    try {
      adopt(await api.save(form));
      setResults({});
      setToast({ text: "Saved. The agent is using it now.", tone: "ok" });
    } catch (e) {
      setToast({ text: (e as Error).message, tone: "bad" });
    } finally {
      setSaving(false);
    }
  };

  const test = async (name: SourceName) => {
    if (!form) return;
    setTesting(name);
    // GitLab alone, or the Atlassian account: Jira and Confluence as switched on.
    const names: TestName[] = name === "gitlab" ? ["gitlab"] : (["jira", "confluence"] as TestName[]).filter((n) => form[n as "jira" | "confluence"].enabled);
    setResults((r) => ({ ...r, ...Object.fromEntries(names.map((n) => [n, undefined])) }));
    const probe: Form = name === "gitlab"
      ? { ...form, gitlab: { ...form.gitlab, enabled: true }, jira: { ...form.jira, enabled: false }, confluence: { enabled: false } }
      : { ...form, gitlab: { ...form.gitlab, enabled: false } };
    try {
      const { results: got } = await api.test(probe);
      setResults((r) => ({ ...r, ...Object.fromEntries(names.map((n) => [n, got[n] ?? { err: "nothing to test" }])) }));
    } catch (e) {
      const err = { err: (e as Error).message };
      setResults((r) => ({ ...r, ...Object.fromEntries(names.map((n) => [n, err])) }));
    } finally {
      setTesting(null);
    }
  };

  const meta = PAGES.find((p) => p.id === page)!;
  const overall = state ? overallTone(state) : null;

  return (
    <div className="shell">
      <aside className="side">
        <div className="brand">
          <img src="/favicon.svg" alt="" width={28} height={28} />
          <div>
            <div className="brand-name">CompaDeck Agent</div>
            <div className="brand-sub">{state?.pc_name ?? "…"}</div>
          </div>
        </div>
        <nav className="nav">
          {PAGES.map((p) => {
            const tone = !state ? null
              : p.id === "gitlab" || p.id === "atlassian" ? sourceView(state, p.id === "gitlab" ? "gitlab" : "jira").tone
              : p.id === "printers" ? bambuView(state).tone
              : p.id === "stocks" ? stocksView(state).tone
              : null;
            return (
              <a key={p.id} href={`#${p.id}`} className={`nav-item ${page === p.id ? "active" : ""}`}
                aria-current={page === p.id ? "page" : undefined}>
                {p.label}
                {tone && tone !== "dim" && <Dot tone={tone} />}
              </a>
            );
          })}
        </nav>
        {overall && (
          <div className={`side-status t-${offline ? "bad" : overall.tone}`}>
            <Dot tone={offline ? "bad" : overall.tone} /> {offline ? "Agent not answering" : overall.text}
          </div>
        )}
      </aside>

      <main className="main">
        <header className="page-head">
          <h1>{meta.label}</h1>
          <p className="muted">{meta.lede}</p>
        </header>

        {offline && <div className="banner bad">{offline}</div>}
        {!state && !offline && <p className="muted"><span className="spinner" /> Loading…</p>}

        {state && form && page === "overview" && (
          <Overview state={state} go={go} onAwake={(awake) => setState((s) => s && { ...s, awake })} />
        )}
        {state && form && page === "gitlab" && (
          <GitLabPage state={state} form={form.gitlab} set={(gitlab) => setForm({ ...form, gitlab })}
            onTest={() => test("gitlab")} testing={testing === "gitlab"} result={results.gitlab} />
        )}
        {state && form && page === "atlassian" && (
          <AtlassianPage state={state} form={form} set={setForm} onTest={() => test("jira")} testing={testing === "jira"}
            results={{ jira: results.jira, confluence: results.confluence }} />
        )}
        {state && page === "printers" && (
          <PrintersPage state={state} onBambu={(bambu) => setState((s) => s && { ...s, bambu })} />
        )}
        {state && page === "stocks" && (
          <StocksPage state={state} onStocks={(stocks) => setState((s) => s && { ...s, stocks })} />
        )}
        {page === "logs" && <LogsPage />}
      </main>

      <div className={`save-bar ${dirty ? "show" : ""}`} aria-hidden={!dirty}>
        <span className="save-msg"><Dot tone="warn" /> Unsaved changes</span>
        <button type="button" className="btn" disabled={saving} onClick={() => form && setForm(JSON.parse(baseline))}>
          Discard
        </button>
        <button type="button" className="btn btn-accent" disabled={saving} onClick={save}>
          {saving ? <><span className="spinner" /> Saving…</> : "Save"}
        </button>
      </div>

      {toast && <div className={`toast ${toast.tone}`} role="status">{toast.text}</div>}
    </div>
  );
}
