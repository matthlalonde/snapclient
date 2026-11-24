/**
 * @file settings_manager.h
 * @brief Settings manager (hostname, mDNS, snapserver, players)
 *
 * Provides getters/setters persisted to NVS for:
 * - device hostname
 * - snapserver mDNS enabled flag
 * - snapserver host (string)
 * - snapserver port (int)
 * - snapcast and Sendspin on/off flags and display names
 * - whether the snapcast name is still to be sent to the snapserver
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
 * saved. */
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
 *   "eq_available": false
 * }
 * Returns ESP_ERR_INVALID_SIZE when the JSON does not fit (it is never cut
 * short); 1024 bytes always fits.
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
