// See flight_service.h. Ported from the companion firmware (core/FlightService.h).
#include "flight_service.h"
#include "companion_settings.h"
#include "psram_json.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <math.h>

namespace flights {
namespace {

constexpr size_t BODY_MAX = 512 * 1024;  // body cap when it comes without Content-Length

// Military callsign prefixes (variable length). Add here.
const char* const MIL_PREFIX[] = {
    "AME", "ASCOT", "BAF", "CNV", "CTM", "DUKE", "FAF", "GAF", "HKY", "IAM",
    "KING", "MMF", "NATO", "PAT", "RCH", "REACH", "RRR", "SPAR",
};
// Cargo operators: 3-letter ICAO prefix. SORTED (binary search).
const char* const CARGO_ICAO[] = {
    "ABR", "ABW", "AHK", "AJT", "ATN", "BCS", "BOX", "CAO", "CKS", "CLX", "DHK", "DHL",
    "FDX", "GEC", "GTI", "ICV", "LCO", "MPH", "MSX", "NCA", "SWN", "TAY", "TPA", "UPS",
};

uint8_t classify(const char* call, uint32_t dbFlags) {
  if (dbFlags & 1) return MILITARY;  // dbFlags bit 0 = military
  for (const char* p : MIL_PREFIX)
    if (!strncmp(call, p, strlen(p))) return MILITARY;
  if (strlen(call) >= 3) {
    int lo = 0, hi = sizeof(CARGO_ICAO) / sizeof(CARGO_ICAO[0]) - 1;
    while (lo <= hi) {
      int mid = (lo + hi) / 2, c = strncmp(call, CARGO_ICAO[mid], 3);
      if (!c) return CARGO;
      if (c < 0) hi = mid - 1;
      else lo = mid + 1;
    }
  }
  return COMMERCIAL;
}

// writeToStream target: collects the body in PSRAM up to a cap.
struct PsramSink : public Stream {
  uint8_t* buf = nullptr;
  size_t len = 0, cap = 0, limit;
  bool overflow = false;
  explicit PsramSink(size_t m) : limit(m) {}
  ~PsramSink() { free(buf); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* d, size_t n) override {
    if (len + n > limit) {
      overflow = true;
      return 0;
    }
    if (len + n > cap) {
      size_t nc = std::max(cap ? cap * 2 : (size_t)16384, len + n);
      if (nc > limit) nc = limit;
      uint8_t* nb = (uint8_t*)ps_realloc(buf, nc);
      if (!nb) {
        overflow = true;
        return 0;
      }
      buf = nb;
      cap = nc;
    }
    memcpy(buf + len, d, n);
    len += n;
    return n;
  }
};

// What the task keeps: every plane in the radius (up to ALL), classified.
struct Store {
  Plane planes[ALL];
  int count = 0;
  State state = State::Idle;
  uint32_t seq = 0, at = 0;
  int httpCode = 0;
};

TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_mux = nullptr;
Store s_store;
volatile bool s_active = false;
volatile uint32_t s_due = 0;

void setState(State st, int code = 0) {
  xSemaphoreTake(s_mux, portMAX_DELAY);
  s_store.state = st;
  s_store.httpCode = code;
  if (st != State::Ok) s_store.seq++;
  xSemaphoreGive(s_mux);
}

void fetch() {
  const companion::Settings& cfg = companion::settings();
  const float lat0 = cfg.lat.toFloat(), lon0 = cfg.lon.toFloat();
  const int km = cfg.radarKm;
  const int nm = (int)ceilf(km / 1.852f);
  char url[112];
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", lat0, lon0, nm);

  WiFiClientSecure tls;
  tls.setInsecure();  // no certificate check: public data, read only
  HTTPClient http;
  // HTTP/1.1: adsb.lol answers HTTP/1.0 with 426 Upgrade Required
  static const char* HDRS[] = {"Transfer-Encoding"};
  http.collectHeaders(HDRS, 1);
  http.setTimeout(8000);
  http.setUserAgent("desktop-companion/1.0");
  if (!http.begin(tls, url)) {
    setState(State::Error, -1);
    return;
  }
  const int code = http.GET();
  if (code != 200) {
    Serial.printf("Radar: HTTP %d\n", code);
    http.end();
    setState(State::Error, code);
    return;
  }

  // Only the fields used: the full JSON has ~40 per plane
  // Both documents in PSRAM: internal RAM is short with TLS up (and WiFi needs it)
  JsonDocument filter(psramJson());
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs", "track", "true_heading",
                        "dbFlags", "category"})
    f[k] = true;
  JsonDocument doc(psramJson());
  DeserializationError err;
  if (http.getSize() > 0 && !http.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
    err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  } else {
    // No length (chunked): whole body into PSRAM; writeToStream removes the chunking
    PsramSink sink(BODY_MAX);
    const int n = http.writeToStream(&sink);
    if (n <= 0 || sink.overflow || !sink.buf) {
      Serial.printf("Radar: body not read (%d, %u B%s)\n", n, (unsigned)sink.len, sink.overflow ? ", too big" : "");
      http.end();
      setState(State::Error, -3);
      return;
    }
    err = deserializeJson(doc, sink.buf, sink.len, DeserializationOption::Filter(filter));
  }
  http.end();
  if (err) {
    Serial.printf("Radar: JSON %s (internal free %u)\n", err.c_str(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    setState(State::Error, -2);
    return;
  }

  static Plane tmp[ALL];  // every plane in the radius; the filter applies in snapshot()
  int n = 0;
  const float kx = cosf(lat0 * DEG_TO_RAD);
  for (JsonObject a : doc["ac"].as<JsonArray>()) {
    if (n >= ALL) break;
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
    const char* cat = a["category"] | "";
    if (cat[0] == 'C') continue;  // C1-C3: airport ground vehicles, not planes
    Plane& p = tmp[n];
    // Flat-earth distance and bearing: plenty for a 100 km radius
    const float dy = (a["lat"].as<float>() - lat0) * 111.32f;
    const float dx = (a["lon"].as<float>() - lon0) * 111.32f * kx;
    p.distKm = sqrtf(dx * dx + dy * dy);
    if (p.distKm > km) continue;
    p.bearing = fmodf(atan2f(dx, dy) * RAD_TO_DEG + 360.0f, 360.0f);
    const char* call = a["flight"] | "";
    if (!*call) call = a["r"] | "";
    if (!*call) call = a["hex"] | "?";
    strlcpy(p.call, call, sizeof(p.call));
    for (int i = strlen(p.call) - 1; i >= 0 && p.call[i] == ' '; i--) p.call[i] = 0;  // padded with spaces
    strlcpy(p.type, a["t"] | "", sizeof(p.type));
    // alt_baro: feet, or "ground" (-1); missing = -2 (unknown)
    if (a["alt_baro"].is<int>()) p.altFt = a["alt_baro"].as<int>();
    else p.altFt = a["alt_baro"].is<const char*>() ? -1 : -2;
    p.gsKt = (int16_t)lroundf(a["gs"] | 0.0f);
    if (a["track"].is<float>()) p.track = (int16_t)lroundf(a["track"].as<float>());
    else if (a["true_heading"].is<float>()) p.track = (int16_t)lroundf(a["true_heading"].as<float>());
    else p.track = -1;
    p.kind = classify(p.call, a["dbFlags"] | 0u);
    p.ground = p.altFt == -1 || (p.altFt >= 0 && p.altFt < 500 && p.gsKt < 40);
    n++;
  }
  std::sort(tmp, tmp + n, [](const Plane& a, const Plane& b) { return a.distKm < b.distKm; });

  xSemaphoreTake(s_mux, portMAX_DELAY);
  s_store.count = n;
  memcpy(s_store.planes, tmp, sizeof(Plane) * n);
  s_store.state = State::Ok;
  s_store.httpCode = 200;
  s_store.at = millis();
  s_store.seq++;
  xSemaphoreGive(s_mux);
  Serial.printf("Radar: %d planes within %d km\n", n, km);
}

void run(void*) {
  for (;;) {
    if (!s_active) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      if (s_store.state != State::NoNet) setState(State::NoNet);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    const uint32_t now = millis();
    if (s_due != 0 && (int32_t)(now - s_due) < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    s_due = now + REFRESH_MS;
    if (s_store.state != State::Ok) setState(State::Loading);
    fetch();
  }
}

}  // namespace

void begin() {
  if (s_task) return;
  s_mux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(run, "flights", 12288, nullptr, 1, &s_task, 0);
}

void setActive(bool on) {
  s_active = on;
  if (on) s_due = 0;  // fetch now
}

void refreshSoon() { s_due = 0; }

void snapshot(Snapshot& out, uint8_t filter) {
  if (!s_mux) return;
  xSemaphoreTake(s_mux, portMAX_DELAY);
  out.state = s_store.state;
  out.at = s_store.at;
  out.httpCode = s_store.httpCode;
  out.seq = s_store.seq * 4 + filter;  // also changes with the filter
  out.total = s_store.count;
  out.count = 0;
  for (int i = 0; i < s_store.count && out.count < MAX; i++) {
    const Plane& p = s_store.planes[i];
    if (filter == BOTH || (filter == ON_GROUND) == p.ground) out.planes[out.count++] = p;
  }
  xSemaphoreGive(s_mux);
}

}  // namespace flights
