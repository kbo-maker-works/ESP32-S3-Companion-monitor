// Work app (app_work.cpp): GitLab (merge requests by dashboard section,
// pipelines) and Jira (one section per filter) in two tabs, from the PC
// agent's work data (work_data.h, pc-agent/td_work.py).
#ifndef APP_WORK_H
#define APP_WORK_H

#include "apps/app.h"

namespace app {
extern const Def kWork;

// The next open of the Work app (or the open one) shows this tab (0 GitLab,
// 1 Jira) with this section unfolded and scrolled up: a GitLab WorkSection,
// WORK_SECTIONS for the pipelines, or a Jira filter's index. -1 = no section.
void workFocus(int tab, int section);
}  // namespace app

#endif  // APP_WORK_H
