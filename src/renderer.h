// Display styles. main.cpp decides what to show; a Renderer decides how it
// looks. "Crisp" is the original full-resolution layout; "Matrix" imitates an
// LED dot-matrix sign (see BRIEF-2-dot-matrix-look.md).
#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "model.h"

enum Style {
  STYLE_CRISP = 0,
  STYLE_MATRIX_64 = 1, // 64x48 dots, 5 px pitch
  STYLE_MATRIX_80 = 2, // 80x60 dots, 4 px pitch
};

class Renderer {
 public:
  virtual ~Renderer() {}
  // Full-screen status text (setup, errors, "Clear skies").
  virtual void drawMessage(const char *title, const char *line1, const char *line2 = "",
                           const char *line3 = "") = 0;
  // Download progress over the current message; redraws only when the percent changes.
  virtual void drawProgress(int done, int total) = 0;
  // New aircraft: whole screen. al is the brand-colour entry, or null.
  virtual void drawPlane(const Plane &p, const Route &route, const Airline *al) = 0;
  // Same aircraft, new numbers: the stats part only.
  virtual void drawStats(const Plane &p) = 0;
  // Top-right fetch-failure marker.
  virtual void drawStatusDot(bool fetchOk) = 0;
  // Top-left row of dots: one per nearby aircraft, the selected one bright.
  // Draws nothing when count < 2. Call after drawPlane/drawStats.
  virtual void drawSelection(int index, int count) = 0;
  // Called often from loop() for animations (matrix scrolling); must not block.
  virtual void tick() {}
  // Panel brightness in percent; the matrix style also dims its dot colours.
  virtual void setBrightness(int percent) {}
  // Debug aid: print what is on screen to serial (matrix style prints its dot grid).
  virtual void dump() {}
};

Renderer *newCrispRenderer(TFT_eSPI &tft);
Renderer *newMatrixRenderer(TFT_eSPI &tft, int cols, int rows);

inline Renderer *makeRenderer(int style, TFT_eSPI &tft) {
  switch (style) {
    case STYLE_MATRIX_64: return newMatrixRenderer(tft, 64, 48);
    case STYLE_MATRIX_80: return newMatrixRenderer(tft, 80, 60);
    default: return newCrispRenderer(tft);
  }
}
