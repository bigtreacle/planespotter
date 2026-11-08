#include "touch.h"

#include <SPI.h>

#include "diag.h"

namespace touch {

// CYD wiring: the panel is not on the display's HSPI bus.
static const int PIN_CLK = 25, PIN_MOSI = 32, PIN_MISO = 39, PIN_CS = 33, PIN_IRQ = 36;
static const int Z_THRESHOLD = 400;        // pressure below this is noise
static const uint32_t DEBOUNCE_MS = 40;    // finger must stay down this long
static const uint32_t REPEAT_MS = 350;     // minimum gap between taps

static SPIClass spi(VSPI);
static bool down = false;
static uint32_t downAt = 0, lastTap = 0;

void begin() {
  pinMode(PIN_IRQ, INPUT); // PENIRQ, pulled up on the board, low while touched
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  spi.begin(PIN_CLK, PIN_MISO, PIN_MOSI, PIN_CS);
  LOG("touch: XPT2046 on VSPI, IRQ %d", PIN_IRQ);
}

// Pressure from the two Z plates, the same arithmetic as the usual XPT2046
// drivers: around 0 when nothing touches, thousands for a firm press.
static int pressure() {
  spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(PIN_CS, LOW);
  spi.transfer(0xB1);                  // start Z1 (12 bit, differential)
  int z1 = spi.transfer16(0xC1) >> 3;  // read Z1, start Z2
  int z2 = spi.transfer16(0x90) >> 3;  // read Z2; 0x90 powers down with PENIRQ enabled
  spi.transfer16(0);                   // flush the last conversion
  digitalWrite(PIN_CS, HIGH);
  spi.endTransaction();
  return z1 + 4095 - z2;
}

bool tapped() {
  // The IRQ pin is a plain read; only talk SPI while something presses.
  bool pressed = digitalRead(PIN_IRQ) == LOW && pressure() > Z_THRESHOLD;
  uint32_t now = millis();
  if (!pressed) {
    down = false;
    return false;
  }
  if (!down) {
    down = true;
    downAt = now;
    return false;
  }
  if (now - downAt < DEBOUNCE_MS) return false;
  if (now - lastTap < REPEAT_MS) return false;
  // one tap per press: a held finger does not repeat
  if (downAt <= lastTap) return false;
  lastTap = now;
  return true;
}

} // namespace touch
