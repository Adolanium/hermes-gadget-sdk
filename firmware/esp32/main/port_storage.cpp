#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <string>
#include <vector>

#include "esp_log.h"
#include "nvs.h"

namespace hgp {
namespace {

const char* TAG = "hg.nvs";
constexpr const char* kNamespace = "hgadget";

// NVS keys are limited to 15 characters; every core setting key fits.
std::string key_of(std::string_view key) { return std::string(key.substr(0, NVS_KEY_NAME_MAX_SIZE - 1)); }

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};

}  // namespace

bool NvsStorage::begin() {
  lock_ = xSemaphoreCreateMutex();
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return false;
  }
  handle_ = h;
  return true;
}

std::optional<std::string> NvsStorage::get(std::string_view key) {
  Lock l(lock_);
  std::string k = key_of(key);
  size_t len = 0;
  if (nvs_get_str(handle_, k.c_str(), nullptr, &len) != ESP_OK || len == 0) return std::nullopt;
  std::vector<char> buf(len);
  if (nvs_get_str(handle_, k.c_str(), buf.data(), &len) != ESP_OK) return std::nullopt;
  return std::string(buf.data());
}

void NvsStorage::set(std::string_view key, std::string_view value) {
  Lock l(lock_);
  std::string k = key_of(key), v(value);
  if (nvs_set_str(handle_, k.c_str(), v.c_str()) == ESP_OK) nvs_commit(handle_);
}

void NvsStorage::erase(std::string_view key) {
  Lock l(lock_);
  std::string k = key_of(key);
  if (nvs_erase_key(handle_, k.c_str()) == ESP_OK) nvs_commit(handle_);
}

}  // namespace hgp
