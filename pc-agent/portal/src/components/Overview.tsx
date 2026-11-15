import { useState } from "react";
import { ago, api, type AwakeStatus, type SourceName, type State } from "../api";
import { Card, Dot, Switch, type Tone } from "./ui";

export const SOURCE_LABEL: Record<SourceName, string> = { gitlab: "GitLab", jira: "Atlassian" };

/** URL and saved token of a source: GitLab's own, or the Atlassian account for Jira. */
function account(s: State, name: SourceName) {
  return name === "gitlab" ? s.config.gitlab : s.config.atlassian;
}

export function sourceView(s: State, name: SourceName): { tone: Tone; title: string; detail: string } {
  const acc = account(s, name);
  const st = s.status?.sources[name];
  if (!acc.url || !acc.token_set) return { tone: "dim", title: "Not set up", detail: "Add its URL and token." };
  if (!s.config[name].enabled) return { tone: "dim", title: "Off", detail: "Turned off on its page." };
  if (!st) return { tone: "warn", title: "Starting", detail: "" };
  if (st.err === "connecting...") return { tone: "warn", title: "Connecting", detail: "First poll on its way." };
  if (st.err)
    return { tone: "bad", title: st.err, detail: st.polled ? `Last good poll ${ago(st.polled, s.now)}` : "No good poll yet" };
  return { tone: "ok", title: "Connected", detail: `Polled ${ago(st.polled, s.now)}` };
}

export function deckView(s: State): { tone: Tone; title: string; detail: string } {
  const p = s.status?.push;
  const target = s.deck.addr && s.deck.addr !== s.deck.host ? `${s.deck.host} → ${s.deck.addr}` : s.deck.host;
  if (!s.status) return { tone: "warn", title: "Work updates off", detail: "See the agent's log." };
  if (!p || p.ok === null) return { tone: "dim", title: "Nothing sent yet", detail: target };
  if (p.ok) return { tone: "ok", title: `Pushed ${ago(p.at, s.now)}`, detail: target };
  return { tone: "bad", title: "Deck not reachable", detail: `Last try ${ago(p.at, s.now)} · ${target}` };
}

function Tile({ label, view, onOpen }: {
  label: string;
  view: { tone: Tone; title: string; detail: string };
  onOpen?: () => void;
}) {
  const body = (
    <>
      <span className="tile-label"><Dot tone={view.tone} />{label}</span>
      <span className={`tile-title t-${view.tone}`}>{view.title}</span>
      <span className="tile-detail">{view.detail || " "}</span>
    </>
  );
  return onOpen ? (
    <button type="button" className="tile" onClick={onOpen}>{body}</button>
  ) : (
    <div className="tile">{body}</div>
  );
}

/** Keep awake: on/off and Stay active apply at once (the deck toggles it too). */
function AwakeCard({ state, onAwake }: { state: State; onAwake: (a: AwakeStatus) => void }) {
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");
  const a = state.awake;
  if (!a) return null;
  const run = async (f: () => Promise<AwakeStatus>) => {
    setBusy(true);
    setErr("");
    try {
      onAwake(await f());
    } catch (e) {
      setErr((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const mins = Math.round(a.jiggle_after_s / 60);
  let line = "Off: the computer sleeps and locks as usual.";
  if (a.on)
    line = !a.jiggle || !a.can_jiggle
      ? "On: no sleep and no screen lock from idleness."
      : a.last_jiggle
        ? `On. Nudged the pointer ${a.jiggles} time${a.jiggles === 1 ? "" : "s"}, last ${ago(a.last_jiggle, state.now)}.`
        : "On. No nudge needed yet: you have not been idle long enough.";
  return (
    <Card title="Keep awake" aside={<Dot tone={a.on ? "ok" : "dim"} />}>
      <p className="muted">
        A mouse jiggler in software. Turn it on here or from the deck: a PC command tile set to Keep awake.
      </p>
      <div className="stack">
        <Switch checked={a.on} onChange={(on) => !busy && run(() => api.awake({ on }))} label="Keep awake" />
        <Switch
          checked={a.jiggle}
          onChange={(jiggle) => !busy && run(() => api.awake({ jiggle }))}
          label={`Stay active: after ${mins} min idle, nudge the pointer 1 px so Slack or Teams keep you active`}
        />
      </div>
      <p className={a.error ? "t-bad" : "muted"}>{a.error || line}</p>
      {a.jiggle && a.needs_permission && (
        <div className="callout-inline">
          <p className="t-warn">
            Stay active needs macOS's Accessibility permission for the agent's Python. Without it, Keep awake still
            stops sleep and the screen lock, but chat apps will show you away.
          </p>
          <p className="muted">
            System Settings → Privacy &amp; Security → Accessibility: add and turn on <span className="mono">{a.program}</span>.
            It applies to any script that Python runs.
          </p>
          <div className="row-actions">
            <button type="button" className="btn" disabled={busy}
              onClick={() => run(async () => (await api.awakePermission()).awake)}>
              Ask macOS for it
            </button>
          </div>
        </div>
      )}
      {err && <p className="t-bad">{err}</p>}
    </Card>
  );
}

export function Overview({ state, go, onAwake }: {
  state: State;
  go: (page: string) => void;
  onAwake: (a: AwakeStatus) => void;
}) {
  const fresh = !(["gitlab", "jira"] as SourceName[]).some((n) => account(state, n).url || account(state, n).token_set);
  return (
    <>
      {fresh && (
        <Card className="callout">
          <h2 className="card-title">Set up the Work app</h2>
          <p className="muted">
            The deck's Work app shows your merge requests, pipelines and Jira board. Give the agent a GitLab token and
            an Atlassian one: they stay on this Mac, and only a summary goes to the deck.
          </p>
          <div className="row-actions">
            <button type="button" className="btn btn-accent" onClick={() => go("gitlab")}>Set up GitLab</button>
            <button type="button" className="btn" onClick={() => go("atlassian")}>Set up Atlassian</button>
          </div>
        </Card>
      )}
      <div className="tiles">
        <Tile label="Deck" view={deckView(state)} />
        <Tile label="GitLab" view={sourceView(state, "gitlab")} onOpen={() => go("gitlab")} />
        <Tile label="Atlassian · Jira" view={sourceView(state, "jira")} onOpen={() => go("atlassian")} />
      </div>
      <AwakeCard state={state} onAwake={onAwake} />
      <Card title="This agent">
        <dl className="facts">
          <div><dt>Computer</dt><dd>{state.pc_name}</dd></div>
          <div><dt>Deck</dt><dd>{state.deck.host}</dd></div>
          <div><dt>Running since</dt><dd>{new Date(state.started * 1000).toLocaleString()}</dd></div>
          <div><dt>Settings file</dt><dd className="mono">{state.path}</dd></div>
        </dl>
      </Card>
    </>
  );
}
