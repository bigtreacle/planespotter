// LED dot-matrix look. Everything is drawn at grid resolution into a 16-bit
// TFT_eSprite with the built-in 5x7 font, then each sprite pixel becomes one
// pitch x pitch cell on the screen: a round dot in the pixel's colour with a
// dim ring of the same colour as glow, on black. Only cells whose colour
// changed since the last flush are pushed to the panel.
#include "renderer.h"
#include "ota.h"

namespace {

const int W = 320, H = 240;

// The classic font is 5x7 but 'g' and 'y' descend into an 8th row, so a text
// row occupies 8 px even though the layout spaces rows as 7 px glyphs.
const int ROW_H = 8;

// Control characters inside row text, drawn as hand-made glyphs because the
// 5x7 font has nothing suitable. Each has a width; every glyph (font chars
// included) is followed by a 1 px gap.
const char UP = '\x18', DOWN = '\x19', RIGHT = '\x1A', NARROW = '\x1C';

// Cell masks: 0 black, 1 glow, 2 full colour. Row-major, pitch x pitch.
const uint8_t MASK5[25] = {
    0, 1, 1, 1, 0,  //
    1, 2, 2, 2, 1,  //
    1, 2, 2, 2, 1,  //
    1, 2, 2, 2, 1,  //
    0, 1, 1, 1, 0,
};
// 4 px pitch: 3 px round dot, corners as glow, one black row/column as the gap.
const uint8_t MASK4[16] = {
    1, 2, 1, 0,  //
    2, 2, 2, 0,  //
    1, 2, 1, 0,  //
    0, 0, 0, 0,
};

const uint32_t SCROLL_STEP_MS = 125;  // ~8 columns per second
const uint32_t SCROLL_PAUSE_MS = 2000;

class MatrixRenderer : public Renderer {
 public:
  MatrixRenderer(TFT_eSPI &t, int c, int r) : tft(t), spr(&t), cols(c), rows(r) {
    pitch = W / cols;
    ox = (W - cols * pitch) / 2;
    oy = (H - rows * pitch) / 2;
    spr.setColorDepth(16);
    if (!spr.createSprite(cols, rows)) Serial.println("matrix: sprite allocation failed");
    spr.setTextFont(1);
    spr.setTextWrap(false);
    prev = new uint16_t[cols * rows];
    // Four text rows of 7 px, spread over the grid height.
    int gap = (rows - 4 * 7) / 4; // the fourth gap is split as top/bottom margin
    int top = (rows - 4 * 7 - 3 * gap) / 2;
    for (int i = 0; i < 4; i++) rowY[i] = top + i * (7 + gap);
    tft.setSwapBytes(true);
    invalidate();
    Serial.printf("matrix %dx%d: pitch %d px, offset %d,%d, rows at %d/%d/%d/%d\n", cols, rows,
                  pitch, ox, oy, rowY[0], rowY[1], rowY[2], rowY[3]);
  }

  ~MatrixRenderer() override {
    spr.deleteSprite();
    delete[] prev;
  }

  void setBrightness(int percent) override {
    // Dim the dots as well as the backlight; never below a quarter so text stays legible.
    bright = 0.25f + 0.75f * constrain(percent, 5, 100) / 100.0f;
    cacheColor = 0xFFFF;
    invalidate();
  }

  void drawMessage(const char *title, const char *line1, const char *line2,
                   const char *line3) override {
    scrolling = false;
    spr.fillSprite(TFT_BLACK);
    drawCentred(1, title, AMBER);
    // Word-wrap the body lines into as many 9 px rows as fit above the version label.
    const int lineStep = 9, bodyTop = 10, maxBody = (rows - bodyTop - 8) / lineStep;
    const int maxChars = (cols + 1) / 6;
    int n = 0;
    for (const char *src : {line1, line2, line3}) {
      String rest = src;
      rest.trim();
      while (rest.length() && n < maxBody) {
        String line;
        if ((int)rest.length() <= maxChars) {
          line = rest;
          rest = "";
        } else {
          int cut = rest.lastIndexOf(' ', maxChars);
          if (cut <= 0) cut = maxChars; // a single word too long for the row
          line = rest.substring(0, cut);
          rest = rest.substring(cut);
          rest.trim();
        }
        drawCentred(bodyTop + n++ * lineStep, line, WARM);
      }
    }
    String v = ota::versionLabel();
    drawText(cols - textW(v), rows - 8, v, DIMWARM);
    flush(true, "message");
  }

  void drawProgress(int done, int total) override {
    int pct = total > 0 ? (int)((int64_t)done * 100 / total) : 0;
    if (pct == lastPct) return;
    lastPct = pct;
    const int y = rows - 17, barX = 2, barW = cols - 4;
    spr.fillRect(0, y, cols, rows - y, TFT_BLACK); // bar band, also covers the version label
    spr.drawRect(barX, y, barW, 3, DIMWARM);
    spr.fillRect(barX, y, barW * pct / 100, 3, WARM);
    drawCentred(rows - 8, String(pct) + "%", WARM);
    flush(false, nullptr);
  }

  void drawPlane(const Plane &p, const Route &route, const Airline *al) override {
    spr.fillSprite(TFT_BLACK);

    // Row 1: airline in brand colour; scrolls if it does not fit.
    String headline = al ? String(al->name)
                         : route.airline.length() ? route.airline
                         : p.callsign.length() ? p.callsign
                         : p.reg.length() ? p.reg
                                          : String("Unknown");
    scrollColor = al ? tft.color565(al->r, al->g, al->b) : WARM;
    scrollText = headline;
    scrollW = textW(headline);
    scrolling = scrollW > cols;
    if (scrolling) {
      scrollX = 0;
      scrollDir = -1;
      scrollNext = millis() + SCROLL_PAUSE_MS;
      drawText(0, rowY[0], headline, scrollColor);
    } else {
      drawCentred(rowY[0], headline, scrollColor);
    }

    // Row 2: callsign and type, dropping the type if the row is too narrow.
    String id = p.callsign.length() ? p.callsign : p.reg.length() ? p.reg : p.type;
    String withType = p.type.length() && p.type != id ? id + NARROW + p.type : id;
    drawCentred(rowY[1], fit({withType, id}), WARM);

    // Row 3: route, or the registration when the route is unknown.
    if (route.known) drawCentred(rowY[2], route.oIata + NARROW + RIGHT + NARROW + route.dIata, TFT_WHITE);
    else drawCentred(rowY[2], p.reg.length() ? p.reg : String("no route"), TFT_WHITE);

    drawStatsRow(p);
    flush(true, "plane");
  }

  void drawStats(const Plane &p) override {
    drawStatsRow(p);
    flush(true, "stats");
  }

  void drawStatusDot(bool fetchOk) override {
    spr.drawPixel(cols - 1, 0, fetchOk ? TFT_BLACK : TFT_RED);
    flush(false, nullptr);
  }

  void dump() override {
    for (int y = 0; y < rows; y++) {
      char line[96];
      for (int x = 0; x < cols; x++) line[x] = spr.readPixel(x, y) ? '#' : '.';
      line[cols] = 0;
      Serial.println(line);
    }
  }

  void tick() override {
    if (!scrolling || (int32_t)(millis() - scrollNext) < 0) return;
    scrollX += scrollDir;
    int minX = cols - scrollW;
    if (scrollX <= minX) {
      scrollX = minX;
      scrollDir = 1;
      scrollNext = millis() + SCROLL_PAUSE_MS;
    } else if (scrollX >= 0) {
      scrollX = 0;
      scrollDir = -1;
      scrollNext = millis() + SCROLL_PAUSE_MS;
    } else {
      scrollNext = millis() + SCROLL_STEP_MS;
    }
    spr.fillRect(0, rowY[0], cols, ROW_H, TFT_BLACK);
    drawText(scrollX, rowY[0], scrollText, scrollColor);
    flush(false, nullptr);
  }

 private:
  TFT_eSPI &tft;
  TFT_eSprite spr;
  int cols, rows, pitch, ox, oy;
  int rowY[4];
  uint16_t *prev;        // colour of each cell as last pushed to the panel
  float bright = 1.0f;
  int lastPct = -1;

  // scrolling headline
  bool scrolling = false;
  String scrollText;
  uint16_t scrollColor = 0;
  int scrollW = 0, scrollX = 0, scrollDir = -1;
  uint32_t scrollNext = 0;

  // one-entry cache of the last cell bitmap, since neighbouring dots share a colour
  uint16_t cacheColor = 0xFFFF;
  uint16_t cell[25];

  const uint16_t WARM = tft.color565(255, 230, 200);
  const uint16_t DIMWARM = tft.color565(110, 95, 80);
  const uint16_t AMBER = tft.color565(255, 180, 0);

  // ---- text at grid resolution ----

  static int glyphW(char c) {
    switch (c) {
      case UP:
      case DOWN: return 3;
      case RIGHT: return 5;
      case NARROW: return 2;
      default: return 5;
    }
  }

  static int textW(const String &s) {
    if (!s.length()) return 0;
    int w = 0;
    for (size_t i = 0; i < s.length(); i++) w += glyphW(s[i]) + 1;
    return w - 1;
  }

  // First candidate that fits the grid width, else the last one.
  int fitIndex(std::initializer_list<String> cands) {
    int i = 0;
    for (const String &c : cands) {
      if (textW(c) <= cols) return i;
      i++;
    }
    return i - 1;
  }
  String fit(std::initializer_list<String> cands) {
    int want = fitIndex(cands), i = 0;
    for (const String &c : cands)
      if (i++ == want) return c;
    return "";
  }

  void drawGlyph(int x, int y, char c, uint16_t color) {
    switch (c) {
      case UP:
      case DOWN: {
        // 3 x 5 arrow, vertically centred in the 7 px row
        int tip = c == UP ? y + 1 : y + 5, dir = c == UP ? 1 : -1;
        spr.drawPixel(x + 1, tip, color);
        spr.drawFastHLine(x, tip + dir, 3, color);
        spr.drawFastVLine(x + 1, y + 1, 5, color);
        break;
      }
      case RIGHT: {
        // 5 x 5 arrow, vertically centred
        int cy = y + 3;
        spr.drawFastHLine(x, cy, 5, color);
        spr.drawPixel(x + 3, cy - 1, color);
        spr.drawPixel(x + 3, cy + 1, color);
        spr.drawPixel(x + 2, cy - 2, color);
        spr.drawPixel(x + 2, cy + 2, color);
        break;
      }
      case NARROW: break;
      default: spr.drawChar(x, y, c, color, color, 1);
    }
  }

  void drawText(int x, int y, const String &s, uint16_t color) {
    for (size_t i = 0; i < s.length(); i++) {
      char c = s[i];
      uint16_t col = c == UP ? TFT_GREEN : c == DOWN ? TFT_ORANGE : color;
      if (x + glyphW(c) > 0 && x < cols) drawGlyph(x, y, c, col);
      x += glyphW(c) + 1;
    }
  }

  void drawCentred(int y, const String &s, uint16_t color) {
    drawText((cols - textW(s)) / 2, y, s, color);
  }

  // Row 4: altitude (with climb/descent arrow), speed, distance and direction,
  // in decreasing detail until it fits.
  void drawStatsRow(const Plane &p) {
    String arrow = p.vrate > 300 ? String(UP) : p.vrate < -300 ? String(DOWN) : String();
    String alt1 = thousands(p.altFt) + arrow, alt0 = String(p.altFt) + arrow;
    String spd = String(p.speedKt);
    String dist1 = String(p.distMi, 1), dist0 = String((int)lroundf(p.distMi));
    String dir = compass(p.bearing);
    String row = fit({
        alt1 + NARROW + spd + "kt" + NARROW + dist1 + "mi" + NARROW + dir,
        alt1 + NARROW + spd + NARROW + dist1 + dir,
        alt0 + NARROW + spd + NARROW + dist1 + dir,
        alt0 + NARROW + spd + NARROW + dist0 + dir,
    });
    spr.fillRect(0, rowY[3], cols, ROW_H, TFT_BLACK);
    drawCentred(rowY[3], row, AMBER);
  }

  // ---- dots on the panel ----

  uint16_t scale565(uint16_t c, float f) {
    int r = ((c >> 11) & 0x1F) * f + 0.5f, g = ((c >> 5) & 0x3F) * f + 0.5f, b = (c & 0x1F) * f + 0.5f;
    return (r << 11) | (g << 5) | b;
  }

  void drawCell(int cx, int cy, uint16_t color) {
    if (color != cacheColor) {
      cacheColor = color;
      uint16_t full = scale565(color, bright), glow = scale565(color, bright * 0.3f);
      const uint8_t *mask = pitch == 5 ? MASK5 : MASK4;
      for (int i = 0; i < pitch * pitch; i++)
        cell[i] = mask[i] == 2 ? full : mask[i] == 1 ? glow : TFT_BLACK;
    }
    tft.pushImage(ox + cx * pitch, oy + cy * pitch, pitch, pitch, cell);
  }

  // Blank the panel and forget what was on it, so the next flush redraws every lit dot.
  void invalidate() {
    tft.fillScreen(TFT_BLACK);
    memset(prev, 0, cols * rows * sizeof(uint16_t));
  }

  // Push every cell whose colour differs from the last flush.
  void flush(bool log, const char *what) {
    uint32_t t0 = millis();
    int changed = 0, lit = 0;
    for (int y = 0; y < rows; y++) {
      for (int x = 0; x < cols; x++) {
        uint16_t c = spr.readPixel(x, y);
        if (c) lit++;
        int i = y * cols + x;
        if (c == prev[i]) continue;
        prev[i] = c;
        drawCell(x, y, c);
        changed++;
      }
    }
    if (log)
      Serial.printf("matrix %dx%d: %s redraw, %d of %d dots changed, %lu ms\n", cols, rows, what,
                    changed, lit, (unsigned long)(millis() - t0));
  }
};

}  // namespace

Renderer *newMatrixRenderer(TFT_eSPI &tft, int cols, int rows) {
  return new MatrixRenderer(tft, cols, rows);
}
