#pragma once
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include "../core/Pins.h"
#include "../core/Panel.h"
#include "../core/Settings.h"
#include "../core/WeatherService.h"
#include "../core/Background.h"
#include "../fonts/Fonts.h"

// Salvapantallas ("refrescador"): sale tras Settings::ssMin minutos sin tocar.
// No toca el brillo (con el PC por USB nunca se atenúa: ver core/Power.h).
// Cualquier toque vuelve a la pantalla anterior.
//
// Modo FOTOS: muestra los .565 de /fotos en orden aleatorio (cada ssPhotoSec s)
// con la hora pequeña en una esquina; sin fotos cae al reloj.
// Modo RELOJ: reloj + tiempo, con varias "faces" (Settings::ssFace). Cada face
// es una función _faceN(); para añadir una, súmala a FACES y a _drawFace().
// El contenido cambia de sitio cada minuto para no marcar el panel.
class SaverScreen {
public:
    static constexpr int FACES = 1;
    static constexpr const char *PHOTO_DIR = "/fotos";

    void enter() {
        _min = -1;
        _photoAt = 0;
        _photoOk = false;
        if (Settings::get().ssMode == 1) _scanPhotos();
    }

    // Al salir: suelta el buffer de la foto (300 KB de PSRAM)
    void exit() {
        free(_photo);
        _photo = nullptr;
        _photos.clear();
    }

    // true si ha repintado: una vez por minuto, o al cambiar de foto
    bool update(Arduino_GFX &g, bool force) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        bool photos = Settings::get().ssMode == 1 && !_photos.empty();

        bool next = photos && (_photoAt == 0 || millis() - _photoAt >= Settings::get().ssPhotoSec * 1000UL);
        if (next) {
            _photoAt = millis();
            _photoOk = _nextPhoto();
        }
        if (!force && !next && t.tm_min == _min) return false;
        _min = t.tm_min;

        if (photos && _photoOk) {
            memcpy(Fb::ptr(), _photo, Background::BYTES);
            _clockCorner(g, t);
        } else {
            g.fillScreen(0x0000);
            _drawFace(g, t, Settings::get().ssFace);
            if (Settings::get().ssMode == 1)
                Panel::spacedCentered(Nunito14, SCREEN_W / 2, SCREEN_H - 10, "NO HAY FOTOS EN /FOTOS", Theme::DIM, 1);
        }
        return true;
    }

private:
    int _min = -1;
    uint16_t *_photo = nullptr;
    std::vector<String> _photos;
    size_t _photoIdx = 0;
    uint32_t _photoAt = 0;
    bool _photoOk = false;

    // ------------------------------------------------------------ fotos
    void _scanPhotos() {
        _photos.clear();
        if (!Background::sdOk()) return;
        File dir = SD.open(PHOTO_DIR);
        if (!dir || !dir.isDirectory()) return;
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            String n = f.name();
            if (!f.isDirectory() && n.endsWith(".565") && f.size() == Background::BYTES)
                _photos.push_back(String(PHOTO_DIR) + "/" + n);
            f.close();
        }
        dir.close();
        _shuffle();
        Serial.printf("[Saver] %u fotos en %s\n", (unsigned)_photos.size(), PHOTO_DIR);
    }

    // Orden aleatorio; al acabar la vuelta se baraja otra vez
    void _shuffle() {
        for (size_t i = _photos.size(); i > 1; i--) std::swap(_photos[i - 1], _photos[esp_random() % i]);
        _photoIdx = 0;
    }

    bool _nextPhoto() {
        if (!_photo) _photo = (uint16_t *)ps_malloc(Background::BYTES);
        if (!_photo) return false;
        for (size_t tries = 0; tries < _photos.size(); tries++) {
            if (_photoIdx >= _photos.size()) _shuffle();
            if (Background::loadInto(_photos[_photoIdx++].c_str(), _photo)) return true;
        }
        return false;
    }

    // Hora y temperatura pequeñas en la esquina inferior derecha, sobre una franja oscura
    static void _clockCorner(Arduino_GFX &g, const struct tm &t) {
        char hm[8] = "--:--", tc[12] = "";
        if (t.tm_year > 120) snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
        const auto &w = WeatherService::data();
        if (w.ok) snprintf(tc, sizeof(tc), "%d°C", (int)lroundf(w.temp));
        int wHm = TextAA::width(Nunito24, hm), wTc = tc[0] ? TextAA::width(Nunito14, tc) + 12 : 0;
        int bw = wHm + wTc + 28, bh = 40, bx = SCREEN_W - bw - 12, by = SCREEN_H - bh - 12;
        Panel::fill(bx, by, bw, bh, 5, 0x0000, 150);
        int x = TextAA::draw(Nunito24, bx + 14, by + 29, hm, Theme::accent());
        if (tc[0]) TextAA::draw(Nunito14, x + 12, by + 27, tc, Theme::TEXT);
    }

    // ------------------------------------------------------------ reloj
    void _drawFace(Arduino_GFX &g, const struct tm &t, uint8_t face) {
        switch (face) {
            default: _face1(g, t); break;
        }
    }

    // Posición pseudoaleatoria de un bloque w x h que cambia cada minuto
    static void _drift(const struct tm &t, int w, int h, int &x, int &y) {
        uint32_t seed = (uint32_t)(t.tm_hour * 60 + t.tm_min) * 2654435761u;
        x = 10 + (seed >> 8) % (SCREEN_W - w - 20);
        y = 10 + (seed >> 20) % (SCREEN_H - h - 20);
    }

    // Marcas de esquina, como las de los iconos
    static void _corners(Arduino_GFX &g, int x0, int y0, int w, int h, uint16_t c) {
        for (int k = 0; k < 4; k++) {
            int x = (k & 1) ? x0 + w - 1 : x0, y = (k & 2) ? y0 + h - 1 : y0;
            g.drawFastHLine((k & 1) ? x - 10 : x, y, 11, c);
            g.drawFastVLine(x, (k & 2) ? y - 10 : y, 11, c);
        }
    }

    // Face 1: hora grande, fecha y una línea con el tiempo
    void _face1(Arduino_GFX &g, const struct tm &t) {
        uint16_t acc = Theme::accent();
        char hm[8], date[28];
        static const char *DAYS[] = {"DOMINGO", "LUNES", "MARTES", "MIÉRCOLES", "JUEVES", "VIERNES", "SÁBADO"};
        static const char *MONTHS[] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};
        if (t.tm_year > 120) {
            snprintf(hm, sizeof(hm), "%02d:%02d", t.tm_hour, t.tm_min);
            snprintf(date, sizeof(date), "%s %02d %s", DAYS[t.tm_wday], t.tm_mday, MONTHS[t.tm_mon]);
        } else {
            strcpy(hm, "--:--");
            strcpy(date, "SIN HORA");
        }

        const int BW = 290, BH = 150;
        int bx, by;
        _drift(t, BW, BH, bx, by);
        int cx = bx + BW / 2;

        TextAA::drawCentered(Nunito72Num, cx, by + 76, hm, acc);
        Panel::spacedCentered(Nunito14, cx, by + 102, date, Theme::MUTED, 2);

        const auto &w = WeatherService::data();
        if (w.ok) {
            String cond = WeatherService::condFromCode(w.code);
            cond.toUpperCase();
            char line[64];
            snprintf(line, sizeof(line), "%d°C  ·  %s  ·  %d%%", (int)lroundf(w.temp), cond.c_str(), w.humidity);
            g.drawFastHLine(cx - 40, by + 116, 80, Theme::FRAME);
            TextAA::drawCentered(Nunito18, cx, by + 140, line, Theme::TEXT);
        }
        _corners(g, bx, by, BW, BH, Theme::FRAME);
    }
};
