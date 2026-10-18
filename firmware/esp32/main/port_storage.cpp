#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <string>
#include <vector>

#include "esp_log.h"
#include "nvs.h"

namespace hgp {
namespace {

const char* TAG = "hg.nvs";
constexpr const char* kNamespace = "hgadget";

// NVS keys are limited to 15 characters; the core stores longer setting names
// under short keys (see setting_storage_key in app.cpp). A longer
// key is refused rather than cut, so two settings can never share a slot.
bool key_of(std::string_view key, std::string& out) {
  if (key.empty() || key.size() > NVS_KEY_NAME_MAX_SIZE - 1) {
    ESP_LOGE(TAG, "key '%.*s' is not 1..%d characters", static_cast<int>(key.size()), key.data(), NVS_KEY_NAME_MAX_SIZE - 1);
    return false;
  }
  out.assign(key);
  return true;
}

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
  std::string k;
  if (!key_of(key, k)) return std::nullopt;
  size_t len = 0;
  if (nvs_get_str(handle_, k.c_str(), nullptr, &len) != ESP_OK || len == 0) return std::nullopt;
  std::vector<char> buf(len);
  if (nvs_get_str(handle_, k.c_str(), buf.data(), &len) != ESP_OK) return std::nullopt;
  return std::string(buf.data());
}

bool NvsStorage::set(std::string_view key, std::string_view value) {
  Lock l(lock_);
  std::string k, v(value);
  if (!key_of(key, k)) return false;
  esp_err_t err = nvs_set_str(handle_, k.c_str(), v.c_str());
  if (err == ESP_OK) err = nvs_commit(handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "saving %s failed: %s", k.c_str(), esp_err_to_name(err));
    return false;
  }
  return true;
}

void NvsStorage::erase(std::string_view key) {
  Lock l(lock_);
  std::string k;
  if (!key_of(key, k)) return;
  if (nvs_erase_key(handle_, k.c_str()) == ESP_OK) nvs_commit(handle_);
}

}  // namespace hgp
