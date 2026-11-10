// The settings page's Status tab (GET /api/status, see ui_http_server.h):
//   output    which source owns the I2S output (AUX and optical) and its format
//   snapcast  server connection, stream, sync health
//   sendspin  server, group, stream, volume
//   device    firmware, uptime, last reset, WiFi, memory
// Each section runs on the web server's task and only copies snapshots that
// the audio tasks publish, so a page refresh never waits on audio work.

#include "status_sections.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

static const char *reset_reason_text(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "reset pin";
    case ESP_RST_SW:
      return "restart (software)";
    case ESP_RST_PANIC:
      return "crash (panic)";
    case ESP_RST_INT_WDT:
      return "crash (interrupt watchdog)";
    case ESP_RST_TASK_WDT:
      return "crash (task watchdog)";
    case ESP_RST_WDT:
      return "crash (watchdog)";
    case ESP_RST_DEEPSLEEP:
      return "wake from deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout (supply voltage dip)";
    case ESP_RST_SDIO:
      return "SDIO reset";
    case ESP_RST_USB:
      return "USB reset";
    case ESP_RST_JTAG:
      return "JTAG reset";
    case ESP_RST_EFUSE:
      return "eFuse error";
    case ESP_RST_PWR_GLITCH:
      return "power glitch";
    case ESP_RST_CPU_LOCKUP:
      return "crash (CPU lockup)";
    default:
      return "unknown";
  }
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
  cJSON_AddStringToObject(o, "reset_reason", reset_reason_text(esp_reset_reason()));

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

void status_sections_init(const char *name) {
  snprintf(device_name, sizeof(device_name), "%s", name ? name : "");
  ui_http_add_status_section("output", output_section);
  ui_http_add_status_section("snapcast", snapcast_section);
#if CONFIG_SENDSPIN_PLAYER_ENABLE
  ui_http_add_status_section("sendspin", sendspin_section);
#endif
  ui_http_add_status_section("device", device_section);
  ESP_LOGD(TAG, "status sections registered");
}
