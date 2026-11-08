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
const int MAX_SCROLL = 8;             // rows that may scroll at once (a message screen has at most 6)

// A row of text that is wider than the grid and sweeps back and forth.
struct Scroller {
  String text;
  uint16_t color;
  int y, w, x, dir;
  uint32_t next;
};

// Shorter wording for the status messages main.cpp uses, which were written
// for the crisp layout. Applied as substring replacements, in this order.
const char *const CONDENSE[][2] = {
    {"On your phone, join the WiFi network", "Join WiFi"},
    {"\"PlaneSpotter\"", "PlaneSpotter"},
    {"then follow the page that opens", "on phone"},
    {"Connecting to WiFi...", "Connecting WiFi"},
    {"Looking up postcode...", "Finding postcode"},
    {"Scanning the sky...", "Scanning sky"},
    {"Checking for updates...", "Checking updates"},
    {"Update complete", "Updated"},
    {"Restarting as version ", "Restarting v"},
    {"Still running ", "Running "},
    {"Will try again later", "Retry later"},
    {"Hold the BOOT button for 3 seconds", "Hold BOOT"},
    {"to try again", "to retry"},
    {"No aircraft nearby right now", "No planes nearby"},
};

String condense(const char *src) {
  String s = src;
  for (auto &pair : CONDENSE) s.replace(pair[0], pair[1]);
  s.trim();
  return s;
}

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

  // Title and body are word-wrapped into 9 px rows; a row that is still too
  // wide (a single long word) scrolls. The version label is only shown when a
  // row is left for it.
  void drawMessage(const char *title, const char *line1, const char *line2,
                   const char *line3) override {
    nScroll = 0;
    spr.fillSprite(TFT_BLACK);
    const int lineStep = 9, top = 1, maxRows = (rows - top) / lineStep;
    String text[8];
    uint16_t color[8];
    int n = 0;
    auto add = [&](const char *src, uint16_t c) {
      String rest = condense(src);
      while (rest.length() && n < maxRows) {
        // longest prefix of whole words that fits; a single long word is its own row
        int cut = rest.length();
        while (textW(rest.substring(0, cut)) > cols) {
          int sp = rest.lastIndexOf(' ', cut - 1);
          if (sp <= 0) break;
          cut = sp;
        }
        text[n] = rest.substring(0, cut);
        color[n++] = c;
        rest = rest.substring(cut);
        rest.trim();
      }
    };
    add(title, AMBER);
    for (const char *l : {line1, line2, line3}) add(l, WARM);
    for (int i = 0; i < n; i++) placeRow(top + i * lineStep, text[i], color[i]);
    if (n < maxRows) {
      String v = ota::versionLabel();
      drawText(cols - textW(v), rows - 8, v, DIMWARM);
    }
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
    nScroll = 0;
    String headline = al ? String(al->name)
                         : route.airline.length() ? route.airline
                         : p.callsign.length() ? p.callsign
                         : p.reg.length() ? p.reg
                                          : String("Unknown");
    placeRow(rowY[0], headline, al ? tft.color565(al->r, al->g, al->b) : WARM);

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

  void drawSelection(int index, int count) override {
    // Dots along the top-left edge, two apart; the text rows start lower.
    for (int i = 0; i < 4; i++) {
      uint16_t c = TFT_BLACK;
      if (count >= 2 && i < count) c = i == index ? WARM : DIMWARM;
      spr.drawPixel(1 + i * 2, 0, c);
    }
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
    bool changed = false;
    for (int i = 0; i < nScroll; i++) {
      Scroller &sc = scroll[i];
      if ((int32_t)(millis() - sc.next) < 0) continue;
      sc.x += sc.dir;
      int minX = cols - sc.w;
      if (sc.x <= minX) {
        sc.x = minX;
        sc.dir = 1;
        sc.next = millis() + SCROLL_PAUSE_MS;
      } else if (sc.x >= 0) {
        sc.x = 0;
        sc.dir = -1;
        sc.next = millis() + SCROLL_PAUSE_MS;
      } else {
        sc.next = millis() + SCROLL_STEP_MS;
      }
      spr.fillRect(0, sc.y, cols, ROW_H, TFT_BLACK);
      drawText(sc.x, sc.y, sc.text, sc.color);
      changed = true;
    }
    if (changed) flush(false, nullptr);
  }

 private:
  TFT_eSPI &tft;
  TFT_eSprite spr;
  int cols, rows, pitch, ox, oy;
  int rowY[4];
  uint16_t *prev;        // colour of each cell as last pushed to the panel
  float bright = 1.0f;
  int lastPct = -1;

  // rows currently scrolling (reset by drawPlane / drawMessage)
  Scroller scroll[MAX_SCROLL];
  int nScroll = 0;

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

  // Centres a row, or starts it scrolling when it is wider than the grid.
  void placeRow(int y, const String &s, uint16_t color) {
    int w = textW(s);
    if (w <= cols || nScroll >= MAX_SCROLL) {
      drawCentred(y, s, color);
      return;
    }
    drawText(0, y, s, color);
    scroll[nScroll++] = {s, color, y, w, 0, -1, millis() + SCROLL_PAUSE_MS};
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
