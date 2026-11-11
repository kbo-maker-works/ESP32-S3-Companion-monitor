#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "Settings.h"
#include "Prof.h"

// Tráfico aéreo alrededor de la ubicación de Ajustes (lat/lon), vía la API
// ADS-B gratuita de adsb.lol (sin key):
//   GET https://api.adsb.lol/v2/point/{lat}/{lon}/{radio_nm}
// (airplanes.live tiene el mismo formato pero exige pedir acceso: 403.)
// Va por HTTP/1.1 (con 1.0 responde 426); admite respuestas chunked.
//
// La petición HTTPS va en una tarea FreeRTOS propia (núcleo 0), así que el loop
// y la animación del radar no se bloquean. Solo pide datos mientras la pantalla
// RADAR está abierta (setActive) y hay Wi-Fi, cada REFRESH_MS.
class FlightService {
public:
    static constexpr int MAX = 30;    // los que ve la pantalla (ya filtrados)
    static constexpr int ALL = 80;    // los que se guardan de cada respuesta, antes de filtrar
    static constexpr uint32_t REFRESH_MS = 12000;
    static constexpr size_t BODY_MAX = 512 * 1024;   // tope del cuerpo si llega sin Content-Length

    struct Plane {
        char call[10];       // callsign (o matrícula / hex si no emite)
        char type[6];        // modelo ICAO (B738, A320...)
        float distKm;        // distancia al centro
        float bearing;       // rumbo desde el centro, grados (0 = norte)
        int32_t altFt;       // altitud barométrica en pies (-1 = "ground", -2 = desconocida)
        int16_t gsKt;        // velocidad sobre el suelo, nudos
        int16_t track;       // hacia dónde va, grados (-1 = desconocido)
        uint8_t kind;        // 0 comercial, 1 militar, 2 carga (Kind)
        bool ground;         // en tierra: alt_baro "ground", o < 40 kt y < 500 ft
    };

    enum class State : uint8_t { Idle, Loading, Ok, NoNet, Error };

    struct Snapshot {
        Plane planes[MAX];
        int count = 0;
        State state = State::Idle;
        uint32_t seq = 0;      // cambia con cada respuesta nueva o cambio de filtro
        int total = 0;         // aviones en el radio, sin filtrar
        uint32_t at = 0;       // millis() de la última respuesta buena
        int httpCode = 0;
    };

    // ------------------------------------------------------------ clasificación
    enum Kind : uint8_t { COMMERCIAL = 0, MILITARY = 1, CARGO = 2 };

    // Prefijos de callsign militares (longitud variable). Añadir aquí.
    static constexpr const char *MIL_PREFIX[] = {
        "AME", "ASCOT", "BAF", "CNV", "CTM", "DUKE", "FAF", "GAF", "HKY", "IAM",
        "KING", "MMF", "NATO", "PAT", "RCH", "REACH", "RRR", "SPAR",
    };

    // Operadores de carga: prefijo ICAO de 3 letras. ORDENADA (búsqueda binaria).
    static constexpr const char *CARGO_ICAO[] = {
        "ABR", "ABW", "AHK", "AJT", "ATN", "BCS", "BOX", "CAO", "CKS", "CLX", "DHK", "DHL",
        "FDX", "GEC", "GTI", "ICV", "LCO", "MPH", "MSX", "NCA", "SWN", "TAY", "TPA", "UPS",
    };

    static uint8_t classify(const char *call, uint32_t dbFlags) {
        if (dbFlags & 1) return MILITARY;   // bit 0 de dbFlags = militar (solo viene si vale algo)
        for (const char *p : MIL_PREFIX)
            if (!strncmp(call, p, strlen(p))) return MILITARY;
        if (strlen(call) >= 3) {
            int lo = 0, hi = sizeof(CARGO_ICAO) / sizeof(CARGO_ICAO[0]) - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2, c = strncmp(call, CARGO_ICAO[mid], 3);
                if (!c) return CARGO;
                if (c < 0) hi = mid - 1; else lo = mid + 1;
            }
        }
        return COMMERCIAL;
    }

    static void begin() {
        if (_task()) return;
        _mux() = xSemaphoreCreateMutex();
        xTaskCreatePinnedToCore(_run, "flights", 12288, nullptr, 1, &_task(), 0);
    }

    // La pantalla RADAR lo activa al entrar y lo desactiva al salir
    static void setActive(bool on) {
        _active() = on;
        if (on) _due() = 0;   // pide ya
    }

    // Pide de nuevo en cuanto pueda (p.ej. al cambiar el radio)
    static void refreshSoon() { _due() = 0; }

    // Filtro por estado: lo que quiere ver la pantalla
    enum Ground : uint8_t { AIRBORNE = 0, ON_GROUND = 1, BOTH = 2 };

    // Copia los datos más recientes ya filtrados (vuelo / tierra / todos) y se
    // queda con los MAX más cercanos que cumplan. El filtro se aplica aquí, no al
    // pedir: cambiarlo se ve al momento y los de tierra no quitan sitio a los que vuelan.
    static void snapshot(Snapshot &out, uint8_t filter) {
        if (!_mux()) return;
        xSemaphoreTake(_mux(), portMAX_DELAY);
        const Store &st = _shared();
        out.state = st.state;
        out.at = st.at;
        out.httpCode = st.httpCode;
        out.seq = st.seq * 4 + filter;   // cambia también al cambiar el filtro
        out.total = st.count;
        out.count = 0;
        for (int i = 0; i < st.count && out.count < MAX; i++) {
            const Plane &p = st.planes[i];
            if (filter == BOTH || (filter == ON_GROUND) == p.ground) out.planes[out.count++] = p;
        }
        xSemaphoreGive(_mux());
    }

private:
    // Destino de writeToStream: acumula el cuerpo en PSRAM hasta un tope
    struct PsramSink : public Stream {   // solo se escribe; la parte de lectura no se usa
        uint8_t *buf = nullptr;
        size_t len = 0, cap = 0, limit;
        bool overflow = false;
        explicit PsramSink(size_t m) : limit(m) {}
        ~PsramSink() { free(buf); }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        size_t write(uint8_t c) override { return write(&c, 1); }
        size_t write(const uint8_t *d, size_t n) override {
            if (len + n > limit) { overflow = true; return 0; }
            if (len + n > cap) {
                size_t nc = max(cap ? cap * 2 : (size_t)16384, len + n);
                if (nc > limit) nc = limit;
                uint8_t *nb = (uint8_t *)ps_realloc(buf, nc);
                if (!nb) { overflow = true; return 0; }
                buf = nb;
                cap = nc;
            }
            memcpy(buf + len, d, n);
            len += n;
            return n;
        }
    };

    static TaskHandle_t &_task() { static TaskHandle_t t = nullptr; return t; }
    static SemaphoreHandle_t &_mux() { static SemaphoreHandle_t m = nullptr; return m; }
    // Lo que guarda la tarea: todos los aviones del radio (hasta ALL), ya clasificados
    struct Store {
        Plane planes[ALL];
        int count = 0;
        State state = State::Idle;
        uint32_t seq = 0, at = 0;
        int httpCode = 0;
    };
    static Store &_shared() { static Store s; return s; }
    static volatile bool &_active() { static volatile bool v = false; return v; }
    static volatile uint32_t &_due() { static volatile uint32_t v = 0; return v; }

    static void _setState(State st, int code = 0) {
        xSemaphoreTake(_mux(), portMAX_DELAY);
        _shared().state = st;
        _shared().httpCode = code;
        if (st != State::Ok) _shared().seq++;
        xSemaphoreGive(_mux());
    }

    static void _run(void *) {
        for (;;) {
            if (!_active()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            if (WiFi.status() != WL_CONNECTED) {
                if (_shared().state != State::NoNet) _setState(State::NoNet);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            uint32_t now = millis();
            if (_due() != 0 && (int32_t)(now - _due()) < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            _due() = now + REFRESH_MS;
            if (_shared().state != State::Ok) _setState(State::Loading);
#if RADAR_PROF
            uint32_t f0 = millis();
            Serial.printf("[Prof] fetch inicio t=%lu\n", (unsigned long)f0);
#endif
            _fetch();
#if RADAR_PROF
            Serial.printf("[Prof] fetch fin t=%lu (%lu ms)\n", (unsigned long)millis(), (unsigned long)(millis() - f0));
#endif
        }
    }

    static void _fetch() {
        const auto &cfg = Settings::get();
        float lat0 = cfg.lat.toFloat(), lon0 = cfg.lon.toFloat();
        int nm = (int)ceilf(cfg.radarKm / 1.852f);
        char url[112];
        snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat0, lon0, nm);

        WiFiClientSecure tls;
        tls.setInsecure();   // sin validar el certificado: solo leemos datos públicos
        HTTPClient http;
        // HTTP/1.1: adsb.lol rechaza HTTP/1.0 con un 426 Upgrade Required
        static const char *HDRS[] = {"Transfer-Encoding"};
        http.collectHeaders(HDRS, 1);
        http.setTimeout(8000);
        http.setUserAgent("desktop-companion/1.0");
        if (!http.begin(tls, url)) { _setState(State::Error, -1); return; }
        int code = http.GET();
        if (code != 200) {
            Serial.printf("[Radar] HTTP %d\n", code);
            http.end();
            _setState(State::Error, code);
            return;
        }

        // Solo los campos que usamos: el JSON completo trae ~40 por avión
        JsonDocument filter;
        JsonObject f = filter["ac"].add<JsonObject>();
        for (const char *k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs", "track", "true_heading",
                              "dbFlags", "category"})
            f[k] = true;
        JsonDocument doc;
        DeserializationError err;
        if (http.getSize() > 0 && !http.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
            // Con Content-Length: se parsea directamente del stream
            err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
        } else {
            // Sin longitud (chunked): cuerpo entero a PSRAM; writeToStream ya quita los trozos
            PsramSink sink(BODY_MAX);
            int n = http.writeToStream(&sink);
            if (n <= 0 || sink.overflow || !sink.buf) {
                Serial.printf("[Radar] cuerpo no leído (%d, %u B%s)\n", n, (unsigned)sink.len, sink.overflow ? ", demasiado grande" : "");
                http.end();
                _setState(State::Error, -3);
                return;
            }
            err = deserializeJson(doc, sink.buf, sink.len, DeserializationOption::Filter(filter));
        }
        http.end();
        if (err) {
            Serial.printf("[Radar] JSON: %s\n", err.c_str());
            _setState(State::Error, -2);
            return;
        }

        static Plane tmp[ALL];   // todos los del radio (hasta ALL); el filtro se aplica en snapshot()
        int n = 0;
        float kx = cosf(lat0 * DEG_TO_RAD);
        for (JsonObject a : doc["ac"].as<JsonArray>()) {
            if (n >= ALL) break;
            if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
            const char *cat = a["category"] | "";
            if (cat[0] == 'C') continue;   // C1-C3: vehículos de tierra del aeropuerto, no aviones
            Plane &p = tmp[n];
            // Distancia/rumbo con aproximación plana: sobra para radios de 100 km
            float dy = (a["lat"].as<float>() - lat0) * 111.32f;
            float dx = (a["lon"].as<float>() - lon0) * 111.32f * kx;
            p.distKm = sqrtf(dx * dx + dy * dy);
            if (p.distKm > cfg.radarKm) continue;
            p.bearing = fmodf(atan2f(dx, dy) * RAD_TO_DEG + 360.0f, 360.0f);
            const char *call = a["flight"] | "";
            if (!*call) call = a["r"] | "";
            if (!*call) call = a["hex"] | "?";
            strlcpy(p.call, call, sizeof(p.call));
            for (int i = strlen(p.call) - 1; i >= 0 && p.call[i] == ' '; i--) p.call[i] = 0;   // vienen con espacios
            strlcpy(p.type, a["t"] | "", sizeof(p.type));
            // alt_baro: pies, o "ground" (-1); si no viene, -2 = desconocida
            if (a["alt_baro"].is<int>()) p.altFt = a["alt_baro"].as<int>();
            else p.altFt = a["alt_baro"].is<const char *>() ? -1 : -2;
            p.gsKt = (int16_t)lroundf(a["gs"] | 0.0f);
            // Rumbo: track y, si falta, true_heading
            if (a["track"].is<float>()) p.track = (int16_t)lroundf(a["track"].as<float>());
            else if (a["true_heading"].is<float>()) p.track = (int16_t)lroundf(a["true_heading"].as<float>());
            else p.track = -1;
            p.kind = classify(p.call, a["dbFlags"] | 0u);
            p.ground = p.altFt == -1 || (p.altFt >= 0 && p.altFt < 500 && p.gsKt < 40);
            n++;
        }
        std::sort(tmp, tmp + n, [](const Plane &a, const Plane &b) { return a.distKm < b.distKm; });

        xSemaphoreTake(_mux(), portMAX_DELAY);
        Store &s = _shared();
        s.count = n;
        memcpy(s.planes, tmp, sizeof(Plane) * s.count);
        s.state = State::Ok;
        s.httpCode = 200;
        s.at = millis();
        s.seq++;
        xSemaphoreGive(_mux());
        Serial.printf("[Radar] %d aviones en %d km\n", s.count, cfg.radarKm);
    }
};
