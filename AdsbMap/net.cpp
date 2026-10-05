// WLAN, ADS-B-Abfrage (adsb.lol), Routenabfrage und Kartenkacheln (CARTO dark)
#include "app.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <math.h>

#define UA "ESP32-ADSB-Map/" APP_VERSION

Aircraft *g_ac;
SemaphoreHandle_t g_lock;
RouteInfo g_route;
volatile uint32_t g_dataVersion = 0, g_tileVersion = 0, g_routeVersion = 0;
volatile int g_acCount = 0;
volatile bool g_apiOk = false, g_wifiOk = false, g_scanBusy = false, g_reconnect = false;
volatile double g_queryLat = 0, g_queryLon = 0;
volatile int g_queryRadiusNm = 60;
volatile bool g_forcePoll = false;  // Ansicht stark geändert -> sofort neu abfragen

// ------------------------------------------------------------------ Routen-Auftrag
static volatile bool s_routePending = false;
static uint32_t s_routeHex;
static char s_routeCs[10];
static double s_routeLat, s_routeLon;

void netRequestRoute(uint32_t hex, const char *callsign, double lat, double lon) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  memset(&g_route, 0, sizeof(g_route));
  g_route.hex = hex;
  if (!callsign || !callsign[0]) {
    g_route.state = 3;  // ohne Rufzeichen keine Route
  } else {
    g_route.state = 1;
    s_routeHex = hex;
    strlcpy(s_routeCs, callsign, sizeof(s_routeCs));
    s_routeLat = lat;
    s_routeLon = lon;
    s_routePending = true;
  }
  xSemaphoreGive(g_lock);
  g_routeVersion = g_routeVersion + 1;
}

// ------------------------------------------------------------------ Hilfen
static void trimCopy(char *dst, size_t n, const char *src) {
  if (!src) { dst[0] = 0; return; }
  while (*src == ' ') src++;
  strlcpy(dst, src, n);
  size_t l = strlen(dst);
  while (l && dst[l - 1] == ' ') dst[--l] = 0;
}

// Speicher-Stream für HTTPClient::writeToStream (dekodiert auch chunked)
class BufStream : public Stream {
 public:
  uint8_t *buf; size_t cap, len = 0; bool overflow = false;
  BufStream(uint8_t *b, size_t c) : buf(b), cap(c) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *d, size_t n) override {
    if (len + n > cap) { overflow = true; n = cap - len; }
    memcpy(buf + len, d, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

// ------------------------------------------------------------------ ADS-B
static void fetchAircraft(WiFiClientSecure &cli) {
  char url[128];
  double qlat = g_queryLat, qlon = g_queryLon;
  int r = g_queryRadiusNm;
  snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", qlat, qlon, r);

  HTTPClient http;
  http.useHTTP10(true);  // kein chunked -> direktes Stream-Parsing
  http.setTimeout(10000);
  http.setConnectTimeout(8000);
  if (!http.begin(cli, url)) { g_apiOk = false; return; }
  http.setUserAgent(UA);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[adsb] HTTP %d\n", code);
    http.end();
    g_apiOk = false;
    return;
  }

  JsonDocument filter;
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char *k : {"hex", "flight", "r", "t", "desc", "lat", "lon", "track", "true_heading", "gs", "alt_baro", "seen_pos", "seen"})
    f[k] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[adsb] JSON: %s\n", err.c_str());
    g_apiOk = false;
    return;
  }

  uint32_t now = millis();
  JsonArray arr = doc["ac"].as<JsonArray>();
  xSemaphoreTake(g_lock, portMAX_DELAY);
  for (JsonObject o : arr) {
    if (!o["lat"].is<double>() || !o["lon"].is<double>()) continue;
    const char *hs = o["hex"] | "";
    if (*hs == '~') continue;  // TIS-B ohne echte ICAO-Adresse
    uint32_t hex = strtoul(hs, nullptr, 16);
    if (!hex) continue;

    int slot = -1, freeSlot = -1;
    for (int i = 0; i < MAX_AC; i++) {
      if (g_ac[i].used && g_ac[i].hex == hex) { slot = i; break; }
      if (!g_ac[i].used && freeSlot < 0) freeSlot = i;
    }
    bool fresh = false;
    if (slot < 0) {
      if (freeSlot < 0) continue;
      slot = freeSlot;
      memset(&g_ac[slot], 0, sizeof(Aircraft));
      g_ac[slot].used = true;
      g_ac[slot].hex = hex;
      fresh = true;
    }
    Aircraft &a = g_ac[slot];
    trimCopy(a.flight, sizeof(a.flight), o["flight"] | "");
    trimCopy(a.reg, sizeof(a.reg), o["r"] | "");
    trimCopy(a.type, sizeof(a.type), o["t"] | "");
    trimCopy(a.desc, sizeof(a.desc), o["desc"] | "");
    double lat = o["lat"], lon = o["lon"];
    if (o["track"].is<float>()) a.track = o["track"];
    else if (o["true_heading"].is<float>()) a.track = o["true_heading"];
    a.gs = o["gs"] | 0.0f;
    if (o["alt_baro"].is<int>()) { a.alt = o["alt_baro"]; a.ground = false; }
    else if (o["alt_baro"].is<const char *>()) { a.alt = 0; a.ground = true; }
    else { a.alt = INT32_MIN; a.ground = false; }
    float seenPos = o["seen_pos"] | 0.0f;
    if (seenPos > 60) { if (fresh) a.used = false; continue; }  // veraltete Position

    // Spur fortschreiben (nur wenn sich die Position merklich geändert hat)
    bool add = a.trailN == 0;
    if (!add) {
      int last = (a.trailHead + TRAIL_LEN - 1) % TRAIL_LEN;
      add = haversineKm(a.trailLat[last], a.trailLon[last], lat, lon) > 0.08;
    }
    if (add) {
      a.trailLat[a.trailHead] = lat;
      a.trailLon[a.trailHead] = lon;
      a.trailHead = (a.trailHead + 1) % TRAIL_LEN;
      if (a.trailN < TRAIL_LEN) a.trailN++;
    }
    a.lat = lat;
    a.lon = lon;
    a.posMs = now - (uint32_t)(seenPos * 1000);
    a.seenMs = now;
  }
  int cnt = 0;
  for (int i = 0; i < MAX_AC; i++) {
    if (!g_ac[i].used) continue;
    if (now - g_ac[i].seenMs > AC_TIMEOUT_MS) g_ac[i].used = false;
    else cnt++;
  }
  g_acCount = cnt;
  xSemaphoreGive(g_lock);
  g_apiOk = true;
  g_dataVersion = g_dataVersion + 1;
}

// ------------------------------------------------------------------ Route (Start-/Zielflughafen)
static void readAirport(JsonObject o, Airport &ap) {
  memset(&ap, 0, sizeof(ap));
  strlcpy(ap.icao, o["icao"] | "", sizeof(ap.icao));
  strlcpy(ap.iata, o["iata"] | "", sizeof(ap.iata));
  strlcpy(ap.name, o["name"] | "", sizeof(ap.name));
  strlcpy(ap.city, o["location"] | "", sizeof(ap.city));
  ap.lat = o["lat"] | 0.0;
  ap.lon = o["lon"] | 0.0;
}

static void fetchRoute(WiFiClientSecure &cli) {
  uint32_t hex;
  char cs[10];
  double lat, lon;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  hex = s_routeHex;
  strlcpy(cs, s_routeCs, sizeof(cs));
  lat = s_routeLat;
  lon = s_routeLon;
  s_routePending = false;
  xSemaphoreGive(g_lock);

  JsonDocument req;
  JsonObject p = req["planes"].add<JsonObject>();
  p["callsign"] = cs;
  p["lat"] = lat;
  p["lng"] = lon;
  String body;
  serializeJson(req, body);

  RouteInfo r;
  memset(&r, 0, sizeof(r));
  r.hex = hex;
  r.state = 4;

  HTTPClient http;
  http.setTimeout(10000);
  if (http.begin(cli, "https://api.adsb.lol/api/0/routeset")) {
    http.setUserAgent(UA);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    if (code == 200) {
      String resp = http.getString();
      JsonDocument doc;
      if (!deserializeJson(doc, resp)) {
        JsonObject o = doc[0];
        strlcpy(r.number, o["number"] | "", sizeof(r.number));
        strlcpy(r.airline, o["airline_code"] | "", sizeof(r.airline));
        r.plausible = o["plausible"] | false;
        JsonArray aps = o["_airports"].as<JsonArray>();
        int n = aps.size();
        if (n >= 2) {
          // Teilstrecke wählen, auf der sich das Flugzeug gerade befindet (Mehrfach-Legs)
          int best = 0;
          double bestScore = 1e18;
          for (int i = 0; i + 1 < n; i++) {
            double la1 = aps[i]["lat"], lo1 = aps[i]["lon"], la2 = aps[i + 1]["lat"], lo2 = aps[i + 1]["lon"];
            double s = haversineKm(la1, lo1, lat, lon) + haversineKm(lat, lon, la2, lo2) - haversineKm(la1, lo1, la2, lo2);
            if (s < bestScore) { bestScore = s; best = i; }
          }
          readAirport(aps[best], r.origin);
          readAirport(aps[best + 1], r.dest);
          r.hasOrigin = r.hasDest = true;
          r.state = 2;
        } else {
          r.state = 3;  // Route unbekannt
        }
      }
    } else {
      Serial.printf("[route] HTTP %d\n", code);
    }
    http.end();
  }

  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (g_route.hex == hex) g_route = r;  // nur übernehmen, wenn noch dasselbe Flugzeug gewählt ist
  xSemaphoreGive(g_lock);
  g_routeVersion = g_routeVersion + 1;
}

// ------------------------------------------------------------------ Kacheln
struct PngCtx { PNG *png; lv_color_t *dst; int w; uint16_t line[TILE_PX]; };

static int pngDraw(PNGDRAW *d) {
  PngCtx *c = (PngCtx *)d->pUser;
  if (c->w == TILE_PX) {
    if (d->y < TILE_PX) c->png->getLineAsRGB565(d, (uint16_t *)(c->dst + d->y * TILE_PX), PNG_RGB565_LITTLE_ENDIAN, 0x000000);
  } else if (c->w == TILE_PX / 2) {
    // 256er-Kachel auf 512 hochskalieren
    c->png->getLineAsRGB565(d, c->line, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
    uint16_t *r0 = (uint16_t *)(c->dst + (d->y * 2) * TILE_PX);
    for (int x = 0; x < TILE_PX / 2; x++) r0[2 * x] = r0[2 * x + 1] = c->line[x];
    memcpy(r0 + TILE_PX, r0, TILE_PX * 2);
  }
  return 1;
}

static void tileTask(void *) {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.setReuse(true);
  http.setTimeout(10000);
  const size_t CAP = 400 * 1024;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
  PNG *png = new PNG();
  PngCtx *ctx = (PngCtx *)heap_caps_malloc(sizeof(PngCtx), MALLOC_CAP_SPIRAM);
  static const char sub[] = "abcd";
  uint8_t subIdx = 0;

  for (;;) {
    if (!g_wifiOk) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    int z, x, y;
    int slot = mapNextTileRequest(&z, &x, &y);
    if (slot < 0) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }

    char url[96];
    snprintf(url, sizeof(url), "https://%c.basemaps.cartocdn.com/dark_all/%d/%d/%d@2x.png", sub[subIdx], z, x, y);
    bool ok = false;
    if (http.begin(cli, url)) {
      http.setUserAgent(UA);
      int code = http.GET();
      if (code == 200) {
        BufStream bs(buf, CAP);
        http.writeToStream(&bs);
        if (!bs.overflow && bs.len > 0 && png->openRAM(buf, bs.len, pngDraw) == PNG_SUCCESS) {
          ctx->png = png;
          ctx->dst = g_tiles[slot].px;
          ctx->w = png->getWidth();
          if (ctx->w == TILE_PX || ctx->w == TILE_PX / 2) ok = png->decode(ctx, 0) == PNG_SUCCESS;
          png->close();
        }
      } else {
        Serial.printf("[tile] %s -> HTTP %d\n", url, code);
        http.end();
        cli.stop();
        subIdx = (subIdx + 1) & 3;
      }
      if (code == 200) http.end();
    }
    mapTileLoaded(slot, ok);
    if (!ok) vTaskDelay(pdMS_TO_TICKS(300));
  }
}

// ------------------------------------------------------------------ WLAN + Daten-Task
static void netTask(void *) {
  WiFiClientSecure cli;
  cli.setInsecure();  // öffentliche, nur lesende Daten
  uint32_t lastTry = 0, lastPoll = 0;
  bool timeSet = false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  for (;;) {
    uint32_t now = millis();
    if (g_reconnect) {
      g_reconnect = false;
      WiFi.disconnect(false, true);
      lastTry = 0;
      vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (WiFi.status() != WL_CONNECTED) {
      g_wifiOk = false;
      if (!g_scanBusy && g_cfg.ssid[0] && (lastTry == 0 || now - lastTry > 20000)) {
        Serial.printf("[wifi] verbinde mit %s\n", g_cfg.ssid);
        WiFi.begin(g_cfg.ssid, g_cfg.pass);
        lastTry = now ? now : 1;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!g_wifiOk) {
      g_wifiOk = true;
      lastPoll = 0;
      Serial.printf("[wifi] verbunden, IP %s\n", WiFi.localIP().toString().c_str());
      if (!timeSet) { configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com"); timeSet = true; }
    }
    if (s_routePending) fetchRoute(cli);
    if (lastPoll == 0 || now - lastPoll >= POLL_INTERVAL_MS || (g_forcePoll && now - lastPoll > 1500)) {
      g_forcePoll = false;
      lastPoll = now;
      fetchAircraft(cli);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

void netStart() {
  g_lock = xSemaphoreCreateMutex();
  g_ac = (Aircraft *)heap_caps_calloc(MAX_AC, sizeof(Aircraft), MALLOC_CAP_SPIRAM);
  memset(&g_route, 0, sizeof(g_route));
  g_queryLat = g_cfg.lat;
  g_queryLon = g_cfg.lon;
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(tileTask, "tiles", 16384, nullptr, 1, nullptr, 0);
}
