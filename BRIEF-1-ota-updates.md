# Brief 1: Over-the-air updates

Project: PlaneSpotter firmware for the ESP32-2432S028 (Cheap Yellow Display).
Repo layout: PlatformIO project, `src/main.cpp`, two envs `cyd` and `cyd2usb`.

Reviewed 2026-11-06 against the installed toolchain (espressif32 platform
8.0.0, Arduino core 3.3) and GitHub's live release infrastructure. Decisions
from that review are marked **Decided**.

## Goal

A unit given to a family member should pick up bug fixes and new features on
its own, with nothing to do at their end. Shipping a release must be a single
`git tag` + `git push --tags`.

## Mechanism

Pull-only HTTP OTA from GitHub Releases using the Arduino-ESP32 `HTTPUpdate`
library. No push, no server to run.

### Firmware identity

- `FIRMWARE_VERSION` string. Default it in code, not in `platformio.ini`
  (there is no clean way to default a missing environment variable there):

  ```cpp
  #ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "dev"
  #endif
  ```

  CI overrides it with the git tag via
  `PLATFORMIO_BUILD_FLAGS='-DFIRMWARE_VERSION=\"1.1.0\"'`.
- `BOARD_VARIANT` string, `"cyd"` or `"cyd2usb"`, set per env in
  `platformio.ini`. The device uses it to pick its binary from the manifest.
- Semantic versions. Compare numerically, not as strings. A `dev` build never
  auto-updates (so a bench unit is not overwritten mid-development) but still
  updates on a manual check.

### Manifest

Attached to every GitHub release as `manifest.json`:

```json
{
  "version": "1.1.0",
  "notes": "Fix route lookup for codeshare callsigns",
  "builds": {
    "cyd":     { "url": "https://github.com/<user>/planespotter/releases/download/v1.1.0/planespotter-cyd.bin",     "sha256": "..." },
    "cyd2usb": { "url": "https://github.com/<user>/planespotter/releases/download/v1.1.0/planespotter-cyd2usb.bin", "sha256": "..." }
  }
}
```

The device fetches the manifest from the fixed URL
`https://github.com/<user>/planespotter/releases/latest/download/manifest.json`.
Verified: GitHub answers with two 302s, first to the versioned
`releases/download/vX.Y.Z/...` path on `github.com`, then to a signed URL on
`release-assets.githubusercontent.com` that expires after about an hour. The
device follows it immediately, so the expiry is irrelevant, but the manifest
must only ever contain the stable `github.com` URLs. `latest` skips
pre-releases (see Build pipeline).

### Update flow

1. Fetch manifest. If `version` <= running version, stop.
2. Draw an update screen: "Updating to 1.1.0", progress bar.
3. Download the binary for `BOARD_VARIANT` via `HTTPUpdate`. Verified the
   installed core has everything needed, so no hand-rolled `Update.h` loop
   and no callback tricks:
   - `httpUpdate.setSHA256sum(manifestSha)` makes it hash while streaming
     and refuse to activate the new slot on mismatch.
   - `httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS)` for the
     two GitHub hops (default redirect limit of 10 is plenty).
   - `httpUpdate.onProgress(...)` drives the progress bar.
   - `httpUpdate.rebootOnUpdate(false)` so the screen can show "Done,
     restarting" before `ESP.restart()`.
4. Reboot.
5. On first boot of new firmware, run as normal. **Decided:** call
   `esp_ota_mark_app_valid_cancel_rollback()` on whichever comes first:
   - the first successful aircraft fetch, or
   - 60 s of running in `loop()` without a crash, or
   - entering the setup portal (so a unit moved to a new house does not sit
     unverified while waiting for WiFi details).

   Rationale: with "first fetch only", a unit that is power-cycled before it
   manages a fetch (WiFi down, moved house, portal open) silently rolls back,
   then sees the release again and re-downloads it: an update/rollback loop
   whenever the network is unavailable at boot. Rollback still protects
   against the real failure mode, a firmware that crashes before 60 s.

### When it checks

- Once at boot, 20 s after first successful WiFi connection, so setup and
  the first aircraft fetch are never delayed by an update.
- Then every 24 h, aligned to a quiet moment between the 30 s polls.
- Manual: a short press (< 1 s) of the BOOT button forces a check and shows
  "Checking for updates..." then either "Up to date (1.0.0)" for 3 s or the
  update screen. The existing 3 s long-press for the setup portal stays.
- Pause the 30 s aircraft poll while an update is in progress.

### Screen

- Progress bar during download, percentage, version number.
- Show the running version in small grey text on the "Clear skies" screen and
  on the setup screen, so a unit's version is visible without a serial cable.

## Partitions and rollback

**Decided: custom partition table, shipped from v1.0.0.** The partition
table cannot be changed over the air, so whatever v1.0.0 carries is what
every unit keeps until reflashed by cable. SPIFFS is unused, so drop it.

Measured: the current firmware is 1.51 MB (`min_spiffs.csv` slots are
0x1E0000 = 1.875 MB). OTA code plus two root certificates will add roughly
60 to 100 KB. App partitions must start on 64 KB boundaries, so on the 4 MB
flash the largest pair of slots is 0x1F0000 each: 64 KB more per slot than
`min_spiffs.csv`, with the coredump partition kept.

`partitions.csv` in the project root, referenced by
`board_build.partitions = partitions.csv` in `[env]`:

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x5000,
otadata,  data, ota,     0xe000,   0x2000,
app0,     app,  ota_0,   0x10000,  0x1F0000,
app1,     app,  ota_1,   0x200000, 0x1F0000,
coredump, data, coredump,0x3F0000, 0x10000,
```

Rollback: verified enabled in the prebuilt Arduino core
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1` in the esp32 sdkconfig), so
`esp_ota_mark_app_valid_cancel_rollback()` is live, not a no-op. Still test
it: deliberately release a build that calls `abort()` in `setup()`, confirm
the unit comes back on the old version.

Watchdog caveat: the task watchdog in this core watches only the idle task
on core 0, and the Arduino `loop()` runs on core 1. A crash resets the board
and triggers rollback; a hang does not. Hang protection is out of scope for
v1; if wanted later, subscribe the loop task to the task watchdog with a
timeout longer than the 12 s HTTP timeout.

## TLS

The code currently uses `setInsecure()`. For OTA, pin root CAs for the
manifest and binary downloads only. The aircraft-data calls can stay
insecure.

**Decided: pin two roots, not one, and not DigiCert.** Verified 2026-11-06,
the download path crosses two hosts with different chains:

| Host                                   | Role                         | Root to pin                                   | Root expires |
|----------------------------------------|------------------------------|-----------------------------------------------|--------------|
| `github.com`                           | manifest URL, first redirect | USERTrust ECC Certification Authority (Sectigo) | Jan 2038     |
| `release-assets.githubusercontent.com` | the actual files             | ISRG Root X1 (Let's Encrypt)                  | Jun 2035     |

Put both PEMs concatenated in one string in `src/certs.h`; `setCACert()`
accepts a multi-certificate PEM. Pin the roots, never the intermediates (the
Let's Encrypt chain currently goes via a "Root YR" intermediate that expires
in 2032 and will rotate sooner). Re-check both chains at implementation
time with `openssl s_client -showcerts`.

Note in the README that GitHub has changed CAs before (DigiCert to Sectigo),
that the pins will need replacing when it happens again, and that a unit
with a stale pin still runs normally, it just stops updating until reflashed
by USB.

## Build pipeline

Prerequisite: the project is not yet a git repository and has no GitHub
remote. Create both first.

GitHub Actions workflow `.github/workflows/release.yml`:

- Trigger: push of a tag matching `v*`.
- Steps: install PlatformIO, `pio run -e cyd` and `pio run -e cyd2usb` with
  `FIRMWARE_VERSION` set from the tag, compute SHA-256 of both `firmware.bin`
  files, write `manifest.json`, create a GitHub release with the two binaries
  (renamed `planespotter-cyd.bin`, `planespotter-cyd2usb.bin`) and the
  manifest attached. Release notes from the tag message.
- Cache `~/.platformio` (keyed on `platformio.ini`). The toolchain download
  is about 2.5 GB; uncached, every tag build repeats it.
- Publish test releases as **pre-releases**. `releases/latest` ignores them,
  so the release flow can be exercised without any family unit picking the
  build up. Promote to a full release when ready.
- Also a `build.yml` on every push to `main` that just compiles both envs,
  so a broken commit is caught before tagging.

## Out of scope

Signed firmware, staged rollouts, a settings web page, delta updates, hang
detection via watchdog.

## Acceptance

1. Flash `v1.0.0` by USB. Tag and push `v1.0.1` with a visible change
   (e.g. version string on the clear-skies screen). On a BOOT short press
   (or within 24 h), the unit updates without any interaction and shows the
   change.
2. Rollback test as above passes: a release whose `setup()` calls `abort()`
   is rolled back and the unit returns to the previous version.
3. Power-cycling a freshly updated unit with WiFi unavailable does **not**
   roll it back (the 60 s / portal rule).
4. A `dev` build on the bench never auto-updates, but does update on a
   manual check.
5. A unit on the wrong variant never receives the other variant's binary.
6. A tampered binary (one byte changed, manifest unchanged) is rejected and
   the unit stays on its current version.
7. README documents: how to cut a release, the two pinned roots and how to
   refresh them, the partition table decision, the cable note
   (USB-A-to-C only on the two-port board, the C-to-C cable will not power it).
