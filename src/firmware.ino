#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <SoftwareSerial.h>
#include <TinyGPS++.h>
#include <EEPROM.h>
#include <time.h>

// ---------------- WiFi ----------------
// Replace these with your network credentials.
const char *WIFI_SSID = "Licslocs";
const char *WIFI_PASSWORD = "12345678";
const uint8_t WIFI_WAKE_PIN = D5; // momentary button to GND to wake/extend WiFi

// ---------------- Hardware pins ----------------
// GPS: connect GY-NEO6MV2 TX -> GPS_RX_PIN, RX -> GPS_TX_PIN (RX is optional).
const uint8_t GPS_RX_PIN = D7;
const uint8_t GPS_TX_PIN = D8;

// Relays: drive coil inputs. Adjust RELAY_ACTIVE_HIGH if your boards are active low.
const uint8_t RELAY1_PIN = D1;
const uint8_t RELAY2_PIN = D2;
const bool RELAY_ACTIVE_HIGH = true;

// Analog sense for battery (expects external divider to 0-1 V for NodeMCU ADC).
const uint8_t BATTERY_ADC_PIN = A0;

// ---------------- Scheduling ----------------
static const uint8_t MAX_INTERVALS = 5;
struct Interval {
  bool enabled;
  uint8_t startHour;   // 0-23
  uint8_t startMinute; // 0-59
  uint8_t endHour;     // 0-23
  uint8_t endMinute;   // 0-59
};

struct Settings {
  int16_t timezoneMinutes;       // minutes offset from UTC
  bool batteryGuardEnabled;
  float batteryDividerRatio;     // (Vout/Vin)^-1 total ratio of external divider
  float batteryCalibration;      // additional multiplier for fine tuning
  float batteryThresholdOff;     // below this -> disable relays
  float batteryThresholdOn;      // above this -> (re)enable relays
  Interval intervals[2][MAX_INTERVALS]; // [relay][slot]
};

static Settings settings;
static bool batteryAllowed = true; // hysteresis state
static bool wifiActive = false;
static uint32_t wifiTurnedOnAt = 0;
static const uint32_t WIFI_ON_DURATION_MS = 10UL * 60UL * 1000UL; // 10 minutes
static uint32_t relayTestUntil[2] = {0, 0}; // millis until forced ON per relay

// ---------------- State ----------------
ESP8266WebServer server(80);
SoftwareSerial gpsSerial(GPS_RX_PIN, GPS_TX_PIN); // RX, TX
TinyGPSPlus gps;

bool gpsHasFix = false;
time_t lastGpsEpoch = 0;       // UTC seconds from GPS
uint32_t lastGpsMillis = 0;    // millis() when lastGpsEpoch was updated

// ---------------- Helpers ----------------
bool isLeap(uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

time_t makeEpoch(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second) {
  // Simple civil date to Unix epoch, valid for 1970-2099.
  static const uint16_t daysBeforeMonth[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31) {
    return 0;
  }
  uint32_t days = 0;
  for (uint16_t y = 1970; y < year; y++) {
    days += 365 + (isLeap(y) ? 1 : 0);
  }
  days += daysBeforeMonth[month - 1];
  if (month > 2 && isLeap(year)) {
    days += 1;
  }
  days += (day - 1);
  return (time_t)(days * 86400UL + hour * 3600UL + minute * 60UL + second);
}

time_t currentUtc() {
  if (!gpsHasFix || lastGpsEpoch == 0) {
    return 0;
  }
  uint32_t elapsed = millis() - lastGpsMillis;
  return lastGpsEpoch + (elapsed / 1000);
}

time_t currentLocal() {
  time_t utc = currentUtc();
  if (utc == 0) {
    return 0;
  }
  return utc + settings.timezoneMinutes * 60;
}

void epochToTm(time_t epoch, tm &out) {
  gmtime_r(&epoch, &out); // uses UTC; we offset before calling when we need "local"
}

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_ADC_PIN); // 0-1023 maps to 0-1.0 V on NodeMCU
  float vAdc = (raw / 1023.0f) * 1.0f;   // NodeMCU ADC reference is 1.0 V
  return vAdc * settings.batteryDividerRatio * settings.batteryCalibration;
}

bool batteryOk() {
  if (!settings.batteryGuardEnabled) {
    return true;
  }
  float v = readBatteryVoltage();
  // If ADC not wired yet, v will be near 0; treat that as "unknown" -> allow.
  if (v < 0.05f) {
    return true;
  }
  // Hysteresis: keep state until a boundary is crossed.
  if (v < settings.batteryThresholdOff) {
    batteryAllowed = false;
  } else if (v > settings.batteryThresholdOn) {
    batteryAllowed = true;
  }
  return batteryAllowed;
}

bool isIntervalActive(const Interval &iv, const tm &now) {
  if (!iv.enabled) {
    return false;
  }
  uint16_t start = iv.startHour * 60 + iv.startMinute;
  uint16_t end = iv.endHour * 60 + iv.endMinute;
  uint16_t nowMin = now.tm_hour * 60 + now.tm_min;
  if (start == end) {
    return false;
  }
  if (start < end) {
    return nowMin >= start && nowMin < end;
  }
  // Handles overnight ranges (e.g., 22:00-06:00).
  return nowMin >= start || nowMin < end;
}

bool relayShouldRun(uint8_t relayIndex, const tm &now) {
  if (!batteryOk()) {
    return false;
  }
  for (uint8_t i = 0; i < MAX_INTERVALS; i++) {
    if (isIntervalActive(settings.intervals[relayIndex][i], now)) {
      return true;
    }
  }
  return false;
}

void applyRelayOutputs() {
  uint32_t nowMs = millis();
  bool test1 = nowMs < relayTestUntil[0];
  bool test2 = nowMs < relayTestUntil[1];

  time_t localEpoch = currentLocal();
  if (localEpoch == 0) {
    // Without time we keep relays off, except if a test override is active.
    bool r1 = test1;
    bool r2 = test2;
    digitalWrite(RELAY1_PIN, r1 ? RELAY_ACTIVE_HIGH : !RELAY_ACTIVE_HIGH);
    digitalWrite(RELAY2_PIN, r2 ? RELAY_ACTIVE_HIGH : !RELAY_ACTIVE_HIGH);
    return;
  }

  tm now;
  epochToTm(localEpoch, now);
  bool r1 = relayShouldRun(0, now);
  bool r2 = relayShouldRun(1, now);
  // Test override forces ON regardless of schedule/battery.
  if (test1) r1 = true;
  if (test2) r2 = true;
  digitalWrite(RELAY1_PIN, r1 ? RELAY_ACTIVE_HIGH : !RELAY_ACTIVE_HIGH);
  digitalWrite(RELAY2_PIN, r2 ? RELAY_ACTIVE_HIGH : !RELAY_ACTIVE_HIGH);
}

// ---------------- Persistence ----------------
struct StoredConfig {
  uint32_t magic;
  Settings data;
};
static const uint32_t CONFIG_MAGIC = 0xC0DEC0DF; // bumped for new thresholds
static const uint16_t EEPROM_SIZE = 1024;

void setDefaults() {
  settings.timezoneMinutes = 0;
  settings.batteryGuardEnabled = false;
  settings.batteryDividerRatio = 20.0f; // example: 1V ADC => 20V pack (adjust to your divider)
  settings.batteryCalibration = 1.0f;
  settings.batteryThresholdOff = 48.0f; // LiFePO4 16s ~ 3.0V/cell -> 48V pack
  settings.batteryThresholdOn = 50.0f;  // restart above this to avoid chatter
  for (uint8_t r = 0; r < 2; r++) {
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) {
      Interval &iv = settings.intervals[r][i];
      iv.enabled = false; // start with all disabled; slot0 still shown in UI
      iv.startHour = 6;
      iv.startMinute = 0;
      iv.endHour = 6;
      iv.endMinute = 30;
    }
  }
}

void loadConfig() {
  EEPROM.begin(EEPROM_SIZE);
  StoredConfig stored;
  EEPROM.get(0, stored);
  if (stored.magic != CONFIG_MAGIC) {
    setDefaults();
    return;
  }
  settings = stored.data;
}

void saveConfig() {
  StoredConfig stored;
  stored.magic = CONFIG_MAGIC;
  stored.data = settings;
  EEPROM.put(0, stored);
  EEPROM.commit();
}

// ---------------- GPS handling ----------------
void tryUpdateGpsTime() {
  if (gps.date.isValid() && gps.time.isValid()) {
    time_t epoch = makeEpoch(
        gps.date.year(),
        gps.date.month(),
        gps.date.day(),
        gps.time.hour(),
        gps.time.minute(),
        gps.time.second());
    if (epoch != 0) {
      lastGpsEpoch = epoch;
      lastGpsMillis = millis();
      gpsHasFix = gps.location.isValid();
    }
  }
}

void serviceGps() {
  while (gpsSerial.available()) {
    if (gps.encode(gpsSerial.read())) {
      tryUpdateGpsTime();
    }
  }
}

// ---------------- Web UI ----------------
String intervalRow(uint8_t relay, uint8_t idx, bool visible) {
  const Interval &iv = settings.intervals[relay][idx];
  String row;
  row += "<tbody class='slot";
  row += visible ? "" : " hidden";
  row += "' data-idx='";
  row += String(idx);
  row += "'>";
  row += "<tr><td rowspan='3' class='slot-label'>#" + String(idx + 1) + "<br><input type='checkbox' name='r" + String(relay) + "e" + String(idx) + "' ";
  if (iv.enabled) row += "checked";
  row += "></td><td class='act-cell' colspan='2'></td></tr>";
  row += "<tr><td>Kezdet<br><input name='r" + String(relay) + "s" + String(idx) + "h' type='number' min='0' max='23' value='" + String(iv.startHour) + "'></td>";
  row += "<td><input name='r" + String(relay) + "s" + String(idx) + "m' type='number' min='0' max='59' value='" + String(iv.startMinute) + "'></td></tr>";
  row += "<tr><td>Vege<br><input name='r" + String(relay) + "e" + String(idx) + "h' type='number' min='0' max='23' value='" + String(iv.endHour) + "'></td>";
  row += "<td><input name='r" + String(relay) + "e" + String(idx) + "m' type='number' min='0' max='59' value='" + String(iv.endMinute) + "'></td></tr>";
  row += "</tbody>";
  return row;
}

String formatTime(const tm &t) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

void handleRoot() {
  time_t utc = currentUtc();
  time_t localEpoch = currentLocal();
  tm tmUtc{}, tmLocal{};
  if (utc) epochToTm(utc, tmUtc);
  if (localEpoch) epochToTm(localEpoch, tmLocal);

  String page;
  page.reserve(7000);
  page += "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Locsolo</title>";
  page += "<style>";
  page += "body{margin:0;padding:24px;font-family:'Segoe UI',sans-serif;color:#0f1b2c;background:linear-gradient(135deg,#0c1d2e,#1f4c5b);}"; 
  page += ".wrap{max-width:900px;margin:0 auto;}h2{margin:0 0 16px;color:#f4f7fb;}h3{margin:24px 0 12px;color:#e0ecff;}h4{margin:0 0 8px;color:#cfe5ff;}";
  page += ".card{background:rgba(255,255,255,0.07);border:1px solid rgba(255,255,255,0.08);border-radius:12px;padding:16px 18px;margin-bottom:18px;box-shadow:0 10px 30px rgba(0,0,0,0.25);backdrop-filter:blur(4px);}";
  page += ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px;}";
  page += "table{width:100%;border-collapse:collapse;margin-top:8px;table-layout:fixed;}th,td{border:1px solid rgba(255,255,255,0.18);padding:8px;text-align:left;color:#f5f8fc;font-size:14px;vertical-align:bottom;}";
  page += "th{background:rgba(255,255,255,0.05);}th:nth-child(1){width:18%;}th:nth-child(2){width:41%;}th:nth-child(3){width:41%;}";
  page += ".table-wrap{overflow-x:auto;width:100%;}"; 
  page += "input[type=number]{width:100%;min-width:52px;max-width:110px;box-sizing:border-box;padding:6px;border-radius:6px;border:1px solid rgba(255,255,255,0.25);background:rgba(255,255,255,0.08);color:#f5f8fc;}";
  page += "input[type=checkbox]{transform:scale(1.2);}label{color:#dfe9f7;}";
  page += "button{margin-top:12px;padding:10px 16px;border:none;border-radius:10px;background:#3ac6c9;color:#07202b;font-weight:700;cursor:pointer;box-shadow:0 8px 18px rgba(0,0,0,0.25);}button:hover{background:#33b1b4;}";
  page += ".ghost{background:rgba(58,198,201,0.2);color:#dff6f7;border:1px solid rgba(255,255,255,0.2);}"; 
  page += ".pill{display:inline-block;padding:6px 10px;border-radius:999px;background:rgba(255,255,255,0.12);color:#c0d6f5;font-size:13px;margin-right:6px;margin-bottom:6px;}";
  page += ".status{display:flex;flex-wrap:wrap;gap:8px;}";
  page += ".hidden{display:none;}"; 
  page += ".subcard{background:rgba(255,255,255,0.03);border:1px dashed rgba(255,255,255,0.15);border-radius:10px;padding:10px;}";
  page += ".slot-label{width:18%;min-width:54px;max-width:90px;text-align:center;vertical-align:middle;font-weight:700;}"; 
  page += ".act-cell{height:14px;}"; 
  page += ".slot td input{display:block;width:100%;margin-top:0;}"; 
  page += ".flex-row{display:flex;align-items:center;justify-content:space-between;gap:8px;}"; 
  page += ".flex-row h4{margin:0;}"; 
  page += ".small-btn{padding:6px 10px;border:none;border-radius:8px;background:#3ac6c9;color:#07202b;font-weight:700;cursor:pointer;box-shadow:0 6px 14px rgba(0,0,0,0.25);width:30%;text-align:center;}"; 
  page += "@media (max-width:640px){body{padding:16px;}h2{font-size:22px;}h3{font-size:18px;}th,td{font-size:13px;padding:6px;}input[type=number]{width:100%;max-width:100%;}button{width:100%;}table{min-width:0;} .pill{width:100%;}}";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='wrap'><h2>Locsolo vezerlo</h2>";

  page += "<div class='card status'>";
  page += "<span class='pill'>GPS fix: ";
  page += gpsHasFix ? "igen" : "nem";
  page += "</span>";

  page += "<span class='pill'>UTC: ";
  page += utc ? formatTime(tmUtc) : "nincs";
  page += "</span>";
  page += "<span class='pill'>Helyi (";
  page += settings.timezoneMinutes;
  page += " perc): ";
  page += localEpoch ? formatTime(tmLocal) : "nincs";
  page += "</span>";
  page += "<span class='pill'>Akkumulator: ";
  page += String(readBatteryVoltage(), 2);
  page += " V</span>";
  page += "<span class='pill'>Ved elem: ";
  page += settings.batteryGuardEnabled ? "aktiv" : "kikapcsolva";
  page += " (" + String(settings.batteryThresholdOff, 1) + "V / ";
  page += String(settings.batteryThresholdOn, 1) + "V)</span>";
  page += "<span class='pill'>Rele1: ";
  page += digitalRead(RELAY1_PIN) == (RELAY_ACTIVE_HIGH ? HIGH : LOW) ? "BE" : "KI";
  page += "</span>";
  page += "<span class='pill'>Rele2: ";
  page += digitalRead(RELAY2_PIN) == (RELAY_ACTIVE_HIGH ? HIGH : LOW) ? "BE" : "KI";
  page += "</span>";
  page += "</div>";

  page += "<div class='card'><h3>Idozitesek</h3>";
  page += "<form method='POST' action='/schedules'><div class='grid'>";
  for (uint8_t r = 0; r < 2; r++) {
    page += "<div class='subcard'>";
    page += "<div class='flex-row'><h4>Rele ";
    page += String(r + 1);
    page += "</h4><button type='submit' formmethod='POST' formaction='/test' name='r' value='";
    page += String(r + 1);
    page += "' class='small-btn'>Teszt</button></div>";
    page += "<div class='table-wrap'><table id='tbl";
    page += String(r);
    page += "'><tr><th style='width:60px'>#</th><th>Ora</th><th>Perc</th></tr>";
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) {
      bool visible = (i == 0) || settings.intervals[r][i].enabled;
      page += intervalRow(r, i, visible);
    }
    page += "</table></div>";
    page += "<button type='button' class='ghost' onclick='addSlot(";
    page += String(r);
    page += ")'>Uj idosav</button>";
    page += "</div>";
  }
  page += "</div><div style='margin-top:10px'><button type='submit'>Idozitesek mentese</button></div></form></div>";

  page += "<form method='POST' action='/config' class='card'><h3>Altalanos beallitasok</h3>";
  page += "Idozona (perc): <input name='tz' type='number' value='" + String(settings.timezoneMinutes) + "'><br><br>";
  page += "Akkumulator feszultseg oszto arany (pl. 20): <input name='div' type='number' step='0.01' value='" + String(settings.batteryDividerRatio, 2) + "'><br>";
  page += "Kalibracios szorzo: <input name='cal' type='number' step='0.01' value='" + String(settings.batteryCalibration, 2) + "'><br>";
  page += "Leallasi kuszob (V): <input name='thOff' type='number' step='0.1' value='" + String(settings.batteryThresholdOff, 1) + "'><br>";
  page += "Visszakapcsolasi kuszob (V): <input name='thOn' type='number' step='0.1' value='" + String(settings.batteryThresholdOn, 1) + "'><br>";
  page += "Akkumulator vedelem aktiv: <input name='bg' type='checkbox' ";
  if (settings.batteryGuardEnabled) page += "checked";
  page += "><br>";
  page += "<button type='submit'>Mentes</button></form>";

  page += "<div class='card'><h3>Megjegyzes</h3>";
  page += "<p style='color:#dfe9f7;margin:0'>GPS ido + web idozites; relek csak ervenyes GPS idovel. Akkumulator vedelem hiszterezissel (le: ";
  page += String(settings.batteryThresholdOff, 1);
  page += "V, fel: ";
  page += String(settings.batteryThresholdOn, 1);
  page += "V). WiFi AP (alacsony teljesitmeny) cime: 192.168.4.1</p>";
  page += "</div>";

  page += "<div class='card'><form method='POST' action='/factory' onsubmit=\"return confirm('Biztos, hogy gyari visszaallitast kersz? Minden beallitas torlodik.');\"><button type='submit' style='width:100%;background:#d9534f;color:white;'>Gyari visszaallitas</button></form></div>";

  page += "</div>";
  page += "<script>const MAX_SLOTS=";
  page += String(MAX_INTERVALS);
  page += ";function addSlot(r){const rows=document.querySelectorAll('#tbl'+r+' tbody.slot');for(let i=0;i<rows.length;i++){if(rows[i].classList.contains('hidden')){rows[i].classList.remove('hidden');return;}}alert('Max '+MAX_SLOTS+' idosav / rele');}</script>";
  page += "</body></html>";
  server.send(200, "text/html", page);
}

void handleConfigPost() {
  if (server.hasArg("tz")) {
    settings.timezoneMinutes = server.arg("tz").toInt();
  }
  if (server.hasArg("div")) {
    settings.batteryDividerRatio = server.arg("div").toFloat();
  }
  if (server.hasArg("cal")) {
    settings.batteryCalibration = server.arg("cal").toFloat();
  }
  if (server.hasArg("thOff")) {
    settings.batteryThresholdOff = server.arg("thOff").toFloat();
  }
  if (server.hasArg("thOn")) {
    settings.batteryThresholdOn = server.arg("thOn").toFloat();
  }
  settings.batteryGuardEnabled = server.hasArg("bg");
  saveConfig();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleSchedulePost() {
  for (uint8_t r = 0; r < 2; r++) {
    for (uint8_t i = 0; i < MAX_INTERVALS; i++) {
      Interval &iv = settings.intervals[r][i];
      iv.enabled = server.hasArg("r" + String(r) + "e" + String(i));
      iv.startHour = constrain(server.arg("r" + String(r) + "s" + String(i) + "h").toInt(), 0, 23);
      iv.startMinute = constrain(server.arg("r" + String(r) + "s" + String(i) + "m").toInt(), 0, 59);
      iv.endHour = constrain(server.arg("r" + String(r) + "e" + String(i) + "h").toInt(), 0, 23);
      iv.endMinute = constrain(server.arg("r" + String(r) + "e" + String(i) + "m").toInt(), 0, 59);
    }
  }
  saveConfig();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleFactoryReset() {
  setDefaults();
  batteryAllowed = true;
  saveConfig();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void handleTestPost() {
  uint8_t relayIdx = 0;
  if (server.hasArg("r")) {
    int v = server.arg("r").toInt();
    if (v == 1 || v == 2) relayIdx = (uint8_t)(v - 1);
  }
  relayTestUntil[relayIdx] = millis() + 10000UL; // 10 seconds
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "OK");
}

void setupPins() {
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY1_PIN, !RELAY_ACTIVE_HIGH);
  digitalWrite(RELAY2_PIN, !RELAY_ACTIVE_HIGH);
  pinMode(WIFI_WAKE_PIN, INPUT_PULLUP);
}

void enableWiFi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASSWORD); // default AP IP: 192.168.4.1
  WiFi.setOutputPower(10.0f); // medium power
  wifiActive = true;
  wifiTurnedOnAt = millis();
}

void disableWiFi() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiActive = false;
}

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(9600);
  loadConfig();
  setupPins();
  enableWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.on("/schedules", HTTP_POST, handleSchedulePost);
  server.on("/factory", HTTP_POST, handleFactoryReset);
  server.on("/test", HTTP_POST, handleTestPost);
  server.begin();
}

void loop() {
  serviceGps();
  applyRelayOutputs();
  server.handleClient();

  // WiFi auto-off after timeout; wake/extend with button to GND on WIFI_WAKE_PIN.
  bool wakePressed = digitalRead(WIFI_WAKE_PIN) == LOW;
  if (wakePressed) {
    if (!wifiActive) {
      enableWiFi();
    } else {
      wifiTurnedOnAt = millis(); // extend window
    }
  }
  if (wifiActive && (millis() - wifiTurnedOnAt > WIFI_ON_DURATION_MS)) {
    disableWiFi();
  }
}
