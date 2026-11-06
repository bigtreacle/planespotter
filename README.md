# PlaneSpotter for the Cheap Yellow Display

Shows the nearest airborne aircraft: airline, route, type, altitude, speed,
and distance and direction from home. Refreshes every 30 seconds.

## Hardware

- ESP32-2432S028 ("Cheap Yellow Display", 2.8" 320x240), no other parts
- USB cable and any 5V USB charger

## Flash

Install PlatformIO (`pipx install platformio`, or the `platformio-core`
package), plug the board in, then from this folder:

    pio run -e cyd -t upload        # original board, one micro-USB port
    pio run -e cyd2usb -t upload    # newer board, USB-C plus micro-USB

If the colours look inverted or wrong, flash the other variant.
`pio device monitor` shows debug output.

## Set up (what the recipient does)

1. Plug it in. The screen says "Setup".
2. On a phone, join the WiFi network "PlaneSpotter".
3. On the page that opens, choose the home WiFi, enter its password and
   a UK postcode, and save.

To change WiFi or location later, hold the BOOT button on the back for
3 seconds. Outside the UK, leave the postcode blank and enter latitude
and longitude.

## Notes

- A red dot in the top-right corner means the last data fetch failed; the
  previous aircraft stays on screen until the next successful one.
- Routes come from adsbdb.com, matched on callsign. They are sometimes
  missing or out of date.
- Add airlines or change colours in the `AIRLINES` table in `src/main.cpp`.
- Data: adsb.lol (fallback adsb.fi), adsbdb.com, postcodes.io.
  All free for personal use, no keys.
