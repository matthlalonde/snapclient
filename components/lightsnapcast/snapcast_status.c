// Live snapcast state for the Status tab, see snapcast_status.h. Notable
// changes also go to the event log, after the spinlock is released; the
// player task's ones are fixed strings (its stack is small).

#include "snapcast_status.h"

#include <string.h>

#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// zero-initialised: readable before any task has published anything
static snapcast_status_t state;
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;
// "Connected" was logged for this connection (on its first codec header:
// only then is it known to be a snapcast server sending a stream)
static bool announced = false;

#define LOCK() taskENTER_CRITICAL(&state_mux)
#define UNLOCK() taskEXIT_CRITICAL(&state_mux)

// strlcpy-style: always terminated, never overruns
static void copy_str(char *dst, size_t size, const char *src) {
  size_t n = 0;
  if (src != NULL) {
    n = strnlen(src, size - 1);
    memcpy(dst, src, n);
  }
  dst[n] = '\0';
}

void snapcast_status_get(snapcast_status_t *out) {
  LOCK();
  *out = state;
  UNLOCK();
}

void snapcast_status_set_server(const char *host, const char *ip,
                                uint16_t port, snapcast_discovery_t discovery) {
  LOCK();
  copy_str(state.server_host, sizeof(state.server_host), host);
  copy_str(state.server_ip, sizeof(state.server_ip), ip);
  state.server_port = port;
  state.discovery = discovery;
  UNLOCK();
}

void snapcast_status_set_client_id(const char *client_id) {
  LOCK();
  copy_str(state.client_id, sizeof(state.client_id), client_id);
  UNLOCK();
}

void snapcast_status_set_connected(bool connected) {
  int64_t now = esp_timer_get_time();
  char host[sizeof(state.server_host)];
  uint16_t port;
  LOCK();
  bool was = state.connected;
  if (connected && !was) {
    state.connections++;
    state.connected_since_us = now;
  }
  state.connected = connected;
  if (!connected) {
    // the next connection sends its own codec header and settings
    state.have_codec = false;
    state.have_settings = false;
  }
  memcpy(host, state.server_host[0] ? state.server_host : state.server_ip,
         sizeof(host));
  host[sizeof(host) - 1] = '\0';
  port = state.server_port;
  UNLOCK();

  if (!connected && was) {
    if (announced) {
      event_log_add(EVENT_SRC_SNAPCAST, EVENT_WARN,
                    "Lost the server connection");
    } else {
      // the same text every time, so a server that keeps closing right away
      // shows as one counted event
      event_log_addf(EVENT_SRC_SNAPCAST, EVENT_WARN,
                     "Connection to %s:%u ended before any audio", host, port);
    }
    announced = false;
  }
}

void snapcast_status_set_codec(const char *codec, uint32_t sample_rate,
                               uint8_t bits, uint8_t channels) {
  // the last format logged: a reconnect to the same stream adds nothing
  static char logged_codec[sizeof(state.codec)];
  static uint32_t logged_rate;
  static uint8_t logged_bits, logged_channels;

  char host[sizeof(state.server_host)];
  uint16_t port;
  LOCK();
  copy_str(state.codec, sizeof(state.codec), codec);
  state.sample_rate = sample_rate;
  state.bits = bits;
  state.channels = channels;
  state.have_codec = true;
  memcpy(host, state.server_host[0] ? state.server_host : state.server_ip,
         sizeof(host));
  host[sizeof(host) - 1] = '\0';
  port = state.server_port;
  UNLOCK();

  if (!announced) {
    announced = true;
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_INFO, "Connected to %s:%u", host,
                   port);
  }

  char name[sizeof(state.codec)];
  copy_str(name, sizeof(name), codec);
  if (strncmp(logged_codec, name, sizeof(logged_codec)) != 0 ||
      logged_rate != sample_rate || logged_bits != bits ||
      logged_channels != channels) {
    copy_str(logged_codec, sizeof(logged_codec), codec);
    logged_rate = sample_rate;
    logged_bits = bits;
    logged_channels = channels;
    char khz[16];
    char upper[sizeof(logged_codec)];
    for (size_t i = 0; i < sizeof(upper); i++) {
      char c = logged_codec[i];
      upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_INFO,
                   "Stream format: %s, %s, %u-bit, %u channels", upper,
                   event_log_khz(khz, sizeof(khz), sample_rate), bits,
                   channels);
  }
}

void snapcast_status_set_server_settings(uint8_t volume, bool muted,
                                         uint32_t buffer_ms,
                                         int32_t latency_ms) {
  // the last values seen on any connection: a change made while this client
  // was offline shows on reconnect; only the first settings after boot are
  // not a change
  static bool seen = false;
  static uint32_t last_buffer_ms;
  static int32_t last_latency_ms;
  bool latency_changed = seen && last_latency_ms != latency_ms;
  bool buffer_changed = seen && last_buffer_ms != buffer_ms;
  seen = true;
  last_latency_ms = latency_ms;
  last_buffer_ms = buffer_ms;

  LOCK();
  state.volume = volume;
  state.muted = muted;
  state.buffer_ms = buffer_ms;
  state.latency_ms = latency_ms;
  state.have_settings = true;
  UNLOCK();

  if (latency_changed) {
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_INFO,
                   "Server set this client's latency to %ld ms",
                   (long)latency_ms);
  }
  if (buffer_changed) {
    event_log_addf(EVENT_SRC_SNAPCAST, EVENT_INFO,
                   "Server buffer changed to %lu ms", (unsigned long)buffer_ms);
  }
}

void snapcast_status_set_player(snapcast_player_state_t player,
                                uint32_t output_rate, uint8_t output_bits) {
  // logged once per player run: resyncs go PLAYING -> SYNCING -> PLAYING
  static bool played = false;
  bool started = false, stopped = false;

  LOCK();
  if (player == SNAPCAST_PLAYER_PLAYING && !played) {
    played = true;
    started = true;
  } else if (player == SNAPCAST_PLAYER_STOPPED && played) {
    played = false;
    stopped = true;
  }
  state.player = player;
  state.output_rate = output_rate;
  state.output_bits = output_bits;
  if (player != SNAPCAST_PLAYER_PLAYING) {
    state.sync_valid = false;
    state.sync_error_us = 0;
    state.correction = 0;
    state.queued_ms = 0;
  }
  UNLOCK();

  if (started) {
    event_log_add(EVENT_SRC_SNAPCAST, EVENT_INFO, "Started playing");
  } else if (stopped) {
    event_log_add(EVENT_SRC_SNAPCAST, EVENT_INFO, "Stopped playing");
  }
}

void snapcast_status_set_sync(bool valid, int32_t sync_error_us,
                              int8_t correction, uint32_t queued_ms) {
  // the player task's stack headroom, every 256 chunks (~5 s): the scan of
  // its 3 KB stack is cheap at that rate
  static uint32_t calls = 0;
  uint32_t stack_free = 0;
  if ((calls++ & 255) == 0) {
    stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
  }
  LOCK();
  if (stack_free != 0) {
    state.player_stack_free = stack_free;
  }
  state.sync_valid = valid;
  state.sync_error_us = sync_error_us;
  state.correction = correction;
  state.queued_ms = queued_ms;
  UNLOCK();
}

void snapcast_status_frames_corrected(int32_t frames) {
  LOCK();
  if (frames > 0) {
    state.frames_dropped += (uint32_t)frames;
  } else if (frames < 0) {
    state.frames_inserted += (uint32_t)(-frames);
  }
  UNLOCK();
}

void snapcast_status_hard_resync(snapcast_resync_reason_t reason) {
  int64_t now = esp_timer_get_time();
  LOCK();
  state.hard_resyncs++;
  state.last_hard_resync_us = now;
  state.last_resync_reason = reason;
  UNLOCK();

  event_log_add(EVENT_SRC_SNAPCAST, EVENT_WARN,
                reason == SNAPCAST_RESYNC_DRIFT
                    ? "Playback interrupted: sync drifted over 2 ms"
                    : "Playback interrupted: audio ran out (network gap)");
}
