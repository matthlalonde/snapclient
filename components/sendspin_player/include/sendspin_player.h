#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Live state for the settings page's Status tab. */
typedef struct {
  bool running;          // the client started (identity loaded, listening)
  uint16_t port;
  char client_id[48];    // base64url X25519 public key the server sees
  bool unpaired_access;  // servers without a pairing record may play

  bool connected;  // a server is admitted and active
  bool paired;     // ... with a pairing record (false: unpaired access)
  char server_name[64];
  bool have_group;  // the server sent group details on this connection
                    // (the name may be empty)
  char group_name[64];
  bool group_playing;
  bool time_synced;
  bool have_sync_error;
  uint32_t sync_error_us;  // clock-offset uncertainty (1 sigma)

  bool stream;  // a stream is active
  char codec[12];
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;
  bool output_open;         // owns the I2S output (snapcast paused)
  uint32_t output_rate;     // ... at this format
  uint8_t output_bits;
  bool waiting_for_output;  // stream active, snapcast not released yet
  bool output_failed;       // snapcast let go but the I2S output failed to
                            // open (retried every 500 ms)
  uint8_t volume;
  bool muted;
  uint32_t stack_free;  // lowest free stack of the Sendspin task, bytes
} sendspin_status_t;

/**
 * Copy the latest snapshot (refreshed by the Sendspin task about every
 * 250 ms). Returns false when the player is not built in or not started.
 * Any task; never blocks.
 */
bool sendspin_player_get_status(sendspin_status_t *out);

/** What the Sendspin server says is playing (CONFIG_SENDSPIN_ENABLE_METADATA). */
typedef struct {
  bool valid;             // the server sent track details on this connection
  char title[128];        // texts are cut at a character boundary
  char artist[128];
  char album_artist[128];
  char album[128];
  char artwork_url[512];  // "" if none, too long, or not a full http(s) URL
  uint16_t year;          // 0 = unknown
  uint16_t track;         // 0 = unknown
  bool has_progress;
  uint32_t progress_ms;   // kept current about every 250 ms
  uint32_t duration_ms;   // 0 = unknown or a live stream
} sendspin_now_playing_t;

/**
 * Copy what is playing. Returns false when nothing is known (no metadata
 * yet, no server, or metadata not built in). Any task; never blocks. ~1 KB:
 * not for a stack, use a heap or static buffer.
 */
bool sendspin_player_get_now_playing(sendspin_now_playing_t *out);

/**
 * Start the Sendspin player (Music Assistant's synchronized audio protocol)
 * under the given display name. It listens for servers on
 * CONFIG_SENDSPIN_PLAYER_PORT and announces itself over mDNS as
 * _sendspin._tcp.
 *
 * While a Sendspin stream plays it owns the I2S output: snapcast audio is
 * dropped until 2 s after the stream ends, then snapcast plays again.
 *
 * Call once, after init_player() and net_mdns_register().
 */
void sendspin_player_start(const char *name);

#ifdef __cplusplus
}
#endif
