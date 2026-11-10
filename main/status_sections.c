// The settings page's Status tab (GET /api/status, see ui_http_server.h):
//   output    which source owns the I2S output (AUX and optical) and its format
//   snapcast  server connection, stream, sync health
//   sendspin  server, group, stream, volume
//   device    firmware, uptime, last reset, WiFi, memory, stack headroom
//   events    a change counter for the event log (GET /api/events)
// It also records the start-up and WiFi events in the event log.
// Each section runs on the web server's task and only copies snapshots that
// the audio tasks publish, so a page refresh never waits on audio work.

#include "status_sections.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "event_log.h"
#include "web_rescue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_interface.h"
#include "player.h"
#include "sdkconfig.h"
#include "sendspin_player.h"
#include "snapcast_status.h"
#include "ui_http_server.h"

static const char *TAG = "STATUS";

// the name this boot runs under (mDNS, snapserver, Sendspin); a new name
// from General Settings applies after a restart
static char device_name[64];

static double seconds_since(int64_t then_us, int64_t now_us) {
  return (double)((now_us - then_us) / 1000000);
}

static void device_section(cJSON *o) {
  const esp_app_desc_t *app = esp_app_get_description();
  const esp_partition_t *running = esp_ota_get_running_partition();
  char built[40];

  cJSON_AddStringToObject(o, "name", device_name);
  cJSON_AddStringToObject(o, "firmware", app->version);
  snprintf(built, sizeof(built), "%s %s", app->date, app->time);
  cJSON_AddStringToObject(o, "built", built);
  cJSON_AddStringToObject(o, "idf", app->idf_ver);
  if (running != NULL) {
    cJSON_AddStringToObject(o, "slot", running->label);
  }
  cJSON_AddNumberToObject(o, "uptime_s", seconds_since(0, esp_timer_get_time()));
  cJSON_AddStringToObject(o, "reset_reason",
                          event_log_reset_reason_text(esp_reset_reason()));

  esp_netif_ip_info_t ip = {0};
  if (network_if_get_ip(&ip) && ip.ip.addr != 0) {
    char addr[16];
    esp_ip4addr_ntoa(&ip.ip, addr, sizeof(addr));
    cJSON_AddStringToObject(o, "ip", addr);
  }

  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    char ssid[sizeof(ap.ssid) + 1];
    memcpy(ssid, ap.ssid, sizeof(ap.ssid));
    ssid[sizeof(ap.ssid)] = '\0';
    cJSON_AddStringToObject(o, "wifi_ssid", ssid);
    cJSON_AddNumberToObject(o, "wifi_rssi", ap.rssi);
    cJSON_AddNumberToObject(o, "wifi_channel", ap.primary);
    // which access point (radio) it is on, for networks with several
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4],
             ap.bssid[5]);
    cJSON_AddStringToObject(o, "wifi_bssid", bssid);
  }

  cJSON_AddNumberToObject(o, "heap_free",
                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  cJSON_AddNumberToObject(o, "heap_min",
                          heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  // lowest free stack seen per task, in bytes: headroom for the event log's
  // formatting and future changes. Looked up by name: only tasks that are
  // never deleted and have a unique name (Sendspin also runs an "httpd").
  static const char *const tasks[] = {"sys_evt", "http", "ss_i2s", "tiT"};
  cJSON *stacks = cJSON_AddObjectToObject(o, "stack_free");
  if (stacks != NULL) {
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); i++) {
      TaskHandle_t t = xTaskGetHandle(tasks[i]);
      if (t != NULL) {
        cJSON_AddNumberToObject(stacks, tasks[i], uxTaskGetStackHighWaterMark(t));
      }
    }
    // this runs on the settings page's own server task
    cJSON_AddNumberToObject(stacks, "httpd", uxTaskGetStackHighWaterMark(NULL));
    // the player measures itself (its task comes and goes)
    snapcast_status_t sc;
    snapcast_status_get(&sc);
    if (sc.player_stack_free != 0) {
      cJSON_AddNumberToObject(stacks, "player", sc.player_stack_free);
    }
    // and so does the Sendspin task (it deletes itself if it can't start)
    sendspin_status_t ss;
    if (sendspin_player_get_status(&ss) && ss.stack_free != 0) {
      cJSON_AddNumberToObject(stacks, "sendspin", ss.stack_free);
    }
  }

  size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psram > 0) {
    cJSON_AddNumberToObject(o, "psram_size", psram);
    cJSON_AddNumberToObject(o, "psram_free",
                            heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(o, "psram_min",
                            heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
  }
}

static const char *player_state_text(snapcast_player_state_t s) {
  switch (s) {
    case SNAPCAST_PLAYER_SYNCING:
      return "syncing";
    case SNAPCAST_PLAYER_PLAYING:
      return "playing";
    default:
      return "stopped";
  }
}

static void snapcast_section(cJSON *o) {
  snapcast_status_t s;
  snapcast_status_get(&s);
  int64_t now = esp_timer_get_time();

  if (s.server_host[0] != '\0' || s.server_ip[0] != '\0') {
    cJSON_AddStringToObject(o, "server_host",
                            s.server_host[0] ? s.server_host : s.server_ip);
    cJSON_AddStringToObject(o, "server_ip", s.server_ip);
    cJSON_AddNumberToObject(o, "server_port", s.server_port);
    cJSON_AddStringToObject(
        o, "discovery", s.discovery == SNAPCAST_DISCOVERY_MDNS ? "mdns" : "static");
  }
  cJSON_AddBoolToObject(o, "connected", s.connected);
  if (s.connected) {
    cJSON_AddNumberToObject(o, "connected_s",
                            seconds_since(s.connected_since_us, now));
  }
  cJSON_AddNumberToObject(o, "connections", s.connections);
  if (s.client_id[0] != '\0') {
    cJSON_AddStringToObject(o, "client_id", s.client_id);
  }

  if (s.connected && s.have_codec) {
    cJSON_AddStringToObject(o, "codec", s.codec);
    cJSON_AddNumberToObject(o, "sample_rate", s.sample_rate);
    cJSON_AddNumberToObject(o, "bits", s.bits);
    cJSON_AddNumberToObject(o, "channels", s.channels);
  }
  if (s.connected && s.have_settings) {
    cJSON_AddNumberToObject(o, "volume", s.volume);
    cJSON_AddBoolToObject(o, "muted", s.muted);
    cJSON_AddNumberToObject(o, "buffer_ms", s.buffer_ms);
    cJSON_AddNumberToObject(o, "latency_ms", s.latency_ms);
  }

  // the player task stops when Sendspin takes the output
  bool handed_over = player_output_released();
  cJSON_AddStringToObject(o, "player", handed_over ? "handed_over"
                                                   : player_state_text(s.player));
  if (!handed_over && s.player == SNAPCAST_PLAYER_PLAYING) {
    if (s.sync_valid) {
      cJSON_AddNumberToObject(o, "sync_error_us", s.sync_error_us);
    }
    cJSON_AddStringToObject(o, "correction",
                            s.correction > 0   ? "faster"
                            : s.correction < 0 ? "slower"
                                               : "none");
    cJSON_AddNumberToObject(o, "queued_ms", s.queued_ms);
  }
#if CONFIG_USE_SAMPLE_INSERTION
  // running totals; the page turns their change into a rate
  cJSON_AddNumberToObject(o, "frames_dropped", s.frames_dropped);
  cJSON_AddNumberToObject(o, "frames_inserted", s.frames_inserted);
#endif
  cJSON_AddNumberToObject(o, "hard_resyncs", s.hard_resyncs);
  if (s.hard_resyncs > 0) {
    cJSON_AddNumberToObject(o, "last_hard_resync_s",
                            seconds_since(s.last_hard_resync_us, now));
    cJSON_AddStringToObject(o, "last_resync_reason",
                            s.last_resync_reason == SNAPCAST_RESYNC_DRIFT
                                ? "drift"
                                : "queue_empty");
  }
}

#if CONFIG_SENDSPIN_PLAYER_ENABLE
static void sendspin_section(cJSON *o) {
  sendspin_status_t s;
  if (!sendspin_player_get_status(&s)) {
    cJSON_AddBoolToObject(o, "running", false);
    return;
  }
  cJSON_AddBoolToObject(o, "running", true);
  cJSON_AddNumberToObject(o, "port", s.port);
  cJSON_AddStringToObject(o, "client_id", s.client_id);
  cJSON_AddBoolToObject(o, "unpaired_access", s.unpaired_access);

  cJSON_AddBoolToObject(o, "connected", s.connected);
  if (s.connected) {
    cJSON_AddStringToObject(o, "server_name", s.server_name);
    cJSON_AddBoolToObject(o, "paired", s.paired);
    if (s.have_group) {
      cJSON_AddStringToObject(o, "group_name", s.group_name);
      cJSON_AddBoolToObject(o, "group_playing", s.group_playing);
    }
    cJSON_AddBoolToObject(o, "time_synced", s.time_synced);
    if (s.have_sync_error) {
      cJSON_AddNumberToObject(o, "sync_error_us", s.sync_error_us);
    }
  }

  cJSON_AddBoolToObject(o, "stream", s.stream);
  if (s.stream) {
    if (s.codec[0] != '\0') {
      cJSON_AddStringToObject(o, "codec", s.codec);
    }
    if (s.sample_rate > 0) {
      cJSON_AddNumberToObject(o, "sample_rate", s.sample_rate);
      cJSON_AddNumberToObject(o, "bits", s.bits);
      cJSON_AddNumberToObject(o, "channels", s.channels);
    }
  }
  cJSON_AddBoolToObject(o, "output_open", s.output_open);
  if (s.output_open) {
    cJSON_AddNumberToObject(o, "output_rate", s.output_rate);
    cJSON_AddNumberToObject(o, "output_bits", s.output_bits);
  }
  cJSON_AddBoolToObject(o, "waiting_for_output", s.waiting_for_output);
  cJSON_AddBoolToObject(o, "output_failed", s.output_failed);
  cJSON_AddNumberToObject(o, "volume", s.volume);
  cJSON_AddBoolToObject(o, "muted", s.muted);
}
#endif

// Which source owns the I2S output right now, and at what format
static void output_section(cJSON *o) {
  sendspin_status_t ss;
  if (sendspin_player_get_status(&ss) && ss.output_open) {
    cJSON_AddStringToObject(o, "source", "sendspin");
    cJSON_AddNumberToObject(o, "sample_rate", ss.output_rate);
    cJSON_AddNumberToObject(o, "bits", ss.output_bits);
    return;
  }
  snapcast_status_t sc;
  snapcast_status_get(&sc);
  if (!player_output_released() && sc.player == SNAPCAST_PLAYER_PLAYING) {
    cJSON_AddStringToObject(o, "source", "snapcast");
    cJSON_AddNumberToObject(o, "sample_rate", sc.output_rate);
    cJSON_AddNumberToObject(o, "bits", sc.output_bits);
    return;
  }
  cJSON_AddStringToObject(o, "source", "none");
}

// The page fetches /api/events only when these change
static void events_section(cJSON *o) {
  cJSON_AddNumberToObject(o, "boot", event_log_boot());
  cJSON_AddNumberToObject(o, "changes", event_log_last_change());
}

void status_events_boot(void) {
  esp_reset_reason_t rr = esp_reset_reason();
  event_level_t level = EVENT_INFO;
  const char *note = "";
  switch (rr) {
    case ESP_RST_POWERON:
    case ESP_RST_DEEPSLEEP:
      break;
    case ESP_RST_SW:
      // deliberate restarts log their reason first (event_log_restart)
      if (!event_log_start_explained()) {
        level = EVENT_WARN;
        note = ", no reason logged";
      }
      break;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_CPU_LOCKUP:
      level = EVENT_ERROR;
      break;
    default:
      level = EVENT_WARN;
      break;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  event_log_addf(EVENT_SRC_SYSTEM, level, "Started after %s%s; %s in %s",
                 event_log_reset_reason_text(rr), note,
                 esp_app_get_description()->version,
                 running != NULL ? running->label : "?");
}

static const char *wifi_reason_text(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
      return "authentication expired";
    case WIFI_REASON_AUTH_LEAVE:
      return "access point ended the connection";
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
      return "dropped by the access point for inactivity";
    case WIFI_REASON_ASSOC_TOOMANY:
      return "access point is full";
    case WIFI_REASON_CLASS2_FRAME_FROM_NONAUTH_STA:
    case WIFI_REASON_CLASS3_FRAME_FROM_NONASSOC_STA:
      return "access point forgot this device";
    case WIFI_REASON_MISSING_ACKS:
      return "too many lost frames";
    case WIFI_REASON_TIMEOUT:
      return "timed out";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
      return "no access point with the expected security";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "no access point with a strong enough signal";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "WPA handshake timed out (wrong password?)";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "lost the access point's signal";
    case WIFI_REASON_NO_AP_FOUND:
      return "access point not found";
    case WIFI_REASON_AUTH_FAIL:
      return "authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
      return "association failed";
    case WIFI_REASON_CONNECTION_FAIL:
      return "connection failed";
    case WIFI_REASON_ROAMING:
      return "roaming";
    default:
      return NULL;
  }
}

#define MACFMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC(b) (b)[0], (b)[1], (b)[2], (b)[3], (b)[4], (b)[5]

// Runs on the event loop task (small stack): short texts only
static void wifi_events(void *arg, esp_event_base_t base, int32_t id,
                        void *data) {
  static uint8_t last_bssid[6];
  static bool have_bssid = false;

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
    const wifi_event_sta_connected_t *e = data;
    if (have_bssid && memcmp(last_bssid, e->bssid, sizeof(last_bssid)) != 0) {
      event_log_addf(EVENT_SRC_WIFI, EVENT_INFO,
                     "Moved to access point " MACFMT " (was " MACFMT
                     "), channel %u",
                     MAC(e->bssid), MAC(last_bssid), e->channel);
    } else {
      char ssid[33];
      size_t n = e->ssid_len < 32 ? e->ssid_len : 32;
      memcpy(ssid, e->ssid, n);
      ssid[n] = '\0';
      event_log_addf(EVENT_SRC_WIFI, EVENT_INFO,
                     "Joined %s via access point " MACFMT ", channel %u", ssid,
                     MAC(e->bssid), e->channel);
    }
    memcpy(last_bssid, e->bssid, sizeof(last_bssid));
    have_bssid = true;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *e = data;
    const char *why = wifi_reason_text(e->reason);
    if (e->reason == WIFI_REASON_ASSOC_LEAVE) {
      // this device disconnected on purpose (a restart, say): that event is
      // already logged, and logging here would race the restart
    } else if (e->reason == WIFI_REASON_BEACON_TIMEOUT) {
      // the signal level tells weak coverage from a vanished access point
      event_log_addf(EVENT_SRC_WIFI, EVENT_WARN, "Disconnected: %s (%d dBm)",
                     why, e->rssi);
    } else if (why != NULL) {
      // no signal level, so retries repeat the same text and are counted
      event_log_addf(EVENT_SRC_WIFI, EVENT_WARN, "Disconnected: %s", why);
    } else {
      event_log_addf(EVENT_SRC_WIFI, EVENT_WARN, "Disconnected (reason %u)",
                     e->reason);
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *e = data;
    if (e->ip_changed) {  // not on every reconnect with the same lease
      event_log_addf(EVENT_SRC_WIFI, EVENT_INFO, "Got IP address " IPSTR,
                     IP2STR(&e->ip_info.ip));
    }
  }
}

// web_rescue's hook (a weak no-op there): rescue mode's restarts and
// decisions go into the event log. Fixed strings: this can run on the 2 KB
// esp_timer task.
void web_rescue_note(bool restart, bool problem, const char *text) {
  event_level_t level = problem ? EVENT_WARN : EVENT_INFO;
  if (restart) {
    event_log_note_restart(level, text);
  } else {
    event_log_add(EVENT_SRC_SYSTEM, level, text);
  }
}

void status_events_start(void) {
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_events,
                             NULL);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                             wifi_events, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL);
}

void status_sections_init(const char *name) {
  snprintf(device_name, sizeof(device_name), "%s", name ? name : "");
  ui_http_add_status_section("output", output_section);
  ui_http_add_status_section("snapcast", snapcast_section);
#if CONFIG_SENDSPIN_PLAYER_ENABLE
  ui_http_add_status_section("sendspin", sendspin_section);
#endif
  ui_http_add_status_section("device", device_section);
  ui_http_add_status_section("events", events_section);
  ESP_LOGD(TAG, "status sections registered");
}
