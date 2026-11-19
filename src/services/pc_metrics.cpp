// PC stats ingest: a WiFiUDP listener that decodes the companions' V2.1 JSON
// push packets, one PcSource per PC. Parse logic ported from SmallOLED-PCMonitor
// network.cpp parseStatsV2, slimmed to the color project's metric model.
//
// WiFiUDP (not AsyncUDP) on purpose: AsyncUDP froze on ESP32 in the sibling
// projects. The socket is polled from loop() via pcMetricsHandle().
#include "services/pc_metrics.h"
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <vector>
#include "network/improv_setup.h"

PcSource* pcSources = nullptr;

static WiFiUDP udp;
static uint32_t s_version = 1;
static char s_mode[17] = "";  // pcActiveMode(): "" = Auto, else a pc_id
static int s_active = 0;      // pcActive()'s index in pcSources

static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = h[b[i] >> 4];
    out[2 * i + 1] = h[b[i] & 15];
  }
  out[2 * n] = 0;
}

static bool fromHex(const char* s, uint8_t* b, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    char t[3] = {s[2 * i], s[2 * i + 1], 0};
    char* end = nullptr;
    b[i] = (uint8_t)strtoul(t, &end, 16);
    if (!end || *end) return false;
  }
  return true;
}

// Known PCs in NVS: "deckpc"/"list" = [{"id","name"}], first = default.
static void saveKnown() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (int i = 0; i < MAX_PCS; i++) {
    if (!pcSources[i].used) continue;
    JsonObject o = a.add<JsonObject>();
    o["id"] = pcSources[i].id;
    o["name"] = pcSources[i].name;
    if (pcSources[i].paired) {
      char k[65];
      toHex(pcSources[i].key, 32, k);
      o["key"] = k;
    }
  }
  String out;
  serializeJson(doc, out);
  Preferences p;
  if (p.begin("deckpc", false)) {
    p.putString("list", out);
    p.end();
  }
}

static void loadKnown() {
  Preferences p;
  String s;
  if (p.begin("deckpc", true)) {
    s = p.getString("list", "");
    p.end();
  }
  JsonDocument doc;
  if (!s.length() || deserializeJson(doc, s)) return;
  int n = 0;
  for (JsonObject o : doc.as<JsonArray>()) {
    if (n == MAX_PCS) break;
    PcSource& pc = pcSources[n++];
    pc.used = true;
    strlcpy(pc.id, o["id"] | "", sizeof(pc.id));
    strlcpy(pc.name, o["name"] | "PC", sizeof(pc.name));
    pc.paired = fromHex(o["key"] | (const char*)nullptr, pc.key, 32);
  }
}

void pcMetricsBegin() {
  pcSources = (PcSource*)heap_caps_calloc(MAX_PCS, sizeof(PcSource), MALLOC_CAP_SPIRAM);
  if (!pcSources) pcSources = (PcSource*)calloc(MAX_PCS, sizeof(PcSource));
  for (int i = 0; i < MAX_PCS; i++) {
    pcSources[i].data.status = PC_STATUS_OK;
    pcSources[i].idleS = -1;
  }
  loadKnown();
  Preferences p;
  if (p.begin("deckpc", true)) {
    strlcpy(s_mode, p.getString("active", "").c_str(), sizeof(s_mode));
    p.end();
  }
  udp.begin(UDP_PORT);
  Serial.printf("pc_metrics: UDP listening on port %u\n", (unsigned)UDP_PORT);
}

void pcMetricsRestartUdp() {
  udp.stop();
  udp.begin(UDP_PORT);
  Serial.println("pc_metrics: UDP socket re-bound");
}

// Track names use typographic quotes, dashes and ellipses the deck fonts lack
// (they stop at Latin Extended-A): turn them into ASCII, in place.
static void asciiPunct(char* s) {
  char* out = s;
  for (const char* p = s; *p;) {
    const uint8_t* u = (const uint8_t*)p;
    if (u[0] == 0xE2 && u[1] == 0x80 && u[2]) {
      const char* rep = nullptr;
      switch (u[2]) {
        case 0x98: case 0x99: rep = "'"; break;
        case 0x9C: case 0x9D: rep = "\""; break;
        case 0x93: case 0x94: rep = "-"; break;
        case 0xA6: rep = "..."; break;
      }
      if (rep) {
        // "..." takes three bytes, as many as it replaces: out never passes p.
        while (*rep) *out++ = *rep++;
        p += 3;
        continue;
      }
    }
    *out++ = *p++;
  }
  *out = 0;
}

static void trimTrailingSpaces(char* s) {
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

// The PC a packet belongs to; a new id takes a free slot (and is remembered).
static PcSource* sourceForPacket(const char* id, const char* name) {
  PcSource* free = nullptr;
  for (int i = 0; i < MAX_PCS; i++) {
    PcSource& pc = pcSources[i];
    if (!pc.used) {
      if (!free) free = &pc;
      continue;
    }
    if (strcmp(pc.id, id)) continue;
    if (name[0] && strcmp(pc.name, name)) {
      strlcpy(pc.name, name, sizeof(pc.name));
      saveKnown();
    }
    return &pc;
  }
  if (!free) {
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 60000) {
      lastLog = millis();
      Serial.printf("pc_metrics: no room for PC '%s' (%d known), forget one in the portal\n", id, MAX_PCS);
    }
    return nullptr;
  }
  memset(free, 0, sizeof(PcSource));
  free->used = true;
  free->data.status = PC_STATUS_OK;
  free->idleS = -1;
  strlcpy(free->id, id, sizeof(free->id));
  strlcpy(free->name, name[0] ? name : "PC", sizeof(free->name));
  saveKnown();
  Serial.printf("pc_metrics: new PC '%s' (%s)\n", free->name, free->id);
  return free;
}

// --- Commands in flight and their results (all on the loop task) -----------
struct PendingCmd {
  String pcId, nonce, cmd;
  uint32_t sentMs;
};
static std::vector<PendingCmd> s_pending;
static std::vector<PcCmdResult> s_results;
constexpr uint32_t CMD_TIMEOUT_MS = 3000;

static void addResult(const String& pcId, const String& cmd, bool ok, const String& msg) {
  const PcSource* pc = pcSource(pcId);
  if (s_results.size() < 8) s_results.push_back({pc ? String(pc->name) : String("PC"), cmd, msg, ok});
}

static void handleAck(JsonDocument& doc) {
  const String nonce = doc["ack"] | "";
  for (size_t i = 0; i < s_pending.size(); i++) {
    if (s_pending[i].nonce != nonce) continue;
    addResult(s_pending[i].pcId, s_pending[i].cmd, doc["ok"] | false, doc["msg"] | "");
    s_pending.erase(s_pending.begin() + i);
    return;
  }
}

// The latest Claude event from any PC: a session waiting for you on the other
// computer must still reach the mascot.
struct ClaudeEvent {
  char ev[8], evTxt[32], evSub[40];
  uint32_t seq, rxMs;
};
static ClaudeEvent s_event = {};

static void parseClaude(PcSource* pc, JsonObjectConst c) {
  ClaudeLive& cl = pc->claude;
  strlcpy(cl.mood, c["mood"] | "", sizeof(cl.mood));
  strlcpy(cl.status, c["status"] | "", sizeof(cl.status));
  const uint32_t id = c["ev_id"] | (uint32_t)0;
  if (id && id != cl.evId) {
    cl.evId = id;
    strlcpy(cl.ev, c["ev"] | "", sizeof(cl.ev));
    strlcpy(cl.evTxt, c["ev_txt"] | "", sizeof(cl.evTxt));
    strlcpy(cl.evSub, c["ev_sub"] | "", sizeof(cl.evSub));
    asciiPunct(cl.evTxt);
    asciiPunct(cl.evSub);
    cl.evRxMs = millis();
    cl.evSeq++;
    if (cl.ev[0]) {
      memcpy(s_event.ev, cl.ev, sizeof(s_event.ev));
      memcpy(s_event.evTxt, cl.evTxt, sizeof(s_event.evTxt));
      memcpy(s_event.evSub, cl.evSub, sizeof(s_event.evSub));
      s_event.seq++;
      s_event.rxMs = cl.evRxMs;
    }
  }
  asciiPunct(cl.status);
  cl.rxMs = millis();

  // Open sessions: [name, state, age, detail] rows; companions without them send none.
  cl.sessionCount = 0;
  for (JsonArrayConst row : c["ss"].as<JsonArrayConst>()) {
    if (cl.sessionCount >= CLAUDE_MAX_SESSIONS) break;
    const char* st = row[1] | "";
    if (!strchr("widz", st[0]) || !st[0]) continue;
    ClaudeSession& s = cl.sessions[cl.sessionCount++];
    strlcpy(s.name, row[0] | "", sizeof(s.name));
    strlcpy(s.detail, row[3] | "", sizeof(s.detail));
    asciiPunct(s.name);
    asciiPunct(s.detail);
    s.state = st[0];
    s.age = row[2] | (uint32_t)0;
  }
  cl.sessionsRxMs = millis();

  // Plan usage and context: each field only when the companion has it.
  ClaudeUsage& u = cl.usage;
  if (!u.rxMs) u.q5 = u.q7 = u.ctx = -1;
  bool any = false;
  auto pct = [&](const char* key, float& out) {
    if (!c[key].is<float>()) return;
    out = constrain(c[key].as<float>(), -1.0f, 999.0f);  // -1: the companion has none now
    any = true;
  };
  auto num = [&](const char* key, uint32_t& out) {
    if (!c[key].is<uint32_t>()) return;
    out = c[key].as<uint32_t>();
    any = true;
  };
  pct("q5", u.q5);
  pct("q7", u.q7);
  pct("ctx", u.ctx);
  num("q5r", u.q5r);
  num("q7r", u.q7r);
  num("ctx_tok", u.ctxTok);
  num("ctx_win", u.ctxWin);
  if (c["proj"].is<const char*>()) strlcpy(u.proj, c["proj"], sizeof(u.proj)), any = true;
  if (c["model"].is<const char*>()) strlcpy(u.model, c["model"], sizeof(u.model)), any = true;
  if (c["acct"].is<const char*>()) strlcpy(u.acct, c["acct"], sizeof(u.acct)), any = true;
  if (c["org"].is<const char*>()) strlcpy(u.org, c["org"], sizeof(u.org)), any = true;
  if (any) u.rxMs = millis();
  s_version++;
}

static void parsePacket(const char* json, IPAddress from, bool serial) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.printf("pc_metrics: JSON parse error: %s\n", err.c_str());
    return;
  }
  if (!doc["ack"].isNull()) {
    handleAck(doc);
    return;
  }
  PcSource* pc = sourceForPacket(doc["pc_id"] | "", doc["pc_name"] | "");
  if (!pc) return;
  pc->viaSerial = serial;
  if (!serial) pc->ip = from;
  pc->cmdPort = doc["cmd_port"] | 0;
  pc->artPort = (uint16_t)constrain(doc["art_port"] | 0, 0, 65535);
  // A few nonces per packet, so quick taps (volume) need not wait for the next one.
  memset(pc->nonces, 0, sizeof(pc->nonces));
  int n = 0;
  for (JsonVariantConst v : doc["nonces"].as<JsonArrayConst>())
    if (n < 4) strlcpy(pc->nonces[n++], v | "", sizeof(pc->nonces[0]));
  PcMetricData& d = pc->data;

  JsonObjectConst claude = doc["claude"];
  if (!claude.isNull()) parseClaude(pc, claude);
  // A Claude-only packet leaves the metrics and the PC state alone.
  if (!claude.isNull() && doc["metrics"].isNull() && doc["pcstate"].isNull()) {
    d.lastReceived = millis();
    d.online = true;
    return;
  }

  PcState& st = pc->state;
  JsonObjectConst ps = doc["pcstate"];
  st.vol = ps["vol"].is<int>() ? (int8_t)constrain(ps["vol"].as<int>(), 0, 100) : -1;
  st.mute = ps["mute"].is<bool>() ? (int8_t)ps["mute"].as<bool>() : -1;
  st.awake = ps["awake"].is<bool>() ? (int8_t)ps["awake"].as<bool>() : -1;
  if (ps["idle"].is<int32_t>()) {
    pc->idleS = max((int32_t)0, ps["idle"].as<int32_t>());
    pc->idleRxMs = millis();
  }
  st.battCount = 0;
  for (JsonVariantConst v : ps["batt"].as<JsonArrayConst>()) {
    if (st.battCount >= PC_MAX_BATTERIES) break;
    JsonArrayConst b = v.as<JsonArrayConst>();
    const int pct = b[2] | -1;
    if (pct < 0) continue;
    PcBattery& x = st.batt[st.battCount++];
    strlcpy(x.name, b[0] | "", sizeof(x.name));
    asciiPunct(x.name);
    x.kind = (b[1] | "")[0];
    x.pct = (int8_t)constrain(pct, 0, 100);
    x.charging = b[3] | false;
  }
  JsonObjectConst media = ps["media"];
  const char* ms = media["status"] | "";
  st.media = media.isNull() ? -1 : !strcmp(ms, "playing") ? PC_MEDIA_PLAYING
                                  : !strcmp(ms, "paused") ? PC_MEDIA_PAUSED : PC_MEDIA_STOPPED;
  strlcpy(st.title, media["title"] | "", sizeof(st.title));
  strlcpy(st.artist, media["artist"] | "", sizeof(st.artist));
  st.dur = max((int32_t)0, media["dur"] | (int32_t)0);
  st.pos = max((int32_t)0, media["pos"] | (int32_t)0);
  st.t0 = max((int32_t)0, media["t0"] | (int32_t)0);
  uint8_t art[4];
  st.art[0] = 0;
  if (fromHex(media["art"] | (const char*)nullptr, art, sizeof(art))) toHex(art, sizeof(art), st.art);
  asciiPunct(st.title);
  asciiPunct(st.artist);
  // A state-only packet (sent when the state changes between metrics
  // packets) leaves the metrics and the chart history alone.
  if (doc["metrics"].isNull() && !ps.isNull()) {
    d.lastReceived = millis();
    d.online = true;
    s_version++;
    return;
  }

  d.status = doc["status"] | PC_STATUS_OK;

  const char* ts = doc["timestamp"];
  if (ts && ts[0] && d.status == PC_STATUS_OK) {
    strncpy(d.timestamp, ts, sizeof(d.timestamp) - 1);
    d.timestamp[sizeof(d.timestamp) - 1] = '\0';
  }
  // Empty timestamp (stale frame) or an unhealthy status: keep the last-OK time
  // rather than tracking the current one.

  JsonArray arr = doc["metrics"];
  d.count = 0;
  for (JsonObject mo : arr) {
    if (d.count >= MAX_METRICS) break;
    PcMetric& m = d.metrics[d.count];

    m.id = mo["id"] | 0;

    const char* name = mo["name"];
    if (name) {
      strncpy(m.name, name, sizeof(m.name) - 1);
      m.name[sizeof(m.name) - 1] = '\0';
      trimTrailingSpaces(m.name);
    } else {
      m.name[0] = '\0';
    }

    const char* unit = mo["unit"];
    if (unit) {
      strncpy(m.unit, unit, sizeof(m.unit) - 1);
      m.unit[sizeof(m.unit) - 1] = '\0';
    } else {
      m.unit[0] = '\0';
    }

    m.value = mo["value"] | 0.0f;
    m.max = mo["max"] | 0.0f;

    // Throughput arrives PRE-MULTIPLIED BY TEN. The Windows companion packs
    // NIC readings as an integer number of tenths of a KB/s so one decimal
    // survives the wire (see _parse_rest_value in touchdeck_companion.py: "the
    // ESP32 divides by 10 when showing"), and this firmware never did - so a
    // 366.8 KB/s link rendered as "3668 KB/s". Undo it once, here, so the
    // history rings and the metric tiles see real KB/s.
    //
    // Keyed on the exact "KB/s" string because that scaling belongs to that
    // one unit: the Linux companion reports "MB/s" unscaled, and both must
    // stay correct.
    if (strcmp(m.unit, "KB/s") == 0) m.value /= 10.0f;

    d.count++;
  }

  d.lastReceived = millis();
  d.sampleMs = d.lastReceived;
  d.online = true;
  s_version++;

  // One history sample per METRIC per packet. Rings are keyed by metric id, so
  // nothing here needs invalidating when a slot is rebound or the metric page
  // changes - the ring follows the metric. A metric missing from this packet
  // simply does not advance, leaving a gap rather than a fake sample.
  for (uint8_t i = 0; i < d.count; i++) {
    const PcMetric& m = d.metrics[i];
    if (m.id == 0 || m.id > MAX_METRICS) continue;
    SlotHistory& h = pc->hist[m.id];
    h.metricId = m.id;
    h.samples[h.head] = m.value;
    h.head = (uint8_t)((h.head + 1) % PC_HISTORY_LEN);
    if (h.count < PC_HISTORY_LEN) h.count++;
  }
}

// USB cable: the companion writes "@TD <json>" lines (the same JSON as UDP)
// between the device's log lines; "@TD {"hello":1}" asks who is there.
static char s_line[PC_PACKET_BUF_SIZE];
static size_t s_lineLen = 0;
static bool s_lineOver = false;

static bool serialPoll() {
  if (improvSetupActive()) return false;  // Improv owns the port in setup mode
  bool parsed = false;
  int budget = 4096;  // bytes per loop pass, so a flood cannot stall the UI
  while (budget-- > 0 && Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c != '\n') {
      if (s_lineLen < sizeof(s_line) - 1) s_line[s_lineLen++] = (char)c;
      else s_lineOver = true;
      continue;
    }
    if (s_lineLen && s_line[s_lineLen - 1] == '\r') s_lineLen--;
    s_line[s_lineLen] = 0;
    const bool ours = !s_lineOver && s_lineLen > 4 && !strncmp(s_line, "@TD ", 4);
    s_lineLen = 0;
    s_lineOver = false;
    if (!ours) continue;
    if (!strncmp(s_line + 4, "{\"hello\":", 9)) {
      Serial.printf("\n@TD {\"hello\":\"%s\",\"fw\":\"%s\"}\n", PRODUCT_NAME, FW_VERSION);
      continue;
    }
    parsePacket(s_line + 4, IPAddress(), true);
    parsed = true;
  }
  return parsed;
}

static bool s_paused = false;

void pcMetricsPause(bool pause) {
  if (pause == s_paused) return;
  s_paused = pause;
  if (pause) {
    udp.stop();
    return;
  }
  udp.begin(UDP_PORT);
  for (int i = 0; i < MAX_PCS; i++)
    if (pcSources[i].used) pcSources[i].data.lastReceived = millis();
}

// --- Active PC ---------------------------------------------------------------
// Seconds since the PC's last input now; huge when it does not say (an older
// agent, Linux), so a PC that reports wins.
static uint32_t idleNow(const PcSource& pc) {
  if (pc.idleS < 0 || !pc.idleRxMs) return 0x7FFFFFFF;
  return (uint32_t)pc.idleS + (millis() - pc.idleRxMs) / 1000;
}

// The pinned PC while it is known; Auto: the online PC used last, changing
// only when another one has had input clearly more recently (a few seconds of
// margin, so two PCs both in use do not flip the deck on every packet). An
// offline active PC stays active until another one comes online.
constexpr uint32_t ACTIVE_MARGIN_S = 5;

static void updateActive() {
  int next = s_active;
  if (s_mode[0]) {
    for (int i = 0; i < MAX_PCS; i++)
      if (pcSources[i].used && !strcmp(pcSources[i].id, s_mode)) next = i;
  } else {
    const PcSource& cur = pcSources[s_active];
    const bool curOn = cur.used && cur.data.online;
    uint32_t best = curOn ? idleNow(cur) : 0xFFFFFFFF;
    for (int i = 0; i < MAX_PCS; i++) {
      const PcSource& pc = pcSources[i];
      if (i == s_active || !pc.used || !pc.data.online) continue;
      const uint32_t idle = idleNow(pc);
      if (!curOn ? idle < best : idle + ACTIVE_MARGIN_S < best) {
        best = idle;
        next = i;
      }
    }
  }
  if (!pcSources[next].used) {
    for (next = 0; next < MAX_PCS - 1 && !pcSources[next].used;) next++;
  }
  if (next == s_active) return;
  s_active = next;
  s_version++;
  Serial.printf("pc_metrics: active PC '%s'%s\n", pcSources[next].name, s_mode[0] ? " (pinned)" : "");
}

PcSource* pcActive() {
  if (!pcSources) return nullptr;
  return pcSources[s_active].used ? &pcSources[s_active] : nullptr;
}

const char* pcActiveMode() { return s_mode; }

// --- Per-app PC -----------------------------------------------------------------
struct AppPc {
  char app[12];
  char id[17];
};
constexpr int MAX_APP_PCS = 8;
static AppPc s_appPc[MAX_APP_PCS];
static bool s_appPcLoaded = false;

static void loadAppPcs() {
  if (s_appPcLoaded) return;
  s_appPcLoaded = true;
  Preferences p;
  String s;
  if (p.begin("deckpc", true)) {
    s = p.getString("apppc", "");
    p.end();
  }
  JsonDocument doc;
  if (!s.length() || deserializeJson(doc, s)) return;
  int n = 0;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    if (n == MAX_APP_PCS) break;
    strlcpy(s_appPc[n].app, kv.key().c_str(), sizeof(s_appPc[0].app));
    strlcpy(s_appPc[n].id, kv.value() | "", sizeof(s_appPc[0].id));
    if (s_appPc[n].app[0] && s_appPc[n].id[0]) n++;
  }
}

const char* pcAppMode(const char* app) {
  loadAppPcs();
  for (const AppPc& a : s_appPc)
    if (a.app[0] && !strcmp(a.app, app)) return a.id;
  return "";
}

void pcSetAppMode(const char* app, const char* id) {
  if (!app || !app[0] || !strcmp(pcAppMode(app), id ? id : "")) return;
  AppPc* slot = nullptr;
  for (AppPc& a : s_appPc)
    if (a.app[0] && !strcmp(a.app, app)) slot = &a;
  for (AppPc& a : s_appPc)
    if (!slot && !a.app[0]) slot = &a;
  if (!slot) return;
  if (id && id[0]) {
    strlcpy(slot->app, app, sizeof(slot->app));
    strlcpy(slot->id, id, sizeof(slot->id));
  } else {
    memset(slot, 0, sizeof(*slot));
  }
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  for (const AppPc& a : s_appPc)
    if (a.app[0]) o[a.app] = a.id;
  String out;
  serializeJson(doc, out);
  Preferences p;
  if (p.begin("deckpc", false)) {
    p.putString("apppc", out);
    p.end();
  }
  s_version++;
}

PcSource* pcForApp(const char* app) {
  const char* id = pcAppMode(app);
  PcSource* pc = id[0] ? pcSource(id) : nullptr;
  return pc ? pc : pcActive();
}

void pcSetActiveMode(const char* id) {
  if (!strcmp(id ? id : "", s_mode)) return;
  strlcpy(s_mode, id ? id : "", sizeof(s_mode));
  Preferences p;
  if (p.begin("deckpc", false)) {
    p.putString("active", s_mode);
    p.end();
  }
  s_version++;
  if (pcSources) updateActive();
}

void pcCycleActiveMode() {
  if (!pcSources) return;
  int at = -1;  // Auto
  for (int i = 0; s_mode[0] && i < MAX_PCS; i++)
    if (pcSources[i].used && !strcmp(pcSources[i].id, s_mode)) at = i;
  for (int i = at + 1; i < MAX_PCS; i++)
    if (pcSources[i].used) return pcSetActiveMode(pcSources[i].id);
  pcSetActiveMode("");
}

// --- What the mascot and the IA app show ------------------------------------
static bool claudeFresh(const PcSource& pc) {
  return pc.used && pc.claude.rxMs && millis() - pc.claude.rxMs < PC_STALE_TIMEOUT_MS;
}

const PcSource* pcClaudePc() {
  if (!pcSources) return nullptr;
  const PcSource* act = pcForApp("ia");
  if (act && (claudeFresh(*act) || pcAppMode("ia")[0])) return act;  // an app's own PC: that one only
  const PcSource* last = nullptr;
  for (int i = 0; i < MAX_PCS; i++) {
    const PcSource& pc = pcSources[i];
    if (pc.used && pc.claude.rxMs && (!last || (int32_t)(pc.claude.rxMs - last->claude.rxMs) > 0)) last = &pc;
  }
  return last ? last : act;
}

bool pcClaudeOnline() {
  const PcSource* pc = pcClaudePc();
  return pc && claudeFresh(*pc);
}

const ClaudeLive& pcClaude() {
  static ClaudeLive out;
  static uint32_t builtFor = 0;
  static uint32_t builtAt = 0;
  // Rebuilt on new data; the session ages count from sessionsRxMs, so a
  // copy a moment old still shows the right ones.
  if (builtFor == s_version && builtAt && millis() - builtAt < 1000) return out;
  builtFor = s_version;
  builtAt = millis();
  const PcSource* main = pcClaudePc();
  if (main) out = main->claude;
  else memset(&out, 0, sizeof(out));
  memcpy(out.ev, s_event.ev, sizeof(out.ev));
  memcpy(out.evTxt, s_event.evTxt, sizeof(out.evTxt));
  memcpy(out.evSub, s_event.evSub, sizeof(out.evSub));
  out.evSeq = s_event.seq;
  out.evRxMs = s_event.rxMs;
  // Every online PC's sessions, the Claude PC's first; ages re-based to now.
  const uint32_t now = millis();
  out.sessionCount = 0;
  out.sessionsRxMs = now;
  for (int pass = 0; pass < 2 && pcSources; pass++)
    for (int i = 0; i < MAX_PCS; i++) {
      const PcSource& pc = pcSources[i];
      if ((&pc == main) != (pass == 0) || !claudeFresh(pc)) continue;
      for (int k = 0; k < pc.claude.sessionCount && out.sessionCount < CLAUDE_MAX_SESSIONS; k++) {
        ClaudeSession& s = out.sessions[out.sessionCount++];
        s = pc.claude.sessions[k];
        s.age += (now - pc.claude.sessionsRxMs) / 1000;
        s.pc = (uint8_t)i;
      }
    }
  return out;
}

bool pcMetricsHandle() {
  bool parsed = serialPoll();
  int packetSize = s_paused ? 0 : udp.parsePacket();
  if (packetSize) {
    static char buffer[PC_PACKET_BUF_SIZE];
    // Reject oversize before reading so a truncated body is never parsed.
    if (packetSize > (int)sizeof(buffer) - 1) {
      Serial.printf("pc_metrics: packet %d bytes exceeds buffer %d, discarding\n",
                    packetSize, (int)sizeof(buffer));
      udp.flush();
    } else {
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        parsePacket(buffer, udp.remoteIP(), false);
        parsed = true;
      }
    }
  }

  for (size_t i = 0; i < s_pending.size();) {
    if (millis() - s_pending[i].sentMs < CMD_TIMEOUT_MS) {
      i++;
      continue;
    }
    addResult(s_pending[i].pcId, s_pending[i].cmd, false, "no answer");
    s_pending.erase(s_pending.begin() + i);
  }

  updateActive();

  // Liveness: drop to offline when no packet has arrived for the stale window.
  for (int i = 0; i < MAX_PCS; i++) {
    PcMetricData& d = pcSources[i].data;
    if (!s_paused && pcSources[i].used && d.online && millis() - d.lastReceived > PC_STALE_TIMEOUT_MS) {
      d.online = false;
      s_version++;
      Serial.printf("pc_metrics: PC '%s' offline (no packets within stale window)\n", pcSources[i].name);
    }
  }
  return parsed;
}

PcSource* pcSource(const String& id) {
  if (!pcSources) return nullptr;
  if (!id.length()) return pcActive();
  for (int i = 0; i < MAX_PCS; i++)
    if (pcSources[i].used && id == pcSources[i].id) return &pcSources[i];
  return nullptr;
}

int pcOnlineCount() {
  int n = 0;
  for (int i = 0; pcSources && i < MAX_PCS; i++) n += pcSources[i].used && pcSources[i].data.online;
  return n;
}

bool pcAnyOnline() { return pcOnlineCount() > 0; }

uint32_t pcVersion() { return s_version; }

bool pcForget(const String& id) {
  for (int i = 0; i < MAX_PCS; i++) {
    if (!pcSources[i].used || id != pcSources[i].id) continue;
    // Keep the order (the first one is the default): close the gap.
    for (int j = i; j + 1 < MAX_PCS; j++) memcpy(&pcSources[j], &pcSources[j + 1], sizeof(PcSource));
    memset(&pcSources[MAX_PCS - 1], 0, sizeof(PcSource));
    pcSources[MAX_PCS - 1].idleS = -1;
    if (s_active > i) s_active--;
    else if (s_active == i) s_active = 0;
    if (id == s_mode) pcSetActiveMode("");
    saveKnown();
    s_version++;
    return true;
  }
  return false;
}

bool pcSendCommand(const String& pcId, const char* cmd, String& err) {
  PcSource* pc = pcSource(pcId);
  if (!pc) {
    err = "unknown PC";
    return false;
  }
  if (!pc->paired) {
    err = String(pc->name) + " is not paired";
    return false;
  }
  if (!pc->data.online || (!pc->cmdPort && !pc->viaSerial)) {
    err = String(pc->name) + (pc->data.online ? " does not take commands" : " is offline");
    return false;
  }
  char* nonce = nullptr;
  for (auto& n : pc->nonces)
    if (n[0]) nonce = n;
  if (!nonce) {
    err = "too fast, try again";
    return false;
  }
  if (s_pending.size() >= 4) {
    err = "busy, try again";
    return false;
  }
  const String msg = String(pc->id) + "|" + nonce + "|" + cmd;
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), pc->key, 32,
                  (const uint8_t*)msg.c_str(), msg.length(), mac);
  char sig[65];
  toHex(mac, 32, sig);
  JsonDocument doc;
  doc["pc"] = pc->id;
  doc["cmd"] = cmd;
  doc["nonce"] = nonce;
  doc["sig"] = sig;
  String out;
  serializeJson(doc, out);
  if (pc->viaSerial) {
    // A leading newline ends any half-written log line first.
    Serial.printf("\n@TD %s\n", out.c_str());
  } else {
    udp.beginPacket(pc->ip, pc->cmdPort);
    udp.write((const uint8_t*)out.c_str(), out.length());
    if (!udp.endPacket()) {
      err = "send failed";
      return false;
    }
  }
  s_pending.push_back({pc->id, nonce, cmd, millis()});
  nonce[0] = 0;  // single use; the next metrics packet brings new ones
  return true;
}

bool pcTakeCmdResult(PcCmdResult& out) {
  if (s_results.empty()) return false;
  out = s_results.front();
  s_results.erase(s_results.begin());
  return true;
}

// --- Pairing -----------------------------------------------------------------
static struct {
  String id;
  char code[7];    // code mode; "" in tap mode
  char token[33];  // tap mode: only the companion that started it may collect the key
  bool allowed;    // tap mode: the user tapped Allow
  uint8_t key[32];
  uint32_t until;  // millis deadline, 0 = no pairing
  uint8_t tries;
} s_pair = {};
constexpr uint32_t PAIR_MS = 120000;

bool pcPairStart(const String& id, bool tap, String& token, String& err) {
  PcSource* pc = pcSource(id);
  if (!pc || id != pc->id) {
    err = "unknown PC: start the companion's metric stream to this device first";
    return false;
  }
  s_pair.id = id;
  s_pair.code[0] = s_pair.token[0] = 0;
  if (tap) {
    uint8_t t[16];
    esp_fill_random(t, sizeof(t));
    toHex(t, sizeof(t), s_pair.token);
    token = s_pair.token;
  } else {
    snprintf(s_pair.code, sizeof(s_pair.code), "%06lu", (unsigned long)(esp_random() % 1000000));
  }
  s_pair.allowed = false;
  esp_fill_random(s_pair.key, sizeof(s_pair.key));
  s_pair.until = millis() + PAIR_MS;
  if (!s_pair.until) s_pair.until = 1;
  s_pair.tries = 0;
  Serial.printf("pc_metrics: pairing '%s' started (%s)\n", pc->name, tap ? "tap" : "code");
  return true;
}

bool pcPairConfirm(const String& id, const String& code, const String& token, String& keyHex,
                   bool& waiting, String& err) {
  waiting = false;
  if (!s_pair.until || (int32_t)(millis() - s_pair.until) >= 0 || id != s_pair.id) {
    err = "no pairing in progress for this PC";
    return false;
  }
  const bool tap = s_pair.token[0] != 0;
  if (tap ? token != s_pair.token : code != s_pair.code) {
    if (++s_pair.tries >= 5) pcPairCancel();
    err = tap ? "wrong token" : "wrong code";
    return false;
  }
  if (tap && !s_pair.allowed) {
    waiting = true;
    err = "waiting for Allow on the device";
    return false;
  }
  PcSource* pc = pcSource(id);
  if (!pc) {
    pcPairCancel();
    err = "unknown PC";
    return false;
  }
  memcpy(pc->key, s_pair.key, 32);
  pc->paired = true;
  saveKnown();
  char k[65];
  toHex(s_pair.key, 32, k);
  keyHex = k;
  pcPairCancel();
  s_version++;
  Serial.printf("pc_metrics: '%s' paired\n", pc->name);
  return true;
}

void pcPairAllow() {
  if (s_pair.until && s_pair.token[0]) s_pair.allowed = true;
}

void pcPairCancel() {
  s_pair.until = 0;
  s_pair.id = "";
  s_pair.allowed = false;
  s_pair.code[0] = s_pair.token[0] = 0;
  memset(s_pair.key, 0, sizeof(s_pair.key));
}

bool pcPairPending(String& pcName, String& code) {
  if (!s_pair.until) return false;
  if ((int32_t)(millis() - s_pair.until) >= 0) {
    pcPairCancel();
    return false;
  }
  if (s_pair.allowed) return false;  // answered; the companion collects the key
  const PcSource* pc = pcSource(s_pair.id);
  pcName = pc ? pc->name : "PC";
  code = s_pair.code;
  return true;
}

bool pcPairedState(const String& id, bool& paired) {
  const PcSource* pc = id.length() ? pcSource(id) : nullptr;
  if (!pc || id != pc->id) return false;
  paired = pc->paired;
  return true;
}

const PcState* pcStateOf(const PcSource* pc) {
  return pc && pc->data.online ? &pc->state : nullptr;
}

const PcMetric* pcMetricFind(const PcSource* pc, uint8_t id) {
  if (!pc || !pc->data.online) return nullptr;
  for (uint8_t i = 0; i < pc->data.count; i++)
    if (pc->data.metrics[i].id == id) return &pc->data.metrics[i];
  return nullptr;
}
