#pragma once

#include <stdbool.h>

#include "sdkconfig.h"
#if CONFIG_HA_MQTT_ENABLE
#include "mqtt_ha.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

// What this boot runs as: read once at start-up, so a change from General
// Settings shows after a restart
typedef struct {
  const char *hostname;        // mDNS host name
  bool snapcast_enabled;
  const char *snapcast_name;   // name in use (never empty)
  bool sendspin_enabled;
  const char *sendspin_name;
} status_identity_t;

/**
 * Register the settings page's live sections (GET /api/status):
 * output, snapcast, sendspin, device, wifi, events and mqtt (when built in).
 * The strings in `id` are copied.
 */
void status_sections_init(const status_identity_t *id);

/**
 * Event log hooks: status_events_boot() records the start-up (call right
 * after event_log_init()); status_events_start() records WiFi connects,
 * disconnects and access point changes (call right after network_if_init()).
 */
void status_events_boot(void);
void status_events_start(void);

/** Who owns the audio output now: "Snapcast", "Sendspin" or "Idle". Any task. */
const char *status_output_source(void);

#if CONFIG_HA_MQTT_ENABLE
/**
 * Home Assistant's callbacks (mqtt_ha_config_t), for mqtt_ha's task only.
 * status_mqtt_state() fills the same words the Status tab shows;
 * status_mqtt_command() runs a command and logs why when it refuses one.
 */
void status_mqtt_state(mqtt_ha_state_t *out);
bool status_mqtt_command(mqtt_ha_cmd_t cmd);
#endif

#ifdef __cplusplus
}
#endif
