/*
 * WM8804(G) S/PDIF transmitter driver. See include/wm8804.h for the two
 * clocking schemes.
 *
 * References: WM8804 datasheet (Cirrus Logic, Rev 4.5), Linux
 * sound/soc/codecs/wm8804.c (register defaults, pll_factors()).
 *
 * Software control over I2C: SDIN/HWMODE high and GPO0 low at reset. The
 * S/PDIF TX source is the audio interface (TXSRC = 1); the receiver is
 * always powered down. All control registers are write-only, so whole
 * values are always written. Reads (ID) need a repeated START, which
 * i2c_master_write_read_device() provides. snapclient's other drivers use
 * the legacy I2C driver, so this one does too (mixing both aborts).
 */
#include "wm8804.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_WM8804_CLK_WM8804_MASTER
#include "driver/pulse_cnt.h"
#endif

static const char *TAG = "WM8804";

/* register map (datasheet Table 57) */
#define WM8804_RST_DEVID1 0x00  // read: 0x05, write: soft reset
#define WM8804_DEVID2 0x01      // read: 0x88
#define WM8804_DEVREV 0x02
#define WM8804_PLL1 0x03        // PLL_K[7:0]
#define WM8804_PLL2 0x04        // PLL_K[15:8]
#define WM8804_PLL3 0x05        // PLL_K[21:16]
#define WM8804_PLL4 0x06        // TXVAL 7:5, PRESCALE 4, PLL_N 3:0
#define WM8804_PLL5 0x07        // MCLKDIV 3, FRACEN 2, FREQMODE 1:0
#define WM8804_SPDTX1 0x12      // channel status byte 0
#define WM8804_SPDTX4 0x15      // TXSRC, CLKACU, FREQ
#define WM8804_SPDTX5 0x16      // word length
#define WM8804_AIFTX 0x1B
#define WM8804_AIFRX 0x1C       // SYNC_OFF 7, AIF_MS 6, WL 3:2, FMT 1:0
#define WM8804_PWRDN 0x1E       // TRIOP 5, AIF 4, OSC 3, TX 2, RX 1, PLL 0

#define PWRDN_ESP_TX_OFF 0x07   // PLL, RX, TX down; oscillator + AIF up
#define PWRDN_ESP_TX_ON 0x03    // PLL, RX down; TX, oscillator, AIF up
#define PWRDN_WM_TX_ON 0x02     // RX down; PLL, TX, oscillator, AIF up
#define PWRDN_ALL_OFF 0x07

#define AIF_MASTER_BITS 0xC0    // SYNC_OFF (clocks without S/PDIF input) +
                                // AIF_MS (master)

/* SPDTX4: TXSRC = audio interface, CLKACU = 11 (not indicated), FREQ 3:0 */
#define SPDTX4_BASE 0x70
/* SPDTX1: consumer, PCM audio, CPY_N = 1 (copying permitted), no emphasis */
#define SPDTX1_VALUE 0x04

/* the WM8804 slave MCLK input tops out at 37 MHz: 384 fs @ 96 kHz */
#define MAX_MCLK_HZ 37000000

#define I2C_TIMEOUT pdMS_TO_TICKS(50)
#define I2C_RETRIES 3

static bool s_present = false;
static bool s_wm_master = false;  // WM8804 drives the clocks right now
static SemaphoreHandle_t s_lock = NULL;

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

// Registers are write-only, so a write cannot be verified by reading it
// back; retry on NAK/timeout instead (BCLK crosstalk and RC resets make
// single transfers fail on some boards).
static esp_err_t wr(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  esp_err_t err = ESP_FAIL;
  for (int i = 0; i < I2C_RETRIES && err != ESP_OK; i++) {
    err = i2c_master_write_to_device(CONFIG_WM8804_I2C_PORT,
                                     CONFIG_WM8804_I2C_ADDR, buf, 2,
                                     I2C_TIMEOUT);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "write R%u=0x%02x failed: %s", reg, val,
             esp_err_to_name(err));
  }
  return err;
}

static esp_err_t rd(uint8_t reg, uint8_t *val) {
  esp_err_t err = ESP_FAIL;
  for (int i = 0; i < I2C_RETRIES && err != ESP_OK; i++) {
    err = i2c_master_write_read_device(CONFIG_WM8804_I2C_PORT,
                                       CONFIG_WM8804_I2C_ADDR, &reg, 1, val,
                                       1, I2C_TIMEOUT);
  }
  return err;
}

static uint64_t pin_mask(int pin) { return pin >= 0 ? (1ULL << pin) : 0; }

// SDA/SCL as released open-drain lines with pull-ups, so the WM8804 sees
// SDIN/HWMODE high (= software mode) when it leaves reset.
static void bus_lines_idle(void) {
  gpio_config_t c = {
      .pin_bit_mask =
          pin_mask(CONFIG_WM8804_I2C_SDA) | pin_mask(CONFIG_WM8804_I2C_SCL),
      .mode = GPIO_MODE_INPUT_OUTPUT_OD,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_set_level(CONFIG_WM8804_I2C_SDA, 1);
  gpio_set_level(CONFIG_WM8804_I2C_SCL, 1);
  gpio_config(&c);
}

// If the ESP32 was reset in the middle of a read, the WM8804 may still hold
// SDA low. Clock SCL until it lets go, then send a STOP.
static void bus_recover(void) {
  const int sda = CONFIG_WM8804_I2C_SDA, scl = CONFIG_WM8804_I2C_SCL;
  bus_lines_idle();
  esp_rom_delay_us(10);
  if (gpio_get_level(sda)) return;  // bus idle
  ESP_LOGW(TAG, "SDA held low, recovering the I2C bus");
  for (int i = 0; i < 9 && !gpio_get_level(sda); i++) {
    gpio_set_level(scl, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
  }
  // STOP: SDA low -> high while SCL is high
  gpio_set_level(scl, 0);
  esp_rom_delay_us(5);
  gpio_set_level(sda, 0);
  esp_rom_delay_us(5);
  gpio_set_level(scl, 1);
  esp_rom_delay_us(5);
  gpio_set_level(sda, 1);
  esp_rom_delay_us(5);
  if (!gpio_get_level(sda)) {
    ESP_LOGE(TAG, "SDA still low; power-cycle the board (or wire RESETB)");
  }
}

static void hw_reset(void) {
  // straps are sampled 9.4-25.6 us after RESETB rises: have the I2C lines
  // pulled up before that
  bus_lines_idle();
#if CONFIG_WM8804_RESET_PIN >= 0
  gpio_set_level(CONFIG_WM8804_RESET_PIN, 0);
  gpio_config_t c = {
      .pin_bit_mask = 1ULL << CONFIG_WM8804_RESET_PIN,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&c);
  vTaskDelay(pdMS_TO_TICKS(10));
  gpio_set_level(CONFIG_WM8804_RESET_PIN, 1);
#endif
  vTaskDelay(pdMS_TO_TICKS(10));  // oscillator start-up
}

/* ------------------------------------------------------------------ */
/* format helpers                                                      */
/* ------------------------------------------------------------------ */

static uint8_t freq_code(uint32_t rate) {
  // IEC 60958 consumer channel status, byte 3 bits 3:0
  switch (rate) {
    case 22050:
      return 0x4;
    case 24000:
      return 0x6;
    case 32000:
      return 0x3;
    case 44100:
      return 0x0;
    case 48000:
      return 0x2;
    case 88200:
      return 0x8;
    case 96000:
      return 0xA;
    case 176400:
      return 0xC;
    case 192000:
      return 0xE;
    default:
      return 0x1;  // not indicated
  }
}

// AIFRX/AIFTX: bits 3:2 word length (00=16, 01=20, 10=24, 11=32),
// bits 1:0 format (10 = I2S, 01 = left-justified)
static uint8_t aif_value(uint8_t bits) {
  const uint8_t wl = bits <= 16 ? 0 : bits <= 20 ? 1 : bits <= 24 ? 2 : 3;
#if CONFIG_I2S_USE_MSB_FORMAT
  const uint8_t fmt = 0x1;
#else
  const uint8_t fmt = 0x2;
#endif
  return (uint8_t)((wl << 2) | fmt);
}

/* ------------------------------------------------------------------ */
/* WM8804 as clock master: PLL + phase lock to the ESP32 clock          */
/* ------------------------------------------------------------------ */

#if CONFIG_WM8804_CLK_WM8804_MASTER

#if CONFIG_WM8804_XTAL_HZ > 14400000 && CONFIG_WM8804_XTAL_HZ < 16280000
#error "WM8804 crystal must be 10-14.4 MHz or 16.28-27 MHz (datasheet)"
#endif

typedef struct {
  uint8_t n, prescale, freqmode;
  uint32_t k;
} wm8804_pll_t;

/*
 * PLL for a 256 fs MCLK (port of pll_factors() in Linux wm8804.c):
 *   f2 = MCLK * post_div must fall in 90..100 MHz,
 *   N = f2 / fin (5..13, fin halved by PRESCALE if needed),
 *   K = round(2^22 * (f2 % fin) / fin).
 * With MCLKDIV = 0 the WM8804 master outputs BCLK = MCLK/4 = 64 fs.
 * Rates above 96 kHz would need 128 fs (MCLKDIV = 1); the ESP32 cannot be
 * an I2S slave that fast anyway, so they use the ESP32-master fallback.
 */
static esp_err_t pll_factors(uint32_t target, uint32_t fin, wm8804_pll_t *p) {
  static const struct {
    uint8_t div, freqmode;
  } post[] = {{2, 0}, {4, 1}, {8, 2}, {12, 3}};  // MCLKDIV = 0 rows
  uint64_t f2 = 0;
  for (size_t i = 0; i < sizeof(post) / sizeof(post[0]); i++) {
    uint64_t t = (uint64_t)target * post[i].div;
    if (t >= 90000000ULL && t <= 100000000ULL) {
      f2 = t;
      p->freqmode = post[i].freqmode;
      break;
    }
  }
  if (f2 == 0) return ESP_ERR_INVALID_ARG;
  p->prescale = 0;
  uint64_t n = f2 / fin;
  if (n < 5) {
    fin /= 2;
    p->prescale = 1;
    n = f2 / fin;
  }
  if (n < 5 || n > 13) return ESP_ERR_INVALID_ARG;
  p->n = (uint8_t)n;
  // one extra decimal of precision, rounded like Linux
  uint64_t kpart = ((uint64_t)(1u << 22) * 10ULL * (f2 % fin)) / fin;
  uint32_t k = (uint32_t)kpart;
  if ((k % 10) >= 5) k += 5;
  p->k = k / 10;
  return ESP_OK;
}

static bool rate_supported(uint32_t rate) {
  wm8804_pll_t p;
  return rate >= 32000 && rate <= 96000 &&
         pll_factors(256 * rate, CONFIG_WM8804_XTAL_HZ, &p) == ESP_OK;
}

/* phase-lock state (register access under s_lock) */
static wm8804_pll_t s_pll;        // untrimmed factors for the current rate
static uint32_t s_k_written;      // K currently in the chip
static double s_trim_ppm;         // applied frequency trim
static double s_learned_ppm;      // crystal error learned so far (kept
                                  // across rate changes: same crystal)
static bool s_master_ok = true;   // boot self-test saw our LRCLK
static volatile uint32_t s_rate;  // nominal fs, 0 = not tracking
static volatile bool s_track_reset;
static pcnt_unit_handle_t s_pcnt = NULL;
static portMUX_TYPE s_meas_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t write_k(uint32_t k) {
  // write only the bytes that change: crossing a byte boundary costs a few
  // microseconds of a slightly wrong frequency, inaudible
  esp_err_t err = ESP_OK;
  if ((k ^ s_k_written) & 0x3F0000) err |= wr(WM8804_PLL3, (k >> 16) & 0x3F);
  if ((k ^ s_k_written) & 0x00FF00) err |= wr(WM8804_PLL2, (k >> 8) & 0xFF);
  if ((k ^ s_k_written) & 0x0000FF) err |= wr(WM8804_PLL1, k & 0xFF);
  // after a failed write the chip state is unknown: rewrite all bytes next
  s_k_written = err == ESP_OK ? k : 0xFFFFFFFFu;
  return err;
}

// Set the PLL output `ppm` away from its untrimmed frequency.
static void apply_trim(double ppm) {
  if (ppm > 500) ppm = 500;
  if (ppm < -500) ppm = -500;
  // f ~ N + K/2^22  =>  dK = ppm * 1e-6 * (N * 2^22 + K)
  double base = (double)s_pll.n * 4194304.0 + (double)s_pll.k;
  double k = (double)s_pll.k + ppm * 1e-6 * base;
  if (k < 0 || k > 4194303.0) return;  // would need another N: give up
  if (write_k((uint32_t)lround(k)) == ESP_OK) s_trim_ppm = ppm;
}

static esp_err_t set_pll(uint32_t rate) {
  if (pll_factors(256 * rate, CONFIG_WM8804_XTAL_HZ, &s_pll) != ESP_OK) {
    return ESP_ERR_NOT_SUPPORTED;
  }
  esp_err_t err = wr(WM8804_PLL4, (uint8_t)(s_pll.n | (s_pll.prescale << 4)));
  // bit4 reserved (default 1), FRACEN = 1, MCLKDIV = 0 (256 fs)
  err |= wr(WM8804_PLL5, (uint8_t)(0x14 | s_pll.freqmode));
  err |= wr(WM8804_PLL1, s_pll.k & 0xFF);
  err |= wr(WM8804_PLL2, (s_pll.k >> 8) & 0xFF);
  err |= wr(WM8804_PLL3, (s_pll.k >> 16) & 0x3F);
  s_k_written = s_pll.k;
  s_trim_ppm = 0;
  // start from what the crystal is known to need, not from zero
  if (err == ESP_OK && s_learned_ppm != 0) apply_trim(s_learned_ppm);
  ESP_LOGD(TAG, "PLL %lu Hz: N=%u K=0x%06lx prescale=%u freqmode=%u",
           (unsigned long)(256 * rate), s_pll.n, (unsigned long)s_pll.k,
           s_pll.prescale, s_pll.freqmode);
  return err;
}

/*
 * Phase lock: count LRCLK edges with PCNT against esp_timer, which runs
 * from the ESP32 crystal like the I2S clock does in ESP32-master mode. The
 * PLL trim is steered so that the edge count stays equal to
 * elapsed_time * fs. snapclient's sync (which assumes the nominal rate on
 * its own time base) then sees no drift, exactly as in ESP32-master mode.
 */
static void track_task(void *arg) {
  const int window_ms = 5000;
  int64_t t0 = 0, tprev = 0;
  int cprev = 0;
  uint64_t total = 0;
  bool have = false;
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(window_ms));
    const uint32_t rate = s_rate;
    if (!rate) {
      have = false;
      continue;
    }
    int c = 0;
    int64_t t;
    taskENTER_CRITICAL(&s_meas_lock);
    pcnt_unit_get_count(s_pcnt, &c);
    t = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_meas_lock);
    if (s_track_reset || !have) {
      s_track_reset = false;
      have = true;
      t0 = tprev = t;
      cprev = c;
      total = 0;
      continue;
    }
    const uint32_t dc = (uint32_t)c - (uint32_t)cprev;  // wraps fine
    const int64_t dt = t - tprev;
    cprev = c;
    tprev = t;
    if (dc == 0 || dt <= 0) {  // clocks stopped
      have = false;
      continue;
    }
    total += dc;
    const double rate_ppm =
        ((double)dc * 1e6 / (double)dt / rate - 1.0) * 1e6;
    const double expected = (double)(t - t0) * rate / 1e6;
    const double phase_s = ((double)total - expected) / rate;
    if (fabs(rate_ppm) > 2000 || fabs(phase_s) > 0.05) {
      ESP_LOGW(TAG, "clock jump (%.0f ppm, %.1f ms), restarting lock",
               rate_ppm, phase_s * 1e3);
      have = false;
      continue;
    }
    // aim for a rate that removes the phase error in ~60 s
    double want_ppm = -phase_s / 60.0 * 1e6;
    if (want_ppm > 30) want_ppm = 30;
    if (want_ppm < -30) want_ppm = -30;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rate == rate && s_wm_master) {
      apply_trim(s_trim_ppm + 0.6 * (want_ppm - rate_ppm));
      s_learned_ppm = s_trim_ppm;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGD(TAG, "lock: rate %+.1f ppm, phase %+.0f us, trim %+.1f ppm",
             rate_ppm, phase_s * 1e6, s_trim_ppm);
  }
}

static esp_err_t tracking_init(void) {
  if (CONFIG_MASTER_I2S_LRCK_PIN < 0) return ESP_ERR_INVALID_ARG;
  pcnt_unit_config_t ucfg = {
      .low_limit = -1,
      .high_limit = 30000,
      .flags.accum_count = 1,
  };
  esp_err_t err = pcnt_new_unit(&ucfg, &s_pcnt);
  if (err != ESP_OK) return err;
  err = pcnt_unit_add_watch_point(s_pcnt, 30000);
  // the LRCLK pin is an I2S input in slave mode; PCNT listens to the same
  // pad through the GPIO matrix
  pcnt_chan_config_t ccfg = {.edge_gpio_num = CONFIG_MASTER_I2S_LRCK_PIN,
                             .level_gpio_num = -1};
  pcnt_channel_handle_t ch = NULL;
  if (err == ESP_OK) err = pcnt_new_channel(s_pcnt, &ccfg, &ch);
  if (err == ESP_OK) {
    pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_channel_set_level_action(ch, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP);
    // ignore crosstalk spikes; LRCLK half periods are >= 5 us
    const pcnt_glitch_filter_config_t gf = {.max_glitch_ns = 200};
    pcnt_unit_set_glitch_filter(s_pcnt, &gf);
    err = pcnt_unit_enable(s_pcnt);
  }
  if (err == ESP_OK) err = pcnt_unit_start(s_pcnt);
  if (err == ESP_OK &&
      xTaskCreate(track_task, "wm8804_lock", 3072, NULL,
                  tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
    err = ESP_ERR_NO_MEM;
  }
  return err;
}

// Boot self-test: let the WM8804 drive the clocks for a moment (the ESP32
// I2S is not set up yet, its pads are inputs) and check LRCLK edges reach
// CONFIG_MASTER_I2S_LRCK_PIN. If not, never make the ESP32 an I2S slave:
// it would wait for a clock forever.
static bool clock_self_test(void) {
  if (set_pll(48000) != ESP_OK) return false;
  esp_err_t err = wr(WM8804_AIFTX, aif_value(16));
  err |= wr(WM8804_AIFRX, AIF_MASTER_BITS | aif_value(16));
  err |= wr(WM8804_PWRDN, 0x06);  // PLL, OSC, AIF up; TX and RX down
  vTaskDelay(pdMS_TO_TICKS(30));                    // PLL lock
  int c0 = 0, c1 = 0;
  pcnt_unit_get_count(s_pcnt, &c0);
  const int64_t t0 = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(50));
  pcnt_unit_get_count(s_pcnt, &c1);
  const int64_t t1 = esp_timer_get_time();
  // back to a quiet slave
  err |= wr(WM8804_PWRDN, PWRDN_ALL_OFF);
  err |= wr(WM8804_AIFRX, aif_value(16));
  const double hz = (double)((uint32_t)c1 - (uint32_t)c0) * 1e6 / (t1 - t0);
  const bool ok = err == ESP_OK && hz > 46000 && hz < 50000;
  ESP_LOGI(TAG, "clock self-test: %.0f Hz on GPIO%d (want 48000) -> %s", hz,
           CONFIG_MASTER_I2S_LRCK_PIN, ok ? "ok" : "FAILED");
  return ok;
}

#endif  // CONFIG_WM8804_CLK_WM8804_MASTER

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

bool wm8804_present(void) { return s_present; }

esp_err_t wm8804_init(void) {
  if (s_present) return ESP_OK;
  if (CONFIG_WM8804_I2C_SDA < 0 || CONFIG_WM8804_I2C_SCL < 0) {
    ESP_LOGE(TAG, "WM8804 I2C pins not configured");
    return ESP_ERR_INVALID_ARG;
  }
#if CONFIG_WM8804_CLK_ESP32_MASTER
  if (CONFIG_MASTER_I2S_MCLK_PIN < 0) {
    ESP_LOGE(TAG,
             "no MCLK pin (\"Master i2s mclk\"): the S/PDIF transmitter would "
             "have no clock. Configure the GPIO wired to WM8804 pin 16, or "
             "make the WM8804 the I2S clock master. Optical output disabled.");
    return ESP_ERR_INVALID_ARG;
  }
#endif
  if (!s_lock) s_lock = xSemaphoreCreateMutex();

  hw_reset();
  bus_recover();

  i2c_config_t cfg = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = CONFIG_WM8804_I2C_SDA,
      .scl_io_num = CONFIG_WM8804_I2C_SCL,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 100000,  // chip allows up to 400 kHz
  };
  esp_err_t err = i2c_param_config(CONFIG_WM8804_I2C_PORT, &cfg);
  if (err == ESP_OK) {
    err = i2c_driver_install(CONFIG_WM8804_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG,
             "I2C init failed: %s (controller %d must not be used by another "
             "driver)",
             esp_err_to_name(err), CONFIG_WM8804_I2C_PORT);
    return err;
  }

  uint8_t id1 = 0, id2 = 0, rev = 0;
  // with an RC reset the chip may still be coming up: try for ~100 ms
  for (int attempt = 0; attempt < 10; attempt++) {
    err = rd(WM8804_RST_DEVID1, &id1);
    if (err == ESP_OK) err = rd(WM8804_DEVID2, &id2);
    if (err == ESP_OK) err = rd(WM8804_DEVREV, &rev);
    if (err == ESP_OK && id1 == 0x05 && id2 == 0x88) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (err != ESP_OK || id1 != 0x05 || id2 != 0x88) {
    ESP_LOGE(TAG,
             "no WM8804 at 0x%02x on SDA=%d SCL=%d (err %s, id %02x%02x)",
             CONFIG_WM8804_I2C_ADDR, CONFIG_WM8804_I2C_SDA,
             CONFIG_WM8804_I2C_SCL, esp_err_to_name(err), id2, id1);
    i2c_driver_delete(CONFIG_WM8804_I2C_PORT);
    return ESP_ERR_NOT_FOUND;
  }
  ESP_LOGI(TAG, "found WM8804 rev %c at 0x%02x", 'A' + (rev & 0x0F),
           CONFIG_WM8804_I2C_ADDR);

  // soft reset = power-on defaults, i.e. a quiet audio-interface slave
  for (int attempt = 0; attempt < 2; attempt++) {
    err = wr(WM8804_RST_DEVID1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(2));
    err |= wr(WM8804_PWRDN, PWRDN_ALL_OFF);
    err |= wr(WM8804_AIFRX, aif_value(16));  // slave
    err |= wr(WM8804_AIFTX, aif_value(16));
    err |= wr(WM8804_SPDTX1, SPDTX1_VALUE);
    err |= wr(WM8804_SPDTX4, SPDTX4_BASE | freq_code(44100));
    if (err == ESP_OK) break;
  }
#if CONFIG_WM8804_CLK_WM8804_MASTER
  if (err == ESP_OK && !s_pcnt) {
    esp_err_t terr = tracking_init();
    if (terr != ESP_OK) {
      ESP_LOGE(TAG, "LRCLK counter failed (%s): clock-master mode off",
               esp_err_to_name(terr));
      err = terr;
    }
  }
  if (err == ESP_OK && !clock_self_test()) {
    s_master_ok = false;
    ESP_LOGE(TAG,
             "the WM8804's LRCLK does not reach GPIO%d: keeping the ESP32 as "
             "I2S master (AUX plays, optical off). Check the LRCK pin.",
             CONFIG_MASTER_I2S_LRCK_PIN);
  }
#endif
  if (err != ESP_OK) {
    wr(WM8804_RST_DEVID1, 0x00);  // leave it as a quiet slave
    i2c_driver_delete(CONFIG_WM8804_I2C_PORT);
    ESP_LOGE(TAG, "WM8804 configuration failed; S/PDIF output disabled");
    return ESP_FAIL;
  }
  s_present = true;
  s_wm_master = false;
  return ESP_OK;
}

bool wm8804_prepare(uint32_t sample_rate, uint8_t bits) {
  if (!s_present) return false;
#if CONFIG_WM8804_CLK_WM8804_MASTER
  if (s_master_ok && rate_supported(sample_rate)) return true;  // clocks
                                                  // start in set_format
  ESP_LOGW(TAG,
           "%lu Hz cannot be clocked by the WM8804 PLL: the ESP32 becomes I2S "
           "master, AUX only, optical off",
           (unsigned long)sample_rate);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_rate = 0;
  // stop driving BCLK/LRCLK/MCLK before the ESP32 starts driving them
  wr(WM8804_PWRDN, PWRDN_ALL_OFF);
  wr(WM8804_AIFRX, aif_value(bits));
  s_wm_master = false;
  xSemaphoreGive(s_lock);
#else
  (void)sample_rate;
  (void)bits;
#endif
  return false;
}

esp_err_t wm8804_set_format(uint32_t sample_rate, uint8_t bits) {
  if (!s_present) return ESP_ERR_INVALID_STATE;
  esp_err_t err = ESP_OK;
  const uint8_t aif = aif_value(bits);
  xSemaphoreTake(s_lock, portMAX_DELAY);
#if CONFIG_WM8804_CLK_WM8804_MASTER
  if (!s_master_ok || !rate_supported(sample_rate)) {
    // the ESP32 is master (see wm8804_prepare) and no MCLK reaches us
    xSemaphoreGive(s_lock);
    return ESP_ERR_NOT_SUPPORTED;
  }
  // PLL down while reprogramming, then clocks + transmitter up
  s_rate = 0;
  err |= wr(WM8804_PWRDN, PWRDN_ALL_OFF);
  err |= set_pll(sample_rate);
  err |= wr(WM8804_AIFTX, aif);
  err |= wr(WM8804_SPDTX5, bits <= 16 ? 0x02 : 0x0B);
  err |= wr(WM8804_SPDTX4, SPDTX4_BASE | freq_code(sample_rate));
  err |= wr(WM8804_AIFRX, AIF_MASTER_BITS | aif);  // ESP32 pads are inputs
  err |= wr(WM8804_PWRDN, PWRDN_WM_TX_ON);
  s_wm_master = true;
  vTaskDelay(pdMS_TO_TICKS(5));  // PLL lock
  s_track_reset = true;
  s_rate = sample_rate;
#else
  // MCLK = 256 fs (384 fs above 16 bit, see player.c) must stay <= 37 MHz
  const uint32_t mclk = sample_rate * (bits > 16 ? 384 : 256);
  err |= wr(WM8804_PWRDN, PWRDN_ESP_TX_OFF);
  if (mclk > MAX_MCLK_HZ) {
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG,
             "%lu Hz/%u bit: MCLK %lu Hz is too fast for the WM8804, optical "
             "off",
             (unsigned long)sample_rate, bits, (unsigned long)mclk);
    return ESP_ERR_NOT_SUPPORTED;
  }
  err |= wr(WM8804_AIFRX, aif);
  err |= wr(WM8804_AIFTX, aif);
  err |= wr(WM8804_SPDTX5, bits <= 16 ? 0x02 : 0x0B);
  err |= wr(WM8804_SPDTX4, SPDTX4_BASE | freq_code(sample_rate));
  err |= wr(WM8804_PWRDN, PWRDN_ESP_TX_ON);
#endif
  xSemaphoreGive(s_lock);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C error while setting %lu Hz / %u bit",
             (unsigned long)sample_rate, bits);
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "S/PDIF out: %lu Hz, %u bit%s", (unsigned long)sample_rate,
           bits, s_wm_master ? " (WM8804 is clock master)" : "");
  return ESP_OK;
}

esp_err_t wm8804_stop(void) {
  if (!s_present) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_lock, portMAX_DELAY);
#if CONFIG_WM8804_CLK_WM8804_MASTER
  s_rate = 0;
#endif
  // transmitter down; in master mode the PLL too, so the clocks stop and
  // the DAC can go to standby between streams
  esp_err_t err = wr(WM8804_PWRDN, PWRDN_ALL_OFF);
  xSemaphoreGive(s_lock);
  return err;
}

esp_err_t wm8804_deinit(void) {
  if (!s_present) return ESP_OK;
  wr(WM8804_PWRDN, 0x3F);  // everything down, outputs tri-stated
  wr(WM8804_RST_DEVID1, 0x00);
  i2c_driver_delete(CONFIG_WM8804_I2C_PORT);
  s_present = false;
  s_wm_master = false;
  return ESP_OK;
}
