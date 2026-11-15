// AI app: how much of the Claude plan is used (5-hour and 7-day windows, with
// their reset countdowns), the Claude mascot, and the open Claude Code
// sessions with their state. (No context meter: with several sessions open,
// one session's context says little.) The data comes from the PC companion's
// "claude" object (pc_metrics.h, ClaudeUsage / ClaudeSession); see
// pc-agent/touchdeck_agent.py.
#include "apps/app_ia.h"
#include "ui/deck_ui_int.h"
#include "services/pc_metrics.h"
#include "services/claude_fmt.h"

namespace app {
namespace {

using namespace deckui;

struct Meter {
  lv_obj_t* pct;
  lv_obj_t* bar;
  lv_obj_t* sub;
};
Meter s_m5, s_m7;
lv_obj_t* s_empty = nullptr;
lv_obj_t *s_acct = nullptr, *s_org = nullptr;  // the logged-in account, over the meters
uint32_t s_shownVer = 0, s_tickAt = 0;

// Open Claude Code sessions: a coloured dot (red waits for you, green turn
// done, blue pulsing working, grey idle), the name, how long, and a line under.
struct SessionRow {
  lv_obj_t *box, *dot, *name, *age, *sub;
  char state;
};
SessionRow s_rows[CLAUDE_MAX_SESSIONS];
lv_obj_t *s_sessCount = nullptr, *s_sessNone = nullptr;
constexpr uint32_t C_WORKING = 0x60A5FA;

// The mascot widget (widget.cpp), hosted in a box under the bars.
const widget::Def* s_mDef = nullptr;
void* s_mState = nullptr;
lv_obj_t* s_mBox = nullptr;

void mascotSize(lv_event_t*) {
  if (!s_mDef || !s_mState || !s_mBox) return;
  s_mDef->resize(s_mState, lv_obj_get_content_width(s_mBox), lv_obj_get_content_height(s_mBox));
  s_mDef->update(s_mState);
}

void mascotTap(lv_event_t*) {
  if (!s_mDef || !s_mDef->tap || !s_mState) return;
  hapticTap();
  audioClick();
  s_mDef->tap(s_mState);
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

// One block: NAME ............ 42%, the bar, then a dim line under it.
Meter meter(lv_obj_t* col, const char* name) {
  lv_obj_t* box = lv_obj_create(col);
  lv_obj_remove_style_all(box);
  lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 3, 0);

  lv_obj_t* head = lv_obj_create(box);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
  label(head, &deck_font_12, C_DIM, name);
  Meter m;
  m.pct = label(head, &deck_font_14, C_TEXT, "--");
  lv_obj_align(m.pct, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_align(lv_obj_get_child(head, 0), LV_ALIGN_LEFT_MID, 0, 0);

  m.bar = lv_bar_create(box);
  lv_obj_set_size(m.bar, LV_PCT(100), 6);
  lv_bar_set_range(m.bar, 0, 1000);
  lv_obj_set_style_bg_color(m.bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(m.bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(m.bar, 3, LV_PART_MAIN);
  lv_obj_set_style_radius(m.bar, 3, LV_PART_INDICATOR);

  m.sub = label(box, &deck_font_12, C_DIM, "");
  lv_label_set_long_mode(m.sub, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(m.sub, LV_PCT(100), lineH(&deck_font_12));  // one line, "..." when long
  return m;
}

uint32_t levelColor(float pct) { return claudefmt::level(pct, C_ACCENT, C_WARN, C_CRIT); }

void setMeter(Meter& m, float pct) {
  if (pct < 0) {
    lv_label_set_text(m.pct, "--");
    lv_obj_set_style_text_color(m.pct, lv_color_hex(C_DIM), 0);
    lv_bar_set_value(m.bar, 0, LV_ANIM_OFF);
    return;
  }
  lv_label_set_text_fmt(m.pct, "%d%%", (int)(pct + 0.5f));
  const uint32_t c = levelColor(pct);
  lv_obj_set_style_text_color(m.pct, lv_color_hex(pct >= 75 ? c : C_TEXT), 0);
  lv_obj_set_style_bg_color(m.bar, lv_color_hex(c), LV_PART_INDICATOR);
  lv_bar_set_value(m.bar, (int32_t)(LV_MIN(pct, 100.0f) * 10), LV_ANIM_OFF);
}

// "Se reinicia en 2h 14m" from a reset epoch; "" while the clock is unset.
void resetText(char* out, size_t n, uint32_t reset) {
  char c[16];
  claudefmt::countdown(c, sizeof(c), reset);
  if (c[0]) snprintf(out, n, "Resets in %s", c);
  else out[0] = 0;
}

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return o;
}

void dotOpa(void* o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t*)o, (lv_opa_t)v, 0); }

void buildSessions(lv_obj_t* panel) {
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel, 7, 0);
  lv_obj_t* head = plain(panel);
  lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_align(label(head, &deck_font_14, C_DIM, "SESSIONS"), LV_ALIGN_LEFT_MID, 0, 0);
  s_sessCount = label(head, &deck_font_14, C_DIM, "");
  lv_obj_align(s_sessCount, LV_ALIGN_RIGHT_MID, 0, 0);
  s_sessNone = label(panel, &deck_font_14, C_DIM, "No open sessions");

  for (SessionRow& r : s_rows) {
    r.state = 0;
    r.box = plain(panel);
    lv_obj_set_size(r.box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r.box, 1, 0);
    lv_obj_t* line = plain(r.box);
    lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(line, 8, 0);
    r.dot = plain(line);
    lv_obj_set_size(r.dot, 10, 10);
    lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
    r.name = label(line, &deck_font_14, C_TEXT, "");
    lv_obj_set_size(r.name, 10, lineH(&deck_font_14));  // grows to the row; one line
    lv_obj_set_flex_grow(r.name, 1);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_MODE_DOTS);
    r.age = label(line, &deck_font_12, C_DIM, "");
    r.sub = label(r.box, &deck_font_12, C_DIM, "");
    lv_obj_set_size(r.sub, LV_PCT(100), lineH(&deck_font_12));
    lv_obj_set_style_pad_left(r.sub, 18, 0);  // under the name
    lv_label_set_long_mode(r.sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_flag(r.box, LV_OBJ_FLAG_HIDDEN);
  }
}

// "now", "4m", "2h", "3d"
void ageText(char* out, size_t n, uint32_t s) {
  if (s < 60) snprintf(out, n, "now");
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

void refreshSessions(bool have) {
  const ClaudeLive& c = pcClaude();
  const int n = have ? c.sessionCount : 0;
  char buf[40];
  snprintf(buf, sizeof(buf), "%d", n);
  setText(s_sessCount, n ? buf : "");
  showObj(s_sessNone, have && !n);
  const uint32_t since = (millis() - c.sessionsRxMs) / 1000;
  for (int i = 0; i < CLAUDE_MAX_SESSIONS; i++) {
    SessionRow& r = s_rows[i];
    showObj(r.box, i < n);
    if (i >= n) continue;
    const ClaudeSession& s = c.sessions[i];
    if (s.state != r.state) {
      r.state = s.state;
      const uint32_t color = s.state == 'i' ? C_CRIT : s.state == 'd' ? C_OK : s.state == 'w' ? C_WORKING : C_DIM;
      lv_obj_set_style_bg_color(r.dot, lv_color_hex(color), 0);
      lv_obj_set_style_text_color(r.sub, lv_color_hex(s.state == 'i' ? C_CRIT : C_DIM), 0);
      lv_anim_delete(r.dot, dotOpa);
      lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
      if (s.state == 'w') {  // a slow pulse while it works
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, r.dot);
        lv_anim_set_exec_cb(&a, dotOpa);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_30);
        lv_anim_set_duration(&a, 700);
        lv_anim_set_reverse_duration(&a, 700);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
      }
    }
    setText(r.name, s.name);
    ageText(buf, sizeof(buf), s.age + since);
    setText(r.age, buf);
    const char* sub = s.detail[0] ? s.detail
                      : s.state == 'i' ? "Waiting for you" : s.state == 'w' ? "Working"
                      : s.state == 'd' ? "Done" : "Idle";
    setText(r.sub, sub);
  }
}

void refresh() {
  const ClaudeUsage& u = pcClaude().usage;
  const bool have = u.rxMs && pcClaudeOnline();
  if (have) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

  lv_label_set_text(s_acct, u.acct[0] ? u.acct : "");
  lv_label_set_text(s_org, u.org);
  if (u.acct[0]) lv_obj_remove_flag(lv_obj_get_parent(s_acct), LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(lv_obj_get_parent(s_acct), LV_OBJ_FLAG_HIDDEN);
  if (u.org[0]) lv_obj_remove_flag(s_org, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_org, LV_OBJ_FLAG_HIDDEN);

  char buf[64];
  setMeter(s_m5, have ? u.q5 : -1);
  resetText(buf, sizeof(buf), have && u.q5 >= 0 ? u.q5r : 0);
  lv_label_set_text(s_m5.sub, buf);

  setMeter(s_m7, have ? u.q7 : -1);
  resetText(buf, sizeof(buf), have && u.q7 >= 0 ? u.q7r : 0);
  lv_label_set_text(s_m7.sub, buf);

  refreshSessions(have);
}

// Landscape: the meters on the left, the sessions on the right. Portrait:
// the meters on top, the sessions under them.
void open(lv_obj_t* body, int w, int h) {
  const bool wide = w > h;
  lv_obj_t* root = plain(body);
  lv_obj_set_size(root, w, h);
  lv_obj_set_style_pad_hor(root, 16, 0);
  lv_obj_set_style_pad_ver(root, 10, 0);
  lv_obj_set_flex_flow(root, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_column(root, 18, 0);
  lv_obj_set_style_pad_row(root, 14, 0);

  lv_obj_t* col = plain(root);
  if (wide) lv_obj_set_size(col, (w - 32 - 18) / 2, LV_PCT(100));
  else lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 8, 0);
  // Whose quotas these are: several accounts can take turns on one computer.
  lv_obj_t* who = lv_obj_create(col);
  lv_obj_remove_style_all(who);
  lv_obj_set_size(who, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(who, LV_FLEX_FLOW_COLUMN);
  s_acct = label(who, &deck_font_14, C_TEXT, "");
  s_org = label(who, &deck_font_12, C_DIM, "");
  for (lv_obj_t* l : {s_acct, s_org}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, LV_PCT(100));
  }
  s_m5 = meter(col, "SESSION · 5 HOURS");
  s_m7 = meter(col, "WEEK · 7 DAYS");

  // The Claude mascot under the bars (the home widget, taps and all).
  s_mBox = plain(col);
  lv_obj_set_width(s_mBox, LV_PCT(100));
  if (wide) lv_obj_set_flex_grow(s_mBox, 1);
  else lv_obj_set_height(s_mBox, 130);
  lv_obj_set_flex_flow(s_mBox, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_mBox, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_mBox, 2, 0);
  lv_obj_add_flag(s_mBox, LV_OBJ_FLAG_CLICKABLE);
  s_mDef = widget::find("mascota");
  s_mState = s_mDef ? s_mDef->create(s_mBox, "detalle", "") : nullptr;  // with what the agent is doing
  lv_obj_add_event_cb(s_mBox, mascotSize, LV_EVENT_SIZE_CHANGED, nullptr);
  lv_obj_add_event_cb(s_mBox, mascotTap, LV_EVENT_SHORT_CLICKED, nullptr);

  lv_obj_t* panel = plain(root);
  if (wide) lv_obj_set_height(panel, LV_PCT(100));
  else lv_obj_set_width(panel, LV_PCT(100));
  lv_obj_set_flex_grow(panel, 1);
  buildSessions(panel);

  // Over everything while the companion sends nothing.
  s_empty = lv_obj_create(body);
  lv_obj_remove_style_all(s_empty);
  lv_obj_set_size(s_empty, w, h);
  lv_obj_set_style_bg_color(s_empty, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s_empty, LV_OPA_80, 0);
  lv_obj_t* msg = label(s_empty, &deck_font_20, C_TEXT, "No Claude Code data");
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -12);
  lv_obj_t* hint = label(s_empty, &deck_font_14, C_DIM, "Start the PC agent (pc-agent)");
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 16);

  s_shownVer = pcVersion();
  s_tickAt = millis();
  refresh();
  lv_obj_update_layout(root);
  mascotSize(nullptr);
}

// The mascot every loop (it paces itself); the rest on new data, or once a
// second for the countdowns and the offline check.
void tick() {
  if (s_mDef && s_mState) s_mDef->update(s_mState);
  const uint32_t ver = pcVersion();
  if (ver == s_shownVer && millis() - s_tickAt < 1000) return;
  s_shownVer = ver;
  s_tickAt = millis();
  refresh();
}

void close() {
  if (s_mDef && s_mState) s_mDef->destroy(s_mState);  // its objects go with the card
  s_mState = nullptr;
  s_mBox = nullptr;
  s_empty = s_sessCount = s_sessNone = nullptr;  // the card deletes the objects (and the pulses)
  s_acct = s_org = nullptr;
  for (SessionRow& r : s_rows) r = {};
}

}  // namespace

const Def kIa = {"ia", "AI", "mdi:robot-outline", true, open, tick, close};

}  // namespace app
