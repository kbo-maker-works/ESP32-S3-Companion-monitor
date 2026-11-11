#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "../config.h"

#ifndef AP_SSID
#define AP_SSID "Companion-Setup"
#endif
#ifndef AP_PASSWORD
#define AP_PASSWORD ""
#endif
#ifndef BG_HOME
#define BG_HOME "/bg/home.565"
#endif
#ifndef CITY_NAME
#define CITY_NAME "Madrid"
#endif

// Ajustes editables desde el portal, guardados en NVS ("companion-cfg").
// Los valores de config.h son solo los de fábrica.
// Las credenciales de la Wi-Fi van aparte, en NetManager ("wifi-config").
class Settings {
public:
    struct Data {
        String lat, lon, city;   // ubicación del clima
        String tz;               // zona horaria POSIX
        uint8_t brightness;      // 5-255
        String apSsid, apPass;   // punto de acceso de configuración
        String bgHome;           // fondo de la pantalla principal (.565 en la SD)
        uint8_t accent;          // color de acento: 0 cian, 1 ámbar
        bool ssOn;               // salvapantallas activo
        uint8_t ssMin;           // minutos sin tocar la pantalla hasta que sale
        uint8_t ssMode;          // qué muestra: 0 reloj, 1 fotos
        uint8_t ssFace;          // face del reloj (SaverScreen::FACES)
        uint16_t ssPhotoSec;     // segundos entre fotos
        uint8_t radarKm;         // radio del radar: 10, 25, 50 o 100 km
        uint8_t radarGnd;        // filtro del radar: 0 en vuelo, 1 en tierra, 2 todos
    };

    // Qué ha cambiado desde el portal; main.cpp lo aplica (pantalla, hora, clima).
    enum Change : uint8_t { CHG_BRIGHT = 1, CHG_TZ = 2, CHG_LOC = 4, CHG_BG = 8, CHG_THEME = 16, CHG_SAVER = 32, CHG_RADAR = 64, CHG_RGND = 128 };

    static Data &get() { static Data d; return d; }

    static void load() {
        Data &d = get();
        Preferences p;
        p.begin(NS, false);   // rw: en el primer arranque crea el namespace
        // getString() saca un error por log si la clave no existe; comprobamos antes
        auto str = [&](const char *k, const char *def) { return p.isKey(k) ? p.getString(k, def) : String(def); };
        d.lat        = str("lat", OWM_LAT);
        d.lon        = str("lon", OWM_LON);
        d.city       = str("city", CITY_NAME);
        d.tz         = str("tz", TZ_INFO);
        d.brightness = p.getUChar("bright", BACKLIGHT);
        d.apSsid     = str("ap_ssid", AP_SSID);
        d.apPass     = str("ap_pass", AP_PASSWORD);
        d.bgHome     = str("bg_home", BG_HOME);
        d.accent     = p.getUChar("accent", 0) ? 1 : 0;
        d.ssOn       = p.getBool("ss_on", true);
        d.ssMin      = p.getUChar("ss_min", 5);
        d.ssMode     = p.getUChar("ss_mode", 0);
        d.ssFace     = p.getUChar("ss_face", 0);
        d.ssPhotoSec = p.getUShort("ss_photo", 30);
        d.radarKm    = p.getUChar("rdr_km", 25);
        d.radarGnd   = min<uint8_t>(p.getUChar("rdr_gnd", 0), 2);
        if (d.radarKm != 10 && d.radarKm != 25 && d.radarKm != 50 && d.radarKm != 100) d.radarKm = 25;
        p.end();
        if (d.brightness < 5) d.brightness = 5;
        if (d.apSsid.isEmpty()) d.apSsid = AP_SSID;
        Serial.printf("[Cfg] %s (%s,%s) tz=%s brillo=%u AP=%s fondo=%s\n", d.city.c_str(), d.lat.c_str(),
                      d.lon.c_str(), d.tz.c_str(), d.brightness, d.apSsid.c_str(), d.bgHome.c_str());
    }

    static void save() {
        const Data &d = get();
        Preferences p;
        p.begin(NS, false);
        p.putString("lat", d.lat);
        p.putString("lon", d.lon);
        p.putString("city", d.city);
        p.putString("tz", d.tz);
        p.putUChar("bright", d.brightness);
        p.putString("ap_ssid", d.apSsid);
        p.putString("ap_pass", d.apPass);
        p.putString("bg_home", d.bgHome);
        p.putUChar("accent", d.accent);
        p.putBool("ss_on", d.ssOn);
        p.putUChar("ss_min", d.ssMin);
        p.putUChar("ss_mode", d.ssMode);
        p.putUChar("ss_face", d.ssFace);
        p.putUShort("ss_photo", d.ssPhotoSec);
        p.putUChar("rdr_km", d.radarKm);
        p.putUChar("rdr_gnd", d.radarGnd);
        p.end();
    }

    static void markChanged(uint8_t what) { _pending() |= what; }

    // Devuelve y limpia los cambios pendientes
    static uint8_t takeChanges() {
        uint8_t v = _pending();
        _pending() = 0;
        return v;
    }

private:
    static constexpr const char *NS = "companion-cfg";
    static uint8_t &_pending() { static uint8_t v = 0; return v; }
};
