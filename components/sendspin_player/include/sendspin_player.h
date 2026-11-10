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
} sendspin_status_t;

/**
 * Copy the latest snapshot (refreshed by the Sendspin task about every
 * 250 ms). Returns false when the player is not built in or not started.
 * Any task; never blocks.
 */
bool sendspin_player_get_status(sendspin_status_t *out);

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
