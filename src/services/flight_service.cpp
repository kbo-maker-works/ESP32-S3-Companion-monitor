// See flight_service.h. Ported from the companion firmware (core/FlightService.h).
#include "services/flight_service.h"
#include "core/companion_settings.h"
#include "core/psram_json.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <math.h>

namespace flights {
namespace {

constexpr size_t BODY_MAX = 512 * 1024;  // response body cap (100 km is ~35 KB)
constexpr int SCAN = 400;                // planes read from one response, before the ALL nearest

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
uint8_t s_holders = 0;           // Holder bits (loop task only)
uint32_t s_lastStartMs = 0;      // when a fetch was last started

void setState(State st, int code = 0) {
  xSemaphoreTake(s_mux, portMAX_DELAY);
  s_store.state = st;
  s_store.httpCode = code;
  if (st != State::Ok) s_store.seq++;
  xSemaphoreGive(s_mux);
}

// Sources with the same readsb v2 JSON (only the array's key differs), tried
// in order. One that answers 429 is skipped until its quota resets.
// adsb.lol: 500 requests a day per IP. adsb.fi: 1 a second, no daily cap.
struct Source {
  const char* name;
  const char* fmt;  // lat, lon, radius in nautical miles
  const char* key;  // the array of planes
};
const Source SOURCES[] = {
    {"adsb.lol", "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", "ac"},
    {"adsb.fi", "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", "aircraft"},
};
constexpr int NSRC = sizeof(SOURCES) / sizeof(SOURCES[0]);
constexpr uint32_t BLOCK_DEFAULT_S = 3600;  // when a 429 has no reset header
uint32_t s_blockedUntil[NSRC] = {};          // millis(), 0 = usable (task only)

bool blocked(int i, uint32_t now) {
  if (!s_blockedUntil[i]) return false;
  if ((int32_t)(now - s_blockedUntil[i]) >= 0) {
    s_blockedUntil[i] = 0;
    return false;
  }
  return true;
}

void fetch() {
  const companion::Place cfg = companion::place();  // a copy: the portal may change the settings meanwhile
  const float lat0 = cfg.lat, lon0 = cfg.lon;
  const int km = cfg.radarKm;
  const int nm = (int)ceilf(km / 1.852f);

  WiFiClientSecure tls;
  tls.setInsecure();  // no certificate check: public data, read only
  HTTPClient http;
  // HTTP/1.1 (HTTPClient's default): adsb.lol answers HTTP/1.0 with 426 Upgrade Required
  http.setTimeout(8000);
  http.setUserAgent("desktop-companion/1.0");
  static const char* HDRS[] = {"x-ratelimit-reset", "retry-after"};

  const Source* src = nullptr;
  int code = 429;  // all blocked: report the rate limit
  for (int i = 0; i < NSRC; i++) {
    if (blocked(i, millis())) continue;
    char url[112];
    snprintf(url, sizeof(url), SOURCES[i].fmt, lat0, lon0, nm);
    if (!http.begin(tls, url)) {
      code = -1;
      continue;
    }
    http.collectHeaders(HDRS, 2);
    code = http.GET();
    if (code == 200) {
      src = &SOURCES[i];
      break;
    }
    if (code == 429) {
      uint32_t secs = http.header("x-ratelimit-reset").toInt();
      if (!secs) secs = http.header("retry-after").toInt();
      if (!secs) secs = BLOCK_DEFAULT_S;
      s_blockedUntil[i] = (millis() + secs * 1000UL) | 1;
      Serial.printf("Radar: %s rate limited, skipped for %lu s\n", SOURCES[i].name, (unsigned long)secs);
    } else {
      Serial.printf("Radar: %s HTTP %d\n", SOURCES[i].name, code);
    }
    http.end();
  }
  if (!src) {
    setState(State::Error, code);
    return;
  }

  // Only the fields used: the full JSON has ~40 per plane
  // Both documents in PSRAM: internal RAM is short with TLS up (and WiFi needs it)
  JsonDocument filter(psramJson());
  JsonObject f = filter[src->key].add<JsonObject>();
  for (const char* k : {"hex", "flight", "r", "t", "lat", "lon", "alt_baro", "gs", "track", "true_heading",
                        "dbFlags", "category"})
    f[k] = true;
  JsonDocument doc(psramJson());
  // Whole body into PSRAM first, then parse: parsing straight off the TLS
  // stream gave IncompleteInput at 100 km (a pause between TLS records reads
  // as the end of the document). writeToStream waits out the pauses with the
  // HTTP timeout and removes chunking when there is any.
  PsramSink sink(BODY_MAX);
  const int got = http.writeToStream(&sink);
  http.end();
  if (got <= 0 || sink.overflow || !sink.buf) {
    Serial.printf("Radar: body not read (%d, %u B%s)\n", got, (unsigned)sink.len, sink.overflow ? ", too big" : "");
    setState(State::Error, -3);
    return;
  }
  const DeserializationError err = deserializeJson(doc, sink.buf, sink.len, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Radar: JSON %s (internal free %u)\n", err.c_str(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    setState(State::Error, -2);
    return;
  }

  // Every plane in the radius, then the ALL nearest are kept (the response is
  // not sorted by distance). The filter applies in snapshot().
  static Plane* tmp = nullptr;  // retried each fetch: one failed allocation must not break the radar till reboot
  if (!tmp) tmp = (Plane*)ps_malloc(sizeof(Plane) * SCAN);
  if (!tmp) {
    setState(State::Error, -4);
    return;
  }
  int n = 0;
  const float kx = cosf(lat0 * DEG_TO_RAD);
  for (JsonObject a : doc[src->key].as<JsonArray>()) {
    if (n >= SCAN) break;
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
  const int inRadius = n;
  if (n > ALL) n = ALL;

  xSemaphoreTake(s_mux, portMAX_DELAY);
  s_store.count = n;
  memcpy(s_store.planes, tmp, sizeof(Plane) * n);
  s_store.state = State::Ok;
  s_store.httpCode = 200;
  s_store.at = millis();
  s_store.seq++;
  xSemaphoreGive(s_mux);
  Serial.printf("Radar: %d planes within %d km from %s (%u B)\n", inRadius, km, src->name, (unsigned)sink.len);
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
    s_lastStartMs = now | 1;
    if (s_store.state != State::Ok) setState(State::Loading);
    fetch();
  }
}

}  // namespace

void begin() {
  if (s_task) return;
  s_mux = xSemaphoreCreateMutex();
  // Stack in PSRAM: it idles until the radar opens, and only fetches and parses
  // (no flash or NVS writes, which need an internal stack). TLS runs fine on one.
  xTaskCreatePinnedToCoreWithCaps(run, "flights", 12288, nullptr, 1, &s_task, 0, MALLOC_CAP_SPIRAM);
}

void setActive(bool on, uint8_t who) {
  const bool was = s_holders != 0;
  s_holders = on ? (s_holders | who) : (s_holders & ~who);
  s_active = s_holders != 0;
  // Fetch now when it starts, unless the last answer is still fresh (a page
  // with the widget swiped away and back).
  if (s_active && !was && (!s_lastStartMs || millis() - s_lastStartMs >= REFRESH_MS)) s_due = 0;
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
