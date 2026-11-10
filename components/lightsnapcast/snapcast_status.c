// Live snapcast state for the Status tab, see snapcast_status.h.

#include "snapcast_status.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// zero-initialised: readable before any task has published anything
static snapcast_status_t state;
static portMUX_TYPE state_mux = portMUX_INITIALIZER_UNLOCKED;

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
  LOCK();
  if (connected && !state.connected) {
    state.connections++;
    state.connected_since_us = now;
  }
  state.connected = connected;
  if (!connected) {
    // the next connection sends its own codec header and settings
    state.have_codec = false;
    state.have_settings = false;
  }
  UNLOCK();
}

void snapcast_status_set_codec(const char *codec, uint32_t sample_rate,
                               uint8_t bits, uint8_t channels) {
  LOCK();
  copy_str(state.codec, sizeof(state.codec), codec);
  state.sample_rate = sample_rate;
  state.bits = bits;
  state.channels = channels;
  state.have_codec = true;
  UNLOCK();
}

void snapcast_status_set_server_settings(uint8_t volume, bool muted,
                                         uint32_t buffer_ms,
                                         int32_t latency_ms) {
  LOCK();
  state.volume = volume;
  state.muted = muted;
  state.buffer_ms = buffer_ms;
  state.latency_ms = latency_ms;
  state.have_settings = true;
  UNLOCK();
}

void snapcast_status_set_player(snapcast_player_state_t player,
                                uint32_t output_rate, uint8_t output_bits) {
  LOCK();
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
}

void snapcast_status_set_sync(bool valid, int32_t sync_error_us,
                              int8_t correction, uint32_t queued_ms) {
  LOCK();
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
}
