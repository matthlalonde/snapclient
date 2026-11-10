/*
 * WM8804(G) S/PDIF transmitter driver for snapclient.
 *
 * The WM8804 listens on the same I2S lines as the DAC and sends the audio
 * out of S/PDIF (optical/coax). It is controlled over I2C (software mode).
 * Two board topologies (menuconfig "I2S clock master"):
 *
 *  CONFIG_WM8804_CLK_ESP32_MASTER: an ESP32 GPIO is wired to WM8804 MCLK.
 *    The ESP32 is I2S master and drives MCLK/BCLK/LRCLK; the WM8804 is an
 *    audio-interface slave whose transmitter runs from that MCLK. No PLL.
 *    Rates above 96 kHz exceed the WM8804's MCLK limit: S/PDIF is off then.
 *
 *  CONFIG_WM8804_CLK_WM8804_MASTER: no ESP32 GPIO reaches WM8804 MCLK. The
 *    WM8804 makes MCLK/BCLK/LRCLK from its crystal with its PLL and the
 *    ESP32 is I2S slave. The PLL fraction is continuously trimmed so the
 *    sample clock stays phase-locked to the ESP32's own clock, which is
 *    what snapclient's sync expects. Rates the PLL cannot make (below
 *    32 kHz, above 96 kHz) fall back to the ESP32 as master with S/PDIF off,
 *    so AUX keeps working.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Reset, identify (device ID 0x8805) and configure. The WM8804 is left as
 * an audio-interface slave with the transmitter powered down. Returns
 * ESP_OK only if the chip is fully configured; otherwise it is reset to its
 * power-on defaults (a quiet slave) and wm8804_present() stays false. */
esp_err_t wm8804_init(void);

/* true once wm8804_init() succeeded */
bool wm8804_present(void);

/* Called before the ESP32 I2S channel is (re)created for a new format.
 * Returns true if the WM8804 will be I2S clock master for this format (the
 * ESP32 must then be I2S slave, 64 fs, no MCLK). If it returns false the
 * WM8804 has already stopped driving the bus, so the ESP32 can be master. */
bool wm8804_prepare(uint32_t sample_rate, uint8_t bits);

/* Called right after the ESP32 I2S channel was set up (MCLK running in
 * ESP32-master mode; pads are inputs in slave mode): configures the format,
 * starts the clocks (WM8804 master) and powers the transmitter up. */
esp_err_t wm8804_set_format(uint32_t sample_rate, uint8_t bits);

/* Called before the ESP32 I2S channel is deleted: transmitter down, and in
 * WM8804-master mode the PLL too (lets the DAC enter clock-halt standby). */
esp_err_t wm8804_stop(void);

esp_err_t wm8804_deinit(void);
