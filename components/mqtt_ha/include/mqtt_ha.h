// The board in Home Assistant over MQTT (discovery): one device with
// sensors, switches and buttons, and a short list of commands from HA.
//
// main hands over callbacks for the live state and the commands, so this
// component needs nothing from main, the players or the web server. The
// broker settings come from settings_manager and apply at start.
// Without CONFIG_HA_MQTT_ENABLE every function is a stub.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  MQTT_HA_CMD_RESTART,
  MQTT_HA_CMD_PAIR_ALLOW,
  MQTT_HA_CMD_PAIR_STOP,
  MQTT_HA_CMD_REQUIRE_PAIRING_ON,
  MQTT_HA_CMD_REQUIRE_PAIRING_OFF,
  MQTT_HA_CMD_SNAPCAST_ON,
  MQTT_HA_CMD_SNAPCAST_OFF,
  MQTT_HA_CMD_SENDSPIN_ON,
  MQTT_HA_CMD_SENDSPIN_OFF,
} mqtt_ha_cmd_t;

// What HA shows. main fills it; the "state" part is compared every second
// and published on change. No secrets in here, ever. Texts are fixed words
// except pair_server.
typedef struct {
  char source[12];        // "Snapcast", "Sendspin" or "Idle"
  char snapcast[24];      // the Status tab's snapcast badge word
  char sendspin[24];      // the Sendspin badge word; "" without Sendspin
  char pairing[16];       // "closed", "open", "server waiting", "pairing"; "" without Sendspin
  bool snapcast_connected;
  bool sendspin_connected;
  bool snapcast_saved_on; // the saved switch (what applies at the next restart)
  bool sendspin_saved_on;
  bool require_pairing;
  bool restart_needed;
  uint32_t interruptions; // snapcast hard resyncs since start
  uint32_t pair_requests; // goes up by one per new pairing request
  char pair_server[48];   // the server asking ("unknown server" when its name is unknown)
  // diagnostics: not compared
  bool sync_valid;
  int32_t sync_error_us;  // snapcast's signed sync error, valid only when sync_valid
} mqtt_ha_state_t;

typedef struct {
  const char *device_name;  // the running hostname (copied by mqtt_ha_start)
  bool sendspin_built;      // CONFIG_SENDSPIN_PLAYER_ENABLE
  bool sendspin_running;    // Sendspin was started this boot
  bool snapcast_running;    // snapcast was started this boot
  // Fills *out. Called every second from mqtt_ha's task. Must not block for
  // long and must not read flash.
  void (*get_state)(mqtt_ha_state_t *out);
  // Runs one command on mqtt_ha's task (internal-RAM stack; NVS writes are
  // fine). Returns false when refused. mqtt_ha has already checked: not
  // retained, rate limits, the 60 s restart guard and the Require pairing
  // permission. RESTART does not return.
  bool (*command)(mqtt_ha_cmd_t cmd);
  // The board's IPv4 as text; false while it has none.
  bool (*get_ip)(char *buf, size_t len);
} mqtt_ha_config_t;

// Reads the MQTT settings itself. Returns ESP_ERR_INVALID_STATE (and starts
// nothing) when MQTT is off or no host is saved, ESP_ERR_NOT_SUPPORTED when
// not built. Creates its task and returns at once. Registers a shutdown
// handler that publishes "offline" before a restart.
esp_err_t mqtt_ha_start(const mqtt_ha_config_t *cfg);

// "Remove from Home Assistant": queued; false when not connected.
bool mqtt_ha_remove(void);

typedef struct {
  bool enabled;             // started this boot
  bool connected;
  bool removed;             // removed from HA this boot
  bool stopped;             // no MQTT until a restart (removed, or the removal failed)
  const char *state;        // fixed: "off", "waiting for network", "connecting", "connected",
                            // "login refused", "can't reach the broker", "removed",
                            // "stopped"
  char last_error[48];      // fixed texts; "" when none
  char broker[136];         // "host:port", "" when off
  char topic[32];           // "snapclient/<mac12>" (always set)
  char device_id[24];       // "snapclient_<mac12>" (always set)
  uint32_t connects;
  uint32_t connected_s;     // while connected
  uint32_t stack_free;      // our task's lowest free stack, bytes (0 = not running)
  uint32_t mqtt_stack_free; // esp-mqtt's task, measured while it runs
} mqtt_ha_status_t;

// A copy taken under a spinlock: any task, never blocks.
void mqtt_ha_get_status(mqtt_ha_status_t *out);

#ifdef __cplusplus
}
#endif
