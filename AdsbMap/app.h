// ADS-B Map für Waveshare ESP32-S3-Touch-AMOLED-1.75
// Gemeinsame Typen, Konstanten und Schnittstellen der Module.
#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define APP_VERSION "1.0.0"

// ---------------------------------------------------------------- Display
#define SCR_W 466
#define SCR_H 466
#define SCR_CX (SCR_W / 2)
#define SCR_CY (SCR_H / 2)

// ---------------------------------------------------------------- Karte
#define TILE_PX 512          // CARTO @2x-Kacheln (512 x 512 Pixel)
#define TILE_CACHE 10        // 10 x 512 KB im PSRAM
#define ZOOM_MIN 3
#define ZOOM_MAX 13

// ---------------------------------------------------------------- Flugzeuge
#define MAX_AC 160
#define TRAIL_LEN 48         // Punkte gestrichelte Spur (ca. 4-5 min bei 5 s Takt)
#define AC_TIMEOUT_MS 60000  // nicht mehr gemeldete Flugzeuge nach 60 s entfernen
#define POLL_INTERVAL_MS 5000

// ---------------------------------------------------------------- Einstellungen
struct AppConfig {
  char ssid[33];
  char pass[65];
  double lat;
  double lon;
  int zoom;
  int brightness;  // 10..255
};
extern AppConfig g_cfg;
void configLoad();
void configSave();

// ---------------------------------------------------------------- Daten
struct Aircraft {
  bool used;
  uint32_t hex;
  char flight[10];
  char reg[12];
  char type[8];
  char desc[40];
  double lat, lon;
  float track;     // Grad
  float gs;        // Knoten
  int alt;         // Fuß, INT32_MIN = unbekannt
  bool ground;
  uint32_t posMs;  // millis() zum Zeitpunkt der Positionsmeldung
  uint32_t seenMs; // millis() letzter Empfang
  uint8_t trailN;
  uint8_t trailHead;
  float trailLat[TRAIL_LEN];
  float trailLon[TRAIL_LEN];
};

struct Airport {
  char icao[5];
  char iata[4];
  char name[48];
  char city[32];
  double lat, lon;
};

struct RouteInfo {
  uint32_t hex;          // zu welchem Flugzeug
  uint8_t state;         // 0 = leer, 1 = lädt, 2 = fertig, 3 = unbekannt, 4 = Fehler
  char number[12];
  char airline[8];
  bool plausible;
  bool hasOrigin, hasDest;
  Airport origin, dest;
};

extern Aircraft *g_ac;             // Array MAX_AC (PSRAM)
extern SemaphoreHandle_t g_lock;   // schützt g_ac und g_route
extern RouteInfo g_route;
extern volatile uint32_t g_dataVersion;   // wird bei neuen ADS-B-Daten erhöht
extern volatile uint32_t g_tileVersion;   // wird bei neu geladener Kachel erhöht
extern volatile uint32_t g_routeVersion;  // wird bei neuer Route erhöht
extern volatile int g_acCount;
extern volatile bool g_apiOk;
extern volatile bool g_wifiOk;
extern volatile bool g_scanBusy;          // Netz-Task pausiert Verbindungsversuche
extern volatile bool g_reconnect;         // Einstellungen geändert -> neu verbinden

// vom UI gesetzt, vom Netz-Task gelesen: Abfragezentrum und Radius
extern volatile double g_queryLat, g_queryLon;
extern volatile int g_queryRadiusNm;

// Routenabfrage anstoßen (UI -> Netz-Task)
void netRequestRoute(uint32_t hex, const char *callsign, double lat, double lon);
void netStart();

// ---------------------------------------------------------------- Karte (map.cpp)
struct TileSlot {
  int16_t z;
  int32_t x, y;
  volatile uint8_t state;  // 0 frei, 1 angefordert, 2 bereit, 3 Fehler
  uint32_t lastUse;
  uint32_t failMs;
  lv_color_t *px;
};
extern TileSlot g_tiles[TILE_CACHE];
extern SemaphoreHandle_t g_tileLock;
extern volatile int g_viewZoom;
extern volatile double g_viewWX, g_viewWY;  // Kartenmitte in Weltpixeln der aktuellen Zoomstufe

void mapInit();
void mapRender(lv_obj_t *canvas, lv_color_t *buf);
int mapNextTileRequest(int *z, int *x, int *y);  // Netz-Task: nächste zu ladende Kachel
void mapTileLoaded(int slot, bool ok);
uint32_t mapHitTest(int sx, int sy);

// Projektion
double lonToWX(double lon, int z);
double latToWY(double lat, int z);
double wxToLon(double wx, int z);
double wyToLat(double wy, int z);
double worldSize(int z);
void mapSetCenter(double lat, double lon, int z);
void mapGetCenter(double *lat, double *lon);
double haversineKm(double lat1, double lon1, double lat2, double lon2);
void greatCirclePoint(double lat1, double lon1, double lat2, double lon2, double f, double *lat, double *lon);

extern volatile uint32_t g_selHex;  // ausgewähltes Flugzeug (0 = keins)

// ---------------------------------------------------------------- UI (ui.cpp)
void uiInit();
void uiTick();

LV_FONT_DECLARE(font_de_14);
LV_FONT_DECLARE(font_de_18);
LV_FONT_DECLARE(font_de_26);

// Symbole (FontAwesome in den Schriften enthalten)
#define SYM_GEAR  "\xEF\x80\x93"  // 0xF013
#define SYM_HOME  "\xEF\x80\x95"  // 0xF015
#define SYM_PLUS  "\xEF\x81\xA7"  // 0xF067
#define SYM_MINUS "\xEF\x81\xA8"  // 0xF068
#define SYM_WIFI  "\xEF\x87\xAB"  // 0xF1EB
#define SYM_CLOSE "\xEF\x80\x8D"  // 0xF00D
#define SYM_OK    "\xEF\x80\x8C"  // 0xF00C
#define SYM_REFRESH "\xEF\x80\xA1" // 0xF021
#define SYM_GPS   "\xEF\x84\xA4"  // 0xF124
#define SYM_SAVE  "\xEF\x83\x87"  // 0xF0C7
#define SYM_WARN  "\xEF\x81\xB1"  // 0xF071
