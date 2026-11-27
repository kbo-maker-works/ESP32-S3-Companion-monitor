#ifndef PC_METRICS_H
#define PC_METRICS_H

#include <Arduino.h>
#include <IPAddress.h>
#include "config.h"

// One metric as pushed by the PC companion (V2.1 protocol). Metric tiles bind
// to it by id.
struct PcMetric {
  uint8_t id;            // 1-based stable id
  char    name[16];      // short sensor name from the companion ("CPU","GPUT")
  char    unit[8];       // "%", "C", "W", "RPM", "MB", ...
  float   value;
  float   max;           // optional "max": the full scale the PC knows (RAM / VRAM total), 0 = by unit
};

// Latest decoded packet + liveness.
struct PcMetricData {
  PcMetric metrics[MAX_METRICS];
  uint8_t  count;
  char     timestamp[6];   // "HH:MM" from the companion
  uint8_t  status;         // PC_STATUS_* (LHM health reported by the companion)
  bool     online;         // a fresh packet arrived within PC_STALE_TIMEOUT_MS
  uint32_t lastReceived;   // millis() of the last valid packet
  uint32_t sampleMs;       // millis() of the last packet with metrics (one history sample)
};

// Value history for the chart tiles, one sample per received packet, keyed by
// metric id. Every metric in the packet is sampled, so a new chart tile starts
// warm.
#define PC_HISTORY_LEN 60

struct SlotHistory {
  uint8_t metricId;   // metric id the samples belong to; 0 for the empty ring
  uint8_t count;      // valid samples (0..PC_HISTORY_LEN)
  uint8_t head;       // next write index
  float   samples[PC_HISTORY_LEN];
};

// Sample oldest->newest: i in [0, count).
static inline float pcHistoryAt(const SlotHistory& h, uint8_t i) {
  return h.samples[(uint8_t)((h.head + PC_HISTORY_LEN - h.count + i) % PC_HISTORY_LEN)];
}

// One PC running the companion. Packets carry "pc_id" and "pc_name"; a
// companion without them is the PC with id "". The list of known PCs (id,
// name, in the order first seen) is kept in NVS "deckpc", so tiles bound to
// a PC survive reboots; the first one is the default PC.
#define MAX_PCS 4

// A battery the PC reports ("batt"): its own and its wireless accessories'
// (pc-agent/td_batteries.py). kind: 'l' computer, 'k' keyboard, 'm' mouse,
// 't' trackpad, 'h' headphones, 'c' case, 'g' game controller, 's' speaker,
// 'p' phone, 'w' watch, 0 other.
constexpr int PC_MAX_BATTERIES = 6;
struct PcBattery {
  char name[24];
  char kind;
  int8_t pct;     // 0..100
  bool charging;
};

// What the companion reports about the PC itself ("pcstate"); -1 = unknown.
struct PcState {
  int8_t vol;     // master volume 0..100
  int8_t mute;    // 0 / 1
  int8_t awake;   // Keep awake (pc-agent/td_awake.py) 0 / 1
  PcBattery batt[PC_MAX_BATTERIES];  // this PC first
  uint8_t battCount;
  int8_t media;   // PC_MEDIA_*, -1 = no media session
  int32_t dur, pos, t0;  // seconds, 0 = unknown; t0 is a Unix epoch
  char   art[9];         // CRC32 of the source cover, empty = none
  char   title[200], artist[200];  // UTF-8, up to 48 characters each
};
enum : int8_t { PC_MEDIA_STOPPED = 0, PC_MEDIA_PAUSED, PC_MEDIA_PLAYING };

// --- Claude Code (mascot widget) -------------------------------------------
// Optional "claude" object in any packet (with or without metrics):
//   "claude": {"mood": "coding",            // happy|coding|thinking|hyped|sleeping
//              "status": "Cogitating...",   // what Claude Code is doing, "" = idle
//              "ev": "input",               // last hook event: input|done|fail, "" = none
//              "ev_txt": "esp32-c3",        // event title (session name, "Bash fallo!")
//              "ev_sub": "esperando respuesta",
//              "ev_id": 1411279918}         // changes with every new event
// The companion sends the event only while it is recent (~30 s).
// Plan usage for the IA app and the usage widget, each optional (absent = unknown):
//   "q5": 42.5, "q5r": 1763121600,   // 5-hour window: % used, reset (Unix epoch)
//   "q7": 18,   "q7r": 1763380800,   // 7-day window
//   "g5", "g5r", "g7", "g7r"          // Antigravity: Gemini models' 5 h and weekly windows
//   "t5", "t5r", "t7", "t7r"          // Antigravity: third-party models' windows
struct ClaudeUsage {
  float q5, q7;             // percent, -1 = unknown
  uint32_t q5r, q7r;        // reset epochs, 0 = unknown
  float g5, g7, t5, t7;     // Antigravity (td_agy_statusline.py): Gemini / third-party, -1 = unknown
  uint32_t g5r, g7r, t5r, t7r;
  char acct[40];            // the logged-in account ("you@company.com"), "" = unknown
  char org[32];             // its organization
  uint32_t rxMs;            // millis() of the last packet carrying any of it, 0 = never
};
// One open Claude Code session ("ss" rows): name, state and how long in it.
constexpr int CLAUDE_MAX_SESSIONS = 6;
struct ClaudeSession {
  char name[24];
  char detail[32];  // working: the tool; input: what it waits for
  char state;       // 'w' working, 'i' waiting for you, 'd' turn done, 'z' idle
  uint32_t age;     // seconds in that state when the packet left the PC
  uint8_t pc;       // pcClaude(): index in pcSources of the PC it runs on
};
struct ClaudeLive {
  ClaudeSession sessions[CLAUDE_MAX_SESSIONS];
  uint8_t sessionCount;
  uint32_t sessionsRxMs;  // millis() of the last list (ages count from here)
  char mood[12];
  char status[32];
  char ev[8];
  char evTxt[32];
  char evSub[40];
  uint32_t evId;     // last event id seen
  uint32_t evSeq;    // bumps on every new event (0 = none yet)
  uint32_t evRxMs;   // millis() when the current event arrived
  uint32_t rxMs;     // millis() of the last packet with a "claude" object, 0 = never
  ClaudeUsage usage;
};

// --- Claude Code asking you (pc-agent/td_ask.py) ---------------------------
// While a Claude Code session waits on a permission prompt or a question, the
// companion sends a packet of its own every second (once paired):
//   "ask": {"id": "a1b2c3d4",              // the prompt; the answer names it
//           "s": "desktop-companion",      // the session
//           "k": "p",                      // p = a permission, q = a question
//           "t": "Bash",                   // p: the tool; q: the question's header
//           "x": "pio run -t upload",      // p: what it runs or touches; q: the question
//           "d": "Build the firmware",     // optional, p: Claude's description of it
//           "o": ["Deny", "Always", "Allow"],  // the options, up to 4; p: the last is the main one
//           "od": ["...", ...],            // optional, q: each option's description
//           "h": "Always: Bash(pio run:*)",    // optional, p: what Always does
//           "m": false,                    // q: several options may be picked
//           "qi": 0, "qn": 2}              // q: question qi+1 of qn
// and its "claude" object names it ("ask": "a1b2c3d4", "" = none waits). The
// terminal shows the same dialog meanwhile; whichever answers first wins. The
// deck answers with the command "claude:<id>:<qi>:<choice>", choice = the
// option's index, or several joined by "," when "m".
constexpr int CLAUDE_ASK_OPTS = 4;
struct ClaudeAsk {
  char id[12];
  char session[24];
  char kind;           // 'p' or 'q', 0 = none
  char title[32];
  char text[240];
  char desc[100];
  char hint[64];
  char opts[CLAUDE_ASK_OPTS][48];
  char optDesc[CLAUDE_ASK_OPTS][80];
  uint8_t optCount;
  bool multi;
  uint8_t qi, qn;
  uint32_t seq;        // bumps when another prompt or question arrives
  uint32_t rxMs;       // millis() of its last packet
};

struct PcSource {
  bool     used;
  char     id[17];
  char     name[24];
  IPAddress ip;          // where its last packet came from
  uint16_t cmdPort;      // its command listener ("cmd_port"), 0 = none
  uint16_t artPort;      // cover HTTP server, 0 = unavailable
  bool     viaSerial;    // its latest packet came over the USB cable
  char     nonces[4][17];  // one-time nonces from its latest packet, "" = used
  bool     paired;
  uint8_t  key[32];      // shared HMAC key from pairing (NVS only, never in the API)
  PcMetricData data;
  PcState state;
  SlotHistory hist[MAX_METRICS + 1];  // by metric id; index 0 stays empty
  int32_t  idleS;        // seconds since the PC's last keyboard or mouse input ("idle"), -1 = unknown
  uint32_t idleRxMs;     // millis() when idleS arrived
  ClaudeLive claude;     // this PC's Claude Code (its own account and quota)
  ClaudeAsk ask;         // what one of its sessions waits on you for
};

// MAX_PCS entries, in PSRAM (each holds ~8 KB of history).
extern PcSource* pcSources;

// Start the UDP listener on UDP_PORT and load the known PCs.
void pcMetricsBegin();
// Close the UDP socket until resumed (the daily download needs its RAM); the
// pause does not count as silence, so no PC goes offline because of it.
void pcMetricsPause(bool pause);
// Service the socket: parse any pending packet, refresh the online flags.
// Returns true if a new packet was parsed this call.
bool pcMetricsHandle();
// Re-bind the socket after a WiFi reconnect (old fd goes stale).
void pcMetricsRestartUdp();

// The PC a tile names: id "" = the active PC. Null when none.
PcSource* pcSource(const String& id);

// The active PC: what every tile, app and command without its own "pc"
// follows. Mode "" = Auto: the online PC with the most recent keyboard or
// mouse input (the agent's "idle"), so the deck follows the computer in use;
// else the pc_id it is pinned to. Kept in NVS. Null when no PC is known.
PcSource* pcActive();
const char* pcActiveMode();
void pcSetActiveMode(const char* id);
// Auto -> each known PC -> Auto (the quick panel's chip).
void pcCycleActiveMode();

// An app's own PC (the portal's Apps page): "" = it follows the active PC,
// else the pc_id it always uses. Kept in NVS.
const char* pcAppMode(const char* app);
void pcSetAppMode(const char* app, const char* id);
// The app's PC while it is known, else the active one.
PcSource* pcForApp(const char* app);
bool pcAnyOnline();
int pcOnlineCount();
// Bumps on every packet and online change of any PC.
uint32_t pcVersion();
// Drop a PC from the known list (portal). False when it is not known.
bool pcForget(const String& id);

// Lookup for the metric tiles. Null when the PC is offline or lacks the id.
const PcMetric* pcMetricFind(const PcSource* pc, uint8_t id);
// The PC's reported state; null when it is offline.
const PcState* pcStateOf(const PcSource* pc);

// What the mascot and the IA app show: mood, status and quota from the
// Claude PC (the active PC while it sends Claude data, else the last one that
// did), the latest event from any PC, and the sessions of every online PC
// (the Claude PC's first; ClaudeSession.pc says whose).
const ClaudeLive& pcClaude();
// True when the Claude PC's data arrived within PC_STALE_TIMEOUT_MS.
bool pcClaudeOnline();
// The Claude PC (null when none ever sent Claude data).
const PcSource* pcClaudePc();
// The prompt a Claude Code session waits on, from any online PC (the one that
// came first); null when none. who: that PC, for pcSendCommand.
const ClaudeAsk* pcClaudeAsk(const PcSource** who);

// --- PC control -------------------------------------------------------------
// A command goes to the PC's cmd_port as {"pc","cmd","nonce","sig"}, sig =
// hex HMAC-SHA256(key, "pc_id|nonce|cmd") over one of the "nonces" from its
// latest metrics packet (single use on the PC side). The PC answers {"ack": nonce,
// "ok", "msg"} to the device's port 4210.
//
// Pairing (the companion calls it). Tap mode: the device asks Allow / Deny on
// screen and the companion polls pcPairConfirm with the token pcPairStart gave
// it; once allowed it gets the new key. Code mode (older companions, panels):
// the device shows a 6-digit code the user types into the companion.
// Over the USB cable the same JSON travels as "@TD <json>" lines on Serial.
bool pcSendCommand(const String& pcId, const char* cmd, String& err);
struct PcCmdResult {
  String pcName, cmd, msg;
  bool ok;
};
// Next finished command (acked, refused or timed out), if any.
bool pcTakeCmdResult(PcCmdResult& out);

// token is filled in tap mode.
bool pcPairStart(const String& id, bool tap, String& token, String& err);
// keyHex is filled on success; waiting = tap mode, not allowed yet. A wrong
// code or token counts toward a limit of 5.
bool pcPairConfirm(const String& id, const String& code, const String& token, String& keyHex,
                   bool& waiting, String& err);
// The Allow button (a real touch only).
void pcPairAllow();
void pcPairCancel();
// For the device's pairing card: true while a pairing waits for the user;
// code is "" in tap mode.
bool pcPairPending(String& pcName, String& code);
// The companion's check that the device still holds its key. False = unknown PC.
bool pcPairedState(const String& id, bool& paired);

#endif // PC_METRICS_H
