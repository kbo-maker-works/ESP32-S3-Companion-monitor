// App registry and the simple apps. See app.h.
#include "app.h"
#include "deck_ui_int.h"
#include "config.h"
#include <WiFi.h>

namespace app {
namespace {

using namespace deckui;

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

// --- Placeholder: apps not written yet ----------------------------------------------
void openSoon(lv_obj_t* body, int, int) {
  lv_obj_t* l = label(body, &deck_font_20, C_DIM, "Próximamente");
  lv_obj_center(l);
}

// --- Ajustes: how to reach the portal, and the device at a glance -------------------
struct InfoRow {
  lv_obj_t* value;
};
InfoRow s_ip, s_net, s_mem, s_up;
lv_obj_t* s_qr = nullptr;
String s_qrUrl;
uint32_t s_infoAt = 0;

String portalUrl() {
  if (WiFi.status() != WL_CONNECTED) return "http://" + WiFi.softAPIP().toString() + "/";
  return "http://" + WiFi.localIP().toString() + "/";
}

InfoRow infoRow(lv_obj_t* col, const char* name) {
  lv_obj_t* row = lv_obj_create(col);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
  label(row, &deck_font_14, C_DIM, name);
  lv_obj_t* v = label(row, &deck_font_20, C_TEXT, "");
  lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_width(v, LV_PCT(100));
  return {v};
}

void infoRefresh() {
  const bool sta = WiFi.status() == WL_CONNECTED;
  String url = portalUrl();
  if (url != s_qrUrl) {
    s_qrUrl = url;
    lv_qrcode_update(s_qr, url.c_str(), url.length());
  }
  String ip = url.substring(7, url.length() - 1);
  if (sta && netSettings.mdnsEnabled && netSettings.hostname[0]) ip += "  ·  " + String(netSettings.hostname) + ".local";
  lv_label_set_text(s_ip.value, ip.c_str());
  lv_label_set_text(s_net.value, sta ? WiFi.SSID().c_str() : "Sin red (modo AP)");
  lv_label_set_text_fmt(s_mem.value, "%u KB interna  ·  %u KB PSRAM",
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  const uint32_t s = millis() / 1000;
  if (s >= 86400) lv_label_set_text_fmt(s_up.value, "%ud %uh %um", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60));
  else lv_label_set_text_fmt(s_up.value, "%uh %um %us", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

void openInfo(lv_obj_t* body, int w, int h) {
  const int pad = 16, qr = h - 2 * pad - 36 < 180 ? h - 2 * pad - 36 : 180;
  // QR to the portal on white, with the hint below it
  lv_obj_t* box = lv_obj_create(body);
  lv_obj_remove_style_all(box);
  lv_obj_set_style_bg_color(box, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(box, 14, 0);
  lv_obj_set_style_pad_all(box, 10, 0);
  lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_pos(box, pad, pad);
  s_qr = lv_qrcode_create(box);
  lv_qrcode_set_size(s_qr, qr - 20);
  lv_qrcode_set_dark_color(s_qr, lv_color_black());
  lv_qrcode_set_light_color(s_qr, lv_color_white());
  s_qrUrl = "";
  lv_obj_t* hint = label(body, &deck_font_14, C_DIM, "Escanéalo para abrir el portal");
  lv_obj_set_width(hint, qr);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(hint, pad, pad + qr + 8);

  lv_obj_t* col = lv_obj_create(body);
  lv_obj_remove_style_all(col);
  const int x = pad + qr + 20;
  lv_obj_set_pos(col, x, pad);
  lv_obj_set_size(col, w - x - pad, h - 2 * pad);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 10, 0);
  s_ip = infoRow(col, "PORTAL");
  s_net = infoRow(col, "RED");
  s_mem = infoRow(col, "MEMORIA LIBRE");
  s_up = infoRow(col, "ENCENDIDO");
  lv_obj_t* fw = label(col, &deck_font_14, C_DIM, "Firmware " FW_VERSION);
  lv_obj_set_style_margin_top(fw, 4, 0);
  infoRefresh();
  s_infoAt = millis();
}

void tickInfo() {
  if (millis() - s_infoAt < 1000) return;
  s_infoAt = millis();
  infoRefresh();
}

void closeInfo() { s_qr = nullptr; }

// --- Registry ------------------------------------------------------------------
// Order = order in the portal's app list.
const Def kApps[] = {
    {"ia", "IA", "mdi:robot-outline", false, openSoon, nullptr, nullptr},
    {"sistema", "Sistema", "mdi:chip", false, openSoon, nullptr, nullptr},
    {"red", "Red", "mdi:lan", false, openSoon, nullptr, nullptr},
    {"radar", "Radar", "mdi:radar", false, openSoon, nullptr, nullptr},
    {"audio", "Audio", "mdi:speaker", false, openSoon, nullptr, nullptr},
    {"ajustes", "Ajustes", "mdi:cog-outline", true, openInfo, tickInfo, closeInfo},
};

}  // namespace

const Def* find(const char* id) {
  for (const Def& d : kApps)
    if (!strcmp(d.id, id)) return &d;
  return nullptr;
}

const Def* all(size_t& count) {
  count = sizeof(kApps) / sizeof(kApps[0]);
  return kApps;
}

}  // namespace app
