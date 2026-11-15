import { useId, useState, type ReactNode } from "react";

export function Card({ title, aside, children, className = "" }: {
  title?: ReactNode;
  aside?: ReactNode;
  children: ReactNode;
  className?: string;
}) {
  return (
    <section className={`card ${className}`}>
      {(title || aside) && (
        <div className="card-head">
          {title && <h2 className="card-title">{title}</h2>}
          {aside}
        </div>
      )}
      {children}
    </section>
  );
}

export function Field({ label, hint, children, full }: {
  label: ReactNode;
  hint?: ReactNode;
  children: (id: string) => ReactNode;
  full?: boolean;
}) {
  const id = useId();
  return (
    <div className={`field ${full ? "full" : ""}`}>
      <label className="field-label" htmlFor={id}>{label}</label>
      {children(id)}
      {hint && <p className="field-hint">{hint}</p>}
    </div>
  );
}

export function Switch({ checked, onChange, label }: {
  checked: boolean;
  onChange: (v: boolean) => void;
  label: string;
}) {
  return (
    <label className="switch">
      <input type="checkbox" role="switch" checked={checked} onChange={(e) => onChange(e.target.checked)} />
      <span className="switch-track" aria-hidden="true"><span className="switch-thumb" /></span>
      <span className="switch-text">{label}</span>
    </label>
  );
}

/** A token is never shown once saved: the field is empty, its placeholder says
 *  whether one is kept, and Forget marks it for removal on Save. */
export function TokenInput({ id, value, saved, cleared, onChange, onForget, placeholder }: {
  id: string;
  value: string;
  saved: boolean;
  cleared: boolean;
  onChange: (v: string) => void;
  onForget: (v: boolean) => void;
  placeholder: string;
}) {
  const [show, setShow] = useState(false);
  const ph = cleared ? "Removed on save" : saved ? "••••••••••••" : placeholder;
  return (
    <div className="token">
      <div className="token-box">
        <input
          id={id}
          type={show ? "text" : "password"}
          value={value}
          placeholder={ph}
          autoComplete="new-password"
          spellCheck={false}
          onChange={(e) => onChange(e.target.value)}
        />
        {value && (
          <button type="button" className="token-eye" onClick={() => setShow(!show)} aria-label={show ? "Hide" : "Show"}>
            {show ? "Hide" : "Show"}
          </button>
        )}
      </div>
      {saved && !value && (
        <button type="button" className={`btn btn-sm ${cleared ? "" : "btn-ghost-danger"}`} onClick={() => onForget(!cleared)}>
          {cleared ? "Keep" : "Forget"}
        </button>
      )}
      {saved && !cleared && !value && <span className="badge ok">saved</span>}
    </div>
  );
}

export type Tone = "ok" | "bad" | "warn" | "dim";

export function Dot({ tone }: { tone: Tone }) {
  return <span className={`dot ${tone}`} aria-hidden="true" />;
}

export function ListInput({ id, value, onChange, placeholder, rows = 3 }: {
  id: string;
  value: string[];
  onChange: (v: string[]) => void;
  placeholder?: string;
  rows?: number;
}) {
  // Kept as text while typing so blank lines and trailing commas don't vanish.
  const [text, setText] = useState(value.join("\n"));
  const [last, setLast] = useState(value);
  if (value !== last && value.join("\n") !== text.split(/[\n,]/).map((s) => s.trim()).filter(Boolean).join("\n")) {
    setLast(value);
    setText(value.join("\n"));
  }
  return (
    <textarea
      id={id}
      rows={rows}
      value={text}
      placeholder={placeholder}
      spellCheck={false}
      onChange={(e) => {
        setText(e.target.value);
        const items = e.target.value.split(/[\n,]/).map((s) => s.trim()).filter(Boolean);
        setLast(items);
        onChange(items);
      }}
    />
  );
}
