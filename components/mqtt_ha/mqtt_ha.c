// The board in Home Assistant over MQTT, see mqtt_ha.h.
//
// One task of ours (mqtt_ha) does all the work: it starts the esp-mqtt client
// once the board has an IP, publishes discovery and the topics below, checks
// the live state every second and runs the commands. esp-mqtt's event handler
// only copies: it sets notification bits and queues commands. It never
// blocks, because it runs on esp-mqtt's task or nested inside our publish.
//
// Topics under snapclient/<mac12>/:
//   avty   "online" / "offline" (retained; the last will is "offline")
//   state  JSON, on change (retained)
//   diag   JSON, every 60 s and on connect (retained)
//   info   JSON, on connect and when HA starts (retained)
//   event  a server asks to pair (not retained)
//   cmd    commands from HA, bare tokens (subscribed)

#include "mqtt_ha.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_HA_MQTT_ENABLE

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "mqtt_ha_json.h"
#include "settings_manager.h"

static const char *TAG = "MQTT_HA";

#define TASK_STACK 5632    // ours, internal RAM: the switches write NVS
#define CLIENT_STACK 5120  // esp-mqtt's task
#define DISC_SIZE 6144     // the discovery message
#define STATE_SIZE 384
#define CMD_MAX_LEN 32
#define SEC 1000000LL
#define BACKOFF_MIN_S 10
#define BACKOFF_MAX_S 300

// notification bits for our task
#define BIT_LINK 0x01      // connected, disconnected or a subscription refused
#define BIT_CMD 0x02       // a command is queued
#define BIT_BIRTH 0x04     // HA said "online"
#define BIT_PUBACK 0x08    // the broker acknowledged a QoS 1 message
#define BIT_SHUTDOWN 0x10  // a restart is under way: publish "offline"
#define BIT_REMOVE 0x20    // "Remove from Home Assistant"
#define BITS_MAIN (BIT_LINK | BIT_CMD | BIT_BIRTH | BIT_SHUTDOWN | BIT_REMOVE)

// why the last connect attempt failed (set by the event handler)
enum { ERR_NONE, ERR_UNREACHABLE, ERR_LOGIN, ERR_REFUSED };
// what the event log last heard from us, so each problem is logged once
enum { LOG_NONE, LOG_CONNECTED, LOG_LOST, LOG_LOGIN, LOG_UNREACHABLE };

// the command tokens on ~/cmd, in mqtt_ha_cmd_t order
static const char *const s_tokens[] = {
    "restart",     "pair_allow",   "pair_stop",   "reqpair_on",   "reqpair_off",
    "snapcast_on", "snapcast_off", "sendspin_on", "sendspin_off",
};
#define CMD_COUNT (sizeof s_tokens / sizeof s_tokens[0])
#define SENDSPIN_CMDS                                                   \
  ((1u << MQTT_HA_CMD_PAIR_ALLOW) | (1u << MQTT_HA_CMD_PAIR_STOP) |    \
   (1u << MQTT_HA_CMD_REQUIRE_PAIRING_ON) |                            \
   (1u << MQTT_HA_CMD_REQUIRE_PAIRING_OFF) |                           \
   (1u << MQTT_HA_CMD_SENDSPIN_ON) | (1u << MQTT_HA_CMD_SENDSPIN_OFF))

typedef struct {
  mqtt_ha_config_t cfg;  // cfg.device_name points at name
  char name[64];
  bool pair_sw;  // HA may change Require pairing (read at start)
  esp_mqtt_client_handle_t client;
  QueueHandle_t cmds;
  SemaphoreHandle_t offline_done;
  char t_avty[32], t_state[32], t_diag[32], t_info[32], t_event[32], t_cmd[32];
  char t_disc[96], t_birth[80];
  // our task only
  char last_state[STATE_SIZE];  // the state JSON last published
  uint32_t pair_seen;           // pair_requests already told
  bool pair_fresh;              // take the next count as told (just connected)
  int64_t last_cmd_us[CMD_COUNT];
  uint8_t logged;  // LOG_*
} ctx_t;

static ctx_t *s_ctx;
static TaskHandle_t s_task;
static uint32_t s_pending;  // our task only: bits not handled yet

// written by the event handler (its calls are serialized by esp-mqtt's lock)
static volatile bool s_up;
static volatile uint32_t s_ups, s_downs;
static volatile uint8_t s_err;
static volatile bool s_sub_refused;
// no more publishing: removed from HA, or a restart is under way
static volatile bool s_removed, s_stopping;

// IDs from the WiFi MAC, set once
static char s_mac[18], s_device_id[24], s_base[24];

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static mqtt_ha_status_t s_st = {.state = "off"};
static int64_t s_since_us;  // connected since; under s_mux

static void ensure_ids(void) {
  if (s_base[0]) return;
  uint8_t m[6] = {0};
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1],
           m[2], m[3], m[4], m[5]);
  snprintf(s_device_id, sizeof s_device_id, "snapclient_%02x%02x%02x%02x%02x%02x",
           m[0], m[1], m[2], m[3], m[4], m[5]);
  snprintf(s_base, sizeof s_base, "snapclient/%02x%02x%02x%02x%02x%02x", m[0],
           m[1], m[2], m[3], m[4], m[5]);
}

// --- status --------------------------------------------------------------------

// NULL keeps the current value
static void st_set(const char *state, const char *error) {
  taskENTER_CRITICAL(&s_mux);
  if (state) s_st.state = state;
  if (error) strlcpy(s_st.last_error, error, sizeof s_st.last_error);
  taskEXIT_CRITICAL(&s_mux);
}

static void st_connected(bool on) {
  int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  s_st.connected = on;
  if (on) {
    s_st.connects++;
    s_since_us = now;
  }
  taskEXIT_CRITICAL(&s_mux);
}

static void log_once(uint8_t what, event_level_t level, const char *text) {
  if (s_ctx->logged == what) return;
  s_ctx->logged = what;
  event_log_add(EVENT_SRC_MQTT, level, text);
}

// --- event handler (esp-mqtt's task, or nested in our publish) -------------------

static void notify(uint32_t bits) {
  TaskHandle_t t = s_task;
  if (t) xTaskNotify(t, bits, eSetBits);
}

static bool topic_is(const esp_mqtt_event_t *ev, const char *topic) {
  size_t n = strlen(topic);
  return ev->topic && (size_t)ev->topic_len == n && memcmp(ev->topic, topic, n) == 0;
}

static void on_data(const esp_mqtt_event_t *ev) {
  // whole, short, live messages only: retained ones are replays, and an empty
  // one is our own clearing of ~/cmd
  if (ev->retain || ev->current_data_offset != 0 ||
      ev->data_len != ev->total_data_len || ev->data_len <= 0 ||
      ev->data_len > CMD_MAX_LEN) {
    return;
  }
  size_t len = (size_t)ev->data_len;
  if (topic_is(ev, s_ctx->t_birth)) {
    if (len == 6 && memcmp(ev->data, "online", 6) == 0) notify(BIT_BIRTH);
    return;
  }
  if (!topic_is(ev, s_ctx->t_cmd)) return;
  for (uint8_t i = 0; i < CMD_COUNT; i++) {
    if (strlen(s_tokens[i]) == len && memcmp(ev->data, s_tokens[i], len) == 0) {
      if (xQueueSend(s_ctx->cmds, &i, 0) == pdTRUE) notify(BIT_CMD);
      return;
    }
  }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  (void)base;
  const esp_mqtt_event_t *ev = data;
  const esp_mqtt_error_codes_t *e = ev->error_handle;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
      s_up = true;
      s_ups++;
      notify(BIT_LINK);
      break;
    case MQTT_EVENT_DISCONNECTED:
      s_up = false;
      s_downs++;
      notify(BIT_LINK);
      break;
    case MQTT_EVENT_ERROR:
      if (!e) break;
      if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        bool login = e->connect_return_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                     e->connect_return_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
        s_err = login ? ERR_LOGIN : ERR_REFUSED;
      } else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        s_err = ERR_UNREACHABLE;
      }
      break;
    case MQTT_EVENT_SUBSCRIBED:
      if (e && e->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
        s_sub_refused = true;
        notify(BIT_LINK);
      }
      break;
    case MQTT_EVENT_PUBLISHED:
      notify(BIT_PUBACK);
      break;
    case MQTT_EVENT_DATA:
      on_data(ev);
      break;
    default:
      break;
  }
}

// --- our task ------------------------------------------------------------------------

// Waits up to ms for any of `want`. Other bits stay pending for later.
static uint32_t wait_bits(uint32_t want, uint32_t ms) {
  TickType_t start = xTaskGetTickCount(), limit = pdMS_TO_TICKS(ms);
  for (;;) {
    if ((s_pending & want) == 0) {
      TickType_t spent = xTaskGetTickCount() - start;
      uint32_t v = 0;
      if (xTaskNotifyWait(0, UINT32_MAX, &v, spent < limit ? limit - spent : 0) ==
          pdTRUE) {
        s_pending |= v;
      }
    }
    uint32_t got = s_pending & want;
    if (got || xTaskGetTickCount() - start >= limit) {
      s_pending &= ~got;
      return got;
    }
  }
}

static bool pub(const char *topic, const char *data, size_t len, bool retain) {
  if (!s_up || s_removed || s_stopping) return false;
  return esp_mqtt_client_publish(s_ctx->client, topic, data, (int)len, 0,
                                 retain) >= 0;
}

static bool pub_json(const char *topic, const mqtt_ha_jb_t *j, bool retain) {
  if (j->full) {
    ESP_LOGE(TAG, "%s: message too long, not sent", topic);
    return false;
  }
  return pub(topic, j->buf, j->len, retain);
}

static void get_ip(char *ip, size_t len) {
  if (!s_ctx->cfg.get_ip(ip, len)) ip[0] = '\0';
  ip[len - 1] = '\0';
}

static void publish_discovery(void) {
  char *buf = NULL;
#if CONFIG_SPIRAM
  buf = heap_caps_malloc(DISC_SIZE, MALLOC_CAP_SPIRAM);
#endif
  if (!buf) buf = malloc(DISC_SIZE);
  if (!buf) {
    ESP_LOGE(TAG, "no memory for the discovery message");
    return;
  }
  char ip[16];
  get_ip(ip, sizeof ip);
  const mqtt_ha_disc_t d = {
      .device_id = s_device_id,
      .base = s_base,
      .mac = s_mac,
      .name = s_ctx->name,
      .firmware = esp_app_get_description()->version,
      .ip = ip,
      .sendspin = s_ctx->cfg.sendspin_built,
      .pair_sw = s_ctx->pair_sw,
  };
  mqtt_ha_jb_t j;
  mqtt_ha_jb_init(&j, buf, DISC_SIZE);
  mqtt_ha_discovery_json(&j, &d);
  pub_json(s_ctx->t_disc, &j, true);
  free(buf);
}

static void publish_info(void) {
  char ip[16], buf[192];
  get_ip(ip, sizeof ip);
  mqtt_ha_jb_t j;
  mqtt_ha_jb_init(&j, buf, sizeof buf);
  mqtt_ha_jb_add(&j, "{\"firmware\":");
  mqtt_ha_jb_str(&j, esp_app_get_description()->version);
  mqtt_ha_jb_add(&j, ",\"ip\":");
  mqtt_ha_jb_str(&j, ip);
  mqtt_ha_jb_add(&j, ",\"last_restart\":");
  mqtt_ha_jb_str(&j, event_log_reset_reason_text(esp_reset_reason()));
  mqtt_ha_jb_add(&j, "}");
  pub_json(s_ctx->t_info, &j, true);
}

static void publish_diag(const mqtt_ha_state_t *st) {
  char buf[160];
  mqtt_ha_jb_t j;
  mqtt_ha_jb_init(&j, buf, sizeof buf);
  wifi_ap_record_t ap;
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    mqtt_ha_jb_add(&j, "{\"rssi\":%d", ap.rssi);
  } else {
    mqtt_ha_jb_add(&j, "{\"rssi\":null");
  }
  if (st->sync_valid) {
    long c = (long)(st->sync_error_us / 10);  // hundredths of a ms
    mqtt_ha_jb_add(&j, ",\"sync_error\":%s%ld.%02ld", c < 0 ? "-" : "",
                   labs(c) / 100, labs(c) % 100);
  } else {
    mqtt_ha_jb_add(&j, ",\"sync_error\":null");
  }
  mqtt_ha_jb_add(&j, ",\"heap_free\":%u,\"heap_min\":%u,\"uptime\":%" PRIu32 "}",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 (uint32_t)(esp_timer_get_time() / SEC));
  pub_json(s_ctx->t_diag, &j, true);
}

static void publish_event(const char *server) {
  char buf[160];
  mqtt_ha_jb_t j;
  mqtt_ha_jb_init(&j, buf, sizeof buf);
  mqtt_ha_jb_add(&j, "{\"event_type\":\"pair_request\",\"server\":");
  mqtt_ha_jb_str(&j, server);
  mqtt_ha_jb_add(&j, "}");
  pub_json(s_ctx->t_event, &j, false);
}

// Reads the live state; publishes ~/state when it changed (or `force`), an
// event for a new pairing request, and ~/diag when `diag`.
static void check_state(bool force, bool diag) {
  mqtt_ha_state_t st;
  memset(&st, 0, sizeof st);
  s_ctx->cfg.get_state(&st);
  st.source[sizeof st.source - 1] = '\0';
  st.snapcast[sizeof st.snapcast - 1] = '\0';
  st.sendspin[sizeof st.sendspin - 1] = '\0';
  st.pairing[sizeof st.pairing - 1] = '\0';
  st.pair_server[sizeof st.pair_server - 1] = '\0';

  char buf[STATE_SIZE];
  mqtt_ha_jb_t j;
  mqtt_ha_jb_init(&j, buf, sizeof buf);
  mqtt_ha_state_json(&j, &st);
  if ((force || strcmp(buf, s_ctx->last_state) != 0) && pub_json(s_ctx->t_state, &j, true)) {
    memcpy(s_ctx->last_state, buf, j.len + 1);
  }

  if (s_ctx->pair_fresh) {
    s_ctx->pair_fresh = false;
    s_ctx->pair_seen = st.pair_requests;
  } else if (st.pair_requests != s_ctx->pair_seen) {
    publish_event(st.pair_server);
    s_ctx->pair_seen = st.pair_requests;
  }
  if (diag) publish_diag(&st);
}

static void run_command(uint8_t c) {
  const mqtt_ha_config_t *cfg = &s_ctx->cfg;
  int64_t now = esp_timer_get_time();
  if (((SENDSPIN_CMDS >> c) & 1) && !cfg->sendspin_built) return;
  if ((c == MQTT_HA_CMD_REQUIRE_PAIRING_ON || c == MQTT_HA_CMD_REQUIRE_PAIRING_OFF) &&
      !s_ctx->pair_sw) {
    ESP_LOGW(TAG, "%s ignored: HA may not change Require pairing", s_tokens[c]);
    return;
  }
  if (c == MQTT_HA_CMD_RESTART && now < 60 * SEC) {
    ESP_LOGW(TAG, "restart ignored: started less than 60 s ago");
    return;
  }
  int64_t gap = c == MQTT_HA_CMD_RESTART      ? 0
                : c == MQTT_HA_CMD_PAIR_ALLOW ? 60 * SEC
                                              : 5 * SEC;
  if (s_ctx->last_cmd_us[c] && now - s_ctx->last_cmd_us[c] < gap) {
    ESP_LOGW(TAG, "%s ignored: too soon after the last one", s_tokens[c]);
    return;
  }
  s_ctx->last_cmd_us[c] = now;
  ESP_LOGI(TAG, "command %s", s_tokens[c]);
  if (!cfg->command((mqtt_ha_cmd_t)c)) ESP_LOGW(TAG, "%s refused", s_tokens[c]);
}

static void on_connected(void) {
  st_connected(true);
  st_set("connected", "");
  // After a plain loss the board is back within seconds: no "Connected" line,
  // so a broker that keeps dropping it fills one repeated "Lost" slot
  if (s_ctx->logged != LOG_LOST) {
    log_once(LOG_CONNECTED, EVENT_INFO, "Connected to the MQTT broker");
  }
  ESP_LOGI(TAG, "connected");
  esp_mqtt_client_subscribe_single(s_ctx->client, s_ctx->t_cmd, 0);
  esp_mqtt_client_subscribe_single(s_ctx->client, s_ctx->t_birth, 0);
  publish_discovery();
  pub(s_ctx->t_avty, "online", 6, true);
  pub(s_ctx->t_cmd, NULL, 0, true);  // clears a stray retained command
  s_ctx->pair_fresh = true;  // requests from before this connection are old
  check_state(true, true);
  publish_info();
}

static void on_lost(void) {
  st_connected(false);
  s_err = ERR_NONE;
  if (s_removed) return;
  st_set("connecting", "Lost the connection");
  // always added: back-to-back losses merge into one counted event
  s_ctx->logged = LOG_LOST;
  event_log_add(EVENT_SRC_MQTT, EVENT_WARN, "Lost the connection to the MQTT broker");
}

static void on_failed(void) {
  uint8_t e = s_err;
  s_err = ERR_NONE;
  if (s_removed) return;
  if (e == ERR_LOGIN) {
    st_set("login refused", "The broker refused the login");
    log_once(LOG_LOGIN, EVENT_ERROR, "The MQTT broker refused the login");
  } else {
    st_set("can't reach the broker", e == ERR_REFUSED
                                         ? "The broker refused the connection"
                                         : "Can't reach the broker");
    log_once(LOG_UNREACHABLE, EVENT_WARN, "Can't reach the MQTT broker");
  }
}

// Clears discovery and our retained topics. The last clear goes out with
// QoS 1: the broker handles one connection in order, so its ack shows all of
// them arrived. Either way no more MQTT until a restart, so an unacked clear
// left in esp-mqtt's outbox can't be resent after a reconnect.
static void do_remove(void) {
  if (s_removed) return;
  if (!s_up) {
    st_set(NULL, "Not connected: nothing was removed");
    return;
  }
  pub(s_ctx->t_disc, NULL, 0, true);
  pub(s_ctx->t_state, NULL, 0, true);
  pub(s_ctx->t_info, NULL, 0, true);
  pub(s_ctx->t_diag, NULL, 0, true);
  s_pending &= ~BIT_PUBACK;
  bool acked = s_up &&
               esp_mqtt_client_publish(s_ctx->client, s_ctx->t_avty, NULL, 0, 1, 1) > 0 &&
               wait_bits(BIT_PUBACK, 2000);
  s_removed = true;
  esp_mqtt_client_disconnect(s_ctx->client);
  if (acked) {
    settings_mqtt_update_t u = {.has_on = true, .on = false};
    if (settings_set_mqtt(&u, NULL) != ESP_OK) ESP_LOGE(TAG, "could not turn MQTT off");
  }
  taskENTER_CRITICAL(&s_mux);
  s_st.removed = acked;
  s_st.stopped = true;
  s_st.state = acked ? "removed" : "stopped";
  strlcpy(s_st.last_error, acked ? "" : "Removal failed: restart and try again",
          sizeof s_st.last_error);
  taskEXIT_CRITICAL(&s_mux);
  if (acked) {
    event_log_add(EVENT_SRC_MQTT, EVENT_INFO,
                  "Removed from Home Assistant; MQTT turns off at the next restart");
  } else {
    event_log_add(EVENT_SRC_MQTT, EVENT_WARN,
                  "Could not remove from Home Assistant; MQTT stopped until a restart");
  }
}

// "offline" (QoS 1, retained) before a restart; waits up to 500 ms for the
// broker's acknowledgement. Our task only.
static void go_offline(void) {
  bool send = s_up && !s_removed && !s_stopping;
  s_stopping = true;
  if (!send) return;
  s_pending &= ~BIT_PUBACK;
  if (esp_mqtt_client_publish(s_ctx->client, s_ctx->t_avty, "offline", 7, 1, 1) > 0) {
    wait_bits(BIT_PUBACK, 500);
  }
}

static void shutdown_handler(void) {
  TaskHandle_t t = s_task;
  if (!t || !s_up || s_removed) return;
  if (xTaskGetCurrentTaskHandle() == t) {
    go_offline();
    return;
  }
  xSemaphoreTake(s_ctx->offline_done, 0);
  xTaskNotify(t, BIT_SHUTDOWN, eSetBits);
  xSemaphoreTake(s_ctx->offline_done, pdMS_TO_TICKS(600));
}

static void wipe(void *p, size_t n) {
  volatile char *v = p;
  while (n--) *v++ = 0;
}

static bool client_init(void) {
  char host[SETTINGS_MQTT_HOST_MAX] = "", user[SETTINGS_MQTT_USER_MAX] = "";
  char pass[SETTINGS_MQTT_PASS_MAX] = "";
  int32_t port = 1883;
  if (settings_get_mqtt_host(host, sizeof host) != ESP_OK) host[0] = '\0';
  if (settings_get_mqtt_port(&port) != ESP_OK || port < 1 || port > 65535) port = 1883;
  if (settings_get_mqtt_user(user, sizeof user) != ESP_OK) user[0] = '\0';
  if (settings_get_mqtt_pass(pass, sizeof pass) != ESP_OK) pass[0] = '\0';
  const esp_mqtt_client_config_t mc = {
      .broker.address = {.hostname = host,
                         .port = (uint32_t)port,
                         .transport = MQTT_TRANSPORT_OVER_TCP},
      .credentials = {.username = user[0] ? user : NULL,
                      .client_id = s_device_id,
                      .authentication.password = pass[0] ? pass : NULL},
      .session = {.last_will = {.topic = s_ctx->t_avty,
                                .msg = "offline",
                                .qos = 1,
                                .retain = 1},
                  .keepalive = 30},
      .network = {.timeout_ms = 5000, .disable_auto_reconnect = true},
      .task.stack_size = CLIENT_STACK,
      .outbox.limit = 8 * 1024,
  };
  esp_log_level_set("mqtt_client", ESP_LOG_WARN);
  s_ctx->client = host[0] ? esp_mqtt_client_init(&mc) : NULL;
  wipe(pass, sizeof pass);  // esp-mqtt keeps its own copy
  if (!s_ctx->client) return false;
  return esp_mqtt_client_register_event(s_ctx->client, MQTT_EVENT_ANY, on_event,
                                        NULL) == ESP_OK;
}

static void give_up(void) {
  ESP_LOGE(TAG, "could not start the MQTT client");
  event_log_add(EVENT_SRC_MQTT, EVENT_ERROR, "Could not start the MQTT client");
  taskENTER_CRITICAL(&s_mux);
  s_st.enabled = false;
  s_st.state = "off";
  strlcpy(s_st.last_error, "Could not start the MQTT client", sizeof s_st.last_error);
  s_st.stack_free = 0;
  taskEXIT_CRITICAL(&s_mux);
  s_task = NULL;
  vTaskDelete(NULL);
}

static void mqtt_ha_task(void *arg) {
  (void)arg;
  s_task = xTaskGetCurrentTaskHandle();
  if (!client_init()) give_up();

  bool started = false, up = false, waited = false;
  uint32_t seen_ups = 0, seen_downs = 0, ticks = 0;
  uint32_t backoff_s = BACKOFF_MIN_S;
  int64_t now = esp_timer_get_time(), next_tick = now, retry_at = 0;
  int64_t since = 0, next_diag = 0, birth_at = 0, last_birth = 0;

  for (;;) {
    now = esp_timer_get_time();
    uint32_t ms = next_tick > now ? (uint32_t)((next_tick - now + 999) / 1000) : 0;
    uint32_t bits = wait_bits(BITS_MAIN, ms);
    now = esp_timer_get_time();

    if (bits & BIT_SHUTDOWN) {
      go_offline();
      xSemaphoreGive(s_ctx->offline_done);
      for (;;) vTaskDelay(portMAX_DELAY);  // the restart follows
    }

    if (bits & BIT_LINK) {
      uint32_t downs = s_downs, ups = s_ups;
      if (downs != seen_downs) {
        seen_downs = downs;
        if (up) {
          up = false;
          on_lost();
        } else {
          on_failed();
        }
        if (!s_removed) {
          retry_at = now + backoff_s * SEC;
          backoff_s = backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : backoff_s * 2;
        }
      }
      if (ups != seen_ups) {
        seen_ups = ups;
        if (s_up) {
          up = true;
          retry_at = 0;
          since = now;
          next_diag = now + 60 * SEC;
          on_connected();
        }
      }
      if (s_sub_refused) {
        s_sub_refused = false;
        st_set(NULL, "The broker refused a subscription");
      }
    }
    if (bits & BIT_REMOVE) do_remove();
    if (bits & BIT_CMD) {
      uint8_t c;
      while (xQueueReceive(s_ctx->cmds, &c, 0) == pdTRUE) {
        if (c < CMD_COUNT) run_command(c);
      }
    }
    if ((bits & BIT_BIRTH) && up && !birth_at &&
        (!last_birth || now - last_birth >= 30 * SEC)) {
      birth_at = now + (1000 + esp_random() % 4001) * 1000LL;  // 1-5 s
    }

    if (now < next_tick) continue;
    next_tick = now + SEC;

    UBaseType_t free_stack = uxTaskGetStackHighWaterMark(NULL);
    uint32_t mqtt_free = 0;
    bool measure = started && ++ticks % 10 == 0;
    if (measure) {
      TaskHandle_t t = xTaskGetHandle("mqtt_task");
      mqtt_free = t ? (uint32_t)uxTaskGetStackHighWaterMark(t) : 0;
    }
    taskENTER_CRITICAL(&s_mux);
    s_st.stack_free = (uint32_t)free_stack;
    if (measure) s_st.mqtt_stack_free = mqtt_free;
    taskEXIT_CRITICAL(&s_mux);

    char ip[16];
    if (!started) {
      get_ip(ip, sizeof ip);
      if (!ip[0]) {
        st_set("waiting for network", NULL);
        continue;
      }
      if (esp_mqtt_client_start(s_ctx->client) != ESP_OK) give_up();
      started = true;
      st_set("connecting", NULL);
      continue;
    }
    if (retry_at && now >= retry_at) {
      get_ip(ip, sizeof ip);
      if (!ip[0]) {
        st_set("waiting for network", NULL);
        waited = true;
        retry_at = now + SEC;
      } else {
        s_err = ERR_NONE;
        if (waited) st_set("connecting", NULL);
        waited = false;
        retry_at = esp_mqtt_client_reconnect(s_ctx->client) == ESP_OK ? 0 : now + SEC;
      }
    }
    if (!up || s_removed) continue;

    // a connection that holds for a minute resets the retry delay
    if (backoff_s != BACKOFF_MIN_S && now - since >= 60 * SEC) backoff_s = BACKOFF_MIN_S;
    bool diag = now >= next_diag;
    if (diag) next_diag = now + 60 * SEC;
    if (birth_at && now >= birth_at) {
      // HA restarted: send everything again
      birth_at = 0;
      last_birth = now;
      next_diag = now + 60 * SEC;
      publish_discovery();
      check_state(true, true);
      publish_info();
    } else {
      check_state(false, diag);
    }
  }
}

// --- public ------------------------------------------------------------------------

static esp_err_t start_failed(ctx_t *c) {
  if (c->cmds) vQueueDelete(c->cmds);
  if (c->offline_done) vSemaphoreDelete(c->offline_done);
  free(c);
  return ESP_ERR_NO_MEM;
}

esp_err_t mqtt_ha_start(const mqtt_ha_config_t *cfg) {
  if (!cfg || !cfg->get_state || !cfg->command || !cfg->get_ip) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_ctx) return ESP_ERR_INVALID_STATE;
  ensure_ids();

  bool on = false, pair_sw = false;
  // the host, then "host:port": one buffer, as main's stack is small
  char broker[sizeof s_st.broker] = "";
  _Static_assert(sizeof broker >= SETTINGS_MQTT_HOST_MAX, "broker too small");
  int32_t port = 1883;
  if (settings_get_mqtt_enabled(&on) != ESP_OK || !on ||
      settings_get_mqtt_host(broker, SETTINGS_MQTT_HOST_MAX) != ESP_OK || !broker[0]) {
    return ESP_ERR_INVALID_STATE;
  }
  if (settings_get_mqtt_port(&port) != ESP_OK || port < 1 || port > 65535) port = 1883;
  if (settings_get_mqtt_pair_sw(&pair_sw) != ESP_OK) pair_sw = false;

  ctx_t *c = calloc(1, sizeof *c);
  if (!c) return ESP_ERR_NO_MEM;
  c->cfg = *cfg;
  strlcpy(c->name, cfg->device_name ? cfg->device_name : "", sizeof c->name);
  c->cfg.device_name = c->name;
  c->pair_sw = pair_sw && cfg->sendspin_built;
  snprintf(c->t_avty, sizeof c->t_avty, "%s/avty", s_base);
  snprintf(c->t_state, sizeof c->t_state, "%s/state", s_base);
  snprintf(c->t_diag, sizeof c->t_diag, "%s/diag", s_base);
  snprintf(c->t_info, sizeof c->t_info, "%s/info", s_base);
  snprintf(c->t_event, sizeof c->t_event, "%s/event", s_base);
  snprintf(c->t_cmd, sizeof c->t_cmd, "%s/cmd", s_base);
  int n1 = snprintf(c->t_disc, sizeof c->t_disc, "%s/device/%s/config",
                    CONFIG_HA_MQTT_DISCOVERY_PREFIX, s_device_id);
  int n2 = snprintf(c->t_birth, sizeof c->t_birth, "%s/status",
                    CONFIG_HA_MQTT_DISCOVERY_PREFIX);
  if (n1 < 0 || n2 < 0 || (size_t)n1 >= sizeof c->t_disc ||
      (size_t)n2 >= sizeof c->t_birth) {
    ESP_LOGE(TAG, "the discovery prefix is too long");
    free(c);
    return ESP_ERR_INVALID_SIZE;
  }
  c->cmds = xQueueCreate(4, sizeof(uint8_t));
  c->offline_done = xSemaphoreCreateBinary();
  if (!c->cmds || !c->offline_done) return start_failed(c);

  size_t n = strlen(broker);
  snprintf(broker + n, sizeof broker - n, ":%" PRId32, port);
  taskENTER_CRITICAL(&s_mux);
  s_st.enabled = true;
  s_st.state = "waiting for network";
  memcpy(s_st.broker, broker, sizeof broker);
  taskEXIT_CRITICAL(&s_mux);

  s_ctx = c;
  if (xTaskCreate(mqtt_ha_task, "mqtt_ha", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
    s_ctx = NULL;
    taskENTER_CRITICAL(&s_mux);
    s_st.enabled = false;
    s_st.state = "off";
    s_st.broker[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
    return start_failed(c);
  }
  esp_err_t err = esp_register_shutdown_handler(shutdown_handler);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "no shutdown handler (%s): only the last will says offline",
             esp_err_to_name(err));
  }
  return ESP_OK;
}

bool mqtt_ha_remove(void) {
  TaskHandle_t t = s_task;
  if (!s_ctx || !t || !s_up || s_removed) return false;
  xTaskNotify(t, BIT_REMOVE, eSetBits);
  return true;
}

void mqtt_ha_get_status(mqtt_ha_status_t *out) {
  ensure_ids();
  int64_t now = esp_timer_get_time();
  taskENTER_CRITICAL(&s_mux);
  *out = s_st;
  int64_t since = s_since_us;
  taskEXIT_CRITICAL(&s_mux);
  if (!out->state) out->state = "off";
  strlcpy(out->topic, s_base, sizeof out->topic);
  strlcpy(out->device_id, s_device_id, sizeof out->device_id);
  out->connected_s = out->connected ? (uint32_t)((now - since) / SEC) : 0;
}

#else  // !CONFIG_HA_MQTT_ENABLE

esp_err_t mqtt_ha_start(const mqtt_ha_config_t *cfg) {
  (void)cfg;
  return ESP_ERR_NOT_SUPPORTED;
}

bool mqtt_ha_remove(void) { return false; }

void mqtt_ha_get_status(mqtt_ha_status_t *out) {
  memset(out, 0, sizeof *out);
  out->state = "off";
}

#endif  // CONFIG_HA_MQTT_ENABLE
