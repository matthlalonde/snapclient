// Event log in RTC memory, see event_log.h.
//
// RTC slow memory keeps its contents through software resets, panics and
// watchdog resets, and holds random data after power-on. A reset can also
// land in the middle of a write (esp_restart stalls the other core, a panic
// stops both). So nothing at start-up trusts the bookkeeping: every slot and
// run record carries its own CRC, and the write position, sequence and run
// number are rebuilt from the valid slots. A torn write costs that one slot.

#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define EVENT_LOG_MAGIC 0x45564C32u  // "EVL2"; change when the layout changes
// how long an add waits for a reader copying the log (it holds the lock for
// one memcpy)
#define ADD_WAIT_TICKS pdMS_TO_TICKS(20)
// a restart note this close to the end of a run explains the next start
#define RESTART_NOTE_WINDOW_US (10LL * 1000000LL)

typedef struct {
  event_log_entry_t e;
  uint32_t crc;
} slot_t;

typedef struct {
  event_log_run_t r;
  uint32_t crc;
} run_slot_t;

static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR slot_t s_slots[EVENT_LOG_SIZE];
static RTC_NOINIT_ATTR run_slot_t s_runs[EVENT_LOG_RUNS];

// bookkeeping in ordinary RAM, rebuilt at start-up
static uint32_t s_boot;
static uint32_t s_next_seq = 1;
static uint32_t s_head;  // next slot to write
static bool s_explained;
static volatile uint32_t s_changes;

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_buf;

static uint32_t slot_crc(const slot_t *s) {
  return esp_rom_crc32_le(0, (const uint8_t *)&s->e, sizeof(s->e));
}

static uint32_t run_crc(const run_slot_t *r) {
  return esp_rom_crc32_le(0, (const uint8_t *)&r->r, sizeof(r->r));
}

static bool slot_ok(const slot_t *s) {
  return s->e.seq != 0 && s->e.text[EVENT_LOG_TEXT_LEN - 1] == '\0' &&
         s->crc == slot_crc(s);
}

static bool run_ok(const run_slot_t *r) {
  return r->r.boot != 0 && r->crc == run_crc(r);
}

void event_log_init(void) {
  if (s_mutex != NULL) {
    return;
  }
  bool fresh = s_magic != EVENT_LOG_MAGIC;
  if (fresh) {
    // power-on, or a firmware with another layout: start empty
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_runs, 0, sizeof(s_runs));
    s_magic = EVENT_LOG_MAGIC;
  }

  // rebuild the bookkeeping from whatever survived intact
  uint32_t max_seq = 0, prev_boot = 0;
  size_t newest = 0;
  for (size_t i = 0; i < EVENT_LOG_SIZE; i++) {
    if (!slot_ok(&s_slots[i])) {
      memset(&s_slots[i], 0, sizeof(s_slots[i]));
      continue;
    }
    if (s_slots[i].e.seq > max_seq) {
      max_seq = s_slots[i].e.seq;
      newest = i;
    }
    if (s_slots[i].e.boot > prev_boot) {
      prev_boot = s_slots[i].e.boot;
    }
  }
  for (size_t i = 0; i < EVENT_LOG_RUNS; i++) {
    if (!run_ok(&s_runs[i])) {
      memset(&s_runs[i], 0, sizeof(s_runs[i]));
    } else if (s_runs[i].r.boot > prev_boot) {
      prev_boot = s_runs[i].r.boot;
    }
  }
  s_next_seq = max_seq + 1;
  s_head = max_seq != 0 ? (newest + 1) % EVENT_LOG_SIZE : 0;

  // explained: the previous run noted a restart near its end
  int64_t prev_last = INT64_MIN;
  for (size_t i = 0; i < EVENT_LOG_SIZE; i++) {
    if (s_slots[i].e.seq != 0 && s_slots[i].e.boot == prev_boot &&
        s_slots[i].e.uptime_us > prev_last) {
      prev_last = s_slots[i].e.uptime_us;
    }
  }
  // an empty log can't tell (first start after flashing, say): no warning
  s_explained = fresh;
  for (size_t i = 0; i < EVENT_LOG_SIZE; i++) {
    const event_log_entry_t *e = &s_slots[i].e;
    if (e->seq != 0 && e->boot == prev_boot && (e->flags & EVENT_FLAG_RESTART) &&
        prev_last - e->uptime_us <= RESTART_NOTE_WINDOW_US) {
      s_explained = true;
    }
  }

  s_boot = prev_boot + 1;
  run_slot_t *run = &s_runs[s_boot % EVENT_LOG_RUNS];
  memset(run, 0, sizeof(*run));
  run->r.boot = s_boot;
  run->r.reset_reason = (uint8_t)esp_reset_reason();
  run->r.explained = s_explained;
  run->crc = run_crc(run);

  s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_buf);
}

static void add(event_source_t source, event_level_t level, uint8_t flags,
                const char *text) {
  if (s_mutex == NULL || text == NULL) {
    return;
  }
  int64_t now = esp_timer_get_time();
  if (xSemaphoreTake(s_mutex, ADD_WAIT_TICKS) != pdTRUE) {
    return;
  }
  slot_t *newest = &s_slots[(s_head + EVENT_LOG_SIZE - 1) % EVENT_LOG_SIZE];
  if (slot_ok(newest) && newest->e.boot == s_boot &&
      newest->e.source == source && newest->e.level == level &&
      newest->e.flags == flags &&
      strncmp(newest->e.text, text, EVENT_LOG_TEXT_LEN - 1) == 0) {
    // the same thing again (a WiFi retry, say): count it
    if (newest->e.count < UINT16_MAX) {
      newest->e.count++;
    }
    newest->e.uptime_us = now;
    newest->crc = slot_crc(newest);
  } else {
    slot_t *s = &s_slots[s_head];
    memset(s, 0, sizeof(*s));
    s->e.seq = s_next_seq++;
    s->e.boot = s_boot;
    s->e.uptime_us = now;
    s->e.count = 1;
    s->e.source = (uint8_t)source;
    s->e.level = (uint8_t)level;
    s->e.flags = flags;
    memcpy(s->e.text, text, strnlen(text, EVENT_LOG_TEXT_LEN - 1));
    s->crc = slot_crc(s);
    s_head = (s_head + 1) % EVENT_LOG_SIZE;
  }
  run_slot_t *run = &s_runs[s_boot % EVENT_LOG_RUNS];
  run->r.last_us = now;
  run->crc = run_crc(run);
  s_changes++;
  xSemaphoreGive(s_mutex);
}

void event_log_add(event_source_t source, event_level_t level,
                   const char *text) {
  add(source, level, 0, text);
}

void event_log_addf(event_source_t source, event_level_t level,
                    const char *fmt, ...) {
  char text[EVENT_LOG_TEXT_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  add(source, level, 0, text);
}

void event_log_note_restart(event_level_t level, const char *why) {
  add(EVENT_SRC_SYSTEM, level, EVENT_FLAG_RESTART, why);
}

void event_log_restart(const char *why) {
  event_log_note_restart(EVENT_ERROR, why);
  esp_restart();
}

uint32_t event_log_boot(void) { return s_boot; }

bool event_log_start_explained(void) { return s_explained; }

uint32_t event_log_last_change(void) { return s_changes; }

int event_log_snapshot(event_log_entry_t *out, size_t max) {
  if (s_mutex == NULL || out == NULL) {
    return -1;
  }
  // ~5 KB, short-lived: PSRAM where there is some
  slot_t *copy = heap_caps_malloc(sizeof(s_slots), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (copy == NULL) {
    copy = malloc(sizeof(s_slots));
  }
  if (copy == NULL) {
    return -1;
  }
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    free(copy);
    return -1;
  }
  // hold the lock for a plain copy only: writers include the player task
  memcpy(copy, s_slots, sizeof(s_slots));
  uint32_t head = s_head;
  xSemaphoreGive(s_mutex);

  size_t n = 0;
  // oldest first: the slot at head is the oldest once the ring has wrapped
  for (size_t i = 0; i < EVENT_LOG_SIZE && n < max; i++) {
    const slot_t *s = &copy[(head + i) % EVENT_LOG_SIZE];
    if (slot_ok(s)) {
      out[n++] = s->e;
    }
  }
  free(copy);
  return (int)n;
}

size_t event_log_runs(event_log_run_t *out, size_t max) {
  run_slot_t copy[EVENT_LOG_RUNS];
  if (s_mutex == NULL || out == NULL ||
      xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    return 0;
  }
  memcpy(copy, s_runs, sizeof(copy));
  xSemaphoreGive(s_mutex);

  size_t n = 0;
  // newest first: walk back from this run
  for (uint32_t b = s_boot; b > 0 && n < max && s_boot - b < EVENT_LOG_RUNS;
       b--) {
    const run_slot_t *r = &copy[b % EVENT_LOG_RUNS];
    if (run_ok(r) && r->r.boot == b) {
      out[n++] = r->r;
    }
  }
  return n;
}

const char *event_log_source_name(uint8_t source) {
  switch (source) {
    case EVENT_SRC_WIFI:
      return "wifi";
    case EVENT_SRC_SNAPCAST:
      return "snapcast";
    case EVENT_SRC_SENDSPIN:
      return "sendspin";
    default:
      return "system";
  }
}

const char *event_log_level_name(uint8_t level) {
  switch (level) {
    case EVENT_WARN:
      return "warn";
    case EVENT_ERROR:
      return "error";
    default:
      return "info";
  }
}

const char *event_log_khz(char *buf, size_t size, uint32_t hz) {
  if (hz % 1000 == 0) {
    snprintf(buf, size, "%lu kHz", (unsigned long)(hz / 1000));
  } else {
    snprintf(buf, size, "%lu.%lu kHz", (unsigned long)(hz / 1000),
             (unsigned long)((hz % 1000) / 100));
  }
  return buf;
}

const char *event_log_reset_reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "reset pin";
    case ESP_RST_SW:
      return "restart (software)";
    case ESP_RST_PANIC:
      return "crash (panic)";
    case ESP_RST_INT_WDT:
      return "crash (interrupt watchdog)";
    case ESP_RST_TASK_WDT:
      return "crash (task watchdog)";
    case ESP_RST_WDT:
      return "crash (watchdog)";
    case ESP_RST_DEEPSLEEP:
      return "wake from deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout (supply voltage dip)";
    case ESP_RST_SDIO:
      return "SDIO reset";
    case ESP_RST_USB:
      return "USB reset";
    case ESP_RST_JTAG:
      return "JTAG reset";
    case ESP_RST_EFUSE:
      return "eFuse error";
    case ESP_RST_PWR_GLITCH:
      return "power glitch";
    case ESP_RST_CPU_LOCKUP:
      return "crash (CPU lockup)";
    default:
      return "unknown";
  }
}
