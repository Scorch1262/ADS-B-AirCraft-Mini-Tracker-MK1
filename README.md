# ADS-B Map 1.0.0 – Waveshare ESP32-S3-Touch-AMOLED-1.75

Live-Flugzeugkarte für das runde 466 × 466 AMOLED-Board.

- dunkle Karte (CARTO „dark_all“, OpenStreetMap-Daten), mit dem Finger verschiebbar, Zoom über **+ / −**
- Flugzeuge in der Umgebung aus der öffentlichen ADS-B-Quelle **adsb.lol** (alle 5 s, Bereich passt sich der Kartenansicht an)
- gestrichelte Spur je Flugzeug, Farbe nach Flughöhe (orange = tief … violett = hoch)
- **Antippen** eines Flugzeugs: komplette Flugbahn Startflughafen → Flugzeug → Zielflughafen, Karte zoomt passend
- Infofenster: Flugnummer, Kennzeichen, Start/Ziel (IATA + Stadt), Zeit bis zur Landung (+ Uhrzeit), Flugzeugtyp, Höhe, Geschwindigkeit
- Einstellungsmenü (Zahnrad): WLAN suchen/eingeben, Standort (Koordinaten oder Kartenmitte), Helligkeit – gespeichert im Flash (NVS)
- **Haus-Knopf**: zurück zum gespeicherten Standort

## Flashen (ohne Arduino-IDE)

Datei: `firmware/ADSB-Map-1.0.0_0x0.bin` – komplettes Image für **Adresse 0x0**.

**Im Browser (Chrome/Edge):**
1. Board per USB-C anschließen.
2. https://espressif.github.io/esptool-js/ öffnen, Baudrate 921600, **Connect**, Port wählen.
   Klappt das nicht: USB abziehen, **BOOT** gedrückt halten, USB wieder einstecken, BOOT loslassen, erneut verbinden.
3. Flash Address `0x0`, Datei auswählen, **Program**.
4. Danach USB-Kabel kurz abziehen und wieder einstecken (Neustart).

**Kommandozeile:** `flash_windows.bat COM5` bzw. `./flash_linux_mac.sh /dev/ttyACM0` (benötigt `pip install esptool`).

## Erster Start

Ohne gespeichertes WLAN öffnet sich das Einstellungsmenü:
1. WLAN-Symbol neben dem Namensfeld → Netz antippen → Passwort eingeben → ✓
2. Breiten-/Längengrad eintippen (Dezimalpunkt, z. B. `52.52000` / `13.40500`) oder später auf der Karte hinschieben und **Kartenmitte übernehmen**.
3. **Speichern**.

Die Spur eines Flugzeugs baut sich ab dem Moment auf, in dem das Gerät es empfängt (ca. 4 min Verlauf).
Die Gesamtflugbahn beim Antippen ist als Großkreis Start → aktuelle Position → Ziel gezeichnet
(geflogen: grau durchgezogen, verbleibend: orange gestrichelt) – keine aufgezeichnete Ist-Bahn.
Die Restflugzeit ist Luftlinie ÷ aktuelle Geschwindigkeit über Grund, also ein Schätzwert.
Ein „(?)“ hinter der Route bedeutet: Die Routendatenbank passt nicht plausibel zur Position.

## Selbst bauen

Arduino-ESP32-Core **3.3.10**, Board „ESP32S3 Dev Module“:
Flash Size 16MB · Partition Scheme „16M Flash (3MB APP/9.9MB FATFS)“ · PSRAM „OPI PSRAM“ · USB CDC On Boot „Enabled“.

Den Inhalt von `libraries/` in den Arduino-Bibliotheksordner kopieren (inkl. `lv_conf.h` direkt im Ordner `libraries`).
Angepasst gegenüber den Originalen:
- `lv_conf.h`: LVGL-Speicher über malloc (PSRAM), `millis()` als Tick, dunkles Theme, Demos aus
- `PNGdec/src/PNGdec.h`: `PNG_MAX_BUFFERED_PIXELS` für 512-px-Kacheln vergrößert

arduino-cli:
```
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,CDCOnBoot=cdc" --libraries libraries AdsbMap
```

## Datenquellen

- Flugzeuge & Routen: [adsb.lol](https://adsb.lol) API (`/v2/point`, `/api/0/routeset`), Daten unter ODbL. Aktuell ohne API-Key; adsb.lol kündigt an, künftig einen Key zu verlangen.
- Karte: © OpenStreetMap-Mitwirkende, © CARTO (Basemaps, nicht-kommerzielle Nutzung).
- HTTPS-Verbindungen prüfen keine Zertifikate (nur lesender Abruf öffentlicher Daten).
