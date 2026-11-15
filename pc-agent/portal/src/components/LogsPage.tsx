import { useEffect, useRef, useState } from "react";
import { api, type Logs } from "../api";
import { Card } from "./ui";

export function LogsPage() {
  const [logs, setLogs] = useState<Logs | null>(null);
  const [error, setError] = useState("");
  const [follow, setFollow] = useState(true);
  const box = useRef<HTMLPreElement>(null);

  useEffect(() => {
    let alive = true;
    const load = () =>
      api.logs().then(
        (l) => alive && (setLogs(l), setError("")),
        (e: Error) => alive && setError(e.message),
      );
    load();
    const t = setInterval(load, 3000);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);

  useEffect(() => {
    if (follow && box.current) box.current.scrollTop = box.current.scrollHeight;
  }, [logs, follow]);

  return (
    <Card
      title="Agent log"
      aside={
        <label className="check">
          <input type="checkbox" checked={follow} onChange={(e) => setFollow(e.target.checked)} /> Follow
        </label>
      }
    >
      {error && <p className="t-bad">{error}</p>}
      {logs && !logs.exists && (
        <p className="muted">
          No log yet. It is written when the agent runs as a macOS service (<code>macos/install.sh</code>); started
          by hand, the agent prints to its terminal instead.
        </p>
      )}
      {logs && logs.exists && (
        <>
          <pre className="log" ref={box}>{logs.lines.join("\n") || "(empty)"}</pre>
          <p className="field-hint mono">{logs.path}</p>
        </>
      )}
    </Card>
  );
}
