/*
 * web_auth: Basic Auth for the board's web pages, shared by the settings
 * server (normal operation) and rescue mode.
 *
 * User "admin", realm "snapclient" (the same in both modes, so a browser
 * keeps its login through a firmware update). The password is stored as a
 * salted PBKDF2-HMAC-SHA256 hash, NVS key "web_pw" in namespace
 * "snapclient". No stored password means no login.
 *
 * Recently accepted and rejected Authorization headers are remembered (by
 * their SHA-256), so a polling page costs one SHA-256 per request, not a
 * PBKDF2 run. After 5 wrong logins in a row, new attempts get 429 for 1, 2,
 * 4... s (at most 5 minutes); clients already logged in keep working.
 *
 * Never logs the password, its hash or the Authorization header: the UDP
 * logger broadcasts logs on the LAN.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Load the stored password from NVS. Idempotent. Call after
// nvs_flash_init(), before the server starts.
void web_auth_init(void);

// true if a login is required (also when the stored record can't be read:
// then nobody can log in until it is removed).
bool web_auth_password_set(void);

// Call first in a handler. true: go ahead (no password set, a valid login,
// or a client on the rescue access point while AP-exempt). false: it has
// already answered 401, 429, or 503 when the login couldn't be checked (a
// crypto call failed; nothing is remembered or counted, try again); the
// handler just returns.
//
// The 401 carries the WWW-Authenticate challenge unless the request sends
// "X-No-Login-Prompt: 1". Page scripts send it from fetch/XHR, so the
// browser doesn't pop a login prompt that their timeouts then cancel.
bool web_auth_check(httpd_req_t *req);
#define WEB_AUTH_NO_PROMPT_HEADER "X-No-Login-Prompt"

// NULL or "" removes the password. Otherwise 8-64 printable ASCII
// characters (0x20-0x7E), else ESP_ERR_INVALID_ARG. Clears the login caches
// and the failure count. Takes about one PBKDF2 run (tens of ms).
esp_err_t web_auth_set_password(const char *pw);

// The request came in on the rescue access point (192.168.4.x).
bool web_auth_request_on_ap(httpd_req_t *req);

// Rescue mode only: clients on the access point skip the password.
void web_auth_set_ap_exempt(bool on);
