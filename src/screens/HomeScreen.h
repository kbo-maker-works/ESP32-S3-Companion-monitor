#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include "../core/Pins.h"
#include "../core/Panel.h"
#include "../core/Icons.h"
#include "../core/Background.h"
#include "../core/NetManager.h"
#include "../core/TouchPanel.h"
#include "../core/StatusBar.h"
#include "../fonts/Fonts.h"

// Pantalla principal: barra de estado + lanzador de módulos 3x2, estilo panel
// de control. Cada app es un "módulo": marco fino con chaflanes, número de
// canal, LED, icono tintado con el acento y etiqueta en mayúsculas espaciadas.
// Pulsado = módulo invertido (relleno de acento, icono en negro), como una tecla.
// Las apps que aún no existen salen apagadas, en gris.
class HomeScreen {
public:
    struct App { const char *id, *label; bool ready; };

    // Orden en pantalla (fila a fila). id = nombre del icono en src/assets/icons o /icons de la SD
    // El reloj y el tiempo ya no son módulos: viven en el salvapantallas
    static constexpr int COUNT = 6;
    App apps[COUNT] = {
        {"icon_ai", "IA", false},     {"icon_sys", "SISTEMA", false}, {"icon_net", "RED", false},
        {"icon_rdr", "RADAR", false}, {"icon_aud", "AUDIO", false},   {"icon_cfg", "AJUSTES", true},
    };


    // ¿Hay que repintar? (cambio de minuto, de conexión o de tecla pulsada)
    bool needsRedraw(const TouchPanel &tp) {
        int key = StatusBar::stateKey();
        int pressed = tp.isDown() ? hit(tp.startX(), tp.startY()) : -1;
        // si el dedo se va lejos (deslizando), la tecla se suelta
        if (pressed >= 0 && (abs(tp.x() - tp.startX()) > 30 || abs(tp.y() - tp.startY()) > 30)) pressed = -1;
        bool dirty = key != _key || pressed != _pressed;
        _key = key;
        _pressed = pressed;
        return dirty;
    }

    void draw(Arduino_GFX &g) {
        Background::draw(Settings::get().bgHome.c_str(), 0x0000);
        Background::dim(140);
        StatusBar::draw(g);
        for (int i = 0; i < COUNT; i++) _module(g, i);
        _footer();
    }

    // Índice del módulo en (x, y) o -1
    int hit(int x, int y) const {
        for (int i = 0; i < COUNT; i++) {
            int mx, my;
            _xy(i, mx, my);
            if (x >= mx - GAP / 2 && x < mx + MW + GAP / 2 && y >= my - GAP / 2 && y < my + MH + GAP / 2) return i;
        }
        return -1;
    }

private:
    static constexpr int COLS = 3, GAP = 8, X0 = 12, Y0 = 46, CH = 7;
    static constexpr int MW = (SCREEN_W - 2 * X0 - (COLS - 1) * GAP) / COLS, MH = 118;
    int _key = -1, _pressed = -1;

    static void _xy(int i, int &x, int &y) {
        x = X0 + (i % COLS) * (MW + GAP);
        y = Y0 + (i / COLS) * (MH + GAP);
    }

    void _module(Arduino_GFX &g, int i) {
        const App &a = apps[i];
        int x, y;
        _xy(i, x, y);
        uint16_t acc = Theme::accent();
        bool down = i == _pressed;

        if (down) Panel::fill(x, y, MW, MH, CH, acc, 255);
        else Panel::fill(x, y, MW, MH, CH, 0x0000, 150);
        Panel::frame(g, x, y, MW, MH, CH, down ? acc : (a.ready ? Theme::FRAME : 0x2945));

        uint16_t ink = down ? 0x0000 : (a.ready ? acc : Theme::DIM);
        uint16_t txt = down ? 0x0000 : (a.ready ? Theme::TEXT : Theme::DIM);

        char ch[4];
        snprintf(ch, sizeof(ch), "%02d", i + 1);
        TextAA::draw(Nunito14, x + 10, y + 19, ch, down ? 0x0000 : Theme::MUTED);
        if (down) g.fillRect(x + MW - 16, y + 10, 6, 6, 0x0000);
        else Panel::led(g, x + MW - 16, y + 10, a.ready, acc);

        Icons::draw(a.id, x + (MW - 48) / 2, y + 26, ink);
        Panel::spacedCentered(Nunito14, x + MW / 2, y + MH - 14, a.label, txt, 2);
    }

    void _footer() {
        int y = SCREEN_H - 8;
        Panel::spaced(Nunito14, 14, y, "COMPANION // S3", 0x52AA, 1);
        String ip = NetManager::isConnected() ? NetManager::staIp() : String("AP ") + NetManager::apIp();
        TextAA::drawRight(Nunito14, SCREEN_W - 14, y, ip.c_str(), 0x52AA);
    }
};
