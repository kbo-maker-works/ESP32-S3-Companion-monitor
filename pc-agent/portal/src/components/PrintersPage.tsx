import { useEffect, useState } from "react";
import { ago, api, type BambuDevice, type BambuPrinter, type BambuState, type State } from "../api";
import { Card, Dot, Field, type Tone } from "./ui";

export function bambuView(s: State): { tone: Tone; title: string; detail: string } {
  const b = s.bambu;
  if (!b) return { tone: "dim", title: "Off", detail: "See the agent's log." };
  if (!b.signed_in) return { tone: b.printers.length ? "bad" : "dim", title: "Not signed in", detail: "" };
  const st = b.status;
  if (st.state === "off") return { tone: "dim", title: "No printer picked", detail: "" };
  if (st.state === "signin") return { tone: "bad", title: "Sign in again", detail: st.err };
  if (st.state === "error") return { tone: "bad", title: st.err || "Error", detail: "" };
  if (st.state === "connecting") return { tone: "warn", title: "Connecting", detail: "" };
  return { tone: "ok", title: "Connected", detail: `${st.printers.length} printer${st.printers.length === 1 ? "" : "s"}` };
}

const LINK: Record<string, { tone: Tone; text: string }> = {
  online: { tone: "ok", text: "Reporting" },
  connecting: { tone: "warn", text: "Waiting for its first report" },
  offline: { tone: "dim", text: "Off or not reporting" },
  error: { tone: "bad", text: "Not connected" },
};

function SignIn({ b, busy, run }: { b: BambuState; busy: boolean; run: (f: () => Promise<{ bambu: BambuState }>) => void }) {
  const [region, setRegion] = useState<string>(b.region);
  const [account, setAccount] = useState(b.account);
  const [password, setPassword] = useState("");
  const [code, setCode] = useState("");

  if (b.pending) {
    const email = b.pending.kind === "email";
    return (
      <form onSubmit={(e) => { e.preventDefault(); run(() => api.bambu.code(code)); }}>
        <p className="muted">
          {email ? <>Bambu sent a code to <strong>{b.pending.account}</strong>.</> : <>Enter the code from your authenticator app.</>}
        </p>
        <div className="grid">
          <Field label={email ? "Code sent to your email" : "Code from your authenticator app"}>
            {(id) => (
              <input id={id} type="text" inputMode="numeric" autoComplete="one-time-code" value={code}
                onChange={(e) => setCode(e.target.value.trim())} autoFocus />
            )}
          </Field>
        </div>
        <div className="row-actions">
          <button type="submit" className="btn btn-accent" disabled={busy || code.length < 4}>
            {busy ? <><span className="spinner" /> Verifying…</> : "Verify"}
          </button>
          {email && (
            <button type="button" className="btn" disabled={busy} onClick={() => run(() => api.bambu.resend())}>
              Send it again
            </button>
          )}
          <button type="button" className="btn" disabled={busy} onClick={() => { setCode(""); run(() => api.bambu.restart()); }}>
            Start over
          </button>
        </div>
      </form>
    );
  }

  return (
    <form onSubmit={(e) => { e.preventDefault(); run(() => api.bambu.signin(region, account, password)); setPassword(""); }}>
      <div className="grid">
        <Field label="Region" hint="Global covers Europe and America.">
          {(id) => (
            <select id={id} value={region} onChange={(e) => setRegion(e.target.value)}>
              <option value="global">Global (Europe, America…)</option>
              <option value="cn">China</option>
            </select>
          )}
        </Field>
        <Field label="Email">
          {(id) => (
            <input id={id} type="email" value={account} autoComplete="username" spellCheck={false}
              onChange={(e) => setAccount(e.target.value)} />
          )}
        </Field>
        <Field label="Password" full hint="Sent to Bambu only, never stored. The agent keeps the sign-in, about three months.">
          {(id) => (
            <input id={id} type="password" value={password} autoComplete="current-password"
              onChange={(e) => setPassword(e.target.value)} />
          )}
        </Field>
      </div>
      <div className="row-actions">
        <button type="submit" className="btn btn-accent" disabled={busy || !account.trim() || !password}>
          {busy ? <><span className="spinner" /> Signing in…</> : "Sign in"}
        </button>
      </div>
    </form>
  );
}

function Picker({ b, busy, run }: { b: BambuState; busy: boolean; run: (f: () => Promise<{ bambu: BambuState }>) => void }) {
  const [devices, setDevices] = useState<BambuDevice[] | null>(null);
  const [err, setErr] = useState("");
  const [loading, setLoading] = useState(false);
  const saved = b.printers.map((p) => p.serial).sort().join(",");
  const [picked, setPicked] = useState<string[]>(b.printers.map((p) => p.serial));
  useEffect(() => setPicked(saved ? saved.split(",") : []), [saved]);

  const load = () => {
    setLoading(true);
    setErr("");
    api.bambu.devices().then(
      (r) => setDevices(r.devices),
      (e: Error) => setErr(e.message),
    ).finally(() => setLoading(false));
  };
  useEffect(load, [b.account]);

  const known: BambuPrinter[] = devices ?? b.printers;
  const toggle = (serial: string, on: boolean) =>
    setPicked((p) => (on ? [...p, serial].slice(-2) : p.filter((x) => x !== serial)));
  const dirty = [...picked].sort().join(",") !== saved;
  const save = () => run(() => api.bambu.pick(known.filter((d) => picked.includes(d.serial)).map(({ serial, name, model }) => ({ serial, name, model }))));

  return (
    <>
      {err && <p className="t-bad">✕ {err}</p>}
      {!known.length && !loading && !err && <p className="muted">No printers on this account.</p>}
      <div className="picks">
        {known.map((d) => (
          <label key={d.serial} className="pick">
            <input type="checkbox" checked={picked.includes(d.serial)} onChange={(e) => toggle(d.serial, e.target.checked)} />
            <span>
              <span className="pick-name">{d.name}</span>
              <span className="pick-sub"> · {d.model || "Bambu printer"} · {d.serial}</span>
            </span>
            {"online" in d && <span className={`badge ${(d as BambuDevice).online ? "ok" : ""}`}>{(d as BambuDevice).online ? "online" : "offline"}</span>}
          </label>
        ))}
      </div>
      <div className="row-actions">
        <button type="button" className="btn btn-accent" disabled={busy || !dirty} onClick={save}>Save</button>
        <button type="button" className="btn" disabled={loading} onClick={load}>
          {loading ? <><span className="spinner" /> Loading…</> : "Refresh list"}
        </button>
        <span className="muted small">Two at most, as on the deck.</span>
      </div>
    </>
  );
}

export function PrintersPage({ state, onBambu }: { state: State; onBambu: (b: BambuState) => void }) {
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState("");
  const b = state.bambu;
  if (!b) {
    return <div className="banner bad">Bambu printers are off on this agent: see the Logs page (is paho-mqtt installed?).</div>;
  }
  const run = async (f: () => Promise<{ bambu: BambuState }>) => {
    setBusy(true);
    setErr("");
    try {
      onBambu((await f()).bambu);
    } catch (e) {
      setErr((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const v = bambuView(state);
  const live = b.status.printers;

  return (
    <>
      <Card title="Bambu Cloud account" aside={<Dot tone={v.tone} />}>
        <p className="muted">
          The printer stays in its normal cloud mode: Bambu Handy's notifications, the camera and remote printing keep
          working. Read only: nothing is sent to the printer but status requests.
        </p>
        {b.signed_in ? (
          <>
            <p className="status-line">
              <Dot tone={v.tone} /> <strong className={`t-${v.tone}`}>{v.title}</strong>
              <span className="muted"> · {b.account} · {b.region === "cn" ? "China" : "Global"}</span>
            </p>
            {v.detail && v.tone === "bad" && <p className="t-bad">{v.detail}</p>}
            <div className="row-actions">
              <button type="button" className="btn btn-ghost-danger" disabled={busy} onClick={() => run(() => api.bambu.signout())}>
                Sign out
              </button>
            </div>
          </>
        ) : (
          <SignIn b={b} busy={busy} run={run} />
        )}
        {err && <p className="t-bad">✕ {err}</p>}
      </Card>

      {b.signed_in && (
        <Card title="Printers on the deck">
          <p className="muted">
            Pick the printers this agent sends to the deck, then add each one on the deck's own page:
            Printers → Add printer → Bambu Cloud (through a PC agent).
          </p>
          <Picker b={b} busy={busy} run={run} />
        </Card>
      )}

      {live.length > 0 && (
        <Card title="Now">
          <dl className="facts">
            {live.map((p) => {
              const l = LINK[p.link] ?? LINK.connecting;
              return (
                <div key={p.serial}>
                  <dt><Dot tone={l.tone} /> {p.name}</dt>
                  <dd>
                    {l.text}
                    {p.rx ? ` · ${p.st || "?"}${p.st === "RUNNING" ? ` ${p.pct}%` : ""} · last report ${ago(p.rx, state.now)}` : ""}
                  </dd>
                </div>
              );
            })}
            <div>
              <dt>Deck</dt>
              <dd>
                {b.status.push.ok === null ? "Nothing sent yet" : b.status.push.ok ? `Pushed ${ago(b.status.push.at, state.now)}` : `Not reachable (${b.status.push.addr})`}
              </dd>
            </div>
          </dl>
        </Card>
      )}
    </>
  );
}
