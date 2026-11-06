// Data shared between main.cpp (which fetches it) and the renderers (which
// draw it).
#pragma once

#include <Arduino.h>

struct Airline {
  const char *icao;
  const char *name;
  uint8_t r, g, b;
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

inline String thousands(long n) {
  String s = String(labs(n));
  for (int i = s.length() - 3; i > 0; i -= 3) s = s.substring(0, i) + "," + s.substring(i);
  return n < 0 ? "-" + s : s;
}

inline const char *compass(float deg) {
  static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return pts[((int)((deg + 22.5f) / 45.0f)) % 8];
}
