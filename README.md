## Locsolo (ESP8266 + GPS + relék + akkuőr)

Firmware NodeMCU ESP8266-hoz: GY-NEO6MV2 GPS ad időt, két relét időzít, webfelületet szolgál ki, és LiFePO4 16s akku feszültségét felügyeli.

### Hardver bekötés
- GPS GY-NEO6MV2: TX -> `D7` (ESP RX), RX -> `D8` (opcionális), GND/GND, VCC/3V3 vagy 5V (modul szerint).
- Relék: IN1 -> `D1`, IN2 -> `D2`, VCC/5V, GND/GND. `RELAY_ACTIVE_HIGH = true` (ha aktív alacsony, állítsd `false`-ra).
- Akkufeszültség: NodeMCU ADC 0-1 V. Nagy ellenállásosztót használj (pl. ~1 MOhm / 18 kOhm) 57.6 V-hoz 1 V alatt. Osztó arány a weben vagy kódban (`batteryDividerRatio`). Bekötés: osztó alja -> `A0`, teteje -> akku +, közepe -> GND.

### Fordítás / függőségek
- Arduino Core for ESP8266 (2.7+).
- Könyvtárak: `TinyGPSPlus`, `ESP8266WebServer`, `SoftwareSerial`.
- Forrás: `src/firmware.ino`. Feltöltés: 80 MHz / 4M (FS:1M)/1M.

### Wi-Fi
- `WIFI_SSID` / `WIFI_PASSWORD` állítás. AP módban indul, alap IP: `192.168.4.1`.
- Teljesítmény közepes (`WiFi.setOutputPower(10.0f)`), rövid hatótáv.
- 10 perc után automatikus Wi-Fi kikapcs; a `WIFI_WAKE_PIN` (D5) gomb GND-re húzva ébreszt/újraindítja a 10 perces időzítőt.

### Webfelület
- Cím: `http://192.168.4.1/`
- Státusz: GPS fix, UTC/helyi idő, akku fesz, relé állapotok.
- Időzítések: relé-kártyák, max 5 intervallum (kezdet/vége óra:perc), első sor látszik, többi gombbal hozzáadható; éjjelre átnyúlás is megy.
- Teszt gomb relénként: 10 mp-re behúzza a relét (GPS idő nélkül is működik).
- Általános beállítások: időzóna (perc), akku osztó arány, kalibráció, le-/visszakapcsolási küszöb (hiszterézis), akkuvédelem ki/be. A panel az időzítések alatt.
- Gyári visszaállítás: alul gomb, megerősítő üzenettel, teljes config törlésével.
- Mobilbarát, rugalmas táblázatokkal; mentés után automatikus visszairányítás. EEPROM: időzóna, akku paraméterek, intervallumok.

### Idő / GPS
- GPS-alapú idő (NTP nincs). Érvényes dátum+idő után epoch beállít, `millis()`-szel léptet.
- GPS idő hiányál: relék kikapcsolva, kivéve aktív teszt alatt.

### Relék és akkuőr
- Feltétel: érvényes GPS idő **és** aktív időintervallum **és** (ha engedélyezett) akku OK.
- Akkumérés: `readBatteryVoltage()` osztó arány + kalibráció szerint. Ha ADC nincs kötve (0 V), engedi a reléket (ismeretlennek veszi).
- Akkuvédelem hiszterézissel: `batteryThresholdOff` alatt tilt, `batteryThresholdOn` felett újra enged (pl. 48 V / 50 V).

### Testreszabás
- Intervallumok száma: `MAX_INTERVALS` (alap 5).
- Aktív szint: `RELAY_ACTIVE_HIGH`.
- Pinek, osztó arány a `src/firmware.ino` tetején állítható.
