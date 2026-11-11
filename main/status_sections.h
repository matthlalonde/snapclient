#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the settings page's live sections (GET /api/status):
 * output, snapcast, sendspin and device. `name` is the name this boot runs
 * under; it is copied.
 */
void status_sections_init(const char *name);

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
