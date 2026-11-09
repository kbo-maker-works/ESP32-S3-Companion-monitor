#pragma once
#include <Arduino_GFX_Library.h>
#include "Colors.h"
#include "Pins.h"

// Misma interfaz que la versión C3, pero dibujando en Arduino_GFX (canvas
// 480x320). update() devuelve true si ha pintado algo, para que main.cpp
// solo vuelque el framebuffer cuando hace falta.
class Page {
public:
    virtual ~Page() = default;

    virtual const char *name() const = 0;

    // Al entrar: repintado completo.
    virtual void onEnter(Arduino_GFX &g) { (void)g; }

    virtual void onExit() {}

    // Cada vuelta del loop. Cada página se autolimita (millis()).
    virtual bool update(Arduino_GFX &g) = 0;

    // Pulsación larga: acción propia (cambiar esfera, refrescar...).
    virtual void onLongPress(Arduino_GFX &g) { (void)g; }

    // Toque corto en (x, y) lógico. Por defecto no hace nada.
    virtual void onTap(Arduino_GFX &g, int x, int y) { (void)g; (void)x; (void)y; }
};

// Helpers de texto con la fuente por defecto (6x8 px por carácter * size)
inline int16_t textW(const char *s, uint8_t size) { return (int16_t)strlen(s) * 6 * size; }

inline void printCentered(Arduino_GFX &g, const char *s, int16_t y, uint8_t size,
                          uint16_t fg, int16_t cx = SCREEN_W / 2) {
    g.setTextSize(size);
    g.setTextColor(fg);
    g.setCursor(cx - textW(s, size) / 2, y);
    g.print(s);
}
