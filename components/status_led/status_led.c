/*
 * RGB status LED for snapclient.
 *
 *   white            booting
 *   amber, blinking  connecting to the network
 *   magenta, blink   no WiFi credentials yet (provision with Improv)
 *   blue             network up, connecting to the snapserver
 *   green, dim       connected to the snapserver, idle / muted
 *   green, bright    playing
 *   red, blinking    error
 *   cyan, blinking   rescue / setup access point
 *   violet           Sendspin pairing window open
 *   violet, blinking a Sendspin server waits for the pairing window
 *
 * The task sleeps until a state change (or the next blink edge), so it does
 * not keep the CPU out of automatic light sleep.
 */
#include "status_led.h"

#include "sdkconfig.h"

#if !(CONFIG_STATUS_LED_WS2812 || CONFIG_STATUS_LED_RGB_GPIO)

void status_led_init(void) {}
void status_led_set_server_connected(bool connected) {}
void status_led_set_playing(bool playing) {}
void status_led_set_error(bool error) {}
void status_led_set_rescue(bool rescue) {}
void status_led_set_pairing(uint8_t state) {}

#else

#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_STATUS_LED_WS2812
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#endif

static const char *TAG = "STATUS_LED";

static volatile bool s_booting = true;
static volatile bool s_wifi_up = false;
static volatile bool s_eth_up = false;
static volatile bool s_need_provisioning = false;
static volatile bool s_server = false;
static volatile bool s_playing = false;
static volatile bool s_error = false;
static volatile bool s_rescue = false;
static volatile uint8_t s_pairing = 0;
static TaskHandle_t s_task = NULL;

/* ---------------------------------------------------------------- */
/* backends                                                          */
/* ---------------------------------------------------------------- */

// A typo here must not take a pin away from the flash/PSRAM (that crashes
// the chip on every boot, and this board has no serial port to recover).
static bool pin_usable(int pin) {
  if (pin < 0) return false;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return false;
  if (pin >= 26 && pin <= 32) return false;  // SPI flash / quad PSRAM
#if CONFIG_SPIRAM_MODE_OCT
  if (pin >= 33 && pin <= 37) return false;  // octal PSRAM
#endif
  return true;
}

#if CONFIG_STATUS_LED_WS2812

#define WS_RES_HZ 10000000  // 0.1 us ticks
static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_enc = NULL;

static esp_err_t backend_init(void) {
  if (!pin_usable(CONFIG_STATUS_LED_WS2812_GPIO)) {
    ESP_LOGE(TAG, "GPIO%d can't be the LED pin", CONFIG_STATUS_LED_WS2812_GPIO);
    return ESP_ERR_INVALID_ARG;
  }
  rmt_tx_channel_config_t tcfg = {
      .gpio_num = CONFIG_STATUS_LED_WS2812_GPIO,
      .clk_src = RMT_CLK_SRC_DEFAULT,
      .resolution_hz = WS_RES_HZ,
      .mem_block_symbols = 64,
      .trans_queue_depth = 1,
  };
  esp_err_t err = rmt_new_tx_channel(&tcfg, &s_chan);
  if (err != ESP_OK) return err;
  // keep driving the (idle low) data line through automatic light sleep
  gpio_sleep_sel_dis(CONFIG_STATUS_LED_WS2812_GPIO);
  // WS2812: T0H 0.3us/T0L 0.9us, T1H 0.9us/T1L 0.3us, MSB first
  rmt_bytes_encoder_config_t ecfg = {
      .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
      .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
      .flags.msb_first = 1,
  };
  return rmt_new_bytes_encoder(&ecfg, &s_enc);
}

static void backend_write(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t buf[CONFIG_STATUS_LED_WS2812_COUNT * 3];
  const uint32_t k = CONFIG_STATUS_LED_BRIGHTNESS;
  for (int i = 0; i < CONFIG_STATUS_LED_WS2812_COUNT; i++) {
    buf[3 * i + 0] = (uint8_t)((g * k) / 255);  // GRB order
    buf[3 * i + 1] = (uint8_t)((r * k) / 255);
    buf[3 * i + 2] = (uint8_t)((b * k) / 255);
  }
  rmt_transmit_config_t xcfg = {.loop_count = 0};
  // enable/disable around each frame so the RMT PM lock is only held while
  // the ~30 us frame is on the wire
  rmt_enable(s_chan);
  if (rmt_transmit(s_chan, s_enc, buf, sizeof(buf), &xcfg) == ESP_OK) {
    rmt_tx_wait_all_done(s_chan, 50);
  }
  rmt_disable(s_chan);
}

#else  // CONFIG_STATUS_LED_RGB_GPIO

static const int s_gpio[3] = {CONFIG_STATUS_LED_R_GPIO,
                              CONFIG_STATUS_LED_G_GPIO,
                              CONFIG_STATUS_LED_B_GPIO};

static esp_err_t backend_init(void) {
  for (int i = 0; i < 3; i++) {
    if (s_gpio[i] < 0) continue;
    if (!pin_usable(s_gpio[i])) {
      ESP_LOGE(TAG, "GPIO%d can't be an LED pin", s_gpio[i]);
      return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t c = {
        .pin_bit_mask = 1ULL << s_gpio[i],
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&c);
    if (err != ESP_OK) return err;
    // keep the level through automatic light sleep
    gpio_sleep_sel_dis(s_gpio[i]);
  }
  return ESP_OK;
}

static void backend_write(uint8_t r, uint8_t g, uint8_t b) {
  const uint8_t v[3] = {r, g, b};
  for (int i = 0; i < 3; i++) {
    if (s_gpio[i] < 0) continue;
    // plain on/off: anything above half brightness is "on"
    int on = v[i] >= 128;
#if CONFIG_STATUS_LED_ACTIVE_LOW
    on = !on;
#endif
    gpio_set_level(s_gpio[i], on);
  }
}

#endif

/* ---------------------------------------------------------------- */
/* state -> colour                                                   */
/* ---------------------------------------------------------------- */

typedef struct {
  uint8_t r, g, b;
  bool blink;
} pattern_t;

static pattern_t current_pattern(void) {
  if (s_error) return (pattern_t){255, 0, 0, true};
  if (s_rescue) return (pattern_t){0, 255, 255, true};
  if (s_pairing == 2) return (pattern_t){150, 0, 255, true};
  if (s_pairing == 1) return (pattern_t){150, 0, 255, false};
  if (s_booting) return (pattern_t){160, 160, 160, false};
  if (!s_wifi_up && !s_eth_up) {
    if (s_need_provisioning) return (pattern_t){255, 0, 255, true};
    return (pattern_t){255, 128, 0, true};
  }
  if (!s_server) return (pattern_t){0, 0, 255, false};
#if CONFIG_STATUS_LED_RGB_GPIO
  // no dimming on plain GPIOs: idle and playing both show green
  return (pattern_t){0, 255, 0, false};
#else
  if (s_server && s_playing) return (pattern_t){0, 255, 0, false};
  return (pattern_t){0, 60, 0, false};
#endif
}

static void led_task(void *arg) {
  bool phase_on = true;
  pattern_t last = {1, 2, 3, false};
  bool last_on = false;
  while (1) {
    pattern_t p = current_pattern();
    bool on = p.blink ? phase_on : true;
    if (on != last_on || memcmp(&p, &last, sizeof(p)) != 0) {
      if (on) {
        backend_write(p.r, p.g, p.b);
      } else {
        backend_write(0, 0, 0);
      }
      last = p;
      last_on = on;
    }
    // blink: wake for the next edge; steady: sleep until notified
    TickType_t wait = p.blink ? pdMS_TO_TICKS(500) : portMAX_DELAY;
    if (ulTaskNotifyTake(pdTRUE, wait) == 0) {
      phase_on = !phase_on;
    } else {
      phase_on = true;
    }
  }
}

static void kick(void) {
  if (s_task) xTaskNotifyGive(s_task);
}

/* ---------------------------------------------------------------- */
/* network events                                                    */
/* ---------------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id,
                     void *data) {
  if (base == WIFI_EVENT) {
    if (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_START) {
      wifi_config_t cfg;
      s_need_provisioning =
          esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK &&
          cfg.sta.ssid[0] == 0;
      if (id == WIFI_EVENT_STA_DISCONNECTED) s_wifi_up = false;
    }
  } else if (base == IP_EVENT) {
    // the http task owns s_server; net-down already wins in the pattern
    if (id == IP_EVENT_STA_GOT_IP) {
      s_wifi_up = true;
      s_need_provisioning = false;
    } else if (id == IP_EVENT_ETH_GOT_IP) {
      s_eth_up = true;
    } else if (id == IP_EVENT_STA_LOST_IP) {
      s_wifi_up = false;
    } else if (id == IP_EVENT_ETH_LOST_IP) {
      s_eth_up = false;
    }
  }
  s_booting = false;
  kick();
}

/* ---------------------------------------------------------------- */
/* API                                                               */
/* ---------------------------------------------------------------- */

void status_led_init(void) {
  if (s_task) return;
  if (backend_init() != ESP_OK) {
    ESP_LOGE(TAG, "LED init failed, status LED disabled");
    return;
  }
  // The default event loop is created by network_if_init(); call this after
  // it. If it does not exist yet we just miss the network colours.
  if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event,
                                 NULL) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_event,
                                 NULL) != ESP_OK) {
    ESP_LOGW(TAG, "no default event loop yet, network state not shown");
  }
  xTaskCreate(led_task, "status_led", 2560, NULL, tskIDLE_PRIORITY + 1,
              &s_task);
  ESP_LOGI(TAG, "status LED started");
}

void status_led_set_server_connected(bool connected) {
  s_server = connected;
  if (connected) s_booting = false;
  kick();
}

void status_led_set_playing(bool playing) {
  s_playing = playing;
  kick();
}

void status_led_set_error(bool error) {
  s_error = error;
  kick();
}

void status_led_set_rescue(bool rescue) {
  s_rescue = rescue;
  kick();
}

void status_led_set_pairing(uint8_t state) {
  s_pairing = state;
  kick();
}

#endif
