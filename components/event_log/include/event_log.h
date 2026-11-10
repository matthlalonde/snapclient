// A short log of notable events (connections, interruptions, restarts) for
// the settings page's Status tab.
//
// The newest EVENT_LOG_SIZE events live in RTC memory, which keeps its
// contents through crashes, watchdog resets and software restarts (not
// through power loss), so the page can show what happened before the last
// restart. A repeat of the newest event only bumps its count. A separate
// table keeps the last EVENT_LOG_RUNS starts (why each began, how long the
// run lasted at least), so restarts stay visible after routine events have
// filled the log.
//
// Any task may add events; not from an ISR. Adding never blocks for long and
// silently drops the event if the log is busy.

#ifndef __EVENT_LOG_H__
#define __EVENT_LOG_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_LOG_SIZE 40
#define EVENT_LOG_RUNS 8
#define EVENT_LOG_TEXT_LEN 96

typedef enum {
  EVENT_SRC_SYSTEM = 0,
  EVENT_SRC_WIFI,
  EVENT_SRC_SNAPCAST,
  EVENT_SRC_SENDSPIN,
} event_source_t;

typedef enum {
  EVENT_INFO = 0,
  EVENT_WARN,
  EVENT_ERROR,
} event_level_t;

#define EVENT_FLAG_RESTART 0x01  // a restart follows: it explains the next start

typedef struct {
  uint32_t seq;        // increases across restarts; 0 = empty slot
  uint32_t boot;       // run (boot) it belongs to, see event_log_boot()
  int64_t uptime_us;   // time since that run started (latest repeat)
  uint16_t count;      // how many times in a row it happened (saturates)
  uint8_t source;      // event_source_t
  uint8_t level;       // event_level_t
  uint8_t flags;       // EVENT_FLAG_*
  char text[EVENT_LOG_TEXT_LEN];
} event_log_entry_t;

typedef struct {
  uint32_t boot;
  uint8_t reset_reason;  // esp_reset_reason_t that started this run
  bool explained;        // the previous run logged why it restarted
  int64_t last_us;       // uptime of the run's latest event: it ran at least this long
} event_log_run_t;

// Once, first thing in app_main: keeps the previous runs' events if the RTC
// copy is intact and starts a new run.
void event_log_init(void);

// Add an event. event_log_addf formats on the caller's stack (about 1 KB of
// printf); tasks with small stacks use event_log_add with a fixed string.
void event_log_add(event_source_t source, event_level_t level,
                   const char *text);
void event_log_addf(event_source_t source, event_level_t level,
                    const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// Record why the device is about to restart (a fixed string, safe on small
// stacks). The next start then counts as explained.
void event_log_note_restart(event_level_t level, const char *why);
// Log `why` as an error, then restart.
void event_log_restart(const char *why) __attribute__((noreturn));

// The current run's number, whether its start was explained (also true when
// the log was empty and can't tell), and a counter that changes with every
// added or repeated event.
uint32_t event_log_boot(void);
bool event_log_start_explained(void);
uint32_t event_log_last_change(void);

// Copy the events, oldest first, into out (up to max). Returns the count, or
// -1 if the log was busy.
int event_log_snapshot(event_log_entry_t *out, size_t max);
// Copy the recorded starts, newest first. Returns the count.
size_t event_log_runs(event_log_run_t *out, size_t max);

const char *event_log_source_name(uint8_t source);  // "system", "wifi", ...
const char *event_log_level_name(uint8_t level);    // "info", "warn", "error"
const char *event_log_reset_reason_text(esp_reset_reason_t reason);

// "44.1 kHz", "48 kHz": writes into buf (at least 16 bytes) and returns it
const char *event_log_khz(char *buf, size_t size, uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif  // __EVENT_LOG_H__
