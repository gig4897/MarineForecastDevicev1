// v 1.0
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <EEPROM.h>
#include <time.h>

void drawForecastScreen();

struct Tide {
  String t;   // "2025-05-18 17:44"
  String tm;  // "17:44"
  String tp;  // H or L
  float h;    // height
};

struct MarineData {
  String timeStr;     // e.g. "2025-05-20T18:00"
  float waveHeight;
  int wavePeriod;
};

#define MAX_MARINE_POINTS 72
MarineData marineForecast[MAX_MARINE_POINTS];
int marineCount = 0;

struct ForecastPeriod {
  String name;
  int temperature;
  String tempUnit;
  int precip;
  String windSpeed;
  String windDir;
  String timeISO;  // ← Add this line
};

const char* NOAA_API_KEY  = "xnBMdeNrExruqCzwqUWdlQzbdywRQZQf";
const char* AP_SSID       = "Marine-Forecast-Setup";
const char* AP_PASS       = "tideclock";
const uint32_t REFRESH_MS = 15UL * 60UL * 1000UL;
#define MAX_FUTURE 6

#define TP_CS   33
#define TP_CLK  25
#define TP_MOSI 32
#define TP_MISO 39
SPIClass tpSPI(HSPI);
XPT2046_Touchscreen ts(TP_CS);

#ifndef TFT_BL
  #define TFT_BL 21
#endif
const uint8_t BL_LEVELS[] = { 0, 64, 128, 192, 255 };
uint8_t blIdx = 4;

#define EE_FLAG 0xA5
#define EE_SIZE 256

struct Config {
  char zip[6]     = "32359";
  char station[9] = "8727520";
  char tz[8]      = "-4";
  char ssid[32]   = "";
  char pass[64]   = "";
} cfg;

TFT_eSPI tft;
TFT_eSprite screenSprite = TFT_eSprite(&tft);
WebServer server(80);
enum Mode { AP_MODE, RUN_MODE } mode;

ForecastPeriod currentForecast;
ForecastPeriod futureForecasts[MAX_FUTURE];
int futureForecastCount = 0;
int futureForecastIndex = 0;

Tide tides[20];
int tideCnt = 0;
float waveHeight = NAN;
int wavePeriod = -1;

uint32_t lastFetch = 0;
uint32_t lastForecastSwitch = 0;

void saveCfg() { EEPROM.writeUChar(0, EE_FLAG); EEPROM.put(1, cfg); EEPROM.commit(); }
bool loadCfg() { if (EEPROM.readUChar(0) != EE_FLAG) return false; EEPROM.get(1, cfg); return true; }

String httpGET(const String& url) {
  Serial.println("GET: " + url);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP GET failed, code: %d\n", code);
    http.end();
    return "";
  }
  String body = http.getString();
  http.end();
  return body;
}

bool getMarineForTime(const String& forecastTimeISO, float& height, int& period) {
  struct tm tmF = {};
  strptime(forecastTimeISO.c_str(), "%Y-%m-%dT%H:%M:%S", &tmF);
  tmF.tm_isdst = -1;
  time_t ft = mktime(&tmF);

  time_t bestDiff = LONG_MAX;
  int bestIdx = -1;

  for (int i = 0; i < marineCount; i++) {
    struct tm tmM = {};
    strptime(marineForecast[i].timeStr.c_str(), "%Y-%m-%dT%H:%M", &tmM);
    tmM.tm_isdst = -1;
    time_t mt = mktime(&tmM);

    time_t diff = abs(mt - ft);
    if (diff < bestDiff) {
      bestDiff = diff;
      bestIdx = i;
    }
  }

  if (bestIdx >= 0) {
    height = marineForecast[bestIdx].waveHeight;
    period = marineForecast[bestIdx].wavePeriod;
    return true;
  }

  return false;
}

bool getAdjustedTime(struct tm* tmNow) {
  int offset = atoi(cfg.tz);
  time_t raw = time(nullptr);
  raw += offset * 3600;
  localtime_r(&raw, tmNow);
  return true;
}

void setup() {
  Serial.begin(115200);
  Serial.println(">> BOOTING...");
  EEPROM.begin(EE_SIZE);
  tft.init(); tft.setRotation(0);
  screenSprite.setColorDepth(16);
  screenSprite.createSprite(240, 160);  // smaller for safety
  pinMode(TFT_BL, OUTPUT); analogWrite(TFT_BL, BL_LEVELS[blIdx]);
  tpSPI.begin(TP_CLK, TP_MISO, TP_MOSI, TP_CS);
  ts.begin(tpSPI); ts.setRotation(0);

  if (!loadCfg() || cfg.ssid[0] == '\0') {
    startAP(); return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    startAP(); return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  while (time(nullptr) < 1600000000UL) {
    Serial.println("Waiting for time sync...");
    delay(1000);
  }

  struct tm tmNow;
  getAdjustedTime(&tmNow);
  char buf[64];
  strftime(buf, sizeof(buf), "Local time: %A %Y-%m-%d %I:%M:%S %p", &tmNow);
  Serial.println(buf);

  mode = RUN_MODE;
  tft.fillScreen(TFT_BLACK);
}

bool fetchData() {
  String zj = httpGET("http://api.zippopotam.us/us/" + String(cfg.zip));
  if (zj == "") return false;
  StaticJsonDocument<512> jz;
  if (deserializeJson(jz, zj)) return false;
  double lat = jz["places"][0]["latitude"].as<double>();
  double lon = jz["places"][0]["longitude"].as<double>();

  String pj = httpGET("https://api.weather.gov/points/" + String(lat, 4) + "," + String(lon, 4));
  if (pj == "") return false;
  StaticJsonDocument<1024> jp;
  if (deserializeJson(jp, pj)) return false;
  const char* fcUrl = jp["properties"]["forecast"];

  String fj = httpGET(fcUrl);
  if (fj == "") return false;
  StaticJsonDocument<8192> jf;
  if (deserializeJson(jf, fj)) return false;

  JsonArray per = jf["properties"]["periods"];
futureForecastCount = 0;
bool gotCurrent = false;

for (JsonObject p : per) {
  String name = p["name"].as<String>();
  if (name.indexOf("Night") != -1) continue;

  ForecastPeriod fp;
  fp.name        = name;
  fp.temperature = p["temperature"].as<int>();
  fp.tempUnit    = p["temperatureUnit"].as<String>();
  fp.precip      = p["probabilityOfPrecipitation"]["value"] | 0;
  fp.windSpeed   = p["windSpeed"].as<String>();
  fp.windDir     = p["windDirection"].as<String>();
  fp.timeISO     = p["startTime"].as<String>();  // full ISO time

  if (!gotCurrent) {
    currentForecast = fp;
    gotCurrent = true;
  } else if (futureForecastCount < MAX_FUTURE) {
    futureForecasts[futureForecastCount++] = fp;
  }

  if (gotCurrent && futureForecastCount >= MAX_FUTURE) break;
}

  // --- Marine data ---
marineCount = 0;
String omUrl = "https://marine-api.open-meteo.com/v1/marine?latitude=" + String(lat, 4) +
               "&longitude=" + String(lon, 4) +
               "&hourly=wave_height,wave_period&forecast_days=3&length_unit=imperial&wind_speed_unit=mph";
String omj = httpGET(omUrl);
if (omj != "") {
  StaticJsonDocument<8192> jm;
  if (!deserializeJson(jm, omj)) {
    JsonArray wh = jm["hourly"]["wave_height"];
    JsonArray wp = jm["hourly"]["wave_period"];
    JsonArray ts = jm["hourly"]["time"];
    for (int i = 0; i < ts.size() && marineCount < MAX_MARINE_POINTS; i++) {
      marineForecast[marineCount].timeStr    = ts[i].as<String>();   // e.g. "2025-05-20T21:00"
      marineForecast[marineCount].waveHeight = wh[i].as<float>();
      marineForecast[marineCount].wavePeriod = wp[i].as<int>();
      marineCount++;
    }
  }
}

  // --- Tide data ---
  struct tm tmNow;
  getAdjustedTime(&tmNow);
  time_t now = mktime(&tmNow);

  char b1[9], b2[9];
  strftime(b1, 9, "%Y%m%d", &tmNow);
  time_t fut = now + 2 * 86400;
  struct tm tmEnd;
  localtime_r(&fut, &tmEnd);
  strftime(b2, 9, "%Y%m%d", &tmEnd);

  String tu = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter"
              "?product=predictions&station=" + String(cfg.station) +
              "&interval=hilo&units=english"
              "&time_zone=lst_ldt&datum=MLLW"
              "&begin_date=" + b1 + "&end_date=" + b2 +
              "&format=json&api_key=" + NOAA_API_KEY;
  String tj = httpGET(tu);
  if (tj == "") return false;
  StaticJsonDocument<8192> jt;
  if (deserializeJson(jt, tj)) return false;

  tideCnt = 0;
  for (JsonObject ev : jt["predictions"].as<JsonArray>()) {
    String tideTimeStr = ev["t"].as<const char*>();
    struct tm tmTide = {};
    strptime(tideTimeStr.c_str(), "%Y-%m-%d %H:%M", &tmTide);
    tmTide.tm_isdst = -1;
    time_t tideEpoch = mktime(&tmTide);
    if (tideEpoch >= now - 36 * 3600) {
      tides[tideCnt].t  = tideTimeStr;
      tides[tideCnt].tm = tideTimeStr.substring(11, 16);
      tides[tideCnt].tp = ev["type"].as<String>();
      tides[tideCnt].h  = atof(ev["v"].as<const char*>());
      if (++tideCnt >= 20) break;
    }
  }

  Serial.printf("Loaded %d tide entries:\n", tideCnt);
  for (int i = 0; i < tideCnt; i++) {
    Serial.printf("%d: %s %s %.2fft\n", i, tides[i].t.c_str(), tides[i].tp.c_str(), tides[i].h);
  }

  return true;
}

void drawTideGraph() {
  if (tideCnt < 4) return;

  struct tm tmNow;
  getAdjustedTime(&tmNow);
  time_t now = mktime(&tmNow);

  // Find last tide before now
  int prevIdx = -1;
  for (int i = 0; i < tideCnt; i++) {
    struct tm tmTide = {};
    strptime(tides[i].t.c_str(), "%Y-%m-%d %H:%M", &tmTide);
    tmTide.tm_isdst = -1;
    time_t tideEpoch = mktime(&tmTide);
    if (tideEpoch > now) break;
    prevIdx = i;
  }
  if (prevIdx < 0) prevIdx = 0;

  int first = max(0, prevIdx);
  int last  = min(tideCnt - 1, first + 4);
  if (last - first < 3) first = max(0, last - 3);
  int displayCnt = last - first + 1;

  // Layout
  int dotPad = 4;
  int graphLeft = 20;
  int graphWidth = 200;
  int graphTop = 44;
  int graphHeight = 70 - 2 * dotPad;
  int graphBottom = graphTop + graphHeight;

  tft.fillRect(graphLeft - 5, graphTop - dotPad, graphWidth + 10, graphHeight + 2 * dotPad, TFT_NAVY);

  float minH = tides[first].h, maxH = tides[first].h;
  for (int i = first + 1; i <= last; i++) {
    if (tides[i].h < minH) minH = tides[i].h;
    if (tides[i].h > maxH) maxH = tides[i].h;
  }
  float range = maxH - minH;
  if (range < 0.01) range = 0.01;

  int x[displayCnt], y[displayCnt];
  time_t timeStart, timeEnd;
  {
    struct tm tm1 = {}, tm2 = {};
    strptime(tides[first].t.c_str(), "%Y-%m-%d %H:%M", &tm1);
    strptime(tides[last].t.c_str(),  "%Y-%m-%d %H:%M", &tm2);
    tm1.tm_isdst = -1;
    tm2.tm_isdst = -1;
    timeStart = mktime(&tm1);
    timeEnd   = mktime(&tm2);
  }

  for (int i = 0; i < displayCnt; i++) {
    struct tm tmTide = {};
    strptime(tides[first + i].t.c_str(), "%Y-%m-%d %H:%M", &tmTide);
    tmTide.tm_isdst = -1;
    time_t tideEpoch = mktime(&tmTide);
    float timeFrac = float(tideEpoch - timeStart) / float(timeEnd - timeStart);
    x[i] = graphLeft + timeFrac * graphWidth;

    float norm = (tides[first + i].h - minH) / range;
    y[i] = graphTop + graphHeight - norm * graphHeight;
  }

  // Fill under curve
  uint16_t fillColor = tft.color565(0, 128, 255);  // light blue
  for (int i = 0; i < displayCnt - 1; i++) {
    int x0 = x[i], y0 = y[i];
    int x1 = x[i + 1], y1 = y[i + 1];
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    for (float t = 0; t <= 1.0; t += 0.01) {
      float inv = 1.0 - t;
      float xt = inv * inv * x0 + 2 * inv * t * cx + t * t * x1;
      float yt = inv * inv * y0 + 2 * inv * t * cy + t * t * y1;
      int yStart = (int)yt;
      int height = graphBottom - yStart;
      if (height > 0 && yStart >= graphTop && yStart <= graphBottom) {
        tft.drawFastVLine((int)xt, yStart, height, fillColor);
      }
    }
  }

  // Tide curve
  for (int i = 0; i < displayCnt - 1; i++) {
    int x0 = x[i], y0 = y[i], x1 = x[i + 1], y1 = y[i + 1];
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    int prevX = x0, prevY = y0;
    for (float t = 0.0; t <= 1.0; t += 0.05) {
      float inv = 1.0 - t;
      float xt = inv * inv * x0 + 2 * inv * t * cx + t * t * x1;
      float yt = inv * inv * y0 + 2 * inv * t * cy + t * t * y1;
      tft.drawLine(prevX, prevY, (int)xt, (int)yt, TFT_CYAN);
      prevX = (int)xt;
      prevY = (int)yt;
    }
  }

  // Red line for now
  for (int i = 0; i < displayCnt - 1; i++) {
    struct tm tm1 = {}, tm2 = {};
    strptime(tides[first + i].t.c_str(), "%Y-%m-%d %H:%M", &tm1);
    strptime(tides[first + i + 1].t.c_str(), "%Y-%m-%d %H:%M", &tm2);
    tm1.tm_isdst = -1;
    tm2.tm_isdst = -1;
    time_t t1 = mktime(&tm1);
    time_t t2 = mktime(&tm2);
    if (now >= t1 && now <= t2) {
      float frac = float(now - t1) / float(t2 - t1);
      int xNow = x[i] + frac * (x[i + 1] - x[i]);
      tft.drawLine(xNow, graphTop - dotPad, xNow, graphBottom + dotPad, TFT_RED);
      break;
    }
  }

  // Dots and labels
  tft.setTextFont(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (int i = 0; i < displayCnt; i++) {
    tft.fillCircle(x[i], y[i], 2, TFT_WHITE);
    int labelX = constrain(x[i] - 12, graphLeft, graphLeft + graphWidth - 40);
    char label[12];
    snprintf(label, sizeof(label), "%s %.1fft", tides[first + i].tp.c_str(), tides[first + i].h);
    if (tides[first + i].tp == "H") {
      tft.setCursor(labelX, graphTop - 22); tft.print(label);
      tft.setCursor(labelX, graphTop - 12); tft.print(tides[first + i].tm);
    } else {
      tft.setCursor(labelX, graphBottom + dotPad + 4);  tft.print(label);
      tft.setCursor(labelX, graphBottom + dotPad + 14); tft.print(tides[first + i].tm);
    }
  }

}

void drawForecastScreen() {
  drawTideGraph();          // drawn directly to screen (flashes once)
  drawForecastOnly();       // into sprite
  screenSprite.pushSprite(0, 140);
}

void drawForecastOnly() {
  // Draw forecast section off-screen
  screenSprite.fillSprite(TFT_BLACK);

  int y = 0;
  screenSprite.setTextFont(2);
  screenSprite.setTextColor(TFT_CYAN, TFT_BLACK);
  screenSprite.setCursor(10, y);
  screenSprite.println(currentForecast.name);

  y += 17;
  screenSprite.setTextColor(TFT_WHITE, TFT_BLACK);
  screenSprite.setCursor(10, y);
  screenSprite.printf("Temp: %d %s", currentForecast.temperature, currentForecast.tempUnit.c_str());

  y += 17;
  screenSprite.setCursor(10, y);
  screenSprite.printf("Rain: %d%%", currentForecast.precip);

  y += 17;
  screenSprite.setCursor(10, y);
  screenSprite.printf("Wind: %s %s", currentForecast.windDir.c_str(), currentForecast.windSpeed.c_str());

  y += 17;
  screenSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
  float h;
  int p;
  if (getMarineForTime(currentForecast.timeISO, h, p))
    screenSprite.setCursor(10, y), screenSprite.printf("Waves: %.1f ft, Period: %d s", h, p);
  else
    screenSprite.setCursor(10, y), screenSprite.print("Waves: --");

  y += 20;
  screenSprite.drawFastHLine(0, y, 240, TFT_SKYBLUE);
  screenSprite.drawFastHLine(0, y + 1, 240, TFT_SKYBLUE);
  y += 7;

  if (futureForecastCount > 0) {
    ForecastPeriod &nf = futureForecasts[futureForecastIndex];
    screenSprite.setTextFont(2);
    screenSprite.setTextColor(TFT_CYAN, TFT_BLACK);
    screenSprite.setCursor(10, y);
    screenSprite.printf("Next: %s", nf.name.c_str());
    y += 17;
    screenSprite.setTextColor(TFT_WHITE, TFT_BLACK);
    screenSprite.setCursor(10, y);
    screenSprite.printf("Temp: %d %s  Rain: %d%%", nf.temperature, nf.tempUnit.c_str(), nf.precip);
    y += 17;
    screenSprite.setCursor(10, y);
    screenSprite.printf("Wind: %s %s", nf.windDir.c_str(), nf.windSpeed.c_str());
    y += 17;
    screenSprite.setTextColor(TFT_YELLOW, TFT_BLACK);
    if (getMarineForTime(nf.timeISO, h, p))
      screenSprite.setCursor(10, y), screenSprite.printf("Waves: %.1f ft, Period: %d s", h, p);
    else
      screenSprite.setCursor(10, y), screenSprite.print("Waves: --");
  }
}

void loop() {
  if (mode == AP_MODE) {
    server.handleClient();
    delay(10);
    return;
  }

  if (!lastFetch || millis() - lastFetch > REFRESH_MS) {
    if (fetchData()) {
      drawForecastScreen();
    } else {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.setTextFont(2);
      tft.setCursor(60, 100); tft.println("OFFLINE");
      tft.setCursor(20, 130); tft.println("Waiting...");
    }
    lastFetch = millis();
    lastForecastSwitch = millis();
  }

if (futureForecastCount > 0 && millis() - lastForecastSwitch > 6000) {
  futureForecastIndex = (futureForecastIndex + 1) % futureForecastCount;
  drawForecastOnly();
  
  screenSprite.pushSprite(0, 140);
  lastForecastSwitch = millis();
}
  handleTouch();
}

void handleTouch() {
  static uint32_t tStart = 0;
  static bool touching = false, longDone = false;
  TS_Point p = ts.getPoint();
  if (p.z > 50) {
    if (!touching) { tStart = millis(); touching = true; longDone = false; }
    else if (!longDone && millis() - tStart > 5000) {
      EEPROM.writeUChar(0, 0); EEPROM.commit(); ESP.restart();
      longDone = true;
    }
  } else if (touching) {
if (!longDone && millis() - tStart < 600) {
  // Short tap: cycle brightness
  blIdx = (blIdx + 1) % (sizeof(BL_LEVELS));
  analogWrite(TFT_BL, BL_LEVELS[blIdx]);

  // Show brightness % on screen
  int percent = (BL_LEVELS[blIdx] * 100) / 255;
  tft.fillRect(60, 140, 120, 30, TFT_BLACK);
  tft.setTextFont(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(70, 150);
  tft.printf("Brightness: %d%%", percent);

  delay(500);  // show the message briefly

  drawForecastOnly();
  screenSprite.pushSprite(0, 140);  // redraw forecast after message
}

    touching = false;
  }
}
bool isValidZip(const String& zip) {
  return zip.length() == 5 && zip.toInt() > 0;
}
bool isValidStation(const String& station) {
  return station.length() >= 6 && station.toInt() > 0;
}
bool isValidTz(const String& tz) {
  int val = tz.toInt();
  return val >= -12 && val <= 14;
}

void handleRoot() {
  String html = "<html><body><form method=post action=/save>";
  html += "ZIP: <input name=zip value='" + String(cfg.zip) + "'><br>";
  html += "Station ID: <input name=station value='" + String(cfg.station) + "'><br>";
  html += "Time Zone: <select name='tz'>";
  for (int i = -12; i <= 14; i++) {
    String label = "(UTC" + String(i >= 0 ? "+" : "") + String(i) + ")";
    if (i == atoi(cfg.tz)) {
      html += "<option value='" + String(i) + "' selected>" + label + "</option>";
    } else {
      html += "<option value='" + String(i) + "'>" + label + "</option>";
    }
  }
  html += "</select><br>";
  html += "SSID: <input name=ssid value='" + String(cfg.ssid) + "'><br>";
  html += "Pass: <input name=pass type=password value='" + String(cfg.pass) + "'><br>";
  html += "<button>Save & Reboot</button></form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String z = server.arg("zip"), st = server.arg("station"), tz = server.arg("tz");
  if (!isValidZip(z) || !isValidStation(st) || !isValidTz(tz)) {
    server.send(400, "text/html", "<h3>Invalid input! ZIP, Station, or TZ is not valid.</h3><a href='/'>Go Back</a>");
    return;
  }
  strncpy(cfg.zip, z.c_str(), 5);
  strncpy(cfg.station, st.c_str(), 8);
  strncpy(cfg.tz, tz.c_str(), 7);
  strncpy(cfg.ssid, server.arg("ssid").c_str(), 31);
  strncpy(cfg.pass, server.arg("pass").c_str(), 63);
  saveCfg();
  server.send(200, "text/html", "<h3>Saved. Rebooting…</h3>");
  delay(1500); ESP.restart();
}

void startAP() {
  mode = AP_MODE;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  IPAddress ip = WiFi.softAPIP();
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(2); tft.setCursor(8, 20);
  tft.println("Setup Mode");
  tft.println("-------------------");
  tft.printf("SSID: %s\n", AP_SSID);
  if (strlen(AP_PASS)) tft.printf("Pass: %s\n", AP_PASS);
  tft.println(); tft.print("Open http://"); tft.println(ip.toString());
}
