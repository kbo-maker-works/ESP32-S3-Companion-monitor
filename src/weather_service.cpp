// See weather_service.h. Ported from the companion firmware (core/WeatherService.h),
// moved off the loop into a task.
#include "weather_service.h"
#include "companion_settings.h"
#include "psram_json.h"
#include <WiFi.h>
#include <HTTPClient.h>

namespace weather {
namespace {

constexpr uint32_t POLL_MS = 600000;   // 10 min
constexpr uint32_t RETRY_MS = 60000;   // after a failure

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Data s_data;
TaskHandle_t s_task = nullptr;
volatile uint32_t s_due = 0;

bool fetch() {
  const companion::Settings& cs = companion::settings();
  const String url = "http://api.open-meteo.com/v1/forecast?latitude=" + cs.lat + "&longitude=" + cs.lon +
                     "&current=temperature_2m,relative_humidity_2m,weather_code,is_day";
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(url)) return false;
  const int code = http.GET();
  if (code != 200) {
    Serial.printf("Weather: HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument doc(psramJson());
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("Weather: JSON %s\n", err.c_str());
    return false;
  }
  JsonObjectConst cur = doc["current"];
  portENTER_CRITICAL(&s_mux);
  s_data.temp = cur["temperature_2m"] | 0.0f;
  s_data.humidity = cur["relative_humidity_2m"] | 0;
  s_data.code = cur["weather_code"] | 0;
  s_data.isDay = (cur["is_day"] | 1) == 1;
  s_data.ok = true;
  s_data.failing = false;
  s_data.seq++;
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("Weather: %.1f C, %d%%, %s\n", s_data.temp, s_data.humidity, condition(s_data.code));
  return true;
}

void run(void*) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(500));
    if (WiFi.status() != WL_CONNECTED) continue;
    const uint32_t now = millis();
    if (s_due != 0 && (int32_t)(now - s_due) < 0) continue;
    const bool ok = fetch();
    s_due = millis() + (ok ? POLL_MS : RETRY_MS);
    if (!ok) {
      portENTER_CRITICAL(&s_mux);
      if (!s_data.failing) {
        s_data.failing = true;
        s_data.seq++;
      }
      portEXIT_CRITICAL(&s_mux);
    }
  }
}

}  // namespace

void begin() {
  if (s_task) return;
  xTaskCreatePinnedToCore(run, "weather", 6144, nullptr, 1, &s_task, 0);
}

void refreshSoon() { s_due = 0; }

Data get() {
  portENTER_CRITICAL(&s_mux);
  Data d = s_data;
  portEXIT_CRITICAL(&s_mux);
  return d;
}

const char* condition(int c) {
  if (c == 0) return "Despejado";
  if (c >= 1 && c <= 3) return "Nublado";
  if (c == 45 || c == 48) return "Niebla";
  if (c >= 51 && c <= 67) return "Lluvia";
  if (c >= 71 && c <= 77) return "Nieve";
  if (c >= 80 && c <= 82) return "Chubascos";
  if (c >= 85 && c <= 86) return "Nieve";
  if (c >= 95) return "Tormenta";
  return "Templado";
}

const char* mdiIcon(const Data& d) {
  const int c = d.code;
  if (c == 0) return d.isDay ? "weather-sunny" : "weather-night";
  if (c == 1 || c == 2) return d.isDay ? "weather-partly-cloudy" : "weather-night-partly-cloudy";
  if (c == 3) return "weather-cloudy";
  if (c == 45 || c == 48) return "weather-fog";
  if (c == 65 || c == 67 || (c >= 80 && c <= 82)) return "weather-pouring";
  if (c >= 51 && c <= 67) return "weather-rainy";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "weather-snowy";
  if (c >= 95) return "weather-lightning-rainy";
  return "weather-cloudy";
}

}  // namespace weather
