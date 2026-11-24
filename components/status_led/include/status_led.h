#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Optional RGB status LED (WS2812 or three GPIOs, see menuconfig "Status
 * LED"). With STATUS_LED_NONE every function is a no-op.
 *
 * Network state is tracked internally from WiFi / Ethernet / IP events; the
 * snapclient task and the Sendspin player each report their server
 * connection, and the DAC task reports playback.
 */
void status_led_init(void);
// snapserver connection
void status_led_set_server_connected(bool connected);
// Sendspin server connection; either server counts as "connected"
void status_led_set_sendspin_connected(bool connected);
void status_led_set_playing(bool playing);
void status_led_set_error(bool error);
// rescue / setup access point active (cyan, blinking)
void status_led_set_rescue(bool rescue);
// Sendspin pairing: 0 none, 1 pairing window open (violet), 2 a server waits
// for the window (violet, blinking)
void status_led_set_pairing(uint8_t state);
