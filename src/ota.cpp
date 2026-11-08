#include "ota.h"
#include "diag.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <esp_ota_ops.h>

#include "certs.h"

static const char *REPO_URL = "https://github.com/bigtreacle/planespotter/";
static const char *MANIFEST_URL =
    "https://github.com/bigtreacle/planespotter/releases/latest/download/manifest.json";

// The Arduino core marks a freshly installed OTA image valid during startup,
// before setup() runs, unless this hook asks it to defer. Deferring is what
// makes rollback real: markValid() below is then the only thing that cancels
// it, and a build that crashes before it gets there reverts on the next boot.
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {

const char *version() { return FIRMWARE_VERSION; }
bool isDevBuild() { return strcmp(FIRMWARE_VERSION, "dev") == 0; }
String versionLabel() { return isDevBuild() ? String("dev") : String("v") + FIRMWARE_VERSION; }

// "v1.2.3" or "1.2.3" -> {1,2,3}; missing parts are 0, anything unparseable is 0.
static void parseVersion(const String &s, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  int start = (s.length() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
  for (int part = 0; part < 3; part++) {
    int dot = s.indexOf('.', start);
    String piece = dot < 0 ? s.substring(start) : s.substring(start, dot);
    out[part] = piece.toInt();
    if (dot < 0) break;
    start = dot + 1;
  }
}

// <0 if a is older than b, 0 if equal, >0 if newer. Numeric, not string, compare.
static int compareVersions(const String &a, const String &b) {
  int va[3], vb[3];
  parseVersion(a, va);
  parseVersion(b, vb);
  for (int i = 0; i < 3; i++)
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  return 0;
}

// NVS keys (namespace "ota"): "try" = version an update is installing,
// "bad" = version that crashed before verification and was rolled back.
static String prefGet(const char *key) {
  Preferences p;
  p.begin("ota", true);
  String v = p.getString(key, "");
  p.end();
  return v;
}
static void prefSet(const char *key, const String &value) {
  Preferences p;
  p.begin("ota", false);
  if (value.length()) p.putString(key, value);
  else p.remove(key);
  p.end();
}

void begin() {
  String attempted = prefGet("try");
  if (!attempted.length()) return;
  if (attempted == FIRMWARE_VERSION) {
    // We are the version that was being installed; markValid() takes it from here.
  } else if (esp_ota_get_last_invalid_partition() != nullptr) {
    // Booted the old firmware with an invalid slot: the new one crashed before
    // it was verified and the bootloader rolled it back. Do not fetch it again,
    // or every unit would loop update -> crash -> rollback until a fix ships.
    LOG("OTA: %s was rolled back, automatic checks will skip it\n", attempted.c_str());
    prefSet("bad", attempted);
  }
  prefSet("try", "");
}

void markValid() {
  static bool done = false; // the state only needs reading once per boot
  if (done) return;
  done = true;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    LOG("OTA: %s marked valid, rollback cancelled\n", FIRMWARE_VERSION);
  }
}

static bool fetchManifest(JsonDocument &doc, Result &failure) {
  NetworkClientSecure client;
  client.setCACert(GITHUB_ROOT_CAS);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(12000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("planespotter/" FIRMWARE_VERSION " " BOARD_VARIANT);
  if (!http.begin(client, MANIFEST_URL)) {
    LOG("OTA: could not start manifest request");
    failure = FAILED;
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    LOG("OTA: manifest HTTP %d\n", code);
    http.end();
    failure = (code == 404) ? NO_RELEASE : FAILED;
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    LOG("OTA: manifest JSON error: %s\n", err.c_str());
    failure = FAILED;
    return false;
  }
  return true;
}

Result check(bool manual, const Ui &ui, String &newVersion) {
  JsonDocument doc;
  Result failure;
  if (!fetchManifest(doc, failure)) return failure;

  newVersion = doc["version"] | "";
  JsonObject build = doc["builds"][BOARD_VARIANT];
  const char *url = build["url"] | "";
  const char *sha256 = build["sha256"] | "";
  if (!newVersion.length() || !*url || strlen(sha256) != 64) {
    LOG("OTA: manifest has no usable entry for " BOARD_VARIANT);
    return FAILED;
  }
  if (strncmp(url, REPO_URL, strlen(REPO_URL)) != 0) {
    LOG("OTA: refusing firmware URL outside the release repo: %s\n", url);
    return FAILED;
  }

  if (!manual && newVersion == prefGet("bad")) {
    LOG("OTA: %s previously rolled back, skipping (manual check overrides)\n",
                  newVersion.c_str());
    return UP_TO_DATE;
  }
  if (isDevBuild()) {
    if (!manual) {
      LOG("OTA: release %s available, dev build, automatic update skipped\n", newVersion.c_str());
      return UP_TO_DATE;
    }
  } else if (compareVersions(newVersion, FIRMWARE_VERSION) <= 0) {
    LOG("OTA: up to date (%s, latest release %s)\n", FIRMWARE_VERSION, newVersion.c_str());
    return UP_TO_DATE;
  }

  LOG("OTA: updating %s -> %s from %s\n", FIRMWARE_VERSION, newVersion.c_str(), url);
  if (ui.message) ui.message("Updating", "to version " + newVersion);

  NetworkClientSecure client;
  client.setCACert(GITHUB_ROOT_CAS);
  HTTPUpdate updater(30000);
  updater.rebootOnUpdate(false);  // the caller shows a "done" screen, then restarts
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  updater.setSHA256sum(sha256);  // hashed while streaming; mismatch aborts before activation
  if (ui.progress) updater.onProgress(ui.progress);

  prefSet("try", newVersion); // begin() on the next boot tells success from rollback
  t_httpUpdate_return r = updater.update(client, url, FIRMWARE_VERSION);
  if (r != HTTP_UPDATE_OK) prefSet("try", ""); // nothing was installed
  switch (r) {
    case HTTP_UPDATE_OK:
      LOG("OTA: %s written and verified\n", newVersion.c_str());
      return UPDATED;
    case HTTP_UPDATE_NO_UPDATES:
      LOG("OTA: server reported no update");
      return UP_TO_DATE;
    default:
      LOG("OTA: update failed (%d): %s\n", updater.getLastError(),
                    updater.getLastErrorString().c_str());
      return FAILED;
  }
}

}  // namespace ota
