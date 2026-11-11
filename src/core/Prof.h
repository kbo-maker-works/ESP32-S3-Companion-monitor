#pragma once
#include <Arduino.h>

// Medición de tiempos (solo con -DRADAR_PROF=1): media y máximo por fase en µs,
// una línea por segundo en el log serie. Sin la bandera no compila nada.
#ifndef RADAR_PROF
#define RADAR_PROF 0
#endif

#if RADAR_PROF
namespace Prof {
enum Slot { SNAP, BUILD, COPY, SWEEP, PLANES, FLUSH, LOOP, N };
static const char *NAMES[N] = {"snap", "build", "copy", "sweep", "planes", "flush", "loop"};
struct Acc { uint32_t sum = 0, max = 0, n = 0; };
inline Acc *acc() { static Acc a[N]; return a; }
inline void add(Slot s, uint32_t us) {
    Acc &a = acc()[s];
    a.sum += us;
    a.n++;
    if (us > a.max) a.max = us;
}
inline void tick() {
    static uint32_t last = 0;
    if (millis() - last < 1000) return;
    last = millis();
    String out = "[Prof] fps=" + String(acc()[FLUSH].n);
    for (int i = 0; i < N; i++) {
        Acc &a = acc()[i];
        if (!a.n) continue;
        out += " " + String(NAMES[i]) + "=" + String(a.sum / a.n / 1000.0f, 1) + "/" + String(a.max / 1000.0f, 1);
        a = Acc();
    }
    Serial.println(out + "  (ms media/max)");
}
struct Scope {
    Slot s;
    uint32_t t0;
    explicit Scope(Slot s) : s(s), t0(micros()) {}
    ~Scope() { add(s, micros() - t0); }
};
}  // namespace Prof
#define PROF(slot) Prof::Scope _prof_##slot(Prof::slot)
#else
#define PROF(slot)
#endif
