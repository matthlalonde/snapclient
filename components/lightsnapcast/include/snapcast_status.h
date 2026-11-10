// Live snapcast state for the settings page's Status tab.
//
// The network task (server, connection, stream format, server settings) and
// the player task (sync health) publish here; the web server copies a
// snapshot with snapcast_status_get(). Every call is short and never blocks:
// the state sits behind a spinlock, so the player can update it per chunk.

#ifndef __SNAPCAST_STATUS_H__
#define __SNAPCAST_STATUS_H__

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SNAPCAST_DISCOVERY_NONE = 0,
  SNAPCAST_DISCOVERY_MDNS,    // found over mDNS
  SNAPCAST_DISCOVERY_STATIC,  // host/port from General Settings or the build
} snapcast_discovery_t;

typedef enum {
  SNAPCAST_PLAYER_STOPPED = 0,  // no player task (no stream yet, or it ended)
  SNAPCAST_PLAYER_SYNCING,      // waiting for clock sync / the start time
  SNAPCAST_PLAYER_PLAYING,
} snapcast_player_state_t;

typedef enum {
  SNAPCAST_RESYNC_NONE = 0,
  SNAPCAST_RESYNC_QUEUE_EMPTY,  // the audio queue ran dry (network gap)
  SNAPCAST_RESYNC_DRIFT,        // sync error beyond the hard limit (2 ms)
} snapcast_resync_reason_t;

typedef struct {
  // ---- network task ----
  char server_host[64];  // as configured, or the mDNS host name
  char server_ip[48];
  uint16_t server_port;
  snapcast_discovery_t discovery;
  bool connected;              // TCP up and Hello sent
  int64_t connected_since_us;  // esp_timer time of the current connection
  uint32_t connections;        // successful connections since boot
  char client_id[18];          // the MAC the server lists this client under

  bool have_codec;  // a codec header arrived on this connection
  char codec[8];    // "flac", "opus", "pcm"
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;

  bool have_settings;  // a server settings message arrived on it
  uint8_t volume;
  bool muted;
  uint32_t buffer_ms;
  int32_t latency_ms;  // per-client latency offset set on the server

  // ---- player task ----
  snapcast_player_state_t player;
  uint32_t output_rate;  // the format the I2S channel runs at while playing
  uint8_t output_bits;
  bool sync_valid;        // the long median filter is full
  int32_t sync_error_us;  // long median: + late, - early
  int8_t correction;      // last decision: +1 catch up, -1 hold back, 0 none
  uint32_t frames_dropped;   // sample insertion: frames skipped (catch up)
  uint32_t frames_inserted;  // and frames repeated (hold back), since boot
  uint32_t queued_ms;        // audio waiting in the chunk queue
  uint32_t hard_resyncs;     // playback interruptions since boot
  int64_t last_hard_resync_us;
  snapcast_resync_reason_t last_resync_reason;
} snapcast_status_t;

void snapcast_status_get(snapcast_status_t *out);

// network task
void snapcast_status_set_server(const char *host, const char *ip,
                                uint16_t port, snapcast_discovery_t discovery);
void snapcast_status_set_client_id(const char *client_id);
void snapcast_status_set_connected(bool connected);
void snapcast_status_set_codec(const char *codec, uint32_t sample_rate,
                               uint8_t bits, uint8_t channels);
void snapcast_status_set_server_settings(uint8_t volume, bool muted,
                                         uint32_t buffer_ms,
                                         int32_t latency_ms);

// player task
void snapcast_status_set_player(snapcast_player_state_t state,
                                uint32_t output_rate, uint8_t output_bits);
void snapcast_status_set_sync(bool valid, int32_t sync_error_us,
                              int8_t correction, uint32_t queued_ms);
// sample insertion applied: frames > 0 skipped (catch up), < 0 repeated
void snapcast_status_frames_corrected(int32_t frames);
void snapcast_status_hard_resync(snapcast_resync_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif  // __SNAPCAST_STATUS_H__
