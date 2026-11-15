import type { Column, Form, GitLabForm, State, TestResult } from "../api";
import { sourceView } from "./Overview";
import { Card, Dot, Field, ListInput, Switch, TokenInput } from "./ui";

const DEFAULT_COLUMNS: Column[] = [
  { label: "TO DO", jql: 'assignee = currentUser() AND statusCategory = "To Do" AND resolution = Unresolved', color: "#E6E9EE" },
  { label: "IN PROG", jql: 'assignee = currentUser() AND statusCategory = "In Progress"', color: "#58A6FF" },
  { label: "DONE", jql: "assignee = currentUser() AND statusCategory = Done AND updated >= -14d", color: "#3FB950" },
];
const MAX_COLS = 6;

type TestProps = { onTest: () => void; testing: boolean; result?: TestResult };

function StatusLine({ state, name }: { state: State; name: "gitlab" | "jira" }) {
  const v = sourceView(state, name);
  return (
    <p className="status-line">
      <Dot tone={v.tone} /> <strong className={`t-${v.tone}`}>{v.title}</strong>
      {v.detail && <span className="muted"> · {v.detail}</span>}
    </p>
  );
}

function TestBox({ onTest, testing, result, can, render }: TestProps & {
  can: boolean;
  render: (r: Exclude<TestResult, { err: string }>) => string;
}) {
  return (
    <div className="test">
      <button type="button" className="btn" onClick={onTest} disabled={testing || !can}>
        {testing ? <><span className="spinner" /> Testing…</> : "Test connection"}
      </button>
      {!can && <span className="muted small">Needs a URL and a token (typed or saved).</span>}
      {result && "err" in result && <span className="test-out t-bad">✕ {result.err}</span>}
      {result && !("err" in result) && <span className="test-out t-ok">✓ {render(result)}</span>}
      {result && <span className="muted small">Nothing saved.</span>}
    </div>
  );
}

export function GitLabPage({ state, form, set, ...test }: {
  state: State;
  form: GitLabForm;
  set: (f: GitLabForm) => void;
} & TestProps) {
  const saved = state.config.gitlab.token_set;
  const sameUrl = (state.config.gitlab.url ?? "") === form.url.replace(/\/+$/, "");
  const tokenKept = saved && sameUrl && !form.clear_token;
  return (
    <>
      <Card title="Connection" aside={<Switch label="On" checked={form.enabled} onChange={(enabled) => set({ ...form, enabled })} />}>
        <StatusLine state={state} name="gitlab" />
        <div className={`grid ${form.enabled ? "" : "is-off"}`}>
          <Field label="Server URL" full hint="gitlab.com or your company's server.">
            {(id) => (
              <input id={id} type="url" value={form.url} placeholder="https://gitlab.example.com" spellCheck={false}
                autoComplete="off" onChange={(e) => set({ ...form, url: e.target.value })} />
            )}
          </Field>
          <Field label="Personal access token" full
            hint={<>Preferences → Access tokens, scope <code>read_api</code>. Changing the URL drops the saved token.</>}>
            {(id) => (
              <TokenInput id={id} value={form.token} saved={tokenKept} cleared={form.clear_token} placeholder="glpat-…"
                onChange={(token) => set({ ...form, token })} onForget={(clear_token) => set({ ...form, clear_token })} />
            )}
          </Field>
        </div>
        <TestBox {...test} can={!!form.url && (!!form.token || tokenKept)}
          render={(r) => "mine" in r
            ? `${r.active} active (${r.review} to review, ${r.mine} yours) · ${r.todos} to-dos · ${r.pipelines} pipelines${r.ko ? ` (${r.ko} failing)` : ""}`
            : ""} />
      </Card>
      <Card title="What the deck shows">
        <div className={`grid ${form.enabled ? "" : "is-off"}`}>
          <Field label="Extra projects" hint="One per line, e.g. group/shop-api. Your open MRs' projects are always added.">
            {(id) => <ListInput id={id} value={form.projects} placeholder="group/project" onChange={(projects) => set({ ...form, projects })} />}
          </Field>
          <Field label="Branches with pipelines" hint="Their latest pipeline shows besides your MRs' branches.">
            {(id) => <ListInput id={id} value={form.branches} placeholder={"main\ndevelop"} onChange={(branches) => set({ ...form, branches })} />}
          </Field>
          <Field label="Poll every" hint="Seconds, 30 or more.">
            {(id) => (
              <div className="suffix">
                <input id={id} type="number" min={30} max={3600} value={form.every_s ?? ""} placeholder="60"
                  onChange={(e) => set({ ...form, every_s: e.target.value === "" ? null : Number(e.target.value) })} />
                <span>s</span>
              </div>
            )}
          </Field>
        </div>
      </Card>
    </>
  );
}

function ColumnsEditor({ cols, set }: { cols: Column[]; set: (c: Column[]) => void }) {
  const upd = (i: number, c: Partial<Column>) => set(cols.map((x, k) => (k === i ? { ...x, ...c } : x)));
  const move = (i: number, d: number) => {
    const next = [...cols];
    [next[i], next[i + d]] = [next[i + d], next[i]];
    set(next);
  };
  if (!cols.length)
    return (
      <div className="empty">
        <p className="muted">
          No filters of your own: the deck shows <strong>To Do</strong>, <strong>In Progress</strong> and{" "}
          <strong>Done</strong> (last 14 days) for tickets assigned to you.
        </p>
        <div className="row-actions">
          <button type="button" className="btn" onClick={() => set(DEFAULT_COLUMNS.map((c) => ({ ...c })))}>Start from these</button>
          <button type="button" className="btn" onClick={() => set([{ label: "", jql: "", color: "#E6E9EE" }])}>Add a filter</button>
        </div>
      </div>
    );
  return (
    <div className="cols">
      {cols.map((c, i) => (
        <div className="col" key={i}>
          <input className="col-color" type="color" value={c.color.toLowerCase()} aria-label="Color"
            onChange={(e) => upd(i, { color: e.target.value.toUpperCase() })} />
          <input className="col-label" type="text" value={c.label} maxLength={11} placeholder="LABEL" aria-label="Label"
            onChange={(e) => upd(i, { label: e.target.value })} />
          <input className="col-jql mono" type="text" value={c.jql} placeholder='assignee = currentUser() AND status = "Review"'
            aria-label="JQL" spellCheck={false} onChange={(e) => upd(i, { jql: e.target.value })} />
          <div className="col-actions">
            <button type="button" className="icon-btn" disabled={i === 0} onClick={() => move(i, -1)} aria-label="Move up">↑</button>
            <button type="button" className="icon-btn" disabled={i === cols.length - 1} onClick={() => move(i, 1)} aria-label="Move down">↓</button>
            <button type="button" className="icon-btn danger" onClick={() => set(cols.filter((_, k) => k !== i))} aria-label="Remove">✕</button>
          </div>
        </div>
      ))}
      <div className="row-actions">
        <button type="button" className="btn btn-sm" disabled={cols.length >= MAX_COLS}
          onClick={() => set([...cols, { label: "", jql: "", color: "#E6E9EE" }])}>
          Add filter
        </button>
        <span className="muted small">{cols.length} of {MAX_COLS}</span>
      </div>
    </div>
  );
}

function ResultLine({ label, result, render }: {
  label: string;
  result?: TestResult;
  render: (r: Exclude<TestResult, { err: string }>) => string;
}) {
  if (!result) return null;
  return "err" in result ? (
    <span className="test-out t-bad">✕ {label}: {result.err}</span>
  ) : (
    <span className="test-out t-ok">✓ {label}: {render(result)}</span>
  );
}

const viaGateway = (r: TestResult) => ("gateway" in r && r.gateway ? " (via api.atlassian.com)" : "");

export function AtlassianPage({ state, form, set, onTest, testing, results }: {
  state: State;
  form: Form;
  set: (f: Form) => void;
  onTest: () => void;
  testing: boolean;
  results: { jira?: TestResult; confluence?: TestResult };
}) {
  const acc = form.atlassian;
  const jira = form.jira;
  const setAcc = (a: Partial<Form["atlassian"]>) => set({ ...form, atlassian: { ...acc, ...a } });
  const setJira = (j: Partial<Form["jira"]>) => set({ ...form, jira: { ...jira, ...j } });
  const saved = state.config.atlassian.token_set;
  const sameUrl = (state.config.atlassian.url ?? "") === acc.url.replace(/\/+$/, "");
  const tokenKept = saved && sameUrl && !acc.clear_token;
  const can = !!acc.url && (!!acc.token || tokenKept) && (jira.enabled || form.confluence.enabled);
  return (
    <>
      <Card title="Account">
        <p className="muted small account-note">One account for Jira and Confluence.</p>
        <div className="grid">
          <Field label="Site URL" full>
            {(id) => (
              <input id={id} type="url" value={acc.url} placeholder="https://yourcompany.atlassian.net" spellCheck={false}
                autoComplete="off" onChange={(e) => setAcc({ url: e.target.value })} />
            )}
          </Field>
          <Field label="Email" hint="Atlassian Cloud. Leave empty for Server / Data Center.">
            {(id) => (
              <input id={id} type="email" value={acc.email} placeholder="you@company.com" spellCheck={false}
                autoComplete="off" onChange={(e) => setAcc({ email: e.target.value })} />
            )}
          </Field>
          <Field label="API token"
            hint={<>id.atlassian.com → Security → API tokens. A token <em>without scopes</em> covers both apps; changing the URL drops the saved one.</>}>
            {(id) => (
              <TokenInput id={id} value={acc.token} saved={tokenKept} cleared={acc.clear_token} placeholder="ATATT…"
                onChange={(token) => setAcc({ token })} onForget={(clear_token) => setAcc({ clear_token })} />
            )}
          </Field>
        </div>
        <div className="test">
          <button type="button" className="btn" onClick={onTest} disabled={testing || !can}>
            {testing ? <><span className="spinner" /> Testing…</> : "Test connection"}
          </button>
          {!can && <span className="muted small">Needs a URL, a token (typed or saved) and Jira or Confluence on.</span>}
          <div className="test-lines">
            <ResultLine label="Jira" result={results.jira}
              render={(r) => "cols" in r ? `${r.cols.map(([l, n]) => `${l} ${n}`).join(" · ")} · ${r.tickets} tickets${viaGateway(r)}` : ""} />
            <ResultLine label="Confluence" result={results.confluence}
              render={(r) => "user" in r ? `signed in as ${r.user}${viaGateway(r)}` : ""} />
          </div>
          {(results.jira || results.confluence) && <span className="muted small">Nothing saved.</span>}
        </div>
      </Card>

      <Card title="Jira" aside={<Switch label="On" checked={jira.enabled} onChange={(enabled) => setJira({ enabled })} />}>
        <StatusLine state={state} name="jira" />
        <div className={jira.enabled ? "" : "is-off"}>
          <h3 className="sub-title">Filters <span className="muted small">each one a section of the Work app, with its count and first tickets, and a tile (Jira filter 1, 2…)</span></h3>
          <ColumnsEditor cols={jira.columns} set={(columns) => setJira({ columns })} />
          <div className="grid sub-grid">
            <Field label="Poll every" hint="Seconds, 60 or more.">
              {(id) => (
                <div className="suffix">
                  <input id={id} type="number" min={60} max={3600} value={jira.every_s ?? ""} placeholder="120"
                    onChange={(e) => setJira({ every_s: e.target.value === "" ? null : Number(e.target.value) })} />
                  <span>s</span>
                </div>
              )}
            </Field>
          </div>
        </div>
      </Card>

      <Card title="Confluence"
        aside={<Switch label="On" checked={form.confluence.enabled} onChange={(enabled) => set({ ...form, confluence: { enabled } })} />}>
        <p className="muted">
          Uses the account above. Nothing on the deck shows Confluence yet: with it on, <strong>Test connection</strong> also
          checks that the token can read Confluence.
        </p>
      </Card>
    </>
  );
}
