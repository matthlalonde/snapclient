// WiFi roaming within one network, see wifi_roam.h.
//
// This chip can't hand over between access points seamlessly: a move
// disconnects, which takes the network interface down and ends every TCP
// connection (snapcast and Sendspin reconnect, DHCP runs again), a few
// seconds of silence. So moves are rare and only for a clear gain:
//   - weak:  averaged signal below CONFIG_WIFI_ROAMING_WEAK_RSSI for 30 s
//   - scan:  only for our network, with short visits to other channels
//            (the radio returns home in between), backing off from 2 to
//            15 minutes while nothing better turns up
//   - move:  to an access point CONFIG_WIFI_ROAMING_MIN_GAIN dB stronger
//            than ours in the same scan and at least -70 dBm, seen in two
//            scans in a row. While audio plays, neither scans nor moves
//            happen unless ours is -82 dBm or weaker.
//   - fail:  no IP within 15 s (or a refused join): back to the previous
//            access point, pinned, and that target is skipped for 6 hours
//   - after: 10 minutes before the next move; an all-channel scan at least
//            once an hour, else only the channels the network was seen on
// Config changes go to RAM only (esp_wifi_set_storage), so a pinned access
// point never ends up in flash.

#include "wifi_roam.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi_interface.h"

static const char *TAG = "wifi_roam";

#define MACFMT "%02X:%02X:%02X:%02X:%02X:%02X"
#define MAC(b) (b)[0], (b)[1], (b)[2], (b)[3], (b)[4], (b)[5]

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_roam_status_t s_status = {.state = WIFI_ROAM_OFF};
static volatile bool s_moving = false;

const char *wifi_roam_state_name(wifi_roam_state_t state) {
  switch (state) {
    case WIFI_ROAM_OFFLINE:
      return "offline";
    case WIFI_ROAM_MEASURING:
      return "measuring";
    case WIFI_ROAM_GOOD:
      return "good";
    case WIFI_ROAM_WEAK:
      return "weak";
    case WIFI_ROAM_SCANNING:
      return "scanning";
    case WIFI_ROAM_WAITING:
      return "waiting";
    case WIFI_ROAM_MOVING:
      return "moving";
    case WIFI_ROAM_COOLDOWN:
      return "cooldown";
    case WIFI_ROAM_FAILED:
      return "failed";
    default:
      return "off";
  }
}

void wifi_roam_get_status(wifi_roam_status_t *out) {
  taskENTER_CRITICAL(&s_mux);
  *out = s_status;
  taskEXIT_CRITICAL(&s_mux);
}

bool wifi_roam_in_progress(void) { return s_moving; }

#if CONFIG_WIFI_ROAMING

#define POLL_MS 5000
#define WEAK_SAMPLES 6                       // 6 x 5 s = 30 s
#define BOOT_QUIET_US (180LL * 1000000)      // Improv provisioning's window
#define BACKOFF_MIN_US (120LL * 1000000)
#define BACKOFF_MAX_US (900LL * 1000000)
#define CONFIRM_AFTER_US (60LL * 1000000)    // second look at a candidate
#define COOLDOWN_US (600LL * 1000000)
#define FULL_SCAN_EVERY_US (3600LL * 1000000)
#define FAILED_HOLD_US (6LL * 3600 * 1000000)
#define MAX_FAILED 4
#define MANUAL_GAP_US (20LL * 1000000)
#define JOIN_TIMEOUT_MS 15000
#define GOOD_ENOUGH_RSSI (-70)
#define VERY_WEAK_RSSI (-82)                 // scan and move even while audio plays
// channels 12-13 are passive-only in the default country setting: without
// this the driver dwells 360 ms there
#define PASSIVE_DWELL_MS 120

// associated but no address for this long: that access point is broken
// (a mesh node without backhaul, say); try another one
#define NO_IP_GIVE_UP_US (30LL * 1000000)
#define NO_IP_RETRY_US (120LL * 1000000)

#define BIT_GOT_IP BIT0
#define BIT_DROPPED BIT1  // a pinned join was refused or lost

typedef struct {
  uint8_t bssid[6];
  int64_t until;
} failed_t;

static TaskHandle_t s_task;
static EventGroupHandle_t s_events;
static bool (*s_busy_cb)(void);
static volatile bool s_scan_request;
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_manual_us = -MANUAL_GAP_US;
static failed_t s_failed[MAX_FAILED];  // roam task only

static void set_state(wifi_roam_state_t state) {
  taskENTER_CRITICAL(&s_mux);
  s_status.state = state;
  taskEXIT_CRITICAL(&s_mux);
}

// RAM only: a pinned access point must never be stored in flash
static esp_err_t set_config_ram(wifi_config_t *cfg) {
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, cfg);
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);
  return err;
}

static void join_policy(wifi_config_t *cfg) {
  cfg->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;  // a fast scan takes the first AP heard
  cfg->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  cfg->sta.bssid_set = 0;
  cfg->sta.channel = 0;
  cfg->sta.failure_retry_cnt = 1;  // then try the next strongest
}

void wifi_roam_apply_connect_policy(void) {
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return;
  }
  join_policy(&cfg);
  esp_err_t err = set_config_ram(&cfg);
  taskENTER_CRITICAL(&s_mux);
  s_status.strongest_at_connect = err == ESP_OK;
  taskEXIT_CRITICAL(&s_mux);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
  xEventGroupSetBits(s_events, BIT_GOT_IP);
}

// While moving: a join refused or lost ends the wait at once. Our own
// esp_wifi_disconnect() (reason ASSOC_LEAVE) doesn't count.
static void on_disconnected(void *arg, esp_event_base_t base, int32_t id,
                            void *data) {
  const wifi_event_sta_disconnected_t *e = data;
  if (s_moving && e->reason != WIFI_REASON_ASSOC_LEAVE) {
    xEventGroupSetBits(s_events, BIT_DROPPED);
  }
}

void wifi_roam_set_busy_cb(bool (*busy)(void)) { s_busy_cb = busy; }

bool wifi_roam_request_scan(void) {
  wifi_ap_record_t ap;
  if (s_task == NULL || s_moving || !wifi_get_ip(NULL) ||
      esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return false;
  }
  int64_t now = esp_timer_get_time();
  bool ok = false;
  taskENTER_CRITICAL(&s_req_mux);
  if (now - s_last_manual_us >= MANUAL_GAP_US) {
    s_last_manual_us = now;
    ok = true;
  }
  taskEXIT_CRITICAL(&s_req_mux);
  if (!ok) {
    return false;
  }
  s_scan_request = true;
  taskENTER_CRITICAL(&s_mux);
  s_status.scan_pending = true;
  taskEXIT_CRITICAL(&s_mux);
  xTaskNotifyGive(s_task);
  return true;
}

// Scan for our network only. mask: 2.4 GHz channel bits (bit n = channel n),
// 0 = all channels. Returns the number of records (strongest first), or -1.
static int scan(const uint8_t *ssid, uint16_t mask, wifi_ap_record_t *recs,
                uint16_t max) {
  wifi_scan_config_t sc = {
      .ssid = (uint8_t *)ssid,
      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
      // short visits to other channels, back home in between
      .scan_time = {.active = {.min = 20, .max = 40}, .passive = PASSIVE_DWELL_MS},
      .home_chan_dwell_time = 100,
  };
  if (mask != 0) {
    sc.channel = 0;
    sc.channel_bitmap.ghz_2_channels = mask;
  }
  esp_err_t err = esp_wifi_scan_start(&sc, true);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    return -1;
  }
  uint16_t n = max;
  if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) {
    return -1;
  }
  return n;  // the driver sorts by signal
}

static void publish_scan(const wifi_ap_record_t *recs, int n) {
  int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  s_status.ap_count = 0;
  for (int i = 0; i < n && i < WIFI_ROAM_MAX_APS; i++) {
    wifi_roam_ap_t *a = &s_status.aps[s_status.ap_count++];
    memcpy(a->bssid, recs[i].bssid, 6);
    a->channel = recs[i].primary;
    a->rssi = recs[i].rssi;
  }
  s_status.last_scan_us = now;
  s_status.scan_pending = false;
  taskEXIT_CRITICAL(&s_mux);
}

static bool is_failed(const uint8_t *bssid, int64_t now) {
  for (int i = 0; i < MAX_FAILED; i++) {
    if (s_failed[i].until > now && memcmp(s_failed[i].bssid, bssid, 6) == 0) {
      return true;
    }
  }
  return false;
}

static void remember_failed(const uint8_t *bssid, int64_t now) {
  int slot = 0;
  for (int i = 0; i < MAX_FAILED; i++) {
    if (s_failed[i].until < s_failed[slot].until) slot = i;  // oldest or free
  }
  memcpy(s_failed[slot].bssid, bssid, 6);
  s_failed[slot].until = now + FAILED_HOLD_US;
}

// Join exactly `bssid` on `channel`. Disconnects first, so the driver is idle
// and the config write can't be refused ("sta is connecting"). True once it
// has an IP address there.
static bool join_pinned(const uint8_t *bssid, uint8_t channel) {
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return false;
  }
  esp_wifi_disconnect();
  xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_DROPPED);
  cfg.sta.bssid_set = 1;
  memcpy(cfg.sta.bssid, bssid, 6);
  cfg.sta.channel = channel;
  cfg.sta.scan_method = WIFI_FAST_SCAN;  // just that BSSID, on that channel
  if (set_config_ram(&cfg) != ESP_OK) {
    return false;
  }
  esp_wifi_connect();
  EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_DROPPED,
                                         pdFALSE, pdFALSE,
                                         pdMS_TO_TICKS(JOIN_TIMEOUT_MS));
  return (bits & BIT_GOT_IP) != 0 && wifi_get_ip(NULL);
}

static bool unpin(void) {
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return false;
  }
  join_policy(&cfg);
  return set_config_ram(&cfg) == ESP_OK;
}

// Move from `from` (ours, on from_channel) to `target`. Returns true if the
// station ended up on the target with an IP address.
static bool move_to(const wifi_ap_record_t *target, int ours,
                    const uint8_t *from, uint8_t from_channel) {
  event_log_addf(EVENT_SRC_WIFI, EVENT_INFO,
                 "Roaming to access point " MACFMT " (%d dBm, this one %d dBm)",
                 MAC(target->bssid), target->rssi, ours);
  set_state(WIFI_ROAM_MOVING);
  s_moving = true;  // the reconnect handler leaves connecting to us now

  bool moved = join_pinned(target->bssid, target->primary);
  if (!moved) {
    // back to the one that worked, pinned: "any" would pick the stronger,
    // failing target again
    event_log_addf(EVENT_SRC_WIFI, EVENT_WARN,
                   "Could not use access point " MACFMT
                   "; going back to " MACFMT,
                   MAC(target->bssid), MAC(from));
    join_pinned(from, from_channel);
  }

  // clear the pin so later drops rescan all channels. Allowed while
  // connected; without an address, disconnect and start over unpinned.
  bool unpinned = wifi_get_ip(NULL) && unpin();
  if (!unpinned) {
    esp_wifi_disconnect();
    if (!unpin()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      if (!unpin()) {
        event_log_add(EVENT_SRC_WIFI, EVENT_ERROR,
                      "Roaming could not clear its access point choice");
      }
    }
    s_moving = false;
    esp_wifi_connect();  // the reconnect handler takes it from here
    return moved;
  }
  s_moving = false;
  // a drop between the join and here was swallowed: reconnect it
  if (!wifi_get_ip(NULL)) {
    esp_wifi_connect();
  }
  return moved;
}

// Associated for a while without an address: join another access point of
// the network, if there is one, and skip this one for hours. Without one,
// nothing changes (rescue mode still takes over after 3 minutes).
static void leave_broken_ap(const wifi_ap_record_t *ap, wifi_ap_record_t *recs,
                            uint16_t max) {
  int n = scan(ap->ssid, 0, recs, max);
  const wifi_ap_record_t *other = NULL;
  int64_t now = esp_timer_get_time();
  for (int i = 0; i < n; i++) {
    if (memcmp(recs[i].bssid, ap->bssid, 6) != 0 && !is_failed(recs[i].bssid, now)) {
      other = &recs[i];  // strongest first
      break;
    }
  }
  if (other == NULL) {
    return;
  }
  wifi_ap_record_t target = *other;
  remember_failed(ap->bssid, now);
  event_log_addf(EVENT_SRC_WIFI, EVENT_WARN,
                 "No IP address from access point " MACFMT
                 "; trying " MACFMT,
                 MAC(ap->bssid), MAC(target.bssid));
  set_state(WIFI_ROAM_MOVING);
  s_moving = true;
  join_pinned(target.bssid, target.primary);
  bool unpinned = wifi_get_ip(NULL) && unpin();
  if (!unpinned) {
    esp_wifi_disconnect();
    if (!unpin()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      unpin();
    }
    s_moving = false;
    esp_wifi_connect();
    return;
  }
  s_moving = false;
  if (!wifi_get_ip(NULL)) {
    esp_wifi_connect();
  }
}

static void roam_task(void *arg) {
  int64_t no_ip_since = 0, no_ip_next_try = 0;
  int8_t samples[WEAK_SAMPLES];
  int nsamples = 0, next = 0;
  int64_t next_scan_at = 0, backoff = BACKOFF_MIN_US, cooldown_until = 0;
  int64_t last_full_scan = INT64_MIN / 2;
  bool last_move_failed = false;
  uint8_t candidate[6] = {0};
  int candidate_hits = 0;
  bool waiting_logged = false;
  uint16_t seen_channels = 0;
  static wifi_ap_record_t recs[WIFI_ROAM_MAX_APS];  // only this task

  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
    int64_t now = esp_timer_get_time();

    wifi_ap_record_t ap;
    bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    bool has_ip = wifi_get_ip(NULL);
    if (associated && !has_ip) {
      // DHCP normally takes a few seconds; much longer means a broken AP
      if (no_ip_since == 0) no_ip_since = now;
      if (now - no_ip_since >= NO_IP_GIVE_UP_US && now >= no_ip_next_try &&
          now >= BOOT_QUIET_US && !s_moving) {
        no_ip_next_try = now + NO_IP_RETRY_US;
        leave_broken_ap(&ap, recs, WIFI_ROAM_MAX_APS);
      }
    } else {
      no_ip_since = 0;
    }
    if (!associated || !has_ip) {
      nsamples = next = 0;
      candidate_hits = 0;
      s_scan_request = false;
      taskENTER_CRITICAL(&s_mux);
      s_status.state = WIFI_ROAM_OFFLINE;
      s_status.rssi_avg = 0;
      s_status.scan_pending = false;
      taskEXIT_CRITICAL(&s_mux);
      continue;
    }

    samples[next] = ap.rssi;
    next = (next + 1) % WEAK_SAMPLES;
    if (nsamples < WEAK_SAMPLES) nsamples++;
    int sum = 0;
    for (int i = 0; i < nsamples; i++) sum += samples[i];
    int avg = sum / nsamples;
    bool measured = nsamples == WEAK_SAMPLES;
    bool weak = measured && avg < CONFIG_WIFI_ROAMING_WEAK_RSSI;
    bool cooling = now < cooldown_until;
    bool very_weak = avg <= VERY_WEAK_RSSI;
    bool busy = s_busy_cb != NULL && s_busy_cb() && !very_weak;
    bool manual = s_scan_request;
    bool may_move = weak && !cooling && now >= BOOT_QUIET_US;
    bool waiting = may_move && busy && candidate_hits > 0;

    if (!weak) {
      backoff = BACKOFF_MIN_US;
      candidate_hits = 0;
    }
    if (waiting && !waiting_logged) {
      event_log_addf(EVENT_SRC_WIFI, EVENT_INFO,
                     "Stronger access point " MACFMT
                     " found; moving when playback stops",
                     MAC(candidate));
    }
    waiting_logged = waiting;

    wifi_roam_state_t state =
        cooling     ? (last_move_failed ? WIFI_ROAM_FAILED : WIFI_ROAM_COOLDOWN)
        : !measured ? WIFI_ROAM_MEASURING
        : waiting   ? WIFI_ROAM_WAITING
        : weak     ? WIFI_ROAM_WEAK
                   : WIFI_ROAM_GOOD;
    taskENTER_CRITICAL(&s_mux);
    s_status.rssi_avg = (int8_t)avg;
    s_status.state = state;
    taskEXIT_CRITICAL(&s_mux);

    // automatic scans only when one could lead to a move: while audio
    // plays a move would wait anyway, so the scan would only cost audio
    bool auto_scan = may_move && !busy && now >= next_scan_at;
    if (!manual && !auto_scan) {
      continue;
    }

    s_scan_request = false;
    set_state(WIFI_ROAM_SCANNING);
    bool full = manual || seen_channels == 0 ||
                now - last_full_scan >= FULL_SCAN_EVERY_US;
    int n = scan(ap.ssid, full ? 0 : seen_channels, recs, WIFI_ROAM_MAX_APS);
    if (n < 0) {
      taskENTER_CRITICAL(&s_mux);
      s_status.scan_pending = false;
      taskEXIT_CRITICAL(&s_mux);
      next_scan_at = now + BACKOFF_MIN_US;
      set_state(state);
      continue;
    }
    if (full) {
      last_full_scan = now;
      seen_channels = 0;  // rebuilt from this scan: channels change too
    }
    publish_scan(recs, n);
    for (int i = 0; i < n; i++) {
      if (recs[i].primary >= 1 && recs[i].primary <= 14) {
        seen_channels |= (uint16_t)(1u << recs[i].primary);
      }
    }
    if (!auto_scan && !(may_move && !busy)) {
      set_state(state);  // a manual scan with nothing to act on
      continue;
    }

    // ours as seen in the same scan (the same measurement), else the average
    int ours = avg;
    const wifi_ap_record_t *best = NULL;
    for (int i = 0; i < n; i++) {
      if (memcmp(recs[i].bssid, ap.bssid, 6) == 0) {
        ours = recs[i].rssi;
      } else if (!is_failed(recs[i].bssid, now) &&
                 (best == NULL || recs[i].rssi > best->rssi)) {
        best = &recs[i];
      }
    }
    if (best == NULL || best->rssi < ours + CONFIG_WIFI_ROAMING_MIN_GAIN ||
        best->rssi < GOOD_ENOUGH_RSSI) {
      candidate_hits = 0;
      // the same text each time, so repeats are counted, not stored
      event_log_add(EVENT_SRC_WIFI, EVENT_INFO,
                    "Weak signal: no clearly stronger access point found");
      next_scan_at = now + backoff;
      backoff = backoff * 2 > BACKOFF_MAX_US ? BACKOFF_MAX_US : backoff * 2;
      set_state(WIFI_ROAM_WEAK);
      continue;
    }

    if (memcmp(candidate, best->bssid, 6) == 0) {
      candidate_hits++;
    } else {
      memcpy(candidate, best->bssid, 6);
      candidate_hits = 1;
    }
    if (candidate_hits < 2) {
      next_scan_at = now + CONFIRM_AFTER_US;  // look again before moving
      set_state(WIFI_ROAM_WEAK);
      continue;
    }

    wifi_ap_record_t target = *best;
    bool moved = move_to(&target, ours, ap.bssid, ap.primary);
    now = esp_timer_get_time();
    if (!moved) {
      remember_failed(target.bssid, now);
    } else {
      taskENTER_CRITICAL(&s_mux);
      s_status.roams++;
      s_status.last_roam_us = now;
      taskEXIT_CRITICAL(&s_mux);
    }
    last_move_failed = !moved;
    candidate_hits = 0;
    backoff = BACKOFF_MIN_US;
    cooldown_until = now + COOLDOWN_US;
    nsamples = next = 0;
    seen_channels = 0;
    set_state(moved ? WIFI_ROAM_COOLDOWN : WIFI_ROAM_FAILED);
  }
}

void wifi_roam_start(void) {
  if (s_task != NULL) {
    return;
  }
  s_events = xEventGroupCreate();
  if (s_events == NULL) {
    return;
  }
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                             on_disconnected, NULL);
  set_state(WIFI_ROAM_OFFLINE);
  if (xTaskCreatePinnedToCore(roam_task, "wifi_roam", 4096, NULL, 3, &s_task,
                              0) != pdPASS) {
    s_task = NULL;
    set_state(WIFI_ROAM_OFF);
    ESP_LOGE(TAG, "could not start the roaming task");
  }
}

#else  // !CONFIG_WIFI_ROAMING: upstream behaviour

void wifi_roam_apply_connect_policy(void) {}
void wifi_roam_start(void) {}
void wifi_roam_set_busy_cb(bool (*busy)(void)) { (void)busy; }
bool wifi_roam_request_scan(void) { return false; }

#endif
