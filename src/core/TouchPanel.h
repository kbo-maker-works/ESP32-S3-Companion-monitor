#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Pins.h"
#include "../config.h"

// Calibración: si los toques salen en sitio raro, activa con TOUCH_DEBUG=1
// (pinta un punto donde detecta el dedo) y ajusta estos flags en config.h.
#ifndef TOUCH_FLIP_X
#define TOUCH_FLIP_X 0
#endif
#ifndef TOUCH_FLIP_Y
#define TOUCH_FLIP_Y 0
#endif

enum class Gesture { None, Tap, LongPress, SwipeLeft, SwipeRight, SwipeUp, SwipeDown };

inline const char *gestureName(Gesture g) {
    switch (g) {
        case Gesture::Tap: return "tap";
        case Gesture::LongPress: return "long";
        case Gesture::SwipeLeft: return "swipe-left";
        case Gesture::SwipeRight: return "swipe-right";
        case Gesture::SwipeUp: return "swipe-up";
        case Gesture::SwipeDown: return "swipe-down";
        default: return "none";
    }
}

// Táctil capacitivo del AXS15231B (I2C), en coordenadas lógicas 480x320.
class TouchPanel {
public:
    void begin() { Wire.begin(TP_SDA, TP_SCL, 400000); }

    // Llamar en cada loop. Devuelve el gesto terminado en esta vuelta (o None).
    Gesture update() {
        Gesture out = Gesture::None;
        int x, y;
        bool down = _read(x, y);
        uint32_t now = millis();

        if (down) {
            _lastSeenMs = now;
            _x = x; _y = y;
            if (!_pressed) {
                _pressed = true;
                _longFired = false;
                _startMs = now;
                _sx = x; _sy = y;
            }
            if (!_longFired && now - _startMs > LONG_PRESS_MS &&
                abs(_x - _sx) < MOVE_PX && abs(_y - _sy) < MOVE_PX) {
                _longFired = true;
                out = Gesture::LongPress;
            }
        } else if (_pressed && now - _lastSeenMs > RELEASE_MS) {
            _pressed = false;
            if (!_longFired) {
                int dx = _x - _sx, dy = _y - _sy;
                if (abs(dx) < SWIPE_PX && abs(dy) < SWIPE_PX) out = Gesture::Tap;
                else if (abs(dx) >= abs(dy)) out = dx < 0 ? Gesture::SwipeLeft : Gesture::SwipeRight;
                else out = dy < 0 ? Gesture::SwipeUp : Gesture::SwipeDown;
            }
        }
        return out;
    }

    bool isDown() const { return _pressed; }
    // Punto de inicio del gesto (para taps: dónde se tocó)
    int startX() const { return _sx; }
    int startY() const { return _sy; }
    // Último punto leído
    int x() const { return _x; }
    int y() const { return _y; }

private:
    static constexpr uint32_t LONG_PRESS_MS = 650;
    static constexpr uint32_t RELEASE_MS = 60;   // el controlador da lecturas vacías sueltas
    static constexpr int SWIPE_PX = 60;
    static constexpr int MOVE_PX = 25;

    bool _read(int &x, int &y) {
        static const uint8_t cmd[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};
        uint8_t b[8] = {0};
        Wire.beginTransmission(TP_ADDR);
        Wire.write(cmd, sizeof(cmd));
        if (Wire.endTransmission() != 0) return false;
        if (Wire.requestFrom((int)TP_ADDR, 8) != 8) return false;
        for (int i = 0; i < 8; i++) b[i] = Wire.read();
        uint8_t n = b[1];
        if (n == 0 || n > 2) return false;
        int nx = ((b[2] & 0x0F) << 8) | b[3];   // nativo 0..319
        int ny = ((b[4] & 0x0F) << 8) | b[5];   // nativo 0..479
        if (nx > 319 || ny > 479) return false;
        // Rotación 1 (apaisado), igual que el canvas
        x = ny;
        y = 319 - nx;
        if (TOUCH_FLIP_X) x = 479 - x;
        if (TOUCH_FLIP_Y) y = 319 - y;
        return true;
    }

    bool _pressed = false, _longFired = false;
    uint32_t _startMs = 0, _lastSeenMs = 0;
    int _sx = 0, _sy = 0, _x = 0, _y = 0;
};
