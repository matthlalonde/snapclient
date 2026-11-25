// JSON texts for mqtt_ha, see mqtt_ha_json.h.

#include "sdkconfig.h"

#if CONFIG_HA_MQTT_ENABLE

#include "mqtt_ha_json.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

void mqtt_ha_jb_init(mqtt_ha_jb_t *j, char *buf, size_t size) {
  j->buf = buf;
  j->size = size;
  j->len = 0;
  j->full = size == 0;
  if (size) buf[0] = '\0';
}

void mqtt_ha_jb_add(mqtt_ha_jb_t *j, const char *fmt, ...) {
  if (j->full) return;
  size_t left = j->size - j->len;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(j->buf + j->len, left, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= left) {
    j->full = true;
    return;
  }
  j->len += (size_t)n;
}

static void put(mqtt_ha_jb_t *j, char c) {
  if (j->full) return;
  if (j->len + 1 >= j->size) {
    j->full = true;
    return;
  }
  j->buf[j->len++] = c;
  j->buf[j->len] = '\0';
}

void mqtt_ha_jb_str(mqtt_ha_jb_t *j, const char *s) {
  put(j, '"');
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c < 0x20) continue;
    if (c == '"' || c == '\\') put(j, '\\');
    put(j, (char)c);
  }
  put(j, '"');
}

// --- discovery ---------------------------------------------------------------

enum { P_SENSOR, P_BINARY, P_SWITCH, P_BUTTON, P_EVENT };
static const char *const platforms[] = {"sensor", "binary_sensor", "switch",
                                        "button", "event"};

// T_STATE uses the shared stat_t at the top level
enum { T_STATE, T_DIAG, T_INFO, T_EVENT };
static const char *const topics[] = {"", "diag", "info", "event"};

// when a component is sent in full; otherwise it goes as a stub {"p":...},
// which removes it in HA (leaving it out would not)
enum { W_ALWAYS, W_SENDSPIN, W_PAIR_SW, W_PAIR_BS };

#define DIAG "\"ent_cat\":\"diagnostic\""
#define MEAS "\"stat_cla\":\"measurement\","
#define CONN "\"dev_cla\":\"connectivity\""
#define BYTES "\"dev_cla\":\"data_size\",\"unit_of_meas\":\"B\"," MEAS DIAG
#define SWITCH(tok)                                                 \
  "\"ent_cat\":\"config\",\"pl_on\":\"" tok "_on\",\"pl_off\":\"" tok \
  "_off\",\"stat_on\":\"ON\",\"stat_off\":\"OFF\""

typedef struct {
  const char *id;     // component id; the unique id is <device id>_<id>
  const char *name;
  const char *key;    // value_json key: NULL = the id, "" = no template
  const char *extra;  // more keys, as JSON, or NULL
  uint8_t platform;
  uint8_t topic;
  uint8_t when;
} comp_t;

static const comp_t comps[] = {
    {"source", "Source", NULL, NULL, P_SENSOR, T_STATE, W_ALWAYS},
    {"snapcast", "Snapcast", NULL, NULL, P_SENSOR, T_STATE, W_ALWAYS},
    {"sendspin", "Sendspin", NULL, NULL, P_SENSOR, T_STATE, W_SENDSPIN},
    {"pairing", "Pairing", NULL, NULL, P_SENSOR, T_STATE, W_SENDSPIN},
    {"snapcast_conn", "Snapcast connected", NULL, CONN, P_BINARY, T_STATE,
     W_ALWAYS},
    {"sendspin_conn", "Sendspin connected", NULL, CONN, P_BINARY, T_STATE,
     W_SENDSPIN},
    {"restart_needed", "Restart needed", NULL, NULL, P_BINARY, T_STATE,
     W_ALWAYS},
    {"interruptions", "Playback interruptions", NULL,
     "\"stat_cla\":\"total_increasing\"", P_SENSOR, T_STATE, W_ALWAYS},
    {"snapcast_sw", "Snapcast player", NULL, SWITCH("snapcast"), P_SWITCH,
     T_STATE, W_ALWAYS},
    {"sendspin_sw", "Sendspin player", NULL, SWITCH("sendspin"), P_SWITCH,
     T_STATE, W_SENDSPIN},
    {"pair_req_sw", "Require pairing", "pair_req", SWITCH("reqpair"),
     P_SWITCH, T_STATE, W_PAIR_SW},
    // a binary sensor can't be a config entity in HA
    {"pair_req", "Require pairing", NULL, DIAG, P_BINARY, T_STATE, W_PAIR_BS},
    {"restart", "Restart", "",
     "\"dev_cla\":\"restart\",\"ent_cat\":\"config\",\"pl_prs\":\"restart\"",
     P_BUTTON, T_STATE, W_ALWAYS},
    {"pair_allow", "Allow pairing", "", "\"pl_prs\":\"pair_allow\"", P_BUTTON,
     T_STATE, W_SENDSPIN},
    {"pair_stop", "Stop pairing", "", "\"pl_prs\":\"pair_stop\"", P_BUTTON,
     T_STATE, W_SENDSPIN},
    {"pair_event", "Pairing request", "", "\"evt_typ\":[\"pair_request\"]",
     P_EVENT, T_EVENT, W_SENDSPIN},
    {"rssi", "WiFi signal", NULL,
     "\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\"," MEAS DIAG,
     P_SENSOR, T_DIAG, W_ALWAYS},
    {"sync_error", "Sync error", NULL, "\"unit_of_meas\":\"ms\"," MEAS DIAG,
     P_SENSOR, T_DIAG, W_ALWAYS},
    {"heap_free", "Free memory", NULL, BYTES, P_SENSOR, T_DIAG, W_ALWAYS},
    {"heap_min", "Lowest free memory", NULL, BYTES, P_SENSOR, T_DIAG,
     W_ALWAYS},
    {"uptime", "Uptime", NULL,
     "\"dev_cla\":\"duration\",\"unit_of_meas\":\"s\"," MEAS DIAG, P_SENSOR,
     T_DIAG, W_ALWAYS},
    {"firmware", "Firmware", NULL, DIAG, P_SENSOR, T_INFO, W_ALWAYS},
    {"ip", "IP address", NULL, DIAG, P_SENSOR, T_INFO, W_ALWAYS},
    {"last_restart", "Last restart", NULL, DIAG, P_SENSOR, T_INFO, W_ALWAYS},
};

static bool comp_used(const comp_t *c, const mqtt_ha_disc_t *d) {
  switch (c->when) {
    case W_ALWAYS:
      return true;
    case W_SENDSPIN:
      return d->sendspin;
    case W_PAIR_SW:
      return d->sendspin && d->pair_sw;
    default:
      return d->sendspin && !d->pair_sw;
  }
}

void mqtt_ha_discovery_json(mqtt_ha_jb_t *j, const mqtt_ha_disc_t *d) {
  mqtt_ha_jb_add(j, "{\"dev\":{\"ids\":[\"%s\"],\"cns\":[[\"mac\",\"%s\"]],\"name\":",
                 d->device_id, d->mac);
  mqtt_ha_jb_str(j, d->name);
  mqtt_ha_jb_add(j, ",\"mf\":\"Espressif\",\"mdl\":\"ESP32-S3 snapclient\",\"sw\":");
  mqtt_ha_jb_str(j, d->firmware);
  if (d->ip[0]) mqtt_ha_jb_add(j, ",\"cu\":\"http://%s/\"", d->ip);
  mqtt_ha_jb_add(j, "},\"o\":{\"name\":\"snapclient\",\"sw\":");
  mqtt_ha_jb_str(j, d->firmware);
  mqtt_ha_jb_add(j,
                 "},\"avty\":[{\"t\":\"%s/avty\"}],\"stat_t\":\"%s/state\","
                 "\"cmd_t\":\"%s/cmd\",\"cmps\":{",
                 d->base, d->base, d->base);
  for (size_t i = 0; i < sizeof comps / sizeof comps[0]; i++) {
    const comp_t *c = &comps[i];
    mqtt_ha_jb_add(j, "%s\"%s\":{\"p\":\"%s\"", i ? "," : "", c->id,
                   platforms[c->platform]);
    if (comp_used(c, d)) {
      mqtt_ha_jb_add(j, ",\"name\":\"%s\",\"uniq_id\":\"%s_%s\"", c->name,
                     d->device_id, c->id);
      if (c->topic != T_STATE) {
        mqtt_ha_jb_add(j, ",\"stat_t\":\"%s/%s\"", d->base, topics[c->topic]);
      }
      const char *key = c->key ? c->key : c->id;
      if (key[0]) mqtt_ha_jb_add(j, ",\"val_tpl\":\"{{ value_json.%s }}\"", key);
      if (c->extra) mqtt_ha_jb_add(j, ",%s", c->extra);
    }
    mqtt_ha_jb_add(j, "}");
  }
  mqtt_ha_jb_add(j, "}}");
}

// --- state ---------------------------------------------------------------------

static const char *on_off(bool on) { return on ? "ON" : "OFF"; }

void mqtt_ha_state_json(mqtt_ha_jb_t *j, const mqtt_ha_state_t *st) {
  mqtt_ha_jb_add(j, "{\"source\":");
  mqtt_ha_jb_str(j, st->source);
  mqtt_ha_jb_add(j, ",\"snapcast\":");
  mqtt_ha_jb_str(j, st->snapcast);
  mqtt_ha_jb_add(j, ",\"sendspin\":");
  mqtt_ha_jb_str(j, st->sendspin);
  mqtt_ha_jb_add(j, ",\"pairing\":");
  mqtt_ha_jb_str(j, st->pairing);
  mqtt_ha_jb_add(j,
                 ",\"snapcast_conn\":\"%s\",\"sendspin_conn\":\"%s\","
                 "\"restart_needed\":\"%s\",\"interruptions\":%" PRIu32
                 ",\"snapcast_sw\":\"%s\",\"sendspin_sw\":\"%s\","
                 "\"pair_req\":\"%s\"}",
                 on_off(st->snapcast_connected), on_off(st->sendspin_connected),
                 on_off(st->restart_needed), st->interruptions,
                 on_off(st->snapcast_saved_on), on_off(st->sendspin_saved_on),
                 on_off(st->require_pairing));
}

#endif  // CONFIG_HA_MQTT_ENABLE
