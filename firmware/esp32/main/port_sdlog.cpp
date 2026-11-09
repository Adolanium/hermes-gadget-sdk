// See port_sdlog.hpp.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h
#include "port_sdlog.hpp"

#include <algorithm>
#include <atomic>
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
size_t g_used = 0;                          // bytes currently in the ring (exact)
std::atomic<size_t> g_dropped_lines{0};     // lines lost to a full ring, or skipped while a flush held the ring
sdmmc_card_t* g_card = nullptr;             // the mounted card, for unmounting after a failed write
SemaphoreHandle_t g_lock = nullptr;
vprintf_like_t g_next = nullptr;
std::atomic<bool> g_capture{false};
char g_line[kLineBytes];

bool ring_put(char c) {
  if (g_used == g_cap) return false;  // full: drop, counted by the caller
  g_ring[g_head] = c;
  if (++g_head == g_cap) g_head = 0;
  ++g_used;
  return true;
}

}  // namespace

int SdLogger::hook(const char* fmt, va_list args) {
  // Like diag capture: never wait, never in an ISR, never block the console.
  if (g_capture.load() && g_ring && !xPortInIsrContext() && xSemaphoreTake(g_lock, 0) != pdTRUE) {
    ++g_dropped_lines;  // a flush holds the ring: count the line, still never wait
  } else if (g_capture.load() && g_ring && !xPortInIsrContext()) {
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(g_line, sizeof(g_line), fmt, copy);
    va_end(copy);
    if (n > 0) {
      size_t len = std::min(static_cast<size_t>(n), sizeof(g_line) - 1);
      bool dropped = false;
      bool escape = false;  // drop colour codes
      for (size_t i = 0; i < len; ++i) {
        char c = g_line[i];
        if (c == '\x1b') escape = true;
        else if (escape) escape = c != 'm';
        else if (!ring_put(c)) dropped = true;
      }
      if (dropped) ++g_dropped_lines;
      else if (len < static_cast<size_t>(n)) ring_put('\n');  // truncated lines still end
    }
    xSemaphoreGive(g_lock);
  }
  return g_next ? g_next(fmt, args) : std::vprintf(fmt, args);
}

void SdLogger::mount() {
  if (mounted_ || !g_capture.load()) return;
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
  // No card is the common case: keep IDF's sdmmc tags quiet around the attempt.
  esp_log_level_set("sdmmc_cmd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_sd", ESP_LOG_NONE);
  esp_log_level_set("sdmmc_periph", ESP_LOG_NONE);
  sdmmc_card_t* card = nullptr;
  const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &slot, &mount_cfg, &card);
  esp_log_level_set("sdmmc_cmd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
  esp_log_level_set("sdmmc_periph", ESP_LOG_WARN);
  if (err != ESP_OK) return;  // retry from the task; stay silent
  mounted_ = true;
  g_card = card;
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
  // The rotation cap counts the whole file, not just this session's bytes.
  written_ = stat(kPath, &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
  const esp_app_desc_t* app = esp_app_get_description();
  std::fprintf(file_, "== hermes gadget %s on %s, IDF %s ==\n", app->version, app->project_name, app->idf_ver);
  if (const size_t dropped = g_dropped_lines.exchange(0)) {  // reported once; later files start clean
    std::fprintf(file_, "(%u log lines dropped before this file)\n", static_cast<unsigned>(dropped));
  }
}

void SdLogger::flush() {
  // Mount lives in task(), not here: probing the card every logger pass
  // would retry far more often than the 30 s budget.
  if (!file_) return;
  if (xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
  // Drain a snapshot of the ring; the hook keeps appending behind it.
  static char drain[kRingBytes];
  size_t n = 0;
  while (g_used && n < sizeof(drain)) {
    drain[n++] = g_ring[g_tail];
    g_tail = (g_tail + 1) % g_cap;
    --g_used;
  }
  xSemaphoreGive(g_lock);
  if (!n) return;
  if (std::fwrite(drain, 1, n, file_) != n || std::fflush(file_) != 0) {
    // The card was pulled or failed: drop the stale mount so the task mounts it again
    // when a card answers. The bytes in this batch are lost either way.
    std::fclose(file_);
    file_ = nullptr;
    esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
    g_card = nullptr;
    mounted_ = false;
    ESP_LOGW(TAG, "write to the card failed; unmounted, retrying the card every 30 s");
    return;
  }
  written_ += n;
  // Rotate while running, not just at open: a long session would otherwise
  // grow past the cap until the card fills.
  if (written_ > kRotateBytes) {
    std::fclose(file_);
    file_ = nullptr;
    open_file();
  }
}

void SdLogger::task(void* arg) {
  auto* self = static_cast<SdLogger*>(arg);
  uint32_t last = 0;
  bool was_on = false;
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(500));
    // The task owns the file: it opens, closes, mounts and rotates, so the
    // app task's set_enabled() only flips the flag and can never race an
    // fwrite or a double mount.
    if (!g_capture.load()) {
      if (self->file_) {
        self->flush();  // drains what was captured before the setting went off
        if (self->file_) std::fclose(self->file_);  // the flush can rotate, or drop a failed card
        self->file_ = nullptr;
      }
      was_on = false;
      continue;
    }
    // Switched on: try the card now, not at the next 30 s mark. Still mounted from
    // before it was switched off: reopen the file mount() opened then.
    const bool just_on = !was_on;
    was_on = true;
    const bool due = just_on || xTaskGetTickCount() - last > pdMS_TO_TICKS(30000);
    if (!self->mounted_ && due) {
      last = xTaskGetTickCount();
      self->mount();  // retried about every 30 s until a card shows up
    } else if (self->mounted_ && !self->file_ && due) {
      last = xTaskGetTickCount();
      self->open_file();  // retried about every 30 s if the open failed
    }
    self->flush();
  }
}

void SdLogger::begin(const SdConfig& sd, bool enabled) {
  sd_ = sd;
  if (!sd_.enabled) return;  // no slot: no hook, no task, nothing
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
  g_capture.store(enabled);
  xTaskCreate(&SdLogger::task, "sdlog", 3072, this, 2, nullptr);
  if (enabled) ESP_LOGI(TAG, "SD logging on");
  else ESP_LOGI(TAG, "SD logging off (sd_log setting)");
}

void SdLogger::set_enabled(bool enabled) {
  // Flag only: the sdlog task opens, closes, mounts and flushes.
  if (g_capture.exchange(enabled) == enabled) return;
  if (enabled) ESP_LOGI(TAG, "SD logging on");
  else ESP_LOGI(TAG, "SD logging off");
}

}  // namespace hgp
