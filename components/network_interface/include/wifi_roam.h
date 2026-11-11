// WiFi roaming within one network (CONFIG_WIFI_ROAMING).
//
// At start-up the station joins the strongest access point of the network.
// While the averaged signal stays below CONFIG_WIFI_ROAMING_WEAK_RSSI, a
// task scans for the network now and then (gently: short visits to other
// channels) and moves to an access point at least CONFIG_WIFI_ROAMING_MIN_GAIN
// dB stronger, seen twice in a row. A move drops the connection for a few
// seconds, so scans and moves wait for playback to stop unless the signal is
// very weak. A move that fails goes back to the previous access point and
// skips that target for hours.

#ifndef _WIFI_ROAM_H_
#define _WIFI_ROAM_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_ROAM_MAX_APS 8

typedef enum {
  WIFI_ROAM_OFF = 0,     // not built in
  WIFI_ROAM_OFFLINE,     // no connection
  WIFI_ROAM_MEASURING,   // fewer than 30 s of signal readings
  WIFI_ROAM_GOOD,        // signal above the threshold
  WIFI_ROAM_WEAK,        // weak: scanning now and then
  WIFI_ROAM_SCANNING,
  WIFI_ROAM_WAITING,     // a better access point found, waiting for playback to stop
  WIFI_ROAM_MOVING,
  WIFI_ROAM_COOLDOWN,    // moved recently
  WIFI_ROAM_FAILED,      // the last move failed: back on the old one, retry later
} wifi_roam_state_t;

typedef struct {
  uint8_t bssid[6];
  uint8_t channel;
  int8_t rssi;
} wifi_roam_ap_t;

typedef struct {
  wifi_roam_state_t state;
  int8_t rssi_avg;        // last 30 s, 0 = unknown
  bool strongest_at_connect;  // joins by scanning all channels, strongest first
  uint32_t roams;         // moves since boot
  int64_t last_roam_us;   // esp_timer time, 0 = never
  int64_t last_scan_us;   // 0 = never
  bool scan_pending;      // a manual scan was asked for
  uint8_t ap_count;       // access points of this network in the last scan
  wifi_roam_ap_t aps[WIFI_ROAM_MAX_APS];  // strongest first
} wifi_roam_status_t;

// network_interface: join policy for every connect (all channels, strongest
// first, no pinned access point). Writes the config to RAM only.
void wifi_roam_apply_connect_policy(void);

// network_interface: start the roaming task (no-op without CONFIG_WIFI_ROAMING)
void wifi_roam_start(void);

// true while a move is under way: the reconnect handler then leaves
// connecting to the roaming task
bool wifi_roam_in_progress(void);

// The application tells whether audio is playing, so moves can wait for a
// pause. Optional.
void wifi_roam_set_busy_cb(bool (*busy)(void));

// Scan now (the System tab's "Scan for access points" button). False when one
// ran in the last 20 s or roaming is off.
bool wifi_roam_request_scan(void);

void wifi_roam_get_status(wifi_roam_status_t *out);

const char *wifi_roam_state_name(wifi_roam_state_t state);

#ifdef __cplusplus
}
#endif

#endif  // _WIFI_ROAM_H_
