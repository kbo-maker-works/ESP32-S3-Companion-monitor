// Work app (app_work.cpp): GitLab (merge requests by dashboard section,
// pipelines), Jira (one section per filter) and the PC's local branches
// (keep / review / delete) in three tabs, from the PC agent's work data
// (work_data.h, pc-agent/td_work.py).
#ifndef APP_WORK_H
#define APP_WORK_H

#include "apps/app.h"

namespace app {
extern const Def kWork;

// The next open of the Work app (or the open one) shows this tab (0 GitLab,
// 1 Jira, 2 Branches) with this section unfolded and scrolled up: a GitLab
// WorkSection, WORK_SECTIONS for the pipelines, a Jira filter's index or a
// WorkVerdict. -1 = no section.
void workFocus(int tab, int section);
}  // namespace app

#endif  // APP_WORK_H
