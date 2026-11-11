#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "../config.h"
#include "Settings.h"
#include "WifiPortalHtml.h"

#ifndef MDNS_NAME
#define MDNS_NAME "companion"     // http://companion.local
#endif

// Wi-Fi en modo AP+STA: el AP de configuración (por defecto "Companion-Setup",
// http://192.168.4.1) está siempre encendido, y en paralelo se conecta a la red
// guardada. El mismo portal se sirve en la LAN por http://companion.local.
//
// Ojo: el ESP32 tiene una sola radio, así que el AP usa el canal de la red
// a la que se conecta. Al conectar a otra red el móvil puede caerse del AP un
// momento; vuelve solo.
class NetManager {
public:
    enum class Event { None, Connected, Disconnected, Failed };

    static void begin() {
        Serial.println("[Net] Iniciando Wi-Fi (AP+STA)...");
        _loadCreds();

        WiFi.persistent(false);       // las credenciales las guardamos nosotros en NVS
        WiFi.setAutoReconnect(false); // reintentos controlados en loop()
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
            uint8_t r = info.wifi_sta_disconnected.reason;
            if (r != WIFI_REASON_ASSOC_LEAVE) lastReason() = r;   // ASSOC_LEAVE = lo hemos pedido nosotros
        }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { lastReason() = 0; }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

        WiFi.mode(WIFI_AP_STA);
        WiFi.setHostname(MDNS_NAME);
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        _startAP();

        dns().setErrorReplyCode(DNSReplyCode::NoError);
        dns().start(53, "*", WiFi.softAPIP());
        _setupServer();
        server().begin();

        if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

        // Primer intento bloqueante (hasta 8 s) para que el reloj arranque con hora
        if (!ssid().isEmpty()) {
            _connect(false);
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) delay(100);
        }
        if (!isConnected()) Serial.println("[Net] Sin conexión a la red; el AP sigue disponible");
    }

    static void loop() {
        dns().processNextRequest();
        server().handleClient();

        uint32_t now = millis();

        if (pendingApAt() && now >= pendingApAt()) {
            pendingApAt() = 0;
            _startAP();
        }
        if (pendingConnectAt() && now >= pendingConnectAt()) {
            pendingConnectAt() = 0;
            _connect(true);
            return;
        }

        bool conn = isConnected();
        if (conn != wasConnected()) {
            wasConnected() = conn;
            if (conn) {
                userAttempt() = false;
                Serial.printf("[Net] Conectado a '%s', IP %s\n", ssid().c_str(), staIp().c_str());
                _push(Event::Connected);
            } else {
                Serial.printf("[Net] Desconectado (%s)\n", reasonText());
                // Si lo hemos tirado nosotros para cambiar de red no avisamos: ya vendrá Connected o Failed
                if (!userAttempt() && !ssid().isEmpty()) _push(Event::Disconnected);
            }
        }

        // Intento pedido desde el portal que no ha llegado a conectar
        if (userAttempt() && !conn && now - lastAttemptMs() > 15000) {
            userAttempt() = false;
            Serial.printf("[Net] No se pudo conectar a '%s': %s\n", ssid().c_str(), reasonText());
            _push(Event::Failed);
        }

        if (conn || ssid().isEmpty()) return;

        // Reintento cada 30 s. Si hay alguien conectado al AP (configurando) no
        // se reintenta: cada intento barre canales y le cortaría el portal.
        if (now - lastAttemptMs() > 30000 && WiFi.softAPgetStationNum() == 0) _connect(false);
    }

    // Último cambio de estado (para avisar en pantalla). Lo consume main.cpp.
    static Event takeEvent() {
        Event e = event();
        event() = Event::None;
        return e;
    }

    // Para que otros módulos registren sus rutas (p.ej. SdManager)
    static WebServer &web() { return server(); }

    static bool isConnected() { return WiFi.status() == WL_CONNECTED; }

    // true cuando no hay red: la UI debe mostrar cómo entrar al AP.
    static bool isApMode() { return !isConnected(); }

    static const char *apSsid() { return Settings::get().apSsid.c_str(); }
    static String apIp() { return WiFi.softAPIP().toString(); }
    static String staIp() { return WiFi.localIP().toString(); }
    static const String &savedSsid() { return ssid(); }
    static const char *mdnsHost() { return MDNS_NAME ".local"; }

    // Motivo del último fallo, en cristiano
    static const char *reasonText() {
        switch (lastReason()) {
            case 0: return "";
            case WIFI_REASON_NO_AP_FOUND: return "red no encontrada";
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_MIC_FAILURE: return "contraseña incorrecta";
            case WIFI_REASON_BEACON_TIMEOUT: return "señal perdida";
            case WIFI_REASON_ASSOC_FAIL:
            case WIFI_REASON_CONNECTION_FAIL: return "el router no responde";
            default:
                // 210-212: no hay AP compatible / con señal suficiente
                return (lastReason() >= 210 && lastReason() <= 212) ? "red no encontrada" : "error de conexión";
        }
    }

private:
    static String& ssid() { static String v; return v; }
    static String& pass() { static String v; return v; }
    static uint32_t& lastAttemptMs() { static uint32_t v = 0; return v; }
    static uint32_t& pendingConnectAt() { static uint32_t v = 0; return v; }
    static uint32_t& pendingApAt() { static uint32_t v = 0; return v; }
    static bool& wasConnected() { static bool v = false; return v; }
    static bool& userAttempt() { static bool v = false; return v; }
    static volatile uint8_t& lastReason() { static volatile uint8_t v = 0; return v; }
    static Event& event() { static Event v = Event::None; return v; }
    static WebServer& server() { static WebServer s(80); return s; }
    static DNSServer& dns() { static DNSServer d; return d; }

    static void _push(Event e) { event() = e; }

    static void _startAP() {
        const auto &c = Settings::get();
        bool secure = c.apPass.length() >= 8;
        WiFi.softAP(c.apSsid.c_str(), secure ? c.apPass.c_str() : nullptr, 1, 0, 4);
        Serial.printf("[Net] AP '%s' (%s) en http://%s\n", c.apSsid.c_str(), secure ? "con clave" : "abierto",
                      apIp().c_str());
    }

    static void _loadCreds() {
        Preferences p;
        p.begin("wifi-config", false);   // rw: en el primer arranque crea el namespace
        ssid() = p.getString("ssid", "");
        pass() = p.getString("pass", "");
        p.end();
        if (ssid().isEmpty() && String(WIFI_SSID) != "your-wifi-name") {
            ssid() = WIFI_SSID;
            pass() = WIFI_PASSWORD;
            Serial.println("[Net] Usando la red de config.h");
        }
    }

    static void _saveCreds(const String &s, const String &pw) {
        Preferences p;
        p.begin("wifi-config", false);
        p.putString("ssid", s);
        p.putString("pass", pw);
        p.end();
        ssid() = s;
        pass() = pw;
    }

    static void _connect(bool fromUser) {
        lastAttemptMs() = millis();
        if (ssid().isEmpty()) return;
        userAttempt() = fromUser;
        lastReason() = 0;
        Serial.printf("[Net] Conectando a '%s'...\n", ssid().c_str());
        WiFi.disconnect(false, false);
        WiFi.begin(ssid().c_str(), pass().c_str());
    }

    static void _sendJson(int code, JsonDocument &doc) {
        String out;
        serializeJson(doc, out);
        server().send(code, "application/json", out);
    }

    static void _sendOk() {
        JsonDocument d;
        d["ok"] = true;
        _sendJson(200, d);
    }

    static void _sendError(const char *msg) {
        JsonDocument d;
        d["ok"] = false;
        d["error"] = msg;
        _sendJson(400, d);
    }

    // Peticiones a dominios ajenos (detección de portal cautivo de Android/iOS/Windows)
    // se redirigen al portal para que el móvil lo abra solo al unirse al AP.
    static bool _captiveRedirect() {
        String host = server().hostHeader();
        if (host.isEmpty() || host == apIp() || host == staIp() || host.equalsIgnoreCase(mdnsHost())) return false;
        server().sendHeader("Location", "http://" + apIp() + "/", true);
        server().send(302, "text/plain", "");
        return true;
    }

    static bool _validCoord(const String &s, float lim) {
        if (s.isEmpty() || s.length() > 12) return false;
        char *end;
        float v = strtof(s.c_str(), &end);
        return *end == '\0' && v >= -lim && v <= lim;
    }

    static void _setupServer() {
        server().on("/", HTTP_GET, [] {
            if (_captiveRedirect()) return;
            server().send_P(200, "text/html", PORTAL_HTML);
        });

        server().on("/api/status", HTTP_GET, [] {
            JsonDocument d;
            bool conn = isConnected();
            d["connected"]  = conn;
            d["connecting"] = !conn && !ssid().isEmpty() && millis() - lastAttemptMs() < 15000;
            d["error"]      = conn ? "" : reasonText();
            d["saved_ssid"] = ssid();
            d["ip"]         = staIp();
            d["host"]       = mdnsHost();
            d["rssi"]       = WiFi.RSSI();
            d["ap_ssid"]    = apSsid();
            d["ap_ip"]      = apIp();
            d["ap_clients"] = WiFi.softAPgetStationNum();
            d["uptime"]     = millis() / 1000;
            d["heap"]       = ESP.getFreeHeap();
            _sendJson(200, d);
        });

        server().on("/api/scan", HTTP_GET, [] {
            int n = WiFi.scanNetworks();
            JsonDocument d;
            JsonArray arr = d.to<JsonArray>();
            for (int i = 0; i < n; ++i) {
                String s = WiFi.SSID(i);
                if (s.isEmpty()) continue;
                bool dup = false;   // misma red en varios APs: nos quedamos con la primera (más fuerte)
                for (JsonObject o : arr) if (o["ssid"] == s) { dup = true; break; }
                if (dup) continue;
                JsonObject o = arr.add<JsonObject>();
                o["ssid"]   = s;
                o["rssi"]   = WiFi.RSSI(i);
                o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
            }
            WiFi.scanDelete();
            _sendJson(200, d);
        });

        server().on("/api/wifi", HTTP_POST, [] {
            String s = server().arg("ssid");
            String pw = server().arg("pass");
            s.trim();
            if (s.isEmpty() || s.length() > 32 || pw.length() > 63 || (pw.length() > 0 && pw.length() < 8)) {
                _sendError("SSID o contraseña no válidos (la contraseña: 8-63 caracteres)");
                return;
            }
            _saveCreds(s, pw);
            Serial.printf("[Net] Nueva red guardada: '%s'\n", s.c_str());
            _sendOk();
            pendingConnectAt() = millis() + 500;   // deja salir la respuesta antes de tocar la radio
        });

        server().on("/api/forget", HTTP_POST, [] {
            _saveCreds("", "");
            userAttempt() = false;
            WiFi.disconnect(false, true);
            Serial.println("[Net] Red olvidada");
            _sendOk();
        });

        server().on("/api/config", HTTP_GET, [] {
            const auto &c = Settings::get();
            JsonDocument d;
            d["city"]       = c.city;
            d["lat"]        = c.lat;
            d["lon"]        = c.lon;
            d["tz"]         = c.tz;
            d["brightness"] = c.brightness;
            d["ap_ssid"]    = c.apSsid;
            d["ap_secure"]  = c.apPass.length() >= 8;
            d["bg_home"]    = c.bgHome;
            d["radar_km"]   = c.radarKm;
            d["radar_gnd"]  = c.radarGnd;
            _sendJson(200, d);
        });

        // Guarda solo los campos que vengan en la petición
        server().on("/api/config", HTTP_POST, [] {
            auto &c = Settings::get();
            uint8_t chg = 0;

            if (server().hasArg("lat") || server().hasArg("lon")) {
                String lat = server().arg("lat"), lon = server().arg("lon");
                lat.trim(); lon.trim();
                if (!_validCoord(lat, 90) || !_validCoord(lon, 180)) { _sendError("Latitud/longitud no válidas"); return; }
                c.lat = lat;
                c.lon = lon;
                c.city = server().arg("city").substring(0, 40);
                chg |= Settings::CHG_LOC;
            }
            if (server().hasArg("tz")) {
                String tz = server().arg("tz");
                tz.trim();
                if (tz.isEmpty() || tz.length() > 60) { _sendError("Zona horaria no válida"); return; }
                c.tz = tz;
                chg |= Settings::CHG_TZ;
            }
            if (server().hasArg("brightness")) {
                c.brightness = constrain(server().arg("brightness").toInt(), 5, 255);
                chg |= Settings::CHG_BRIGHT;
            }
            if (server().hasArg("radar_gnd")) {
                int v = server().arg("radar_gnd").toInt();
                if (v < 0 || v > 2) { _sendError("Filtro no válido (0 vuelo, 1 tierra, 2 todos)"); return; }
                c.radarGnd = v;
                chg |= Settings::CHG_RGND;
            }
            if (server().hasArg("radar_km")) {
                int km = server().arg("radar_km").toInt();
                if (km != 10 && km != 25 && km != 50 && km != 100) { _sendError("Radio no válido (10, 25, 50 o 100 km)"); return; }
                c.radarKm = km;
                chg |= Settings::CHG_RADAR;
            }
            if (server().hasArg("bg_home")) {
                String bg = server().arg("bg_home");
                bg.trim();
                if (!bg.startsWith("/") || !bg.endsWith(".565") || bg.indexOf("..") >= 0 || bg.length() > 64) {
                    _sendError("Fondo no válido"); return;
                }
                c.bgHome = bg;
                chg |= Settings::CHG_BG;
            }
            if (server().hasArg("ap_ssid")) {
                String s = server().arg("ap_ssid");
                s.trim();
                if (s.isEmpty() || s.length() > 32) { _sendError("Nombre del AP no válido"); return; }
                if (server().hasArg("ap_pass")) {   // sin el campo se mantiene la clave actual; "" = abierto
                    String pw = server().arg("ap_pass");
                    if (pw.length() > 0 && (pw.length() < 8 || pw.length() > 63)) { _sendError("La clave del AP debe tener 8-63 caracteres"); return; }
                    c.apPass = pw;
                }
                c.apSsid = s;
                pendingApAt() = millis() + 800;   // el AP se reinicia: los clientes se reconectan
            }

            Settings::save();
            Settings::markChanged(chg);
            _sendOk();
        });

        // Brillo en vivo mientras se arrastra el slider (no se guarda)
        server().on("/api/brightness", HTTP_POST, [] {
            Settings::get().brightness = constrain(server().arg("v").toInt(), 5, 255);
            Settings::markChanged(Settings::CHG_BRIGHT);
            _sendOk();
        });

        server().on("/api/reboot", HTTP_POST, [] {
            _sendOk();
            delay(300);
            ESP.restart();
        });

        server().onNotFound([] {
            if (_captiveRedirect()) return;
            server().sendHeader("Location", "/", true);
            server().send(302, "text/plain", "");
        });
    }
};
