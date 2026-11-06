# Brief 2: Dot-matrix display mode

Project: PlaneSpotter firmware for the ESP32-2432S028 (Cheap Yellow Display,
320x240 TFT, TFT_eSPI).

## Goal

An optional rendering mode that makes the TFT look like an LED matrix sign:
discrete round dots on a black grid, pixel font, airline brand colour, a
faint glow around each lit dot. Selectable per unit in the setup portal
("Display style: Crisp / LED matrix"), stored in Preferences. Default stays
Crisp. Do not break the existing layout.

## Approach

Render everything at low resolution into a TFT_eSPI sprite, then upscale
each lit sprite pixel into a dot on the real screen.

### Grid

Two pitches to prototype, chosen by a compile-time constant first, then make
the winner (or both) a setting:

| Pitch | Grid     | Dot diameter | Chars per line (5x7 font, 1 px gap) |
|-------|----------|--------------|-------------------------------------|
| 5 px  | 64 x 48  | 3 px         | 10                                  |
| 4 px  | 80 x 60  | 2–3 px       | 13                                  |

Keep the grid square and centred; any leftover screen edge stays black.

### Rendering

- Sprite: `TFT_eSprite` at grid size, 8-bit or 16-bit colour (RAM is fine
  at 80x60x2 = 9.6 KB).
- Draw the layout into the sprite with the built-in 5x7 GLCD font
  (`setTextFont(1)`), no smooth fonts, no anti-aliasing. Scale 1 only.
- Upscale: for each sprite pixel that is not black, `fillCircle` (or
  `fillRect` with rounded look at 3 px, which is cheaper) at the matching
  screen position in the pixel's colour, plus a 1 px ring in the same colour
  at ~30% brightness for glow. Lit dots change rarely, so draw the delta:
  keep the previous sprite frame and only redraw dots that changed, erasing
  with a black circle of (dot + ring) size. Full redraw must still work for
  screen transitions.
- Colour: lit dots use the airline brand colour from the existing table;
  secondary text uses a warm white (e.g. 255,230,200) rather than pure white,
  which reads more like an LED. Grey labels become a dimmer warm white.
- Brightness: scale all dot colours by the existing brightness setting in
  addition to the backlight PWM, so dim mode darkens the dots rather than
  greying the whole panel.

### Layout in matrix mode (64 x 48 reference, adapt for 80 x 60)

Four text rows of 7 px plus gaps:

1. Airline name, brand colour. If wider than the grid, scroll horizontally at
   ~8 columns/s with a 2 s pause at each end. Scrolling is a sprite shift,
   so it fits the delta-redraw model.
2. Callsign, aircraft type (e.g. `BAW669 A320`), warm white.
3. Route `RAK>LHR` (ASCII `>` or a 3-px arrow glyph drawn by hand), white.
   If route unknown, show registration.
4. `1950 v 170kt 3.6mi E` style stats, amber (255,180,0): altitude with a
   small up/down arrow glyph for climb/descent, speed, distance+direction.
   Use thousands separator only if it fits.

A 5x7 font cannot render every character; the existing `ascii()` filter
already strips non-ASCII.

### Transitions

Keep them simple: on a new aircraft, clear the sprite and redraw. Optional
nicety if time allows: a one-frame "all dots off" then on, which mimics a
real sign updating.

## Settings

- `style` key in Preferences: `0` crisp (default), `1` matrix 64x48,
  `2` matrix 80x60. Exposed in the WiFiManager portal as a parameter;
  a dropdown if WiFiManager's custom HTML parameter supports it cleanly,
  otherwise a numeric field with the options listed in the label.
- Switching style takes effect on the next redraw, no reboot.

## Structure

Pull the drawing code out of `main.cpp` into a small renderer interface,
`Renderer` with `drawPlane`, `drawStats`, `drawMessage`, `drawStatusDot`,
and two implementations `CrispRenderer` (current code moved as-is) and
`MatrixRenderer`. Everything else in `main.cpp` stays untouched.

## Performance

- Full 80x60 redraw with circles plus rings should be well under 200 ms;
  measure with `millis()` and print to serial. If a full redraw is visibly
  slow, draw dots with `fillRect` + corner pixels instead of `fillCircle`.
- Scrolling must not block the 30 s poll loop: drive it from `loop()` with
  a timestamp, not `delay()`.

## Out of scope

Touch, radar page, logos, anything beyond the rendering mode.

## Acceptance

1. With style=1 and 2, the screen shows the current aircraft as discrete
   dots with visible black gaps, brand colour on row 1, long airline names
   scroll.
2. Style=0 is pixel-identical to the current display.
3. Same aircraft, new stats: only the changed dots redraw (verify by
   serial timing or visually, no full-screen flicker).
4. The style persists across power cycles and can be changed from the
   setup portal.
5. Photo both pitches side by side for choosing a default.
