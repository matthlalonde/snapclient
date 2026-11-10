#ifndef __UI_HTTP_SERVER_H__
#define __UI_HTTP_SERVER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <esp_err.h>

#include "cJSON.h"

void init_http_server_task(void);

/*
 * Status tab: GET /api/status answers with one JSON object holding a
 * sub-object per registered section, {"<name>": {...}, ...}, in the order
 * they were added. The callback fills its section and runs on the web
 * server's task (4 KB stack) every couple of seconds while the tab is open:
 * copy a snapshot, never block or wait for a lock held across audio work.
 * `name` must stay valid forever (a string literal). Any task, any time.
 */
typedef void (*ui_status_section_fn)(cJSON *section);
esp_err_t ui_http_add_status_section(const char *name, ui_status_section_fn fn);

typedef struct {
  char key[16];
  int32_t int_value;
} URL_t;

/*
 * Save a single integer parameter to NVS under namespace "ui_http".
 */
esp_err_t ui_http_save_param(const char *name, int32_t value);

/*
 * Load a single integer parameter from NVS. Returns ESP_OK on success or
 * ESP_ERR_NVS_NOT_FOUND if not present.
 */
esp_err_t ui_http_load_param(const char *name, int32_t *value);

#ifdef __cplusplus
}
#endif

#endif  // __UI_HTTP_SERVER_H__
