#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include "Pins.h"
#include "Panel.h"
#include "NetManager.h"
#include "WeatherService.h"
#include "Power.h"
#include "Icons.h"
#include "../fonts/Fonts.h"

#ifndef BAT_ADC_PIN
#define BAT_ADC_PIN -1   // sin medición de batería confirmada en la JC3248W535
#endif

// Barra superior fija, la misma en todas las pantallas (alto HEIGHT px):
//   HH:MM  DÍA DD MES  ·  temperatura   ...   BAT  USB/PC  WIFI
// Las pantallas pintan su contenido debajo de HEIGHT.
class StatusBar {
public:
    static constexpr int HEIGHT = 40;

    static void setPcOnline(bool v) { _pc() = v; }
    static void setServerState(int8_t s) { _srv() = s; }   // -1 sin configurar, 0 caído, 1 ok

    // Nivel de señal 0..3, o -1 sin conexión. Con histéresis (4 dB) y como mucho
    // una lectura cada 2 s: si el RSSI baila en un umbral, el nivel no parpadea
    // (cada cambio obliga a repintar la barra, y en el radar a rehacer su capa).
    static int wifiLevel() {
        static int level = -1;
        static uint32_t at = 0;
        if (!NetManager::isConnected()) { level = -1; return -1; }
        if (level >= 0 && millis() - at < 2000) return level;
        at = millis();
        static const int TH[3] = {-78, -67, -55};   // umbrales para subir a 1, 2, 3
        int r = WiFi.RSSI();
        if (level < 0) level = r > TH[2] ? 3 : r > TH[1] ? 2 : r > TH[0] ? 1 : 0;
        while (level < 3 && r > TH[level] + 4) level++;
        while (level > 0 && r < TH[level - 1] - 4) level--;
        return level;
    }

    // Clave que cambia cuando hay que repintar (minuto, conexión, tiempo)
    static int stateKey() {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        const auto &w = WeatherService::data();
        return t.tm_min * 8 + (NetManager::isConnected() ? 1 : 0) + (_pc() ? 2 : 0) + (w.ok ? 4 : 0) + (Power::usbHost() ? 500 : 0) + (_srv() + 1) * 2000000 + (wifiLevel() + 1) * 100000 +
               (w.ok ? (int)lroundf(w.temp) * 1000 : 0);
    }

    // background=false: el fondo semitransparente ya está pintado (lo usa el radar,
    // que lo guarda en su capa fija para no recalcularlo)
    static void drawBackground() { Panel::fill(0, 0, SCREEN_W, HEIGHT - 2, 0, 0x0000, 120); }

    static void draw(Arduino_GFX &g, bool background = true) {
        uint16_t acc = Theme::accent();
        if (background) drawBackground();

        // Izquierda: hora, fecha y temperatura
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        char hm[8], date[24];
        static const char *DAYS[] = {"DOM", "LUN", "MAR", "MIÉ", "JUE", "VIE", "SÁB"};
        static const char *MONTHS[] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};
        if (t.tm_year > 120) {
            snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
            snprintf(date, sizeof(date), "%s %02d %s", DAYS[t.tm_wday], t.tm_mday, MONTHS[t.tm_mon]);
        } else {
            strcpy(hm, "--:--");
            strcpy(date, "SIN HORA");
        }
        int x = TextAA::draw(Nunito24, 14, 29, hm, acc);
        x = Panel::spaced(Nunito14, x + 12, 27, date, Theme::MUTED, 1);
        const auto &w = WeatherService::data();
        if (w.ok) {
            char tc[12];
            snprintf(tc, sizeof(tc), "%d°C", (int)lroundf(w.temp));
            g.drawFastVLine(x + 10, 14, 16, Theme::FRAME);
            TextAA::draw(Nunito14, x + 20, 27, tc, Theme::TEXT);
        }

        // Derecha (de izquierda a derecha): Wi-Fi, USB, servidor, batería.
        // Iconos del usuario icon_wifi/usb/cloud/bat (48 px) pintados a 24 px; el
        // estado va por color: rojo = desconectado, gris = apagado o sin dato.
        // Si falta un icono: etiqueta + LED (verde/rojo = estado, hueco = sin dato).
        int rx = SCREEN_W - 12;
        const int IY = (HEIGHT - 2 - 24) / 2;
        auto ind = [&](const char *label, int state /* 1 ok, 0 ko, -1 sin dato */) {
            rx -= 6;
            if (state < 0) g.drawRect(rx, 17, 6, 6, Theme::DIM);
            else g.fillRect(rx, 17, 6, 6, state ? Theme::OK : Theme::KO);
            rx -= 6 + Panel::spacedWidth(Nunito14, label, 1);
            Panel::spaced(Nunito14, rx, 27, label, state > 0 ? Theme::TEXT : Theme::MUTED, 1);
            rx -= 14;
        };
        auto icon = [&](const char *name, auto colorAt) {
            if (!Icons::get(name).data) return false;
            rx -= 24;
            Icons::drawHalf(name, rx, IY, colorAt);
            rx -= 12;
            return true;
        };
        auto flat = [](uint16_t c) { return [c](int, int) { return c; }; };

        // Solo se muestran los indicadores en uso; se compactan a la derecha (rx avanza
        // solo cuando se pinta algo).

        // Batería: oculta mientras no haya medición (BAT_ADC_PIN = -1 en la JC3248W535)
#if BAT_ADC_PIN >= 0
        if (!icon("icon_bat", flat(Theme::C_BAT))) ind("BAT", 1);
#endif

        // Servidor del Mac: oculto mientras no esté configurado. Cuando lo esté:
        // siempre visible, violeta si responde y rojo si no.
        if (_srv() >= 0 && !icon("icon_cloud", flat(_srv() ? Theme::C_SRV : Theme::KO))) ind("MAC", _srv());

        // USB: solo con un PC como host USB (por ahí habla el pc-agent)
        if (Power::usbHost() && !icon("icon_usb", flat(Theme::C_USB))) ind("USB", 1);

        // Wi-Fi: rojo sin conexión; con conexión se apagan los arcos según la señal
        int lv = wifiLevel();
        bool okW = icon("icon_wifi", [lv](int sx, int sy) {
            if (lv < 0) return Theme::KO;
            int dx = sx - 24, dy = sy - 38, d2 = dx * dx + dy * dy;   // centro de los arcos
            int arc = d2 < 25 ? 0 : d2 < 169 ? 1 : d2 < 441 ? 2 : 3;   // punto, arcos r9/r17/r25
            return arc <= lv ? Theme::C_WIFI : Theme::DIM;
        });
        if (!okW) ind("WIFI", lv >= 0 ? 1 : 0);


        g.drawFastHLine(10, HEIGHT - 2, SCREEN_W - 20, Theme::FRAME);
        g.drawFastHLine(10, HEIGHT - 2, 44, acc);
    }

private:
    static bool &_pc() { static bool v = false; return v; }
    // Estado del servidor del Mac: -1 sin configurar (oculto), 0 caído, 1 responde
    static int8_t &_srv() { static int8_t v = -1; return v; }
};
