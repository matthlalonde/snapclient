#pragma once

#include <stdbool.h>

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
 * output, snapcast, sendspin and device. The strings in `id` are copied.
 */
void status_sections_init(const status_identity_t *id);

/**
 * Event log hooks: status_events_boot() records the start-up (call right
 * after event_log_init()); status_events_start() records WiFi connects,
 * disconnects and access point changes (call right after network_if_init()).
 */
void status_events_boot(void);
void status_events_start(void);

#ifdef __cplusplus
}
#endif
