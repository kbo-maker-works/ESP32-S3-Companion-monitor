#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include "Settings.h"
#include "TextAA.h"

// Estilo "panel de control retro": un único acento tipo fósforo, grises para
// lo secundario, rojo/verde solo para estados. Módulos con esquinas en chaflán.
namespace Theme {
constexpr uint16_t CYAN  = 0x3F9C;   // #3CF0E0
constexpr uint16_t AMBER = 0xFD80;   // #FFB000
constexpr uint16_t TEXT  = 0xCE59;   // gris claro
constexpr uint16_t MUTED = 0x8C71;   // gris medio
constexpr uint16_t DIM   = 0x4A49;   // apagado
constexpr uint16_t FRAME = 0x39E7;   // marcos finos
constexpr uint16_t OK    = 0x07E8;
constexpr uint16_t KO    = 0xF8A2;
// Color propio de cada indicador de la barra (como en status_icons_preview.png)
constexpr uint16_t C_WIFI  = CYAN;
constexpr uint16_t C_USB   = AMBER;
constexpr uint16_t C_SRV   = 0xA3FF;   // violeta #A07CFF
constexpr uint16_t C_BAT   = OK;

inline uint16_t accent() { return Settings::get().accent == 1 ? AMBER : CYAN; }
inline const char *accentName(uint8_t a) { return a == 1 ? "ÁMBAR" : "CIAN"; }
}  // namespace Theme

namespace Panel {

// Cuánto se mete la fila j (0..h-1) por los chaflanes de c px en las 4 esquinas
inline int _inset(int j, int h, int c) { return max(0, max(c - j, c - (h - 1 - j))); }

inline void fill(int x, int y, int w, int h, int c, uint16_t color, uint8_t alpha = 255) {
    for (int j = 0; j < h; j++) {
        int in = _inset(j, h, c);
        for (int i = in; i < w - in; i++) Fb::blendPixel(x + i, y + j, color, alpha);
    }
}

inline void frame(Arduino_GFX &g, int x, int y, int w, int h, int c, uint16_t color) {
    int x1 = x + w - 1, y1 = y + h - 1;
    g.drawFastHLine(x + c, y, w - 2 * c, color);
    g.drawFastHLine(x + c, y1, w - 2 * c, color);
    g.drawFastVLine(x, y + c, h - 2 * c, color);
    g.drawFastVLine(x1, y + c, h - 2 * c, color);
    g.drawLine(x, y + c, x + c, y, color);
    g.drawLine(x1 - c, y, x1, y + c, color);
    g.drawLine(x, y1 - c, x + c, y1, color);
    g.drawLine(x1 - c, y1, x1, y1 - c, color);
}

// LED cuadrado de estado (encendido = color, apagado = hueco)
inline void led(Arduino_GFX &g, int x, int y, bool on, uint16_t color) {
    if (on) g.fillRect(x, y, 6, 6, color);
    else g.drawRect(x, y, 6, 6, Theme::DIM);
}

// Texto con espaciado extra entre letras (etiquetas en mayúsculas pequeñas)
inline int spacedWidth(const AAFont &f, const char *s, int sp) {
    int w = 0, n = 0;
    for (const char *p = s; *p;) {
        char one[5] = {0};
        const char *q = p;
        TextAA::nextCodepoint(q);
        memcpy(one, p, q - p);
        w += TextAA::width(f, one);
        n++;
        p = q;
    }
    return w + (n > 0 ? (n - 1) * sp : 0);
}

inline int spaced(const AAFont &f, int x, int y, const char *s, uint16_t color, int sp = 2) {
    for (const char *p = s; *p;) {
        char one[5] = {0};
        const char *q = p;
        TextAA::nextCodepoint(q);
        memcpy(one, p, q - p);
        x = TextAA::draw(f, x, y, one, color) + sp;
        p = q;
    }
    return x - sp;
}

inline void spacedCentered(const AAFont &f, int cx, int y, const char *s, uint16_t color, int sp = 2) {
    spaced(f, cx - spacedWidth(f, s, sp) / 2, y, s, color, sp);
}

}  // namespace Panel
