#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include "NetManager.h"
#include "Settings.h"
#include "Colors.h"
#include "../config.h"

// Una sola fuente de clima para todas las páginas (en la C3 lo pedían el
// reloj y la página del tiempo por separado). Open-Meteo, sin API key.
class WeatherService {
public:
    struct Data {
        bool ok = false;
        float temp = 0;
        int humidity = 0;
        int code = 0;
        bool isDay = true;
        String cond;
        String error;
    };

    static const Data &data() { return _d(); }

    // true si ha llegado dato nuevo (para repintar)
    static bool loop(bool force = false) {
        if (!NetManager::isConnected()) return false;
        uint32_t now = millis();
        uint32_t &last = _last();
        if (!force && last != 0 && now - last < POLL_MS) return false;
        last = now;
        return _fetch();
    }

    // Pide el dato de nuevo en la siguiente vuelta (p.ej. al cambiar la ubicación)
    static void refreshSoon() { _last() = 0; _d().ok = false; }

    static const char *condFromCode(int c) {
        if (c == 0) return "Despejado";
        if (c >= 1 && c <= 3) return "Nublado";
        if (c == 45 || c == 48) return "Niebla";
        if (c >= 51 && c <= 67) return "Lluvia";
        if (c >= 71 && c <= 77) return "Nieve";
        if (c >= 80 && c <= 82) return "Chubascos";
        if (c >= 95) return "Tormenta";
        return "Templado";
    }

    // Icono vectorial en una caja de s x s px (diseñado sobre rejilla 32x32,
    // el mismo de la WeatherPage de la C3, escalado).
    static void drawIcon(Arduino_GFX &g, int x, int y, int s, uint16_t bg = ST77XX_BLACK) {
        const Data &d = data();
        if (!d.ok) return;
        auto P = [&](float v) { return (int)(v * s / 32.0f); };
        int th = max(1, s / 32);  // grosor de líneas

        auto line = [&](float x1, float y1, float x2, float y2, uint16_t c) {
            for (int t = 0; t < th; t++)
                g.drawLine(x + P(x1) + t, y + P(y1), x + P(x2) + t, y + P(y2), c);
        };
        auto cloud = [&](float dy) {
            g.fillCircle(x + P(10), y + P(12 + dy), P(7), COLOR_CLOUD);
            g.fillCircle(x + P(20), y + P(10 + dy), P(9), COLOR_CLOUD);
            g.fillRect(x + P(5), y + P(14 + dy), P(20), P(8), COLOR_CLOUD);
        };

        if (d.code == 0) {
            if (d.isDay) {
                g.fillCircle(x + P(16), y + P(16), P(10), ST77XX_YELLOW);
                line(16, 1, 16, 4, ST77XX_YELLOW);   line(16, 28, 16, 31, ST77XX_YELLOW);
                line(1, 16, 4, 16, ST77XX_YELLOW);   line(28, 16, 31, 16, ST77XX_YELLOW);
                line(5, 5, 8, 8, ST77XX_YELLOW);     line(24, 24, 27, 27, ST77XX_YELLOW);
                line(27, 5, 24, 8, ST77XX_YELLOW);   line(5, 27, 8, 24, ST77XX_YELLOW);
            } else {
                g.fillCircle(x + P(16), y + P(16), P(12), COLOR_GOLD);
                g.fillCircle(x + P(21), y + P(12), P(11), bg);
                g.fillCircle(x + P(6), y + P(8), max(1, P(1)), ST77XX_WHITE);
                g.fillCircle(x + P(26), y + P(24), max(1, P(1)), ST77XX_WHITE);
            }
        } else if (d.code >= 51 && d.code <= 67) {
            cloud(0);
            line(9, 24, 6, 29, ST77XX_CYAN); line(16, 24, 13, 29, ST77XX_CYAN); line(23, 24, 20, 29, ST77XX_CYAN);
        } else if (d.code >= 95) {
            cloud(-2);
            line(18, 21, 12, 27, ST77XX_YELLOW); line(12, 27, 16, 30, ST77XX_YELLOW);
        } else {
            cloud(0);
        }
    }

private:
    static constexpr uint32_t POLL_MS = 600000;  // 10 min

    static Data &_d() { static Data d; return d; }
    static uint32_t &_last() { static uint32_t v = 0; return v; }

    static bool _fetch() {
        String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + Settings::get().lat +
                     "&longitude=" + Settings::get().lon +
                     "&current=temperature_2m,relative_humidity_2m,weather_code,is_day";
        Data &d = _d();
        for (int intento = 0; intento < 2; intento++) {
            HTTPClient http;
            http.useHTTP10(true);
            http.setTimeout(10000);
            http.begin(url);
            int code = http.GET();
            if (code == 200) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, http.getStream());
                http.end();
                if (err) { d.error = "Error JSON"; return true; }
                d.temp = doc["current"]["temperature_2m"].as<float>();
                d.humidity = doc["current"]["relative_humidity_2m"].as<int>();
                d.code = doc["current"]["weather_code"].as<int>();
                d.isDay = doc["current"]["is_day"].as<int>() == 1;
                d.cond = condFromCode(d.code);
                d.ok = true;
                d.error = "";
                Serial.printf("[Weather] %.1f C, %d%%, %s\n", d.temp, d.humidity, d.cond.c_str());
                return true;
            }
            http.end();
            d.error = "HTTP Err " + String(code);
            delay(300);
        }
        return true;
    }
};
