/**
 * @file settings_manager.h
 * @brief Settings manager (hostname, mDNS, snapserver, players, MQTT)
 *
 * Provides getters/setters persisted to NVS for:
 * - device hostname
 * - snapserver mDNS enabled flag
 * - snapserver host (string)
 * - snapserver port (int)
 * - snapcast and Sendspin on/off flags and display names
 * - whether the snapcast name is still to be sent to the snapserver
 * - the MQTT broker for Home Assistant
 * and tells whether a saved setting waits for a restart.
 */

#ifndef __SETTINGS_MANAGER_H__
#define __SETTINGS_MANAGER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t settings_manager_init(void);

/* true while a saved setting that applies at the next restart differs from
 * what this boot read: hostname, mDNS, snapserver host and port, the player
 * flags and names, and the MQTT settings. Saving the MQTT password always
 * counts, whatever its value. Never touches flash; any task. */
bool settings_restart_needed(void);

/* Hostname */
esp_err_t settings_get_hostname(char *hostname, size_t max_len);
esp_err_t settings_set_hostname(const char *hostname);
esp_err_t settings_clear_hostname(void);

/* mDNS enabled flag */
esp_err_t settings_get_mdns_enabled(bool *enabled);
esp_err_t settings_set_mdns_enabled(bool enabled);
esp_err_t settings_clear_mdns_enabled(void);

/* Snapserver host/port */
esp_err_t settings_get_server_host(char *host, size_t max_len);
esp_err_t settings_set_server_host(const char *host);
esp_err_t settings_clear_server_host(void);

esp_err_t settings_get_server_port(int32_t *port);
esp_err_t settings_set_server_port(int32_t port);
esp_err_t settings_clear_server_port(void);

/* Player on/off flags, read at boot. Both default to on when nothing is
 * saved. The getters read flash once, then answer from RAM. */
esp_err_t settings_get_snapcast_enabled(bool *enabled);
esp_err_t settings_set_snapcast_enabled(bool enabled);
esp_err_t settings_get_sendspin_enabled(bool *enabled);
esp_err_t settings_set_sendspin_enabled(bool enabled);

/* A valid player name: 1-63 bytes of valid UTF-8, no control characters,
 * no leading or trailing space. */
bool settings_name_valid(const char *name);

/* Player display names. The getters give the saved name, "" when none is
 * saved. The setters erase the name for "" or NULL; anything else must pass
 * settings_name_valid or gets ESP_ERR_INVALID_ARG. */
esp_err_t settings_get_snapcast_name(char *name, size_t max_len);
esp_err_t settings_set_snapcast_name(const char *name);
esp_err_t settings_get_sendspin_name(char *name, size_t max_len);
esp_err_t settings_set_sendspin_name(const char *name);

/* The name in use: the saved name, else the hostname. Always ESP_OK (unless
 * name is NULL or max_len is 0). */
esp_err_t settings_get_snapcast_display_name(char *name, size_t max_len);
esp_err_t settings_get_sendspin_display_name(char *name, size_t max_len);

/* true while the snapcast name in use is still to be sent to the snapserver
 * (Client.SetName). Defaults to false. */
esp_err_t settings_get_snapcast_rename_pending(bool *pending);
esp_err_t settings_set_snapcast_rename_pending(bool pending);
/* After a rename: clear the pending flag only if the name in use is still
 * sent_name (a name saved meanwhile needs its own rename). */
esp_err_t settings_clear_snapcast_rename_if(const char *sent_name, bool *cleared);

/* MQTT (Home Assistant), read by mqtt_ha at boot. Missing keys: off, no
 * host, port 1883, no user, no password, pairing switch off. */
#define SETTINGS_MQTT_HOST_MAX 128  /* with the NUL */
#define SETTINGS_MQTT_USER_MAX 64
#define SETTINGS_MQTT_PASS_MAX 129

/* 1-127 bytes: letters, digits, '.' and '-', no '.' or '-' at either end */
bool settings_mqtt_host_valid(const char *host);
/* 1-128 bytes of valid UTF-8, no control characters */
bool settings_mqtt_pass_valid(const char *pass);

esp_err_t settings_get_mqtt_enabled(bool *on);
esp_err_t settings_get_mqtt_host(char *host, size_t max_len);  /* "" when none */
esp_err_t settings_get_mqtt_port(int32_t *port);
esp_err_t settings_get_mqtt_user(char *user, size_t max_len);  /* "" when none */
esp_err_t settings_get_mqtt_pass_set(bool *set);
/* For mqtt_ha only. Never log it; wipe the buffer after use. */
esp_err_t settings_get_mqtt_pass(char *pass, size_t max_len);
/* Home Assistant may change Require pairing */
esp_err_t settings_get_mqtt_pair_sw(bool *allowed);

typedef struct {                 /* NULL / has_* false: keep. "" erases */
  bool has_on;      bool on;
  const char *host;              /* stored lowercase */
  bool has_port;    int32_t port;
  const char *user;              /* settings_name_valid */
  const char *pass;
  bool has_pair_sw; bool pair_sw;
} settings_mqtt_update_t;

/* Checks every field first (ESP_ERR_INVALID_ARG, nothing written). Then,
 * under one mutex hold: a host that differs from the saved one (case does
 * not count) erases the saved password first, and a pass in the same update
 * is then stored. On (given or saved) with no host (given or saved) is
 * ESP_ERR_INVALID_ARG. *pass_cleared (may be NULL): the host change erased
 * the saved password and this update gave no new one. Never logs the
 * password. */
esp_err_t settings_set_mqtt(const settings_mqtt_update_t *u, bool *pass_cleared);

/**
 * Get all settings as a JSON string
 * @param json_out Buffer to store JSON string (caller must allocate)
 * @param max_len Maximum size of output buffer
 * @return ESP_OK on success
 * 
 * Example output (server_host and server_port are left out when not set;
 * the names are "" when none is saved):
 * {
 *   "hostname": "esp32-snapclient",
 *   "mdns_enabled": true,
 *   "server_host": "192.168.1.100",
 *   "server_port": 1704,
 *   "snapcast_enabled": true,
 *   "sendspin_enabled": true,
 *   "snapcast_name": "Living Room",
 *   "sendspin_name": "",
 *   "sendspin_available": true,
 *   "dsp_available": false,
 *   "dac_available": false,
 *   "eq_available": false,
 *   "mqtt_available": true
 * }
 * No MQTT settings here: /capabilities sends CORS headers.
 * Returns ESP_ERR_INVALID_SIZE when the JSON does not fit (it is never cut
 * short); the longest is about 720 bytes, so 1024 always fits.
 */
esp_err_t settings_get_json(char *json_out, size_t max_len);

/**
 * Update settings from a JSON string
 * @param json_in JSON string containing settings to update
 * @return ESP_OK on success
 * 
 * Expected format (all fields optional; an empty name clears it):
 * {
 *   "hostname": "my-device",
 *   "mdns_enabled": false,
 *   "server_host": "192.168.1.100",
 *   "server_port": 1704,
 *   "snapcast_enabled": true,
 *   "sendspin_enabled": false,
 *   "snapcast_name": "Living Room",
 *   "sendspin_name": ""
 * }
 * Like the settings page, it marks the snapcast name for the snapserver when
 * it changes (see settings_set_snapcast_rename_pending).
 */
esp_err_t settings_set_from_json(const char *json_in);

#ifdef __cplusplus
}
#endif

#endif /* __SETTINGS_MANAGER_H__ */
