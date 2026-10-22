// The chip's own 8-bit DAC as a speaker: how a classic ESP32 drives a small
// amplifier with no extra parts. The CYD's SC8002B is wired that way, on GPIO26
// (DAC channel 1), so this plays the boot beeps that show the amplifier and the
// speaker work before the port has an audio path for replies.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include "esp_log.h"
#include "freertos/task.h"
#if SOC_DAC_SUPPORTED
#include "driver/dac_cosine.h"
#endif

namespace hgp {
namespace {

const char* TAG = "hg.dac";
constexpr uint32_t kBeepHz = 1000;  // well above the generator's ~130 Hz floor
constexpr uint32_t kBeepMs = 120;
constexpr uint32_t kGapMs = 120;
constexpr int kBeeps = 3;  // three, so a missed one is obvious

}  // namespace

bool dac_beep(int gpio) {
#if SOC_DAC_SUPPORTED
  // Only two pins reach the DAC: GPIO25 is channel 0 and GPIO26 is channel 1.
  if (gpio != 25 && gpio != 26) {
    if (gpio >= 0) ESP_LOGW(TAG, "GPIO%d is not a DAC output; no boot beep", gpio);
    return false;
  }
  const auto channel = gpio == 25 ? DAC_CHAN_0 : DAC_CHAN_1;
  dac_cosine_config_t cfg = {};
  cfg.chan_id = channel;
  cfg.freq_hz = kBeepHz;
  cfg.clk_src = DAC_COSINE_CLK_SRC_DEFAULT;  // the generator runs off RC_FAST
  cfg.atten = DAC_COSINE_ATTEN_DB_6;         // half amplitude: audible without being shrill
  cfg.phase = DAC_COSINE_PHASE_0;
  cfg.offset = 0;
  dac_cosine_handle_t handle = nullptr;
  if (!esp_ok(dac_cosine_new_channel(&cfg, &handle), TAG, "dac_cosine_new_channel")) return false;
  for (int i = 0; i < kBeeps; ++i) {
    if (!esp_ok(dac_cosine_start(handle), TAG, "dac_cosine_start")) break;
    vTaskDelay(pdMS_TO_TICKS(kBeepMs));
    dac_cosine_stop(handle);
    if (i + 1 < kBeeps) vTaskDelay(pdMS_TO_TICKS(kGapMs));
  }
  dac_cosine_del_channel(handle);
  ESP_LOGI(TAG, "three beeps on GPIO%d (DAC channel %d)", gpio, static_cast<int>(channel));
  return true;
#else
  (void)gpio;
  return false;
#endif
}

}  // namespace hgp
