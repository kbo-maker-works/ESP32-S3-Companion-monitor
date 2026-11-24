#include "services/work_data.h"
#include "services/pc_metrics.h"
#include "core/psram_json.h"
#include <ArduinoJson.h>

namespace {

// ~10 KB: in PSRAM, allocated by the first push. Until then the empty one (flash).
WorkData* s_work = nullptr;
const WorkData kEmpty = {};

void copy(char* out, size_t n, JsonVariantConst v) { strlcpy(out, v | "", n); }

// "ok" / "warn" / "bad" / "info" / "dim", or already the letter.
char tone(const char* t) {
  if (t[0] && !t[1] && strchr("owbid", t[0])) return t[0];
  if (!strcmp(t, "ok")) return 'o';
  if (!strcmp(t, "warn")) return 'w';
  if (!strcmp(t, "bad")) return 'b';
  if (!strcmp(t, "info")) return 'i';
  return 'd';
}

uint32_t hexColor(const char* s, uint32_t fallback) {
  if (!s || s[0] != '#' || strlen(s) != 7) return fallback;
  char* end = nullptr;
  const uint32_t v = strtoul(s + 1, &end, 16);
  return end && !*end ? v : fallback;
}

const char* const kSectionNames[WORK_SECTIONS] = {
    "Returned to you",  "Review requested", "Your merge requests", "Waiting for author",
    "Waiting for approvals", "Approved by you", "Approved by others"};

}  // namespace

const char* const kVerdictNames[WORK_BR_VERDICTS] = {"Keep", "Review", "Delete"};

const char* workVerdictName(int v) { return v >= 0 && v < WORK_BR_VERDICTS ? kVerdictNames[v] : ""; }

const char* workSectionName(int sec) { return sec >= 0 && sec < WORK_SECTIONS ? kSectionNames[sec] : ""; }

const WorkData& workData() { return s_work ? *s_work : kEmpty; }

bool workFresh() { return s_work && s_work->rxMs && millis() - s_work->rxMs < WORK_STALE_MS; }

int workPipelinesKo() { return workFresh() && s_work->gitlab ? s_work->ko : 0; }

bool workApply(const String& json, IPAddress from, String& err) {
  JsonDocument doc(psramJson());  // up to 24 KB of rows: keep it out of internal RAM
  if (deserializeJson(doc, json)) {
    err = "bad JSON";
    return false;
  }
  const char* id = doc["pc_id"] | "";
  const PcSource* pc = id[0] ? pcSource(id) : nullptr;
  if (!pc || !(pc->ip == from)) {
    err = "unknown PC (its agent must be sending metrics)";
    return false;
  }
  // The Work app has a PC of its own: the others' summaries are not shown.
  const char* own = pcAppMode("work");
  if (own[0] && strcmp(own, pc->id) && pcSource(own)) {
    err = "the Work app shows another PC";
    return false;
  }

  // Built aside in PSRAM (not on the web server's stack), then copied over.
  if (!s_work) s_work = (WorkData*)heap_caps_calloc(1, sizeof(WorkData), MALLOC_CAP_SPIRAM);
  WorkData* w = (WorkData*)heap_caps_calloc(1, sizeof(WorkData), MALLOC_CAP_SPIRAM);
  if (!s_work || !w) {
    heap_caps_free(w);
    err = "out of memory";
    return false;
  }
  strlcpy(w->pcId, pc->id, sizeof(w->pcId));
  strlcpy(w->pcName, pc->name, sizeof(w->pcName));
  JsonObjectConst gl = doc["gitlab"];
  if ((w->gitlab = !gl.isNull())) {
    copy(w->glErr, sizeof(w->glErr), gl["err"]);
    int i = 0;
    for (JsonVariantConst n : gl["sec"].as<JsonArrayConst>())
      if (i < WORK_SECTIONS) w->sec[i++] = n | 0;
    w->active = gl["active"] | 0;
    w->todos = gl["todos"] | 0;
    w->ko = gl["ko"] | 0;
    for (JsonArrayConst r : gl["pipelines"].as<JsonArrayConst>()) {
      if (w->pipelineCount >= WORK_MAX_PIPELINES) break;
      WorkPipeline& p = w->pipelines[w->pipelineCount++];
      copy(p.name, sizeof(p.name), r[0]);
      copy(p.ref, sizeof(p.ref), r[1]);
      p.status = (r[2] | "c")[0];
      copy(p.info, sizeof(p.info), r[3]);
      copy(p.link, sizeof(p.link), r[4]);
    }
    // [sec, ref, title, chip, tone, add, del, pipe, threads, threadsTotal, age, link]
    for (JsonArrayConst r : gl["mrs"].as<JsonArrayConst>()) {
      if (w->mrCount >= WORK_MAX_MRS) break;
      const int sec = r[0] | -1;
      if (sec < 0 || sec >= WORK_SECTIONS) continue;
      WorkMr& m = w->mrs[w->mrCount++];
      m.sec = sec;
      copy(m.ref, sizeof(m.ref), r[1]);
      copy(m.title, sizeof(m.title), r[2]);
      copy(m.chip, sizeof(m.chip), r[3]);
      m.tone = tone(r[4] | "");
      m.add = r[5] | 0;
      m.del = r[6] | 0;
      m.pipe = (r[7] | "")[0];
      m.threads = r[8] | 0;
      m.threadsTotal = r[9] | 0;
      copy(m.age, sizeof(m.age), r[10]);
      copy(m.link, sizeof(m.link), r[11]);
    }
  }
  JsonObjectConst ji = doc["jira"];
  if ((w->jira = !ji.isNull())) {
    copy(w->jiraErr, sizeof(w->jiraErr), ji["err"]);
    // [label, n, color, [[key, title, status, tone, link], ...]]
    for (JsonArrayConst c : ji["cols"].as<JsonArrayConst>()) {
      if (w->colCount >= WORK_MAX_COLS) break;
      const uint8_t ci = w->colCount++;
      WorkCol& col = w->cols[ci];
      copy(col.label, sizeof(col.label), c[0]);
      col.n = c[1] | 0;
      col.color = hexColor(c[2] | "", 0xE6E9EE);
      for (JsonArrayConst t : c[3].as<JsonArrayConst>()) {
        if (w->ticketCount >= WORK_MAX_TICKETS) break;
        WorkTicket& k = w->tickets[w->ticketCount++];
        k.col = ci;
        copy(k.key, sizeof(k.key), t[0]);
        copy(k.title, sizeof(k.title), t[1]);
        copy(k.status, sizeof(k.status), t[2]);
        k.tone = tone(t[3] | "");
        copy(k.link, sizeof(k.link), t[4]);
        col.shown++;
      }
    }
  }

  JsonObjectConst br = doc["branches"];
  if ((w->branches = !br.isNull())) {
    w->brScanning = br["scanning"] | false;
    copy(w->brErr, sizeof(w->brErr), br["err"]);
    w->brAt = br["at"] | 0;
    int i = 0;
    for (JsonVariantConst n : br["n"].as<JsonArrayConst>())
      if (i < WORK_BR_VERDICTS) w->brN[i++] = n | 0;
    // [verdict, state, tone, repo, branch, ticket, mr, reason, age, current, link]
    for (JsonArrayConst r : br["rows"].as<JsonArrayConst>()) {
      if (w->brCount >= WORK_MAX_BRANCHES) break;
      const char* v = r[0] | "k";
      const uint8_t verdict = v[0] == 'd' ? WORK_BR_DELETE : v[0] == 'r' ? WORK_BR_REVIEW : WORK_BR_KEEP;
      WorkBranch& b = w->br[w->brCount++];
      b.verdict = verdict;
      copy(b.state, sizeof(b.state), r[1]);
      b.tone = tone(r[2] | "");
      copy(b.repo, sizeof(b.repo), r[3]);
      copy(b.name, sizeof(b.name), r[4]);
      copy(b.ticket, sizeof(b.ticket), r[5]);
      copy(b.mr, sizeof(b.mr), r[6]);
      copy(b.reason, sizeof(b.reason), r[7]);
      copy(b.age, sizeof(b.age), r[8]);
      b.current = (r[9] | 0) != 0;
      copy(b.link, sizeof(b.link), r[10]);
    }
  }

  // Same content as before: only the time moves (the agent repeats it every minute).
  w->seq = s_work->seq;
  w->rxMs = s_work->rxMs;
  if (memcmp(w, s_work, sizeof(WorkData))) w->seq++;
  w->rxMs = millis();
  *s_work = *w;
  heap_caps_free(w);
  return true;
}

bool workOpen(const char* link, String& err) {
  if (!s_work || !s_work->pcId[0] || !link || !link[0]) {
    err = "no link for this row";
    return false;
  }
  char cmd[24];
  snprintf(cmd, sizeof(cmd), "open:%s", link);
  return pcSendCommand(s_work->pcId, cmd, err);
}

bool workScanBranches(String& err) {
  if (!s_work || !s_work->pcId[0] || !s_work->branches) {
    err = "no branches on this PC's agent";
    return false;
  }
  return pcSendCommand(s_work->pcId, "branches_scan", err);
}
