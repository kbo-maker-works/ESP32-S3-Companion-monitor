#pragma once
#include <Arduino.h>

// Texto con antialiasing (alpha 4 bits) pintado directamente sobre el
// framebuffer del canvas, mezclando con lo que haya debajo (fondos, tiles...).
// Las fuentes se generan con tools/make_aafont.py y van compiladas en flash.

struct AAGlyph {
    uint32_t offset;   // en el bitmap (nibbles empaquetados, 2 px por byte)
    uint8_t w, h;
    uint8_t adv;       // avance horizontal
    int8_t xo, yo;     // offset desde el origen (x, línea base)
};

struct AAFont {
    const uint8_t *bitmap;
    const AAGlyph *glyphs;
    uint16_t first, last;
    uint8_t lineHeight;
    uint8_t ascent;
};

namespace Fb {
// Framebuffer nativo del panel (320x480 vertical). Lo fija DisplayManager.
inline uint16_t *&ptr() { static uint16_t *p = nullptr; return p; }
constexpr int NATIVE_W = 320;
constexpr int W = 480, H = 320;   // lógico (apaisado, rotación 1)

// Lógico (x,y) -> índice nativo. Rotación 1: nx = 319 - y, ny = x
inline uint32_t idx(int x, int y) { return (uint32_t)x * NATIVE_W + (NATIVE_W - 1 - y); }

inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a /*0-255*/) {
    if (a >= 250) return fg;
    if (a <= 4) return bg;
    uint32_t inv = 255 - a;
    uint32_t r = (((fg >> 11) & 0x1F) * a + ((bg >> 11) & 0x1F) * inv) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * inv) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * inv) / 255;
    return (r << 11) | (g << 5) | b;
}

inline void blendPixel(int x, int y, uint16_t c, uint8_t a) {
    if (!ptr() || x < 0 || y < 0 || x >= W || y >= H) return;
    uint16_t &p = ptr()[idx(x, y)];
    p = blend(p, c, a);
}

// Rectángulo redondeado semitransparente (para tiles sobre el fondo)
inline void fillRoundRectAlpha(int x, int y, int w, int h, int r, uint16_t c, uint8_t a) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = 0, dy = 0;
            if (i < r) dx = r - i; else if (i >= w - r) dx = i - (w - r - 1);
            if (j < r) dy = r - j; else if (j >= h - r) dy = j - (h - r - 1);
            if (dx && dy && dx * dx + dy * dy > r * r) continue;
            blendPixel(x + i, y + j, c, a);
        }
    }
}
}  // namespace Fb

namespace TextAA {

// Decodifica UTF-8 (hasta 2 bytes basta para Latin-1: ñ, á, ¿, °...)
inline uint16_t nextCodepoint(const char *&s) {
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && (*s & 0xC0) == 0x80) return ((c & 0x1F) << 6) | ((uint8_t)*s++ & 0x3F);
    while ((*s & 0xC0) == 0x80) s++;   // secuencia más larga: se salta
    return '?';
}

inline const AAGlyph *glyph(const AAFont &f, uint16_t cp) {
    if (cp < f.first || cp > f.last) return nullptr;
    return &f.glyphs[cp - f.first];
}

inline int width(const AAFont &f, const char *s) {
    int w = 0;
    while (*s) {
        const AAGlyph *g = glyph(f, nextCodepoint(s));
        if (g) w += g->adv;
    }
    return w;
}

// y = línea base. Devuelve la x final.
inline int draw(const AAFont &f, int x, int y, const char *s, uint16_t color) {
    while (*s) {
        const AAGlyph *g = glyph(f, nextCodepoint(s));
        if (!g) continue;
        const uint8_t *bm = f.bitmap + g->offset;
        uint32_t n = 0;
        for (int j = 0; j < g->h; j++) {
            for (int i = 0; i < g->w; i++, n++) {
                uint8_t byte = bm[n >> 1];
                uint8_t a4 = (n & 1) ? (byte & 0x0F) : (byte >> 4);
                if (a4) Fb::blendPixel(x + g->xo + i, y + g->yo + j, color, a4 * 17);
            }
        }
        x += g->adv;
    }
    return x;
}

// Alineaciones habituales. y = línea base.
inline void drawCentered(const AAFont &f, int cx, int y, const char *s, uint16_t c) {
    draw(f, cx - width(f, s) / 2, y, s, c);
}
inline void drawRight(const AAFont &f, int rx, int y, const char *s, uint16_t c) {
    draw(f, rx - width(f, s), y, s, c);
}
// Con sombra suave (texto sobre fondos con detalle)
inline void drawShadow(const AAFont &f, int x, int y, const char *s, uint16_t c) {
    const char *p = s;
    int xx = x + 1;
    while (*p) {   // sombra
        const AAGlyph *g = glyph(f, nextCodepoint(p));
        if (!g) continue;
        const uint8_t *bm = f.bitmap + g->offset;
        uint32_t n = 0;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++, n++) {
                uint8_t byte = bm[n >> 1];
                uint8_t a4 = (n & 1) ? (byte & 0x0F) : (byte >> 4);
                if (a4) Fb::blendPixel(xx + g->xo + i, y + 1 + g->yo + j, 0x0000, a4 * 10);
            }
        xx += g->adv;
    }
    draw(f, x, y, s, c);
}

}  // namespace TextAA
