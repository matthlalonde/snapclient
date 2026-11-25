/*
 * web_auth: Basic Auth shared by the settings server and rescue mode.
 * See include/web_auth.h.
 */
#include "web_auth.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "web_auth";

#define NVS_NS "snapclient"
#define NVS_KEY "web_pw"
#define USERNAME "admin"
#define CHALLENGE "Basic realm=\"snapclient\", charset=\"UTF-8\""

#define REC_VERSION 1
#define REC_ALGO_PBKDF2_SHA256 1
// Tune on the board (set_password logs the time): aim for 50-100 ms.
#define PBKDF2_ITERATIONS 2048
// A bad record must not stall the single httpd task for long
#define MAX_ITERATIONS 200000
#define PW_MIN 8
#define PW_MAX 64

// "Basic " + base64("admin:" + 64 chars) is about 100 characters
#define MAX_HEADER 200
#define CACHE_SIZE 4
#define FREE_FAILURES 5  // wrong logins in a row before the lockout
#define MAX_LOCK_S 300

typedef struct {
  uint8_t version;     // REC_VERSION
  uint8_t algo;        // REC_ALGO_PBKDF2_SHA256
  uint16_t reserved;
  uint32_t iterations;
  uint8_t salt[16];
  uint8_t dk[32];      // PBKDF2 output
} pw_record_t;
_Static_assert(sizeof(pw_record_t) == 56, "web_pw record layout");

typedef struct {
  uint8_t key[CACHE_SIZE][32];  // SHA-256 of an Authorization header
  int count;
  int next;  // slot to overwrite
} header_cache_t;

// Everything below is guarded by s_mutex (httpd tasks and the rescue
// button loop all get here).
static SemaphoreHandle_t volatile s_mutex;
static portMUX_TYPE s_mutex_init = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;
static bool s_set;          // a login is required
static bool s_unmatchable;  // the stored record can't be used: nobody matches
static pw_record_t s_rec;
static bool s_ap_exempt;
static header_cache_t s_accepted, s_rejected;
static uint32_t s_failures;  // wrong logins in a row
static int64_t s_locked_until_us;

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static void lock(void) {
  if (s_mutex == NULL) {
    // first use: whoever comes first publishes its mutex
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    if (m == NULL) abort();  // a few bytes at start-up
    taskENTER_CRITICAL(&s_mutex_init);
    if (s_mutex == NULL) {
      s_mutex = m;
      m = NULL;
    }
    taskEXIT_CRITICAL(&s_mutex_init);
    if (m) vSemaphoreDelete(m);
  }
  xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_mutex); }

static void cache_clear(header_cache_t *c) {
  mbedtls_platform_zeroize(c, sizeof(*c));
}

static bool cache_has(const header_cache_t *c, const uint8_t key[32]) {
  bool hit = false;
  for (int i = 0; i < c->count; i++) {
    hit |= mbedtls_ct_memcmp(c->key[i], key, 32) == 0;
  }
  return hit;
}

static void cache_add(header_cache_t *c, const uint8_t key[32]) {
  memcpy(c->key[c->next], key, 32);
  c->next = (c->next + 1) % CACHE_SIZE;
  if (c->count < CACHE_SIZE) c->count++;
}

// After any password change: old logins and old rejections no longer apply
static void reset_logins_locked(void) {
  cache_clear(&s_accepted);
  cache_clear(&s_rejected);
  s_failures = 0;
  s_locked_until_us = 0;
}

static bool record_ok(const pw_record_t *r) {
  return r->version == REC_VERSION && r->algo == REC_ALGO_PBKDF2_SHA256 &&
         r->iterations >= 1 && r->iterations <= MAX_ITERATIONS;
}

static void load_locked(void) {
  if (s_loaded) return;
  s_loaded = true;
  s_set = false;
  s_unmatchable = false;
  mbedtls_platform_zeroize(&s_rec, sizeof(s_rec));

  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(TAG, "no settings password set");
    return;
  }
  if (err == ESP_ERR_NVS_NOT_INITIALIZED || err == ESP_ERR_NVS_PART_NOT_FOUND) {
    // only rescue mode without NVS gets here; nothing can be stored either
    ESP_LOGW(TAG, "NVS not usable (%s): no settings password",
             esp_err_to_name(err));
    return;
  }
  if (err == ESP_OK) {
    size_t len = sizeof(s_rec);
    err = nvs_get_blob(h, NVS_KEY, &s_rec, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(TAG, "no settings password set");
      return;
    }
    if (err == ESP_OK && len == sizeof(s_rec) && record_ok(&s_rec)) {
      s_set = true;
      ESP_LOGI(TAG, "settings password set");
      return;
    }
    if (err == ESP_OK) err = ESP_ERR_INVALID_VERSION;
  }
  // fail closed: the button gesture or the rescue access point removes it
  mbedtls_platform_zeroize(&s_rec, sizeof(s_rec));
  s_set = true;
  s_unmatchable = true;
  ESP_LOGE(TAG, "stored settings password unreadable (%s): no login works "
           "until it is removed", esp_err_to_name(err));
}

static int derive(const uint8_t *pw, size_t len, const uint8_t salt[16],
                  uint32_t iterations, uint8_t dk[32]) {
  return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, pw, len, salt, 16,
                                       iterations, 32, dk);
}

typedef enum {
  MATCH_YES,
  MATCH_NO,     // wrong or malformed login
  MATCH_ERROR,  // PBKDF2 failed (out of memory?): we can't tell
} match_t;

// `token`: the base64 part of a Basic header. On MATCH_ERROR, *rc is the
// mbedtls error code.
static match_t credentials_match_locked(const char *token, size_t len,
                                        int *rc) {
  if (s_unmatchable) return MATCH_NO;
  uint8_t dec[160];  // base64 of at most MAX_HEADER characters
  uint8_t dk[32];
  size_t n = 0;
  match_t m = MATCH_NO;
  if (mbedtls_base64_decode(dec, sizeof(dec), &n, (const uint8_t *)token,
                            len) == 0) {
    const uint8_t *colon = memchr(dec, ':', n);
    if (colon) {
      const size_t ulen = colon - dec;
      const bool user_ok = ulen == strlen(USERNAME) &&
                           mbedtls_ct_memcmp(dec, USERNAME, ulen) == 0;
      // always run PBKDF2, so a wrong username takes as long
      *rc = derive(colon + 1, n - ulen - 1, s_rec.salt, s_rec.iterations, dk);
      if (*rc != 0) {
        m = MATCH_ERROR;
      } else {
        const bool pw_ok = mbedtls_ct_memcmp(dk, s_rec.dk, sizeof(dk)) == 0;
        m = user_ok && pw_ok ? MATCH_YES : MATCH_NO;
      }
    }
  }
  mbedtls_platform_zeroize(dec, sizeof(dec));
  mbedtls_platform_zeroize(dk, sizeof(dk));
  return m;
}

/* ------------------------------------------------------------------ */
/* answers                                                             */
/* ------------------------------------------------------------------ */

// Page scripts send WEB_AUTH_NO_PROMPT_HEADER from fetch/XHR: without the
// challenge the browser pops no login prompt for their timeouts to cancel.
// Navigations, curl and Home Assistant don't send it and get the challenge.
static bool wants_prompt(httpd_req_t *req) {
  char v[4] = "";
  return httpd_req_get_hdr_value_str(req, WEB_AUTH_NO_PROMPT_HEADER, v,
                                     sizeof(v)) != ESP_OK ||
         strcmp(v, "1") != 0;
}

static bool send_401(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "text/plain");
  if (wants_prompt(req)) httpd_resp_set_hdr(req, "WWW-Authenticate", CHALLENGE);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, "login required\n");
  return false;
}

// The login couldn't be checked (a crypto call failed): not the client's
// fault, so no prompt and nothing remembered
static bool send_503(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Retry-After", "1");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, "login check failed, try again\n");
  return false;
}

static bool send_429(httpd_req_t *req, uint32_t wait_s) {
  char retry[12];  // kept until the answer is sent below
  snprintf(retry, sizeof(retry), "%lu", (unsigned long)wait_s);
  httpd_resp_set_status(req, "429 Too Many Requests");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Retry-After", retry);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, "too many wrong logins, try again later\n");
  return false;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

void web_auth_init(void) {
  lock();
  load_locked();
  unlock();
}

bool web_auth_password_set(void) {
  lock();
  load_locked();
  const bool set = s_set;
  unlock();
  return set;
}

void web_auth_set_ap_exempt(bool on) {
  lock();
  s_ap_exempt = on;
  unlock();
}

bool web_auth_check(httpd_req_t *req) {
  lock();
  load_locked();
  const bool needed = s_set;
  const bool exempt = s_ap_exempt;
  unlock();
  if (!needed) return true;
  if (exempt && web_auth_request_on_ap(req)) return true;

  // no header (a client that hasn't logged in yet) is not a failed login
  const size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len == 0 || len > MAX_HEADER) return send_401(req);
  char hdr[MAX_HEADER + 1];
  if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) !=
          ESP_OK ||
      strncasecmp(hdr, "Basic ", 6) != 0) {
    mbedtls_platform_zeroize(hdr, sizeof(hdr));
    return send_401(req);
  }
  uint8_t key[32];
  int rc = mbedtls_sha256((const uint8_t *)hdr, len, key, 0);
  if (rc != 0) {
    mbedtls_platform_zeroize(hdr, sizeof(hdr));
    ESP_LOGE(TAG, "SHA-256 failed (-0x%04x): login not checked",
             (unsigned)-rc);
    return send_503(req);
  }
  const char *token = hdr + 6;
  while (*token == ' ') token++;

  enum { ALLOW, DENY, LOCKED, UNCHECKED } verdict;
  uint32_t wait_s = 0;
  lock();
  const int64_t now = esp_timer_get_time();
  if (!s_set) {
    verdict = ALLOW;  // removed meanwhile
  } else if (cache_has(&s_accepted, key)) {
    verdict = ALLOW;  // logged in: skips the lockout
  } else if (cache_has(&s_rejected, key)) {
    verdict = DENY;  // a stale login retried: not counted again
  } else if (now < s_locked_until_us) {
    verdict = LOCKED;
    wait_s = (uint32_t)((s_locked_until_us - now + 999999) / 1000000);
  } else {
    const match_t m = credentials_match_locked(token, strlen(token), &rc);
    if (m == MATCH_YES) {
      verdict = ALLOW;
      cache_add(&s_accepted, key);
      s_failures = 0;
      s_locked_until_us = 0;
    } else if (m == MATCH_ERROR) {
      verdict = UNCHECKED;  // not remembered or counted: the next try checks
    } else {
      verdict = DENY;
      cache_add(&s_rejected, key);
      s_failures++;
      if (s_failures >= FREE_FAILURES) {
        // 1, 2, 4... s, at most MAX_LOCK_S
        const uint32_t k = s_failures - FREE_FAILURES;
        uint32_t lock_s = k >= 9 ? MAX_LOCK_S : 1u << k;
        if (lock_s > MAX_LOCK_S) lock_s = MAX_LOCK_S;
        s_locked_until_us = now + (int64_t)lock_s * 1000000;
        ESP_LOGW(TAG, "%lu wrong logins in a row: new logins wait %lu s",
                 (unsigned long)s_failures, (unsigned long)lock_s);
      } else {
        ESP_LOGW(TAG, "wrong login (%lu in a row)", (unsigned long)s_failures);
      }
    }
  }
  unlock();
  mbedtls_platform_zeroize(hdr, sizeof(hdr));
  mbedtls_platform_zeroize(key, sizeof(key));

  if (verdict == ALLOW) return true;
  if (verdict == LOCKED) return send_429(req, wait_s);
  if (verdict == UNCHECKED) {
    ESP_LOGE(TAG, "PBKDF2 failed (-0x%04x): login not checked", (unsigned)-rc);
    return send_503(req);
  }
  return send_401(req);
}

esp_err_t web_auth_set_password(const char *pw) {
  const size_t len = pw ? strlen(pw) : 0;
  if (len) {
    if (len < PW_MIN || len > PW_MAX) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < len; i++) {
      const unsigned char c = (unsigned char)pw[i];
      if (c < 0x20 || c > 0x7E) return ESP_ERR_INVALID_ARG;
    }
  }

  pw_record_t rec = {0};
  if (len) {
    rec.version = REC_VERSION;
    rec.algo = REC_ALGO_PBKDF2_SHA256;
    rec.iterations = PBKDF2_ITERATIONS;
    esp_fill_random(rec.salt, sizeof(rec.salt));
    const int64_t t0 = esp_timer_get_time();
    const int rc = derive((const uint8_t *)pw, len, rec.salt, rec.iterations,
                          rec.dk);
    const int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (rc != 0) {
      ESP_LOGE(TAG, "PBKDF2 failed (-0x%04x)", (unsigned)-rc);
      mbedtls_platform_zeroize(&rec, sizeof(rec));
      return ESP_FAIL;
    }
    // for tuning PBKDF2_ITERATIONS: each new login costs this much
    ESP_LOGI(TAG, "PBKDF2 with %lu iterations took %lld ms",
             (unsigned long)rec.iterations, (long long)ms);
  }

  lock();
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
  if (err == ESP_OK) {
    if (len) {
      err = nvs_set_blob(h, NVS_KEY, &rec, sizeof(rec));
    } else {
      err = nvs_erase_key(h, NVS_KEY);
      if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  if (err == ESP_OK) {
    s_loaded = true;
    s_set = len > 0;
    s_unmatchable = false;
    s_rec = rec;
    reset_logins_locked();
  }
  unlock();
  mbedtls_platform_zeroize(&rec, sizeof(rec));

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "could not %s the settings password: %s",
             len ? "store" : "remove", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "settings password %s", len ? "set" : "removed");
  return ESP_OK;
}

// IPv4 address (network order) of a socket address: plain IPv4 or
// IPv4-mapped IPv6 (httpd listens on IPv6)
static bool ipv4_of(const struct sockaddr_storage *ss, uint32_t *out) {
  if (ss->ss_family == AF_INET) {
    *out = ((const struct sockaddr_in *)ss)->sin_addr.s_addr;
    return true;
  }
#if LWIP_IPV6
  if (ss->ss_family == AF_INET6) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    const uint8_t *a = ((const struct sockaddr_in6 *)ss)->sin6_addr.s6_addr;
    if (memcmp(a, mapped, sizeof(mapped)) != 0) return false;
    memcpy(out, a + 12, 4);
    return true;
  }
#endif
  return false;
}

bool web_auth_request_on_ap(httpd_req_t *req) {
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_ip_info_t ip;
  if (ap == NULL || esp_netif_get_ip_info(ap, &ip) != ESP_OK ||
      ip.ip.addr == 0) {
    return false;
  }
  const int fd = httpd_req_to_sockfd(req);
  if (fd < 0) return false;
  struct sockaddr_storage local, peer;
  socklen_t llen = sizeof(local), plen = sizeof(peer);
  uint32_t local4, peer4;
  if (getsockname(fd, (struct sockaddr *)&local, &llen) != 0 ||
      getpeername(fd, (struct sockaddr *)&peer, &plen) != 0 ||
      !ipv4_of(&local, &local4) || !ipv4_of(&peer, &peer4)) {
    return false;
  }
  // Addressed to the AP's IP, from the AP's subnet. (lwIP sends replies from
  // the AP's IP out of the AP interface, so a LAN host routing 192.168.4.1
  // through our home WiFi address gets no answer anyway.)
  return local4 == ip.ip.addr &&
         (peer4 & ip.netmask.addr) == (ip.ip.addr & ip.netmask.addr);
}
