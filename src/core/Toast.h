#pragma once
#include <Arduino.h>
#include "Pins.h"
#include "TextAA.h"
#include "../fonts/Fonts.h"

// Aviso flotante abajo, encima de cualquier pantalla (home o apps).
// main.cpp lo pinta después de la página en cada frame que se repinta, y
// cuando caduca fuerza un repintado completo para borrarlo.
class Toast {
public:
    static void show(const String &title, const String &sub = "", uint32_t ms = 1800, uint16_t accent = 0x07FF) {
        _title() = title;
        _sub() = sub;
        _accent() = accent;
        _until() = millis() + ms;
        _fresh() = true;
    }

    static bool visible() { return _until() != 0; }

    // true una vez cuando aparece un aviso nuevo (hay que pintarlo)
    static bool takeFresh() {
        bool v = _fresh();
        _fresh() = false;
        return v;
    }

    // true una vez cuando caduca (hay que repintar la página debajo)
    static bool takeExpired() {
        if (!_until() || (int32_t)(millis() - _until()) < 0) return false;
        _until() = 0;
        return true;
    }

    static void draw() {
        if (!visible()) return;
        const char *t = _title().c_str(), *s = _sub().c_str();
        bool two = !_sub().isEmpty();
        int w = max(TextAA::width(Nunito18, t), two ? TextAA::width(Nunito14, s) : 0) + 48;
        if (w > SCREEN_W - 24) w = SCREEN_W - 24;
        int h = two ? 62 : 40;
        int x = (SCREEN_W - w) / 2, y = SCREEN_H - h - 18;

        Fb::fillRoundRectAlpha(x, y, w, h, 20, 0x0000, 225);
        Fb::fillRoundRectAlpha(x + 14, y + h / 2 - 4, 8, 8, 4, _accent(), 255);   // punto de color
        int cx = SCREEN_W / 2 + 8;
        if (two) {
            TextAA::drawCentered(Nunito18, cx, y + 27, t, 0xF77D);
            TextAA::drawCentered(Nunito14, cx, y + 49, s, 0x9CF3);
        } else {
            TextAA::drawCentered(Nunito18, cx, y + 27, t, 0xF77D);
        }
    }

private:
    static String &_title() { static String v; return v; }
    static String &_sub() { static String v; return v; }
    static uint16_t &_accent() { static uint16_t v = 0x07FF; return v; }
    static uint32_t &_until() { static uint32_t v = 0; return v; }
    static bool &_fresh() { static bool v = false; return v; }
};
