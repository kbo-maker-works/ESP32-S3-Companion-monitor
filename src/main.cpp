#include <Arduino.h>
#include "config.h"
#include "core/Pins.h"
#include "core/DisplayManager.h"
#include "core/TouchPanel.h"
#include "core/Settings.h"
#include "core/NetManager.h"
#include "core/Toast.h"
#include "core/PcLink.h"
#include "core/WeatherService.h"
#include "core/Background.h"
#include "core/SdManager.h"
#include "core/TextAA.h"
#include "core/Power.h"
#include "fonts/Fonts.h"
#include "screens/HomeScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/SaverScreen.h"

// Desktop companion — Guition JC3248W535 (ESP32-S3, 3.5" 480x320 táctil)
//
// Base limpia: pantalla, táctil, Wi-Fi + portal, ajustes, avisos y enlace con
// el PC. Las pantallas se irán añadiendo una a una.

#ifndef TOUCH_DEBUG
#define TOUCH_DEBUG 0
#endif
// El fondo de la home se elige desde el portal (Settings::bgHome)

DisplayManager display;
TouchPanel touch;
PcLink pcLink;

// ---------------------------------------------------------------------------
// Red y ajustes del portal
// ---------------------------------------------------------------------------
static void applyTimezone() {
    configTzTime(Settings::get().tz.c_str(), NTP_SERVER, "time.google.com");
}

// Aviso en pantalla cuando cambia la conexión
static void notifyNet(NetManager::Event e) {
    String ssid = NetManager::savedSsid();
    switch (e) {
        case NetManager::Event::Connected:
            Toast::show("Wi-Fi conectado · " + ssid,
                        NetManager::staIp() + " · " + NetManager::mdnsHost(), 6000, 0x07E0);
            break;
        case NetManager::Event::Disconnected:
            Toast::show("Wi-Fi desconectado", String("Reintentando · portal ") + NetManager::apSsid(),
                        5000, 0xFD20);
            break;
        case NetManager::Event::Failed:
            Toast::show("No se pudo conectar a " + ssid,
                        String(NetManager::reasonText()) + " · revisa el portal", 7000, 0xF800);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Pantallas: inicio (lanzador de módulos), Ajustes y salvapantallas
// ---------------------------------------------------------------------------
enum class Screen { Home, Settings, Saver };
static Screen screen = Screen::Home;
static HomeScreen homeScreen;
static SettingsScreen settingsScreen;
static SaverScreen saverScreen;
static Screen saverReturn = Screen::Home;   // a dónde vuelve el salvapantallas
static uint32_t lastTouchMs = 0;
static bool swallowGesture = false;          // el toque que despierta no cuenta como gesto

// Pinta la pantalla actual; con force=false solo si algo ha cambiado
static bool drawScreen(bool force) {
    StatusBar::setPcOnline(pcLink.isOnline());
    if (screen == Screen::Saver) return saverScreen.update(display.gfx(), force);
    if (screen == Screen::Settings) {
        if (force) settingsScreen.draw(display.gfx());
        return force;
    }
    if (!homeScreen.needsRedraw(touch) && !force) return false;
    homeScreen.draw(display.gfx());
    return true;
}

static void openScreen(Screen s) {
    if (screen == Screen::Settings) settingsScreen.exit();
    screen = s;
    if (s == Screen::Settings) settingsScreen.enter();
}

// Cambios guardados desde el portal o desde Ajustes. Devuelve true si hay que repintar.
static bool applySettings(uint8_t chg) {
    bool repaint = false;
    if (chg & Settings::CHG_BRIGHT) display.setBacklight(Settings::get().brightness);
    if (chg & Settings::CHG_TZ) {
        applyTimezone();
        Toast::show("Zona horaria actualizada");
        repaint = true;
    }
    if (chg & Settings::CHG_LOC) {
        WeatherService::refreshSoon();
        Toast::show("Ubicación: " + Settings::get().city);
    }
    if (chg & Settings::CHG_BG) {
        String n = Settings::get().bgHome;
        n = n.substring(n.lastIndexOf('/') + 1);
        n.replace(".565", "");
        Toast::show("Fondo: " + n, "", 1500, Theme::accent());
        if (screen == Screen::Settings) settingsScreen.refresh();
        repaint = true;
    }
    if (chg & Settings::CHG_THEME) repaint = true;
    return repaint;
}

// Salvapantallas: entra tras ssMin minutos sin tocar y sale con cualquier toque,
// volviendo a la pantalla de antes tal cual estaba. No toca el brillo.
static bool updateSaver() {
    const auto &s = Settings::get();
    if (touch.isDown()) {
        lastTouchMs = millis();
        if (screen == Screen::Saver && !swallowGesture) {
            saverScreen.exit();
            screen = saverReturn;
            swallowGesture = true;
            return true;
        }
    }
    if (screen != Screen::Saver && s.ssOn && millis() - lastTouchMs > (uint32_t)s.ssMin * 60000UL) {
        saverReturn = screen;
        screen = Screen::Saver;
        saverScreen.enter();
        Serial.println("[Saver] inactivo: salvapantallas");
        return true;
    }
    return false;
}

static bool handleGesture(Gesture gs) {
    if (gs == Gesture::None || screen == Screen::Saver) return false;
    if (screen == Screen::Home) {
        if (gs != Gesture::Tap) return false;
        int i = homeScreen.hit(touch.startX(), touch.startY());
        if (i < 0) return false;
        const HomeScreen::App &a = homeScreen.apps[i];
        if (!strcmp(a.id, "icon_cfg")) { openScreen(Screen::Settings); return true; }
        Toast::show(String(a.label) + " · aún no disponible", "", 1500, Theme::MUTED);
        return false;
    }
    switch (settingsScreen.handle(gs, touch.startX(), touch.startY())) {
        case SettingsScreen::Result::Exit:   openScreen(Screen::Home); return true;
        case SettingsScreen::Result::Redraw: return true;
        default:                             return false;
    }
}

// ---------------------------------------------------------------------------

void setup() {
    pcLink.begin(115200);   // Serial (USB CDC): logs + enlace con el pc-agent
    delay(200);

    Settings::load();
    display.begin(Settings::get().brightness);
    touch.begin();
    Background::beginSD();

    Background::draw(Settings::get().bgHome.c_str(), 0x0841);
    Background::dim(120);
    TextAA::drawCentered(Nunito18, SCREEN_W / 2, 170, "Conectando Wi-Fi...", 0x9CF3);
    display.flush();

    NetManager::begin();    // AP siempre activo; espera hasta 8 s a la red guardada
    SdManager::setupRoutes(NetManager::web());   // gestor de la microSD en el portal

    drawScreen(true);
    display.flush();
}

void loop() {
    NetManager::loop();
    pcLink.update();

    static bool ntp = false;
    if (!ntp && NetManager::isConnected()) {
        applyTimezone();
        ntp = true;
    }
    notifyNet(NetManager::takeEvent());
    if (NetManager::isConnected()) WeatherService::loop();   // tiempo de la barra superior (cada 10 min)
    static int usb = -1;   // regla: con PC por USB nunca se atenúa la pantalla (core/Power.h)
    if ((int)Power::usbHost() != usb) { usb = Power::usbHost(); Serial.printf("[Power] host USB: %s\n", usb ? "si" : "no"); }

    bool force = false;
    if (uint8_t chg = Settings::takeChanges()) force |= applySettings(chg);

    Gesture gs = touch.update();
    if (gs != Gesture::None) Serial.printf("[Touch] %s (%d,%d)\n", gestureName(gs), touch.startX(), touch.startY());
    force |= updateSaver();
    if (swallowGesture && gs != Gesture::None) { gs = Gesture::None; swallowGesture = false; }
    force |= handleGesture(gs);
    // Un cambio hecho desde Ajustes se aplica en la siguiente vuelta (markChanged)

    // Aviso flotante: al caducar se repinta la pantalla entera para borrarlo
    if (Toast::takeExpired()) force = true;
    bool dirty = drawScreen(force);
    if (Toast::takeFresh()) dirty = true;
    if (dirty) Toast::draw();

#if TOUCH_DEBUG
    if (touch.isDown()) {
        display.gfx().fillCircle(touch.x(), touch.y(), 4, 0xF800);
        dirty = true;
    }
#endif

    if (dirty) display.flush();
    delay(5);
}
