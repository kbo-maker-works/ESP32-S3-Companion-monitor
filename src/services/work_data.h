// Work data for the Work app and its tiles: GitLab (merge requests by section,
// as on GitLab's merge request dashboard, plus pipelines) and Jira (one section
// per filter: its count and first tickets). The PC agent (pc-agent/td_work.py)
// polls them with the user's own tokens and pushes them here: POST
// /api/pc/work. Field sizes match td_work.py's MAX_*.
#ifndef WORK_DATA_H
#define WORK_DATA_H

#include <Arduino.h>
#include <IPAddress.h>

constexpr int WORK_MAX_PIPELINES = 8, WORK_MAX_MRS = 16, WORK_MAX_COLS = 6, WORK_MAX_TICKETS = 24;
constexpr uint32_t WORK_STALE_MS = 10 * 60 * 1000;  // nothing pushed this long: not current

// GitLab's dashboard sections, in td_work.py's SECTIONS order. The first
// WORK_ACTIVE_SECTIONS make the "active" count; the rest wait on someone else.
enum WorkSection : uint8_t {
  WORK_SEC_RETURNED,   // your MRs a reviewer sent back
  WORK_SEC_REVIEW,     // your review is requested
  WORK_SEC_MINE,       // your MRs with no reviewer yet
  WORK_SEC_AUTHOR,     // you reviewed, the author has to act
  WORK_SEC_APPROVALS,  // your MRs waiting for their reviewers
  WORK_SEC_BY_ME,      // you approved
  WORK_SEC_BY_OTHERS,  // your MRs others approved
  WORK_SECTIONS
};
constexpr int WORK_ACTIVE_SECTIONS = 3;
// "Review requested", ...
const char* workSectionName(int sec);

struct WorkPipeline {
  char name[20];  // project
  char ref[40];   // branch
  char status;    // 's' success, 'f' failed, 'r' running, 'p' pending, 'm' manual, 'c' canceled
  char info[16];  // "6m" ago, the failed job, "2/5" jobs done
  char link[9];   // id of its web page for "open:<link>" (the agent keeps the URL), "" = none
};
struct WorkMr {
  uint8_t sec;     // WorkSection
  char ref[32];    // "core-services!60"
  char title[64];
  char chip[12];   // "Ready", "Requested", "Conflict", "Draft"...
  char tone;       // the chip's: 'o' ok, 'w' warn, 'b' bad, 'i' info, 'd' dim
  int32_t add, del;  // lines
  char pipe;       // head pipeline as WorkPipeline::status, 0 = none
  uint8_t threads, threadsTotal;  // resolved / resolvable
  char age[6];     // "3h", "2d"
  char link[9];    // see WorkPipeline::link
};
struct WorkTicket {
  uint8_t col;     // the filter it came from
  char key[16];    // "PROJ-101"
  char title[64];
  char status[16];
  char tone;
  char link[9];    // see WorkPipeline::link
};
struct WorkCol {   // a Jira filter
  char label[12];
  int n;
  uint32_t color;
  uint8_t shown;   // its tickets sent (the first ones)
};

struct WorkData {
  char pcId[17], pcName[24];    // the PC whose agent sent it (and opens its links)
  bool gitlab, jira;            // the agent sends that section
  char glErr[40], jiraErr[40];  // the last poll failed: why (the rest is the last good data)
  int sec[WORK_SECTIONS];       // MRs per section (exact; the rows below are the first ones)
  int active, todos, ko;
  WorkPipeline pipelines[WORK_MAX_PIPELINES];
  uint8_t pipelineCount;
  WorkMr mrs[WORK_MAX_MRS];     // grouped by section, newest first in each
  uint8_t mrCount;
  WorkCol cols[WORK_MAX_COLS];
  uint8_t colCount;
  WorkTicket tickets[WORK_MAX_TICKETS];  // grouped by filter
  uint8_t ticketCount;
  uint32_t rxMs;  // millis() of the last push, 0 = never
  uint32_t seq;   // bumps when the content changes
};

const WorkData& workData();
// A push arrived within WORK_STALE_MS.
bool workFresh();
// Pipelines failing now, 0 while the data is not current.
int workPipelinesKo();
// Takes a push. Only from a PC the deck already knows, at the address its
// metrics come from (the agent sends both).
bool workApply(const String& json, IPAddress from, String& err);
// Asks that PC's agent to open a row's page (an MR, a pipeline, a ticket) in
// its browser: a signed "open:<link>" command (pc_metrics.h), so the PC must be
// paired. The result comes back as a command toast.
bool workOpen(const char* link, String& err);

#endif  // WORK_DATA_H
