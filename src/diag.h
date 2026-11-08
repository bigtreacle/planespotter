// Diagnostics: a watchdog on the main loop, a record of why the board last
// reset, and a log that survives a reset so the next boot can say what the
// board was doing when it stopped.
//
// Everything the firmware logs goes through LOG(). Lines go to the serial
// port as before and into a ring buffer in RTC memory, which keeps its
// contents across every kind of reset except a power cycle. When a boot
// follows a crash, a watchdog reset or a brownout, begin() prints that
// buffer and also saves it to NVS, so it can be read back with the "diag"
// serial command even after the unit has been unplugged.
#pragma once

#include <Arduino.h>

namespace diag {

// Call first thing in setup(), after Serial.begin().
void begin();

// Subscribe the calling task (the loop task) to the task watchdog. If the loop
// stops making progress for watchdogSeconds the board panics and reboots,
// which also records the event for the next boot to report.
void watchdogStart(uint32_t watchdogSeconds);

// Tell the watchdog the loop is still alive. Called once per loop() pass and
// from inside long blocking operations that report progress.
void feed();

// Unsubscribe / resubscribe around operations that legitimately block for
// longer than the timeout (the setup portal waits for a person).
void watchdogPause();
void watchdogResume();

// Printf to serial and the persistent ring buffer.
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Print the stored record of the last abnormal reset and the boot counters.
void report();

// Bench commands: hang the loop (watchdog test) or dereference null (panic).
void testHang();
void testCrash();

} // namespace diag

#define LOG(...) diag::logf(__VA_ARGS__)
