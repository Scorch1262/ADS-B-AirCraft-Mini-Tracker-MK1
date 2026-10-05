// Kartenprojektion, Kachel-Cache und Zeichnen der Karte in den Canvas-Puffer
#include "app.h"
#include <math.h>
#include <algorithm>

TileSlot g_tiles[TILE_CACHE];
SemaphoreHandle_t g_tileLock;
volatile int g_viewZoom = 8;
volatile double g_viewWX = 0, g_viewWY = 0;
volatile uint32_t g_selHex = 0;
static volatile int s_busySlot = -1;

// ------------------------------------------------------------------ Farben
static const lv_color_t C_BG       = LV_COLOR_MAKE(0x0d, 0x10, 0x14);   // fehlende Kachel
static const lv_color_t C_GRID     = LV_COLOR_MAKE(0x1a, 0x1f, 0x26);
static const lv_color_t C_HOME     = LV_COLOR_MAKE(0x3d, 0xd6, 0xf5);
static const lv_color_t C_SEL      = LV_COLOR_MAKE(0xff, 0xff, 0xff);
static const lv_color_t C_FLOWN    = LV_COLOR_MAKE(0x8a, 0x9b, 0xb0);
static const lv_color_t C_REMAIN   = LV_COLOR_MAKE(0xff, 0xb0, 0x20);
static const lv_color_t C_AIRPORT  = LV_COLOR_MAKE(0xff, 0xb0, 0x20);
static const lv_color_t C_OUTLINE  = LV_COLOR_MAKE(0x00, 0x00, 0x00);
static const lv_color_t C_LABEL    = LV_COLOR_MAKE(0xd8, 0xde, 0xe6);

// ------------------------------------------------------------------ Projektion (Web-Mercator)
double worldSize(int z) { return (double)TILE_PX * (double)(1UL << z); }

double lonToWX(double lon, int z) { return (lon + 180.0) / 360.0 * worldSize(z); }

double latToWY(double lat, int z) {
  if (lat > 85.05) lat = 85.05;
  if (lat < -85.05) lat = -85.05;
  double r = lat * M_PI / 180.0;
  return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * worldSize(z);
}

double wxToLon(double wx, int z) { return wx / worldSize(z) * 360.0 - 180.0; }

double wyToLat(double wy, int z) {
  double n = M_PI - 2.0 * M_PI * wy / worldSize(z);
  return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

void mapSetCenter(double lat, double lon, int z) {
  if (z < ZOOM_MIN) z = ZOOM_MIN;
  if (z > ZOOM_MAX) z = ZOOM_MAX;
  g_viewZoom = z;
  g_viewWX = lonToWX(lon, z);
  g_viewWY = latToWY(lat, z);
}

void mapGetCenter(double *lat, double *lon) {
  int z = g_viewZoom;
  double W = worldSize(z);
  double wx = fmod(g_viewWX, W);
  if (wx < 0) wx += W;
  *lon = wxToLon(wx, z);
  *lat = wyToLat(g_viewWY, z);
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
  double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
  double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void greatCirclePoint(double lat1, double lon1, double lat2, double lon2, double f, double *lat, double *lon) {
  double p1 = lat1 * M_PI / 180, l1 = lon1 * M_PI / 180;
  double p2 = lat2 * M_PI / 180, l2 = lon2 * M_PI / 180;
  double x1 = cos(p1) * cos(l1), y1 = cos(p1) * sin(l1), z1 = sin(p1);
  double x2 = cos(p2) * cos(l2), y2 = cos(p2) * sin(l2), z2 = sin(p2);
  double dot = x1 * x2 + y1 * y2 + z1 * z2;
  if (dot > 1) dot = 1;
  if (dot < -1) dot = -1;
  double d = acos(dot);
  if (d < 1e-9) { *lat = lat1; *lon = lon1; return; }
  double A = sin((1 - f) * d) / sin(d), B = sin(f * d) / sin(d);
  double x = A * x1 + B * x2, y = A * y1 + B * y2, zz = A * z1 + B * z2;
  *lat = atan2(zz, sqrt(x * x + y * y)) * 180 / M_PI;
  *lon = atan2(y, x) * 180 / M_PI;
}

// Weltkoordinate -> Bildschirm (mit Umbruch an der Datumsgrenze)
static inline double wrapDX(double dx, double W) {
  while (dx > W / 2) dx -= W;
  while (dx < -W / 2) dx += W;
  return dx;
}

static void geoToScreen(double lat, double lon, double *sx, double *sy) {
  int z = g_viewZoom;
  double W = worldSize(z);
  *sx = wrapDX(lonToWX(lon, z) - g_viewWX, W) + SCR_CX;
  *sy = latToWY(lat, z) - g_viewWY + SCR_CY;
}

// ------------------------------------------------------------------ Zeichenprimitive
static lv_color_t *B;  // aktueller Zielpuffer SCR_W x SCR_H

static inline void px(int x, int y, lv_color_t c) {
  if ((unsigned)x < SCR_W && (unsigned)y < SCR_H) B[y * SCR_W + x] = c;
}

static inline lv_color_t mix(lv_color_t a, lv_color_t b, uint8_t f) { return lv_color_mix(a, b, f); }

static void dot(int x, int y, int r, lv_color_t c) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r + r) px(x + dx, y + dy, c);
}

static void ring(int x, int y, int r, int t, lv_color_t c) {
  int ro = (r + t) * (r + t), ri = r * r;
  for (int dy = -r - t; dy <= r + t; dy++)
    for (int dx = -r - t; dx <= r + t; dx++) {
      int d = dx * dx + dy * dy;
      if (d <= ro && d >= ri) px(x + dx, y + dy, c);
    }
}

// Liang-Barsky-Clipping auf den Bildschirm (+Rand)
static bool clipLine(double &x0, double &y0, double &x1, double &y1) {
  const double xmin = -4, ymin = -4, xmax = SCR_W + 4, ymax = SCR_H + 4;
  double dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
  double p[4] = {-dx, dx, -dy, dy};
  double q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0) return false;
    } else {
      double t = q[i] / p[i];
      if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
      else { if (t < t0) return false; if (t < t1) t1 = t; }
    }
  }
  double nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
  x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
  return true;
}

// Linie mit Dicke 1..3 und optionaler Strichelung (dashOn/dashOff in Pixeln, *phase läuft weiter)
static void line(double fx0, double fy0, double fx1, double fy1, lv_color_t c, int thick,
                 int dashOn = 0, int dashOff = 0, int *phase = nullptr) {
  double ox0 = fx0, oy0 = fy0;
  double len0 = hypot(fx1 - fx0, fy1 - fy0);
  if (!clipLine(fx0, fy0, fx1, fy1)) {
    if (phase) *phase += (int)len0;
    return;
  }
  if (phase) *phase += (int)hypot(fx0 - ox0, fy0 - oy0);  // Strichmuster durchgehend halten
  int x0 = lround(fx0), y0 = lround(fy0), x1 = lround(fx1), y1 = lround(fy1);
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int ph = phase ? *phase : 0;
  int period = dashOn + dashOff;
  for (;;) {
    bool on = period == 0 || (ph % period) < dashOn;
    if (on) {
      px(x0, y0, c);
      if (thick >= 2) { px(x0 + 1, y0, c); px(x0, y0 + 1, c); }
      if (thick >= 3) { px(x0 - 1, y0, c); px(x0, y0 - 1, c); }
    }
    ph++;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
  if (phase) *phase = ph;
}

static void fillTri(float x0, float y0, float x1, float y1, float x2, float y2, lv_color_t c) {
  if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
  if (y0 > y2) { std::swap(x0, x2); std::swap(y0, y2); }
  if (y1 > y2) { std::swap(x1, x2); std::swap(y1, y2); }
  int ys = (int)ceilf(y0), ye = (int)floorf(y2);
  for (int y = ys; y <= ye; y++) {
    if ((unsigned)y >= SCR_H) continue;
    float fy = (float)y;
    float xa = (y2 == y0) ? x0 : x0 + (x2 - x0) * (fy - y0) / (y2 - y0);
    float xb;
    if (fy < y1) xb = (y1 == y0) ? x0 : x0 + (x1 - x0) * (fy - y0) / (y1 - y0);
    else xb = (y2 == y1) ? x1 : x1 + (x2 - x1) * (fy - y1) / (y2 - y1);
    if (xa > xb) std::swap(xa, xb);
    int xs = (int)ceilf(xa), xe = (int)floorf(xb);
    if (xs < 0) xs = 0;
    if (xe >= SCR_W) xe = SCR_W - 1;
    lv_color_t *row = B + y * SCR_W;
    for (int x = xs; x <= xe; x++) row[x] = c;
  }
}

// Flugzeugsymbol: Dreiecke in lokalen Koordinaten, Nase zeigt nach -y
static const int8_t PLANE_TRIS[][6] = {
    {0, -12, -2, -7, 2, -7},                                   // Nase
    {-2, -7, 2, -7, 2, 9}, {-2, -7, 2, 9, -2, 9},              // Rumpf
    {-2, -3, -13, 3, -13, 5}, {-2, -3, -13, 5, -2, 2},         // linker Flügel
    {2, -3, 13, 3, 13, 5}, {2, -3, 13, 5, 2, 2},               // rechter Flügel
    {-1, 5, -6, 10, -6, 12}, {-1, 5, -6, 12, -1, 10},          // Leitwerk links
    {1, 5, 6, 10, 6, 12}, {1, 5, 6, 12, 1, 10},                // Leitwerk rechts
};

static void drawPlane(float cx, float cy, float trackDeg, float scale, lv_color_t c) {
  float r = trackDeg * (float)M_PI / 180.0f;
  float cs = cosf(r) * scale, sn = sinf(r) * scale;
  for (auto &t : PLANE_TRIS) {
    float p[6];
    for (int i = 0; i < 3; i++) {
      float x = t[i * 2], y = t[i * 2 + 1];
      p[i * 2] = cx + x * cs - y * sn;
      p[i * 2 + 1] = cy + x * sn + y * cs;
    }
    fillTri(p[0], p[1], p[2], p[3], p[4], p[5], c);
  }
}

// Höhenabhängige Farbe (ähnlich tar1090): niedrig = orange, hoch = violett
static lv_color_t altColor(const Aircraft &a) {
  if (a.ground) return LV_COLOR_MAKE(0x90, 0x90, 0x90);
  if (a.alt == INT32_MIN) return LV_COLOR_MAKE(0xc0, 0xc0, 0xc0);
  float t = a.alt / 40000.0f;
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  uint16_t hue = (uint16_t)(20 + t * 270);
  return lv_color_hsv_to_rgb(hue, 80, 100);
}

// ------------------------------------------------------------------ Text auf Canvas
static lv_obj_t *s_canvas;

static void text(int x, int y, const char *s, lv_color_t c, const lv_font_t *font, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  if (x < -150 || x > SCR_W + 150 || y < -30 || y > SCR_H) return;
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.font = font;
  d.align = align;
  int w = 140;
  int xx = align == LV_TEXT_ALIGN_CENTER ? x - w / 2 : x;
  d.color = C_OUTLINE;
  lv_canvas_draw_text(s_canvas, xx + 1, y + 1, w, &d, s);
  lv_canvas_draw_text(s_canvas, xx - 1, y - 1, w, &d, s);
  d.color = c;
  lv_canvas_draw_text(s_canvas, xx, y, w, &d, s);
}

// ------------------------------------------------------------------ Kacheln
void mapInit() {
  g_tileLock = xSemaphoreCreateMutex();
  for (int i = 0; i < TILE_CACHE; i++) {
    g_tiles[i].state = 0;
    g_tiles[i].z = -1;
    g_tiles[i].px = (lv_color_t *)heap_caps_malloc(TILE_PX * TILE_PX * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  }
}

static int findTile(int z, int x, int y) {
  for (int i = 0; i < TILE_CACHE; i++)
    if (g_tiles[i].z == z && g_tiles[i].x == x && g_tiles[i].y == y && g_tiles[i].state != 0) return i;
  return -1;
}

// Freien Platz suchen bzw. den am längsten nicht genutzten (nicht sichtbaren) Platz überschreiben
static int allocTile(uint32_t frameStamp) {
  int best = -1;
  uint32_t bestUse = UINT32_MAX;
  for (int i = 0; i < TILE_CACHE; i++) {
    if (!g_tiles[i].px || i == s_busySlot) continue;
    if (g_tiles[i].state == 0) return i;
    if (g_tiles[i].lastUse == frameStamp) continue;  // in diesem Bild sichtbar
    if (g_tiles[i].lastUse < bestUse) { bestUse = g_tiles[i].lastUse; best = i; }
  }
  return best;
}

int mapNextTileRequest(int *z, int *x, int *y) {
  int best = -1;
  xSemaphoreTake(g_tileLock, portMAX_DELAY);
  int zoom = g_viewZoom;
  double W = worldSize(zoom);
  double bestD = 1e18;
  uint32_t newest = 0;
  for (int i = 0; i < TILE_CACHE; i++)
    if (g_tiles[i].lastUse > newest) newest = g_tiles[i].lastUse;
  for (int i = 0; i < TILE_CACHE; i++) {
    TileSlot &t = g_tiles[i];
    if (t.state != 1) continue;
    if (t.z != zoom || t.lastUse + 2 < newest) { t.state = 0; t.z = -1; continue; }  // veraltet
    double cx = t.x * (double)TILE_PX + TILE_PX / 2, cy = t.y * (double)TILE_PX + TILE_PX / 2;
    double d = hypot(wrapDX(cx - g_viewWX, W), cy - g_viewWY);
    if (d < bestD) { bestD = d; best = i; }
  }
  if (best >= 0) {
    *z = g_tiles[best].z; *x = g_tiles[best].x; *y = g_tiles[best].y;
    s_busySlot = best;
  }
  xSemaphoreGive(g_tileLock);
  return best;
}

void mapTileLoaded(int slot, bool ok) {
  xSemaphoreTake(g_tileLock, portMAX_DELAY);
  if (slot >= 0 && slot < TILE_CACHE) {
    g_tiles[slot].state = ok ? 2 : 3;
    g_tiles[slot].failMs = millis();
  }
  s_busySlot = -1;
  xSemaphoreGive(g_tileLock);
  g_tileVersion = g_tileVersion + 1;
}

static void drawTiles(uint32_t stamp) {
  int z = g_viewZoom;
  long n = 1L << z;
  double left = g_viewWX - SCR_CX, top = g_viewWY - SCR_CY;
  long tx0 = (long)floor(left / TILE_PX), tx1 = (long)floor((left + SCR_W - 1) / TILE_PX);
  long ty0 = (long)floor(top / TILE_PX), ty1 = (long)floor((top + SCR_H - 1) / TILE_PX);

  xSemaphoreTake(g_tileLock, portMAX_DELAY);
  for (long ty = ty0; ty <= ty1; ty++) {
    for (long tx = tx0; tx <= tx1; tx++) {
      // Bildschirmbereich dieser Kachel
      int sx0 = (int)lround(tx * (double)TILE_PX - left);
      int sy0 = (int)lround(ty * (double)TILE_PX - top);
      int cx0 = sx0 < 0 ? 0 : sx0, cy0 = sy0 < 0 ? 0 : sy0;
      int cx1 = sx0 + TILE_PX > SCR_W ? SCR_W : sx0 + TILE_PX;
      int cy1 = sy0 + TILE_PX > SCR_H ? SCR_H : sy0 + TILE_PX;
      if (cx1 <= cx0 || cy1 <= cy0) continue;

      int slot = -1;
      if (ty >= 0 && ty < n) {
        long wx = ((tx % n) + n) % n;
        slot = findTile(z, wx, ty);
        if (slot < 0) {
          slot = allocTile(stamp);
          if (slot >= 0) {
            g_tiles[slot].z = z; g_tiles[slot].x = wx; g_tiles[slot].y = ty;
            g_tiles[slot].state = 1;
          }
        } else if (g_tiles[slot].state == 3 && millis() - g_tiles[slot].failMs > 15000) {
          g_tiles[slot].state = 1;  // erneut versuchen
        }
        if (slot >= 0) g_tiles[slot].lastUse = stamp;
      }

      if (slot >= 0 && g_tiles[slot].state == 2) {
        lv_color_t *src = g_tiles[slot].px;
        for (int y = cy0; y < cy1; y++)
          memcpy(B + y * SCR_W + cx0, src + (y - sy0) * TILE_PX + (cx0 - sx0), (cx1 - cx0) * sizeof(lv_color_t));
      } else {
        // Platzhalter mit dezentem Raster
        for (int y = cy0; y < cy1; y++) {
          lv_color_t *row = B + y * SCR_W;
          bool gy = ((y - sy0) & 63) == 0;
          for (int x = cx0; x < cx1; x++) row[x] = (gy || ((x - sx0) & 63) == 0) ? C_GRID : C_BG;
        }
      }
    }
  }
  xSemaphoreGive(g_tileLock);
}

// ------------------------------------------------------------------ Flugzeuge
static void extrapolate(const Aircraft &a, uint32_t now, double *lat, double *lon) {
  *lat = a.lat; *lon = a.lon;
  if (a.ground || a.gs < 30) return;
  float dt = (now - a.posMs) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 30) dt = 30;
  double d = a.gs * 0.514444 * dt;  // Meter
  double tr = a.track * M_PI / 180;
  *lat += d * cos(tr) / 111320.0;
  *lon += d * sin(tr) / (111320.0 * cos(a.lat * M_PI / 180));
}

static void drawGreatCircle(double la1, double lo1, double la2, double lo2, lv_color_t c, int thick, int dOn, int dOff) {
  double km = haversineKm(la1, lo1, la2, lo2);
  int n = (int)(km / 40);
  if (n < 2) n = 2;
  if (n > 160) n = 160;
  double W = worldSize(g_viewZoom);
  double px0, py0;
  geoToScreen(la1, lo1, &px0, &py0);
  int phase = 0;
  for (int i = 1; i <= n; i++) {
    double la, lo, x, y;
    greatCirclePoint(la1, lo1, la2, lo2, (double)i / n, &la, &lo);
    geoToScreen(la, lo, &x, &y);
    x = px0 + wrapDX(x - px0, W);  // keine Sprünge an der Datumsgrenze
    line(px0, py0, x, y, c, thick, dOn, dOff, dOn ? &phase : nullptr);
    px0 = x; py0 = y;
  }
}

static void drawAirport(const Airport &ap) {
  double x, y;
  geoToScreen(ap.lat, ap.lon, &x, &y);
  if (x < -40 || x > SCR_W + 40 || y < -40 || y > SCR_H + 40) return;
  ring((int)x, (int)y, 6, 2, C_AIRPORT);
  dot((int)x, (int)y, 2, C_AIRPORT);
  text((int)x, (int)y + 10, ap.iata[0] ? ap.iata : ap.icao, C_AIRPORT, &font_de_14, LV_TEXT_ALIGN_CENTER);
}

void mapRender(lv_obj_t *canvas, lv_color_t *buf) {
  static uint32_t stamp = 1;
  stamp++;
  B = buf;
  s_canvas = canvas;
  uint32_t now = millis();
  int z = g_viewZoom;
  double W = worldSize(z);

  drawTiles(stamp);

  xSemaphoreTake(g_lock, portMAX_DELAY);

  // Gesamte Flugbahn des ausgewählten Flugzeugs (Start -> Flugzeug -> Ziel)
  const Aircraft *sel = nullptr;
  if (g_selHex) {
    for (int i = 0; i < MAX_AC; i++)
      if (g_ac[i].used && g_ac[i].hex == g_selHex) { sel = &g_ac[i]; break; }
  }
  if (sel && g_route.hex == sel->hex && g_route.state == 2) {
    double la, lo;
    extrapolate(*sel, now, &la, &lo);
    if (g_route.hasOrigin) drawGreatCircle(g_route.origin.lat, g_route.origin.lon, la, lo, C_FLOWN, 2, 0, 0);
    if (g_route.hasDest) drawGreatCircle(la, lo, g_route.dest.lat, g_route.dest.lon, C_REMAIN, 2, 9, 6);
    if (g_route.hasOrigin) drawAirport(g_route.origin);
    if (g_route.hasDest) drawAirport(g_route.dest);
  }

  // Gestrichelte Spur jedes Flugzeugs
  for (int i = 0; i < MAX_AC; i++) {
    const Aircraft &a = g_ac[i];
    if (!a.used || a.trailN == 0) continue;
    lv_color_t c = mix(altColor(a), C_BG, 170);
    bool isSel = &a == sel;
    int phase = 0;
    double px0 = 0, py0 = 0;
    for (int k = 0; k <= a.trailN; k++) {
      double la, lo, x, y;
      if (k < a.trailN) {
        int idx = (a.trailHead + TRAIL_LEN - a.trailN + k) % TRAIL_LEN;
        la = a.trailLat[idx]; lo = a.trailLon[idx];
      } else {
        extrapolate(a, now, &la, &lo);
      }
      geoToScreen(la, lo, &x, &y);
      if (k > 0) {
        x = px0 + wrapDX(x - px0, W);
        line(px0, py0, x, y, isSel ? C_SEL : c, 2, 5, 4, &phase);
      }
      px0 = x; py0 = y;
    }
  }

  // Flugzeuge
  int visible = 0;
  for (int i = 0; i < MAX_AC; i++)
    if (g_ac[i].used) visible++;
  bool labels = z >= 7 || visible <= 40;
  for (int i = 0; i < MAX_AC; i++) {
    const Aircraft &a = g_ac[i];
    if (!a.used) continue;
    double la, lo, x, y;
    extrapolate(a, now, &la, &lo);
    geoToScreen(la, lo, &x, &y);
    if (x < -30 || x > SCR_W + 30 || y < -30 || y > SCR_H + 30) continue;
    bool isSel = &a == sel;
    float sc = isSel ? 1.25f : 0.95f;
    drawPlane(x, y, a.track, sc * 1.3f, C_OUTLINE);
    drawPlane(x, y, a.track, sc, isSel ? C_SEL : altColor(a));
    if (isSel) ring((int)x, (int)y, 20, 2, C_REMAIN);
    if ((labels || isSel) && a.flight[0])
      text((int)x + 14, (int)y + 6, a.flight, isSel ? C_SEL : C_LABEL, &font_de_14);
  }
  xSemaphoreGive(g_lock);

  // Eigener Standort
  double hx, hy;
  geoToScreen(g_cfg.lat, g_cfg.lon, &hx, &hy);
  ring((int)hx, (int)hy, 7, 2, C_HOME);
  dot((int)hx, (int)hy, 2, C_HOME);
}

uint32_t mapHitTest(int sx, int sy) {
  uint32_t best = 0;
  double bestD = 30 * 30;
  uint32_t now = millis();
  xSemaphoreTake(g_lock, portMAX_DELAY);
  for (int i = 0; i < MAX_AC; i++) {
    const Aircraft &a = g_ac[i];
    if (!a.used) continue;
    double la, lo, x, y;
    extrapolate(a, now, &la, &lo);
    geoToScreen(la, lo, &x, &y);
    double d = (x - sx) * (x - sx) + (y - sy) * (y - sy);
    if (d < bestD) { bestD = d; best = a.hex; }
  }
  xSemaphoreGive(g_lock);
  return best;
}
