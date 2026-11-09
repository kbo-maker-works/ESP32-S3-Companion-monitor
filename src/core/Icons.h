#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>
#include "TextAA.h"
#include "Background.h"
#include "../assets/icons_builtin.h"

// Iconos monocromos (alpha de 4 bits) que se tiñen al pintarlos.
//
// id = nombre del archivo sin extensión (icon_env, sb_wifi_3...).
// Busca primero /icons/<id>.a4 en la microSD (los sube tools/make_icons.py
// o el portal) y si no, usa el compilado en src/assets/icons_builtin.h.
// Así se cambian los iconos sin reflashear.
class Icons {
public:
    struct Icon { uint8_t w = 0, h = 0; const uint8_t *data = nullptr; };

    // Pinta el icono con su esquina superior izquierda en (x, y)
    static bool draw(const char *id, int x, int y, uint16_t color) {
        Icon ic = get(id);
        if (!ic.data) return false;
        uint32_t n = 0;
        for (int j = 0; j < ic.h; j++)
            for (int i = 0; i < ic.w; i++, n++) {
                uint8_t b = ic.data[n >> 1];
                uint8_t a4 = (n & 1) ? (b & 0x0F) : (b >> 4);
                if (a4) Fb::blendPixel(x + i, y + j, color, a4 * 17);
            }
        return true;
    }

    // Pinta el icono a la mitad de tamaño (48 -> 24) promediando bloques de 2x2.
    // colorAt(sx, sy) da el color de cada píxel según su posición en el icono
    // original (sirve para p.ej. apagar los arcos exteriores del Wi-Fi).
    template <typename F>
    static bool drawHalf(const char *id, int x, int y, F colorAt) {
        Icon ic = get(id);
        if (!ic.data) return false;
        auto a4 = [&](int i, int j) {
            uint32_t n = (uint32_t)j * ic.w + i;
            uint8_t b = ic.data[n >> 1];
            return (n & 1) ? (b & 0x0F) : (b >> 4);
        };
        for (int j = 0; j + 1 < ic.h; j += 2)
            for (int i = 0; i + 1 < ic.w; i += 2) {
                int s = a4(i, j) + a4(i + 1, j) + a4(i, j + 1) + a4(i + 1, j + 1);   // 0..60
                // x6 en vez de x4.25: los trazos de 2 px quedan de 1 px y así no se ven lavados
                if (s) Fb::blendPixel(x + i / 2, y + j / 2, colorAt(i, j), (uint8_t)min(255, s * 6));
            }
        return true;
    }

    static bool drawHalf(const char *id, int x, int y, uint16_t color) {
        return drawHalf(id, x, y, [color](int, int) { return color; });
    }

    static Icon get(const char *id) {
        for (auto &e : cache()) if (e.id == id) return e.icon;
        Entry e;
        e.id = id;
        e.icon = _loadSD(id);
        e.fromSD = e.icon.data != nullptr;
        if (!e.fromSD) e.icon = _builtin(id);
        cache().push_back(e);
        return e.icon;
    }

    // Tras subir/borrar algo en /icons: se recargan al pintarse otra vez
    static void forgetAll() {
        for (auto &e : cache()) if (e.fromSD) free((void *)e.icon.data);
        cache().clear();
    }

private:
    struct Entry { String id; Icon icon; bool fromSD = false; };
    static std::vector<Entry> &cache() { static std::vector<Entry> c; return c; }

    static Icon _builtin(const char *id) {
        for (int i = 0; i < BUILTIN_ICON_COUNT; i++)
            if (!strcmp(BUILTIN_ICONS[i].id, id)) return {BUILTIN_ICONS[i].w, BUILTIN_ICONS[i].h, BUILTIN_ICONS[i].data};
        return {};
    }

    static Icon _loadSD(const char *id) {
        if (!Background::sdOk()) return {};
        String path = String("/icons/") + id + ".a4";
        if (!SD.exists(path)) return {};
        File f = SD.open(path, FILE_READ);
        if (!f) return {};
        uint8_t hdr[4];
        if (f.read(hdr, 4) != 4 || hdr[0] != 'A' || hdr[1] != '4' || !hdr[2] || !hdr[3] || hdr[2] > 128 || hdr[3] > 128) {
            Serial.printf("[Icon] %s no es un .a4 válido\n", path.c_str());
            f.close();
            return {};
        }
        size_t len = ((size_t)hdr[2] * hdr[3] + 1) / 2;
        uint8_t *buf = (uint8_t *)ps_malloc(len);
        if (!buf || f.read(buf, len) != len) {
            free(buf);
            f.close();
            return {};
        }
        f.close();
        Serial.printf("[Icon] %s desde la SD (%ux%u)\n", id, hdr[2], hdr[3]);
        return {hdr[2], hdr[3], buf};
    }
};
