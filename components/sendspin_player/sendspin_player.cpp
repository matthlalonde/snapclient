// Sendspin player for this firmware.
//
// The official sendspin-cpp library does the protocol work: the
// Noise-encrypted WebSocket, clock sync, FLAC/PCM decoding and timed
// playback. This file connects it to the board:
//   - the I2S output, which snapcast hands over while a Sendspin stream plays
//     (player_release_output) and gets back 2 s after the stream ends
//   - NVS for the device identity (X25519 key) and pairing records
//   - mDNS (_sendspin._tcp) so Music Assistant finds the player
//   - pairing: a static code, a pairing window opened by the device button or
//     General Settings, and the "require pairing" setting
//
// Sync rests on exact playback reports. Every DMA buffer is written whole as
// [real audio][silence padding], and a lockstep queue records how many real
// frames each one carries. The I2S on_sent interrupt then tells exactly when
// real audio finished playing, with startup zeros and padding left out. This
// is the method ESPHome's I2S speaker uses. The I2S interrupt is IRAM-safe
// (CONFIG_I2S_ISR_IRAM_SAFE), so flash writes don't stall it.

#include "sendspin_player.h"

#include "sdkconfig.h"

#if CONFIG_SENDSPIN_PLAYER_ENABLE

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <vector>

#include "driver/i2s_std.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "player.h"
#include "sendspin/client.h"
#include "sendspin/persistence_codec.h"
#include "sendspin/persistence_keys.h"
#include "sendspin/player_role.h"
#if CONFIG_SENDSPIN_ENABLE_METADATA
#include "sendspin/metadata_role.h"
#endif

extern "C" {
#include "event_log.h"
#include "network_interface.h"
#include "status_led.h"
// main.c: DAC enable / mute through the board HAL; also drives the status LED
void audio_set_mute(bool mute);
void audio_dac_enable(bool enabled);
}

namespace {

const char *TAG = "sendspin";

// The one format we advertise. Snapclient's I2S setup (16-bit Philips slots,
// 32 fs BCK for the PCM5102A's PLL, 256 fs MCLK for the WM8804) is verified
// on this board at 16 bit; with a single rate the server resamples instead of
// changing the format mid-stream.
constexpr uint32_t kSampleRate = 44100;
constexpr uint8_t kChannels = 2;
constexpr uint8_t kBits = 16;
constexpr size_t kFrameBytes = kChannels * kBits / 8;

// 6 x 512 frames = 70 ms of DMA at 44.1 kHz, 12 KB of internal RAM
constexpr uint32_t kDmaDescNum = 6;
constexpr uint32_t kDmaFrameNum = 512;
constexpr size_t kDmaBufBytes = kDmaFrameNum * kFrameBytes;
constexpr int64_t kDmaBufUs = int64_t(kDmaFrameNum) * 1000000 / kSampleRate;
// sync task -> output task, 46 ms. Small on purpose: everything queued here
// delays the start of a stream and still plays after a seek. The library
// handles partial writes, so chunks larger than this are fine.
constexpr size_t kPcmBufBytes = 8 * 1024;
// largest single send: the wait for space then ends after a few DMA buffers
constexpr size_t kMaxSendBytes = kPcmBufBytes / 2;
// how long the output task waits for more audio before padding a DMA buffer
// with silence; well below one buffer (11.6 ms) so the DMA never runs dry
constexpr TickType_t kFillWaitTicks = pdMS_TO_TICKS(3);

// Startup lead we ask the server for: handover from snapcast (up to ~100 ms
// while snapcast plays), 70 ms of preloaded DMA silence, the 46 ms PCM
// buffer, the library's 25 ms priming and 50 ms extra silence, plus margin.
constexpr uint16_t kRequiredLeadMs = 350;

constexpr uint32_t kHandoverTimeoutMs = 500;  // later attempts come from poll()
constexpr int64_t kRetryUs = 500 * 1000;
constexpr int64_t kReleaseDelayUs = 2 * 1000 * 1000;
constexpr uint32_t kLoopPeriodMs = 20;

constexpr const char *kAppNamespace = "ss_app";  // our own keys
constexpr const char *kLibNamespace = "sendspin";  // the library's keyspace

// Status tab snapshot: written by the loop task, copied by the web server
constexpr int64_t kStatusPeriodUs = 250 * 1000;
sendspin_status_t g_status = {};
portMUX_TYPE g_status_mux = portMUX_INITIALIZER_UNLOCKED;
// what is playing: written by the loop task (metadata listener, progress)
sendspin_now_playing_t g_np = {};
portMUX_TYPE g_np_mux = portMUX_INITIALIZER_UNLOCKED;
// pairing state for General Settings: written by the loop task
sendspin_pairing_t g_pair = {};
portMUX_TYPE g_pair_mux = portMUX_INITIALIZER_UNLOCKED;
// commands from other tasks (web server, button) for the loop task
QueueHandle_t g_cmds = nullptr;
std::atomic<bool> g_started{false};

// "Let servers play without pairing" from the build; General Settings can
// change it (saved in NVS only while it differs from this)
#if CONFIG_SENDSPIN_UNPAIRED_ACCESS
constexpr bool kUnpairedDefault = true;
#else
constexpr bool kUnpairedDefault = false;
#endif
constexpr const char *kUnpairedKey = "unpaired";
constexpr const char *kCodeKey = "pair_code";
constexpr const char *kNamesKey = "srv_names";
// the library's pairing window: 5 minutes, closed by five wrong codes
constexpr int64_t kWindowUs = 300LL * 1000 * 1000;
constexpr uint8_t kWindowFailLimit = 5;
// the library ends an attempt after 120 s; a little more, in case a server
// drops one without either callback
constexpr int64_t kAttemptUs = 125LL * 1000 * 1000;
// a server may also drop its pairing (server/unpair): re-read now and then
constexpr int64_t kRecordsRefreshUs = 60LL * 1000 * 1000;
constexpr size_t kMaxRecords =
    sendspin::SendspinClientConfig::DEFAULT_MAX_PAIRING_RECORDS;
static_assert(kMaxRecords == SENDSPIN_MAX_PAIRED,
              "General Settings lists every pairing record");

void copy_str(char *dst, size_t size, const std::string &src) {
  size_t n = std::min(src.size(), size - 1);
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

// at most size-1 bytes, never splitting a UTF-8 character
void copy_utf8(char *dst, size_t size, const std::string &src) {
  size_t n = std::min(src.size(), size - 1);
  if (n < src.size()) {
    // src[n] is the first byte left out: if it continues a character, that
    // character started earlier; cut before it
    while (n > 0 && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80) {
      n--;
    }
  }
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}

const char *codec_name(sendspin::SendspinCodecFormat codec) {
  switch (codec) {
    case sendspin::SendspinCodecFormat::FLAC:
      return "flac";
    case sendspin::SendspinCodecFormat::OPUS:
      return "opus";
    case sendspin::SendspinCodecFormat::PCM:
      return "pcm";
    default:
      return "unsupported";
  }
}

// Our own settings, namespace "ss_app".
std::optional<uint8_t> app_load_u8(const char *key) {
  nvs_handle_t h;
  uint8_t v = 0;
  if (nvs_open(kAppNamespace, NVS_READONLY, &h) != ESP_OK) {
    return std::nullopt;
  }
  esp_err_t err = nvs_get_u8(h, key, &v);
  nvs_close(h);
  return err == ESP_OK ? std::optional<uint8_t>(v) : std::nullopt;
}

void app_save_u8(const char *key, uint8_t v) {
  nvs_handle_t h;
  if (nvs_open(kAppNamespace, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, key, v);  // NVS skips the write when nothing changed
    nvs_commit(h);
    nvs_close(h);
  }
}

void app_erase(const char *key) {
  nvs_handle_t h;
  if (nvs_open(kAppNamespace, NVS_READWRITE, &h) == ESP_OK) {
    if (nvs_erase_key(h, key) == ESP_OK) {
      nvs_commit(h);
    }
    nvs_close(h);
  }
}

bool is_pairing_code(const char *s) {
  size_t n = 0;
  for (; s[n] != '\0'; n++) {
    if (s[n] < '0' || s[n] > '9') {
      return false;
    }
  }
  return n == 8;
}

// The static pairing code: 8 digits drawn once per device from the hardware
// RNG (a CSPRNG while the radio runs, as the Sendspin spec asks), then kept.
std::string load_or_make_pairing_code() {
  char code[16] = "";
  size_t len = sizeof(code);
  nvs_handle_t h;
  const bool nvs = nvs_open(kAppNamespace, NVS_READWRITE, &h) == ESP_OK;
  if (nvs && nvs_get_str(h, kCodeKey, code, &len) == ESP_OK &&
      is_pairing_code(code)) {
    nvs_close(h);
    return code;
  }
  uint32_t v;
  do {
    v = esp_random();
  } while (v >= 4000000000u);  // 40 x 10^8: every code equally likely
  std::snprintf(code, sizeof(code), "%08lu",
                static_cast<unsigned long>(v % 100000000u));
  if (!nvs || nvs_set_str(h, kCodeKey, code) != ESP_OK || nvs_commit(h) != ESP_OK) {
    ESP_LOGE(TAG, "could not save the pairing code: it changes at every start");
  }
  if (nvs) {
    nvs_close(h);
  }
  return code;
}

int32_t volume_to_gain_q15(uint8_t volume, bool muted) {
  if (muted || volume == 0) {
    return 0;
  }
  if (volume >= 100) {
    return 32768;
  }
  // amplitude = (volume / 100)^1.5, the curve the Sendspin player spec gives
  return static_cast<int32_t>(lroundf(powf(volume / 100.0f, 1.5f) * 32768.0f));
}

// ---------------------------------------------------------------------------
// I2S output
// ---------------------------------------------------------------------------

class I2sOutput {
 public:
  bool init(sendspin::PlayerRole *player) {
    player_ = player;
    // a static create gives xLength usable bytes minus one: + 1 keeps the
    // capacity a whole number of frames, so partial sends stay aligned
    pcm_ = xStreamBufferCreateWithCaps(kPcmBufBytes + 1, kFrameBytes,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // the ISR posts to sent_: internal RAM
    sent_ = xQueueCreateWithCaps(2 * kDmaDescNum + 4, sizeof(int64_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    records_ = xQueueCreate(2 * kDmaDescNum + 4, sizeof(uint32_t));
    go_ = xSemaphoreCreateBinary();
    idle_ = xSemaphoreCreateBinary();
    if (!pcm_ || !sent_ || !records_ || !go_ || !idle_) {
      ESP_LOGE(TAG, "out of memory for the I2S output");
      return false;
    }
    return xTaskCreatePinnedToCore(&I2sOutput::task_entry, "ss_i2s", 4096,
                                   this, 14, nullptr, 1) == pdPASS;
  }

  bool is_open() const { return chan_ != nullptr; }
  uint32_t sample_rate() const { return sample_rate_; }

  // Main loop thread, with no stream writing: the I2S port must be free
  // (snapcast released it).
  bool open(uint32_t sample_rate) {
    if (chan_ != nullptr) {
      return true;
    }
    i2s_std_gpio_config_t pins;
    i2s_port_t port;
    player_get_i2s_config(&pins, &port);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    // a buffer the task did not refill plays as zeros
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &chan_, nullptr);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
      chan_ = nullptr;
      return false;
    }

    // same clocking as snapclient's player (verified on this board)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = pins,
    };
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;

    i2s_event_callbacks_t cbs = {};
    cbs.on_sent = &I2sOutput::on_sent;
    cbs.on_send_q_ovf = &I2sOutput::on_send_q_ovf;

    err = i2s_channel_init_std_mode(chan_, &std_cfg);
    if (err == ESP_OK) {
      // before enabling: the very first transmitted buffer must raise an
      // event that pairs with the first preloaded record
      err = i2s_channel_register_event_callback(chan_, &cbs, this);
    }
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "I2S setup: %s", esp_err_to_name(err));
      i2s_del_channel(chan_);
      chan_ = nullptr;
      return false;
    }

    sample_rate_ = sample_rate;
    // nobody writes now: the sync task starts only after on_stream_start
    xStreamBufferReset(pcm_);
    if (!start_dma()) {
      i2s_del_channel(chan_);
      chan_ = nullptr;
      return false;
    }

    run_.store(true);
    xSemaphoreGive(go_);
    ESP_LOGI(TAG, "I2S output open: %lu Hz", (unsigned long)sample_rate);
    return true;
  }

  // Main loop thread, after the stream ended (the sync task is idle). Frees
  // the I2S port; whatever is still buffered is dropped.
  void close() {
    if (chan_ == nullptr) {
      return;
    }
    run_.store(false);
    if (xSemaphoreTake(idle_, pdMS_TO_TICKS(1000)) != pdTRUE) {
      // cannot happen: every wait in the task is bounded well below this
      ESP_LOGE(TAG, "I2S output task did not stop");
    }
    i2s_channel_disable(chan_);
    i2s_del_channel(chan_);
    chan_ = nullptr;
    xQueueReset(sent_);
    xQueueReset(records_);
    ESP_LOGI(TAG, "I2S output closed");
  }

  // Sync task (on_audio_write). Returns whole frames only.
  size_t write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!run_.load()) {
      // no output yet (the handover is still pending): don't let the sync
      // task spin; poll() retries the handover
      vTaskDelay(pdMS_TO_TICKS(timeout_ms));
      return 0;
    }
    len = std::min(len - len % kFrameBytes, kMaxSendBytes);
    // capacity and every read and write are whole frames, so a partial send
    // after the timeout is whole frames too
    size_t n = xStreamBufferSend(pcm_, data, len, pdMS_TO_TICKS(timeout_ms));
    if (n % kFrameBytes != 0) {
      ESP_LOGE(TAG, "misaligned PCM write (%u bytes)", (unsigned)n);
    }
    return n - n % kFrameBytes;
  }

  void set_gain(uint8_t volume, bool muted) {
    gain_target_.store(volume_to_gain_q15(volume, muted));
  }

 private:
  static void task_entry(void *arg) { static_cast<I2sOutput *>(arg)->task(); }

  void task() {
    for (;;) {
      xSemaphoreTake(go_, portMAX_DELAY);
      while (run_.load()) {
        if (!report_played() || !write_one_buffer()) {
          if (!run_.load()) {
            break;  // close() raced the error; nothing to repair
          }
          ESP_LOGW(TAG, "I2S buffer accounting lost, restarting the output");
          i2s_channel_disable(chan_);
          settle_accounting();
          if (!start_dma()) {
            ESP_LOGE(TAG, "I2S restart failed");
            run_.store(false);
          }
        }
      }
      xSemaphoreGive(idle_);
    }
  }

  // Channel disabled: fill every DMA descriptor with silence and record each
  // as carrying no audio, then start the DMA.
  bool start_dma() {
    xQueueReset(sent_);
    xQueueReset(records_);
    lockstep_lost_.store(false);
    std::memset(buf_, 0, sizeof(buf_));
    for (uint32_t i = 0; i < kDmaDescNum; i++) {
      size_t loaded = 0;
      esp_err_t err = i2s_channel_preload_data(chan_, buf_, kDmaBufBytes, &loaded);
      if (err != ESP_OK || loaded != kDmaBufBytes) {
        ESP_LOGE(TAG, "DMA preload %lu: %s, %u bytes", (unsigned long)i,
                 esp_err_to_name(err), (unsigned)loaded);
        return false;
      }
      uint32_t none = 0;
      xQueueSend(records_, &none, 0);
    }
    gain_now_ = gain_target_.load();
    return i2s_channel_enable(chan_) == ESP_OK;
  }

  // Before a restart (channel disabled): the library counted every frame we
  // accepted as buffered until it is reported played. Report what did play
  // with its real time, and what never will as finishing when the fresh
  // preload does, so the library's estimate stays right.
  void settle_accounting() {
    int64_t sent_at;
    uint32_t frames;
    while (xQueueReceive(sent_, &sent_at, 0) == pdTRUE &&
           xQueueReceive(records_, &frames, 0) == pdTRUE) {
      report(frames, sent_at);
    }
    uint32_t lost = 0;
    while (xQueueReceive(records_, &frames, 0) == pdTRUE) {
      lost += frames;
    }
    if (lost > 0) {
      player_->notify_audio_played(lost, esp_timer_get_time() + kDmaDescNum * kDmaBufUs);
    }
  }

  void report(uint32_t frames, int64_t sent_at) {
    if (frames > 0) {
      // the real audio sits at the start of the buffer: it finished playing
      // before the trailing silence
      int64_t pad_us = static_cast<int64_t>(kDmaFrameNum - frames) * 1000000 /
                       sample_rate_;
      player_->notify_audio_played(frames, sent_at - pad_us);
    }
  }

  // Pair each sent buffer with its record and report the real audio in it.
  bool report_played() {
    int64_t sent_at;
    while (xQueueReceive(sent_, &sent_at, 0) == pdTRUE) {
      uint32_t frames;
      if (xQueueReceive(records_, &frames, 0) != pdTRUE) {
        return false;  // a buffer played that we never wrote
      }
      report(frames, sent_at);
    }
    return !lockstep_lost_.load();
  }

  // Write exactly one DMA buffer: the audio we have, then silence.
  bool write_one_buffer() {
    size_t got = xStreamBufferReceive(pcm_, buf_, kDmaBufBytes, 0);
    if (got < kDmaBufBytes) {
      got += xStreamBufferReceive(pcm_, buf_ + got, kDmaBufBytes - got,
                                  kFillWaitTicks);
    }
    got -= got % kFrameBytes;
    apply_gain(reinterpret_cast<int16_t *>(buf_), got / sizeof(int16_t));
    std::memset(buf_ + got, 0, kDmaBufBytes - got);

    size_t written = 0;
    // blocks until a buffer frees up (one every 11.6 ms)
    esp_err_t err = i2s_channel_write(chan_, buf_, kDmaBufBytes, &written,
                                      pdMS_TO_TICKS(200));
    uint32_t frames = got / kFrameBytes;
    if (written == kDmaBufBytes) {
      // record it even if a check below fails: it will play
      xQueueSend(records_, &frames, 0);
    }
    if (err != ESP_OK || written != kDmaBufBytes) {
      ESP_LOGW(TAG, "I2S write: %s, %u of %u bytes", esp_err_to_name(err),
               (unsigned)written, (unsigned)kDmaBufBytes);
      return false;
    }
    return true;
  }

  // Volume, ramped linearly over the buffer so changes don't click.
  void apply_gain(int16_t *samples, size_t count) {
    const int32_t target = gain_target_.load();
    const int32_t start = gain_now_;
    if (count == 0) {
      return;
    }
    if (start == 32768 && target == 32768) {
      return;
    }
    for (size_t i = 0; i < count; i++) {
      int32_t g = start + static_cast<int32_t>(
                              (static_cast<int64_t>(target - start) * i) / count);
      samples[i] = static_cast<int16_t>((static_cast<int32_t>(samples[i]) * g) >> 15);
    }
    gain_now_ = target;
  }

  static bool on_sent(i2s_chan_handle_t, i2s_event_data_t *, void *ctx);
  static bool on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *, void *ctx);

  sendspin::PlayerRole *player_ = nullptr;
  i2s_chan_handle_t chan_ = nullptr;
  uint32_t sample_rate_ = kSampleRate;
  StreamBufferHandle_t pcm_ = nullptr;
  QueueHandle_t sent_ = nullptr;     // on_sent timestamps, ISR -> task
  QueueHandle_t records_ = nullptr;  // real frames per written DMA buffer
  SemaphoreHandle_t go_ = nullptr;
  SemaphoreHandle_t idle_ = nullptr;
  std::atomic<bool> run_{false};
  std::atomic<bool> lockstep_lost_{false};
  std::atomic<int32_t> gain_target_{32768};
  int32_t gain_now_ = 32768;
  alignas(4) uint8_t buf_[kDmaBufBytes];
};

// IRAM-safe ISR callbacks: they touch only the internal-RAM queue and flags
bool IRAM_ATTR I2sOutput::on_sent(i2s_chan_handle_t, i2s_event_data_t *, void *ctx) {
  auto *self = static_cast<I2sOutput *>(ctx);
  int64_t now = esp_timer_get_time();
  BaseType_t woken = pdFALSE;
  if (xQueueSendFromISR(self->sent_, &now, &woken) != pdTRUE) {
    // the task fell far behind; it restarts the DMA
    self->lockstep_lost_.store(true);
  }
  return woken == pdTRUE;
}

// The driver dropped a free buffer the task never took: from here on the
// buffers no longer match the records, so the task restarts the DMA.
bool IRAM_ATTR I2sOutput::on_send_q_ovf(i2s_chan_handle_t, i2s_event_data_t *,
                                        void *ctx) {
  static_cast<I2sOutput *>(ctx)->lockstep_lost_.store(true);
  return false;
}

// ---------------------------------------------------------------------------
// Library glue
// ---------------------------------------------------------------------------

// The library's keyspace: fixed keys of at most 12 characters, all calls on
// the main loop thread.
class NvsStore : public sendspin::SendspinPersistenceProvider {
 public:
  bool open() {
    ok_ = nvs_open(kLibNamespace, NVS_READWRITE, &h_) == ESP_OK;
    if (!ok_) {
      ESP_LOGE(TAG, "NVS namespace %s unavailable: identity and pairing "
                    "will not survive a reboot", kLibNamespace);
    }
    return ok_;
  }

  std::optional<std::vector<uint8_t>> load_blob(const std::string &key) override {
    size_t len = 0;
    if (!ok_ || nvs_get_blob(h_, key.c_str(), nullptr, &len) != ESP_OK) {
      return std::nullopt;
    }
    std::vector<uint8_t> v(len);
    if (len > 0 && nvs_get_blob(h_, key.c_str(), v.data(), &len) != ESP_OK) {
      return std::nullopt;
    }
    return v;
  }

  bool save_blob(const std::string &key, const uint8_t *data, size_t len) override {
    return ok_ && nvs_set_blob(h_, key.c_str(), data, len) == ESP_OK;
  }

  bool commit() override { return ok_ && nvs_commit(h_) == ESP_OK; }

  // forget_paired() only, right before a restart
  bool erase(const std::string &key) {
    if (!ok_) {
      return false;
    }
    esp_err_t err = nvs_erase_key(h_, key.c_str());
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
  }

 private:
  nvs_handle_t h_ = 0;
  bool ok_ = false;
};

class Network : public sendspin::SendspinNetworkProvider {
 public:
  bool is_network_ready() override {
    esp_netif_ip_info_t ip = {};
    return network_if_get_ip(&ip) && ip.ip.addr != 0;
  }
};

const char *abort_text(sendspin::SendspinPairAbortReason reason) {
  using R = sendspin::SendspinPairAbortReason;
  switch (reason) {
    case R::ATTEMPT_TIMEOUT:
      return "timed out";
    case R::CONCURRENT_ATTEMPT:
      return "another pairing under way";
    case R::METHOD_NOT_SUPPORTED:
      return "unsupported method";
    case R::PAIRING_CODE_MISMATCH:
      return "wrong code";
    case R::USER_CANCELLED:
      return "cancelled";
    default:
      return "protocol error";
  }
}

// Pairing state for General Settings and the LED, and the commands that change
// it. Loop task only; the library calls its listeners there too.
//
// The library keeps its pairing window to itself, so the window here follows
// its rules from what can be seen (pairing.md "Pairing Window"):
//   - a gesture (button or General Settings) opens it for 5 minutes; with a
//     server waiting, that server's attempt starts at once;
//   - the first attempt under it binds it to that server's connection;
//   - it closes when a pairing under it succeeds, after five wrong codes, on
//     Stop, at expiry, and when its connection is lost.
// The library admits one server at a time, and get_server_information()
// names it (nothing while none is admitted): when that is no longer the
// window's or the attempt's server, its connection is gone. Some ends of an
// attempt fire no callback (a dropped connection with no prompt shown, or a
// server leaving pairing with a plain activate), so those checks and a
// trust report end it too. Missed: the same server reconnecting within one
// 20 ms loop.
// A gesture during an attempt would reopen the window unbound (open to any
// connection, and left open by the success), so it is ignored; a success
// also cancels any window still open.
class Pairing {
 public:
  void init(sendspin::SendspinClient *client, NvsStore *store, std::string code,
            bool from_page) {
    client_ = client;
    store_ = store;
    code_ = std::move(code);
    from_page_ = from_page;
    load_names();
  }

  // ---- library callbacks, forwarded by Player ----

  void on_started(const std::string &server_id) {
    attempt_id_ = server_id;
    // bounded by the library's 120 s attempt timeout (which reports a
    // failure); a server may also drop an attempt without either callback
    attempt_until_us_ = esp_timer_get_time() + kAttemptUs;
    if (window_until_us_ != 0 && window_for_.empty()) {
      window_for_ = server_id;  // an open window admits it and binds to it
    }
    ESP_LOGI(TAG, "pairing started by server %s", server_id.c_str());
  }

  // the attempt waits for the pairing-window gesture
  void on_open_window() {
    prompt_ = true;
    gesture_ = false;
    // event texts end at 95 characters, and a server name can take 45
    event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                   "%s asks to pair: press the button or Allow pairing",
                   display_name(attempt_id_).c_str());
  }

  // after every attempt that opened the prompt, whatever its outcome
  void on_close_window() {
    prompt_ = false;
    gesture_ = false;
    attempt_until_us_ = 0;
  }

  void on_succeeded(const std::string &server_id) {
    // the library closes the window this attempt ran under; close any other
    // one too, so nothing stays open that the page doesn't show
    client_->cancel_pairing_window();
    window_until_us_ = 0;
    window_for_.clear();
    attempt_until_us_ = 0;
    refresh_records();  // written before this callback
    learn_name(server_id);
    event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO, "Paired with %s",
                   display_name(server_id).c_str());
  }

  void on_failed(const std::string &server_id,
                 sendspin::SendspinPairAbortReason reason) {
    using R = sendspin::SendspinPairAbortReason;
    if (server_id == attempt_id_) {
      attempt_until_us_ = 0;
    }
    if (window_until_us_ != 0) {
      if (reason == R::PAIRING_CODE_MISMATCH && ++window_failures_ >= kWindowFailLimit) {
        close_window_state();  // five wrong codes
      } else if ((reason == R::UNKNOWN || reason == R::CONCURRENT_ATTEMPT) &&
                 server_id == window_for_) {
        close_window_state();  // the library closed that connection
      }
    }
    event_log_addf(EVENT_SRC_SENDSPIN, EVENT_WARN, "Pairing with %s failed: %s",
                   display_name(server_id).c_str(), abort_text(reason));
  }

  // a connection reported its trust: keep a paired server's name for the list
  // (always after a handshake, which ends any attempt on that connection; a
  // new attempt on it reports on_started after this)
  void on_trust(bool paired) {
    if (!prompt_) {
      attempt_until_us_ = 0;
    }
    auto info = client_->get_server_information();
    if (paired && info) {
      learn_name(info->server_id);
    }
  }

  // ---- commands ----

  void open_window(const char *how) {
    if (pairing_now()) {
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                     "Pairing with %s is under way: type the code in it",
                     display_name(attempt_id_).c_str());
      return;
    }
    client_->confirm_pairing_window();
    window_until_us_ = esp_timer_get_time() + kWindowUs;
    window_failures_ = 0;
    window_for_.clear();
    if (waiting()) {
      gesture_ = true;              // the waiting attempt starts now...
      window_for_ = attempt_id_;    // ...and the window binds to it
      attempt_until_us_ = esp_timer_get_time() + kAttemptUs;
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                     "Pairing allowed (%s): type the code in %s", how,
                     display_name(attempt_id_).c_str());
    } else {
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                     "Pairing allowed for 5 minutes (%s)", how);
    }
  }

  // Stop: closes the window, and refuses a server still waiting for it. An
  // attempt already under way runs to its end (the library's rule), but a
  // retry after a wrong code then needs a new gesture.
  void close_window() {
    client_->cancel_pairing_window();
    if (pairing_now()) {
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                     "Pairing window closed; %s may still finish pairing",
                     display_name(attempt_id_).c_str());
    } else if (window_until_us_ != 0 || waiting()) {
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_INFO,
                    waiting() ? "Pairing refused (settings page)"
                              : "Pairing window closed (settings page)");
    }
    close_window_state();
  }

  void set_require(bool require) {
    const bool unpaired = !require;
    const bool changed = client_->is_unpaired_access_enabled() != unpaired;
    // turning it off closes the connections of unpaired servers
    client_->set_unpaired_access_enabled(unpaired);
    // saved only while it differs from the build setting, so the build
    // setting applies again once the two agree
    from_page_ = unpaired != kUnpairedDefault;
    if (from_page_) {
      app_save_u8(kUnpairedKey, unpaired ? 1 : 0);
    } else {
      app_erase(kUnpairedKey);
    }
    if (changed) {
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_INFO,
                    require ? "Pairing required: unpaired servers are refused "
                              "(settings page)"
                            : "Unpaired servers may play (settings page)");
    }
  }

  size_t paired_count() const { return records_.size(); }

  // Read the record slots the library stored (the same thread uses them).
  void refresh_records() {
    records_.clear();
    for (size_t n = 0; n < kMaxRecords; n++) {
      auto blob = store_->load_blob(sendspin::persistence_keys::record_slot_key(n));
      if (!blob) {
        continue;
      }
      // a free slot is all zeros, which decodes to nothing
      auto rec = sendspin::decode_pairing_record(blob->data(), blob->size());
      if (rec) {
        records_.push_back(rec->server_id);
      }
    }
    next_refresh_us_ = esp_timer_get_time() + kRecordsRefreshUs;
  }

  // Every loop: expiries, the LED, deferred saves, the snapshot.
  void poll(bool output_open) {
    const int64_t now = esp_timer_get_time();
    if (window_until_us_ != 0 && now >= window_until_us_) {
      close_window_state();
    }
    if (attempt_until_us_ != 0 && now >= attempt_until_us_) {
      attempt_until_us_ = 0;
    }
    // the one admitted server: has the window's or the attempt's gone?
    if ((window_until_us_ != 0 && !window_for_.empty()) ||
        (attempt_until_us_ != 0 && !waiting())) {
      auto info = client_->get_server_information();
      if (!window_for_.empty() && (!info || info->server_id != window_for_)) {
        close_window_state();  // the library closed it with the connection
      }
      if (attempt_until_us_ != 0 && !waiting() &&
          (!info || info->server_id != attempt_id_)) {
        attempt_until_us_ = 0;
      }
    }
    // blinking: press the button; steady: pairing is allowed or under way
    const uint8_t led = waiting() ? 2 : (window_until_us_ != 0 || pairing_now()) ? 1 : 0;
    if (led != led_) {
      led_ = led;
      status_led_set_pairing(led);
    }
    if (now >= next_refresh_us_) {
      refresh_records();
    }
    // a flash write during playback can stall the output task
    if (names_dirty_ && !output_open) {
      names_dirty_ = false;
      save_names();
    }
    if (now < next_publish_us_) {
      return;
    }
    next_publish_us_ = now + kStatusPeriodUs;

    sendspin_pairing_t &p = scratch_;  // ~900 bytes: not on the stack
    p = {};
    copy_str(p.code, sizeof(p.code), code_);
    p.require_pairing = !client_->is_unpaired_access_enabled();
    p.require_from_page = from_page_;
    p.window_open = window_until_us_ != 0;
    if (p.window_open) {
      p.window_left_s = static_cast<uint16_t>((window_until_us_ - now + 999999) / 1000000);
      if (!window_for_.empty()) {
        copy_utf8(p.window_for, sizeof(p.window_for), display_name(window_for_));
      }
    }
    p.waiting = waiting();
    p.pairing_now = pairing_now();
    if (p.waiting || p.pairing_now) {
      copy_utf8(p.server, sizeof(p.server), display_name(attempt_id_));
    }
    auto info = client_->is_connected() ? client_->get_server_information()
                                        : std::nullopt;
    const size_t count = std::min(records_.size(), size_t{SENDSPIN_MAX_PAIRED});
    for (size_t i = 0; i < count; i++) {
      const std::string &id = records_[i];
      sendspin_paired_server_t &e = p.paired[i];
      // snprintf: GCC misreads a copy helper inlined in this loop as overflowing
      std::snprintf(e.id, sizeof(e.id), "%s", id.c_str());
      const Name *n = find_name(id);
      if (n != nullptr) {
        std::snprintf(e.name, sizeof(e.name), "%s", n->name);
      }
      e.connected = info && info->server_id == id;
    }
    p.paired_count = static_cast<uint8_t>(count);
    taskENTER_CRITICAL(&g_pair_mux);
    g_pair = p;
    taskEXIT_CRITICAL(&g_pair_mux);
  }

 private:
  // Server names for the list: a record holds only the server's key.
  struct Name {
    char id[17];  // the key's first 16 characters
    char name[47];
  };

  static bool same_id(const Name &n, const std::string &id) {
    return n.id[0] != '\0' && id.compare(0, sizeof(n.id) - 1, n.id) == 0;
  }

  const Name *find_name(const std::string &id) const {
    for (const auto &n : names_) {
      if (same_id(n, id)) {
        return &n;
      }
    }
    return nullptr;
  }

  bool is_paired(const std::string &id) const {
    return std::find(records_.begin(), records_.end(), id) != records_.end();
  }

  // Remember the connected server's name if it is this paired server.
  void learn_name(const std::string &id) {
    auto info = client_->get_server_information();
    if (!info || info->server_id != id || info->name.empty() || !is_paired(id)) {
      return;
    }
    Name *slot = nullptr;
    for (auto &n : names_) {
      if (same_id(n, id)) {
        slot = &n;
        break;
      }
    }
    if (slot == nullptr) {
      // a free entry, or one whose server is no longer paired
      for (auto &n : names_) {
        bool stale = n.id[0] == '\0';
        if (!stale) {
          stale = true;
          for (const auto &r : records_) {
            if (same_id(n, r)) {
              stale = false;
              break;
            }
          }
        }
        if (stale) {
          slot = &n;
          break;
        }
      }
    }
    if (slot == nullptr) {
      return;
    }
    char name[sizeof(slot->name)];
    copy_utf8(name, sizeof(name), info->name);
    if (same_id(*slot, id) && std::strcmp(slot->name, name) == 0) {
      return;
    }
    std::memset(slot, 0, sizeof(*slot));
    copy_str(slot->id, sizeof(slot->id), id);
    std::memcpy(slot->name, name, sizeof(name));
    names_dirty_ = true;
  }

  // a name for events: the one learned, the connected server's, or its key
  std::string display_name(const std::string &id) const {
    const Name *n = find_name(id);
    if (n != nullptr && n->name[0] != '\0') {
      return n->name;
    }
    auto info = client_->get_server_information();
    if (info && !id.empty() && info->server_id == id && !info->name.empty()) {
      return info->name;
    }
    if (id.empty()) {
      return "A server";
    }
    return "server " + id.substr(0, 8);
  }

  void load_names() {
    nvs_handle_t h;
    std::memset(names_, 0, sizeof(names_));
    if (nvs_open(kAppNamespace, NVS_READONLY, &h) != ESP_OK) {
      return;
    }
    size_t len = sizeof(names_);
    if (nvs_get_blob(h, kNamesKey, names_, &len) != ESP_OK || len != sizeof(names_)) {
      std::memset(names_, 0, sizeof(names_));
    }
    nvs_close(h);
    for (auto &n : names_) {  // never trust a stored string's end
      n.id[sizeof(n.id) - 1] = '\0';
      n.name[sizeof(n.name) - 1] = '\0';
    }
  }

  void save_names() {
    nvs_handle_t h;
    if (nvs_open(kAppNamespace, NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_blob(h, kNamesKey, names_, sizeof(names_));
      nvs_commit(h);
      nvs_close(h);
    }
  }

  // the server's attempt waits for the gesture
  bool waiting() const { return prompt_ && !gesture_; }
  // an attempt runs: the operator types the code in the server
  bool pairing_now() const { return attempt_until_us_ != 0 && !waiting(); }

  void close_window_state() {
    window_until_us_ = 0;
    window_for_.clear();
    window_failures_ = 0;
  }

  sendspin::SendspinClient *client_ = nullptr;
  NvsStore *store_ = nullptr;
  std::string code_;
  bool from_page_ = false;
  std::vector<std::string> records_;  // paired servers' keys
  Name names_[kMaxRecords] = {};
  bool names_dirty_ = false;
  std::string attempt_id_;      // the server of the latest pairing attempt
  int64_t attempt_until_us_ = 0;  // 0: no attempt under way
  bool prompt_ = false;   // the library asked for the gesture (open..close)
  bool gesture_ = false;  // ... and got it
  int64_t window_until_us_ = 0;
  std::string window_for_;  // the server it is bound to, "" = unbound
  uint8_t window_failures_ = 0;
  uint8_t led_ = 0;
  int64_t next_refresh_us_ = 0;
  int64_t next_publish_us_ = 0;
  sendspin_pairing_t scratch_ = {};
};

class Player : public sendspin::PlayerRoleListener,
               public sendspin::SendspinClientListener {
 public:
  void attach(sendspin::SendspinClient *client, sendspin::PlayerRole *role,
              I2sOutput *out, Pairing *pairing) {
    client_ = client;
    role_ = role;
    out_ = out;
    pairing_ = pairing;
    // held while the library asks for low-latency networking (streaming)
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "sendspin", &cpu_lock_);
  }

  // ---- PlayerRoleListener ----

  size_t on_audio_write(uint8_t *data, size_t length, uint32_t timeout_ms) override {
    return out_->write(data, length, timeout_ms);
  }

  // Main loop thread. For a new stream this runs before the sync task writes
  // any of it; a start without an end before it changes the active stream
  // in place while its audio keeps flowing.
  void on_stream_start() override {
    const auto &p = role_->get_current_stream_params();
    uint32_t rate = p.sample_rate.value_or(kSampleRate);
    ESP_LOGI(TAG, "stream start: %lu Hz, %u ch, %u bit%s", (unsigned long)rate,
             (unsigned)p.channels.value_or(0), (unsigned)p.bit_depth.value_or(0),
             stream_active_ ? " (in place)" : "");
    if (p.channels.value_or(kChannels) != kChannels ||
        p.bit_depth.value_or(kBits) != kBits || rate != kSampleRate) {
      // only 44.1 kHz 16-bit stereo is advertised; play it anyway
      ESP_LOGE(TAG, "server sent a format this player did not offer");
    }

    if (!stream_active_) {
      char khz[16];
      std::string codec = p.codec ? codec_name(*p.codec) : "?";
      for (auto &c : codec) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                     "Stream started: %s, %s, %u-bit", codec.c_str(),
                     event_log_khz(khz, sizeof(khz), rate),
                     (unsigned)p.bit_depth.value_or(0));
    }
    stream_active_ = true;
    release_at_us_ = 0;
    output_failed_ = false;  // a new stream: no failure seen for it yet

    if (out_->is_open()) {
      // keep running, never drop audio the library has counted: an
      // in-place start keeps streaming, and a new stream in the grace period
      // after an end follows the old one's (already played) tail
      return;
    }
    bool port_free;
    if (!holding_) {
      // snapcast stops and frees the I2S port; its audio is dropped from now on
      holding_ = true;
      port_free = player_release_output(kHandoverTimeoutMs) == 0;
    } else {
      port_free = player_release_output(0) == 0;  // asked earlier: just check
    }
    if (!port_free) {
      ESP_LOGW(TAG, "snapcast still has the output; retrying");
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_WARN,
                    "Waiting for snapcast to release the output");
      retry_at_us_ = esp_timer_get_time() + kRetryUs;
      return;
    }
    open_output();
  }

  void on_stream_end() override {
    ESP_LOGI(TAG, "stream end");
    event_log_add(EVENT_SRC_SENDSPIN, EVENT_INFO, "Stream ended");
    stream_active_ = false;
    // keep the output a little longer in case the next stream follows
    release_at_us_ = esp_timer_get_time() + kReleaseDelayUs;
  }

  void on_volume_changed(uint8_t volume) override {
    volume_ = volume;
    out_->set_gain(volume_, muted_);
    // saved once the output is closed: a flash write during playback can
    // stall the output task
    settings_dirty_ = true;
  }

  void on_mute_changed(bool muted) override {
    muted_ = muted;
    out_->set_gain(volume_, muted_);
    settings_dirty_ = true;
  }

  // ---- SendspinClientListener ----

  void on_request_high_performance() override {
    esp_wifi_get_ps(&saved_ps_);
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (cpu_lock_) {
      esp_pm_lock_acquire(cpu_lock_);
    }
  }

  void on_release_high_performance() override {
    esp_wifi_set_ps(saved_ps_);
    if (cpu_lock_) {
      esp_pm_lock_release(cpu_lock_);
    }
  }

  void on_pairing_started(const std::string &server_id) override {
    pairing_->on_started(server_id);
  }

  void on_pairing_succeeded(const std::string &server_id) override {
    ESP_LOGI(TAG, "paired with server %s", server_id.c_str());
    pairing_->on_succeeded(server_id);
  }

  void on_pairing_failed(const std::string &server_id,
                         sendspin::SendspinPairAbortReason reason) override {
    ESP_LOGW(TAG, "pairing with server %s failed", server_id.c_str());
    pairing_->on_failed(server_id, reason);
  }

  void on_open_pairing_window() override { pairing_->on_open_window(); }
  void on_close_pairing_window() override { pairing_->on_close_window(); }

  // every new connection (and a re-handshake after pairing) reports its trust
  void on_trust_changed(sendspin::ConnectionTrust trust) override {
    trust_ = trust;
    const bool paired = trust == sendspin::ConnectionTrust::USER;
    ESP_LOGI(TAG, "server connected (%s)", paired ? "paired" : "unpaired");
    auto info = client_->get_server_information();
    server_name_ = info ? info->name : std::string();
    event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO, "Server %s connected (%s)",
                   server_name_.empty() ? "?" : server_name_.c_str(),
                   paired ? "paired" : "unpaired");
    pairing_->on_trust(paired);
  }

  // Deltas (an absent field is unchanged). Kept here rather than read from
  // get_group_state(), which survives into the next server's connection.
  // A server switch happens within one protocol tick, so the poll loop may
  // never see a disconnect: start over when the server changes.
  void on_group_update(const sendspin::GroupUpdateObject &group) override {
    auto info = client_->get_server_information();
    std::string id = info ? info->server_id : std::string();
    if (id != group_server_id_) {
      group_server_id_ = id;
      group_name_.clear();
      group_playing_ = false;
    }
    have_group_ = true;
    if (group.group_name) {
      group_name_ = *group.group_name;  // "" when the server cleared it
    }
    if (group.playback_state) {
      group_playing_ = *group.playback_state == sendspin::SendspinPlaybackState::PLAYING;
    }
  }

  // the clock filter's uncertainty in microseconds, about every 10 s
  void on_time_sync_updated(float error) override {
    if (std::isfinite(error) && error >= 0.0f && error < 1e9f) {
      sync_error_us_ = static_cast<uint32_t>(error);
      have_sync_error_ = true;
    }
  }

  // ---- loop ----

  // After client.start(): report the volume this player had before reboot.
  void restore_volume() {
    volume_ = app_load_u8("vol").value_or(volume_);
    muted_ = app_load_u8("mute").value_or(0) != 0;
    out_->set_gain(volume_, muted_);
    role_->update_volume(volume_);
    role_->update_muted(muted_);
  }

  // Main loop thread, after every client.loop().
  void poll() {
    int64_t now = esp_timer_get_time();

    // a stream waits for snapcast to let go of the output (or for a failed
    // open): retry, without blocking the loop
    if (stream_active_ && holding_ && !out_->is_open() && now >= retry_at_us_) {
      if (player_release_output(0) == 0) {
        open_output();
      }
      if (!out_->is_open()) {
        retry_at_us_ = now + kRetryUs;
      }
    }

    if (holding_ && !stream_active_ && release_at_us_ != 0 && now >= release_at_us_) {
      release_at_us_ = 0;
      if (out_->is_open()) {
        out_->close();
        audio_set_mute(true);
        audio_dac_enable(false);
      }
      holding_ = false;
      player_reclaim_output();
      ESP_LOGI(TAG, "output handed back to snapcast");
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_INFO,
                    "Handed the output back to snapcast");
    }

    if (settings_dirty_ && !out_->is_open()) {
      settings_dirty_ = false;
      app_save_u8("vol", volume_);
      app_save_u8("mute", muted_ ? 1 : 0);
    }
  }

  // Main loop thread, after poll(): refresh the Status tab snapshot. Most
  // of these getters are main-loop only, so the web server never calls the
  // library itself.
  void publish_status(sendspin::SendspinClient *client) {
    int64_t now = esp_timer_get_time();
    bool connected = client->is_connected();
    if (!connected && was_connected_) {
      event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO, "Server %s disconnected",
                     server_name_.empty() ? "?" : server_name_.c_str());
      // nothing in the library signals a disconnect: forget what belonged
      // to that server (trust is reported again by every new connection)
      have_group_ = false;
      group_server_id_.clear();
      group_name_.clear();
      group_playing_ = false;
      have_sync_error_ = false;
    }
    was_connected_ = connected;
    if (now < next_status_us_) {
      return;
    }
    next_status_us_ = now + kStatusPeriodUs;

    sendspin_status_t st = {};
    st.running = client->is_started();
    st.port = CONFIG_SENDSPIN_PLAYER_PORT;
    copy_str(st.client_id, sizeof(st.client_id), client->client_id());
    st.unpaired_access = client->is_unpaired_access_enabled();

    st.connected = connected;
    if (connected) {
      st.paired = trust_ == sendspin::ConnectionTrust::USER;
      auto info = client->get_server_information();
      if (info) {
        copy_utf8(st.server_name, sizeof(st.server_name), info->name);
      }
      if (have_group_) {
        st.have_group = true;
        copy_str(st.group_name, sizeof(st.group_name), group_name_);
        st.group_playing = group_playing_;
      }
      st.time_synced = client->is_time_synced();
      st.have_sync_error = st.time_synced && have_sync_error_;
      st.sync_error_us = st.have_sync_error ? sync_error_us_ : 0;
    }

    st.stream = stream_active_;
    if (stream_active_) {
      // the library keeps the last stream's parameters after it ends
      const auto &p = role_->get_current_stream_params();
      if (p.codec) {
        std::snprintf(st.codec, sizeof(st.codec), "%s", codec_name(*p.codec));
      }
      st.sample_rate = p.sample_rate.value_or(0);
      st.bits = p.bit_depth.value_or(0);
      st.channels = p.channels.value_or(0);
    }
    st.output_open = out_->is_open();
    if (st.output_open) {
      st.output_rate = out_->sample_rate();
      st.output_bits = kBits;
    }
    st.output_failed = output_failed_ && stream_active_ && holding_ && !st.output_open;
    st.waiting_for_output =
        stream_active_ && holding_ && !st.output_open && !st.output_failed;
    st.volume = volume_;
    st.muted = muted_;
    st.stack_free = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));

    taskENTER_CRITICAL(&g_status_mux);
    g_status = st;
    taskEXIT_CRITICAL(&g_status_mux);
  }

 private:
  void open_output() {
    output_failed_ = false;
    if (out_->open(kSampleRate)) {
      audio_dac_enable(true);
      audio_set_mute(false);
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_INFO,
                    "Took the audio output (snapcast paused)");
    } else {
      output_failed_ = true;  // snapcast let go; our own I2S setup failed
      retry_at_us_ = esp_timer_get_time() + kRetryUs;
      event_log_add(EVENT_SRC_SENDSPIN, EVENT_ERROR,
                    "Could not open the audio output");
    }
  }

  sendspin::SendspinClient *client_ = nullptr;
  sendspin::PlayerRole *role_ = nullptr;
  I2sOutput *out_ = nullptr;
  Pairing *pairing_ = nullptr;
  bool stream_active_ = false;
  bool holding_ = false;  // snapcast was asked to release the output
  int64_t release_at_us_ = 0;
  int64_t retry_at_us_ = 0;
  // first-boot volume: 30 -> -15.7 dB, about where snapcast sits here
  uint8_t volume_ = 30;
  bool muted_ = false;
  bool settings_dirty_ = false;
  wifi_ps_type_t saved_ps_ = WIFI_PS_MIN_MODEM;
  esp_pm_lock_handle_t cpu_lock_ = nullptr;
  // Status tab
  sendspin::ConnectionTrust trust_ = sendspin::ConnectionTrust::NONE;
  bool was_connected_ = false;
  bool have_group_ = false;
  std::string group_server_id_;
  std::string server_name_;  // for the disconnect event
  std::string group_name_;
  bool group_playing_ = false;
  bool output_failed_ = false;
  bool have_sync_error_ = false;
  uint32_t sync_error_us_ = 0;
  int64_t next_status_us_ = 0;
};

#if CONFIG_SENDSPIN_ENABLE_METADATA
// Track details for the Status tab. Not for the event log: one entry per
// song would push everything else out of it within hours.
class NowPlaying : public sendspin::MetadataRoleListener {
 public:
  void attach(sendspin::MetadataRole *role) { role_ = role; }

  // Main loop thread. Each state is complete: a field left out is unknown.
  void on_metadata(const sendspin::ServerMetadataStateObject &m) override {
    // ~1 KB: a member, not on the loop task's stack
    sendspin_now_playing_t &np = scratch_;
    np = {};
    np.valid = true;
    if (m.title) copy_utf8(np.title, sizeof(np.title), *m.title);
    if (m.artist) copy_utf8(np.artist, sizeof(np.artist), *m.artist);
    if (m.album_artist) {
      copy_utf8(np.album_artist, sizeof(np.album_artist), *m.album_artist);
    }
    if (m.album) copy_utf8(np.album, sizeof(np.album), *m.album);
    // the browser loads it from the server: only a whole http(s) URL
    if (m.artwork_url && m.artwork_url->size() < sizeof(np.artwork_url) &&
        (m.artwork_url->rfind("http://", 0) == 0 ||
         m.artwork_url->rfind("https://", 0) == 0)) {
      copy_utf8(np.artwork_url, sizeof(np.artwork_url), *m.artwork_url);
    }
    np.year = m.year.value_or(0);
    np.track = m.track.value_or(0);
    np.has_progress = m.progress.has_value();
    if (np.has_progress && role_ != nullptr) {
      np.progress_ms = role_->get_track_progress_ms();
      np.duration_ms = role_->get_track_duration_ms();
    }
    has_progress_ = np.has_progress;
    taskENTER_CRITICAL(&g_np_mux);
    g_np = np;
    taskEXIT_CRITICAL(&g_np_mux);
  }

  void on_metadata_clear() override {
    has_progress_ = false;
    taskENTER_CRITICAL(&g_np_mux);
    g_np.valid = false;
    taskEXIT_CRITICAL(&g_np_mux);
  }

  // Main loop thread: keep the position current (the role interpolates it)
  void poll() {
    int64_t now = esp_timer_get_time();
    if (role_ == nullptr || !has_progress_ || now < next_us_) {
      return;
    }
    next_us_ = now + kStatusPeriodUs;
    uint32_t progress = role_->get_track_progress_ms();
    uint32_t duration = role_->get_track_duration_ms();
    taskENTER_CRITICAL(&g_np_mux);
    g_np.progress_ms = progress;
    g_np.duration_ms = duration;
    taskEXIT_CRITICAL(&g_np_mux);
  }

 private:
  sendspin::MetadataRole *role_ = nullptr;
  bool has_progress_ = false;
  int64_t next_us_ = 0;
  sendspin_now_playing_t scratch_ = {};
};
#endif

// Static, so it is in internal RAM: the IRAM-safe I2S driver requires the
// callback context there (a plain `new` may land in PSRAM).
I2sOutput g_output;
std::string g_name;

// Delete every pairing record, then restart the device: the library keeps
// the records in memory while it runs. (Stopping and restarting the client
// instead would send every server a "shutdown" goodbye, and Music Assistant
// would not come back by itself.) The device key and the Pairing PSK stay,
// so the client ID does not change. Loop task, between two client loops, so
// no write the library owes can land in between.
[[noreturn]] void forget_paired(Pairing *pairing, NvsStore *store) {
  const size_t count = pairing->paired_count();
  bool erased = true;
  for (size_t n = 0; n < kMaxRecords; n++) {
    erased &= store->erase(sendspin::persistence_keys::record_slot_key(n));
  }
  erased &= store->erase(sendspin::persistence_keys::RECORD_ORDER);
  erased &= store->commit();
  app_erase(kNamesKey);
  if (erased) {
    event_log_addf(EVENT_SRC_SENDSPIN, EVENT_INFO,
                   "Forgot %u paired Sendspin server%s (settings page)",
                   static_cast<unsigned>(count), count == 1 ? "" : "s");
    event_log_note_restart(EVENT_INFO, "Restarting to forget the paired servers");
  } else {
    // whatever was erased stays erased; the restart reloads the rest
    event_log_note_restart(EVENT_WARN,
                           "Forgetting the paired servers partly failed; restarting");
  }
  // Let the page's request finish first. Blocking this task (not a timer)
  // keeps the library from writing the erased records back meanwhile.
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

void run_command(uint8_t cmd, Pairing *pairing, NvsStore *store) {
  switch (cmd) {
    case SENDSPIN_CMD_ALLOW_PAIRING_BUTTON:
      pairing->open_window("device button");
      break;
    case SENDSPIN_CMD_ALLOW_PAIRING_PAGE:
      pairing->open_window("settings page");
      break;
    case SENDSPIN_CMD_STOP_PAIRING:
      pairing->close_window();
      break;
    case SENDSPIN_CMD_REQUIRE_PAIRING:
      pairing->set_require(true);
      break;
    case SENDSPIN_CMD_ALLOW_UNPAIRED:
      pairing->set_require(false);
      break;
    case SENDSPIN_CMD_FORGET_PAIRED:
      forget_paired(pairing, store);
      break;
    default:
      break;
  }
}

void loop_task(void *) {
  // Everything here lives as long as the firmware runs.
  sendspin::SendspinClientConfig cfg;
  cfg.name = g_name;
  cfg.product_name = "ESP32-S3 snapclient";
  cfg.software_version = esp_app_get_description()->version;
  cfg.server_port = CONFIG_SENDSPIN_PLAYER_PORT;
  // task stacks that never touch flash go to PSRAM to spare internal RAM.
  // Larger than the library defaults: those leave ~200 B of slack, measured
  // at -Os on the ESP32, and every log line here also runs the UDP logger's
  // vprintf hook (~600 B).
  cfg.httpd_psram_stack = true;
  cfg.httpd_stack_size = 6144;
  cfg.protocol_task_psram_stack = true;
  cfg.protocol_task_stack_size = 9216;
  // Pairing: the static code, shown on General Settings ("device"). Each
  // attempt also needs the pairing window (device button or General
  // Settings).
  const std::string code = load_or_make_pairing_code();
  cfg.static_pairing_code = code;
  cfg.static_pairing_code_locations = {"device"};
  auto *client = new sendspin::SendspinClient(std::move(cfg));

  sendspin::PlayerRoleConfig pcfg;
  pcfg.audio_formats = {
      {sendspin::SendspinCodecFormat::FLAC, kChannels, kSampleRate, kBits},
      {sendspin::SendspinCodecFormat::PCM, kChannels, kSampleRate, kBits},
  };
  // ~5 s of FLAC; the inbound ring sized from it lives in PSRAM
  pcfg.audio_buffer_capacity = 500000;
  pcfg.required_lead_time_ms = kRequiredLeadMs;
  // decode must stay ahead of the output task (14) but needs no more
  pcfg.priority = 12;
  sendspin::PlayerRole &role = client->add_player(std::move(pcfg));

  auto *player = new Player();
  auto *pairing = new Pairing();
  auto *store = new NvsStore();
  auto *net = new Network();
  player->attach(client, &role, &g_output, pairing);

  if (!g_output.init(&role)) {
    vTaskDelete(nullptr);
    return;
  }
  store->open();

  role.set_listener(player);
  client->set_listener(player);
#if CONFIG_SENDSPIN_ENABLE_METADATA
  // roles must be added before start(): they go into the hello
  auto *now_playing = new NowPlaying();
  sendspin::MetadataRole &metadata = client->add_metadata();
  now_playing->attach(&metadata);
  metadata.set_listener(now_playing);
#endif
  client->set_network_provider(net);
  client->set_persistence_provider(store);
  // the build setting, unless General Settings changed it
  const bool unpaired = app_load_u8(kUnpairedKey).value_or(kUnpairedDefault) != 0;
  pairing->init(client, store, code, unpaired != kUnpairedDefault);
  client->set_unpaired_access_enabled(unpaired);

  if (!client->start()) {
    ESP_LOGE(TAG, "Sendspin client did not start");
    vTaskDelete(nullptr);
    return;
  }
  player->restore_volume();
  pairing->refresh_records();
  g_started.store(true);

  mdns_txt_item_t txt[] = {
      {"path", "/sendspin"},
      {"name", g_name.c_str()},
  };
  esp_err_t err = mdns_service_add(g_name.c_str(), "_sendspin", "_tcp",
                                   CONFIG_SENDSPIN_PLAYER_PORT, txt, 2);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mDNS service: %s", esp_err_to_name(err));
  }

  ESP_LOGI(TAG, "Sendspin player \"%s\" on port %d, client id %s, pairing %s",
           g_name.c_str(), CONFIG_SENDSPIN_PLAYER_PORT,
           client->client_id().c_str(), unpaired ? "optional" : "required");

  for (;;) {
    uint8_t cmd;
    while (xQueueReceive(g_cmds, &cmd, 0) == pdTRUE) {
      run_command(cmd, pairing, store);
    }
    client->loop();
    player->poll();
    player->publish_status(client);
    pairing->poll(g_output.is_open());
#if CONFIG_SENDSPIN_ENABLE_METADATA
    now_playing->poll();
#endif
    vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
  }
}

}  // namespace

extern "C" bool sendspin_player_get_status(sendspin_status_t *out) {
  taskENTER_CRITICAL(&g_status_mux);
  *out = g_status;
  taskEXIT_CRITICAL(&g_status_mux);
  return out->running;
}

extern "C" bool sendspin_player_get_now_playing(sendspin_now_playing_t *out) {
  taskENTER_CRITICAL(&g_np_mux);
  *out = g_np;
  taskEXIT_CRITICAL(&g_np_mux);
  return out->valid;
}

extern "C" bool sendspin_player_get_pairing(sendspin_pairing_t *out) {
  taskENTER_CRITICAL(&g_pair_mux);
  *out = g_pair;
  taskEXIT_CRITICAL(&g_pair_mux);
  return g_started.load();
}

extern "C" bool sendspin_player_command(sendspin_cmd_t cmd) {
  if (g_cmds == nullptr || !g_started.load()) {
    return false;
  }
  const uint8_t c = static_cast<uint8_t>(cmd);
  return xQueueSend(g_cmds, &c, 0) == pdTRUE;
}

extern "C" void sendspin_player_start(const char *name) {
  g_name = (name && name[0]) ? name : "snapclient";
  g_cmds = xQueueCreate(4, sizeof(uint8_t));
  // NVS writes happen on this task (persistence provider, volume), so its
  // stack stays in internal RAM
  if (xTaskCreatePinnedToCore(loop_task, "sendspin", 6144, nullptr, 4,
                              nullptr, tskNO_AFFINITY) != pdPASS) {
    ESP_LOGE(TAG, "could not start the Sendspin task");
  }
}

#else  // !CONFIG_SENDSPIN_PLAYER_ENABLE

#include <cstring>

extern "C" void sendspin_player_start(const char *) {}

extern "C" bool sendspin_player_get_status(sendspin_status_t *out) {
  std::memset(out, 0, sizeof(*out));
  return false;
}

extern "C" bool sendspin_player_get_now_playing(sendspin_now_playing_t *out) {
  std::memset(out, 0, sizeof(*out));
  return false;
}

extern "C" bool sendspin_player_get_pairing(sendspin_pairing_t *out) {
  std::memset(out, 0, sizeof(*out));
  return false;
}

extern "C" bool sendspin_player_command(sendspin_cmd_t) { return false; }

#endif
