/*
 * web_rescue: WiFi access point + small HTTP API for boards without a
 * serial port. Used by board-probe (as its whole user interface) and by
 * snapclient (rescue / setup mode).
 *
 * HTTP API (port 80):
 *   GET  /api/status       JSON: chip, flash, partitions + installed apps,
 *                          WiFi state, OTA/rollback state
 *   POST /api/wifi         body "ssid=...&password=..." (urlencoded):
 *                          store STA credentials in NVS and connect
 *   POST /api/update       raw body = an ESP32-S3 *app* image (.bin, not a
 *                          merged image): written to the next OTA slot,
 *                          verified, made the boot slot, then reboot
 *   POST /api/boot?label=X make app partition X the boot slot and reboot
 *   POST /api/reboot
 *   GET  /api/backup.bin   the whole physical flash, for safekeeping
 *   GET  /                 built-in setup page (unless the app serves
 *                          its own, see web_rescue_cfg_t.serve_root)
 *
 * The running app is marked valid (rollback cancelled) on the first HTTP
 * request: reaching the page proves the new firmware works.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
  const char *title;            // shown on the page and in /api/status
  const char *ap_ssid_prefix;   // AP SSID = "<prefix>-XXXX" (MAC suffix)
  const char *ap_password;      // >= 8 chars for WPA2, NULL or "" = open
  bool nvs_ok;                  // NVS initialised (else WiFi runs RAM-only)
  bool serve_root;              // serve the built-in page at "/"
} web_rescue_cfg_t;

// Bring up netif + default event loop (if needed), WiFi AP+STA (STA uses
// credentials stored in NVS, if any) and the HTTP server.
esp_err_t web_rescue_start(const web_rescue_cfg_t *cfg);

// The running HTTP server, to register more URI handlers.
httpd_handle_t web_rescue_httpd(void);

// esp_timer time (us) of the last HTTP request, 0 if none yet.
int64_t web_rescue_last_request_us(void);

// Call from app-registered URI handlers: records activity and, on the first
// request, marks the running firmware valid.
void web_rescue_touch(void);

// Request gate for every handler (built-in ones use it too). Rejects with
// 403 and returns false when
//   - the Host header is not an IP literal or a *.local name (blocks DNS
//     rebinding: a hostile web page cannot make the browser talk to us
//     under its own domain), or
//   - `sensitive` and the request lacks the "X-Web-Rescue: 1" header. A
//     cross-site page cannot add a custom header without a CORS preflight,
//     which this server never answers, so forms/fetches from other sites
//     (CSRF) are refused. Use it for anything that changes state, drives
//     pins or returns secrets (the flash backup holds WiFi passwords).
// On success it also calls web_rescue_touch().
bool web_rescue_allow(httpd_req_t *req, bool sensitive);
#define WEB_RESCUE_HEADER "X-Web-Rescue"

// true while a firmware upload is being written
bool web_rescue_update_in_progress(void);

// IP of the STA interface as text ("" if not connected) and the AP SSID.
const char *web_rescue_sta_ip(void);
const char *web_rescue_ap_ssid(void);

// The app image header can claim less flash than the chip has (we build
// for 4 MB so the image boots on every module). Raise the esp_flash bound
// to the physical size so partitions above 4 MB and full backups work.
// Call before anything touches partitions (i.e. before nvs_flash_init()).
void web_rescue_unlock_physical_flash(void);

/* ---- snapclient rescue mode (rescue_mode.c, CONFIG_RESCUE_MODE_ENABLE) */

// true if this boot should run rescue mode instead of the application:
// the rescue button is held, or the previous run asked for it.
bool rescue_mode_requested(void);

// Run the rescue access point until the idle timeout, then reboot. Does
// not return. Call after nvs_flash_init().
void rescue_mode_run(bool nvs_ok) __attribute__((noreturn));

// Reboot into rescue mode now (logs `why`). Does not return.
void rescue_mode_request(const char *why) __attribute__((noreturn));

// Normal operation: watch the button (5 s hold -> rescue) and start the
// "no IP within CONFIG_RESCUE_NO_IP_TIMEOUT_S -> rescue" timer. Call after
// the default event loop exists (after network_if_init()).
void rescue_mode_arm(void);
