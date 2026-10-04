// Over-the-air updates through ESP-IDF's OTA API, with rollback: the image goes
// into the app slot that isn't running, and the bootloader keeps the old one
// until the new one has proven it can reach Hermes.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace hgp {
namespace {

const char* TAG = "hg.ota";
// A new image has this long to reach Hermes. It needs Wi-Fi, the server and a
// handshake; a gateway restarting at the same moment fits in it too.
constexpr int64_t kConfirmWindowUs = 5LL * 60 * 1000 * 1000;

const esp_partition_t* as_partition(const void* p) { return static_cast<const esp_partition_t*>(p); }

void roll_back(void*) {
  ESP_LOGE(TAG, "the new firmware didn't reach Hermes within 5 minutes; going back to the previous one");
  esp_ota_mark_app_invalid_rollback_and_reboot();
  ESP_LOGE(TAG, "can't roll back: there is no previous firmware to go back to");  // only returns then
}

}  // namespace

void EspUpdater::start() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return;
  pending_ = true;
  esp_timer_create_args_t args = {};
  args.callback = &roll_back;
  args.name = "hg-rollback";
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, kConfirmWindowUs) == ESP_OK) {
    rollback_timer_ = timer;
  }
  ESP_LOGW(TAG, "running new firmware %s from %s: it must reach Hermes within 5 minutes, or the previous one comes back",
           esp_app_get_description()->version, running->label);
}

size_t EspUpdater::capacity() const {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next ? next->size : 0;
}

bool EspUpdater::begin(size_t size, std::string& error) {
  abort();
  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (!target) {
    error = "this partition table has no update slot; flash the firmware over USB once";
    return false;
  }
  if (size > target->size) {
    error = "the image doesn't fit the update slot";
    return false;
  }
  esp_ota_handle_t handle = 0;
  // Sequential writes erase each sector as it is reached, so begin() returns at once.
  esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
  if (err != ESP_OK) {
    error = std::string("can't start the update: ") + esp_err_to_name(err);
    return false;
  }
  target_ = target;
  handle_ = handle;
  open_ = true;
  written_ = 0;
  ESP_LOGI(TAG, "writing %u bytes to %s", static_cast<unsigned>(size), target->label);
  return true;
}

bool EspUpdater::write(const uint8_t* data, size_t len, std::string& error) {
  if (!open_) {
    error = "no update in progress";
    return false;
  }
  // The first bytes say what the image is: refuse anything but this firmware.
  if (written_ < kHeadBytes) {
    size_t n = std::min(len, kHeadBytes - written_);
    std::memcpy(head_ + written_, data, n);
    if (written_ + n == kHeadBytes) {
      uint32_t magic = 0;
      std::memcpy(&magic, head_ + 32, sizeof(magic));
      char project[33] = {};
      std::memcpy(project, head_ + 80, 32);
      if (head_[0] != ESP_IMAGE_HEADER_MAGIC || magic != ESP_APP_DESC_MAGIC_WORD) {
        error = "not an ESP32 app image";
        return false;
      }
      if (std::strncmp(project, esp_app_get_description()->project_name, sizeof(project) - 1) != 0) {
        error = std::string("the image is ") + project + ", not this firmware";
        return false;
      }
    }
  }
  esp_err_t err = esp_ota_write(static_cast<esp_ota_handle_t>(handle_), data, len);
  if (err != ESP_OK) {
    error = std::string("writing to flash failed: ") + esp_err_to_name(err);
    return false;
  }
  written_ += len;
  return true;
}

bool EspUpdater::finish(std::string& error) {
  if (!open_) {
    error = "no update in progress";
    return false;
  }
  open_ = false;
  // Checks the image's segments and its appended SHA-256 before anything changes.
  esp_err_t err = esp_ota_end(static_cast<esp_ota_handle_t>(handle_));
  if (err != ESP_OK) {
    error = err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image failed validation" : esp_err_to_name(err);
    return false;
  }
  err = esp_ota_set_boot_partition(as_partition(target_));
  if (err != ESP_OK) {
    error = std::string("can't select the new firmware: ") + esp_err_to_name(err);
    return false;
  }
  ESP_LOGI(TAG, "installed %u bytes in %s; it boots next", static_cast<unsigned>(written_), as_partition(target_)->label);
  return true;
}

void EspUpdater::abort() {
  if (!open_) return;
  esp_ota_abort(static_cast<esp_ota_handle_t>(handle_));
  open_ = false;
}

void EspUpdater::restart() {
  ESP_LOGI(TAG, "restarting into the new firmware");
  esp_restart();
}

void EspUpdater::confirm() {
  if (!pending_) return;
  if (rollback_timer_) {
    auto timer = static_cast<esp_timer_handle_t>(rollback_timer_);
    esp_timer_stop(timer);
    esp_timer_delete(timer);
    rollback_timer_ = nullptr;
  }
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  pending_ = false;
  if (err == ESP_OK) ESP_LOGI(TAG, "kept the new firmware");
  else ESP_LOGE(TAG, "can't mark the new firmware valid: %s", esp_err_to_name(err));
}

hg::json::Value EspUpdater::describe() const {
  hg::json::Value d = hg::json::Value::object();
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  d.set("running", running ? running->label : "?").set("next", next ? next->label : "none")
      .set("slot_size", next ? next->size : 0u).set("probation", pending_);
  return d;
}

}  // namespace hgp
