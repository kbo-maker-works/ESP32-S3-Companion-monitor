// Strip effects and events (led_fx.h). Light on purpose: a handful of LEDs,
// a frame every 16 ms, plain float math, no palettes.
#include "hardware/led_fx.h"
#include "config.h"
#include <math.h>
#include <string.h>

namespace {

constexpr float TAU = 6.2831853f;

struct Rgb {
  float r, g, b;
};

Rgb rgbOf(uint32_t c) {
  return {(float)((c >> 16) & 0xFF), (float)((c >> 8) & 0xFF), (float)(c & 0xFF)};
}
Rgb mix(const Rgb& a, const Rgb& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
Rgb scale(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }

uint8_t to8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (uint8_t)(v + 0.5f); }
void put(uint8_t* p, const Rgb& c) {
  p[0] = to8(c.r);
  p[1] = to8(c.g);
  p[2] = to8(c.b);
}

// A hue in turns (0..1) at full value, saturation 0..1.
Rgb hsv(float h, float s) {
  h -= floorf(h);
  const float x = h * 6.0f;
  const int i = (int)x % 6;
  const float f = x - floorf(x);
  const float p = 255.0f * (1 - s), q = 255.0f * (1 - s * f), t = 255.0f * (1 - s * (1 - f));
  switch (i) {
    case 0: return {255, t, p};
    case 1: return {q, 255, p};
    case 2: return {p, 255, t};
    case 3: return {p, q, 255};
    case 4: return {t, p, 255};
    default: return {255, p, q};
  }
}

// Speed 0..255 as a period, slow at 0 and fast at 255, evenly spaced to the eye.
float periodMs(uint8_t speed, float slowMs, float fastMs) {
  return slowMs * powf(fastMs / slowMs, speed / 255.0f);
}
// Where ms falls in a period, 0..1. Double: a float loses milliseconds after a few hours.
float phase(uint32_t ms, float period) {
  const double x = (double)ms / period;
  return (float)(x - floor(x));
}
float wave01(float ph) { return 0.5f - 0.5f * cosf(TAU * ph); }  // 0 -> 1 -> 0 over one phase
float tri(float x) {
  x -= floorf(x);
  return x < 0.5f ? 2 * x : 2 - 2 * x;
}
float frac(float x) { return x - floorf(x); }
float k01(uint8_t v) { return v / 255.0f; }

uint32_t s_rng = 0x9E3779B9u;
uint32_t rnd() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return s_rng;
}
float rndf() { return (rnd() >> 8) / 16777216.0f; }

// Per-LED state of the effects that remember (twinkle level, candle flicker, fire heat).
float s_a[LED_STRIP_MAX], s_b[LED_STRIP_MAX];
uint32_t s_lastMs = 0;
uint8_t s_lastFx = 0xFF;
float s_stepAcc = 0;

Rgb heatColor(float h) {
  h = h < 0 ? 0 : h > 1 ? 1 : h;
  if (h < 1.0f / 3) return {h * 3 * 255, 0, 0};
  if (h < 2.0f / 3) return {255, (h - 1.0f / 3) * 3 * 255, 0};
  return {255, 255, (h - 2.0f / 3) * 3 * 150};
}

}  // namespace

void ledFxReset() {
  memset(s_a, 0, sizeof(s_a));
  memset(s_b, 0, sizeof(s_b));
  s_lastFx = 0xFF;
  s_stepAcc = 0;
}

void ledFxRender(const LedFxSettings& fx, uint32_t c1hex, uint8_t n, uint32_t ms, uint8_t (*px)[3]) {
  if (!n) return;
  if (fx.effect != s_lastFx) {
    ledFxReset();
    s_lastFx = fx.effect;
    s_lastMs = ms;
    for (uint8_t i = 0; i < n; i++) s_a[i] = s_b[i] = 1;  // the candle starts lit
    if (fx.effect == LED_FX_TWINKLE || fx.effect == LED_FX_FIRE)
      for (uint8_t i = 0; i < n; i++) s_a[i] = s_b[i] = 0;
  }
  const float dt = (float)(uint32_t)(ms - s_lastMs);  // ms since the last frame
  s_lastMs = ms;
  const Rgb c1 = rgbOf(c1hex), c2 = rgbOf(fx.color2);
  const float in = k01(fx.intensity);
  const float last = n > 1 ? (float)(n - 1) : 1.0f;

  switch (fx.effect) {
    case LED_FX_BREATHE: {
      // Intensity: how deep it fades.
      const float e = wave01(phase(ms, periodMs(fx.speed, 8000, 800)));
      const float v = 1 - in + in * e * e;
      for (uint8_t i = 0; i < n; i++) put(px[i], scale(c1, v));
      break;
    }
    case LED_FX_RAINBOW: {
      // Intensity: saturation, pastel to full.
      const Rgb c = hsv(phase(ms, periodMs(fx.speed, 60000, 2000)), 0.35f + 0.65f * in);
      for (uint8_t i = 0; i < n; i++) put(px[i], c);
      break;
    }
    case LED_FX_RAINBOW_CYCLE: {
      // Intensity: how much of the wheel the strip holds.
      const float ph = phase(ms, periodMs(fx.speed, 30000, 1500)), spread = 0.1f + 0.9f * in;
      for (uint8_t i = 0; i < n; i++) put(px[i], hsv(ph - spread * i / n, 1));
      break;
    }
    case LED_FX_WAVE: {
      // One crest over the strip, travelling; intensity: how dark between crests.
      const float ph = phase(ms, periodMs(fx.speed, 6000, 600));
      for (uint8_t i = 0; i < n; i++) {
        const float e = wave01((float)i / n - ph);
        put(px[i], scale(c1, 1 - in + in * e * e));
      }
      break;
    }
    case LED_FX_GRADIENT: {
      // Color to color2 along the strip; with speed, the colors flow.
      const float ph = fx.speed ? phase(ms, periodMs(fx.speed, 30000, 1500)) : 0;
      for (uint8_t i = 0; i < n; i++) put(px[i], mix(c1, c2, tri(i / last / 2 + ph)));
      break;
    }
    case LED_FX_GLOW: {
      // Each LED its own slow breath; intensity: how deep.
      const float per = periodMs(fx.speed, 9000, 1200);
      for (uint8_t i = 0; i < n; i++) {
        const float k = 0.75f + 0.5f * frac(i * 0.618f);
        const float e = wave01(phase(ms, per * k) + i * 0.37f);
        put(px[i], scale(c1, 1 - in + in * e * e));
      }
      break;
    }
    case LED_FX_AMBIENT: {
      // Color and color2 morphing; intensity: how independently each LED drifts.
      const float per = periodMs(fx.speed, 40000, 3000);
      for (uint8_t i = 0; i < n; i++) {
        const float m = wave01(phase(ms, per * (1 + in * 0.6f * frac(i * 0.618f))) + in * i * 0.29f);
        put(px[i], mix(c1, c2, m));
      }
      break;
    }
    case LED_FX_TWINKLE: {
      // Color sparks on color2; speed: how fast they fade, intensity: how many.
      const float fade = periodMs(fx.speed, 3000, 250);
      for (uint8_t i = 0; i < n; i++) {
        s_a[i] -= dt / fade;
        if (s_a[i] < 0) s_a[i] = 0;
      }
      const float perSecond = (0.3f + 4.0f * in) * n / 6.0f;
      if (rndf() < perSecond * dt / 1000.0f) s_a[rnd() % n] = 1;
      for (uint8_t i = 0; i < n; i++) put(px[i], mix(c2, c1, s_a[i] * s_a[i]));
      break;
    }
    case LED_FX_CANDLE: {
      // Color flickering; speed: how fast, intensity: how deep.
      const float step = periodMs(fx.speed, 260, 40);
      for (uint8_t i = 0; i < n; i++) {
        if (rndf() < dt / step) {
          const float r = rndf();
          s_b[i] = 1 - (0.1f + 0.7f * in) * r * r;
        }
        const float k = dt / (step * 0.6f);
        s_a[i] += (s_b[i] - s_a[i]) * (k > 1 ? 1 : k);
        put(px[i], scale(c1, s_a[i]));
      }
      break;
    }
    case LED_FX_FIRE: {
      // Heat rising from the first LED; speed: how lively, intensity: how much it cools.
      const float step = periodMs(fx.speed, 90, 15);
      s_stepAcc += dt;
      if (s_stepAcc > 10 * step) s_stepAcc = step;
      while (s_stepAcc >= step) {
        s_stepAcc -= step;
        for (uint8_t i = 0; i < n; i++) {
          s_a[i] -= rndf() * (0.02f + 0.25f * in);
          if (s_a[i] < 0) s_a[i] = 0;
        }
        for (int i = n - 1; i >= 2; i--) s_a[i] = (s_a[i - 1] + 2 * s_a[i - 2]) / 3;
        if (n > 1) s_a[1] = (s_a[0] + s_a[1]) / 2;
        if (rndf() < 0.6f) {
          const uint8_t j = n > 1 ? rnd() % 2 : 0;
          s_a[j] += 0.35f + 0.4f * rndf();
          if (s_a[j] > 1) s_a[j] = 1;
        }
      }
      for (uint8_t i = 0; i < n; i++) put(px[i], heatColor(s_a[i]));
      break;
    }
    case LED_FX_SCANNER: {
      // A dot going back and forth over color2; intensity: its tail.
      const float pos = tri(phase(ms, periodMs(fx.speed, 5000, 500))) * last;
      const float tail = 0.6f + 2.4f * in;
      for (uint8_t i = 0; i < n; i++) {
        float e = 1 - fabsf(i - pos) / tail;
        if (e < 0) e = 0;
        put(px[i], mix(c2, c1, e * e));
      }
      break;
    }
    case LED_FX_COMET: {
      // A head running along the strip with a fading tail; intensity: the tail.
      const float tail = 1 + 4 * in;
      const float head = phase(ms, periodMs(fx.speed, 5000, 500)) * (n + tail) - 0.5f;
      for (uint8_t i = 0; i < n; i++) {
        const float d = head - i;
        float e = d >= 0 ? 1 - d / tail : 1 + d * 2;
        if (e < 0) e = 0;
        put(px[i], mix(c2, c1, e * e));
      }
      break;
    }
    case LED_FX_CHASE: {
      // Every few LEDs lit, stepping along; intensity: the gap.
      const int spacing = 2 + fx.intensity * 3 / 256;
      const int step = (int)(phase(ms, periodMs(fx.speed, 3000, 300)) * spacing);
      for (uint8_t i = 0; i < n; i++) put(px[i], (i + step) % spacing == 0 ? c1 : c2);
      break;
    }
    case LED_FX_RADAR: {
      // The beam turns clockwise from north like the radar app's, in step with
      // it; the strip sits under the screen, from south-west (first LED) to
      // south-east, so the beam crosses it from the right and leaves a fading
      // trail. Intensity: the trail's length; a faint glow stays between passes.
      const float sweep = phase(ms, LED_RADAR_SWEEP_MS) * 360.0f;
      const float trail = 40 + 140 * in;
      for (uint8_t i = 0; i < n; i++) {
        const float at = n > 1 ? 225.0f - 90.0f * i / (n - 1) : 180.0f;
        float d = sweep - at;  // degrees since the beam passed
        d -= 360.0f * floorf(d / 360.0f);
        const float e = d < trail ? 1 - d / trail : 0;
        put(px[i], scale(c1, 0.06f + 0.94f * e * e));
      }
      break;
    }
    default:  // LED_FX_SOLID
      for (uint8_t i = 0; i < n; i++) put(px[i], c1);
      break;
  }
}

void ledFxEvent(uint8_t ev, uint8_t pct, uint32_t color, bool paused, uint8_t n, uint32_t ms,
                uint8_t (*px)[3]) {
  if (!n) return;
  switch (ev) {
    case LED_EV_PROGRESS: {
      // The filament's color at full value (a dark one would not show), black as white.
      Rgb c = rgbOf(color);
      const float top = fmaxf(c.r, fmaxf(c.g, c.b));
      c = top < 40 ? Rgb{200, 200, 200} : scale(c, 255 / top);
      const float bg = 0.08f;
      const float fill = (pct > 100 ? 100 : pct) * n / 100.0f;
      const float pulse = 0.35f * wave01(phase(ms, 1800));
      const float hold = paused ? 0.3f + 0.7f * wave01(phase(ms, 3000)) : 1;
      for (uint8_t i = 0; i < n; i++) {
        float v;
        if (i + 1 <= fill) v = 1;
        else if (i == (uint8_t)fill) {
          const float f = fill - i;  // the LED being filled pulses toward full
          v = bg + (1 - bg) * (f + (1 - f) * pulse);
        } else v = bg;
        put(px[i], scale(c, v * hold));
      }
      break;
    }
    case LED_EV_DONE: {
      const float e = wave01(phase(ms, 2200));
      for (uint8_t i = 0; i < n; i++) put(px[i], scale(Rgb{16, 255, 64}, 0.2f + 0.8f * e * e));
      break;
    }
    case LED_EV_CLAUDE: {
      const float e = wave01(phase(ms, 1600));
      for (uint8_t i = 0; i < n; i++) put(px[i], scale(Rgb{255, 122, 0}, 0.12f + 0.88f * e * e));
      break;
    }
    case LED_EV_ERROR: {
      const bool on = phase(ms, 700) < 0.5f;
      for (uint8_t i = 0; i < n; i++) put(px[i], on ? Rgb{255, 0, 0} : Rgb{0, 0, 0});
      break;
    }
    default:
      memset(px, 0, (size_t)n * 3);
      break;
  }
}
