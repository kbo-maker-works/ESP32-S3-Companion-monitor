#pragma once
#include <Arduino_GFX_Library.h>
#include "Pins.h"
#include "TextAA.h"

// Pantalla con framebuffer completo en PSRAM.
// Las páginas pintan en el canvas (480x320 apaisado) y DisplayManager lo
// vuelca entero con flush(). Así no hay parpadeo, y además esquivamos el
// problema del AXS15231B por QSPI con los refrescos parciales.
class DisplayManager {
public:
    bool begin(uint8_t brightness = 255) {
        _bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
        _panel = new Arduino_AXS15231B(_bus, GFX_NOT_DEFINED /* RST */, 0 /* rot */, false /* IPS */, 320, 480);
        // Canvas nativo 320x480 con rotación 1 -> 480x320 lógico.
        _canvas = new Arduino_Canvas(320, 480, _panel, 0, 0, 1);

        if (!_canvas->begin()) {
            Serial.println("[Display] begin() fallo: revisa PSRAM (qio_opi) y pines");
            return false;
        }
        Fb::ptr() = _canvas->getFramebuffer();   // para TextAA / Background
        _canvas->fillScreen(0x0000);
        _canvas->flush();

        ledcAttach(LCD_BL, 5000, 8);
        setBacklight(brightness);
        Serial.printf("[Display] OK %dx%d, PSRAM libre: %u\n",
                      _canvas->width(), _canvas->height(), ESP.getFreePsram());
        return true;
    }

    Arduino_GFX &gfx() { return *_canvas; }

    void flush() { _canvas->flush(); }

    // Vuelca solo las primeras `rows` filas NATIVAS del panel (= columnas lógicas
    // x 0..rows-1, a todo lo alto). Siempre desde la fila 0: el AXS15231B por QSPI
    // no respeta bien el inicio de ventana en refrescos parciales, pero
    // empezando en 0 funciona. Para animaciones que solo tocan la parte izquierda.
    void flushRows(int rows) {
        if (rows >= 480) { flush(); return; }
        _panel->draw16bitRGBBitmap(0, 0, _canvas->getFramebuffer(), 320, rows);
    }

    void setBacklight(uint8_t v) { ledcWrite(LCD_BL, v); }

private:
    Arduino_DataBus *_bus = nullptr;
    Arduino_GFX *_panel = nullptr;
    Arduino_Canvas *_canvas = nullptr;
};
