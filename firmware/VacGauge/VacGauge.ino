/*
 * VacGauge - WiFi vacuum gauge for a CNC vacuum table
 * Board:  M5Stack ATOM Lite (ESP32-PICO-D4)
 * Sensor: M5Stack Tube Pressure Unit (U130), analog, -100..+200 kPa
 *         plugged into the Atom's Grove port (signal on G32)
 *
 * Serves the reading on the shop network:
 *   http://vac-gauge.local/             live page
 *   http://vac-gauge.local/api/vacuum   JSON for the ncSender plugin
 *   POST http://vac-gauge.local/api/zero   zero the sensor (pump off, hose open)
 *
 * Reading is in inHg, negative under vacuum (example: -18.4).
 * The LED shows the zone: green / yellow / red. Blue = connecting to WiFi.
 *
 * Arduino IDE: Boards Manager -> "esp32 by Espressif Systems",
 * then Tools -> Board -> "M5Atom" (or "M5Stack-ATOM"). No extra libraries.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------------- settings you change ----------------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* HOSTNAME      = "vac-gauge";      // -> http://vac-gauge.local

const float LED_GREEN_AT  = -15.0;  // inHg: green at this vacuum or stronger
const float LED_YELLOW_AT = -8.0;   // inHg: yellow down to here, red if weaker
// ------------------------------------------------------

// ATOM Lite pins
const int PIN_SENSOR = 32;   // Grove white wire (ADC1, works with WiFi on)
const int PIN_LED    = 27;   // built-in RGB LED
const int LED_LEVEL  = 24;   // 0-255, the LED is very bright

// Tube Pressure Unit: 0.1 V = -100 kPa, 1.1 V = atmosphere, 3.1 V = +200 kPa
const float KPA_PER_VOLT = 100.0;
const float KPA_AT_ZERO_VOLT = -110.0;
const float INHG_PER_KPA = 0.2953;

WebServer server(80);
Preferences prefs;

float zeroOffsetKpa = 0.0;     // stored by /api/zero
float filteredKpa = 0.0;       // before the zero offset
float filteredVolts = 0.0;
bool haveSample = false;
unsigned long lastSampleMs = 0;
unsigned long lastLedMs = 0;
unsigned long wifiDownSince = 0;

void setLed(uint8_t r, uint8_t g, uint8_t b) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rgbLedWrite(PIN_LED, r, g, b);
#else
  neopixelWrite(PIN_LED, r, g, b);
#endif
}

float readVolts() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_SENSOR);
  return (mv / 16.0) / 1000.0;
}

void sampleSensor() {
  float volts = readVolts();
  float kpa = volts * KPA_PER_VOLT + KPA_AT_ZERO_VOLT;
  if (!haveSample) {
    filteredKpa = kpa;
    filteredVolts = volts;
    haveSample = true;
  } else {
    // light smoothing: settles in about a second at 20 samples per second
    filteredKpa += 0.15 * (kpa - filteredKpa);
    filteredVolts += 0.15 * (volts - filteredVolts);
  }
}

float currentKpa()  { return filteredKpa - zeroOffsetKpa; }
float currentInHg() { return currentKpa() * INHG_PER_KPA; }

// The sensor sends about 1.1 V at atmosphere. Far below 0.05 V means it is unplugged.
bool sensorPresent() { return haveSample && filteredVolts > 0.05; }

void addCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
  server.sendHeader("Cache-Control", "no-store");
}

void handleVacuum() {
  addCors();
  if (!sensorPresent()) {
    server.send(503, "application/json", "{\"error\":\"sensor not connected\"}");
    return;
  }
  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"inHg\":%.2f,\"kPa\":%.2f,\"volts\":%.3f,\"zeroOffsetKpa\":%.2f,\"rssi\":%d,\"uptimeSec\":%lu,\"name\":\"%s\"}",
    currentInHg(), currentKpa(), filteredVolts, zeroOffsetKpa, (int)WiFi.RSSI(), millis() / 1000UL, HOSTNAME);
  server.send(200, "application/json", buf);
}

// Same shape as an ESPHome sensor endpoint, for tools written for that build.
void handleEsphomeStyle() {
  addCors();
  if (!sensorPresent()) {
    server.send(503, "application/json", "{\"error\":\"sensor not connected\"}");
    return;
  }
  char buf[140];
  snprintf(buf, sizeof(buf),
    "{\"id\":\"sensor/Vac Pressure\",\"value\":%.1f,\"state\":\"%.1f inHg\"}",
    currentInHg(), currentInHg());
  server.send(200, "application/json", buf);
}

void handleZero() {
  addCors();
  if (!sensorPresent()) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"sensor not connected\"}");
    return;
  }
  // Refuse when the reading is far from atmosphere: the pump is probably on.
  if (fabs(filteredKpa) > 8.0) {
    server.send(409, "application/json", "{\"ok\":false,\"error\":\"reading is not near zero - turn the pump off and open the hose\"}");
    return;
  }
  zeroOffsetKpa = filteredKpa;
  prefs.putFloat("zero", zeroOffsetKpa);
  server.send(200, "application/json", "{\"ok\":true}");
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Vac Gauge</title>
<style>body{font-family:system-ui,sans-serif;background:#121a22;color:#f2f5f8;text-align:center;margin:0;padding:12vh 16px}
#v{font-size:22vw;font-weight:700;font-variant-numeric:tabular-nums;line-height:1}
#u{font-size:5vw;opacity:.7}#s{margin-top:3vh;font-size:4vw;opacity:.7}
button{margin-top:6vh;padding:12px 20px;font-size:16px;border-radius:8px;border:1px solid #556;background:none;color:inherit}</style>
</head><body><div id="v">--.-</div><div id="u">inHg</div><div id="s"></div>
<button onclick="z()">Zero (pump off, hose open)</button>
<script>
async function t(){try{const r=await fetch('/api/vacuum');const j=await r.json();
if(!r.ok)throw new Error(j.error);v.textContent=j.inHg.toFixed(1);s.textContent=j.kPa.toFixed(1)+' kPa'}
catch(e){v.textContent='--.-';s.textContent=e.message||'no reading'}}
async function z(){const r=await fetch('/api/zero',{method:'POST'});const j=await r.json();alert(j.ok?'Zeroed':j.error)}
setInterval(t,500);t();
</script></body></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleNotFound() {
  addCors();
  if (server.method() == HTTP_OPTIONS) { server.send(204); return; }
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

void updateLed() {
  if (WiFi.status() != WL_CONNECTED) {
    bool on = (millis() / 400) % 2;
    setLed(0, 0, on ? LED_LEVEL : 0);            // blinking blue: no WiFi
    return;
  }
  if (!sensorPresent()) {
    bool on = (millis() / 400) % 2;
    setLed(on ? LED_LEVEL : 0, 0, on ? LED_LEVEL : 0);   // blinking purple: no sensor
    return;
  }
  float v = currentInHg();
  if (v <= LED_GREEN_AT)       setLed(0, LED_LEVEL, 0);
  else if (v <= LED_YELLOW_AT) setLed(LED_LEVEL, LED_LEVEL / 2, 0);
  else                         setLed(LED_LEVEL, 0, 0);
}

void setup() {
  Serial.begin(115200);
  setLed(0, 0, LED_LEVEL);

  analogSetPinAttenuation(PIN_SENSOR, ADC_11db);   // full 0-3.1 V range

  prefs.begin("vacgauge", false);
  zeroOffsetKpa = prefs.getFloat("zero", 0.0);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setSleep(false);              // faster replies
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to %s", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
    updateLed();
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. Open http://");
    Serial.print(WiFi.localIP());
    Serial.printf("  or  http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("WiFi not connected yet - still trying. Check the name and password.");
  }

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/vacuum", HTTP_GET, handleVacuum);
  server.on("/api/zero", HTTP_POST, handleZero);
  server.on("/sensor/Vac Pressure", HTTP_GET, handleEsphomeStyle);
  server.on("/sensor/Vac%20Pressure", HTTP_GET, handleEsphomeStyle);
  server.on("/sensor/vac_pressure", HTTP_GET, handleEsphomeStyle);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  unsigned long now = millis();

  if (now - lastSampleMs >= 50) {
    lastSampleMs = now;
    sampleSensor();
  }

  if (now - lastLedMs >= 200) {
    lastLedMs = now;
    updateLed();
  }

  // If WiFi stays down, kick it every 15 seconds.
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) wifiDownSince = now;
    if (now - wifiDownSince > 15000) {
      wifiDownSince = now;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  } else {
    wifiDownSince = 0;
  }

  server.handleClient();
  delay(2);
}
