// The original full-resolution layout, moved here unchanged from main.cpp.
#include "renderer.h"
#include "ota.h"

namespace {

const int W = 320, H = 240;
const int STATS_Y = 168;  // top of the bottom stats band

#define GREY rgb(150, 150, 150)
#define DIM rgb(70, 70, 70)

class CrispRenderer : public Renderer {
 public:
  explicit CrispRenderer(TFT_eSPI &t) : tft(t) {}

  void drawMessage(const char *title, const char *line1, const char *line2,
                   const char *line3) override {
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
  void drawProgress(int done, int total) override {
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

  void drawStats(const Plane &p) override {
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

  void drawPlane(const Plane &p, const Route &route, const Airline *al) override {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);

    // Headline: airline in brand colour (or callsign/registration for others)
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

  void drawStatusDot(bool fetchOk) override {
    tft.fillCircle(W - 7, 7, 3, fetchOk ? TFT_BLACK : TFT_RED);
  }

 private:
  TFT_eSPI &tft;
  int lastPct = -1;

  uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return tft.color565(r, g, b); }

  // Draws text centred, dropping to the small font if it would overflow.
  void drawFit(const String &s, int x, int y, int maxW) {
    tft.drawString(s, x, y, tft.textWidth(s, 4) <= maxW ? 4 : 2);
  }
};

}  // namespace

Renderer *newCrispRenderer(TFT_eSPI &tft) { return new CrispRenderer(tft); }
