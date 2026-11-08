# PlaneSpotter for the Cheap Yellow Display

Shows the nearest airborne aircraft: airline, route, type, altitude, speed,
and distance and direction from home. Refreshes every 30 seconds.

## Hardware

- ESP32-2432S028 ("Cheap Yellow Display", 2.8" 320x240), no other parts
- USB cable and any 5V USB charger. On the two-port (USB-C plus micro-USB)
  board use a USB-A-to-C cable: a C-to-C cable will not power it.

## Flash

Install PlatformIO (`pipx install platformio`, or the `platformio-core`
package), plug the board in, then from this folder:

    pio run -e cyd -t upload        # original board, one micro-USB port
    pio run -e cyd2usb -t upload    # newer board, USB-C plus micro-USB

If the colours look inverted or wrong, flash the other variant.
`pio device monitor` shows debug output.

A USB flash is only needed once. After that, units update themselves from
GitHub Releases (see "Updates" below). A USB build is version `dev` and never
updates on its own, so a bench unit is not overwritten mid-development.

The project uses its own partition table (`partitions.csv`: two 1.94 MB app
slots, no SPIFFS). Partition tables cannot change over the air, so if that
file ever changes, every unit needs a USB reflash.

## Set up (what the recipient does)

1. Plug it in. The screen says "Setup".
2. On a phone, join the WiFi network "PlaneSpotter".
3. On the page that opens, choose the home WiFi, enter its password and
   a UK postcode, and save.

To change WiFi or location later, hold the BOOT button on the back for
3 seconds. Outside the UK, leave the postcode blank and enter latitude
and longitude. The same page has a "Flip screen" field for a case that
mounts the board upside down.

## Touch

Tap the screen to step through the four nearest aircraft; a row of small
dots in the top-left corner shows how many there are and which one is on
screen. The display follows the aircraft you picked as the others move
around it, and drifts back to the nearest 30 seconds after the last tap.
Routes are cached, so stepping back to an aircraft you have already seen is
instant. Anywhere on the screen counts, so a flipped display needs nothing
special.

## Display style

The setup portal has a "Display style" field: `0` is the default crisp
layout, `1` and `2` imitate an LED dot-matrix sign at 64x48 dots (5 px
pitch) or 80x60 dots (4 px pitch). In the matrix styles the airline name
scrolls when it is too long for the row, the route is `RAK>LHR`, and the
bottom row packs altitude (with a climb/descent arrow), speed, distance and
direction into whatever detail fits. The choice is stored on the unit; the
screen is black and redraws when the style changes.

On a bench unit the style can also be switched over serial without the
portal: type `style 1` (or `0`, `2`) in the serial monitor; `flip 1` or
`flip 0` turns the screen. `dump` prints
the matrix grid as text, which is handy when tweaking the layout. `tap` does
what a touch does.

## If it stops

Every network step is bounded: connections, TLS handshakes and reads each
give up after 12-15 seconds, and a chunked response body is read with its
own 20-second deadline (the core's HTTP client has none once a chunk has
started), so a bad connection shows a red dot rather than a frozen display. Behind that, a watchdog watches the main loop. If the
firmware stops making progress for two minutes anyway, the board reboots on
its own and carries on. Waiting in the setup portal does not count.

Every log line is also kept in a small buffer in RTC memory, which survives
a reboot. When a boot follows a crash, a watchdog reset or a brownout, the
firmware prints what the previous boot was doing just before it stopped,
and saves that record to flash so it is still there after the unit has been
unplugged. On a bench unit type `diag` in the serial monitor to see the boot
counters, free heap and the saved record; `hang` and `crash` provoke a
watchdog reset and a panic on purpose to prove the machinery works.

## Updates

Units check for a new firmware release 20 seconds after connecting to WiFi
and then once a day. An update shows a progress bar, verifies the download's
SHA-256, restarts, and if the new firmware crashes within its first minute
the board rolls back to the previous version by itself. A version that was
rolled back is remembered and not fetched again automatically, so a bad
release does not loop; publish a fixed version, or force it with a manual
check.

- A short press of the BOOT button checks right now and shows either
  "Up to date" or the update screen.
- The running version is shown in the bottom-right corner of the setup and
  "Clear skies" screens (`dev` for a USB build).
- A `dev` build only updates on a manual check.

### Cutting a release

Releases are built by GitHub Actions from an annotated tag; the tag message
becomes the release notes:

    git tag -a v1.1.0 -m "Fix route lookup for codeshare callsigns"
    git push --tags

The workflow builds both variants with the version baked in, attaches
`planespotter-cyd.bin`, `planespotter-cyd2usb.bin` and `manifest.json` to a
GitHub release, and units pick it up from the "latest release" URL. A tag
with a hyphen (`v1.1.0-rc1`) is published as a pre-release, which units
ignore, so the pipeline can be tested without touching anyone's display.
Versions compare numerically (`1.10.0` is newer than `1.9.0`).

### Pinned certificates

Update downloads only trust the two root CAs in `src/certs.h`: USERTrust ECC
(for github.com, expires 2038) and ISRG Root X1 (for the release asset host,
expires 2035). GitHub has changed certificate authorities before. If updates
start failing with a TLS error, check both chains with

    openssl s_client -connect github.com:443 -servername github.com -showcerts
    openssl s_client -connect release-assets.githubusercontent.com:443 -servername release-assets.githubusercontent.com -showcerts

and replace the PEMs in `src/certs.h`. A unit with a stale pin keeps working
normally; it just stops updating until reflashed over USB.

## Notes

- A red dot in the top-right corner means the last data fetch failed; the
  previous aircraft stays on screen until the next successful one.
- Routes come from adsbdb.com, matched on callsign. They are sometimes
  missing or out of date.
- Add airlines or change colours in the `AIRLINES` table in `src/main.cpp`.
- Drawing lives behind the `Renderer` interface in `src/renderer.h`:
  `src/crisp_renderer.cpp` is the original layout, `src/matrix_renderer.cpp`
  the dot-matrix look.
- Touch is read directly from the XPT2046 in `src/touch.cpp`; the panel is
  on its own SPI bus (VSPI), not the display's, so TFT_eSPI's touch code is
  not used.
- Watchdog, reset-reason record and the persistent log live in `src/diag.cpp`;
  everything logs through the `LOG()` macro from `src/diag.h`.
- OTA code lives in `src/ota.cpp`; the release pipeline in
  `.github/workflows/release.yml` and `scripts/make_manifest.py`.
- Data: adsb.lol (fallback adsb.fi), adsbdb.com, postcodes.io.
  All free for personal use, no keys.
