/*
 * snapclient rescue mode: a way back in on boards with no serial port.
 *
 * Entered (instead of starting snapclient) when
 *   - the rescue button is held while the board powers up, or
 *   - the previous run asked for it: button held 5 s, or no IP address
 *     within CONFIG_RESCUE_NO_IP_TIMEOUT_S.
 * Then: access point "snapclient-XXXX" (password CONFIG_RESCUE_AP_PASSWORD),
 * setup page at http://192.168.4.1 (WiFi credentials, firmware upload,
 * firmware slot switch). It also joins the stored WiFi, so the page is
 * reachable on the home network too. After CONFIG_RESCUE_IDLE_REBOOT_S
 * without web requests it reboots into normal operation.
 */
#include "esp_system.h"
#include "sdkconfig.h"
#include "web_rescue.h"

#if CONFIG_RESCUE_MODE_ENABLE

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// web_rescue falls back to an OPEN access point for passwords under 8
// characters (WPA2 minimum). Never build that: the AP serves firmware
// install and a full-flash backup with the stored WiFi passwords.
_Static_assert(sizeof(CONFIG_RESCUE_AP_PASSWORD) - 1 >= 8 &&
                   sizeof(CONFIG_RESCUE_AP_PASSWORD) - 1 <= 63,
               "CONFIG_RESCUE_AP_PASSWORD must be 8-63 characters");

static const char *TAG = "rescue";


#define RESCUE_MAGIC 0x5245534Bu  // "RESK"
#define HOLD_TO_RESCUE_MS 5000
#define BUTTON_SAMPLE_MS 50
#define SHORT_PRESS_MIN_MS 100   // two samples: not a glitch
#define SHORT_PRESS_MAX_MS 2000  // a longer press is on its way to rescue

#define CRASH_MAGIC 0x43524153u  // "CRAS"
#define CRASHES_TO_RESCUE 3         // consecutive abnormal resets
#define STABLE_AFTER_IP_S 60        // then the firmware counts as good

// RTC memory survives esp_restart(), panics and watchdog resets, not a
// power cycle
static RTC_NOINIT_ATTR uint32_t s_rescue_request;
static RTC_NOINIT_ATTR uint32_t s_crash_magic;
static RTC_NOINIT_ATTR uint32_t s_crash_count;

static esp_timer_handle_t s_no_ip_timer = NULL;
static esp_timer_handle_t s_stable_timer = NULL;

void rescue_mode_request(const char *why) {
  // flag first: this can run on small stacks (esp_timer task), keep the
  // work before the restart minimal
  s_rescue_request = RESCUE_MAGIC;
  ESP_EARLY_LOGW(TAG, "%s: rebooting into rescue mode", why);
  web_rescue_note(true, true, why);
  esp_restart();
}

static bool abnormal_reset(esp_reset_reason_t rr) {
  return rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
         rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
         rr == ESP_RST_BROWNOUT;
}

// A typo must not take a flash/PSRAM pin (crash on every boot, no serial
// port to recover) or read a non-existent pin as "pressed".
static bool button_pin_ok(void) {
  const int pin = CONFIG_RESCUE_BUTTON_GPIO;
  if (pin < 0 || !GPIO_IS_VALID_GPIO(pin)) return false;
  if (pin >= 26 && pin <= 32) return false;
#if CONFIG_SPIRAM_MODE_OCT
  if (pin >= 33 && pin <= 37) return false;
#endif
  return true;
}

static bool button_pressed(void) {
#if CONFIG_RESCUE_BUTTON_GPIO >= 0
  if (!button_pin_ok()) return false;
  return gpio_get_level(CONFIG_RESCUE_BUTTON_GPIO) ==
         (CONFIG_RESCUE_BUTTON_ACTIVE_LOW ? 0 : 1);
#else
  return false;
#endif
}

static void button_init(void) {
#if CONFIG_RESCUE_BUTTON_GPIO >= 0
  if (!button_pin_ok()) {
    ESP_LOGE(TAG, "GPIO%d can't be the rescue button; button disabled",
             CONFIG_RESCUE_BUTTON_GPIO);
    return;
  }
  gpio_config_t c = {
      .pin_bit_mask = 1ULL << CONFIG_RESCUE_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = CONFIG_RESCUE_BUTTON_ACTIVE_LOW ? GPIO_PULLUP_ENABLE
                                                    : GPIO_PULLUP_DISABLE,
      .pull_down_en = CONFIG_RESCUE_BUTTON_ACTIVE_LOW ? GPIO_PULLDOWN_DISABLE
                                                      : GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&c);
  vTaskDelay(pdMS_TO_TICKS(5));
#endif
}

bool rescue_mode_requested(void) {
  const esp_reset_reason_t rr = esp_reset_reason();
  // A firmware that crashes before it has run stably (60 s with an IP)
  // would never reach the other triggers: after three such resets in a
  // row, open the setup AP instead (it runs none of the audio code).
  if (s_crash_magic != CRASH_MAGIC || rr == ESP_RST_POWERON) {
    s_crash_magic = CRASH_MAGIC;
    s_crash_count = 0;
  }
  if (abnormal_reset(rr)) {
    s_crash_count++;
    ESP_LOGW(TAG, "abnormal reset (%d), %lu in a row", rr,
             (unsigned long)s_crash_count);
    if (s_crash_count >= CRASHES_TO_RESCUE) {
      s_crash_count = 0;
      s_rescue_request = 0;
      ESP_LOGW(TAG, "crash loop: starting rescue mode");
      web_rescue_note(false, true, "Three crashes in a row: opening rescue mode");
      return true;
    }
  }
  if (s_rescue_request == RESCUE_MAGIC) {
    s_rescue_request = 0;  // one rescue session per request
    if (esp_reset_reason() != ESP_RST_POWERON) {
      ESP_LOGW(TAG, "rescue mode requested by the previous run");
      return true;
    }
  }
  s_rescue_request = 0;
  button_init();
  if (button_pressed()) {
    // debounce: still pressed 300 ms later
    vTaskDelay(pdMS_TO_TICKS(300));
    if (button_pressed()) {
      ESP_LOGW(TAG, "rescue button held at boot");
      web_rescue_note(false, false,
                      "Rescue button held at start-up: opening rescue mode");
      return true;
    }
  }
  return false;
}

void rescue_mode_run(bool nvs_ok) {
  web_rescue_unlock_physical_flash();
  const web_rescue_cfg_t cfg = {
      .title = "snapclient rescue / setup",
      .ap_ssid_prefix = "snapclient",
      .ap_password = CONFIG_RESCUE_AP_PASSWORD,
      .nvs_ok = nvs_ok,
      .serve_root = true,
  };
  if (web_rescue_start(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "could not start the access point, rebooting in 30 s");
    vTaskDelay(pdMS_TO_TICKS(30000));
    web_rescue_note(true, true,
                    "Rescue mode could not open its access point; restarting");
    esp_restart();
  }
  web_rescue_note(false, false, "Rescue mode: setup access point open");
  const int64_t start = esp_timer_get_time();
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
#if CONFIG_RESCUE_IDLE_REBOOT_S > 0
    if (web_rescue_update_in_progress()) continue;
    int64_t last = web_rescue_last_request_us();
    if (last < start) last = start;
    if (esp_timer_get_time() - last >
        (int64_t)CONFIG_RESCUE_IDLE_REBOOT_S * 1000000LL) {
      ESP_LOGI(TAG, "idle, rebooting into normal mode");
      web_rescue_note(true, false, "Rescue mode idle; restarting normally");
      esp_restart();
    }
#endif
  }
}

/* ---- normal operation ---------------------------------------------- */

static void no_ip_cb(void *arg) {
  rescue_mode_request("Rescue mode: no IP address in time");
}

static void stable_cb(void *arg) {
  // ran with a network for a while: this firmware is good
  s_crash_count = 0;
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  ESP_LOGI(TAG, "firmware confirmed (%s)", esp_err_to_name(err));
}

static void on_net(void *arg, esp_event_base_t base, int32_t id, void *data) {
  const bool up = (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP ||
                                        id == IP_EVENT_ETH_GOT_IP));
  if (up) {
    if (s_no_ip_timer) esp_timer_stop(s_no_ip_timer);
    if (s_stable_timer && !esp_timer_is_active(s_stable_timer)) {
      esp_timer_start_once(s_stable_timer, STABLE_AFTER_IP_S * 1000000ULL);
    }
  } else if (s_no_ip_timer && !esp_timer_is_active(s_no_ip_timer)) {
    // lost the network: if it does not come back, open the setup AP (also
    // catches the WiFi driver giving up after its retry limit)
    esp_timer_start_once(s_no_ip_timer,
                         (uint64_t)CONFIG_RESCUE_NO_IP_TIMEOUT_S * 1000000ULL);
  }
}

#if CONFIG_RESCUE_BUTTON_GPIO >= 0
static void button_task(void *arg) {
  int held_ms = 0;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(BUTTON_SAMPLE_MS));
    if (button_pressed()) {
      held_ms += BUTTON_SAMPLE_MS;
      if (held_ms >= HOLD_TO_RESCUE_MS) rescue_mode_request("Rescue mode: button held");
      continue;
    }
    // released: a short press is for the application (Sendspin pairing)
    if (held_ms >= SHORT_PRESS_MIN_MS && held_ms <= SHORT_PRESS_MAX_MS) {
      web_rescue_button_pressed();
    }
    held_ms = 0;
  }
}
#endif

static void no_credentials_task(void *arg) {
  // give the WiFi driver a moment to load its stored config
  vTaskDelay(pdMS_TO_TICKS(3000));
  wifi_config_t cfg;
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] == 0) {
    rescue_mode_request("Rescue mode: no WiFi credentials stored");
  }
  vTaskDelete(NULL);
}

void rescue_mode_arm(void) {
#if CONFIG_ENABLE_WIFI_PROVISIONING
  // credentials come from NVS: without any, go straight to the setup AP
  // instead of waiting for the no-IP timeout
  xTaskCreate(no_credentials_task, "rescue_cred", 3072, NULL,
              tskIDLE_PRIORITY + 1, NULL);
#endif
#if CONFIG_RESCUE_BUTTON_GPIO >= 0
  button_init();
  xTaskCreate(button_task, "rescue_btn", 3072, NULL, tskIDLE_PRIORITY + 1,
              NULL);
#endif
  const esp_timer_create_args_t sa = {.callback = stable_cb, .name = "stable"};
  esp_timer_create(&sa, &s_stable_timer);
#if CONFIG_RESCUE_NO_IP_TIMEOUT_S > 0
  const esp_timer_create_args_t a = {.callback = no_ip_cb, .name = "noip"};
  esp_timer_create(&a, &s_no_ip_timer);
#endif
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_net, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_net, NULL);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net,
                             NULL);
#if CONFIG_RESCUE_NO_IP_TIMEOUT_S > 0
  if (s_no_ip_timer) {
    esp_timer_start_once(s_no_ip_timer,
                         (uint64_t)CONFIG_RESCUE_NO_IP_TIMEOUT_S * 1000000ULL);
  }
#endif
}

#else  // !CONFIG_RESCUE_MODE_ENABLE

bool rescue_mode_requested(void) { return false; }
void rescue_mode_run(bool nvs_ok) {
  (void)nvs_ok;
  web_rescue_note(true, true, "Rescue mode is not built in; restarting");
  esp_restart();
}
void rescue_mode_request(const char *why) {
  web_rescue_note(true, true, why);
  esp_restart();
}
void rescue_mode_arm(void) {}

#endif
