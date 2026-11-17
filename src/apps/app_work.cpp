// Work app: GitLab and Jira tabs over the PC agent's pushes (work_data.h).
//
// GitLab follows GitLab's merge request dashboard: one foldable section per
// state (Review requested, Your merge requests, ...) with its count, the
// "active" ones first, then the ones waiting on someone else, then the
// pipelines of the interesting branches. Jira has one foldable section per
// filter set up in the agent (its count and first tickets).
//
// The rows are built once per open at their maximum count and moved into
// their section on each new push (WorkData::seq); unused ones wait hidden in
// a pool. Once a second the age line moves. A tile's widget can ask for a tab
// and section before opening the app (workFocus).
//
// A tap on a row (an MR, a pipeline, a ticket) asks the agent's PC to open its
// page in the browser, like a Stream Deck key (workOpen).
#include "apps/app_work.h"
#include "ui/deck_ui_int.h"
#include "services/work_data.h"

namespace app {
namespace {

using namespace deckui;

constexpr uint32_t C_RUNNING = 0x60A5FA;
constexpr int GL_PIPES = WORK_SECTIONS;  // the pipelines section, after the MR ones
constexpr int GL_GROUPS = WORK_SECTIONS + 1;

struct Section { lv_obj_t *head, *arrow, *dot, *title, *count, *body; };
struct MrRow { lv_obj_t *box, *chip, *title, *ref, *add, *del, *pipe, *threads, *age; };
struct PipeRow { lv_obj_t *box, *dot, *ref, *info; };
struct TicketRow { lv_obj_t *box, *key, *title, *status; };

struct Ui {
  lv_obj_t *tab[2], *badge[2], *pane[2], *age, *empty, *pool;
  lv_obj_t *summary, *glErr, *excluded, *jiraErr, *jiraNone;
  Section gl[GL_GROUPS];
  Section jira[WORK_MAX_COLS];
  MrRow mrs[WORK_MAX_MRS];
  PipeRow pipes[WORK_MAX_PIPELINES];
  TicketRow tickets[WORK_MAX_TICKETS];
  int active;
  uint32_t seq, ageAt;
};
Ui* s = nullptr;

// Folded or not, per section, for the whole session: 0 = by default (open
// when it has something), 1 = open, 2 = folded.
uint8_t s_fold[2][GL_GROUPS] = {};
int s_focusTab = -1, s_focusSection = -1;

lv_obj_t* plain(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return o;
}

lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* t = "") {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, t);
  return l;
}

// One line that ends in "..." when it does not fit.
lv_obj_t* line(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = text(parent, font, color);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(l, 10, lineH(font));
  lv_obj_set_flex_grow(l, 1);
  return l;
}

lv_obj_t* flexRow(lv_obj_t* parent, int gap) {
  lv_obj_t* r = plain(parent);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, gap, 0);
  return r;
}

lv_obj_t* card(lv_obj_t* parent) {
  lv_obj_t* b = plain(parent);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_TILE), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  return b;
}

lv_obj_t* errorLine(lv_obj_t* parent) {
  lv_obj_t* l = text(parent, &deck_font_12, C_CRIT);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_size(l, LV_PCT(100), lineH(&deck_font_12));
  return l;
}

// A small coloured label: the MR's state, the ticket's status.
lv_obj_t* chip(lv_obj_t* parent) {
  lv_obj_t* c = text(parent, &deck_font_12, C_DIM);
  lv_obj_set_style_radius(c, 4, 0);
  lv_obj_set_style_pad_hor(c, 5, 0);
  lv_obj_set_style_pad_ver(c, 1, 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_20, 0);
  return c;
}

void setChip(lv_obj_t* c, const char* t, uint32_t color) {
  setText(c, t);
  lv_obj_set_style_text_color(c, lv_color_hex(color), 0);
  lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
}

uint32_t toneColor(char t) {
  switch (t) {
    case 'o': return C_OK;
    case 'w': return C_WARN;
    case 'b': return C_CRIT;
    case 'i': return C_RUNNING;
    default: return C_DIM;
  }
}

uint32_t pipeColor(char st) {
  switch (st) {
    case 's': return C_OK;
    case 'f': return C_CRIT;
    case 'r': return C_RUNNING;
    case 'p': return C_WARN;
    default: return C_DIM;
  }
}

const char* pipeSymbol(char st) {
  switch (st) {
    case 's': return LV_SYMBOL_OK;
    case 'f': return LV_SYMBOL_CLOSE;
    case 'r': return LV_SYMBOL_REFRESH;
    case 'p': return LV_SYMBOL_REFRESH;
    case 'm': return LV_SYMBOL_PLAY;
    case 'c': return "-";
    default: return "";
  }
}

void setNumber(lv_obj_t* l, int n) {
  char b[12];
  snprintf(b, sizeof(b), "%d", n);
  setText(l, b);
}

void setBadge(lv_obj_t* b, int n, uint32_t color) {
  showObj(b, n > 0);
  if (n <= 0) return;
  setNumber(b, n);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
}

// --- Sections -----------------------------------------------------------------

void applyFold(int tab, int i, Section& sec, bool hasItems) {
  const uint8_t f = s_fold[tab][i];
  const bool open = f == 1 || (f == 0 && hasItems);
  showObj(sec.body, open);
  setText(sec.arrow, open ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
}

void onHead(lv_event_t* e) {
  if (!s) return;
  const uintptr_t d = (uintptr_t)lv_event_get_user_data(e);
  const int tab = d >> 8, i = d & 0xFF;
  Section& sec = tab ? s->jira[i] : s->gl[i];
  hapticTap();
  s_fold[tab][i] = lv_obj_has_flag(sec.body, LV_OBJ_FLAG_HIDDEN) ? 1 : 2;
  applyFold(tab, i, sec, true);
}

void buildSection(lv_obj_t* parent, Section& sec, int tab, int i, bool dot) {
  sec.head = card(parent);
  lv_obj_add_flag(sec.head, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(sec.head, LV_PCT(100), 30);
  lv_obj_set_flex_flow(sec.head, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sec.head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(sec.head, 8, 0);
  lv_obj_set_style_pad_column(sec.head, 6, 0);
  lv_obj_set_style_margin_top(sec.head, 4, 0);
  sec.arrow = text(sec.head, &deck_font_12, C_DIM, LV_SYMBOL_DOWN);
  sec.dot = nullptr;
  if (dot) {
    sec.dot = plain(sec.head);
    lv_obj_set_size(sec.dot, 9, 9);
    lv_obj_set_style_radius(sec.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(sec.dot, LV_OPA_COVER, 0);
  }
  sec.title = text(sec.head, &deck_font_14, C_TEXT);
  sec.count = text(sec.head, &deck_font_12, C_DIM);
  lv_obj_add_event_cb(sec.head, onHead, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)((tab << 8) | i));

  sec.body = plain(parent);
  lv_obj_set_size(sec.body, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sec.body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(sec.body, 3, 0);
  lv_obj_set_style_pad_left(sec.body, 4, 0);
}

lv_obj_t* pane(lv_obj_t* parent) {
  lv_obj_t* p = lv_obj_create(parent);
  lv_obj_remove_style_all(p);
  lv_obj_set_width(p, LV_PCT(100));
  lv_obj_set_flex_grow(p, 1);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(p, 8, 0);
  lv_obj_set_style_pad_top(p, 4, 0);
  lv_obj_set_style_pad_row(p, 3, 0);
  lv_obj_set_scroll_dir(p, LV_DIR_VER);  // the lists can be longer than the screen
  lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);
  return p;
}

// --- Rows ---------------------------------------------------------------------

enum RowKind : uintptr_t { ROW_MR, ROW_PIPE, ROW_TICKET };

// The row at this index now (rows are refilled in place on each push).
void onRow(lv_event_t* e) {
  if (!s) return;
  const uintptr_t d = (uintptr_t)lv_event_get_user_data(e);
  const int kind = d >> 8, i = d & 0xFF;
  const WorkData& w = workData();
  const char* link = kind == ROW_MR       ? (i < w.mrCount ? w.mrs[i].link : "")
                     : kind == ROW_PIPE   ? (i < w.pipelineCount ? w.pipelines[i].link : "")
                                          : (i < w.ticketCount ? w.tickets[i].link : "");
  hapticTap();
  String err;
  if (!workOpen(link, err)) deckToastShow(err.c_str(), 2500);  // done: the command's own toast
}

void tappable(lv_obj_t* box, RowKind kind, int i) {
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(box, lv_color_hex(C_TILE_LINE), LV_STATE_PRESSED);
  lv_obj_add_event_cb(box, onRow, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)((kind << 8) | i));
}

// Two lines: [chip] title / project!iid +add -del  pipeline threads age
void buildMr(MrRow& r, int i) {
  r.box = card(s->pool);
  tappable(r.box, ROW_MR, i);
  lv_obj_set_size(r.box, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_hor(r.box, 8, 0);
  lv_obj_set_style_pad_ver(r.box, 5, 0);
  lv_obj_set_style_pad_row(r.box, 2, 0);
  lv_obj_t* top = flexRow(r.box, 6);
  r.chip = chip(top);
  r.title = line(top, &deck_font_14, C_TEXT);
  lv_obj_t* meta = flexRow(r.box, 6);
  r.ref = line(meta, &deck_font_12, C_DIM);
  r.add = text(meta, &deck_font_12, C_OK);
  r.del = text(meta, &deck_font_12, C_CRIT);
  r.pipe = text(meta, &deck_font_12, C_DIM);
  r.threads = text(meta, &deck_font_12, C_DIM);
  r.age = text(meta, &deck_font_12, C_DIM);
}

void buildPipe(PipeRow& r, int i) {
  r.box = card(s->pool);
  tappable(r.box, ROW_PIPE, i);
  lv_obj_set_size(r.box, LV_PCT(100), 24);
  lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(r.box, 8, 0);
  lv_obj_set_style_pad_column(r.box, 8, 0);
  r.dot = plain(r.box);
  lv_obj_set_size(r.dot, 8, 8);
  lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
  r.ref = line(r.box, &deck_font_12, C_TEXT);
  r.info = text(r.box, &deck_font_12, C_DIM);
}

void buildTicket(TicketRow& r, int i) {
  r.box = card(s->pool);
  tappable(r.box, ROW_TICKET, i);
  lv_obj_set_size(r.box, LV_PCT(100), 26);
  lv_obj_set_flex_flow(r.box, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r.box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(r.box, 8, 0);
  lv_obj_set_style_pad_column(r.box, 8, 0);
  r.key = text(r.box, &deck_font_12, C_RUNNING);
  r.title = line(r.box, &deck_font_12, C_TEXT);
  r.status = chip(r.box);
}

// --- Tabs ---------------------------------------------------------------------

void showTab(int t) {
  s->active = t;
  for (int i = 0; i < 2; i++) {
    const bool on = i == t;
    showObj(s->pane[i], on);
    lv_obj_set_style_bg_opa(s->tab[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->tab[i], on ? 2 : 0, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(s->tab[i], 0), lv_color_hex(on ? C_TEXT : C_DIM), 0);
  }
}

void onTab(lv_event_t* e) {
  if (!s) return;
  hapticTap();
  showTab((int)(uintptr_t)lv_event_get_user_data(e));
}

// --- Refresh ------------------------------------------------------------------

void refreshGitlab(const WorkData& w) {
  char b[80];
  if (w.gitlab) snprintf(b, sizeof(b), "%d active  ·  %d to-do%s", w.active, w.todos, w.todos == 1 ? "" : "s");
  else b[0] = 0;
  setText(s->summary, b);
  lv_obj_set_style_text_color(s->summary, lv_color_hex(w.todos ? C_WARN : C_DIM), 0);
  snprintf(b, sizeof(b), "GitLab: %s", w.glErr);
  showObj(s->glErr, w.glErr[0]);
  setText(s->glErr, b);

  for (int i = 0; i < WORK_SECTIONS; i++) {
    Section& sec = s->gl[i];
    setText(sec.title, workSectionName(i));
    setNumber(sec.count, w.sec[i]);
    applyFold(0, i, sec, w.sec[i] > 0);
  }
  for (int i = 0; i < WORK_MAX_MRS; i++) {
    MrRow& r = s->mrs[i];
    if (i >= w.mrCount) {
      lv_obj_set_parent(r.box, s->pool);
      continue;
    }
    const WorkMr& m = w.mrs[i];
    lv_obj_set_parent(r.box, s->gl[m.sec].body);
    // set_parent keeps a row where it was or appends it: moved last, in data
    // order, a section lists its rows as the data does (newest first).
    lv_obj_move_foreground(r.box);
    setChip(r.chip, m.chip, toneColor(m.tone));
    setText(r.title, m.title);
    setText(r.ref, m.ref);
    snprintf(b, sizeof(b), "+%ld", (long)m.add);
    setText(r.add, b);
    snprintf(b, sizeof(b), "-%ld", (long)m.del);
    setText(r.del, b);
    setText(r.pipe, pipeSymbol(m.pipe));
    lv_obj_set_style_text_color(r.pipe, lv_color_hex(pipeColor(m.pipe)), 0);
    showObj(r.threads, m.threadsTotal > 0);
    snprintf(b, sizeof(b), LV_SYMBOL_LIST " %u/%u", m.threads, m.threadsTotal);
    setText(r.threads, b);
    lv_obj_set_style_text_color(r.threads, lv_color_hex(m.threads < m.threadsTotal ? C_WARN : C_OK), 0);
    setText(r.age, m.age);
  }

  Section& ps = s->gl[GL_PIPES];
  setText(ps.title, "Pipelines");
  if (w.ko) snprintf(b, sizeof(b), "%u  ·  %d failing", w.pipelineCount, w.ko);
  else snprintf(b, sizeof(b), "%u", w.pipelineCount);
  setText(ps.count, b);
  lv_obj_set_style_text_color(ps.count, lv_color_hex(w.ko ? C_CRIT : C_DIM), 0);
  applyFold(0, GL_PIPES, ps, w.pipelineCount > 0);
  for (int i = 0; i < WORK_MAX_PIPELINES; i++) {
    PipeRow& r = s->pipes[i];
    if (i >= w.pipelineCount) {
      lv_obj_set_parent(r.box, s->pool);
      continue;
    }
    lv_obj_set_parent(r.box, ps.body);
    lv_obj_move_foreground(r.box);
    const WorkPipeline& p = w.pipelines[i];
    snprintf(b, sizeof(b), "%s · %s", p.name, p.ref);
    setText(r.ref, b);
    setText(r.info, p.info);
    lv_obj_set_style_bg_color(r.dot, lv_color_hex(pipeColor(p.status)), 0);
    lv_obj_set_style_text_color(r.info, lv_color_hex(p.status == 'f' ? C_CRIT : C_DIM), 0);
  }
  setBadge(s->badge[0], w.active, w.ko ? 0xF85149 : 0x1F6FEB);
}

void refreshJira(const WorkData& w) {
  char b[64];
  snprintf(b, sizeof(b), "Jira: %s", w.jiraErr);
  showObj(s->jiraErr, w.jiraErr[0]);
  setText(s->jiraErr, b);
  showObj(s->jiraNone, w.jira && !w.colCount && !w.jiraErr[0]);
  for (int i = 0; i < WORK_MAX_COLS; i++) {
    Section& sec = s->jira[i];
    const bool on = i < w.colCount;
    showObj(sec.head, on);
    if (!on) {
      showObj(sec.body, false);
      continue;
    }
    const WorkCol& c = w.cols[i];
    setText(sec.title, c.label);
    if (c.n > c.shown) snprintf(b, sizeof(b), "%d  ·  first %u", c.n, c.shown);
    else snprintf(b, sizeof(b), "%d", c.n);
    setText(sec.count, b);
    lv_obj_set_style_bg_color(sec.dot, lv_color_hex(c.color), 0);
    applyFold(1, i, sec, c.shown > 0);
  }
  for (int i = 0; i < WORK_MAX_TICKETS; i++) {
    TicketRow& r = s->tickets[i];
    if (i >= w.ticketCount) {
      lv_obj_set_parent(r.box, s->pool);
      continue;
    }
    const WorkTicket& t = w.tickets[i];
    lv_obj_set_parent(r.box, s->jira[t.col].body);
    lv_obj_move_foreground(r.box);
    setText(r.key, t.key);
    setText(r.title, t.title);
    setChip(r.status, t.status, toneColor(t.tone));
  }
  setBadge(s->badge[1], w.colCount > 1 ? w.cols[1].n : 0, 0x1F6FEB);  // the second filter: "in progress"
}

void refresh() {
  const WorkData& w = workData();
  showObj(s->empty, !w.rxMs);
  s->seq = w.seq;
  refreshGitlab(w);
  refreshJira(w);
}

// Nothing while the data is current (the top bar's PC icon says the agent is
// there); "updated 12m ago" in amber once its pushes stop.
void refreshAge() {
  const WorkData& w = workData();
  char b[40] = "";
  if (w.rxMs && !workFresh()) snprintf(b, sizeof(b), "updated %um ago", (unsigned)((millis() - w.rxMs) / 60000));
  setText(s->age, b);
}

// A tile asked for a tab and a section: open that section and bring it up.
void applyFocus() {
  if (s_focusTab < 0) return;
  const int tab = s_focusTab, sec = s_focusSection;
  s_focusTab = s_focusSection = -1;
  showTab(tab);
  if (sec < 0) return;
  Section* target = tab ? (sec < WORK_MAX_COLS ? &s->jira[sec] : nullptr) : (sec < GL_GROUPS ? &s->gl[sec] : nullptr);
  if (!target) return;
  s_fold[tab][sec] = 1;
  showObj(target->body, true);
  setText(target->arrow, LV_SYMBOL_DOWN);
  lv_obj_update_layout(s->pane[tab]);
  lv_obj_scroll_to_y(s->pane[tab], LV_MAX(0, lv_obj_get_y(target->head) - 4), LV_ANIM_OFF);
}

void open(lv_obj_t* body, int w, int h) {
  s = new (std::nothrow) Ui();
  if (!s) return;
  lv_obj_t* root = plain(body);
  lv_obj_set_size(root, w, h);
  lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

  // Tabs, and on their right who sent the data and when
  lv_obj_t* bar = plain(root);
  lv_obj_set_size(bar, LV_PCT(100), 32);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_hor(bar, 8, 0);
  lv_obj_set_style_pad_column(bar, 4, 0);
  lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(C_TILE_LINE), 0);
  static const char* const kTabs[2] = {"GitLab", "Jira"};
  for (int i = 0; i < 2; i++) {
    lv_obj_t* t = plain(bar);
    s->tab[i] = t;
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(t, 104, 30);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t, 6, 0);
    lv_obj_set_style_radius(t, 7, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(C_TILE), 0);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(C_ACCENT), 0);
    text(t, &deck_font_14, C_TEXT, kTabs[i]);
    s->badge[i] = text(t, &deck_font_12, 0xFFFFFF);
    lv_obj_set_style_bg_opa(s->badge[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s->badge[i], 8, 0);
    lv_obj_set_style_pad_hor(s->badge[i], 5, 0);
    lv_obj_add_event_cb(t, onTab, LV_EVENT_SHORT_CLICKED, (void*)(uintptr_t)i);
  }
  s->age = text(bar, &deck_font_12, C_WARN);
  lv_obj_set_flex_grow(s->age, 1);
  lv_obj_set_style_text_align(s->age, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_pad_bottom(s->age, 8, 0);

  // Rows wait here, hidden, until a section takes them.
  s->pool = plain(body);
  lv_obj_add_flag(s->pool, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(s->pool, w - 24);

  // GitLab: summary, the active sections, the rest, the pipelines
  lv_obj_t* gp = s->pane[0] = pane(root);
  s->summary = text(gp, &deck_font_12, C_DIM);
  s->glErr = errorLine(gp);
  for (int i = 0; i < GL_GROUPS; i++) {
    if (i == WORK_ACTIVE_SECTIONS) {
      s->excluded = text(gp, &deck_font_12, C_DIM, "Below: waiting on someone else, not counted as active");
      lv_obj_set_style_pad_top(s->excluded, 8, 0);
    }
    buildSection(gp, s->gl[i], 0, i, false);
  }
  for (int i = 0; i < WORK_MAX_MRS; i++) buildMr(s->mrs[i], i);
  for (int i = 0; i < WORK_MAX_PIPELINES; i++) buildPipe(s->pipes[i], i);

  // Jira: one section per filter
  lv_obj_t* jp = s->pane[1] = pane(root);
  s->jiraErr = errorLine(jp);
  s->jiraNone = text(jp, &deck_font_12, C_DIM, "No filters: add them on the agent's settings page");
  for (int i = 0; i < WORK_MAX_COLS; i++) buildSection(jp, s->jira[i], 1, i, true);
  for (int i = 0; i < WORK_MAX_TICKETS; i++) buildTicket(s->tickets[i], i);

  // Over everything until the first push
  s->empty = plain(body);
  lv_obj_set_size(s->empty, w, h);
  lv_obj_set_style_bg_color(s->empty, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s->empty, LV_OPA_90, 0);
  lv_obj_t* msg = text(s->empty, &deck_font_20, C_TEXT, "No work data yet");
  lv_obj_align(msg, LV_ALIGN_CENTER, 0, -22);
  lv_obj_t* hint = text(s->empty, &deck_font_14, C_DIM,
                        "Set up GitLab and Atlassian on the agent's\nsettings page (127.0.0.1:4213 where it runs)");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);

  // GitLab first, Jira when only Jira is set up
  const WorkData& wd = workData();
  showTab(wd.rxMs && !wd.gitlab && wd.jira ? 1 : 0);
  refresh();
  refreshAge();
  applyFocus();
  s->ageAt = millis();
}

void tick() {
  if (!s) return;
  if (workData().seq != s->seq) refresh();  // every push with new content bumps it
  if (millis() - s->ageAt >= 1000) {
    s->ageAt = millis();
    refreshAge();
  }
}

void close() {
  delete s;  // the card deletes the objects
  s = nullptr;
}

}  // namespace

void workFocus(int tab, int section) {
  s_focusTab = tab;
  s_focusSection = section;
  if (s) applyFocus();  // already open
}

const Def kWork = {"work", "Work", "mdi:briefcase-outline", true, open, tick, close, true};

}  // namespace app
