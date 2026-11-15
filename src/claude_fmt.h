// Shared formatting for the Claude plan usage (IA app and the "claude" widget).
#ifndef CLAUDE_FMT_H
#define CLAUDE_FMT_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "pc_metrics.h"

namespace claudefmt {

// What one meter shows: 0 = 5-hour window, 1 = 7-day window, 2 = context.
enum Kind : uint8_t { K_5H = 0, K_7D, K_CTX };

// Percent for a kind, -1 when unknown or the companion is silent.
inline float pct(const ClaudeUsage& u, bool online, Kind k) {
  if (!online || !u.rxMs) return -1;
  return k == K_5H ? u.q5 : k == K_7D ? u.q7 : u.ctx;
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

// 74000 -> "74k", 1000000 -> "1M", 1500000 -> "1.5M"
inline void tokens(char* out, size_t n, uint32_t t) {
  if (t >= 1000000 && t % 1000000 == 0) snprintf(out, n, "%luM", (unsigned long)(t / 1000000));
  else if (t >= 1000000) snprintf(out, n, "%.1fM", t / 1e6);
  else snprintf(out, n, "%luk", (unsigned long)((t + 500) / 1000));
}

// The line under a meter: the reset countdown, or the context's tokens.
inline void detail(char* out, size_t n, const ClaudeUsage& u, Kind k) {
  out[0] = 0;
  if (k == K_CTX) {
    if (u.ctxTok && u.ctxWin) {
      char a[12], b[12];
      tokens(a, sizeof(a), u.ctxTok);
      tokens(b, sizeof(b), u.ctxWin);
      snprintf(out, n, "%s / %s", a, b);
    }
    return;
  }
  char c[16];
  countdown(c, sizeof(c), k == K_5H ? u.q5r : u.q7r);
  if (c[0]) snprintf(out, n, "Resets in %s", c);
}

}  // namespace claudefmt

#endif  // CLAUDE_FMT_H
