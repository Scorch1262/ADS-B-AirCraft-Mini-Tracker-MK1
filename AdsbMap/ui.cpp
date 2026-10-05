// Bedienoberfläche: Kartenansicht, Infofenster, Einstellungsmenü
#include "app.h"
#include <WiFi.h>
#include <time.h>
#include <math.h>

extern volatile bool g_forcePoll;
void displaySetBrightness(uint8_t v);

// ------------------------------------------------------------------ Farben
#define COL_PANEL   lv_color_hex(0x12161c)
#define COL_PANEL2  lv_color_hex(0x1b212a)
#define COL_ACCENT  lv_color_hex(0x3dd6f5)
#define COL_AMBER   lv_color_hex(0xffb020)
#define COL_TEXT    lv_color_hex(0xe6ebf1)
#define COL_MUTED   lv_color_hex(0x8a96a6)
#define COL_RED     lv_color_hex(0xff5a5a)

// ------------------------------------------------------------------ Objekte
static lv_obj_t *scrMap, *scrSet;
static lv_obj_t *canvas;
static lv_color_t *canvasBuf;
static lv_obj_t *lblStatus;
static lv_obj_t *panel, *lblCall, *lblReg, *lblRoute, *lblCities, *lblType, *lblEta, *lblAlt;

static bool dirty = true;
static uint32_t lastRender = 0;
static uint32_t seenData = 0, seenTile = 0, seenRoute = 0;
static bool dragging = false, pressed = false;
static int32_t dragDist = 0;

// Ansicht vor dem Antippen (wird beim Schließen wiederhergestellt)
static bool hasPrevView = false;
static double prevLat, prevLon;
static int prevZoom;
static uint32_t fittedFor = 0;

// ------------------------------------------------------------------ Hilfen
static void styleRoundBtn(lv_obj_t *b) {
  lv_obj_set_size(b, 54, 54);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(0x2c3642), 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, COL_PANEL2, LV_STATE_PRESSED);
}

static lv_obj_t *mkRoundBtn(lv_obj_t *parent, const char *sym, int x, int y, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  styleRoundBtn(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, sym);
  lv_obj_set_style_text_font(l, &font_de_18, 0);
  lv_obj_set_style_text_color(l, COL_TEXT, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static lv_obj_t *mkLabel(lv_obj_t *parent, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, "");
  return l;
}

// Abfragebereich für die ADS-B-Daten an die aktuelle Ansicht anpassen
static void updateQuery() {
  double lat, lon;
  mapGetCenter(&lat, &lon);
  double mpp = 156543.03392 * cos(lat * M_PI / 180.0) / (double)(1UL << g_viewZoom) * (256.0 / TILE_PX);
  double nm = 330.0 * mpp / 1852.0 * 1.15;
  int r = (int)ceil(nm);
  if (r < 10) r = 10;
  if (r > 250) r = 250;
  g_queryLat = lat;
  g_queryLon = lon;
  g_queryRadiusNm = r;
}

static void setZoom(int z) {
  if (z < ZOOM_MIN) z = ZOOM_MIN;
  if (z > ZOOM_MAX) z = ZOOM_MAX;
  double lat, lon;
  mapGetCenter(&lat, &lon);
  mapSetCenter(lat, lon, z);
  updateQuery();
  g_forcePoll = true;
  dirty = true;
}

// ------------------------------------------------------------------ Flugzeugtypen
struct TypeEntry { const char *code, *name; };
static const TypeEntry TYPES[] = {
  {"A318", "Airbus A318"}, {"A319", "Airbus A319"}, {"A320", "Airbus A320"}, {"A321", "Airbus A321"},
  {"A19N", "Airbus A319neo"}, {"A20N", "Airbus A320neo"}, {"A21N", "Airbus A321neo"},
  {"BCS1", "Airbus A220-100"}, {"BCS3", "Airbus A220-300"}, {"A306", "Airbus A300-600"}, {"A310", "Airbus A310"},
  {"A332", "Airbus A330-200"}, {"A333", "Airbus A330-300"}, {"A338", "Airbus A330-800neo"}, {"A339", "Airbus A330-900neo"},
  {"A342", "Airbus A340-200"}, {"A343", "Airbus A340-300"}, {"A345", "Airbus A340-500"}, {"A346", "Airbus A340-600"},
  {"A359", "Airbus A350-900"}, {"A35K", "Airbus A350-1000"}, {"A388", "Airbus A380-800"},
  {"A3ST", "Airbus Beluga"}, {"A337", "Airbus BelugaXL"}, {"A400", "Airbus A400M"},
  {"B712", "Boeing 717"}, {"B733", "Boeing 737-300"}, {"B734", "Boeing 737-400"}, {"B735", "Boeing 737-500"},
  {"B736", "Boeing 737-600"}, {"B737", "Boeing 737-700"}, {"B738", "Boeing 737-800"}, {"B739", "Boeing 737-900"},
  {"B37M", "Boeing 737 MAX 7"}, {"B38M", "Boeing 737 MAX 8"}, {"B39M", "Boeing 737 MAX 9"}, {"B3XM", "Boeing 737 MAX 10"},
  {"B744", "Boeing 747-400"}, {"B748", "Boeing 747-8"}, {"B752", "Boeing 757-200"}, {"B753", "Boeing 757-300"},
  {"B762", "Boeing 767-200"}, {"B763", "Boeing 767-300"}, {"B764", "Boeing 767-400"},
  {"B772", "Boeing 777-200"}, {"B77L", "Boeing 777-200LR/F"}, {"B773", "Boeing 777-300"}, {"B77W", "Boeing 777-300ER"},
  {"B778", "Boeing 777-8"}, {"B779", "Boeing 777-9"}, {"B788", "Boeing 787-8"}, {"B789", "Boeing 787-9"}, {"B78X", "Boeing 787-10"},
  {"MD11", "McDonnell Douglas MD-11"}, {"E135", "Embraer ERJ-135"}, {"E145", "Embraer ERJ-145"},
  {"E170", "Embraer 170"}, {"E75L", "Embraer 175"}, {"E75S", "Embraer 175"}, {"E190", "Embraer 190"},
  {"E195", "Embraer 195"}, {"E290", "Embraer 190-E2"}, {"E295", "Embraer 195-E2"},
  {"CRJ2", "Bombardier CRJ200"}, {"CRJ7", "Bombardier CRJ700"}, {"CRJ9", "Bombardier CRJ900"}, {"CRJX", "Bombardier CRJ1000"},
  {"AT43", "ATR 42-300"}, {"AT45", "ATR 42-500"}, {"AT46", "ATR 42-600"}, {"AT72", "ATR 72"}, {"AT75", "ATR 72-500"}, {"AT76", "ATR 72-600"},
  {"DH8A", "Dash 8-100"}, {"DH8C", "Dash 8-300"}, {"DH8D", "Dash 8-400"}, {"SF34", "Saab 340"}, {"D328", "Dornier 328"},
  {"C25A", "Cessna Citation CJ2"}, {"C25B", "Cessna Citation CJ3"}, {"C25C", "Cessna Citation CJ4"}, {"C56X", "Cessna Citation Excel"},
  {"C68A", "Cessna Citation Latitude"}, {"C700", "Cessna Citation Longitude"}, {"CL35", "Bombardier Challenger 350"},
  {"CL60", "Bombardier Challenger 600"}, {"GLEX", "Bombardier Global Express"}, {"GL7T", "Bombardier Global 7500"},
  {"GLF5", "Gulfstream G550"}, {"GLF6", "Gulfstream G650"}, {"F900", "Dassault Falcon 900"}, {"FA7X", "Dassault Falcon 7X"},
  {"FA8X", "Dassault Falcon 8X"}, {"LJ45", "Learjet 45"}, {"PC12", "Pilatus PC-12"}, {"PC24", "Pilatus PC-24"},
  {"BE20", "Beechcraft King Air 200"}, {"B350", "Beechcraft King Air 350"}, {"C172", "Cessna 172"}, {"C182", "Cessna 182"},
  {"P28A", "Piper PA-28"}, {"DA40", "Diamond DA40"}, {"DA42", "Diamond DA42"}, {"SR22", "Cirrus SR22"},
  {"EC35", "Airbus H135"}, {"EC45", "Airbus H145"}, {"EC30", "Airbus H130"}, {"EC55", "Airbus H155"}, {"H160", "Airbus H160"},
  {"C130", "Lockheed C-130 Hercules"}, {"C30J", "Lockheed C-130J"}, {"A124", "Antonov An-124"}, {"IL76", "Iljuschin Il-76"},
  {"C17", "Boeing C-17 Globemaster"}, {"K35R", "Boeing KC-135"}, {"EUFI", "Eurofighter Typhoon"},
};

static const char *typeName(const char *code) {
  for (const auto &t : TYPES)
    if (!strcmp(t.code, code)) return t.name;
  return nullptr;
}

// ------------------------------------------------------------------ Infofenster
static const Aircraft *findAc(uint32_t hex) {
  for (int i = 0; i < MAX_AC; i++)
    if (g_ac[i].used && g_ac[i].hex == hex) return &g_ac[i];
  return nullptr;
}

static void closeSelection() {
  g_selHex = 0;
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
  if (hasPrevView) {
    mapSetCenter(prevLat, prevLon, prevZoom);
    hasPrevView = false;
    updateQuery();
    g_forcePoll = true;
  }
  fittedFor = 0;
  dirty = true;
}

static void updateInfo() {
  if (!g_selHex) return;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const Aircraft *a = findAc(g_selHex);
  if (!a) {
    xSemaphoreGive(g_lock);
    closeSelection();
    return;
  }
  Aircraft ac = *a;
  RouteInfo r = g_route;
  xSemaphoreGive(g_lock);

  lv_label_set_text(lblCall, ac.flight[0] ? ac.flight : "ohne Rufz.");
  if (ac.reg[0]) lv_label_set_text_fmt(lblReg, "%s\n%06lX", ac.reg, (unsigned long)ac.hex);
  else lv_label_set_text_fmt(lblReg, "\n%06lX", (unsigned long)ac.hex);

  // Start und Ziel
  if (r.hex == ac.hex && r.state == 2) {
    lv_label_set_text_fmt(lblRoute, "%s  \xE2\x86\x92  %s%s",
                          r.origin.iata[0] ? r.origin.iata : r.origin.icao,
                          r.dest.iata[0] ? r.dest.iata : r.dest.icao,
                          r.plausible ? "" : "  (?)");
    lv_label_set_text_fmt(lblCities, "%s \xE2\x86\x92 %s",
                          r.origin.city[0] ? r.origin.city : r.origin.name,
                          r.dest.city[0] ? r.dest.city : r.dest.name);
  } else if (r.hex == ac.hex && r.state == 1) {
    lv_label_set_text(lblRoute, "Route wird geladen\xE2\x80\xA6");
    lv_label_set_text(lblCities, "");
  } else if (r.hex == ac.hex && r.state == 4) {
    lv_label_set_text(lblRoute, "Route: Abfrage fehlgeschlagen");
    lv_label_set_text(lblCities, "");
  } else {
    lv_label_set_text(lblRoute, "Start/Ziel unbekannt");
    lv_label_set_text(lblCities, "");
  }

  // Flugzeugtyp (ICAO-Typcode -> Klartext, falls die Quelle keine Beschreibung liefert)
  if (!ac.desc[0] && ac.type[0]) {
    const char *n = typeName(ac.type);
    if (n) strlcpy(ac.desc, n, sizeof(ac.desc));
  }
  if (ac.desc[0] && ac.type[0]) lv_label_set_text_fmt(lblType, "Typ: %s (%s)", ac.desc, ac.type);
  else if (ac.desc[0] || ac.type[0]) lv_label_set_text_fmt(lblType, "Typ: %s", ac.desc[0] ? ac.desc : ac.type);
  else lv_label_set_text(lblType, "Typ: unbekannt");

  // Zeit bis zur Landung (Luftlinie Restdistanz / Geschwindigkeit über Grund)
  if (r.hex == ac.hex && r.state == 2 && r.hasDest) {
    double km = haversineKm(ac.lat, ac.lon, r.dest.lat, r.dest.lon);
    if (ac.ground) {
      lv_label_set_text_fmt(lblEta, "Am Boden \xC2\xB7 %.0f km bis %s", km, r.dest.iata[0] ? r.dest.iata : r.dest.icao);
    } else if (ac.gs > 60) {
      int mins = (int)lround(km / (ac.gs * 1.852) * 60.0);
      char eta[16] = "";
      time_t now = time(nullptr);
      if (now > 1700000000) {
        time_t t = now + mins * 60;
        struct tm tmv;
        localtime_r(&t, &tmv);
        snprintf(eta, sizeof(eta), " (%02d:%02d)", tmv.tm_hour, tmv.tm_min);
      }
      if (mins >= 60) lv_label_set_text_fmt(lblEta, "Landung in %d h %02d min%s", mins / 60, mins % 60, eta);
      else lv_label_set_text_fmt(lblEta, "Landung in %d min%s", mins, eta);
    } else {
      lv_label_set_text_fmt(lblEta, "Noch %.0f km", km);
    }
  } else {
    lv_label_set_text(lblEta, "Landung: \xE2\x80\x93");
  }

  // Höhe und Geschwindigkeit
  char alt[24];
  if (ac.ground) strcpy(alt, "am Boden");
  else if (ac.alt == INT32_MIN) strcpy(alt, "H\xC3\xB6he ?");
  else snprintf(alt, sizeof(alt), "%d m (FL%03d)", (int)lround(ac.alt * 0.3048), ac.alt / 100);
  lv_label_set_text_fmt(lblAlt, "%s \xC2\xB7 %d km/h", alt, (int)lround(ac.gs * 1.852));
}

// Ansicht so wählen, dass Start, Ziel und Flugzeug in den runden Bildschirm passen
static void fitRoute() {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const Aircraft *a = findAc(g_selHex);
  RouteInfo r = g_route;
  double alat = a ? a->lat : 0, alon = a ? a->lon : 0;
  xSemaphoreGive(g_lock);
  if (!a || r.hex != g_selHex || r.state != 2) return;

  // Punkte entlang der Großkreise sammeln (Bogen kann über die Endpunkte hinausragen)
  double lats[40], lons[40];
  int n = 0;
  for (int i = 0; i <= 19; i++) {
    double la, lo;
    greatCirclePoint(r.origin.lat, r.origin.lon, alat, alon, i / 19.0, &la, &lo);
    lats[n] = la; lons[n++] = lo;
    greatCirclePoint(alat, alon, r.dest.lat, r.dest.lon, i / 19.0, &la, &lo);
    lats[n] = la; lons[n++] = lo;
  }
  // Längen relativ zum Flugzeug entfalten (Datumsgrenze)
  for (int i = 0; i < n; i++) {
    while (lons[i] - alon > 180) lons[i] -= 360;
    while (lons[i] - alon < -180) lons[i] += 360;
  }
  for (int z = ZOOM_MAX; z >= ZOOM_MIN; z--) {
    double x0 = 1e18, x1 = -1e18, y0 = 1e18, y1 = -1e18;
    for (int i = 0; i < n; i++) {
      double x = (lons[i] + 180.0) / 360.0 * worldSize(z), y = latToWY(lats[i], z);
      x0 = fmin(x0, x); x1 = fmax(x1, x); y0 = fmin(y0, y); y1 = fmax(y1, y);
    }
    // Platz oben frei lassen, unten liegt das Infofenster
    if (((x1 - x0) <= 280 && (y1 - y0) <= 170) || z == ZOOM_MIN) {
      g_viewZoom = z;
      g_viewWX = (x0 + x1) / 2;
      g_viewWY = (y0 + y1) / 2 + 85;  // Route in die obere Bildhälfte schieben
      break;
    }
  }
  updateQuery();
  g_forcePoll = true;
  dirty = true;
}

static void selectAircraft(uint32_t hex) {
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const Aircraft *a = findAc(hex);
  char cs[10] = "";
  double lat = 0, lon = 0;
  if (a) { strlcpy(cs, a->flight, sizeof(cs)); lat = a->lat; lon = a->lon; }
  xSemaphoreGive(g_lock);
  if (!a) return;

  if (!g_selHex) {
    mapGetCenter(&prevLat, &prevLon);
    prevZoom = g_viewZoom;
    hasPrevView = true;
  }
  g_selHex = hex;
  fittedFor = 0;
  netRequestRoute(hex, cs, lat, lon);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_HIDDEN);
  updateInfo();
  dirty = true;
}

// ------------------------------------------------------------------ Karten-Ereignisse
static void canvasEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;
  if (code == LV_EVENT_PRESSED) {
    pressed = true;
    dragging = false;
    dragDist = 0;
  } else if (code == LV_EVENT_PRESSING) {
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    dragDist += abs(v.x) + abs(v.y);
    if (dragDist > 10) dragging = true;
    if (dragging && (v.x || v.y)) {
      g_viewWX = g_viewWX - v.x;
      double wy = g_viewWY - v.y;
      double W = worldSize(g_viewZoom);
      if (wy < 0) wy = 0;
      if (wy > W) wy = W;
      g_viewWY = wy;
      double ww = fmod(g_viewWX, W);
      if (ww < 0) ww += W;
      g_viewWX = ww;
      dirty = true;
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    pressed = false;
    if (dragging) {
      dragging = false;
      hasPrevView = false;  // Nutzer hat selbst verschoben -> nicht zurückspringen
      updateQuery();
      g_forcePoll = true;
    } else if (code == LV_EVENT_RELEASED) {
      lv_point_t p;
      lv_indev_get_point(indev, &p);
      uint32_t hex = mapHitTest(p.x, p.y);
      if (hex) selectAircraft(hex);
      else if (g_selHex) closeSelection();
    }
  }
}

static void zoomInCb(lv_event_t *) { hasPrevView = false; setZoom(g_viewZoom + 1); }
static void zoomOutCb(lv_event_t *) { hasPrevView = false; setZoom(g_viewZoom - 1); }
static void homeCb(lv_event_t *) {
  hasPrevView = false;
  mapSetCenter(g_cfg.lat, g_cfg.lon, g_viewZoom);
  updateQuery();
  g_forcePoll = true;
  dirty = true;
}
static void panelCloseCb(lv_event_t *) { closeSelection(); }
static void openSettings(lv_event_t *);

// ------------------------------------------------------------------ Kartenbildschirm
static void buildMap() {
  scrMap = lv_obj_create(nullptr);
  lv_obj_clear_flag(scrMap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scrMap, lv_color_black(), 0);

  canvasBuf = (lv_color_t *)heap_caps_malloc(SCR_W * SCR_H * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  canvas = lv_canvas_create(scrMap);
  lv_canvas_set_buffer(canvas, canvasBuf, SCR_W, SCR_H, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvas, 0, 0);
  lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
  lv_obj_add_event_cb(canvas, canvasEvent, LV_EVENT_ALL, nullptr);

  // Statuszeile oben
  lblStatus = mkLabel(scrMap, &font_de_14, COL_TEXT);
  lv_obj_set_style_bg_color(lblStatus, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(lblStatus, LV_OPA_70, 0);
  lv_obj_set_style_radius(lblStatus, 12, 0);
  lv_obj_set_style_pad_hor(lblStatus, 10, 0);
  lv_obj_set_style_pad_ver(lblStatus, 4, 0);
  lv_obj_align(lblStatus, LV_ALIGN_TOP_MID, 0, 22);

  // Bedienknöpfe am Rand des runden Displays
  mkRoundBtn(scrMap, SYM_GEAR, 16, 168, openSettings);
  mkRoundBtn(scrMap, SYM_HOME, 16, 244, homeCb);
  mkRoundBtn(scrMap, SYM_PLUS, SCR_W - 16 - 54, 168, zoomInCb);
  mkRoundBtn(scrMap, SYM_MINUS, SCR_W - 16 - 54, 244, zoomOutCb);

  // Quellenangabe (Lizenzpflicht der Kartendaten)
  lv_obj_t *attr = mkLabel(scrMap, &font_de_14, COL_MUTED);
  lv_label_set_text(attr, "\xC2\xA9 OpenStreetMap \xC2\xA9 CARTO");
  lv_obj_align(attr, LV_ALIGN_BOTTOM_MID, 0, -14);

  // Infofenster
  panel = lv_obj_create(scrMap);
  lv_obj_set_size(panel, 304, 178);
  lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
  lv_obj_set_style_border_color(panel, COL_AMBER, 0);
  lv_obj_set_style_border_width(panel, 2, 0);
  lv_obj_set_style_radius(panel, 26, 0);
  lv_obj_set_style_pad_all(panel, 12, 0);
  lv_obj_set_style_pad_left(panel, 18, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(panel, 1, 0);

  lv_obj_t *head = lv_obj_create(panel);
  lv_obj_remove_style_all(head);
  lv_obj_set_size(head, LV_PCT(100), 34);
  lblCall = mkLabel(head, &font_de_26, COL_AMBER);
  lv_obj_align(lblCall, LV_ALIGN_LEFT_MID, 0, 0);
  lblReg = mkLabel(head, &font_de_14, COL_MUTED);
  lv_obj_set_style_text_line_space(lblReg, -2, 0);
  lv_obj_align(lblReg, LV_ALIGN_RIGHT_MID, -40, 0);
  lv_obj_t *x = lv_btn_create(head);
  lv_obj_set_size(x, 34, 34);
  lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(x, COL_PANEL2, 0);
  lv_obj_set_style_shadow_width(x, 0, 0);
  lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_add_event_cb(x, panelCloseCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *xl = mkLabel(x, &font_de_14, COL_TEXT);
  lv_label_set_text(xl, SYM_CLOSE);
  lv_obj_center(xl);

  lblRoute = mkLabel(panel, &font_de_18, COL_TEXT);
  lblCities = mkLabel(panel, &font_de_14, COL_MUTED);
  lblType = mkLabel(panel, &font_de_14, COL_TEXT);
  lblEta = mkLabel(panel, &font_de_14, COL_ACCENT);
  lblAlt = mkLabel(panel, &font_de_14, COL_MUTED);
  for (lv_obj_t *l : {lblRoute, lblCities, lblType, lblEta, lblAlt}) {
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

static void updateStatus() {
  static char last[64];
  char s[64];
  if (!g_cfg.ssid[0]) snprintf(s, sizeof(s), SYM_WARN " WLAN nicht eingerichtet");
  else if (!g_wifiOk) snprintf(s, sizeof(s), SYM_WIFI " verbinde\xE2\x80\xA6");
  else if (!g_apiOk) snprintf(s, sizeof(s), SYM_WARN " keine ADS-B-Daten");
  else snprintf(s, sizeof(s), "%d Flugzeuge \xC2\xB7 Z%d", (int)g_acCount, (int)g_viewZoom);
  if (strcmp(s, last)) {
    strcpy(last, s);
    lv_label_set_text(lblStatus, s);
  }
}

// ------------------------------------------------------------------ Einstellungen
static lv_obj_t *taSsid, *taPass, *taLat, *taLon, *slBright, *lblInfo;
static lv_obj_t *editOverlay, *editTa, *editKb, *editTitle;
static lv_obj_t *editTarget;
static lv_obj_t *scanOverlay, *scanList, *scanStatus;
static lv_timer_t *scanTimer;

static void editClose(bool apply) {
  if (apply && editTarget) lv_textarea_set_text(editTarget, lv_textarea_get_text(editTa));
  lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
  editTarget = nullptr;
}

static void editKbEvent(lv_event_t *e) {
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_READY) editClose(true);
  else if (c == LV_EVENT_CANCEL) editClose(false);
}

static void editOpen(lv_obj_t *ta) {
  editTarget = ta;
  const char *title = "";
  bool num = false;
  if (ta == taSsid) title = "WLAN-Name (SSID)";
  else if (ta == taPass) title = "WLAN-Passwort";
  else if (ta == taLat) { title = "Breitengrad (z. B. 52.5200)"; num = true; }
  else if (ta == taLon) { title = "L\xC3\xA4ngengrad (z. B. 13.4050)"; num = true; }
  lv_label_set_text(editTitle, title);
  lv_textarea_set_text(editTa, lv_textarea_get_text(ta));
  lv_textarea_set_password_mode(editTa, false);  // beim Tippen sichtbar
  lv_textarea_set_accepted_chars(editTa, num ? "0123456789.-" : nullptr);
  lv_keyboard_set_mode(editKb, num ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_obj_clear_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(editOverlay);
}

static void taClicked(lv_event_t *e) { editOpen(lv_event_get_target(e)); }

static void buildEditor() {
  editOverlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(editOverlay);
  lv_obj_set_size(editOverlay, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(editOverlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(editOverlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(editOverlay, LV_OBJ_FLAG_SCROLLABLE);

  editTitle = mkLabel(editOverlay, &font_de_14, COL_MUTED);
  lv_obj_align(editTitle, LV_ALIGN_TOP_MID, 0, 62);

  editTa = lv_textarea_create(editOverlay);
  lv_textarea_set_one_line(editTa, true);
  lv_textarea_set_max_length(editTa, 64);
  lv_obj_set_width(editTa, 330);
  lv_obj_set_style_text_font(editTa, &font_de_18, 0);
  lv_obj_align(editTa, LV_ALIGN_TOP_MID, 0, 88);
  lv_obj_add_state(editTa, LV_STATE_FOCUSED);

  editKb = lv_keyboard_create(editOverlay);
  lv_obj_set_size(editKb, 420, 210);
  lv_obj_align(editKb, LV_ALIGN_TOP_MID, 0, 145);
  lv_obj_set_style_text_font(editKb, &font_de_18, 0);
  lv_obj_set_style_bg_opa(editKb, LV_OPA_TRANSP, 0);
  lv_keyboard_set_textarea(editKb, editTa);
  lv_obj_add_event_cb(editKb, editKbEvent, LV_EVENT_ALL, nullptr);

  lv_obj_t *hint = mkLabel(editOverlay, &font_de_14, COL_MUTED);
  lv_label_set_text(hint, LV_SYMBOL_OK " \xC3\xBC" "bernehmen   " LV_SYMBOL_KEYBOARD " abbrechen");
  lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 368);
  lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
}

// --- WLAN-Suche
static void scanClose() {
  if (scanTimer) { lv_timer_del(scanTimer); scanTimer = nullptr; }
  WiFi.scanDelete();
  g_scanBusy = false;
  lv_obj_add_flag(scanOverlay, LV_OBJ_FLAG_HIDDEN);
}

static void scanPick(lv_event_t *e) {
  lv_obj_t *btn = lv_event_get_target(e);
  const char *ssid = lv_list_get_btn_text(scanList, btn);
  lv_textarea_set_text(taSsid, ssid);
  scanClose();
  editOpen(taPass);
}

static void scanCloseCb(lv_event_t *) { scanClose(); }

static void scanPoll(lv_timer_t *) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n < 0) {
    lv_label_set_text(scanStatus, "Suche fehlgeschlagen");
    lv_timer_del(scanTimer);
    scanTimer = nullptr;
    g_scanBusy = false;
    return;
  }
  lv_timer_del(scanTimer);
  scanTimer = nullptr;
  lv_label_set_text_fmt(scanStatus, "%d Netze gefunden", n);
  lv_obj_clean(scanList);
  for (int i = 0; i < n && i < 20; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j = 0; j < i; j++)
      if (WiFi.SSID(j) == s) { dup = true; break; }
    if (dup) continue;
    lv_obj_t *b = lv_list_add_btn(scanList, SYM_WIFI, s.c_str());
    lv_obj_set_style_text_font(b, &font_de_18, 0);
    lv_obj_add_event_cb(b, scanPick, LV_EVENT_CLICKED, nullptr);
  }
  g_scanBusy = false;
}

static void scanStart(lv_event_t *) {
  g_scanBusy = true;
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect(false, false);
  lv_obj_clean(scanList);
  lv_label_set_text(scanStatus, "Suche WLAN-Netze\xE2\x80\xA6");
  lv_obj_clear_flag(scanOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(scanOverlay);
  WiFi.scanNetworks(true);
  scanTimer = lv_timer_create(scanPoll, 300, nullptr);
}

static void buildScan() {
  scanOverlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(scanOverlay);
  lv_obj_set_size(scanOverlay, SCR_W, SCR_H);
  lv_obj_set_style_bg_color(scanOverlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(scanOverlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(scanOverlay, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *t = mkLabel(scanOverlay, &font_de_18, COL_TEXT);
  lv_label_set_text(t, "WLAN ausw\xC3\xA4hlen");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
  scanStatus = mkLabel(scanOverlay, &font_de_14, COL_MUTED);
  lv_obj_align(scanStatus, LV_ALIGN_TOP_MID, 0, 70);

  scanList = lv_list_create(scanOverlay);
  lv_obj_set_size(scanList, 340, 270);
  lv_obj_align(scanList, LV_ALIGN_TOP_MID, 0, 98);
  lv_obj_set_style_bg_color(scanList, COL_PANEL, 0);

  lv_obj_t *b = lv_btn_create(scanOverlay);
  lv_obj_set_size(b, 150, 46);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 380);
  lv_obj_set_style_bg_color(b, COL_PANEL2, 0);
  lv_obj_add_event_cb(b, scanCloseCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *l = mkLabel(b, &font_de_18, COL_TEXT);
  lv_label_set_text(l, "Abbrechen");
  lv_obj_center(l);
  lv_obj_add_flag(scanOverlay, LV_OBJ_FLAG_HIDDEN);
}

// --- Einstellungen öffnen / speichern
static void fillSettings() {
  lv_textarea_set_text(taSsid, g_cfg.ssid);
  lv_textarea_set_text(taPass, g_cfg.pass);
  char b[24];
  snprintf(b, sizeof(b), "%.5f", g_cfg.lat);
  lv_textarea_set_text(taLat, b);
  snprintf(b, sizeof(b), "%.5f", g_cfg.lon);
  lv_textarea_set_text(taLon, b);
  lv_slider_set_value(slBright, g_cfg.brightness, LV_ANIM_OFF);
  if (g_wifiOk)
    lv_label_set_text_fmt(lblInfo, "Version " APP_VERSION "\nIP %s", WiFi.localIP().toString().c_str());
  else
    lv_label_set_text(lblInfo, "Version " APP_VERSION "\nnicht verbunden");
}

static void openSettings(lv_event_t *) {
  fillSettings();
  lv_scr_load(scrSet);
}

static void brightCb(lv_event_t *e) {
  displaySetBrightness((uint8_t)lv_slider_get_value(lv_event_get_target(e)));
}

static void useCenterCb(lv_event_t *) {
  double lat, lon;
  mapGetCenter(&lat, &lon);
  char b[24];
  snprintf(b, sizeof(b), "%.5f", lat);
  lv_textarea_set_text(taLat, b);
  snprintf(b, sizeof(b), "%.5f", lon);
  lv_textarea_set_text(taLon, b);
}

static void cancelCb(lv_event_t *) {
  displaySetBrightness(g_cfg.brightness);
  lv_scr_load(scrMap);
  dirty = true;
}

static void saveCb(lv_event_t *) {
  double lat = atof(lv_textarea_get_text(taLat));
  double lon = atof(lv_textarea_get_text(taLon));
  if (lat < -85 || lat > 85 || lon < -180 || lon > 180) {
    lv_label_set_text(lblInfo, "#ff5a5a Ung\xC3\xBCltige Koordinaten#");
    lv_label_set_recolor(lblInfo, true);
    return;
  }
  bool wifiChanged = strcmp(g_cfg.ssid, lv_textarea_get_text(taSsid)) || strcmp(g_cfg.pass, lv_textarea_get_text(taPass));
  bool posChanged = fabs(lat - g_cfg.lat) > 1e-6 || fabs(lon - g_cfg.lon) > 1e-6;
  strlcpy(g_cfg.ssid, lv_textarea_get_text(taSsid), sizeof(g_cfg.ssid));
  strlcpy(g_cfg.pass, lv_textarea_get_text(taPass), sizeof(g_cfg.pass));
  g_cfg.lat = lat;
  g_cfg.lon = lon;
  g_cfg.brightness = lv_slider_get_value(slBright);
  g_cfg.zoom = g_viewZoom;
  configSave();
  if (wifiChanged) g_reconnect = true;
  if (posChanged) {
    closeSelection();
    mapSetCenter(lat, lon, g_viewZoom);
    updateQuery();
    g_forcePoll = true;
  }
  lv_scr_load(scrMap);
  dirty = true;
}

static lv_obj_t *mkSection(lv_obj_t *p, const char *t) {
  lv_obj_t *l = mkLabel(p, &font_de_14, COL_ACCENT);
  lv_label_set_text(l, t);
  lv_obj_set_style_pad_top(l, 8, 0);
  return l;
}

static lv_obj_t *mkTa(lv_obj_t *p, const char *ph, bool pw) {
  lv_obj_t *ta = lv_textarea_create(p);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, ph);
  lv_textarea_set_password_mode(ta, pw);
  lv_textarea_set_max_length(ta, 64);
  lv_obj_set_width(ta, LV_PCT(100));
  lv_obj_set_style_text_font(ta, &font_de_18, 0);
  lv_obj_clear_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
  lv_obj_add_event_cb(ta, taClicked, LV_EVENT_CLICKED, nullptr);
  return ta;
}

static lv_obj_t *mkBtn(lv_obj_t *p, const char *txt, lv_color_t bg, lv_event_cb_t cb, int w) {
  lv_obj_t *b = lv_btn_create(p);
  lv_obj_set_size(b, w, 46);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *l = mkLabel(b, &font_de_18, COL_TEXT);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  return b;
}

static void buildSettings() {
  scrSet = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scrSet, lv_color_black(), 0);
  lv_obj_clear_flag(scrSet, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *c = lv_obj_create(scrSet);
  lv_obj_remove_style_all(c);
  lv_obj_set_size(c, 340, SCR_H);
  lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_top(c, 56, 0);
  lv_obj_set_style_pad_bottom(c, 110, 0);
  lv_obj_set_style_pad_row(c, 8, 0);
  lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(c, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t *t = mkLabel(c, &font_de_26, COL_TEXT);
  lv_label_set_text(t, SYM_GEAR " Einstellungen");

  mkSection(c, "WLAN");
  lv_obj_t *row = lv_obj_create(c);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 8, 0);
  taSsid = mkTa(row, "WLAN-Name", false);
  lv_obj_set_flex_grow(taSsid, 1);
  lv_obj_t *sb = mkBtn(row, SYM_WIFI, lv_color_hex(0x1f6f86), scanStart, 54);
  lv_obj_set_height(sb, 44);
  taPass = mkTa(c, "Passwort", true);

  mkSection(c, "Standort");
  taLat = mkTa(c, "Breitengrad", false);
  taLon = mkTa(c, "L\xC3\xA4ngengrad", false);
  mkBtn(c, SYM_GPS " Kartenmitte \xC3\xBC" "bernehmen", COL_PANEL2, useCenterCb, 340);

  mkSection(c, "Helligkeit");
  slBright = lv_slider_create(c);
  lv_slider_set_range(slBright, 10, 255);
  lv_obj_set_width(slBright, 300);
  lv_obj_add_event_cb(slBright, brightCb, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t *row2 = lv_obj_create(c);
  lv_obj_remove_style_all(row2);
  lv_obj_set_size(row2, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row2, 10, 0);
  lv_obj_set_style_pad_top(row2, 12, 0);
  mkBtn(row2, "Abbrechen", COL_PANEL2, cancelCb, 150);
  mkBtn(row2, SYM_SAVE " Speichern", lv_color_hex(0x1f6f86), saveCb, 170);

  lblInfo = mkLabel(c, &font_de_14, COL_MUTED);
  lv_obj_set_style_text_align(lblInfo, LV_TEXT_ALIGN_CENTER, 0);

  lv_obj_t *src = mkLabel(c, &font_de_14, COL_MUTED);
  lv_label_set_text(src, "Daten: adsb.lol (ODbL)\nKarte: \xC2\xA9 OpenStreetMap \xC2\xA9 CARTO");
  lv_obj_set_style_text_align(src, LV_TEXT_ALIGN_CENTER, 0);
}

// ------------------------------------------------------------------ Taktgeber
static void uiTimer(lv_timer_t *) {
  uint32_t now = millis();
  if (g_dataVersion != seenData) {
    seenData = g_dataVersion;
    dirty = true;
    updateInfo();
  }
  if (g_tileVersion != seenTile) { seenTile = g_tileVersion; dirty = true; }
  if (g_routeVersion != seenRoute) {
    seenRoute = g_routeVersion;
    updateInfo();
    if (g_selHex && fittedFor != g_selHex && g_route.hex == g_selHex && g_route.state == 2) {
      fittedFor = g_selHex;
      fitRoute();
    }
    dirty = true;
  }
  // Positionen zwischen den Abfragen weiterrechnen (sanfte Bewegung)
  if (!pressed && now - lastRender > 1000) dirty = true;

  if (lv_scr_act() == scrMap && dirty && now - lastRender >= 30) {
    dirty = false;
    lastRender = now;
    mapRender(canvas, canvasBuf);
    lv_obj_invalidate(canvas);
  }
  static uint32_t lastStatus = 0;
  if (now - lastStatus > 500) {
    lastStatus = now;
    updateStatus();
    if (g_selHex && !pressed) updateInfo();
  }
}

void uiInit() {
  mapSetCenter(g_cfg.lat, g_cfg.lon, g_cfg.zoom);
  updateQuery();
  buildMap();
  buildSettings();
  buildEditor();
  buildScan();
  lv_scr_load(scrMap);
  lv_timer_create(uiTimer, 15, nullptr);
  if (!g_cfg.ssid[0]) openSettings(nullptr);
}

void uiTick() {}
