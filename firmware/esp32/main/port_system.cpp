#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <atomic>
#include <string>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

namespace hgp {

uint32_t EspSystem::now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void EspSystem::random_bytes(uint8_t* out, size_t len) {
  // Hardware RNG; entropy is good once the radio is up, and the device key is
  // generated after Wi-Fi starts (see main.cpp).
  esp_fill_random(out, len);
}

void EspSystem::log(hg::LogLevel level, std::string_view message) {
  static const char* TAG = "hg";
  std::string msg(message);
  switch (level) {
    case hg::LogLevel::Debug: ESP_LOGD(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Info: ESP_LOGI(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Warn: ESP_LOGW(TAG, "%s", msg.c_str()); break;
    case hg::LogLevel::Error: ESP_LOGE(TAG, "%s", msg.c_str()); break;
  }
}

namespace {
// Light sleep taken since the doze began, for the log line that ends it.
std::atomic<int64_t> g_slept_us{0};
std::atomic<uint32_t> g_sleeps{0};
std::atomic<bool> g_dozing{false};

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
esp_err_t count_sleep(int64_t slept_us, void*) {
  g_slept_us.fetch_add(slept_us, std::memory_order_relaxed);
  g_sleeps.fetch_add(1, std::memory_order_relaxed);
  return ESP_OK;
}
#endif
}  // namespace

bool EspSystem::enable_doze() {
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
  if (awake_) return true;
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = &count_sleep;
  esp_pm_light_sleep_register_cbs(&cbs);
#endif
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "hg-awake", &awake_) != ESP_OK) {
    awake_ = nullptr;
    return false;
  }
  esp_pm_lock_acquire(awake_);
  return true;
#else
  return false;
#endif
}

bool EspSystem::dozing_now() { return g_dozing.load(std::memory_order_relaxed); }

void EspSystem::set_dozing(bool dozing) {
  if (!awake_ || dozing == dozing_) return;
  dozing_ = dozing;
  if (dozing) {
    g_slept_us = 0;
    g_sleeps = 0;
    doze_started_us_ = esp_timer_get_time();
    if (on_doze_start) on_doze_start();
    g_dozing = true;
    ESP_LOGI("hg.power", "dozing: light sleep allowed");
    esp_pm_lock_release(awake_);
    return;
  }
  esp_pm_lock_acquire(awake_);
  g_dozing = false;
  if (on_doze_end) on_doze_end();
  const int64_t dozed_ms = (esp_timer_get_time() - doze_started_us_) / 1000;
  const int64_t slept_ms = g_slept_us.load() / 1000;
  ESP_LOGI("hg.power", "awake after %lld s dozing: light sleep %lld s (%d%%), %u wake-ups", dozed_ms / 1000,
           slept_ms / 1000, dozed_ms > 0 ? static_cast<int>(slept_ms * 100 / dozed_ms) : 0,
           static_cast<unsigned>(g_sleeps.load()));
}

}  // namespace hgp
