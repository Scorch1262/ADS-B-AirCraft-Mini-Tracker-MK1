/*
  ADS-B Map  –  Live-Flugzeugkarte für Waveshare ESP32-S3-Touch-AMOLED-1.75
  -------------------------------------------------------------------------
  - dunkle Karte (CARTO "dark_all", OpenStreetMap-Daten), mit dem Finger verschiebbar
  - Flugzeuge in der Nähe aus der öffentlichen ADS-B-Quelle adsb.lol
  - gestrichelte Spur jedes Flugzeugs, Farbe nach Flughöhe
  - Antippen: komplette Flugbahn Start -> Flugzeug -> Ziel plus Infofenster
    (Flugnummer, Start/Ziel, Zeit bis zur Landung, Flugzeugtyp)
  - Einstellungsmenü für WLAN und Standort, gespeichert im Flash (NVS)

  Arduino-ESP32-Core 3.3.x, Board "ESP32S3 Dev Module":
    Flash Size 16MB, Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)", PSRAM "OPI PSRAM",
    USB CDC On Boot "Enabled"
*/
#include "app.h"
#include <Wire.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"

// ---------------------------------------------------------------- Pins (Board-Schaltplan)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
static Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, SCR_W, SCR_H, 6, 0, 0, 0);
static TouchDrvCST92xx touch;
static bool touchOk = false;

#define DRAW_LINES 32
static lv_disp_draw_buf_t drawBuf;

void displaySetBrightness(uint8_t v) { gfx->setBrightness(v); }

// CO5300 benötigt gerade Startkoordinaten und gerade Breiten/Höhen
static void rounderCb(lv_disp_drv_t *, lv_area_t *a) {
  if (a->x1 & 1) a->x1--;
  if (a->y1 & 1) a->y1--;
  if (!(a->x2 & 1)) a->x2++;
  if (!(a->y2 & 1)) a->y2++;
}

static void flushCb(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, w, h);
  lv_disp_flush_ready(d);
}

static void touchReadCb(lv_indev_drv_t *, lv_indev_data_t *data) {
  static int16_t x[2], y[2];
  static lv_coord_t lastX = 0, lastY = 0;
  if (touchOk && touch.getPoint(x, y, 1)) {
    lastX = x[0];
    lastY = y[0];
    data->state = LV_INDEV_STATE_PR;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
  data->point.x = lastX;
  data->point.y = lastY;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nADS-B Map " APP_VERSION);
  if (!psramFound()) Serial.println("WARNUNG: kein PSRAM gefunden - Board-Einstellung 'OPI PSRAM' pruefen!");

  configLoad();

  Wire.begin(IIC_SDA, IIC_SCL);
  gfx->begin();
  gfx->fillScreen(0x0000);
  gfx->setBrightness(g_cfg.brightness);

  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(30);
  digitalWrite(TP_RESET, HIGH);
  delay(80);
  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(SCR_W, SCR_H);
    touch.setMirrorXY(true, true);
  } else {
    Serial.println("Touch-Controller nicht gefunden");
  }

  lv_init();
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(SCR_W * DRAW_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(SCR_W * DRAW_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&drawBuf, b1, b2, SCR_W * DRAW_LINES);

  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = SCR_W;
  dd.ver_res = SCR_H;
  dd.flush_cb = flushCb;
  dd.rounder_cb = rounderCb;
  dd.draw_buf = &drawBuf;
  lv_disp_t *disp = lv_disp_drv_register(&dd);
  lv_disp_set_theme(disp, lv_theme_default_init(disp, lv_color_hex(0x1f8fb0), lv_color_hex(0xffb020), true, &font_de_14));

  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touchReadCb;
  lv_indev_drv_register(&id);

  mapInit();    // Kachel-Cache im PSRAM
  netStart();   // Datenpuffer und Netz-Tasks
  uiInit();
  Serial.printf("Freier Heap: intern %u, PSRAM %u\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void loop() {
  lv_timer_handler();
  delay(4);
}
