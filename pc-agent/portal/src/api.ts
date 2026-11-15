// The agent's API (td_portal.py). Every call carries the per-run session the
// agent put in the page's <meta>; in `npm run dev` the Vite proxy adds it.

export type Column = { label: string; jql: string; color: string };

export type GitLabConfig = {
  enabled: boolean;
  url?: string;
  token_set: boolean;
  projects?: string[];
  branches?: string[];
  every_s?: number;
};

export type AtlassianConfig = { url: string; email: string; token_set: boolean };

export type JiraConfig = {
  enabled: boolean;
  columns?: Column[];
  every_s?: number;
};

export type SourceStatus = { polled: number; err: string };

export type State = {
  config: { gitlab: GitLabConfig; atlassian: AtlassianConfig; jira: JiraConfig; confluence: { enabled: boolean } };
  path: string;
  pc_name: string;
  deck: { host: string; addr: string };
  status: null | {
    sources: Partial<Record<SourceName, SourceStatus>>;
    push: { ok: boolean | null; at: number; addr: string };
  };
  awake: AwakeStatus | null;
  started: number;
  now: number;
};

/** Keep awake (td_awake.py): applied at once, not through Save. */
export type AwakeStatus = {
  supported: boolean;
  on: boolean;
  jiggle: boolean;
  can_jiggle: boolean;
  needs_permission: boolean;
  program: string;
  jiggle_after_s: number;
  idle_s: number | null;
  jiggles: number;
  last_jiggle: number | null;
  error: string | null;
};

export type SourceName = "gitlab" | "jira";

export type GitLabForm = {
  enabled: boolean;
  url: string;
  token: string;
  clear_token: boolean;
  projects: string[];
  branches: string[];
  every_s: number | null;
};

export type AtlassianForm = { url: string; email: string; token: string; clear_token: boolean };

export type JiraForm = {
  enabled: boolean;
  columns: Column[];
  every_s: number | null;
};

export type Form = { gitlab: GitLabForm; atlassian: AtlassianForm; jira: JiraForm; confluence: { enabled: boolean } };

/** What a Test returns per source ("confluence" only from the Atlassian test). */
export type TestName = SourceName | "confluence";

export type TestResult =
  | { err: string }
  | { active: number; review: number; mine: number; todos: number; pipelines: number; ko: number }
  | { cols: [string, number][]; tickets: number; gateway: boolean }
  | { user: string; gateway: boolean };

export type Logs = { path: string; lines: string[]; exists: boolean };

const meta = document.querySelector<HTMLMetaElement>('meta[name="td-session"]')?.content ?? "";
const SESSION = meta.startsWith("__") ? "" : meta;

export class ApiError extends Error {}

async function call<T>(path: string, method = "GET", body?: unknown): Promise<T> {
  const headers: Record<string, string> = {};
  if (SESSION) headers["X-TD-Session"] = SESSION;
  if (body !== undefined) headers["Content-Type"] = "application/json";
  let r: Response;
  try {
    r = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body) });
  } catch {
    throw new ApiError("The agent is not answering. Is it running?");
  }
  const data = await r.json().catch(() => ({}));
  // The agent restarted: its session is new, the page gets it on a reload.
  // Once per minute at most, so a real problem shows instead of looping.
  if (r.status === 403 && (data as { detail?: string }).detail === "Reload the page" && SESSION) {
    let last = 0;
    try {
      last = Number(sessionStorage.getItem("td-reload") || 0);
      sessionStorage.setItem("td-reload", String(Date.now()));
    } catch {
      /* storage off: reload anyway */
    }
    if (Date.now() - last > 60_000) location.reload();
  }
  if (!r.ok) {
    const d = (data as { detail?: unknown }).detail;
    const text = Array.isArray(d)
      ? d.map((e: { loc?: unknown[]; msg?: string }) => `${(e.loc ?? []).slice(1).join(".")}: ${e.msg}`).join("; ")
      : typeof d === "string"
        ? d
        : `HTTP ${r.status}`;
    throw new ApiError(text);
  }
  return data as T;
}

export const api = {
  state: () => call<State>("/api/state"),
  save: (form: Form) => call<State>("/api/config", "PUT", form),
  test: (form: Form) => call<{ results: Partial<Record<TestName, TestResult>> }>("/api/test", "POST", form),
  logs: (lines = 300) => call<Logs>(`/api/logs?lines=${lines}`),
  awake: (change: { on?: boolean; jiggle?: boolean }) => call<AwakeStatus>("/api/awake", "PUT", change),
  awakePermission: () => call<{ granted: boolean; awake: AwakeStatus }>("/api/awake/permission", "POST"),
};

export function toForm(s: State): Form {
  const g = s.config.gitlab;
  const a = s.config.atlassian;
  const j = s.config.jira;
  return {
    gitlab: {
      enabled: g.enabled,
      url: g.url ?? "",
      token: "",
      clear_token: false,
      projects: g.projects ?? [],
      branches: g.branches ?? [],
      every_s: g.every_s ?? null,
    },
    atlassian: { url: a.url ?? "", email: a.email ?? "", token: "", clear_token: false },
    jira: {
      enabled: j.enabled,
      columns: (j.columns ?? []).map((c) => ({ ...c })),
      every_s: j.every_s ?? null,
    },
    confluence: { enabled: s.config.confluence.enabled },
  };
}

export function ago(t: number, now: number): string {
  if (!t) return "never";
  const s = Math.max(0, Math.round(now - t));
  if (s < 5) return "just now";
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  if (s < 86400) return `${Math.round(s / 3600)} h ago`;
  return `${Math.round(s / 86400)} d ago`;
}
