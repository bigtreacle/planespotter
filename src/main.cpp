// Nearest-plane display for the Cheap Yellow Display (ESP32-2432S028).
//
// Shows the closest airborne aircraft to a home location: airline (in a brand
// colour), route, type, altitude, speed, and distance/direction from you.
//
// Setup: on first boot the board opens a WiFi hotspot called "PlaneSpotter".
// Join it, pick the home WiFi, enter a UK postcode (or lat/lon). To change
// settings later, hold the BOOT button on the back for 3 seconds. A short
// press checks for a firmware update (see ota.h); updates also happen
// automatically 20 s after WiFi connects and then once a day.
//
// Data: adsb.lol (fallback adsb.fi) for positions, adsbdb.com for routes,
// postcodes.io for postcode lookup. All free, no API keys.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

#include "ota.h"

// ---------- constants ----------
static const char *AP_NAME = "PlaneSpotter";
static const int BOOT_BTN = 0;
static const int BACKLIGHT_PIN = 21;
static const uint32_t POLL_MS = 30000;
static const uint32_t OTA_BOOT_DELAY_MS = 20000;            // after WiFi is up
static const uint32_t OTA_PERIOD_MS = 24UL * 60 * 60 * 1000; // then daily
static const uint32_t OTA_VALID_AFTER_MS = 60000;           // uptime that counts as "works"
static const int W = 320, H = 240;
static const int STATS_Y = 168;  // top of the bottom stats band

// ---------- airline brand colours (by ICAO callsign prefix) ----------
struct Airline {
  const char *icao;
  const char *name;
  uint8_t r, g, b;
};
static const Airline AIRLINES[] = {
    {"BAW", "British Airways", 60, 120, 230},
    {"SHT", "British Airways", 60, 120, 230},
    {"CFE", "BA CityFlyer", 60, 120, 230},
    {"EZY", "easyJet", 255, 102, 0},
    {"EJU", "easyJet", 255, 102, 0},
    {"EZS", "easyJet", 255, 102, 0},
    {"RYR", "Ryanair", 241, 201, 51},
    {"RUK", "Ryanair", 241, 201, 51},
    {"VIR", "Virgin Atlantic", 230, 20, 60},
    {"EXS", "Jet2", 235, 40, 40},
    {"TOM", "TUI", 110, 190, 240},
    {"WZZ", "Wizz Air", 210, 0, 140},
    {"WUK", "Wizz Air", 210, 0, 140},
    {"LOG", "Loganair", 220, 40, 50},
    {"EIN", "Aer Lingus", 0, 170, 120},
    {"DLH", "Lufthansa", 255, 173, 0},
    {"AFR", "Air France", 80, 110, 230},
    {"KLM", "KLM", 0, 161, 228},
    {"IBE", "Iberia", 225, 30, 40},
    {"VLG", "Vueling", 255, 204, 0},
    {"TAP", "TAP Air Portugal", 0, 170, 90},
    {"SWR", "Swiss", 230, 30, 40},
    {"AUA", "Austrian", 230, 30, 40},
    {"SAS", "SAS", 70, 100, 220},
    {"FIN", "Finnair", 90, 120, 220},
    {"NOZ", "Norwegian", 220, 30, 50},
    {"NSZ", "Norwegian", 220, 30, 50},
    {"THY", "Turkish Airlines", 225, 30, 40},
    {"UAE", "Emirates", 225, 30, 40},
    {"QTR", "Qatar Airways", 180, 40, 90},
    {"ETD", "Etihad", 200, 160, 100},
    {"SIA", "Singapore Airlines", 240, 170, 40},
    {"CPA", "Cathay Pacific", 0, 150, 130},
    {"QFA", "Qantas", 230, 30, 40},
    {"ACA", "Air Canada", 230, 30, 40},
    {"AAL", "American Airlines", 180, 195, 210},
    {"UAL", "United", 70, 120, 230},
    {"DAL", "Delta", 215, 30, 60},
    {"JBU", "JetBlue", 60, 130, 240},
    {"AIC", "Air India", 225, 50, 40},
    {"FDX", "FedEx", 140, 80, 210},
    {"UPS", "UPS", 170, 120, 60},
    {"BCS", "DHL", 255, 204, 0},
    {"DHK", "DHL", 255, 204, 0},
};

// ---------- state ----------
struct Config {
  String postcode;     // as entered by the user
  String pcResolved;   // postcode that lat/lon were last resolved from
  float lat = 51.5074; // default: central London
  float lon = -0.1278;
  int radiusNm = 15;
  int brightness = 80; // percent
};

struct Plane {
  bool valid = false;
  String hex, callsign, reg, type;
  long altFt = 0;
  int speedKt = 0;
  int vrate = 0; // ft/min
  float distMi = 0;
  float bearing = 0; // degrees from home to aircraft
};

struct Route {
  String forCallsign;
  bool known = false;
  String airline, oIata, oCity, dIata, dCity;
};

TFT_eSPI tft;
Preferences prefs;
WiFiManager wm;
Config cfg;
Plane shown;
Route route;
uint32_t lastPoll = 0;
bool lastFetchOk = true;
bool everFetchedOk = false;
uint32_t wifiUpAt = 0;
bool otaBootCheckDone = false;
uint32_t lastOtaCheck = 0;
bool otaScreenShown = false; // an update attempt drew over the aircraft display

WiFiManagerParameter *pPostcode, *pLat, *pLon, *pRadius, *pBright;

// ---------- helpers ----------
static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }
#define GREY rgb(150, 150, 150)
#define DIM rgb(70, 70, 70)

static void setBacklight(int percent) {
  percent = constrain(percent, 5, 100);
  analogWrite(BACKLIGHT_PIN, map(percent, 0, 100, 0, 255));
}

// The built-in fonts are ASCII only, so drop anything else (accents etc).
static String ascii(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c >= 32 && c < 127) out += c;
  }
  return out;
}

static String thousands(long n) {
  String s = String(labs(n));
  for (int i = s.length() - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return n < 0 ? "-" + s : s;
}

static float haversineMi(float lat1, float lon1, float lat2, float lon2) {
  const float R = 3958.8f;
  float dLat = radians(lat2 - lat1), dLon = radians(lon2 - lon1);
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(radians(lat1)) * cosf(radians(lat2)) * sinf(dLon / 2) * sinf(dLon / 2);
  return 2 * R * atan2f(sqrtf(a), sqrtf(1 - a));
}

static float bearingDeg(float lat1, float lon1, float lat2, float lon2) {
  float dLon = radians(lon2 - lon1);
  float y = sinf(dLon) * cosf(radians(lat2));
  float x = cosf(radians(lat1)) * sinf(radians(lat2)) -
            sinf(radians(lat1)) * cosf(radians(lat2)) * cosf(dLon);
  float b = degrees(atan2f(y, x));
  return b < 0 ? b + 360 : b;
}

static const char *compass(float deg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return pts[((int)((deg + 22.5f) / 45.0f)) % 8];
}

static const Airline *findAirline(const String &callsign) {
  if (callsign.length() < 4) return nullptr;
  for (const Airline &a : AIRLINES)
    if (callsign.startsWith(a.icao)) return &a;
  return nullptr;
}

// Airline callsigns are three letters followed by a flight number.
static bool looksLikeAirline(const String &cs) {
  if (cs.length() < 4) return false;
  for (int i = 0; i < 3; i++)
    if (!isalpha(cs[i])) return false;
  return isdigit(cs[3]);
}

// ---------- config storage ----------
static void loadConfig() {
  prefs.begin("plane", true);
  cfg.postcode = prefs.getString("pc", "");
  cfg.pcResolved = prefs.getString("pcres", "");
  cfg.lat = prefs.getFloat("lat", cfg.lat);
  cfg.lon = prefs.getFloat("lon", cfg.lon);
  cfg.radiusNm = prefs.getInt("rad", cfg.radiusNm);
  cfg.brightness = prefs.getInt("bri", cfg.brightness);
  prefs.end();
}

static void saveConfig() {
  prefs.begin("plane", false);
  prefs.putString("pc", cfg.postcode);
  prefs.putString("pcres", cfg.pcResolved);
  prefs.putFloat("lat", cfg.lat);
  prefs.putFloat("lon", cfg.lon);
  prefs.putInt("rad", cfg.radiusNm);
  prefs.putInt("bri", cfg.brightness);
  prefs.end();
}

// ---------- HTTP ----------
static bool getJson(const String &url, JsonDocument &doc, const JsonDocument *filter = nullptr) {
  WiFiClientSecure client;
  client.setInsecure(); // public read-only data; skip certificate pinning
  HTTPClient http;
  http.setReuse(false); // one request per connection; servers reject HTTP/1.0
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("cyd-plane-display/1.0");
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP %d for %s\n", code, url.c_str());
    http.end();
    return false;
  }
  DeserializationError err;
  if (http.getSize() > 0) {
    // Known Content-Length: parse straight off the socket, no copy.
    err = filter ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, http.getStream());
  } else {
    // Chunked transfer: let HTTPClient de-chunk into a String first.
    String body = http.getString();
    err = filter ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, body);
  }
  http.end();
  if (err) Serial.printf("JSON error: %s\n", err.c_str());
  return !err;
}

static bool resolvePostcode() {
  String pc = cfg.postcode;
  pc.replace(" ", "");
  JsonDocument doc;
  if (!getJson("https://api.postcodes.io/postcodes/" + pc, doc)) return false;
  if (!doc["result"]["latitude"].is<float>()) return false;
  cfg.lat = doc["result"]["latitude"];
  cfg.lon = doc["result"]["longitude"];
  cfg.pcResolved = cfg.postcode;
  saveConfig();
  return true;
}

// Finds the closest airborne aircraft. Returns false only on a network/parse
// failure; an empty sky returns true with out.valid == false.
static bool fetchNearest(Plane &out) {
  // Both are readsb-style feeds with the same per-aircraft fields; adsb.lol
  // puts the list under "ac", adsb.fi under "aircraft".
  const String lat = String(cfg.lat, 4), lon = String(cfg.lon, 4), rad = String(cfg.radiusNm);
  const String URLS[] = {
      "https://api.adsb.lol/v2/point/" + lat + "/" + lon + "/" + rad,
      "https://opendata.adsb.fi/api/v2/lat/" + lat + "/lon/" + lon + "/dist/" + rad,
  };
  JsonDocument filter;
  for (const char *list : {"ac", "aircraft"}) {
    JsonObject f = filter[list][0].to<JsonObject>();
    for (const char *k : {"hex", "flight", "r", "t", "alt_baro", "gs", "baro_rate", "lat", "lon"})
      f[k] = true;
  }

  JsonDocument doc;
  const String *used = nullptr;
  for (const String &url : URLS) {
    doc.clear();
    if (getJson(url, doc, &filter)) {
      used = &url;
      break;
    }
  }
  if (!used) return false;

  JsonArray list = doc["ac"].is<JsonArray>() ? doc["ac"].as<JsonArray>()
                                             : doc["aircraft"].as<JsonArray>();
  out = Plane();
  float best = 1e9;
  for (JsonObject a : list) {
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
    if (!a["alt_baro"].is<float>()) continue; // "ground" or missing
    float d = haversineMi(cfg.lat, cfg.lon, a["lat"], a["lon"]);
    if (d >= best) continue;
    best = d;
    out.valid = true;
    out.hex = a["hex"] | "";
    out.callsign = a["flight"] | "";
    out.callsign.trim();
    out.reg = a["r"] | "";
    out.type = a["t"] | "";
    out.altFt = (long)a["alt_baro"].as<float>();
    out.speedKt = (int)(a["gs"] | 0.0f);
    out.vrate = a["baro_rate"] | 0;
    out.distMi = d;
    out.bearing = bearingDeg(cfg.lat, cfg.lon, a["lat"], a["lon"]);
  }
  Serial.printf("%s: %d aircraft, nearest %s at %.1f mi\n", used->c_str(), (int)list.size(),
                out.valid ? out.callsign.c_str() : "-", out.distMi);
  return true;
}

static void fetchRoute(const String &callsign) {
  route = Route();
  route.forCallsign = callsign;
  if (!looksLikeAirline(callsign)) return;
  JsonDocument doc;
  if (!getJson("https://api.adsbdb.com/v0/callsign/" + callsign, doc)) return;
  JsonObject fr = doc["response"]["flightroute"];
  if (fr.isNull()) return;
  route.airline = ascii(fr["airline"]["name"] | "");
  route.oIata = fr["origin"]["iata_code"] | "";
  route.oCity = ascii(fr["origin"]["municipality"] | "");
  route.dIata = fr["destination"]["iata_code"] | "";
  route.dCity = ascii(fr["destination"]["municipality"] | "");
  route.known = route.oIata.length() && route.dIata.length();
}

// ---------- drawing ----------
static void drawMessage(const char *title, const char *line1, const char *line2 = "",
                        const char *line3 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(title, W / 2, 60, 4);
  tft.setTextColor(GREY, TFT_BLACK);
  tft.drawString(line1, W / 2, 115, 2);
  tft.drawString(line2, W / 2, 140, 2);
  tft.drawString(line3, W / 2, 165, 2);
  // running firmware version, so a unit can be identified without a cable
  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(DIM, TFT_BLACK);
  tft.drawString(ota::versionLabel(), W - 6, H - 4, 2);
  tft.setTextDatum(MC_DATUM);
}

// Download progress bar for the update screen; redraws only when the percent changes.
static void drawProgress(int done, int total) {
  static int lastPct = -1;
  int pct = total > 0 ? (int)((int64_t)done * 100 / total) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  const int x = 40, y = 150, w = W - 80, h = 14;
  tft.drawRect(x, y, w, h, GREY);
  tft.fillRect(x + 2, y + 2, (w - 4) * pct / 100, h - 4, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(GREY, TFT_BLACK);
  tft.drawString(String(pct) + "%  ", W / 2, y + 32, 2);
}

// Draws text centred, dropping to the small font if it would overflow.
static void drawFit(const String &s, int x, int y, int maxW) {
  tft.drawString(s, x, y, tft.textWidth(s, 4) <= maxW ? 4 : 2);
}

static void drawStats(const Plane &p) {
  tft.fillRect(0, STATS_Y, W, H - STATS_Y, TFT_BLACK);
  tft.drawFastHLine(10, STATS_Y, W - 20, DIM);
  tft.setTextDatum(MC_DATUM);

  const int cols[] = {55, 160, 265};
  const char *labels[] = {"ALTITUDE", "SPEED", "DISTANCE"};
  tft.setTextColor(GREY, TFT_BLACK);
  for (int i = 0; i < 3; i++) tft.drawString(labels[i], cols[i], STATS_Y + 16, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String alt = thousands(p.altFt);
  tft.drawString(alt, cols[0], STATS_Y + 42, 4);
  // climb / descent marker next to the altitude
  int ax = cols[0] + tft.textWidth(alt, 4) / 2 + 9, ay = STATS_Y + 40;
  if (p.vrate > 300) tft.fillTriangle(ax - 5, ay + 5, ax + 5, ay + 5, ax, ay - 6, TFT_GREEN);
  if (p.vrate < -300) tft.fillTriangle(ax - 5, ay - 5, ax + 5, ay - 5, ax, ay + 6, TFT_ORANGE);

  tft.drawString(String(p.speedKt), cols[1], STATS_Y + 42, 4);
  tft.drawString(String(p.distMi, 1), cols[2], STATS_Y + 42, 4);

  tft.setTextColor(GREY, TFT_BLACK);
  tft.drawString("feet", cols[0], STATS_Y + 63, 2);
  tft.drawString("knots", cols[1], STATS_Y + 63, 2);
  tft.drawString(String("miles ") + compass(p.bearing), cols[2], STATS_Y + 63, 2);
}

static void drawPlane(const Plane &p) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  // Headline: airline in brand colour (or callsign/registration for others)
  const Airline *al = findAirline(p.callsign);
  String headline = al ? String(al->name)
                       : route.airline.length() ? route.airline
                       : p.callsign.length() ? p.callsign
                       : p.reg.length() ? p.reg
                                        : String("Unknown aircraft");
  tft.setTextColor(al ? rgb(al->r, al->g, al->b) : TFT_WHITE, TFT_BLACK);
  drawFit(headline, W / 2, 22, W - 16);

  // Sub-line: callsign, type, registration
  String sub = p.callsign;
  if (p.type.length()) sub += (sub.length() ? "   " : "") + p.type;
  if (p.reg.length() && p.reg != p.callsign) sub += (sub.length() ? "   " : "") + p.reg;
  tft.setTextColor(GREY, TFT_BLACK);
  tft.drawString(sub, W / 2, 50, 2);

  // Route
  if (route.known) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString(route.oIata, 78, 98, 4);
    tft.drawString(route.dIata, 242, 98, 4);
    tft.setTextSize(1);
    tft.fillRect(142, 96, 26, 4, GREY);
    tft.fillTriangle(168, 88, 168, 108, 182, 98, GREY);
    tft.setTextColor(GREY, TFT_BLACK);
    tft.drawString(route.oCity.substring(0, 18), 78, 140, 2);
    tft.drawString(route.dCity.substring(0, 18), 242, 140, 2);
  } else {
    tft.setTextColor(DIM, TFT_BLACK);
    tft.drawString("Route not available", W / 2, 110, 4);
  }

  drawStats(p);
}

static void drawStatusDot() {
  tft.fillCircle(W - 7, 7, 3, lastFetchOk ? TFT_BLACK : TFT_RED);
}

// ---------- setup portal ----------
static void onPortalStart(WiFiManager *) {
  drawMessage("Setup", "On your phone, join the WiFi network", "\"PlaneSpotter\"",
              "then follow the page that opens");
}

static void applyPortalParams() {
  cfg.postcode = pPostcode->getValue();
  cfg.postcode.trim();
  cfg.postcode.toUpperCase();
  if (cfg.postcode.length() == 0) {
    // manual coordinates only apply when no postcode is given
    cfg.lat = String(pLat->getValue()).toFloat();
    cfg.lon = String(pLon->getValue()).toFloat();
    cfg.pcResolved = "";
  }
  cfg.radiusNm = constrain(String(pRadius->getValue()).toInt(), 2, 50);
  cfg.brightness = constrain(String(pBright->getValue()).toInt(), 5, 100);
  saveConfig();
}

static void runPortal(bool onDemand) {
  ota::markValid(); // a unit waiting for new WiFi details must not roll back on power cycle
  wm.setConfigPortalTimeout(onDemand ? 300 : 0);
  bool ok = onDemand ? wm.startConfigPortal(AP_NAME) : wm.autoConnect(AP_NAME);
  applyPortalParams();
  if (onDemand || !ok) ESP.restart();
}

// ---------- firmware updates ----------
static void runOtaCheck(bool manual) {
  if (WiFi.status() != WL_CONNECTED) return;
  otaScreenShown = manual;
  if (manual) drawMessage("PlaneSpotter", "Checking for updates...");

  ota::Ui ui;
  ui.message = [](const char *title, const String &line) {
    otaScreenShown = true;
    drawMessage(title, line.c_str());
  };
  ui.progress = drawProgress;

  String newVersion;
  ota::Result r = ota::check(manual, ui, newVersion);
  switch (r) {
    case ota::UPDATED:
      drawMessage("Update complete", ("Restarting as version " + newVersion).c_str());
      delay(2000);
      ESP.restart();
      break;
    case ota::FAILED:
      if (otaScreenShown) {
        drawMessage("Update failed", ("Still running " + ota::versionLabel()).c_str(),
                    "Will try again later");
        delay(3000);
      }
      break;
    case ota::NO_RELEASE:
    case ota::UP_TO_DATE:
      if (manual) {
        drawMessage("Up to date", ota::versionLabel().c_str());
        delay(3000);
      }
      break;
  }
  if (otaScreenShown) {
    // put the aircraft display back: forget what is shown and poll straight away
    shown = Plane();
    lastPoll = millis() - POLL_MS;
    otaScreenShown = false;
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  tft.init();
  tft.setRotation(1);
  loadConfig();
  setBacklight(cfg.brightness);
  drawMessage("PlaneSpotter", "Connecting to WiFi...");

  pPostcode = new WiFiManagerParameter("pc", "UK postcode", cfg.postcode.c_str(), 10);
  pLat = new WiFiManagerParameter("lat", "Latitude (only if no postcode)",
                                  String(cfg.lat, 4).c_str(), 12);
  pLon = new WiFiManagerParameter("lon", "Longitude (only if no postcode)",
                                  String(cfg.lon, 4).c_str(), 12);
  pRadius = new WiFiManagerParameter("rad", "Search radius (nautical miles, 2-50)",
                                     String(cfg.radiusNm).c_str(), 4);
  pBright = new WiFiManagerParameter("bri", "Brightness (5-100)",
                                     String(cfg.brightness).c_str(), 4);
  wm.addParameter(pPostcode);
  wm.addParameter(pLat);
  wm.addParameter(pLon);
  wm.addParameter(pRadius);
  wm.addParameter(pBright);
  wm.setAPCallback(onPortalStart);
  wm.setTitle("PlaneSpotter setup");

  runPortal(false);
  wifiUpAt = millis();
  setBacklight(cfg.brightness);

  if (cfg.postcode.length() && cfg.postcode != cfg.pcResolved) {
    drawMessage("PlaneSpotter", "Looking up postcode...");
    if (!resolvePostcode()) {
      drawMessage("Postcode not found", cfg.postcode.c_str(),
                  "Hold the BOOT button for 3 seconds", "to try again");
      delay(8000);
    }
  }
  drawMessage("PlaneSpotter", "Scanning the sky...");
  lastPoll = millis() - POLL_MS; // poll straight away
}

void loop() {
  // BOOT button: hold 3 seconds to reopen the setup portal, short press to
  // check for a firmware update now.
  if (digitalRead(BOOT_BTN) == LOW) {
    uint32_t t0 = millis();
    while (digitalRead(BOOT_BTN) == LOW && millis() - t0 < 3000) delay(20);
    uint32_t held = millis() - t0;
    if (held >= 3000) runPortal(true);
    else if (held >= 50 && held < 1000) runOtaCheck(true);
  }

  // A freshly updated image is only pending until it has shown it works: one
  // successful fetch, or a minute of running. Then rollback is cancelled.
  if (everFetchedOk || millis() > OTA_VALID_AFTER_MS) ota::markValid();

  // Automatic update checks: once shortly after WiFi came up, then daily.
  if (WiFi.status() == WL_CONNECTED) {
    bool due = !otaBootCheckDone ? (millis() - wifiUpAt > OTA_BOOT_DELAY_MS)
                                 : (millis() - lastOtaCheck > OTA_PERIOD_MS);
    if (due) {
      otaBootCheckDone = true;
      lastOtaCheck = millis();
      runOtaCheck(false);
    }
  }

  if (millis() - lastPoll < POLL_MS) {
    delay(50);
    return;
  }
  lastPoll = millis();

  if (WiFi.status() != WL_CONNECTED) {
    lastFetchOk = false;
    drawStatusDot();
    WiFi.reconnect();
    return;
  }

  Plane p;
  lastFetchOk = fetchNearest(p);
  if (lastFetchOk) everFetchedOk = true;
  if (!lastFetchOk) {
    drawStatusDot(); // keep showing the last aircraft, flag the problem
    return;
  }

  if (!p.valid) {
    if (shown.valid || shown.hex != "none") {
      drawMessage("Clear skies", "No aircraft nearby right now");
      shown = Plane();
      shown.hex = "none";
    }
  } else if (p.hex != shown.hex || p.callsign != shown.callsign) {
    if (p.callsign != route.forCallsign) fetchRoute(p.callsign);
    drawPlane(p);
    shown = p;
  } else {
    drawStats(p); // same aircraft: refresh the numbers only
    shown = p;
  }
  drawStatusDot();
}
