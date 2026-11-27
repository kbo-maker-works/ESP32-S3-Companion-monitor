// Shared formatting for the Claude plan usage (IA app and the "claude" widget).
#ifndef CLAUDE_FMT_H
#define CLAUDE_FMT_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "services/pc_metrics.h"

namespace claudefmt {

// What one meter shows: Claude Code's 5-hour and 7-day windows, then
// Antigravity's (Gemini models, third-party models), 5-hour and weekly.
enum Kind : uint8_t { K_5H = 0, K_7D, K_G5, K_G7, K_T5, K_T7, K_COUNT };

// Percent for a kind, -1 when unknown or the companion is silent.
inline float pct(const ClaudeUsage& u, bool online, Kind k) {
  if (!online || !u.rxMs) return -1;
  switch (k) {
    case K_5H: return u.q5;
    case K_7D: return u.q7;
    case K_G5: return u.g5;
    case K_G7: return u.g7;
    case K_T5: return u.t5;
    case K_T7: return u.t7;
    default: return -1;
  }
}

// Reset epoch of a kind's window, 0 = unknown.
inline uint32_t reset(const ClaudeUsage& u, Kind k) {
  switch (k) {
    case K_5H: return u.q5r;
    case K_7D: return u.q7r;
    case K_G5: return u.g5r;
    case K_G7: return u.g7r;
    case K_T5: return u.t5r;
    case K_T7: return u.t7r;
    default: return 0;
  }
}

// Colour of a fill: accent, amber from 75 %, red from 90 %.
inline uint32_t level(float pct, uint32_t accent, uint32_t warn, uint32_t crit) {
  return pct >= 90 ? crit : pct >= 75 ? warn : accent;
}

// Countdown to a reset epoch: "2h 14m", "3d 4h", "12m"; "" while the clock is unset.
inline void countdown(char* out, size_t n, uint32_t reset) {
  out[0] = 0;
  const time_t now = time(nullptr);
  if (!reset || now < 1609459200) return;
  const long s = (long)reset - (long)now;
  if (s <= 0) snprintf(out, n, "0m");
  else if (s >= 86400) snprintf(out, n, "%ldd %ldh", s / 86400, s / 3600 % 24);
  else if (s >= 3600) snprintf(out, n, "%ldh %02ldm", s / 3600, s / 60 % 60);
  else snprintf(out, n, "%ldm", s / 60 > 0 ? s / 60 : 1L);
}

// The line under a meter: the reset countdown.
inline void detail(char* out, size_t n, const ClaudeUsage& u, Kind k) {
  out[0] = 0;
  char c[16];
  countdown(c, sizeof(c), reset(u, k));
  if (c[0]) snprintf(out, n, "Resets in %s", c);
}

}  // namespace claudefmt

#endif  // CLAUDE_FMT_H
