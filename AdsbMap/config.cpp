// Einstellungen dauerhaft im Flash (NVS) speichern
#include "app.h"
#include <Preferences.h>

AppConfig g_cfg;

void configLoad() {
  Preferences p;
  p.begin("adsbmap", true);
  memset(&g_cfg, 0, sizeof(g_cfg));
  p.getString("ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
  p.getString("pass", g_cfg.pass, sizeof(g_cfg.pass));
  g_cfg.lat = p.getDouble("lat", 51.1657);   // Standard: Mitte Deutschlands
  g_cfg.lon = p.getDouble("lon", 10.4515);
  g_cfg.zoom = p.getInt("zoom", 8);
  g_cfg.brightness = p.getInt("bright", 180);
  p.end();
  if (g_cfg.zoom < ZOOM_MIN || g_cfg.zoom > ZOOM_MAX) g_cfg.zoom = 8;
  if (g_cfg.brightness < 10 || g_cfg.brightness > 255) g_cfg.brightness = 180;
}

void configSave() {
  Preferences p;
  p.begin("adsbmap", false);
  p.putString("ssid", g_cfg.ssid);
  p.putString("pass", g_cfg.pass);
  p.putDouble("lat", g_cfg.lat);
  p.putDouble("lon", g_cfg.lon);
  p.putInt("zoom", g_cfg.zoom);
  p.putInt("bright", g_cfg.brightness);
  p.end();
}
