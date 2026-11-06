// Over-the-air updates from GitHub Releases (pull only, no server to run).
//
// A release carries manifest.json with a version and, per board variant, a
// firmware URL and SHA-256. The device fetches the manifest from the fixed
// "latest release" URL, and if it is newer than the running build, streams the
// binary for its own variant into the inactive OTA slot, verifying the hash.
// Rollback: the new image boots in "pending verify" state; markValid() must be
// called once it is known to work, or the next reset reverts to the old slot.
#pragma once

#include <Arduino.h>
#include <functional>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"  // CI sets this from the git tag, e.g. "1.1.0"
#endif
#ifndef BOARD_VARIANT
#error "BOARD_VARIANT must be defined per environment in platformio.ini"
#endif

namespace ota {

enum Result {
  NO_RELEASE,  // no release published yet (manifest 404)
  UP_TO_DATE,  // running version is current (or dev build on an automatic check)
  UPDATED,     // new firmware written and verified; caller should restart
  FAILED,      // network, TLS, manifest or flash error; still on the old firmware
};

struct Ui {
  std::function<void(const char *title, const String &line)> message;  // full-screen status
  std::function<void(int done, int total)> progress;                    // download progress
};

const char *version();      // FIRMWARE_VERSION
bool isDevBuild();          // true when FIRMWARE_VERSION is "dev"
String versionLabel();      // "v1.1.0" or "dev", for the screen

// Call once early in setup(). Detects that a previous update attempt was
// rolled back by the bootloader and remembers that version so automatic
// checks skip it (a manual check still installs it).
void begin();

// Cancels rollback if this image is still pending verification. Safe to call
// repeatedly; does nothing once the image is marked valid.
void markValid();

// Checks the latest release and updates if appropriate. A dev build only
// updates when manual is true. newVersion receives the manifest version.
Result check(bool manual, const Ui &ui, String &newVersion);

}  // namespace ota
