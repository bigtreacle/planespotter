#include "diag.h"

#include <Preferences.h>
#include <esp_core_dump.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <stdarg.h>

namespace diag {

// ---------- ring buffer in RTC slow memory ----------
// RTC_NOINIT_ATTR data is left alone by the bootloader on every reset other
// than power on, so the lines written before a crash are still here after it.
static const uint32_t LOG_MAGIC = 0x50534c47; // "PSLG"
static const size_t LOG_SIZE = 3072;

struct RtcLog {
  uint32_t magic;
  uint32_t head;  // next write position
  uint32_t count; // bytes written in total, capped at LOG_SIZE once wrapped
  char buf[LOG_SIZE];
};
RTC_NOINIT_ATTR static RtcLog rtcLog;

static bool rtcLogValid() {
  return rtcLog.magic == LOG_MAGIC && rtcLog.head < LOG_SIZE && rtcLog.count <= LOG_SIZE;
}

static void rtcLogReset() {
  rtcLog.magic = LOG_MAGIC;
  rtcLog.head = 0;
  rtcLog.count = 0;
}

static void rtcLogWrite(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    rtcLog.buf[rtcLog.head] = s[i];
    rtcLog.head = (rtcLog.head + 1) % LOG_SIZE;
  }
  rtcLog.count += n;
  if (rtcLog.count > LOG_SIZE) rtcLog.count = LOG_SIZE;
}

// Copy the buffer out in order, oldest first, as a NUL-terminated string.
static String rtcLogContents() {
  if (!rtcLogValid() || rtcLog.count == 0) return String();
  String out;
  out.reserve(rtcLog.count + 1);
  uint32_t start = rtcLog.count < LOG_SIZE ? 0 : rtcLog.head;
  for (uint32_t i = 0; i < rtcLog.count; i++) {
    char c = rtcLog.buf[(start + i) % LOG_SIZE];
    if (c) out += c;
  }
  // a wrapped buffer usually starts mid-line; drop the partial first line
  if (rtcLog.count == LOG_SIZE) {
    int nl = out.indexOf('\n');
    if (nl >= 0) out.remove(0, nl + 1);
  }
  return out;
}

// ---------- reset reason ----------
static const char *reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external reset";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    case ESP_RST_SDIO: return "SDIO reset";
    default: return "unknown";
  }
}

// Resets the firmware did not ask for. Power on and ESP.restart() are normal.
static bool abnormal(esp_reset_reason_t r) {
  return r != ESP_RST_POWERON && r != ESP_RST_SW && r != ESP_RST_EXT && r != ESP_RST_DEEPSLEEP;
}

// ---------- NVS record ----------
// Namespace "diag": boots (u32), bad (u32, abnormal resets), last (string,
// reason of the last abnormal reset), lastlog (string, log lines before it),
// lastup (u32, seconds of uptime when it happened).
static Preferences prefs;

static uint32_t uptimeSeconds() { return millis() / 1000; }

static String coreDumpSummary() {
  if (esp_core_dump_image_check() != ESP_OK) return String();
  esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
  if (!s) return String();
  String out;
  if (esp_core_dump_get_summary(s) == ESP_OK) {
    char line[96];
    snprintf(line, sizeof line, "core dump: task %s, PC 0x%08lx", s->exc_task, (unsigned long)s->exc_pc);
    out = line;
  }
  free(s);
  return out;
}

void begin() {
  esp_reset_reason_t reason = esp_reset_reason();
  String previous = rtcLogContents();
  // A core dump summary names the task that was running at the moment of the
  // panic. For a watchdog reset that is whichever task happened to be
  // scheduled, not the one that stalled, so only report it for real crashes.
  String dump = reason == ESP_RST_PANIC ? coreDumpSummary() : String();
  rtcLogReset();

  prefs.begin("diag", false);
  uint32_t boots = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", boots);
  if (abnormal(reason)) {
    prefs.putUInt("bad", prefs.getUInt("bad", 0) + 1);
    prefs.putString("last", reasonName(reason));
    String record = previous;
    if (dump.length()) record += dump + "\n";
    // Preferences strings are limited; keep the tail if the buffer is bigger.
    const size_t MAX_RECORD = 2900;
    if (record.length() > MAX_RECORD) record.remove(0, record.length() - MAX_RECORD);
    prefs.putString("lastlog", record);
  }
  prefs.end();

  logf("boot %lu, reset reason: %s", (unsigned long)boots, reasonName(reason));
  if (abnormal(reason)) {
    Serial.println("---- previous boot ended abnormally; its last log lines: ----");
    Serial.print(previous.length() ? previous : String("(none recorded)\n"));
    if (dump.length()) Serial.println(dump);
    Serial.println("---- end of previous boot ----");
    // The dump stays in its partition (the next crash overwrites it) so it can
    // be pulled over USB and decoded with the matching .elf when needed.
  }
}

void report() {
  prefs.begin("diag", true);
  uint32_t boots = prefs.getUInt("boots", 0);
  uint32_t bad = prefs.getUInt("bad", 0);
  String last = prefs.getString("last", "");
  String lastlog = prefs.getString("lastlog", "");
  prefs.end();
  Serial.printf("diag: boots %lu, abnormal resets %lu, uptime %lu s, free heap %lu (min %lu)\n",
                (unsigned long)boots, (unsigned long)bad, (unsigned long)uptimeSeconds(),
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  if (last.length()) {
    Serial.printf("diag: last abnormal reset: %s; log before it:\n", last.c_str());
    Serial.print(lastlog);
    Serial.println("diag: end of record");
  } else {
    Serial.println("diag: no abnormal reset recorded");
  }
}

// ---------- watchdog ----------
static bool wdtSubscribed = false;

void watchdogStart(uint32_t watchdogSeconds) {
  // The core starts the task watchdog at 5 s watching only the idle task.
  // Lengthen it and add the loop task.
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = watchdogSeconds * 1000;
  cfg.idle_core_mask = 1 << 0; // as the core had it
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err != ESP_OK) {
    logf("watchdog: configure failed (%d)", (int)err);
    return;
  }
  watchdogResume();
  logf("watchdog: loop must make progress every %lu s", (unsigned long)watchdogSeconds);
}

void feed() {
  if (wdtSubscribed) esp_task_wdt_reset();
}

void watchdogPause() {
  if (!wdtSubscribed) return;
  esp_task_wdt_delete(NULL);
  wdtSubscribed = false;
}

void watchdogResume() {
  if (wdtSubscribed) return;
  if (esp_task_wdt_add(NULL) == ESP_OK) {
    wdtSubscribed = true;
    esp_task_wdt_reset();
  }
}

// ---------- logging ----------
void logf(const char *fmt, ...) {
  char line[256];
  int n = snprintf(line, sizeof line, "[%6lu.%03lu] ", millis() / 1000, millis() % 1000);
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof line - n, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  n = min((int)sizeof line - 2, n + m);
  // strip any trailing newline the caller added, then add exactly one
  while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  line[n++] = '\n';
  line[n] = 0;
  Serial.print(line);
  if (!rtcLogValid()) rtcLogReset();
  rtcLogWrite(line, n);
}

// ---------- bench tests ----------
void testHang() {
  logf("TEST: hanging the loop on purpose, the watchdog should reboot the board");
  Serial.flush();
  for (;;) {
    // no delay, no yield, nothing that feeds the watchdog
  }
}

void testCrash() {
  logf("TEST: crashing on purpose, a panic reset should follow");
  Serial.flush();
  volatile int *p = nullptr;
  *p = 1;
}

} // namespace diag
