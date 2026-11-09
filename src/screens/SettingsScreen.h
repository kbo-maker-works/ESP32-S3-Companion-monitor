#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <Arduino_GFX_Library.h>
#include "../core/Pins.h"
#include "../core/TextAA.h"
#include "../core/TouchPanel.h"
#include "../core/Settings.h"
#include "../core/Background.h"
#include "../core/NetManager.h"
#include "../core/Panel.h"
#include "../core/StatusBar.h"
#include "../fonts/Fonts.h"

// Ajustes en el propio dispositivo.
//
// Barra lateral con secciones (Fondo, Pantalla, Info) y contenido a la derecha,
// todo sobre el fondo de inicio oscurecido: al elegir otro fondo se ve el cambio
// detrás al momento. Para añadir una sección: nueva entrada en Section/LABELS,
// su _drawX() y su _tapX().
//
// Los cambios van a Settings (NVS) con los mismos bits CHG_* que usa el portal,
// así que pantalla y portal quedan sincronizados: main.cpp repinta al recibirlos.
class SettingsScreen {
public:
    enum class Result { None, Redraw, Exit };

    void enter() {
        _section = Section::Background;
        _page = 0;
        _scan();
        _pageOfSelected();
    }

    void exit() { _freeThumbs(); }

    // Repinta la pantalla entera en el framebuffer
    void draw(Arduino_GFX &g) {
        _g = &g;
        Background::draw(Settings::get().bgHome.c_str(), 0x0841);
        Background::dim(170);
        StatusBar::draw(g);
        _drawSidebar(g);
        switch (_section) {
            case Section::Background: _drawBackgrounds(g); break;
            case Section::Display:    _drawDisplay(g); break;
            case Section::Saver:      _drawSaver(g); break;
            case Section::Info:       _drawInfo(); break;
        }
    }

    Result handle(Gesture gs, int x, int y) {
        if (gs == Gesture::SwipeRight) return Result::Exit;
        if (_section == Section::Background && (gs == Gesture::SwipeUp || gs == Gesture::SwipeDown)) {
            int np = _page + (gs == Gesture::SwipeUp ? 1 : -1);
            if (np < 0 || np >= _pages()) return Result::None;
            _page = np;
            return Result::Redraw;
        }
        if (gs != Gesture::Tap) return Result::None;

        if (x < SIDEBAR_W) {
            if (y < T + 44) return Result::Exit;   // "Inicio"
            for (int i = 0; i < SECTIONS; i++) {
                int iy = ITEM_Y0 + i * ITEM_H;
                if (y >= iy && y < iy + ITEM_H - 4) {
                    if ((int)_section == i) return Result::None;
                    _section = (Section)i;
                    if (_section == Section::Background) { _scan(); _pageOfSelected(); }
                    return Result::Redraw;
                }
            }
            return Result::None;
        }
        switch (_section) {
            case Section::Background: return _tapBackgrounds(x, y);
            case Section::Display:    return _tapDisplay(x, y);
            case Section::Saver:      return _tapSaver(x, y);
            default:                  return Result::None;
        }
    }

    // Al cambiar el fondo desde el portal: la lista puede haber cambiado
    void refresh() {
        if (_section == Section::Background) _scan();
    }

private:
    enum class Section : uint8_t { Background, Display, Saver, Info };
    static constexpr int SECTIONS = 4;
    static constexpr const char *LABELS[SECTIONS] = {"FONDO", "PANTALLA", "REPOSO", "INFO"};

    static constexpr uint16_t TEXT = Theme::TEXT, MUTED = Theme::MUTED;
    static constexpr int T = StatusBar::HEIGHT;                 // la barra fija ocupa 0..T
    static constexpr int SIDEBAR_W = 140, ITEM_Y0 = T + 56, ITEM_H = 50;
    static constexpr int CX0 = 148, CX1 = 472;                  // zona de contenido
    static constexpr int TW = 96, TH = 64, GAP = 12, LABEL_H = 26, COLS = 3;   // miniaturas
    static constexpr int GRID_X = CX0 + (CX1 - CX0 - (COLS * TW + (COLS - 1) * GAP)) / 2, GRID_Y = T + 52;
    static constexpr int PER_PAGE = COLS * 2;

    struct Item { String path, label; uint16_t *thumb = nullptr; };

    Section _section = Section::Background;
    std::vector<Item> _items;
    int _page = 0;
    Arduino_GFX *_g = nullptr;

    // ---------------------------------------------------------------- barra
    void _drawSidebar(Arduino_GFX &g) {
        uint16_t acc = Theme::accent();
        Panel::fill(8, T + 6, SIDEBAR_W - 8, SCREEN_H - T - 14, 7, 0x0000, 160);
        Panel::frame(g, 8, T + 6, SIDEBAR_W - 8, SCREEN_H - T - 14, 7, Theme::FRAME);
        // Volver: chevron + texto
        for (int t = 0; t < 2; t++) {
            g.drawLine(24 + t, T + 23, 31 + t, T + 16, MUTED);
            g.drawLine(24 + t, T + 23, 31 + t, T + 30, MUTED);
        }
        Panel::spaced(Nunito14, 40, T + 29, "INICIO", MUTED, 2);
        g.drawFastHLine(16, T + 44, SIDEBAR_W - 24, Theme::FRAME);
        g.drawFastHLine(16, T + 44, 24, acc);
        for (int i = 0; i < SECTIONS; i++) {
            int iy = ITEM_Y0 + i * ITEM_H;
            bool on = (int)_section == i;
            if (on) Panel::fill(16, iy, SIDEBAR_W - 24, ITEM_H - 6, 5, acc, 255);   // tecla "encendida"
            else Panel::frame(g, 16, iy, SIDEBAR_W - 24, ITEM_H - 6, 5, Theme::FRAME);
            Panel::spaced(Nunito14, 28, iy + 29, LABELS[i], on ? 0x0000 : MUTED, 2);
        }
    }

    void _title(const char *t, const char *tag = nullptr) {
        Panel::spaced(Nunito18, CX0 + 4, T + 26, t, TEXT, 2);
        if (tag) TextAA::drawRight(Nunito14, CX1 - 4, T + 25, tag, MUTED);
        _g->drawFastHLine(CX0 + 4, T + 36, CX1 - CX0 - 8, Theme::FRAME);
        _g->drawFastHLine(CX0 + 4, T + 36, 24, Theme::accent());
    }

    // ---------------------------------------------------------------- fondo
    void _scan() {
        _freeThumbs();
        _items.clear();
        if (!Background::sdOk()) return;
        File dir = SD.open("/bg");
        if (!dir || !dir.isDirectory()) return;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String n = f.name();
            if (!f.isDirectory() && n.endsWith(".565") && f.size() == Background::BYTES) {
                Item it;
                it.path = "/bg/" + n;
                it.label = n.substring(0, n.length() - 4);
                it.label.replace('_', ' ');
                _items.push_back(it);
            }
            f.close();
        }
        dir.close();
        std::sort(_items.begin(), _items.end(), [](const Item &a, const Item &b) { return a.label < b.label; });
        if (_page >= _pages()) _page = max(0, _pages() - 1);
    }

    int _pages() const { return max(1, ((int)_items.size() + PER_PAGE - 1) / PER_PAGE); }

    void _pageOfSelected() {
        for (size_t i = 0; i < _items.size(); i++)
            if (_items[i].path == Settings::get().bgHome) { _page = i / PER_PAGE; return; }
    }

    void _freeThumbs() {
        for (auto &it : _items) { free(it.thumb); it.thumb = nullptr; }
    }

    // Miniatura TWxTH por muestreo: de la caché de fondos si está, si no de la SD
    static uint16_t *_makeThumb(const String &path) {
        uint16_t *t = (uint16_t *)ps_malloc(TW * TH * 2);
        if (!t) return nullptr;
        const uint16_t *src = Background::cached(path.c_str());
        if (src) {
            for (int ty = 0; ty < TH; ty++)
                for (int tx = 0; tx < TW; tx++)
                    t[ty * TW + tx] = src[Fb::idx(tx * 480 / TW, ty * 320 / TH)];
            return t;
        }
        File f = SD.open(path, FILE_READ);
        if (!f) { free(t); return nullptr; }
        static uint16_t row[480];
        for (int ty = 0; ty < TH; ty++) {
            f.seek((uint32_t)(ty * 320 / TH) * 480 * 2);
            if (f.read((uint8_t *)row, sizeof(row)) != sizeof(row)) { f.close(); free(t); return nullptr; }
            for (int tx = 0; tx < TW; tx++) t[ty * TW + tx] = row[tx * 480 / TW];
        }
        f.close();
        return t;
    }

    static void _blitRounded(const uint16_t *t, int x, int y, int r) {
        for (int j = 0; j < TH; j++)
            for (int i = 0; i < TW; i++) {
                int dx = 0, dy = 0;
                if (i < r) dx = r - i; else if (i >= TW - r) dx = i - (TW - r - 1);
                if (j < r) dy = r - j; else if (j >= TH - r) dy = j - (TH - r - 1);
                if (dx && dy && dx * dx + dy * dy > r * r) continue;
                Fb::ptr()[Fb::idx(x + i, y + j)] = t[j * TW + i];
            }
    }

    void _cellXY(int slot, int &x, int &y) const {
        x = GRID_X + (slot % COLS) * (TW + GAP);
        y = GRID_Y + (slot / COLS) * (TH + LABEL_H + 8);
    }

    void _drawBackgrounds(Arduino_GFX &g) {
        char tag[24] = "";
        if (_pages() > 1) snprintf(tag, sizeof(tag), "%d/%d  desliza ^v", _page + 1, _pages());
        else if (!_items.empty()) snprintf(tag, sizeof(tag), "%u fondos", (unsigned)_items.size());
        _title("FONDO DE INICIO", tag);

        if (_items.empty()) {
            int cx = (CX0 + CX1) / 2;
            TextAA::drawCentered(Nunito18, cx, 150, Background::sdOk() ? "No hay fondos en /bg" : "No hay microSD", TEXT);
            TextAA::drawCentered(Nunito14, cx, 180, "Súbelos desde el portal:", MUTED);
            TextAA::drawCentered(Nunito14, cx, 202, "http://" MDNS_NAME ".local", Theme::accent());
            return;
        }

        const String &cur = Settings::get().bgHome;
        for (int s = 0; s < PER_PAGE; s++) {
            int i = _page * PER_PAGE + s;
            if (i >= (int)_items.size()) break;
            Item &it = _items[i];
            int x, y;
            _cellXY(s, x, y);
            bool on = it.path == cur;
            if (on) Panel::fill(x - 4, y - 4, TW + 8, TH + 8, 6, Theme::accent(), 255);
            else Panel::frame(*_g, x - 2, y - 2, TW + 4, TH + 4, 4, Theme::FRAME);
            if (!it.thumb) it.thumb = _makeThumb(it.path);
            if (it.thumb) _blitRounded(it.thumb, x, y, 0);
            else Panel::fill(x, y, TW, TH, 0, 0x2104, 255);
            if (on) {   // marca de "en uso"
                int bx = x + TW - 26, by = y + 6;
                g.fillRect(bx, by, 20, 20, Theme::accent());
                for (int t = 0; t < 2; t++) {
                    g.drawLine(bx + 5, by + 10 + t, bx + 8, by + 13 + t, 0x0000);
                    g.drawLine(bx + 8, by + 13 + t, bx + 15, by + 6 + t, 0x0000);
                }
            }
            String lbl = it.label;
            while (lbl.length() > 3 && TextAA::width(Nunito14, lbl.c_str()) > TW) lbl.remove(lbl.length() - 1);
            TextAA::drawCentered(Nunito14, x + TW / 2, y + TH + 20, lbl.c_str(), on ? TEXT : MUTED);
        }
    }

    Result _tapBackgrounds(int x, int y) {
        for (int s = 0; s < PER_PAGE; s++) {
            int i = _page * PER_PAGE + s;
            if (i >= (int)_items.size()) break;
            int cx, cy;
            _cellXY(s, cx, cy);
            if (x >= cx - 6 && x < cx + TW + 6 && y >= cy - 6 && y < cy + TH + LABEL_H) {
                if (_items[i].path == Settings::get().bgHome) return Result::None;
                Settings::get().bgHome = _items[i].path;
                Settings::save();
                Settings::markChanged(Settings::CHG_BG);   // main repinta y avisa
                return Result::None;
            }
        }
        return Result::None;
    }

    // ---------------------------------------------------------------- pantalla
    static constexpr int BAR_X = CX0 + 58, BAR_X1 = CX1 - 58, BAR_Y = T + 74, BAR_H = 24, SEGS = 16;
    static constexpr int KEY_Y = T + 160, KEY_H = 48, KEY_W = (CX1 - CX0 - 8 - 12) / 2;

    void _drawDisplay(Arduino_GFX &g) {
        _title("PANTALLA");
        uint16_t acc = Theme::accent();
        uint8_t b = Settings::get().brightness;
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", (b * 100 + 127) / 255);
        Panel::spaced(Nunito14, CX0 + 4, T + 58, "BRILLO", MUTED, 2);
        TextAA::drawRight(Nunito18, CX1 - 4, T + 59, buf, acc);

        // Barra de segmentos, tipo vúmetro
        int on = (b * SEGS + 127) / 255;
        int sw = (BAR_X1 - BAR_X) / SEGS;
        for (int i = 0; i < SEGS; i++) {
            int x = BAR_X + i * sw;
            if (i < on) g.fillRect(x, BAR_Y, sw - 3, BAR_H, acc);
            else g.drawRect(x, BAR_Y, sw - 3, BAR_H, Theme::DIM);
        }
        _key(g, CX0 + 4, BAR_Y + BAR_H / 2 - 22, 44, 44, "-", false);
        _key(g, CX1 - 48, BAR_Y + BAR_H / 2 - 22, 44, 44, "+", false);

        Panel::spaced(Nunito14, CX0 + 4, KEY_Y - 14, "ACENTO", MUTED, 2);
        for (int a = 0; a < 2; a++) {
            int x = CX0 + 4 + a * (KEY_W + 12);
            bool sel = Settings::get().accent == a;
            _key(g, x, KEY_Y, KEY_W, KEY_H, Theme::accentName(a), sel, a ? Theme::AMBER : Theme::CYAN);
        }
        TextAA::draw(Nunito14, CX0 + 4, SCREEN_H - 16, "También se cambia desde el portal.", 0x52AA);
    }

    // Tecla de panel: marco con chaflán; seleccionada = rellena (invertida)
    static void _key(Arduino_GFX &g, int x, int y, int w, int h, const char *label, bool sel, uint16_t color = 0) {
        uint16_t acc = color ? color : Theme::TEXT;
        if (sel) Panel::fill(x, y, w, h, 6, acc, 255);
        else {
            Panel::fill(x, y, w, h, 6, 0x0000, 120);
            Panel::frame(g, x, y, w, h, 6, color ? Theme::FRAME : Theme::MUTED);
        }
        int lw = Panel::spacedWidth(Nunito18, label, 2);
        int tx = x + (w - lw) / 2;
        if (color && !sel) {   // muestra del color
            g.fillRect(tx - 16, y + h / 2 - 4, 8, 8, color);
            tx += 4;
        }
        Panel::spaced(Nunito18, tx, y + h / 2 + 7, label, sel ? 0x0000 : Theme::TEXT, 2);
    }

    Result _tapDisplay(int x, int y) {
        if (y >= KEY_Y - 4 && y < KEY_Y + KEY_H + 4) {
            for (int a = 0; a < 2; a++) {
                int kx = CX0 + 4 + a * (KEY_W + 12);
                if (x >= kx && x < kx + KEY_W && Settings::get().accent != a) {
                    Settings::get().accent = a;
                    Settings::save();
                    Settings::markChanged(Settings::CHG_THEME);
                    return Result::Redraw;
                }
            }
            return Result::None;
        }
        int cy = BAR_Y + BAR_H / 2;
        if (abs(y - cy) > 30) return Result::None;
        int b = Settings::get().brightness;
        if (x < CX0 + 52) b -= 255 / SEGS;
        else if (x > CX1 - 52) b += 255 / SEGS;
        else if (x >= BAR_X - 6 && x <= BAR_X1 + 6) b = (x - BAR_X + (BAR_X1 - BAR_X) / SEGS) * 255 / (BAR_X1 - BAR_X);
        else return Result::None;
        b = constrain(b, 5, 255);
        if (b == Settings::get().brightness) return Result::None;
        Settings::get().brightness = b;
        Settings::save();
        Settings::markChanged(Settings::CHG_BRIGHT);
        return Result::Redraw;
    }

    // ---------------------------------------------------------------- reposo
    // Salvapantallas: sale tras X minutos sin tocar. No toca el brillo, así que
    // también funciona con el PC por USB (la regla de Power solo prohíbe atenuar).
    static constexpr uint8_t SS_MINS[] = {1, 2, 5, 10, 30};
    static constexpr int SS_N = sizeof(SS_MINS);
    static constexpr uint16_t SS_SECS[] = {10, 30, 60, 300, 900};
    static constexpr const char *SS_SECS_L[] = {"10 S", "30 S", "1 M", "5 M", "15 M"};
    static constexpr int SP_N = sizeof(SS_SECS) / sizeof(SS_SECS[0]);
    static constexpr int OPT_H = 44, OPT_GAP = 8;
    static constexpr int SS_MODE_Y = T + 56, SS_MIN_Y = T + 128, SS_SEC_Y = T + 200;

    static int _optW(int n) { return (CX1 - CX0 - 8 - (n - 1) * OPT_GAP) / n; }
    static int _optX(int i, int n) { return CX0 + 4 + i * (_optW(n) + OPT_GAP); }

    // Opción de panel: marco fino; elegida = rellena de acento (invertida); off = atenuada
    static void _opt(Arduino_GFX &g, int x, int y, int w, const char *label, bool sel, bool enabled = true) {
        if (sel && enabled) Panel::fill(x, y, w, OPT_H, 5, Theme::accent(), 255);
        else {
            Panel::fill(x, y, w, OPT_H, 5, 0x0000, 120);
            Panel::frame(g, x, y, w, OPT_H, 5, sel ? Theme::MUTED : Theme::FRAME);
        }
        uint16_t c = sel && enabled ? 0x0000 : (enabled ? Theme::TEXT : Theme::DIM);
        Panel::spacedCentered(Nunito14, x + w / 2, y + OPT_H / 2 + 5, label, c, 2);
    }

    // Modo efectivo: 0 apagado, 1 reloj, 2 fotos
    static int _ssMode() {
        const auto &s = Settings::get();
        return !s.ssOn ? 0 : s.ssMode == 1 ? 2 : 1;
    }

    void _drawSaver(Arduino_GFX &g) {
        _title("SALVAPANTALLAS");
        const auto &s = Settings::get();
        int m = _ssMode();
        static const char *MODES[] = {"APAGADO", "RELOJ", "FOTOS"};
        Panel::spaced(Nunito14, CX0 + 4, SS_MODE_Y - 10, "MOSTRAR", MUTED, 2);
        for (int i = 0; i < 3; i++) _opt(g, _optX(i, 3), SS_MODE_Y, _optW(3), MODES[i], m == i);

        Panel::spaced(Nunito14, CX0 + 4, SS_MIN_Y - 10, "TRAS (MIN SIN TOCAR)", MUTED, 2);
        for (int i = 0; i < SS_N; i++) {
            char b[4];
            snprintf(b, sizeof(b), "%u", SS_MINS[i]);
            _opt(g, _optX(i, SS_N), SS_MIN_Y, _optW(SS_N), b, s.ssMin == SS_MINS[i], m != 0);
        }

        Panel::spaced(Nunito14, CX0 + 4, SS_SEC_Y - 10, "CAMBIAR FOTO CADA", MUTED, 2);
        for (int i = 0; i < SP_N; i++)
            _opt(g, _optX(i, SP_N), SS_SEC_Y, _optW(SP_N), SS_SECS_L[i], s.ssPhotoSec == SS_SECS[i], m == 2);
        TextAA::draw(Nunito14, CX0 + 4, SCREEN_H - 10, "Fotos: portal > microSD > Subir fotos (/fotos).", 0x52AA);
    }

    Result _tapSaver(int x, int y) {
        auto &s = Settings::get();
        auto in = [&](int oy) { return y >= oy - 4 && y < oy + OPT_H + 4; };
        auto col = [&](int n) {
            for (int i = 0; i < n; i++)
                if (x >= _optX(i, n) && x < _optX(i, n) + _optW(n)) return i;
            return -1;
        };
        bool changed = false;
        if (in(SS_MODE_Y)) {
            int c = col(3);
            if (c >= 0 && c != _ssMode()) {
                s.ssOn = c != 0;
                if (c) s.ssMode = c == 2 ? 1 : 0;
                changed = true;
            }
        } else if (in(SS_MIN_Y) && _ssMode() != 0) {
            int c = col(SS_N);
            if (c >= 0 && s.ssMin != SS_MINS[c]) { s.ssMin = SS_MINS[c]; changed = true; }
        } else if (in(SS_SEC_Y) && _ssMode() == 2) {
            int c = col(SP_N);
            if (c >= 0 && s.ssPhotoSec != SS_SECS[c]) { s.ssPhotoSec = SS_SECS[c]; changed = true; }
        }
        if (!changed) return Result::None;
        Settings::save();
        Settings::markChanged(Settings::CHG_SAVER);
        return Result::Redraw;
    }

    // ---------------------------------------------------------------- info
    void _drawInfo() {   // etiqueta a la izquierda, valor a la derecha
        _title("INFO");
        char up[24];
        uint32_t s = millis() / 1000;
        snprintf(up, sizeof(up), "%lud %luh %lum", (unsigned long)(s / 86400), (unsigned long)(s / 3600 % 24),
                 (unsigned long)(s / 60 % 60));
        String wifi = NetManager::isConnected() ? NetManager::savedSsid() + " · " + NetManager::staIp() : String("Sin conexión");
        String sd = Background::sdOk() ? String((unsigned long)(Background::cardMB() / 1024)) + " GB" : String("No hay tarjeta");
        String psram = String(ESP.getFreePsram() / 1024) + " KB libres";
        const String rows[][2] = {
            {"Wi-Fi", wifi},
            {"Portal", String("http://") + NetManager::mdnsHost()},
            {"Punto de acceso", String(NetManager::apSsid()) + " · " + NetManager::apIp()},
            {"microSD", sd},
            {"PSRAM", psram},
            {"Encendido", String(up)},
        };
        int y = T + 68;
        for (auto &r : rows) {
            TextAA::draw(Nunito14, CX0 + 4, y, r[0].c_str(), MUTED);
            _g->drawFastHLine(CX0 + 4, y + 9, CX1 - CX0 - 8, 0x2945);
            TextAA::drawRight(Nunito14, CX1 - 4, y, r[1].c_str(), TEXT);
            y += 32;
        }
    }
};
