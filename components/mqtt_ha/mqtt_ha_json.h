// JSON texts for mqtt_ha: a small builder, the discovery message and the
// state message. Plain C with no ESP-IDF calls, so it can be tested on a PC.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "mqtt_ha.h"

// Text built into a fixed buffer. When something does not fit, `full` is
// set and the text must not be published.
typedef struct {
  char *buf;
  size_t size;
  size_t len;
  bool full;
} mqtt_ha_jb_t;

void mqtt_ha_jb_init(mqtt_ha_jb_t *j, char *buf, size_t size);
void mqtt_ha_jb_add(mqtt_ha_jb_t *j, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
// A quoted JSON string: escapes '"' and '\', drops control characters.
void mqtt_ha_jb_str(mqtt_ha_jb_t *j, const char *s);

typedef struct {
  const char *device_id;  // "snapclient_<mac12>"
  const char *base;       // "snapclient/<mac12>"
  const char *mac;        // "aa:bb:cc:dd:ee:ff"
  const char *name;       // device name
  const char *firmware;
  const char *ip;         // "" leaves the configuration URL out
  bool sendspin;          // Sendspin built in
  bool pair_sw;           // HA may change Require pairing
} mqtt_ha_disc_t;

// The device discovery message (homeassistant/device/<id>/config).
void mqtt_ha_discovery_json(mqtt_ha_jb_t *j, const mqtt_ha_disc_t *d);
// The ~/state message.
void mqtt_ha_state_json(mqtt_ha_jb_t *j, const mqtt_ha_state_t *st);
