/**
 * @file hostname_manager.c
 * @brief Hostname management implementation
 */

#include "settings_manager.h"

#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "cJSON.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "snapclient";
static const char *NVS_KEY_HOSTNAME = "hostname";
static const char *NVS_KEY_MDNS = "mdns";        // int32 0/1
static const char *NVS_KEY_SERVER_HOST = "server_host"; // string
static const char *NVS_KEY_SERVER_PORT = "server_port"; // int32
static const char *NVS_KEY_SNAP_ON = "snap_on";         // int32 0/1, missing = on
static const char *NVS_KEY_SS_ON = "ss_on";             // int32 0/1, missing = on
static const char *NVS_KEY_SNAP_NAME = "snap_name";     // string, missing = hostname
static const char *NVS_KEY_SS_NAME = "ss_name";         // string, missing = hostname
static const char *NVS_KEY_SNAP_RENAME = "snap_rename"; // int32, 1 = still to send to the snapserver

// Mutex for thread-safe NVS access
static SemaphoreHandle_t hostname_mutex = NULL;

/**
 * @brief Validate hostname according to RFC 1123
 */
static bool validate_hostname(const char *hostname) {
    if (!hostname) {
        ESP_LOGD(TAG, "%s: hostname=NULL", __func__);
        return false;
    }
    
    size_t len = strlen(hostname);
    
    // Length check: 1-63 characters
    if (len == 0 || len > 63) {
        return false;
    }
    
    // Cannot start or end with hyphen
    if (hostname[0] == '-' || hostname[len - 1] == '-') {
        return false;
    }
    
    // Check each character: must be alphanumeric or hyphen
    for (size_t i = 0; i < len; i++) {
        char c = hostname[i];
        if (!isalnum((unsigned char)c) && c != '-') {
            ESP_LOGD(TAG, "%s: invalid char '%c' at pos %u", __func__, c, (unsigned) i);
            return false;
        }
    }
    ESP_LOGD(TAG, "%s: hostname '%s' valid", __func__, hostname);
    return true;
}

/* The hostname used when none is saved */
static void default_hostname(char *hostname, size_t max_len) {
#ifdef CONFIG_SNAPCLIENT_NAME
    strncpy(hostname, CONFIG_SNAPCLIENT_NAME, max_len - 1);
    hostname[max_len - 1] = '\0';
    ESP_LOGD(TAG, "%s: Hostname from CONFIG: %s", __func__, hostname);
#else
    // Ultimate fallback
    strncpy(hostname, "esp32-snapclient", max_len - 1);
    hostname[max_len - 1] = '\0';
    ESP_LOGW(TAG, "%s: Using default hostname: %s", __func__, hostname);
#endif
}

esp_err_t settings_manager_init(void) {
    if (hostname_mutex == NULL) {
        hostname_mutex = xSemaphoreCreateMutex();
        if (hostname_mutex == NULL) {
            ESP_LOGE(TAG, "%s: Failed to create mutex", __func__);
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "%s: Snapclient manager initialized", __func__);
    return ESP_OK;
}

esp_err_t settings_get_hostname(char *hostname, size_t max_len) {
    if (!hostname || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Hostname manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    esp_err_t err = ESP_OK;
    nvs_handle_t nvs_handle;
    
    // Try to open NVS
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        // Try to read hostname from NVS
        size_t required_size = max_len;
        err = nvs_get_str(nvs_handle, NVS_KEY_HOSTNAME, hostname, &required_size);
        nvs_close(nvs_handle);
        
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: Hostname from NVS: %s", __func__, hostname);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }
    
    // Fall back to CONFIG default
    default_hostname(hostname, max_len);
    err = ESP_OK;

    xSemaphoreGive(hostname_mutex);
    return err;
}

esp_err_t settings_set_hostname(const char *hostname) {
    if (!hostname) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Validate hostname
    if (!validate_hostname(hostname)) {
        ESP_LOGE(TAG, "%s: Invalid hostname: %s", __func__, hostname);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Snapclient manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: Failed to open NVS: %s", __func__, esp_err_to_name(err));
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    // Write hostname to NVS
    err = nvs_set_str(nvs_handle, NVS_KEY_HOSTNAME, hostname);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    xSemaphoreGive(hostname_mutex);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: Hostname saved to NVS: %s", __func__, hostname);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save hostname: %s", __func__, esp_err_to_name(err));
    }

    return err;
}

esp_err_t settings_clear_hostname(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Hostname manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Acquire mutex
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        // If namespace doesn't exist, that's fine - already cleared
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    // Erase the key
    err = nvs_erase_key(nvs_handle, NVS_KEY_HOSTNAME);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(nvs_handle);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: Hostname cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear hostname: %s", __func__, esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    xSemaphoreGive(hostname_mutex);

    return err;
}

/* mDNS enabled flag */
esp_err_t settings_get_mdns_enabled(bool *enabled) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!enabled) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, NVS_KEY_MDNS, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *enabled = (v != 0);
            ESP_LOGD(TAG, "%s: mdns from NVS: %d", __func__, *enabled ? 1 : 0);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }
    // Not present in NVS. Check sdkconfig if available, otherwise default true
#ifndef CONFIG_SNAPSERVER_USE_MDNS
    *enabled = false;
#else
    *enabled = true;
#endif
    ESP_LOGD(TAG, "%s: mdns from CONFIG_SNAPSERVER_USE_MDNS: %d", __func__, *enabled ? 1 : 0);
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_mdns_enabled(bool enabled) {
    ESP_LOGD(TAG, "%s: enabled=%d", __func__, enabled ? 1 : 0);
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    err = nvs_set_i32(h, NVS_KEY_MDNS, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: mdns saved: %d", __func__, enabled ? 1 : 0);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save mdns: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_mdns_enabled(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Settings manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    err = nvs_erase_key(h, NVS_KEY_MDNS);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: mdns cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear mdns: %s", __func__, esp_err_to_name(err));
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    return err;
}

/* Snapserver host/port */
esp_err_t settings_get_server_host(char *host, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!host || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, NVS_KEY_SERVER_HOST, host, &required);
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGD(TAG, "%s: server_host from NVS: %s", __func__, host);
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
            xSemaphoreGive(hostname_mutex);
            return err;
        }
    }

    // not present in NVS -> check sdkconfig fallbacks or empty
#ifdef CONFIG_SNAPSERVER_HOST
    strncpy(host, CONFIG_SNAPSERVER_HOST, max_len - 1);
    host[max_len - 1] = '\0';
    ESP_LOGD(TAG, "%s: server_host from CONFIG_SNAPSERVER_HOST: %s", __func__, host);
#else
    host[0] = '\0';
    ESP_LOGD(TAG, "%s: server_host not set (default empty)", __func__);
#endif
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_server_host(const char *host) {
    ESP_LOGD(TAG, "%s: host='%s'", __func__, host ? host : "(null)");
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    if (host == NULL || host[0] == '\0') {
        // erase (clearing a host that was never set is fine)
        err = nvs_erase_key(h, NVS_KEY_SERVER_HOST);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
    } else {
        err = nvs_set_str(h, NVS_KEY_SERVER_HOST, host);
        if (err == ESP_OK) err = nvs_commit(h);
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: server_host saved: %s", __func__, host ? host : "(erased)");
    } else {
        ESP_LOGE(TAG, "%s: Failed to save server_host: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_server_host(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    return settings_set_server_host(NULL);
}

esp_err_t settings_get_server_port(int32_t *port) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!port) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, NVS_KEY_SERVER_PORT, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *port = v;
            xSemaphoreGive(hostname_mutex);
            return ESP_OK;
        }
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: NVS read error: %s", __func__, esp_err_to_name(err));
        }
    }

    // not present in NVS -> check sdkconfig fallbacks or default 0
#ifdef CONFIG_SNAPSERVER_PORT
    *port = CONFIG_SNAPSERVER_PORT;
    ESP_LOGD(TAG, "%s: server_port from CONFIG_SNAPSERVER_PORT: %ld", __func__, (long)*port);
#else
    *port = 0;
    ESP_LOGD(TAG, "%s: server_port not set (default 0)", __func__);
#endif
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

esp_err_t settings_set_server_port(int32_t port) {
    ESP_LOGD(TAG, "%s: port=%ld", __func__, (long)port);
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return err;
    }

    err = nvs_set_i32(h, NVS_KEY_SERVER_PORT, port);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: server_port saved: %ld", __func__, (long)port);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save server_port: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_clear_server_port(void) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    if (!hostname_mutex) {
        ESP_LOGE(TAG, "%s: Settings manager not initialized", __func__);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "%s: Failed to acquire mutex (timeout)", __func__);
        return ESP_ERR_TIMEOUT;
    }
    
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(hostname_mutex);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
    }

    err = nvs_erase_key(h, NVS_KEY_SERVER_PORT);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_commit(h);
        err = ESP_OK;
        ESP_LOGI(TAG, "%s: server_port cleared from NVS", __func__);
    } else {
        ESP_LOGE(TAG, "%s: Failed to clear server_port: %s", __func__, esp_err_to_name(err));
    }

    nvs_close(h);
    xSemaphoreGive(hostname_mutex);
    return err;
}

/* An int32 0/1 flag; `def` when the key is missing or unreadable, and on an
 * error return */
static esp_err_t get_flag(const char *key, bool def, bool *value) {
    if (!value) return ESP_ERR_INVALID_ARG;
    *value = def;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        int32_t v = 0;
        err = nvs_get_i32(h, key, &v);
        nvs_close(h);
        if (err == ESP_OK) {
            *value = (v != 0);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "%s: %s: NVS read error: %s", __func__, key, esp_err_to_name(err));
        }
    }
    ESP_LOGD(TAG, "%s: %s=%d", __func__, key, *value ? 1 : 0);
    xSemaphoreGive(hostname_mutex);
    return ESP_OK;
}

static esp_err_t set_flag(const char *key, bool value) {
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i32(h, key, value ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: %s saved: %d", __func__, key, value ? 1 : 0);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save %s: %s", __func__, key, esp_err_to_name(err));
    }
    return err;
}

/* A saved name, "" when none is saved (and on a read error) */
static esp_err_t get_name(const char *key, char *name, size_t max_len) {
    if (!name || max_len == 0) return ESP_ERR_INVALID_ARG;
    name[0] = '\0';
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t required = max_len;
        err = nvs_get_str(h, key, name, &required);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;  // no namespace or no key: nothing saved
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s: NVS read error: %s", __func__, key, esp_err_to_name(err));
    }
    if (err != ESP_OK) name[0] = '\0';
    xSemaphoreGive(hostname_mutex);
    return err;
}

/* "" or NULL erases the name */
static esp_err_t set_name(const char *key, const char *name) {
    bool erase = (name == NULL || name[0] == '\0');
    if (!erase && !settings_name_valid(name)) {
        ESP_LOGE(TAG, "%s: Invalid %s", __func__, key);
        return ESP_ERR_INVALID_ARG;
    }
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        if (erase) {
            err = nvs_erase_key(h, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        } else {
            err = nvs_set_str(h, key, name);
        }
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(hostname_mutex);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s: %s saved: %s", __func__, key, erase ? "(erased)" : name);
    } else {
        ESP_LOGE(TAG, "%s: Failed to save %s: %s", __func__, key, esp_err_to_name(err));
    }
    return err;
}

/* The saved name, else the hostname; always ESP_OK for valid arguments */
static esp_err_t get_display_name(const char *key, char *name, size_t max_len) {
    if (!name || max_len == 0) return ESP_ERR_INVALID_ARG;
    // get_name gives the mutex back before settings_get_hostname takes it
    if (get_name(key, name, max_len) == ESP_OK && name[0] != '\0') return ESP_OK;
    if (settings_get_hostname(name, max_len) != ESP_OK) default_hostname(name, max_len);
    return ESP_OK;
}

bool settings_name_valid(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len == 0 || len > 63) return false;
    if (name[0] == ' ' || name[len - 1] == ' ') return false;

    const unsigned char *s = (const unsigned char *)name;
    size_t i = 0;
    while (i < len) {
        unsigned char c = s[i];
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return false;  // control characters
            i++;
            continue;
        }
        // lead byte: how many continuation bytes follow, and the smallest
        // code point that needs this many (anything less is overlong)
        size_t n;
        uint32_t cp, min;
        if (c >= 0xc2 && c <= 0xdf) {
            n = 1; cp = c & 0x1f; min = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            n = 2; cp = c & 0x0f; min = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            n = 3; cp = c & 0x07; min = 0x10000;
        } else {
            return false;  // a stray continuation byte, 0xc0, 0xc1 or 0xf5-0xff
        }
        for (size_t k = 1; k <= n; k++) {
            // the terminating NUL is not a continuation byte, so this also
            // stops at the end of the string
            if ((s[i + k] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (s[i + k] & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        if (cp >= 0x80 && cp <= 0x9f) return false;  // C1 control characters
        i += n + 1;
    }
    return true;
}

/* Player on/off flags */
esp_err_t settings_get_snapcast_enabled(bool *enabled) {
    return get_flag(NVS_KEY_SNAP_ON, true, enabled);
}

esp_err_t settings_set_snapcast_enabled(bool enabled) {
    return set_flag(NVS_KEY_SNAP_ON, enabled);
}

esp_err_t settings_get_sendspin_enabled(bool *enabled) {
    return get_flag(NVS_KEY_SS_ON, true, enabled);
}

esp_err_t settings_set_sendspin_enabled(bool enabled) {
    return set_flag(NVS_KEY_SS_ON, enabled);
}

/* Player names */
esp_err_t settings_get_snapcast_name(char *name, size_t max_len) {
    return get_name(NVS_KEY_SNAP_NAME, name, max_len);
}

esp_err_t settings_set_snapcast_name(const char *name) {
    return set_name(NVS_KEY_SNAP_NAME, name);
}

esp_err_t settings_get_sendspin_name(char *name, size_t max_len) {
    return get_name(NVS_KEY_SS_NAME, name, max_len);
}

esp_err_t settings_set_sendspin_name(const char *name) {
    return set_name(NVS_KEY_SS_NAME, name);
}

esp_err_t settings_get_snapcast_display_name(char *name, size_t max_len) {
    return get_display_name(NVS_KEY_SNAP_NAME, name, max_len);
}

esp_err_t settings_get_sendspin_display_name(char *name, size_t max_len) {
    return get_display_name(NVS_KEY_SS_NAME, name, max_len);
}

/* Snapcast name still to be sent to the snapserver */
esp_err_t settings_get_snapcast_rename_pending(bool *pending) {
    return get_flag(NVS_KEY_SNAP_RENAME, false, pending);
}

esp_err_t settings_set_snapcast_rename_pending(bool pending) {
    return set_flag(NVS_KEY_SNAP_RENAME, pending);
}

esp_err_t settings_clear_snapcast_rename_if(const char *sent_name, bool *cleared) {
    if (cleared) *cleared = false;
    if (!sent_name) return ESP_ERR_INVALID_ARG;
    if (!hostname_mutex) return ESP_ERR_INVALID_STATE;
    // one mutex hold for the compare and the clear, so a name saved in between
    // keeps its pending rename
    if (xSemaphoreTake(hostname_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        // the name in use, as settings_get_snapcast_display_name works it out
        char name[64] = "";
        size_t len = sizeof(name);
        if (nvs_get_str(h, NVS_KEY_SNAP_NAME, name, &len) != ESP_OK || name[0] == '\0') {
            len = sizeof(name);
            if (nvs_get_str(h, NVS_KEY_HOSTNAME, name, &len) != ESP_OK || name[0] == '\0') {
                default_hostname(name, sizeof(name));
            }
        }
        if (strcmp(name, sent_name) == 0) {
            err = nvs_erase_key(h, NVS_KEY_SNAP_RENAME);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            if (err == ESP_OK) err = nvs_commit(h);
            if (err == ESP_OK && cleared) *cleared = true;
        }
        nvs_close(h);
    }
    xSemaphoreGive(hostname_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", __func__, esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_get_json(char *json_out, size_t max_len) {
    ESP_LOGD(TAG, "%s: entered", __func__);
    
    if (!json_out || max_len == 0) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to create JSON object", __func__);
        return ESP_ERR_NO_MEM;
    }

    // Get hostname
    char hostname[64] = {0};
    if (settings_get_hostname(hostname, sizeof(hostname)) == ESP_OK) {
        cJSON_AddStringToObject(root, "hostname", hostname);
    }

    // Get mdns enabled
    bool mdns = true;
    if (settings_get_mdns_enabled(&mdns) == ESP_OK) {
        cJSON_AddBoolToObject(root, "mdns_enabled", mdns);
    }

    // Get server host
    char host[128] = {0};
    if (settings_get_server_host(host, sizeof(host)) == ESP_OK && host[0] != '\0') {
        cJSON_AddStringToObject(root, "server_host", host);
    }

    // Get server port
    int32_t port = 0;
    if (settings_get_server_port(&port) == ESP_OK && port != 0) {
        cJSON_AddNumberToObject(root, "server_port", port);
    }

    // Player on/off flags (both on unless saved off)
    bool on = true;
    settings_get_snapcast_enabled(&on);
    cJSON_AddBoolToObject(root, "snapcast_enabled", on);
    on = true;
    settings_get_sendspin_enabled(&on);
    cJSON_AddBoolToObject(root, "sendspin_enabled", on);

    // Saved player names, "" when none is saved (the hostname is used then)
    char name[64];
    settings_get_snapcast_name(name, sizeof(name));
    cJSON_AddStringToObject(root, "snapcast_name", name);
    settings_get_sendspin_name(name, sizeof(name));
    cJSON_AddStringToObject(root, "sendspin_name", name);

    // Add Sendspin availability flag
#if CONFIG_SENDSPIN_PLAYER_ENABLE
    cJSON_AddBoolToObject(root, "sendspin_available", true);
#else
    cJSON_AddBoolToObject(root, "sendspin_available", false);
#endif

    // Add DSP availability flag
#if CONFIG_USE_DSP_PROCESSOR
    cJSON_AddBoolToObject(root, "dsp_available", true);
#else
    cJSON_AddBoolToObject(root, "dsp_available", false);
#endif

    // Add DAC availability flag
#if CONFIG_DAC_TAS5805M
    cJSON_AddBoolToObject(root, "dac_available", true);
#else
    cJSON_AddBoolToObject(root, "dac_available", false);
#endif

    // Add EQ availability flag
#if defined(CONFIG_DAC_TAS5805M_EQ_SUPPORT)
    cJSON_AddBoolToObject(root, "eq_available", true);
#else
    cJSON_AddBoolToObject(root, "eq_available", false);
#endif

    // Render to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        ESP_LOGE(TAG, "%s: Failed to render JSON", __func__);
        return ESP_ERR_NO_MEM;
    }

    if (strlen(json_str) >= max_len) {
        ESP_LOGE(TAG, "%s: JSON too large for buffer", __func__);
        cJSON_free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strncpy(json_out, json_str, max_len - 1);
    json_out[max_len - 1] = '\0';
    cJSON_free(json_str);

    ESP_LOGV(TAG, "%s: JSON generated: %s", __func__, json_out);
    return ESP_OK;
}

esp_err_t settings_set_from_json(const char *json_in) {
    ESP_LOGD(TAG, "%s: json=%s", __func__, json_in);
    
    if (!json_in) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_Parse(json_in);
    if (!root) {
        ESP_LOGE(TAG, "%s: Failed to parse JSON", __func__);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    bool hostname_saved = false;
    bool snap_name_saved = false;

    // Update hostname if present
    cJSON *hostname = cJSON_GetObjectItem(root, "hostname");
    if (cJSON_IsString(hostname) && hostname->valuestring) {
        esp_err_t save_err = settings_set_hostname(hostname->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save hostname", __func__);
            err = save_err;
        } else {
            hostname_saved = true;
        }
    }

    // Update mdns_enabled if present
    cJSON *mdns = cJSON_GetObjectItem(root, "mdns_enabled");
    if (cJSON_IsBool(mdns)) {
        esp_err_t save_err = settings_set_mdns_enabled(cJSON_IsTrue(mdns));
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save mdns_enabled", __func__);
            err = save_err;
        }
    }

    // Update server_host if present
    cJSON *host = cJSON_GetObjectItem(root, "server_host");
    if (cJSON_IsString(host) && host->valuestring) {
        esp_err_t save_err = settings_set_server_host(host->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save server_host", __func__);
            err = save_err;
        }
    }

    // Update server_port if present
    cJSON *port = cJSON_GetObjectItem(root, "server_port");
    if (cJSON_IsNumber(port)) {
        esp_err_t save_err = settings_set_server_port((int32_t)port->valueint);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save server_port", __func__);
            err = save_err;
        }
    }

    // Update the player on/off flags if present
    cJSON *snap_on = cJSON_GetObjectItem(root, "snapcast_enabled");
    if (cJSON_IsBool(snap_on)) {
        esp_err_t save_err = settings_set_snapcast_enabled(cJSON_IsTrue(snap_on));
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save snapcast_enabled", __func__);
            err = save_err;
        }
    }

    cJSON *ss_on = cJSON_GetObjectItem(root, "sendspin_enabled");
    if (cJSON_IsBool(ss_on)) {
        esp_err_t save_err = settings_set_sendspin_enabled(cJSON_IsTrue(ss_on));
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save sendspin_enabled", __func__);
            err = save_err;
        }
    }

    // Update the player names if present ("" clears)
    cJSON *snap_name = cJSON_GetObjectItem(root, "snapcast_name");
    if (cJSON_IsString(snap_name) && snap_name->valuestring) {
        esp_err_t save_err = settings_set_snapcast_name(snap_name->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save snapcast_name", __func__);
            err = save_err;
        } else {
            snap_name_saved = true;
        }
    }

    cJSON *ss_name = cJSON_GetObjectItem(root, "sendspin_name");
    if (cJSON_IsString(ss_name) && ss_name->valuestring) {
        esp_err_t save_err = settings_set_sendspin_name(ss_name->valuestring);
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "%s: Failed to save sendspin_name", __func__);
            err = save_err;
        }
    }

    // A new snapcast name, or a new hostname while no snapcast name is
    // saved, is sent to the snapserver at the next boot
    bool rename = snap_name_saved;
    if (hostname_saved && !rename) {
        char name[64] = "";
        settings_get_snapcast_name(name, sizeof(name));
        rename = (name[0] == '\0');
    }
    if (rename) {
        esp_err_t save_err = settings_set_snapcast_rename_pending(true);
        if (save_err != ESP_OK) err = save_err;
    }

    cJSON_Delete(root);
    return err;
}
