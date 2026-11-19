/*
 * VacDisplay - WiFi display for the Vacuum Gauge
 * Board:  M5Stack CoreS3 Lite (also CoreS3 / CoreS3 SE), 320 x 240 touch screen
 *
 * Shows the vacuum-table reading from the VacGauge (M5Stack ATOM Lite) on the
 * CoreS3 screen. It reads the gauge over WiFi, so it can sit anywhere in the
 * shop and does not need ncSender to be running.
 *
 *   Tap the screen to change the view: Dial, Number, Bar, Colour block.
 *   Every view fills the whole screen. The choice is remembered.
 *
 * Arduino IDE:
 *   Boards Manager  -> "esp32 by Espressif Systems"
 *   Library Manager -> "M5Unified" (this also installs "M5GFX", 0.2.27 or newer)
 *   Tools -> Board  -> "M5CoreS3"
 *   Tools -> PSRAM  -> "OPI PSRAM" (if the menu is shown)
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------------- settings you change ----------------
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// The gauge: its IP address (best, for example "192.168.3.60") or its name.
const char* GAUGE_HOST    = "vac-gauge.local";

// Colours: green at this vacuum or stronger, red when weaker than the second.
float GREEN_AT  = -15.0;   // inHg
float YELLOW_AT = -8.0;    // inHg

// Optional. Put the ncSender PC's IP address here (for example "192.168.3.55")
// and the display uses the same green and red levels as the ncSender plugin.
// Leave it empty to use the two numbers above.
const char* NCSENDER_HOST = "";
const int   NCSENDER_PORT = 8090;

const int   BRIGHTNESS    = 170;   // 0-255
// ------------------------------------------------------

const int W = 320, H = 240;
const unsigned long POLL_MS = 500, STALE_MS = 3000, LEVELS_MS = 30000;

M5Canvas canvas(&M5.Display);
Preferences prefs;

IPAddress gaugeIp;                 // resolved address of the gauge
bool gaugeIpKnown = false;
int failsInARow = 0;
float inHg = 0;
bool haveReading = false;
unsigned long lastOkMs = 0, lastPollMs = 0, lastLevelsMs = 0, lastDrawMs = 0;
String lastError = "Connecting to WiFi";
const int SCREEN_COUNT = 4;
int screenStyle = 0;               // 0 dial, 1 number, 2 bar, 3 colour block

enum Zone { Z_GREEN, Z_YELLOW, Z_RED, Z_OFF };

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return canvas.color565(r, g, b); }

Zone zoneNow() {
  if (!haveReading || millis() - lastOkMs > STALE_MS) return Z_OFF;
  if (inHg <= GREEN_AT) return Z_GREEN;
  if (inHg <= YELLOW_AT) return Z_YELLOW;
  return Z_RED;
}

const char* zoneLabel(Zone z) {
  switch (z) {
    case Z_GREEN:  return "HOLDING";
    case Z_YELLOW: return "MARGINAL";
    case Z_RED:    return "TOO WEAK";
    default:       return "NO SIGNAL";
  }
}

uint16_t zoneColor(Zone z) {
  switch (z) {
    case Z_GREEN:  return rgb(63, 185, 80);
    case Z_YELLOW: return rgb(245, 184, 61);
    case Z_RED:    return rgb(229, 72, 77);
    default:       return rgb(125, 138, 150);
  }
}

// ---------- reading the gauge ----------

// Find "key": <number> in a small JSON text. Returns false if it is not there.
bool jsonNumber(const String& json, const char* key, float& out) {
  String pat = String("\"") + key + "\":";
  int i = json.indexOf(pat);
  if (i < 0) return false;
  i += pat.length();
  while (i < (int)json.length() && json[i] == ' ') i++;
  int j = i;
  while (j < (int)json.length() && (isDigit(json[j]) || json[j] == '-' || json[j] == '+' || json[j] == '.' || json[j] == 'e' || json[j] == 'E')) j++;
  if (j == i) return false;
  out = json.substring(i, j).toFloat();
  return true;
}

// A typed IP address is used as it is. A name ending in .local is looked up
// once and remembered, and looked up again only after several failures.
bool resolveGauge() {
  IPAddress ip;
  if (ip.fromString(GAUGE_HOST)) { gaugeIp = ip; gaugeIpKnown = true; return true; }
  String name = GAUGE_HOST;
  if (name.endsWith(".local")) {
    name.remove(name.length() - 6);
    ip = MDNS.queryHost(name.c_str(), 1500);
  } else {
    WiFi.hostByName(GAUGE_HOST, ip);
  }
  if (ip == IPAddress(0, 0, 0, 0)) { gaugeIpKnown = false; return false; }
  gaugeIp = ip;
  gaugeIpKnown = true;
  return true;
}

void pollGauge() {
  if (WiFi.status() != WL_CONNECTED) { lastError = "No WiFi"; return; }
  if (!gaugeIpKnown && !resolveGauge()) { lastError = String("Cannot find ") + GAUGE_HOST; return; }

  HTTPClient http;
  http.setConnectTimeout(1200);
  http.setTimeout(1500);
  http.setReuse(false);
  String url = String("http://") + gaugeIp.toString() + "/api/vacuum";
  bool ok = false;
  if (http.begin(url)) {
    int code = http.GET();
    if (code == 200) {
      float v;
      if (jsonNumber(http.getString(), "inHg", v)) {
        inHg = v;
        haveReading = true;
        lastOkMs = millis();
        lastError = "";
        ok = true;
      } else {
        lastError = "Unexpected reply from the gauge";
      }
    } else if (code == 503) {
      lastError = "Sensor not connected";
    } else {
      lastError = "No reply from the gauge";
    }
    http.end();
  }
  if (ok) failsInARow = 0;
  else if (++failsInARow >= 6) { failsInARow = 0; gaugeIpKnown = false; }   // look the gauge up again
}

// Optional: use the ncSender plugin's green and red levels.
void pollLevels() {
  if (NCSENDER_HOST[0] == 0 || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(1200);
  http.setTimeout(1500);
  String url = String("http://") + NCSENDER_HOST + ":" + NCSENDER_PORT + "/api/plugins/community.vacuum-gauge/settings";
  if (http.begin(url)) {
    if (http.GET() == 200) {
      String body = http.getString();
      float g, y;
      if (jsonNumber(body, "greenAt", g) && jsonNumber(body, "yellowAt", y)) {
        g = -fabs(g); y = -fabs(y);
        if (y < g) { float t = y; y = g; g = t; }
        GREEN_AT = g; YELLOW_AT = y;
      }
    }
    http.end();
  }
}

// ---------- drawing ----------

float fracOf(float v) {
  float f = -v / 30.0;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return f;
}

String numberText(Zone z) {
  if (z == Z_OFF) return "--.-";
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f", inHg);
  return String(buf);
}

// Shared pieces. Every screen fills the whole 320 x 240 display.

void drawStatus(Zone z, uint16_t fg) {
  canvas.fillCircle(18, 20, 8, zoneColor(z));
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(fg);
  canvas.drawString(zoneLabel(z), 34, 21);
}

void drawUnit(Zone z, int x, int y, uint16_t color) {
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(color);
  canvas.drawString(z == Z_OFF && lastError.length() ? lastError : String("inHg"), x, y);
}

// Screen 1: half-circle dial across the full width.
void drawDial(Zone z) {
  const int cx = 160, cy = 178, rOut = 152, rIn = 126;
  uint16_t bg = rgb(18, 26, 34), fg = rgb(242, 245, 248), muted = rgb(154, 167, 179);
  canvas.fillScreen(bg);

  // Scale: 180 degrees, left (0 inHg) to right (30 inHg).
  float aY = 180 + fracOf(YELLOW_AT) * 180, aG = 180 + fracOf(GREEN_AT) * 180;
  canvas.fillArc(cx, cy, rIn, rOut, 180, aY, rgb(229, 72, 77));
  canvas.fillArc(cx, cy, rIn, rOut, aY, aG, rgb(245, 184, 61));
  canvas.fillArc(cx, cy, rIn, rOut, aG, 360, rgb(63, 185, 80));

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(muted);
  for (int v = 5; v <= 25; v += 5) {
    float a = (180 + (v / 30.0) * 180) * DEG_TO_RAD;
    canvas.drawLine(cx + cos(a) * (rIn - 4), cy + sin(a) * (rIn - 4), cx + cos(a) * (rIn - 13), cy + sin(a) * (rIn - 13), muted);
    canvas.drawString(String(v), cx + cos(a) * (rIn - 28), cy + sin(a) * (rIn - 28));
  }

  // Needle
  float a = (180 + (z == Z_OFF ? 0 : fracOf(inHg)) * 180) * DEG_TO_RAD;
  float tx = cx + cos(a) * 112, ty = cy + sin(a) * 112;
  float px = -sin(a) * 7, py = cos(a) * 7;
  canvas.fillTriangle(tx, ty, cx + px, cy + py, cx - px, cy - py, fg);
  canvas.fillCircle(cx, cy, 12, fg);

  // Status in the corner, number under the dial
  canvas.fillCircle(12, 12, 6, zoneColor(z));
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(fg);
  canvas.drawString(zoneLabel(z), 24, 13);

  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.setTextDatum(middle_right);
  canvas.drawString(numberText(z), 218, 216);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.setTextDatum(middle_left);
  canvas.setTextColor(muted);
  canvas.drawString(z == Z_OFF ? "" : "inHg", 226, 222);
  if (z == Z_OFF && lastError.length()) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_right);
    canvas.drawString(lastError, 314, 13);
  }
}

// Screen 2: big number on a dark background.
void drawNumber(Zone z) {
  uint16_t bg = rgb(18, 26, 34), fg = rgb(242, 245, 248), muted = rgb(154, 167, 179);
  canvas.fillScreen(bg);
  drawStatus(z, fg);
  canvas.setFont(&fonts::Font8);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(fg);
  canvas.drawString(numberText(z), W / 2, 118);
  drawUnit(z, W / 2, 186, muted);
  canvas.fillRoundRect(16, 212, W - 32, 14, 7, zoneColor(z));
}

// Screen 3: big number with a coloured bar and a marker.
void drawBar(Zone z) {
  uint16_t bg = rgb(18, 26, 34), fg = rgb(242, 245, 248), muted = rgb(154, 167, 179);
  canvas.fillScreen(bg);
  drawStatus(z, fg);
  canvas.setFont(&fonts::Font8);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(fg);
  canvas.drawString(numberText(z), W / 2, 104);
  drawUnit(z, W / 2, 162, muted);

  const int x0 = 16, bw = W - 32, y0 = 192, bh = 26;
  int xY = x0 + fracOf(YELLOW_AT) * bw, xG = x0 + fracOf(GREEN_AT) * bw;
  canvas.fillRoundRect(x0, y0, bw, bh, 8, rgb(63, 185, 80));
  canvas.fillRect(x0 + 8, y0, xG - x0 - 8, bh, rgb(245, 184, 61));
  canvas.fillRoundRect(x0, y0, xY - x0 + 8, bh, 8, rgb(229, 72, 77));
  canvas.fillRect(xY, y0, xG - xY, bh, rgb(245, 184, 61));
  int xm = x0 + (z == Z_OFF ? 0 : fracOf(inHg)) * bw;
  canvas.fillRoundRect(xm - 5, y0 - 8, 10, bh + 16, 4, bg);
  canvas.fillRoundRect(xm - 3, y0 - 6, 6, bh + 12, 3, fg);
}

// Screen 4: the whole screen takes the zone colour.
void drawBlock(Zone z) {
  uint16_t bg = zoneColor(z);
  uint16_t fg = (z == Z_GREEN || z == Z_YELLOW) ? rgb(11, 20, 16) : rgb(255, 255, 255);
  canvas.fillScreen(bg);
  canvas.setTextColor(fg);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.drawString(zoneLabel(z), W / 2, 36);
  canvas.setFont(&fonts::Font8);                       // large digits
  canvas.drawString(numberText(z), W / 2, 124);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString(z == Z_OFF && lastError.length() ? lastError : String("inHg"), W / 2, 208);
}

void draw() {
  Zone z = zoneNow();
  switch (screenStyle) {
    case 1:  drawNumber(z); break;
    case 2:  drawBar(z);    break;
    case 3:  drawBlock(z);  break;
    default: drawDial(z);   break;
  }
  canvas.pushSprite(0, 0);
}

void message(const char* line1, const String& line2) {
  canvas.fillScreen(rgb(18, 26, 34));
  canvas.setTextColor(rgb(242, 245, 248));
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.drawString(line1, W / 2, 104);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.drawString(line2, W / 2, 138);
  canvas.pushSprite(0, 0);
}

// ---------- setup and loop ----------

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHTNESS);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(W, H);

  prefs.begin("vacdisplay", false);
  screenStyle = prefs.getInt("style", 0);
  if (screenStyle < 0 || screenStyle >= SCREEN_COUNT) screenStyle = 0;

  message("Vacuum Gauge", String("Connecting to ") + WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("vac-display");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(200);
  MDNS.begin("vac-display");
  if (WiFi.status() == WL_CONNECTED) lastError = "Looking for the gauge";
}

void loop() {
  M5.update();
  unsigned long now = millis();

  auto touch = M5.Touch.getDetail();
  if (touch.wasClicked()) {
    screenStyle = (screenStyle + 1) % SCREEN_COUNT;
    prefs.putInt("style", screenStyle);
    draw();
  }

  if (now - lastPollMs >= POLL_MS) {
    lastPollMs = now;
    pollGauge();
    draw();
    lastDrawMs = millis();
  }

  if (lastLevelsMs == 0 || now - lastLevelsMs >= LEVELS_MS) {
    lastLevelsMs = now ? now : 1;
    pollLevels();
  }

  delay(5);
}
