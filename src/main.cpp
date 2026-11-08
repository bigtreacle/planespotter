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

#include "diag.h"
#include "ota.h"
#include "touch.h"
#include "model.h"
#include "renderer.h"

// ---------- constants ----------
static const char *AP_NAME = "PlaneSpotter";
static const int BOOT_BTN = 0;
static const int BACKLIGHT_PIN = 21;
static const uint32_t POLL_MS = 30000;
static const int NEARBY_MAX = 4;                    // aircraft kept for tap cycling
static const uint32_t SELECT_HOLD_MS = 30000;       // after the last tap, drift back to nearest
static const int ROUTE_CACHE = 8;                   // routes remembered, by callsign
static const uint32_t OTA_BOOT_DELAY_MS = 20000;            // after WiFi is up
static const uint32_t OTA_PERIOD_MS = 24UL * 60 * 60 * 1000; // then daily
static const uint32_t OTA_RETRY_MS = 10UL * 60 * 1000;      // after a check that could not complete
static const uint32_t OTA_VALID_AFTER_MS = 60000;           // uptime that counts as "works"
// Longest legitimate blocking stretch in loop() is a few chained 12 s HTTP
// timeouts; the firmware download feeds the watchdog as it goes.
static const uint32_t WATCHDOG_S = 120;
// The core lets a TLS handshake run for two minutes before giving up; on a
// bad connection that is a frozen display. Cap it so every network step is
// bounded well inside the watchdog.
static const uint32_t TLS_HANDSHAKE_S = 15;

// ---------- airline brand colours (by ICAO callsign prefix) ----------
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
  int style = STYLE_CRISP; // see renderer.h
  bool flip = false;       // rotate the display 180 degrees (case mounted upside down)
};

TFT_eSPI tft;
Renderer *renderer = nullptr;
Preferences prefs;
WiFiManager wm;
Config cfg;
Plane shown;
Plane nearby[NEARBY_MAX]; // sorted by distance, nearest first
int nearbyCount = 0;
int sel = 0;              // index into nearby[] that is on screen
uint32_t lastTapAt = 0;
Route routes[ROUTE_CACHE]; // most recently used first
bool preferFallback = false; // bench: try the second data source first
uint32_t lastPoll = 0;
bool lastFetchOk = true;
bool everFetchedOk = false;
uint32_t wifiUpAt = 0;
uint32_t nextOtaCheck = 0; // millis() at which the next automatic check is due
bool otaScreenShown = false; // an update attempt drew over the aircraft display

WiFiManagerParameter *pPostcode, *pLat, *pLon, *pRadius, *pBright, *pStyle, *pFlip;

// ---------- helpers ----------
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
  cfg.style = prefs.getInt("style", cfg.style);
  cfg.flip = prefs.getBool("flip", cfg.flip);
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
  prefs.putInt("style", cfg.style);
  prefs.putBool("flip", cfg.flip);
  prefs.end();
}

// ---------- HTTP ----------
// Body reading. The core's secure client leaves its socket non-blocking and
// reports "no data yet" as an error, which NetworkClient::readBytes treats as
// the end; HTTPClient works around that by looping with no deadline, which is
// how a server stalling mid-chunk froze the display (adsb.fi, 2026-11-08,
// found by the watchdog's backtrace). These helpers poll instead, with an
// inactivity timeout and an overall deadline.
static const uint32_t BODY_IDLE_MS = 12000;

// One byte, or -1 after BODY_IDLE_MS with nothing (or when the deadline passes).
static int readByteWithin(Stream &in, uint32_t deadline) {
  uint32_t idleUntil = millis() + BODY_IDLE_MS;
  while ((int32_t)(millis() - deadline) < 0 && (int32_t)(millis() - idleUntil) < 0) {
    if (in.available() > 0) {
      int c = in.read();
      if (c >= 0) return c;
    }
    delay(2);
  }
  return -1;
}

static bool readLineWithin(Stream &in, String &line, uint32_t deadline) {
  line = "";
  for (;;) {
    int c = readByteWithin(in, deadline);
    if (c < 0) return false;
    if (c == '\n') return true;
    if (c != '\r' && line.length() < 64) line += (char)c;
  }
}

// Reads a chunked body into out; false on timeout or a malformed chunk.
static bool readChunked(NetworkClient &in, String &out, uint32_t deadlineMs) {
  uint32_t deadline = millis() + deadlineMs;
  String line;
  while (readLineWithin(in, line, deadline)) {
    if (!line.length()) continue; // blank line between chunks
    size_t size = strtoul(line.c_str(), nullptr, 16);
    if (size == 0) return true;   // final chunk; trailers, if any, are ignored
    if (!out.reserve(out.length() + size)) return false;
    for (size_t got = 0; got < size;) {
      // read() with a length is fine once available() says data is there
      uint32_t idleUntil = millis() + BODY_IDLE_MS;
      int avail = 0;
      while ((avail = in.available()) <= 0) {
        if ((int32_t)(millis() - deadline) >= 0 || (int32_t)(millis() - idleUntil) >= 0) return false;
        delay(2);
      }
      char buf[512];
      size_t want = min((size_t)avail, min(sizeof buf, size - got));
      int n = in.read((uint8_t *)buf, want); // NetworkClient::read, not Stream
      if (n <= 0) return false;
      out.concat(buf, n);
      got += n;
    }
  }
  return false;
}

static bool getJson(const String &url, JsonDocument &doc, const JsonDocument *filter = nullptr) {
  WiFiClientSecure client;
  client.setInsecure(); // public read-only data; skip certificate pinning
  client.setHandshakeTimeout(TLS_HANDSHAKE_S);
  HTTPClient http;
  http.setReuse(false); // one request per connection; servers reject HTTP/1.0
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("cyd-plane-display/1.0");
  const char *wanted[] = {"Transfer-Encoding"};
  http.collectHeaders(wanted, 1);
  if (!http.begin(client, url)) return false;
  LOG("GET %s", url.c_str()); // breadcrumb: a stall shows up as the last line before a reset
  int code = http.GET();
  if (code != 200) {
    LOG("HTTP %d for %s\n", code, url.c_str());
    http.end();
    return false;
  }
  DeserializationError err;
  if (http.header("Transfer-Encoding").equalsIgnoreCase("chunked")) {
    // De-chunk into a String ourselves, with a deadline (see readChunked).
    String body;
    if (!readChunked(http.getStream(), body, 20000)) {
      LOG("body read timed out for %s", url.c_str());
      http.end();
      return false;
    }
    err = filter ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, body);
  } else {
    // Content-Length or close-delimited: parse straight off the socket. Each
    // read is bounded by the client timeout, so a stall ends in a parse error.
    err = filter ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
                 : deserializeJson(doc, http.getStream());
  }
  http.end();
  if (err) LOG("JSON error: %s", err.c_str());
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

// Insert p into nearby[] keeping it sorted by distance, nearest first.
static void insertNearby(const Plane &p) {
  int i = nearbyCount < NEARBY_MAX ? nearbyCount++ : NEARBY_MAX - 1;
  if (i == NEARBY_MAX - 1 && nearby[i].valid && nearby[i].distMi <= p.distMi) return;
  while (i > 0 && nearby[i - 1].distMi > p.distMi) {
    nearby[i] = nearby[i - 1];
    i--;
  }
  nearby[i] = p;
}

// Fills nearby[] with the closest airborne aircraft. Returns false only on a
// network/parse failure; an empty sky returns true with nearbyCount == 0.
static bool fetchNearest() {
  // Both are readsb-style feeds with the same per-aircraft fields; adsb.lol
  // puts the list under "ac", adsb.fi under "aircraft".
  const String lat = String(cfg.lat, 4), lon = String(cfg.lon, 4), rad = String(cfg.radiusNm);
  String URLS[] = {
      "https://api.adsb.lol/v2/point/" + lat + "/" + lon + "/" + rad,
      "https://opendata.adsb.fi/api/v2/lat/" + lat + "/lon/" + lon + "/dist/" + rad,
  };
  if (preferFallback) std::swap(URLS[0], URLS[1]); // bench: serial "fallback"
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
  nearbyCount = 0;
  for (JsonObject a : list) {
    if (!a["lat"].is<float>() || !a["lon"].is<float>()) continue;
    if (!a["alt_baro"].is<float>()) continue; // "ground" or missing
    float d = haversineMi(cfg.lat, cfg.lon, a["lat"], a["lon"]);
    if (nearbyCount == NEARBY_MAX && d >= nearby[NEARBY_MAX - 1].distMi) continue;
    Plane p;
    p.valid = true;
    p.hex = a["hex"] | "";
    p.callsign = a["flight"] | "";
    p.callsign.trim();
    p.reg = a["r"] | "";
    p.type = a["t"] | "";
    p.altFt = (long)a["alt_baro"].as<float>();
    p.speedKt = (int)(a["gs"] | 0.0f);
    p.vrate = a["baro_rate"] | 0;
    p.distMi = d;
    p.bearing = bearingDeg(cfg.lat, cfg.lon, a["lat"], a["lon"]);
    insertNearby(p);
  }
  LOG("%s: %d aircraft, %d kept, nearest %s at %.1f mi, heap %lu", used->c_str(),
      (int)list.size(), nearbyCount, nearbyCount ? nearby[0].callsign.c_str() : "-",
      nearbyCount ? nearby[0].distMi : 0.0f, (unsigned long)ESP.getFreeHeap());
  return true;
}

static void fetchRoute(Route &route, const String &callsign) {
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

// The route for a callsign, from the cache or fetched. Cycling between a few
// aircraft must not hit adsbdb on every tap.
static const Route &routeFor(const String &callsign) {
  int i = 0;
  while (i < ROUTE_CACHE && routes[i].forCallsign != callsign) i++;
  if (i == ROUTE_CACHE) {
    i = ROUTE_CACHE - 1; // evict the least recently used
    fetchRoute(routes[i], callsign);
  }
  // move to front
  Route r = routes[i];
  for (; i > 0; i--) routes[i] = routes[i - 1];
  routes[0] = r;
  return routes[0];
}

// ---------- drawing ----------
// Thin wrappers over the active Renderer (see renderer.h).
static void drawMessage(const char *title, const char *line1, const char *line2 = "",
                        const char *line3 = "") {
  renderer->drawMessage(title, line1, line2, line3);
}

static void drawProgress(int done, int total) { renderer->drawProgress(done, total); }

static void drawStatusDot() { renderer->drawStatusDot(lastFetchOk); }

// Full redraw for a new aircraft, timed so the styles can be compared on serial.
static void drawPlane(const Plane &p) {
  uint32_t t0 = millis();
  renderer->drawPlane(p, routeFor(p.callsign), findAirline(p.callsign));
  LOG("style %d: full redraw of %s took %lu ms\n", cfg.style, p.callsign.c_str(),
                (unsigned long)(millis() - t0));
}

static void drawStats(const Plane &p) { renderer->drawStats(p); }

static void drawSelection() { renderer->drawSelection(sel, nearbyCount); }

// Puts nearby[sel] on screen: a full redraw if it is a different aircraft
// from the one showing, otherwise just the numbers.
static void showSelected() {
  if (nearbyCount == 0) {
    if (shown.valid || shown.hex != "none") {
      drawMessage("Clear skies", "No aircraft nearby right now");
      shown = Plane();
      shown.hex = "none";
    }
    return;
  }
  const Plane &p = nearby[sel];
  if (p.hex != shown.hex || p.callsign != shown.callsign) drawPlane(p);
  else drawStats(p);
  shown = p;
  drawSelection();
  drawStatusDot();
}

// Step to the next nearby aircraft on a tap.
static void onTap() {
  if (nearbyCount < 2) return;
  sel = (sel + 1) % nearbyCount;
  lastTapAt = millis();
  LOG("tap: showing %d of %d (%s)", sel + 1, nearbyCount, nearby[sel].callsign.c_str());
  showSelected();
}

// (Re)creates the renderer for the current style and forces a full redraw.
static void rebuildRenderer() {
  delete renderer;
  renderer = makeRenderer(cfg.style, tft);
  renderer->setBrightness(cfg.brightness);
  shown = Plane(); // force a redraw on the next poll
}

// Switches display style; takes effect on the next redraw.
static void setStyle(int style) {
  if (renderer && style == cfg.style) return;
  cfg.style = style;
  rebuildRenderer();
}

// Applies the screen orientation. Rotation 1 is landscape with the USB port
// on the right; 3 is the same turned 180 degrees for a case that mounts the
// board the other way up.
static void applyFlip() {
  tft.setRotation(cfg.flip ? 3 : 1);
  tft.fillScreen(TFT_BLACK);
  if (renderer) rebuildRenderer(); // the matrix style caches what is on screen
}

// ---------- setup portal ----------
static void onPortalStart(WiFiManager *) {
  // A unit sitting here waiting for new WiFi details may be power-cycled at
  // any time; that must not roll a fresh update back.
  ota::markValid();
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
  cfg.style = constrain((int)String(pStyle->getValue()).toInt(), (int)STYLE_CRISP, (int)STYLE_MATRIX_80);
  bool flip = String(pFlip->getValue()).toInt() != 0;
  saveConfig();
  setStyle(cfg.style);
  if (flip != cfg.flip) {
    cfg.flip = flip;
    saveConfig();
    applyFlip();
  }
}

static void runPortal(bool onDemand) {
  wm.setConfigPortalTimeout(onDemand ? 300 : 0);
  diag::watchdogPause(); // waiting for a person is not a hang
  bool ok = onDemand ? wm.startConfigPortal(AP_NAME) : wm.autoConnect(AP_NAME);
  diag::watchdogResume();
  applyPortalParams();
  if (onDemand || !ok) ESP.restart();
}

// ---------- firmware updates ----------
// Returns the outcome so the caller can decide when to check again.
static ota::Result runOtaCheck(bool manual) {
  if (WiFi.status() != WL_CONNECTED) return ota::FAILED;
  otaScreenShown = manual;
  if (manual) drawMessage("PlaneSpotter", "Checking for updates...");

  ota::Ui ui;
  ui.message = [](const char *title, const String &line) {
    otaScreenShown = true;
    drawMessage(title, line.c_str());
  };
  ui.progress = [](int done, int total) {
    diag::feed(); // a slow download is progress, not a hang
    drawProgress(done, total);
  };

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
  return r;
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  diag::begin(); // reports why the last boot ended before anything else logs
  LOG("PlaneSpotter %s (%s)", ota::versionLabel().c_str(), BOARD_VARIANT);
  diag::watchdogStart(WATCHDOG_S);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  tft.init();
  touch::begin();
  loadConfig();
  applyFlip();
  ota::begin();
  setBacklight(cfg.brightness);
  setStyle(cfg.style);
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
  pStyle = new WiFiManagerParameter(
      "style", "Display style: 0 = crisp, 1 = LED matrix 64x48, 2 = LED matrix 80x60",
      String(cfg.style).c_str(), 2, "type='number' min='0' max='2'");
  wm.addParameter(pPostcode);
  wm.addParameter(pLat);
  wm.addParameter(pLon);
  wm.addParameter(pRadius);
  wm.addParameter(pBright);
  pFlip = new WiFiManagerParameter("flip", "Flip screen: 0 = no, 1 = upside down (180 degrees)",
                                   cfg.flip ? "1" : "0", 2, "type='number' min='0' max='1'");
  wm.addParameter(pStyle);
  wm.addParameter(pFlip);
  wm.setAPCallback(onPortalStart);
  wm.setTitle("PlaneSpotter setup");

  runPortal(false);
  wifiUpAt = millis();
  nextOtaCheck = wifiUpAt + OTA_BOOT_DELAY_MS;
  setBacklight(cfg.brightness);
  renderer->setBrightness(cfg.brightness);

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

// Serial commands for a bench unit: "style N" switches display style without
// the portal (saved like a portal change); "flip N"; "dump" prints the screen
// as text; "diag" prints the boot counters and the log from before the last
// crash or watchdog reset; "hang" and "crash" provoke those two on purpose.
static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 32) line += c;
      continue;
    }
    if (line.startsWith("style ")) {
      int style = constrain((int)line.substring(6).toInt(), (int)STYLE_CRISP, (int)STYLE_MATRIX_80);
      setStyle(style);
      saveConfig();
      lastPoll = millis() - POLL_MS; // redraw straight away
      LOG("style set to %d\n", style);
    } else if (line.startsWith("flip ")) {
      cfg.flip = line.substring(5).toInt() != 0;
      saveConfig();
      applyFlip();
      lastPoll = millis() - POLL_MS;
      LOG("flip set to %d\n", cfg.flip);
    } else if (line == "dump") {
      renderer->dump();
    } else if (line == "diag") {
      diag::report();
    } else if (line == "fallback") {
      preferFallback = !preferFallback;
      LOG("fallback source first: %d", preferFallback);
      lastPoll = millis() - POLL_MS;
    } else if (line == "tap") {
      onTap(); // what a touch does, for a bench unit without a finger on it
    } else if (line == "hang") {
      diag::testHang(); // does not return; the watchdog reboots the board
    } else if (line == "crash") {
      diag::testCrash(); // does not return; panic, core dump, reboot
    }
    line = "";
  }
}

void loop() {
  diag::feed();
  handleSerial();
  if (touch::tapped() && !otaScreenShown) onTap();

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

  // Automatic update checks: shortly after WiFi came up, then daily. A check
  // that could not complete (WiFi or the network down at that moment) is
  // retried after a short delay rather than in a day's time, so a unit
  // powered on before the router is ready still updates promptly.
  if (WiFi.status() == WL_CONNECTED && (int32_t)(millis() - nextOtaCheck) >= 0) {
    ota::Result r = runOtaCheck(false);
    nextOtaCheck = millis() + (r == ota::FAILED ? OTA_RETRY_MS : OTA_PERIOD_MS);
  }

  if (millis() - lastPoll < POLL_MS) {
    renderer->tick(); // matrix-style scrolling; returns at once when idle
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

  lastFetchOk = fetchNearest();
  if (lastFetchOk) everFetchedOk = true;
  if (!lastFetchOk) {
    drawStatusDot(); // keep showing the last aircraft, flag the problem
    return;
  }

  // Keep following the aircraft that was selected by tap, wherever it now
  // sits in the list; fall back to the nearest when it has gone or the
  // selection has aged out.
  if (sel > 0 && millis() - lastTapAt > SELECT_HOLD_MS) sel = 0;
  if (sel > 0) {
    int i = 0;
    while (i < nearbyCount && nearby[i].hex != shown.hex) i++;
    sel = i < nearbyCount ? i : 0;
  }
  showSelected();
}
