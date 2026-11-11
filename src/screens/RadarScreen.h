#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include "../core/Pins.h"
#include "../core/Panel.h"
#include "../core/Background.h"
#include "../core/StatusBar.h"
#include "../core/FlightService.h"
#include "../core/TouchPanel.h"
#include "../core/Toast.h"
#include "../core/Prof.h"
#include "../fonts/Fonts.h"

// RADAR: tráfico aéreo alrededor de la ubicación de Ajustes (FlightService).
//
// Radar circular verde fósforo con barrido de ~4 s y estela de 70°; cada avión
// se ilumina cuando pasa el barrido y se apaga poco a poco. Norte arriba, centro
// = nuestra posición. Panel lateral con los más cercanos y el detalle del
// seleccionado (tocar un avión o una fila).
//
// Rendimiento: la parte que no se mueve (fondo, barra, disco, anillos, panel)
// se pinta una vez en una capa en PSRAM y solo se rehace cuando cambian los
// datos; cada fotograma copia la capa y añade barrido + blips (~25 fps). El
// ángulo de cada píxel del disco está precalculado en una tabla.
class RadarScreen {
public:
    enum class Result { None, Redraw, Exit };

    void enter() {
        _layer = (uint16_t *)ps_malloc(Background::BYTES);
        _base = (uint16_t *)ps_malloc(Background::BYTES);
        _baseDirty = true;
        _lut = (uint16_t *)ps_malloc(LUT_S * LUT_S * 2);
        if (_lut) _buildLut();
        _sel[0] = 0;
        _seq = 0xFFFFFFFF;
        _barKey = -1;
        _dirty = true;
        _lastState = FlightService::State::Idle;
        for (auto &t : _litAt) t = 0;
        FlightService::setActive(true);
    }

    void exit() {
        FlightService::setActive(false);
        free(_layer);
        free(_base);
        _base = nullptr;
        free(_lut);
        _layer = nullptr;
        _lut = nullptr;
    }

    // Llamar en cada vuelta: true si ha pintado un fotograma
    bool update(Arduino_GFX &g, bool force) {
        uint32_t now = millis();
        if (!force && now - _frameAt < FRAME_MS) return false;
        _frameAt = now;

        // Copia nueva: el brillo de cada avión sigue a su callsign, no a su posición en la lista
        char prevCall[FlightService::MAX][10];
        uint32_t prevLit[FlightService::MAX];
        int prevN = _snap.count;
        for (int i = 0; i < prevN; i++) { strlcpy(prevCall[i], _snap.planes[i].call, 10); prevLit[i] = _litAt[i]; }
        { PROF(SNAP); FlightService::snapshot(_snap, Settings::get().radarGnd); }
        if (_snap.seq != _seq) {
            for (int i = 0; i < _snap.count; i++) {
                _litAt[i] = 0;
                for (int k = 0; k < prevN; k++)
                    if (!strcmp(prevCall[k], _snap.planes[i].call)) { _litAt[i] = prevLit[k]; break; }
            }
            _seq = _snap.seq;
            _dirty = true;
            if (_snap.state == FlightService::State::Error && _lastState != FlightService::State::Error)
                Toast::show(_snap.httpCode >= 400 && _snap.httpCode < 500 ? "Radar: la API rechaza la petición" : "Radar: la API no responde",
                            _snap.httpCode > 0 ? "HTTP " + String(_snap.httpCode) : "", 2500, Theme::KO);
            _lastState = _snap.state;
        }
        int bk = StatusBar::stateKey();
        if (bk != _barKey) { _barKey = bk; _dirty = true; }
        if (force) _dirty = true;

        if (!_layer || !_base || !_lut) {   // sin memoria: pantalla mínima
            g.fillScreen(0x0000);
            StatusBar::draw(g);
            TextAA::drawCentered(Nunito18, SCREEN_W / 2, 180, "Sin memoria para el radar", Theme::KO);
            return true;
        }
        if (_dirty) {
            PROF(BUILD);
            if (_baseDirty) { _buildBase(g); _baseDirty = false; }
            _buildLayer(g);
            _dirty = false;
            _needFull = true;
            _justBuilt = true;
        }
        // Si solo se mueve el radar, basta con restaurar y volcar su franja (x < PART).
        // Con un aviso en pantalla o la capa recién hecha, va entero.
        _full = _needFull || Toast::visible();
        _needFull = false;
        // Recién hecha, la capa ya está en el framebuffer: no hace falta copiarla otra vez
        if (!_justBuilt) { PROF(COPY); memcpy(Fb::ptr(), _layer, _full ? Background::BYTES : (size_t)PART * 320 * 2); }
        _justBuilt = false;
        { PROF(SWEEP); _drawSweep(now); }
        { PROF(PLANES); _drawPlanes(g, now); }
        return true;
    }

    Result handle(Gesture gs, int x, int y) {
        if (gs == Gesture::SwipeRight) return Result::Exit;
        if (gs != Gesture::Tap) return Result::None;

        // Volver
        // Cabecera del panel: chip del filtro (derecha) y volver (izquierda)
        if (x >= PX && y >= BACK_Y - 4 && y < BACK_Y + BACK_H + 4) {
            if (x >= CHIP_X - 6) {
                auto &s = Settings::get();
                s.radarGnd = (s.radarGnd + 1) % 3;   // VUELO -> TIERRA -> TODOS
                Settings::save();
                Settings::markChanged(Settings::CHG_RGND);
                return Result::Redraw;
            }
            return Result::Exit;
        }

        // Pie del radar: cambia el radio 10 -> 25 -> 50 -> 100 km
        if (y >= CY + R - 4 && x < PX - 8) {
            static const uint8_t KMS[] = {10, 25, 50, 100};
            auto &s = Settings::get();
            int i = 0;
            while (i < 4 && KMS[i] != s.radarKm) i++;
            s.radarKm = KMS[(i + 1) % 4];
            Settings::save();
            Settings::markChanged(Settings::CHG_RADAR);
            return Result::Redraw;
        }

        // Fila de la lista
        if (x >= PX && y >= LIST_Y && y < LIST_Y + ROWS * ROW_H) {
            int r = (y - LIST_Y) / ROW_H;
            if (r < _snap.count) _toggle(_snap.planes[r].call);
            return Result::Redraw;
        }

        // Blip en el radar: el más cercano al dedo (hasta 24 px)
        int best = -1, bestD = 24 * 24;
        for (int i = 0; i < _snap.count; i++) {
            int px, py;
            _planeXY(_snap.planes[i], millis(), px, py);
            int d = (px - x) * (px - x) + (py - y) * (py - y);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (best >= 0) _toggle(_snap.planes[best].call);
        else if (_sel[0]) _sel[0] = 0;
        else return Result::None;
        return Result::Redraw;
    }

    // Filas nativas que hay que volcar este fotograma (480 = pantalla entera)
    int flushRows() const { return _full ? 480 : PART; }

    // Cambió el fondo o el acento: rehacer también la parte fija
    void invalidateBase() { _baseDirty = _dirty = true; }

    // Tras cambiar el radio (aquí o en el portal)
    void radiusChanged() {
        for (auto &t : _litAt) t = 0;
        FlightService::refreshSoon();
        _baseDirty = _dirty = true;   // anillos y pie cambian
    }

private:
    // Geometría (480x320, barra arriba de StatusBar::HEIGHT)
    static constexpr int T = StatusBar::HEIGHT;
    static constexpr int R = 122;                      // radio del disco
    static constexpr int CX = 14 + R, CY = T + 12 + R;  // centro
    static constexpr int GLOW = 4;
    static constexpr int LUT_R = R + GLOW, LUT_S = 2 * LUT_R + 1;
    static constexpr int PX = 286, PX1 = 470;          // panel lateral
    static constexpr int BACK_Y = T + 8, BACK_H = 34;
    static constexpr int CHIP_X = PX1 - 76;            // chip VUELO/TIERRA/TODOS
    static constexpr int LIST_Y = T + 74, ROW_H = 24, ROWS = 5;
    static constexpr int DET_Y = LIST_Y + ROWS * ROW_H + 10;
    static constexpr uint32_t FRAME_MS = 40;           // ~25 fps
    static constexpr uint32_t SWEEP_MS = 4000;         // una vuelta
    static constexpr int ANG = 1024;                   // unidades de ángulo por vuelta
    static constexpr int TRAIL = ANG * 70 / 360;       // estela de 70°
    static constexpr uint16_t OUT = 0xFFFF;            // fuera del disco en la tabla
    static constexpr int PART = PX - 6;                // franja izquierda (radar) en volcados parciales

    // Colores del demo
    static constexpr uint16_t GREEN = 0x37B4;   // #34F5A0
    static constexpr uint16_t BLIP  = 0x7FF9;   // #7DFFC8
    static constexpr uint16_t LABEL = 0x9FB9;   // #9DF7CF
    // Por tipo (FlightService::Kind): comercial verde, militar rojo, carga azul
    static constexpr uint16_t KIND_COL[3] = {BLIP, 0xFACB, 0x5D3F};   // #7DFFC8 #FF5A5A #58A6FF
    static constexpr const char *KIND_NAME[3] = {"COMERCIAL", "MILITAR", "CARGA"};
    static constexpr const char *KIND_SHORT[3] = {"COM", "MIL", "CAR"};
    static constexpr uint16_t FOOT  = 0x7D53;   // #7FA89A
    static constexpr uint16_t DISC  = 0x0082;   // verde casi negro

    uint16_t *_layer = nullptr, *_base = nullptr, *_lut = nullptr;
    bool _baseDirty = true, _needFull = true, _full = true, _justBuilt = false;
    FlightService::Snapshot _snap;
    FlightService::State _lastState = FlightService::State::Idle;
    uint32_t _seq = 0, _frameAt = 0;
    uint32_t _litAt[FlightService::MAX];
    int _prevSweep = 0;
    int _barKey = -1;
    bool _dirty = true;
    char _sel[10] = "";

    void _toggle(const char *call) {
        if (!strcmp(_sel, call)) _sel[0] = 0;
        else strlcpy(_sel, call, sizeof(_sel));
    }

    // ------------------------------------------------------------ geometría
    void _buildLut() {
        for (int j = 0; j < LUT_S; j++)
            for (int i = 0; i < LUT_S; i++) {
                int dx = i - LUT_R, dy = j - LUT_R;
                uint16_t v = OUT;
                if (dx * dx + dy * dy <= R * R) {
                    float a = atan2f((float)dx, (float)-dy);   // 0 = norte, sentido horario
                    if (a < 0) a += 2 * PI;
                    v = (uint16_t)(a / (2 * PI) * ANG) % ANG;
                }
                _lut[j * LUT_S + i] = v;
            }
    }

    // Posición en pantalla, extrapolando con velocidad y rumbo desde la última lectura
    void _planeXY(const FlightService::Plane &p, uint32_t now, int &x, int &y) const {
        float b = p.bearing * DEG_TO_RAD;
        float ex = p.distKm * sinf(b), ny = p.distKm * cosf(b);
        if (p.track >= 0 && p.gsKt > 0 && _snap.at) {
            float t = min(30.0f, (now - _snap.at) / 1000.0f);
            float km = p.gsKt * 1.852f / 3600.0f * t;
            ex += km * sinf(p.track * DEG_TO_RAD);
            ny += km * cosf(p.track * DEG_TO_RAD);
        }
        float k = (R - 6) / (float)Settings::get().radarKm;
        x = CX + (int)lroundf(ex * k);
        y = CY - (int)lroundf(ny * k);
    }

    static int _angleOf(int x, int y) {
        float a = atan2f((float)(x - CX), (float)(CY - y));
        if (a < 0) a += 2 * PI;
        return (int)(a / (2 * PI) * ANG) % ANG;
    }

    // ------------------------------------------------------------ capa estática
    // Parte fija: fondo, disco, anillos y pie. Solo cambia con el radio, el fondo o el acento.
    void _buildBase(Arduino_GFX &g) {
        Background::draw(Settings::get().bgHome.c_str(), 0x0000);
        Background::dim(200);
        // Fondos de la barra y del panel (fijos); su contenido va en _buildLayer
        StatusBar::drawBackground();
        Panel::fill(PX - 6, T + 4, PX1 - PX + 12, SCREEN_H - T - 10, 7, 0x0000, 170);
        Panel::frame(g, PX - 6, T + 4, PX1 - PX + 12, SCREEN_H - T - 10, 7, Theme::FRAME);

        // Disco: fondo, glow del borde, anillos y cruz
        for (int j = 0; j < LUT_S; j++)
            for (int i = 0; i < LUT_S; i++) {
                int dx = i - LUT_R, dy = j - LUT_R, d2 = dx * dx + dy * dy;
                int x = CX + dx, y = CY + dy;
                if (d2 <= R * R) Fb::blendPixel(x, y, DISC, 235);
                float d = sqrtf((float)d2);
                float e = fabsf(d - R);   // glow a ambos lados del borde
                if (e < GLOW) Fb::blendPixel(x, y, GREEN, (uint8_t)(e < 1 ? 230 : 120 / (e * e)));
            }
        uint16_t ring = Fb::blend(DISC, GREEN, 70), cross = Fb::blend(DISC, GREEN, 40);
        g.drawCircle(CX, CY, R / 3, ring);
        g.drawCircle(CX, CY, 2 * R / 3, ring);
        g.drawFastHLine(CX - R + 2, CY, 2 * R - 3, cross);
        g.drawFastVLine(CX, CY - R + 2, 2 * R - 3, cross);
        TextAA::drawCentered(Nunito14, CX, CY - R + 16, "N", Fb::blend(DISC, GREEN, 140));

        // Distancia de los anillos
        char b[12];
        int km = Settings::get().radarKm;
        for (int k = 1; k <= 2; k++) {
            float v = km * k / 3.0f;
            if (v < 10) snprintf(b, sizeof(b), "%.1f", v);
            else snprintf(b, sizeof(b), "%d", (int)lroundf(v));
            TextAA::draw(Nunito14, CX + 4, CY - R * k / 3 + 14, b, Fb::blend(DISC, GREEN, 110));
        }

        // Centro = nosotros
        g.fillRect(CX - 1, CY - 1, 3, 3, GREEN);

        // Pie: radio a la izquierda (se toca para cambiarlo) y leyenda de tipos a la derecha
        char foot[24];
        snprintf(foot, sizeof(foot), "RADIO %d KM", km);
        Panel::spaced(Nunito14, CX - R, SCREEN_H - 3, foot, FOOT, 1);
        int lx = CX + R + 2;
        for (int k = 2; k >= 0; k--) {
            lx -= Panel::spacedWidth(Nunito14, KIND_SHORT[k], 1);
            Panel::spaced(Nunito14, lx, SCREEN_H - 3, KIND_SHORT[k], FOOT, 1);
            lx -= 10;
            g.fillRect(lx, SCREEN_H - 11, 6, 6, KIND_COL[k]);
            lx -= 10;
        }

        memcpy(_base, Fb::ptr(), Background::BYTES);
    }

    // Capa estática: base + barra + estado + panel. Se rehace con datos nuevos (~20 ms).
    void _buildLayer(Arduino_GFX &g) {
        memcpy(Fb::ptr(), _base, Background::BYTES);
        StatusBar::draw(g, false);
        _overlayState();
        _drawPanel(g);
        memcpy(_layer, Fb::ptr(), Background::BYTES);
    }

    // Mensaje en el centro del disco si no hay datos que enseñar
    void _overlayState() {
        const char *msg = nullptr;
        char buf[32];
        switch (_snap.state) {
            case FlightService::State::NoNet:   msg = "SIN RED"; break;
            case FlightService::State::Idle:
            case FlightService::State::Loading: if (!_snap.at) msg = "CARGANDO..."; break;
            case FlightService::State::Error:
                if (!_snap.at) { snprintf(buf, sizeof(buf), "ERROR API %d", _snap.httpCode); msg = buf; }
                break;
            case FlightService::State::Ok:
                if (!_snap.count) msg = (const char *[]){"SIN VUELOS", "NADIE EN TIERRA", "SIN TRÁFICO"}[Settings::get().radarGnd % 3];
                break;
        }
        if (msg) Panel::spacedCentered(Nunito14, CX, CY + 30, msg, LABEL, 2);
    }

    // ------------------------------------------------------------ panel lateral
    static void _fmtAlt(int32_t ft, char *out, size_t n) {
        if (ft == -2) snprintf(out, n, "--");
        else if (ft < 0) snprintf(out, n, "TIERRA");
        else if (ft >= 10000) snprintf(out, n, "FL%03ld", (long)(ft / 100));
        else snprintf(out, n, "%ld ft", (long)ft);
    }

    void _drawPanel(Arduino_GFX &g) {
        uint16_t acc = Theme::accent();
        // (el fondo del panel está en la capa fija: _panelBackground)


        // Volver
        for (int t = 0; t < 2; t++) {
            g.drawLine(PX + 8 + t, BACK_Y + 17, PX + 15 + t, BACK_Y + 10, Theme::MUTED);
            g.drawLine(PX + 8 + t, BACK_Y + 17, PX + 15 + t, BACK_Y + 24, Theme::MUTED);
        }
        Panel::spaced(Nunito14, PX + 24, BACK_Y + 23, "INICIO", Theme::MUTED, 2);
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", _snap.count);   // solo los filtrados
        // Chip del filtro: tecla de panel con el estado en el acento
        static const char *GND[3] = {"VUELO", "TIERRA", "TODOS"};
        Panel::fill(CHIP_X, BACK_Y, PX1 - CHIP_X, BACK_H, 5, acc, 255);
        Panel::spacedCentered(Nunito14, (CHIP_X + PX1) / 2, BACK_Y + 22, GND[Settings::get().radarGnd % 3], 0x0000, 1);
        g.drawFastHLine(PX, BACK_Y + BACK_H + 4, PX1 - PX, Theme::FRAME);
        g.drawFastHLine(PX, BACK_Y + BACK_H + 4, 24, acc);

        Panel::spaced(Nunito14, PX + 2, LIST_Y - 8, "CERCANOS", Theme::MUTED, 2);
        TextAA::drawRight(Nunito14, PX1 - 2, LIST_Y - 8, cnt, acc);
        for (int i = 0; i < ROWS && i < _snap.count; i++) {
            const auto &p = _snap.planes[i];
            int y = LIST_Y + i * ROW_H;
            bool sel = !strcmp(p.call, _sel);
            if (sel) Panel::fill(PX - 2, y + 1, PX1 - PX + 4, ROW_H - 2, 3, acc, 255);
            uint16_t c = sel ? 0x0000 : Theme::TEXT, m = sel ? 0x0000 : Theme::MUTED;
            char alt[12], d[12];
            _fmtAlt(p.altFt, alt, sizeof(alt));
            snprintf(d, sizeof(d), "%.1f km", p.distKm);
            g.fillRect(PX - 2, y + 5, 3, ROW_H - 10, KIND_COL[p.kind % 3]);   // tipo
            TextAA::draw(Nunito14, PX + 6, y + 17, p.call, c);
            TextAA::draw(Nunito14, PX + 82, y + 17, alt, m);
            TextAA::drawRight(Nunito14, PX1 - 2, y + 17, d, c);
        }

        // Detalle del seleccionado
        g.drawFastHLine(PX, DET_Y - 4, PX1 - PX, Theme::FRAME);
        const FlightService::Plane *s = nullptr;
        for (int i = 0; i < _snap.count; i++)
            if (_sel[0] && !strcmp(_snap.planes[i].call, _sel)) s = &_snap.planes[i];
        if (!s) {
            Panel::spaced(Nunito14, PX + 2, DET_Y + 20, "TOCA UN AVIÓN", Theme::DIM, 2);
            return;
        }
        char l1[40], l2[24], l3[16], alt[12];
        _fmtAlt(s->altFt, alt, sizeof(alt));
        TextAA::draw(Nunito18, PX + 2, DET_Y + 18, s->call, acc);
        const char *kn = KIND_NAME[s->kind % 3];
        Panel::spaced(Nunito14, PX1 - 2 - Panel::spacedWidth(Nunito14, kn, 1), DET_Y + 17, kn, KIND_COL[s->kind % 3], 1);
        if (s->type[0]) snprintf(l1, sizeof(l1), "%s · %s · %d kt", s->type, alt, s->gsKt);
        else snprintf(l1, sizeof(l1), "%s · %d kt", alt, s->gsKt);
        snprintf(l2, sizeof(l2), "%.1f km · %03d°", s->distKm, (int)s->bearing);
        TextAA::draw(Nunito14, PX + 2, DET_Y + 38, l1, Theme::TEXT);
        TextAA::draw(Nunito14, PX + 2, DET_Y + 56, l2, Theme::MUTED);
        if (s->track >= 0) {
            snprintf(l3, sizeof(l3), "rumbo %03d°", s->track);
            TextAA::drawRight(Nunito14, PX1 - 2, DET_Y + 56, l3, Theme::MUTED);
        }
    }
    // ------------------------------------------------------------ animación
    int _sweepAngle(uint32_t now) const { return (int)((now % SWEEP_MS) * ANG / SWEEP_MS); }

    // Caja que contiene la cuña del barrido (de sw-TRAIL a sw): centro, los dos
    // extremos del arco y los puntos cardinales que caigan dentro.
    void _wedgeBox(int sw, int &i0, int &j0, int &i1, int &j1) const {
        i0 = i1 = j0 = j1 = LUT_R;
        auto add = [&](int a) {
            float r = a * 2 * PI / ANG;
            int i = LUT_R + (int)lroundf(R * sinf(r)), j = LUT_R - (int)lroundf(R * cosf(r));
            i0 = min(i0, i); i1 = max(i1, i); j0 = min(j0, j); j1 = max(j1, j);
        };
        add(sw);
        add((sw - TRAIL) & (ANG - 1));
        for (int c = 0; c < ANG; c += ANG / 4)
            if (((sw - c) & (ANG - 1)) < TRAIL) add(c);
        i0 = max(0, i0 - 1); j0 = max(0, j0 - 1);
        i1 = min(LUT_S - 1, i1 + 1); j1 = min(LUT_S - 1, j1 + 1);
    }

    void _drawSweep(uint32_t now) {
        int sw = _sweepAngle(now);
        int i0, j0, i1, j1;
        _wedgeBox(sw, i0, j0, i1, j1);   // solo la zona de la estela, no todo el disco
        uint16_t *fb = Fb::ptr();
        for (int j = j0; j <= j1; j++) {
            const uint16_t *row = _lut + j * LUT_S;
            int y = CY - LUT_R + j;
            for (int i = i0; i <= i1; i++) {
                uint16_t a = row[i];
                if (a == OUT) continue;
                int diff = (sw - a) & (ANG - 1);   // cuánto hace que pasó el barrido
                if (diff >= TRAIL) continue;
                int alpha = diff < 3 ? 210 : 120 * (TRAIL - diff) / TRAIL;
                uint16_t &p = fb[Fb::idx(CX - LUT_R + i, y)];
                p = Fb::blend(p, GREEN, alpha);
            }
        }
    }

    // Línea con alfa (para el trazo de rumbo)
    static void _lineA(int x0, int y0, int x1, int y1, uint16_t c, uint8_t a) {
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (;;) {
            Fb::blendPixel(x0, y0, c, a);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void _drawPlanes(Arduino_GFX &g, uint32_t now) {
        int sw = _sweepAngle(now);
        int span = (sw - _prevSweep) & (ANG - 1);   // lo que ha barrido desde el fotograma anterior
        for (int i = 0; i < _snap.count; i++) {
            const auto &p = _snap.planes[i];
            int x, y;
            _planeXY(p, now, x, y);
            int dx = x - CX, dy = y - CY;
            if (dx * dx + dy * dy > (R - 2) * (R - 2)) continue;   // se ha salido del radio

            // Fósforo: se enciende al pasar el barrido y se apaga en una vuelta
            int a = _angleOf(x, y);
            if (((a - _prevSweep) & (ANG - 1)) <= span) _litAt[i] = now;
            bool sel = !strcmp(p.call, _sel);
            float b = _litAt[i] ? 1.0f - (now - _litAt[i]) / (float)(SWEEP_MS - 300) : 0.0f;
            if (b < 0.18f) b = 0.18f;
            if (sel) b = max(b, 0.75f);
            uint8_t al = (uint8_t)(255 * b);

            uint16_t col = KIND_COL[p.kind % 3];
            if (p.ground) {   // en tierra: cuadrado pequeño, sin rumbo
                Fb::fillRoundRectAlpha(x - 4, y - 4, 9, 9, 0, col, (uint8_t)(55 * b));
                Fb::fillRoundRectAlpha(x - 2, y - 2, 5, 5, 0, col, al);
            } else {
                Fb::fillRoundRectAlpha(x - 5, y - 5, 11, 11, 5, col, (uint8_t)(70 * b));   // halo
                Fb::fillRoundRectAlpha(x - 2, y - 2, 5, 5, 2, col, al);
                if (p.track >= 0) {
                    float t = p.track * DEG_TO_RAD;
                    _lineA(x, y, x + (int)lroundf(10 * sinf(t)), y - (int)lroundf(10 * cosf(t)), col, al);
                }
            }
            if (sel) {   // marca de selección: esquinas
                uint16_t c = Theme::accent();
                for (int k = 0; k < 4; k++) {
                    int cx = x + ((k & 1) ? 8 : -8), cy = y + ((k & 2) ? 8 : -8);
                    g.drawFastHLine((k & 1) ? cx - 3 : cx, cy, 4, c);
                    g.drawFastVLine(cx, (k & 2) ? cy - 3 : cy, 4, c);
                }
            }
            // Solo el seleccionado lleva nombre en el disco. No puede pasar a la zona del
            // panel (allí no se restaura cada fotograma)
            if (!sel) continue;
            int lw = TextAA::width(Nunito14, p.call), lx = x + 8;
            if (lx + lw > PART - 2) lx = x - 8 - lw;
            TextAA::draw(Nunito14, lx, y - 4, p.call, Theme::accent());
        }
        _prevSweep = sw;
    }
};
