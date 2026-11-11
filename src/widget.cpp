// Widget registry and the weather widget. See widget.h.
#include "widget.h"
#include "deck_ui_int.h"
#include "companion_settings.h"
#include "weather_service.h"

namespace widget {
namespace {

using namespace deckui;

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, lv_opa_t opa = LV_OPA_COVER) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_opa(l, opa, 0);
  lv_label_set_text(l, "");
  lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

void show(lv_obj_t* o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// --- Tiempo -----------------------------------------------------------------
// compacto: icon and temperature. detalle: plus condition, humidity and city,
// dropped one by one when the tile is too small for them.
struct Weather {
  bool detail;
  lv_obj_t *top, *icon, *temp, *cond, *extra, *city;
  uint32_t key = UINT32_MAX;
  int w = 0, h = 0;
};

void* weatherCreate(lv_obj_t* t, const char* variant) {
  Weather* s = new (std::nothrow) Weather();
  if (!s) return nullptr;
  s->detail = !strcmp(variant, "detalle");
  // Icon and temperature side by side, the rest below
  s->top = lv_obj_create(t);
  lv_obj_remove_style_all(s->top);
  lv_obj_set_size(s->top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s->top, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s->top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(s->top, 6, 0);
  lv_obj_remove_flag(s->top, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  s->icon = label(s->top, &deck_font_28);
  s->temp = label(s->top, &deck_font_28);
  s->cond = label(t, &deck_font_14);
  s->extra = label(t, &deck_font_12, LV_OPA_70);
  s->city = label(t, &deck_font_12, LV_OPA_60);
  for (lv_obj_t* l : {s->cond, s->extra, s->city}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_max_width(l, LV_PCT(100), 0);
  }
  return s;
}

bool weatherUpdate(void* p) {
  Weather* s = (Weather*)p;
  if (!s || !s->w) return false;
  const weather::Data d = weather::get();
  const bool icons = iconsReady();
  const uint32_t key = d.seq * 4 + (icons ? 2 : 0) + (d.ok ? 1 : 0);
  if (key == s->key) return false;
  s->key = key;

  // Room decides what fits: the icon follows the tile's height
  const int big = s->w >= 120 && s->h >= 70;
  const lv_font_t* tempFont = big ? &deck_font_28 : &deck_font_20;
  lv_obj_set_style_text_font(s->temp, tempFont, 0);
  int iconPx = s->detail ? (s->h >= 110 ? 40 : 28) : std::min(s->h * 45 / 100, 56);
  if (iconPx < 20) iconPx = 20;
  const uint32_t cp = d.ok && icons ? iconCodepoint(weather::mdiIcon(d)) : 0;
  const lv_font_t* f = cp ? iconFont(iconPx, cp) : nullptr;
  show(s->icon, f != nullptr);
  if (f) {
    char u[5];
    iconUtf8(cp, u);
    lv_obj_set_style_text_font(s->icon, f, 0);
    lv_label_set_text(s->icon, u);
  }
  char buf[32];
  if (d.ok) snprintf(buf, sizeof(buf), "%d°", (int)lroundf(d.temp));
  else snprintf(buf, sizeof(buf), "--°");
  lv_label_set_text(s->temp, buf);

  const int lines = s->detail ? (s->h >= 120 ? 3 : s->h >= 90 ? 2 : s->h >= 60 ? 1 : 0) : 0;
  // Without the icon font the condition says what the icon would
  const bool condLine = lines >= 1 || (!f && s->h >= 60);
  show(s->cond, condLine && d.ok);
  lv_label_set_text(s->cond, d.ok ? weather::condition(d.code) : "");
  show(s->extra, lines >= 2 && d.ok);
  snprintf(buf, sizeof(buf), "Humedad %d%%", d.humidity);
  lv_label_set_text(s->extra, buf);
  show(s->city, lines >= 3);
  lv_label_set_text(s->city, companion::settings().city.c_str());
  return true;
}

void weatherResize(void* p, int w, int h) {
  Weather* s = (Weather*)p;
  if (!s || (s->w == w && s->h == h)) return;
  s->w = w;
  s->h = h;
  s->key = UINT32_MAX;  // relayout on the next update
}

void weatherDestroy(void* p) { delete (Weather*)p; }

const Variant kWeatherVariants[] = {{"compacto", "Compacto"}, {"detalle", "Detalle"}};

// --- Registry -----------------------------------------------------------------
const Def kWeather = {"tiempo", "Tiempo", nullptr, kWeatherVariants, 2,
                      weatherCreate, weatherUpdate, weatherResize, weatherDestroy};

const Def* const kWidgets[] = {&kWeather};
constexpr size_t kCount = sizeof(kWidgets) / sizeof(kWidgets[0]);

}  // namespace

const Def* find(const char* id) {
  for (const Def* d : kWidgets)
    if (!strcmp(d->id, id)) return d;
  return nullptr;
}

size_t count() { return kCount; }
const Def* get(size_t i) { return i < kCount ? kWidgets[i] : nullptr; }

const char* variantOr(const Def* d, const char* variant) {
  for (uint8_t i = 0; i < d->variantCount; i++)
    if (!strcmp(d->variants[i].id, variant)) return d->variants[i].id;
  return d->variants[0].id;
}

}  // namespace widget
