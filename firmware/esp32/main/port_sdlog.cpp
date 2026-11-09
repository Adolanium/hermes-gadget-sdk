// See port_sdlog.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_sdlog.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

namespace hgp {
namespace {

constexpr const char* TAG = "hg.sdlog";
constexpr const char* kMountPoint = "/sdcard";
constexpr const char* kPath = "/sdcard/hermes.log";
constexpr const char* kRotatedPath = "/sdcard/hermes.log.1";
constexpr size_t kRingBytes = 16 * 1024;
constexpr size_t kLineBytes = 256;  // longer log lines are cut, never on the console
constexpr size_t kRotateBytes = 512 * 1024;

char* g_ring = nullptr;
size_t g_cap = 0, g_head = 0, g_tail = 0;  // next byte to write / read
bool g_wrapped = false;
SemaphoreHandle_t g_lock = nullptr;
vprintf_like_t g_next = nullptr;
volatile bool g_capture = false;
char g_line[kLineBytes];

void ring_put(char c) {
  if (g_head == g_cap) return;  // full: drop, counted by the caller
  g_ring[g_head] = c;
  if (++g_head == g_cap) {
    g_head = 0;
    g_wrapped = true;
  }
}

size_t ring_left() {
  return g_wrapped ? g_cap : g_head - g_tail;
}

}  // namespace

int SdLogger::hook(const char* fmt, va_list args) {
  // Like diag capture: never wait, never in an ISR, never block the console.
  if (g_capture && g_ring && !xPortInIsrContext() && xSemaphoreTake(g_lock, 0) == pdTRUE) {
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(g_line, sizeof(g_line), fmt, copy);
    va_end(copy);
    if (n > 0) {
      size_t len = std::min(static_cast<size_t>(n), sizeof(g_line) - 1);
      for (size_t i = 0; i < len; ++i) {
        if (g_head == g_cap) break;  // ring full: the rest of the line is dropped
        ring_put(g_line[i]);
      }
      if (len < static_cast<size_t>(n) && g_head != g_cap) ring_put('\n');
    }
    xSemaphoreGive(g_lock);
  }
  return g_next ? g_next(fmt, args) : std::vprintf(fmt, args);
}

void SdLogger::mount() {
  if (mounted_ || !enabled_) return;
  // Slot 0 for the card: slot 1 carries the C6 companion's SDIO link
  // (CONFIG_ESP_HOSTED_SDIO_SLOT_1), and SDMMC_HOST_DEFAULT() also picks 1.
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = static_cast<gpio_num_t>(sd_.clk);
  slot.cmd = static_cast<gpio_num_t>(sd_.cmd);
  slot.d0 = static_cast<gpio_num_t>(sd_.d0);
  slot.d1 = static_cast<gpio_num_t>(sd_.d1);
  slot.d2 = static_cast<gpio_num_t>(sd_.d2);
  slot.d3 = static_cast<gpio_num_t>(sd_.d3);
  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = false;  // never format the user's card
  mount_cfg.max_files = 2;
  sdmmc_card_t* card = nullptr;
  if (esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card) != ESP_OK) {
    // No card, or not FAT32: stay silent and retry from the flush task.
    return;
  }
  mounted_ = true;
  ESP_LOGI(TAG, "mounted %s (%s)", kMountPoint, card->cid.name);
  open_file();
}

void SdLogger::open_file() {
  if (!mounted_ || file_) return;
  struct stat st = {};
  if (stat(kPath, &st) == 0 && st.st_size > static_cast<off_t>(kRotateBytes)) {
    remove(kRotatedPath);
    rename(kPath, kRotatedPath);
  }
  file_ = fopen(kPath, "a");
  if (!file_) {
    ESP_LOGW(TAG, "could not open %s; logging stays in RAM only", kPath);
    return;
  }
  const esp_app_desc_t* app = esp_app_get_description();
  std::fprintf(file_, "== hermes gadget %s on %s, IDF %s ==\n", app->version, app->project_name, app->idf_ver);
  if (dropped_) std::fprintf(file_, "(%d log lines dropped before this file)\n", dropped_);
}

void SdLogger::flush() {
  if (!enabled_) return;
  if (!mounted_) mount();
  if (!file_) return;
  if (xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
  // Drain a snapshot of the ring; the hook keeps appending behind it.
  static char drain[kRingBytes];
  size_t n = 0;
  while (g_tail != g_head || g_wrapped) {
    if (n == sizeof(drain)) break;
    drain[n++] = g_ring[g_tail];
    g_tail = (g_tail + 1) % g_cap;
    if (g_tail == g_head) {
      g_wrapped = false;
      break;
    }
    if (!g_wrapped && g_tail == g_head) break;
  }
  if (g_wrapped) {  // lapped: the oldest bytes are gone either way
    g_tail = g_head;
    g_wrapped = false;
  }
  xSemaphoreGive(g_lock);
  if (n) {
    std::fwrite(drain, 1, n, file_);
    std::fflush(file_);
  }
}

void SdLogger::task(void* arg) {
  auto* self = static_cast<SdLogger*>(arg);
  uint32_t last = 0;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));
    self->flush();
    // Retry the mount about every 30 s until a card shows up.
    if (!self->mounted_ && xTaskGetTickCount() - last > pdMS_TO_TICKS(30000)) {
      last = xTaskGetTickCount();
      self->mount();
    }
  }
}

void SdLogger::begin(const SdConfig& sd, bool enabled) {
  sd_ = sd;
  enabled_ = enabled;
  if (!sd_.enabled) return;
  size_t cap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? kRingBytes : kRingBytes / 2;
  g_ring = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g_ring) g_ring = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_8BIT));
  g_lock = xSemaphoreCreateMutex();
  if (!g_ring || !g_lock) {
    ESP_LOGW(TAG, "no memory for the SD log; logging stays off");
    g_ring = nullptr;
    return;
  }
  g_cap = cap;
  g_next = esp_log_set_vprintf(&SdLogger::hook);
  g_capture = enabled_;
  mount();
  xTaskCreate(&SdLogger::task, "sdlog", 3072, this, 2, nullptr);
  if (enabled_) ESP_LOGI(TAG, "SD logging on");
  else ESP_LOGI(TAG, "SD logging off (sd_log setting)");
}

void SdLogger::set_enabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  g_capture = enabled_;
  if (!enabled_ && file_) {
    flush();
    std::fclose(file_);
    file_ = nullptr;
  } else if (enabled_) {
    mount();
    open_file();
  }
  if (enabled_) ESP_LOGI(TAG, "SD logging on");
  else ESP_LOGI(TAG, "SD logging off");
}

}  // namespace hgp
