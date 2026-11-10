#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Register the settings page's Status tab sections (GET /api/status):
 * output, snapcast, sendspin and device. `name` is the name this boot runs
 * under; it is copied.
 */
void status_sections_init(const char *name);

#ifdef __cplusplus
}
#endif
