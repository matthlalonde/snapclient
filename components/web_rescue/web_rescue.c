/*
 * web_rescue: WiFi AP+STA and a small HTTP API to configure WiFi, upload
 * firmware and switch firmware slots on boards without a serial port.
 * See include/web_rescue.h for the API.
 */
#include "web_rescue.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_secure_boot.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"

static const char *TAG = "web_rescue";

// Installs must be written as "new" so a rollback-capable bootloader
// trial-boots them; without this option esp_ota_set_boot_partition() marks
// them valid straight away.
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "web_rescue needs CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y"
#endif

#define STA_RETRY_US (10 * 1000 * 1000)  // keep the AP usable between scans
#define CHUNK 4096

static web_rescue_cfg_t s_cfg;
static httpd_handle_t s_httpd = NULL;
static char s_ap_ssid[33];
static char s_sta_ssid[33];
static char s_sta_ip[16];
static volatile int64_t s_last_req = 0;
static volatile bool s_updating = false;
static bool s_marked_valid = false;
static esp_timer_handle_t s_retry_timer = NULL;
static esp_timer_handle_t s_reboot_timer = NULL;

extern const char web_rescue_page_html[];

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

void web_rescue_unlock_physical_flash(void) {
  uint32_t phys = 0;
  if (esp_flash_get_physical_size(esp_flash_default_chip, &phys) == ESP_OK &&
      phys > esp_flash_default_chip->size) {
    ESP_LOGI(TAG, "flash chip is %lu KB, image header says %lu KB: using %lu",
             (unsigned long)(phys / 1024),
             (unsigned long)(esp_flash_default_chip->size / 1024),
             (unsigned long)(phys / 1024));
    esp_flash_default_chip->size = phys;
  }
}

void web_rescue_touch(void) {
  s_last_req = esp_timer_get_time();
  if (!s_marked_valid) {
    s_marked_valid = true;
    // Reaching this page proves the firmware boots and the network works.
    // With a rollback-enabled bootloader this stops it from reverting.
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "running firmware marked valid (%s)", esp_err_to_name(err));
  }
}

static bool host_ok(httpd_req_t *req) {
  char host[80] = "";  // 63-char name + ".local" + ":65535"
  size_t n = httpd_req_get_hdr_value_len(req, "Host");
  if (n == 0) return true;  // non-browser client
  if (n >= sizeof(host) ||
      httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
    return false;
  }
  char *colon = strrchr(host, ':');
  if (colon) *colon = 0;  // drop the port
  size_t len = strlen(host);
  bool ip = len > 0;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)host[i]) && host[i] != '.') ip = false;
  }
  if (ip) return true;
  return len > 6 && strcasecmp(host + len - 6, ".local") == 0;
}

bool web_rescue_allow(httpd_req_t *req, bool sensitive) {
  if (!host_ok(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                        "use the board's IP address or name.local");
    return false;
  }
  if (sensitive) {
    char v[4] = "";
    if (httpd_req_get_hdr_value_str(req, WEB_RESCUE_HEADER, v, sizeof(v)) !=
            ESP_OK ||
        strcmp(v, "1") != 0) {
      httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                          "missing " WEB_RESCUE_HEADER " header");
      return false;
    }
  }
  web_rescue_touch();
  return true;
}

int64_t web_rescue_last_request_us(void) { return s_last_req; }
bool web_rescue_update_in_progress(void) { return s_updating; }
const char *web_rescue_sta_ip(void) { return s_sta_ip; }
const char *web_rescue_ap_ssid(void) { return s_ap_ssid; }
httpd_handle_t web_rescue_httpd(void) { return s_httpd; }

// web_rescue_note default: does nothing (board-probe has no event log);
// snapclient's main defines the real one
__attribute__((weak)) void web_rescue_note(bool restart, bool problem,
                                           const char *text) {
  (void)restart;
  (void)problem;
  (void)text;
}

static void reboot_cb(void *arg) { esp_restart(); }

static void reboot_soon(void) {
  if (!s_reboot_timer) {
    const esp_timer_create_args_t a = {.callback = reboot_cb, .name = "rb"};
    esp_timer_create(&a, &s_reboot_timer);
  }
  esp_timer_start_once(s_reboot_timer, 1500 * 1000);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  char *txt = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!txt) return httpd_resp_send_500(req);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, txt);
  free(txt);
  return err;
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *msg) {
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  cJSON_AddStringToObject(r, "msg", msg);
  if (!ok) httpd_resp_set_status(req, "400 Bad Request");
  return send_json(req, r);
}

// in-place application/x-www-form-urlencoded decoding
static void url_decode(char *s) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '+') {
      *o++ = ' ';
    } else if (*s == '%' && isxdigit((unsigned char)s[1]) &&
               isxdigit((unsigned char)s[2])) {
      char hex[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(hex, NULL, 16);
      s += 2;
    } else {
      *o++ = *s;
    }
  }
  *o = 0;
}

static const char *ota_state_str(const esp_partition_t *p) {
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(p, &st) != ESP_OK) return "";
  switch (st) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "pending-verify";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "invalid";
    case ESP_OTA_IMG_ABORTED:
      return "aborted";
    default:
      return "undefined";
  }
}

/* ------------------------------------------------------------------ */
/* WiFi                                                                */
/* ------------------------------------------------------------------ */

static void retry_cb(void *arg) {
  // a connect attempt scans other channels and can stall an upload on the AP
  if (s_updating) {
    esp_timer_start_once(s_retry_timer, STA_RETRY_US);
    return;
  }
  if (s_sta_ssid[0]) esp_wifi_connect();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id,
                     void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    s_sta_ip[0] = 0;
    if (s_sta_ssid[0] && s_retry_timer) {
      esp_timer_stop(s_retry_timer);
      esp_timer_start_once(s_retry_timer, STA_RETRY_US);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "joined '%s' as %s", s_sta_ssid, s_sta_ip);
  }
}

static esp_err_t wifi_up(void) {
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK) return err;
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
  esp_netif_create_default_wifi_ap();
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  if (!s_cfg.nvs_ok) wcfg.nvs_enable = 0;
  err = esp_wifi_init(&wcfg);
  if (err != ESP_OK) return err;

  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                             on_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
  const esp_timer_create_args_t ta = {.callback = retry_cb, .name = "sta"};
  esp_timer_create(&ta, &s_retry_timer);

  // Mode and AP settings must not overwrite what the real firmware stored.
  // Only STA credentials entered on the page are written to NVS.
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

  wifi_config_t sta = {0};
  esp_wifi_get_config(WIFI_IF_STA, &sta);  // loaded from NVS by esp_wifi_init
  memcpy(s_sta_ssid, sta.sta.ssid, 32);
  s_sta_ssid[32] = 0;

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X",
           s_cfg.ap_ssid_prefix ? s_cfg.ap_ssid_prefix : "esp32", mac[4],
           mac[5]);
  wifi_config_t ap = {0};
  strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
  ap.ap.ssid_len = strlen(s_ap_ssid);
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  if (s_cfg.ap_password && strlen(s_cfg.ap_password) >= 8) {
    strlcpy((char *)ap.ap.password, s_cfg.ap_password,
            sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    ap.ap.authmode = WIFI_AUTH_OPEN;
  }
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());
  if (s_sta_ssid[0]) {
    ESP_LOGI(TAG, "trying stored WiFi '%s'", s_sta_ssid);
    esp_wifi_connect();
  }
  ESP_LOGI(TAG, "access point '%s' up, http://192.168.4.1", s_ap_ssid);
  return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* handlers                                                            */
/* ------------------------------------------------------------------ */

static void add_app_desc(cJSON *o, const esp_app_desc_t *d) {
  cJSON *a = cJSON_AddObjectToObject(o, "app");
  cJSON_AddStringToObject(a, "project", d->project_name);
  cJSON_AddStringToObject(a, "version", d->version);
  cJSON_AddStringToObject(a, "idf", d->idf_ver);
  char built[40];
  snprintf(built, sizeof(built), "%s %s", d->date, d->time);
  cJSON_AddStringToObject(a, "built", built);
}

static esp_err_t status_get(httpd_req_t *req) {
  if (!web_rescue_allow(req, false)) return ESP_OK;
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "title", s_cfg.title ? s_cfg.title : "");
  add_app_desc(r, esp_app_get_description());

  esp_chip_info_t ci;
  esp_chip_info(&ci);
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macs[18];
  snprintf(macs, sizeof(macs), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  cJSON *chip = cJSON_AddObjectToObject(r, "chip");
  cJSON_AddNumberToObject(chip, "revision", ci.revision);
  cJSON_AddStringToObject(chip, "mac", macs);
  uint8_t psram_cap = 0;
  esp_efuse_read_field_blob(ESP_EFUSE_PSRAM_CAP, &psram_cap, 3);
  static const int psram_mb[] = {0, 8, 2, 16, 4, 0, 0, 0};
  cJSON_AddNumberToObject(chip, "psram_mb", psram_mb[psram_cap & 7]);
  cJSON_AddStringToObject(chip, "psram_type",
                          psram_cap == 1 || psram_cap == 3 ? "octal"
                          : psram_cap ? "quad"
                                      : "none");
  uint32_t phys = 0;
  esp_flash_get_physical_size(esp_flash_default_chip, &phys);
  cJSON_AddNumberToObject(chip, "flash_bytes", phys);
  cJSON_AddBoolToObject(chip, "flash_encryption",
                        esp_flash_encryption_enabled());
  cJSON_AddBoolToObject(chip, "secure_boot", esp_secure_boot_enabled());
  cJSON_AddStringToObject(chip, "reset_reason",
                          esp_reset_reason() == ESP_RST_POWERON ? "power-on"
                          : esp_reset_reason() == ESP_RST_SW    ? "software"
                          : esp_reset_reason() == ESP_RST_PANIC ? "PANIC"
                          : esp_reset_reason() == ESP_RST_BROWNOUT
                              ? "BROWNOUT"
                          : esp_reset_reason() == ESP_RST_TASK_WDT ||
                                  esp_reset_reason() == ESP_RST_INT_WDT ||
                                  esp_reset_reason() == ESP_RST_WDT
                              ? "WATCHDOG"
                              : "other");

  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
  cJSON_AddStringToObject(r, "running", running ? running->label : "");
  cJSON_AddStringToObject(r, "running_state",
                          running ? ota_state_str(running) : "");
  cJSON_AddBoolToObject(r, "rollback_possible",
                        esp_ota_check_rollback_is_possible());
  cJSON_AddStringToObject(r, "next_update", next ? next->label : "");
  cJSON_AddNumberToObject(r, "next_update_size", next ? next->size : 0);

  cJSON *parts = cJSON_AddArrayToObject(r, "partitions");
  esp_partition_iterator_t it = esp_partition_find(
      ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "label", p->label);
    cJSON_AddNumberToObject(o, "type", p->type);
    cJSON_AddNumberToObject(o, "subtype", p->subtype);
    cJSON_AddNumberToObject(o, "offset", p->address);
    cJSON_AddNumberToObject(o, "size", p->size);
    if (p->type == ESP_PARTITION_TYPE_APP) {
      esp_app_desc_t d;
      if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
        add_app_desc(o, &d);
      }
      cJSON_AddBoolToObject(o, "running", running && p->address == running->address);
      cJSON_AddBoolToObject(o, "boot", boot && p->address == boot->address);
      cJSON_AddStringToObject(o, "state", ota_state_str(p));
    }
    cJSON_AddItemToArray(parts, o);
  }
  esp_partition_iterator_release(it);

  cJSON *w = cJSON_AddObjectToObject(r, "wifi");
  cJSON_AddStringToObject(w, "ap_ssid", s_ap_ssid);
  cJSON_AddStringToObject(w, "ap_ip", "192.168.4.1");
  cJSON_AddStringToObject(w, "sta_ssid", s_sta_ssid);
  cJSON_AddStringToObject(w, "sta_ip", s_sta_ip);
  cJSON_AddBoolToObject(w, "saved_to_nvs", s_cfg.nvs_ok);
  cJSON_AddNumberToObject(r, "uptime_s", esp_timer_get_time() / 1000000);
  cJSON_AddNumberToObject(r, "free_heap", esp_get_free_heap_size());
  return send_json(req, r);
}

static esp_err_t wifi_post(httpd_req_t *req) {
  if (!web_rescue_allow(req, true)) return ESP_OK;
  char body[512];
  int len = req->content_len < sizeof(body) - 1 ? req->content_len
                                                 : sizeof(body) - 1;
  int got = 0;
  while (got < len) {
    int n = httpd_req_recv(req, body + got, len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (n <= 0) return ESP_FAIL;
    got += n;
  }
  body[got] = 0;

  // values arrive URL-encoded (up to 3 bytes per character)
  char ssid[3 * 32 + 1] = "", pass[3 * 64 + 1] = "";
  if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
    return send_result(req, false, "missing or too long ssid");
  }
  esp_err_t qerr = httpd_query_key_value(body, "password", pass, sizeof(pass));
  if (qerr != ESP_OK && qerr != ESP_ERR_NOT_FOUND) {
    return send_result(req, false, "password too long");
  }
  url_decode(ssid);
  url_decode(pass);
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (!sl || sl > 32 || pl > 64 || (pl && pl < 8)) {
    return send_result(req, false,
                       "SSID must be 1-32 bytes, password empty or 8-64");
  }

  wifi_config_t sta = {0};
  strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
  strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
  sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  sta.sta.threshold.authmode = pl ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
  sta.sta.pmf_cfg.capable = true;
  // persist only the STA credentials (snapclient reads them at boot)
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  if (err != ESP_OK) return send_result(req, false, esp_err_to_name(err));
  strlcpy(s_sta_ssid, ssid, sizeof(s_sta_ssid));
  s_sta_ip[0] = 0;
  esp_wifi_disconnect();
  esp_wifi_connect();
  ESP_LOGI(TAG, "WiFi credentials for '%s' saved%s", ssid,
           s_cfg.nvs_ok ? "" : " (RAM only, NVS unavailable)");
  return send_result(req, true,
                     s_cfg.nvs_ok ? "saved, connecting"
                                  : "connecting (NOT saved: NVS unavailable)");
}

// Reject anything that is not an ESP32-S3 application image. In particular
// a "merged" image (bootloader + partition table + app) also starts with
// 0xE9 but its first segment loads to IRAM/DRAM, not DROM, and it has no
// app descriptor; writing it into an app slot would make the board
// unbootable.
static const char *check_app_image(const uint8_t *b, size_t have) {
  const size_t need = sizeof(esp_image_header_t) +
                      sizeof(esp_image_segment_header_t) +
                      sizeof(esp_app_desc_t);
  if (have < need) return "file too small";
  const esp_image_header_t *h = (const esp_image_header_t *)b;
  if (h->magic != ESP_IMAGE_HEADER_MAGIC) return "not an ESP firmware image";
  if (h->chip_id != ESP_CHIP_ID_ESP32S3) return "image is not for ESP32-S3";
  const esp_image_segment_header_t *s =
      (const esp_image_segment_header_t *)(b + sizeof(esp_image_header_t));
  const esp_app_desc_t *d =
      (const esp_app_desc_t *)(b + sizeof(esp_image_header_t) +
                               sizeof(esp_image_segment_header_t));
  if (s->load_addr < SOC_DROM_LOW || s->load_addr >= SOC_DROM_HIGH ||
      d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
    return "not an application image (a merged/full-flash image? upload "
           "the plain .bin, e.g. snapclient.bin, not *-merged.bin)";
  }
  return NULL;
}

static esp_err_t update_post(httpd_req_t *req) {
  if (!web_rescue_allow(req, true)) return ESP_OK;
  const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
  const esp_partition_t *running = esp_ota_get_running_partition();
  const size_t total = req->content_len;
  if (!target || (running && target->address == running->address)) {
    return send_result(req, false, "no other OTA slot to write to");
  }
  if (total > target->size) {
    char m[96];
    snprintf(m, sizeof(m), "image is %u bytes, slot %s holds only %lu",
             (unsigned)total, target->label, (unsigned long)target->size);
    return send_result(req, false, m);
  }
  uint8_t *buf = malloc(CHUNK);
  if (!buf) return send_result(req, false, "out of memory");

  // first chunk: validate before touching flash
  size_t have = 0, first = total < CHUNK ? total : CHUNK;
  int timeouts = 0;
  while (have < first) {
    int n = httpd_req_recv(req, (char *)buf + have, first - have);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    have += n;
  }
  const char *why = check_app_image(buf, have);
  if (why) {
    free(buf);
    return send_result(req, false, why);
  }
  const esp_app_desc_t *d =
      (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) +
                               sizeof(esp_image_segment_header_t));
  ESP_LOGI(TAG, "installing '%s' %s (%u bytes) into %s", d->project_name,
           d->version, (unsigned)total, target->label);

  esp_ota_handle_t h = 0;
  esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h);
  if (err != ESP_OK) {
    free(buf);
    return send_result(req, false, esp_err_to_name(err));
  }
  s_updating = true;
  err = esp_ota_write(h, buf, have);
  size_t done = have;
  timeouts = 0;
  while (err == ESP_OK && done < total) {
    int n = httpd_req_recv(req, (char *)buf,
                           total - done < CHUNK ? total - done : CHUNK);
    // a vanished client must not wedge the server (recv_wait_timeout 30 s)
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (n <= 0) {
      err = ESP_FAIL;
      break;
    }
    timeouts = 0;
    err = esp_ota_write(h, buf, n);
    done += n;
  }
  free(buf);
  if (err != ESP_OK) {
    esp_ota_abort(h);
    s_updating = false;
    return send_result(req, false, esp_err_to_name(err));
  }
  err = esp_ota_end(h);  // verifies the whole image (and its hash)
  if (err == ESP_OK) err = esp_ota_set_boot_partition(target);
  s_updating = false;
  if (err != ESP_OK) {
    return send_result(req, false, err == ESP_ERR_OTA_VALIDATE_FAILED
                                       ? "image failed verification"
                                       : esp_err_to_name(err));
  }
  ESP_LOGI(TAG, "update written to %s, rebooting", target->label);
  char note[64];
  snprintf(note, sizeof(note), "Installed new firmware into %s; restarting",
           target->label);
  web_rescue_note(true, false, note);
  send_result(req, true, "installed, rebooting into the new firmware");
  reboot_soon();
  return ESP_OK;
}

static esp_err_t boot_post(httpd_req_t *req) {
  if (!web_rescue_allow(req, true)) return ESP_OK;
  char q[64], label[17];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
      httpd_query_key_value(q, "label", label, sizeof(label)) != ESP_OK) {
    return send_result(req, false, "missing label");
  }
  const esp_partition_t *p = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, label);
  esp_app_desc_t d;
  if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) {
    return send_result(req, false, "no valid firmware in that partition");
  }
  esp_err_t err = esp_ota_set_boot_partition(p);
  if (err != ESP_OK) return send_result(req, false, esp_err_to_name(err));
  ESP_LOGI(TAG, "boot partition set to %s ('%s'), rebooting", label,
           d.project_name);
  char note[64];
  snprintf(note, sizeof(note), "Switching to the firmware in %s; restarting",
           label);
  web_rescue_note(true, false, note);
  send_result(req, true, "rebooting into the selected firmware");
  reboot_soon();
  return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req) {
  if (!web_rescue_allow(req, true)) return ESP_OK;
  web_rescue_note(true, false, "Restart requested from the rescue page");
  send_result(req, true, "rebooting");
  reboot_soon();
  return ESP_OK;
}

static esp_err_t backup_get(httpd_req_t *req) {
  // the dump contains every stored WiFi password
  if (!web_rescue_allow(req, true)) return ESP_OK;
  uint32_t phys = 0;
  if (esp_flash_get_physical_size(esp_flash_default_chip, &phys) != ESP_OK) {
    return httpd_resp_send_500(req);
  }
  if (phys > esp_flash_default_chip->size) phys = esp_flash_default_chip->size;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char disp[80];
  snprintf(disp, sizeof(disp),
           "attachment; filename=\"flash-%02x%02x%02x-%luMB.bin\"", mac[3],
           mac[4], mac[5], (unsigned long)(phys >> 20));
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", disp);
  uint8_t *buf = malloc(CHUNK);
  if (!buf) return httpd_resp_send_500(req);
  esp_err_t err = ESP_OK;
  for (uint32_t off = 0; off < phys && err == ESP_OK; off += CHUNK) {
    err = esp_flash_read(esp_flash_default_chip, buf, off, CHUNK);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, (char *)buf, CHUNK);
    s_last_req = esp_timer_get_time();
  }
  free(buf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "backup aborted: %s", esp_err_to_name(err));
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page_get(httpd_req_t *req) {
  if (!web_rescue_allow(req, false)) return ESP_OK;
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, web_rescue_page_html);
}

/* ------------------------------------------------------------------ */
/* start                                                               */
/* ------------------------------------------------------------------ */

esp_err_t web_rescue_start(const web_rescue_cfg_t *cfg) {
  s_cfg = *cfg;
  esp_err_t err = wifi_up();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
    return err;
  }

  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.max_uri_handlers = 32;
  hc.stack_size = 8192;
  hc.recv_wait_timeout = 30;
  hc.send_wait_timeout = 30;
  hc.lru_purge_enable = true;
  // fewer sessions than lwIP's TCP pool (CONFIG_LWIP_MAX_ACTIVE_TCP=6), so
  // a new client can still connect and LRU purge closes the oldest idle one
  hc.max_open_sockets = 5;
  err = httpd_start(&s_httpd, &hc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(err));
    return err;
  }
  const httpd_uri_t uris[] = {
      {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
      {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
      {.uri = "/api/update", .method = HTTP_POST, .handler = update_post},
      {.uri = "/api/boot", .method = HTTP_POST, .handler = boot_post},
      {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post},
      {.uri = "/api/backup.bin", .method = HTTP_GET, .handler = backup_get},
      {.uri = "/setup", .method = HTTP_GET, .handler = page_get},
  };
  for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
    httpd_register_uri_handler(s_httpd, &uris[i]);
  }
  if (cfg->serve_root) {
    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET,
                              .handler = page_get};
    httpd_register_uri_handler(s_httpd, &root);
  }
  return ESP_OK;
}
