#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include "Pins.h"
#include "TextAA.h"

// Fondos de pantalla 480x320 desde la microSD.
//
// Formato .565: RGB565 crudo, little-endian, 480x320 fila a fila (307200 bytes,
// sin cabecera). Se generan con tools/convert_bg.py a partir de PNG/JPG.
// Al cargarlos se rotan a la orientación nativa del panel y se cachean en
// PSRAM, así que pintarlos es un memcpy al framebuffer (~1 ms).
//
// Si no hay SD o falta el fichero, usa el fondo compilado en flash
// (src/assets/background_480x320.h) y, si tampoco está, un color liso.

#if __has_include("../assets/background_480x320.h")
#include "../assets/background_480x320.h"
#define BG_HAS_BUILTIN 1
#endif

class Background {
public:
    static constexpr uint32_t PIXELS = 480 * 320;
    static constexpr uint32_t BYTES = PIXELS * 2;
    static constexpr size_t MAX_CACHE = 8;   // 8 x 300 KB = 2.4 MB de PSRAM

    static bool beginSD() {
        // El display QSPI ocupa SPI2 (FSPI): la SD va por SPI3 (HSPI).
        spi().begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
        sdOk() = SD.begin(SD_CS, spi(), 20000000);
        if (sdOk()) {
            Serial.printf("[SD] OK, %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
        } else {
            Serial.println("[SD] No hay tarjeta o no se pudo montar (FAT32?)");
        }
        return sdOk();
    }

    static bool &sdOk() { static bool v = false; return v; }

    static uint64_t cardMB() { return sdOk() ? SD.cardSize() / (1024ULL * 1024ULL) : 0; }

    // Pinta el fondo en el framebuffer. path tipo "/bg/home.565".
    static void draw(const char *path, uint16_t fallback = 0x0000) {
        uint16_t *px = get(path);
        if (px && Fb::ptr()) {
            memcpy(Fb::ptr(), px, BYTES);
            return;
        }
        // color liso
        if (Fb::ptr()) for (uint32_t i = 0; i < PIXELS; i++) Fb::ptr()[i] = fallback;
    }

    // Oscurece todo el framebuffer (para que el texto se lea sobre fondos claros)
    static void dim(uint8_t alpha) {
        if (!Fb::ptr()) return;
        for (uint32_t i = 0; i < PIXELS; i++) Fb::ptr()[i] = Fb::blend(Fb::ptr()[i], 0x0000, alpha);
    }

    // Carga un .565 en un buffer propio (BYTES, orden nativo del panel) sin pasar
    // por la caché: para las fotos del salvapantallas, que no se repiten.
    static bool loadInto(const char *path, uint16_t *dst) {
        if (!sdOk() || !dst) return false;
        File f = SD.open(path, FILE_READ);
        if (!f) return false;
        if (f.size() != BYTES) { f.close(); return false; }
        static uint16_t row[480];
        for (int y = 0; y < 320; y++) {
            if (f.read((uint8_t *)row, sizeof(row)) != sizeof(row)) { f.close(); return false; }
            _putRow(dst, y, row);
        }
        f.close();
        return true;
    }

    // Píxeles ya cacheados (orden nativo, usar Fb::idx) o nullptr; no lee la SD
    static const uint16_t *cached(const char *path) {
        for (auto &e : cache()) if (e.path == path) return e.px;
        return nullptr;
    }

    static void forget(const char *path) {
        auto &c = cache();
        for (size_t i = 0; i < c.size(); i++)
            if (c[i].path == path) { if (c[i].px != _builtinPtr()) free(c[i].px); c.erase(c.begin() + i); return; }
    }

    // Vacía la caché entera (tras formatear o remontar la SD)
    static void forgetAll() {
        for (auto &e : cache()) if (e.px != _builtinPtr()) free(e.px);
        cache().clear();
    }

private:
    struct Entry { String path; uint16_t *px; };

    static SPIClass &spi() { static SPIClass s(HSPI); return s; }
    static std::vector<Entry> &cache() { static std::vector<Entry> c; return c; }

    static uint16_t *get(const char *path) {
        for (auto &e : cache()) if (e.path == path) return e.px;
        uint16_t *px = _loadSD(path);
        if (!px) px = _builtin();
        if (!px) return nullptr;
        if (cache().size() >= MAX_CACHE) {   // fuera el más antiguo
            if (cache().front().px != _builtinPtr()) free(cache().front().px);
            cache().erase(cache().begin());
        }
        cache().push_back({String(path), px});
        return px;
    }

    static uint16_t *_alloc() {
        uint16_t *p = (uint16_t *)ps_malloc(BYTES);
        if (!p) Serial.println("[BG] Sin PSRAM para el fondo");
        return p;
    }

    // Copia una fila lógica (480 px) a su sitio en el buffer nativo
    static void _putRow(uint16_t *dst, int y, const uint16_t *row) {
        for (int x = 0; x < 480; x++) dst[Fb::idx(x, y)] = row[x];
    }

    static uint16_t *_loadSD(const char *path) {
        if (!sdOk()) return nullptr;
        File f = SD.open(path, FILE_READ);
        if (!f) { Serial.printf("[BG] No existe %s\n", path); return nullptr; }
        if (f.size() != BYTES) {
            Serial.printf("[BG] %s mide %u bytes, esperaba %u\n", path, (unsigned)f.size(), (unsigned)BYTES);
            f.close();
            return nullptr;
        }
        uint16_t *dst = _alloc();
        if (!dst) { f.close(); return nullptr; }
        static uint16_t row[480];
        uint32_t t0 = millis();
        for (int y = 0; y < 320; y++) {
            if (f.read((uint8_t *)row, sizeof(row)) != sizeof(row)) {
                Serial.printf("[BG] Lectura corta en %s\n", path);
                free(dst); f.close(); return nullptr;
            }
            _putRow(dst, y, row);
        }
        f.close();
        Serial.printf("[BG] %s cargado en %lu ms\n", path, (unsigned long)(millis() - t0));
        return dst;
    }

    static uint16_t *&_builtinPtr() { static uint16_t *p = nullptr; return p; }

    static uint16_t *_builtin() {
#ifdef BG_HAS_BUILTIN
        if (_builtinPtr()) return _builtinPtr();
        uint16_t *dst = _alloc();
        if (!dst) return nullptr;
        for (int y = 0; y < 320; y++) _putRow(dst, y, &BACKGROUND_480X320[y * 480]);
        _builtinPtr() = dst;
        Serial.println("[BG] Usando fondo integrado");
        return dst;
#else
        return nullptr;
#endif
    }
};
