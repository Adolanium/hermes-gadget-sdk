// Diagnostics for bug reports: a RAM copy of recent log lines, a boot summary,
// and the port's half of the console's `diag` report.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "i2c_scan.hpp"
#include "sdkconfig.h"

namespace hgp::diag {
namespace {

const char* TAG = "hg.diag";
constexpr size_t kLineBytes = 192;  // longer log lines are cut in the copy, never on the console

char* g_ring = nullptr;
size_t g_cap = 0;
size_t g_head = 0;  // next byte to write
bool g_wrapped = false;
SemaphoreHandle_t g_lock = nullptr;
vprintf_like_t g_next = nullptr;
char g_line[kLineBytes];
Parts g_parts;

void ring_put(char c) {
  g_ring[g_head] = c;
  if (++g_head == g_cap) {
    g_head = 0;
    g_wrapped = true;
  }
}

// Every ESP_LOG line passes through here on its way to the console.
int capture(const char* fmt, va_list args) {
  // Never wait: a line logged while another task holds the copy still reaches the console.
  if (g_ring && !xPortInIsrContext() && xSemaphoreTake(g_lock, 0) == pdTRUE) {
    va_list copy;
    va_copy(copy, args);
    int n = std::vsnprintf(g_line, sizeof(g_line), fmt, copy);
    va_end(copy);
    if (n > 0) {
      size_t len = std::min(static_cast<size_t>(n), sizeof(g_line) - 1);
      bool escape = false;  // drop colour codes
      for (size_t i = 0; i < len; ++i) {
        char c = g_line[i];
        if (c == '\x1b') escape = true;
        else if (escape) escape = c != 'm';
        else ring_put(c);
      }
      if (len < static_cast<size_t>(n)) ring_put('\n');
    }
    xSemaphoreGive(g_lock);
  }
  return g_next ? g_next(fmt, args) : std::vprintf(fmt, args);
}

const char* reset_reason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "other";
  }
}

unsigned kb(size_t bytes) { return static_cast<unsigned>(bytes / 1024); }

hg::json::Value memory() {
  hg::json::Value m = hg::json::Value::object();
  m.set("internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL))
      .set("internal_min_free", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL))
      .set("internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
      .set("dma_free", heap_caps_get_free_size(MALLOC_CAP_DMA))
      .set("dma_largest", heap_caps_get_largest_free_block(MALLOC_CAP_DMA))
      .set("psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM))
      .set("psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  return m;
}

hg::json::Value wifi() {
  hg::json::Value w = hg::json::Value::object();
  wifi_ap_record_t ap = {};
  bool joined = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  w.set("joined", joined);
  if (joined) {
    w.set("ssid", reinterpret_cast<const char*>(ap.ssid)).set("rssi", ap.rssi).set("channel", ap.primary);
  }
  esp_netif_ip_info_t ip = {};
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    w.set("ip", buf);
  }
  return w;
}

// Which addresses answer on the board's I2C bus: the quickest way to tell a
// dead or unpowered chip from a driver problem.
hg::json::Value i2c_scan() {
  hg::json::Value found = hg::json::Value::array();
  if (!g_parts.i2c) return found;
  struct Ops {
    i2c_master_bus_handle_t bus;
    hg::i2c_scan::Probe probe(uint16_t addr) {
      // One limit covers the wait for the bus and for the transfer. 50 ms as in
      // the drivers: at the 100 Hz tick, 10 ms is a single tick, which can run
      // out mid-transfer.
      esp_err_t err = i2c_master_probe(bus, addr, 50);
      if (err == ESP_OK) return hg::i2c_scan::Probe::Ack;
      return err == ESP_ERR_TIMEOUT ? hg::i2c_scan::Probe::Timeout : hg::i2c_scan::Probe::Nack;
    }
    void pause() { vTaskDelay(pdMS_TO_TICKS(20)); }
  } ops{g_parts.i2c};
  for (const std::string& entry : hg::i2c_scan::scan(ops)) found.push(entry);
  return found;
}

hg::json::Value stacks() {
  hg::json::Value out = hg::json::Value::array();
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
  tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), nullptr));
  // Closest to overflowing first.
  std::sort(tasks.begin(), tasks.end(),
            [](const TaskStatus_t& a, const TaskStatus_t& b) { return a.usStackHighWaterMark < b.usStackHighWaterMark; });
  for (const auto& t : tasks) {
    hg::json::Value o = hg::json::Value::object();
    o.set("name", t.pcTaskName).set("stack_free", static_cast<unsigned>(t.usStackHighWaterMark));
    out.push(o);
  }
#endif
  return out;
}

}  // namespace

void begin() {
  // A few KB of history is enough for a first boot and the minutes before a fault.
  size_t cap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? 8 * 1024 : 2 * 1024;
  g_ring = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!g_ring) g_ring = static_cast<char*>(heap_caps_malloc(cap, MALLOC_CAP_8BIT));
  g_lock = xSemaphoreCreateMutex();
  if (!g_ring || !g_lock) {
    ESP_LOGW(TAG, "no memory for the log copy; diag log stays empty");
    g_ring = nullptr;
    return;
  }
  g_cap = cap;
  g_next = esp_log_set_vprintf(&capture);
}

void set_parts(const Parts& parts) { g_parts = parts; }

void log_boot_summary() {
  const esp_app_desc_t* app = esp_app_get_description();
  ESP_LOGI(TAG, "reset reason: %s; built %s %s with ESP-IDF %s", reset_reason(), app->date, app->time,
           app->idf_ver);
  ESP_LOGI(TAG, "memory: internal %u KB free (largest block %u KB), DMA %u KB, PSRAM %u KB of %u KB",
           kb(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), kb(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           kb(heap_caps_get_free_size(MALLOC_CAP_DMA)), kb(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           kb(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
  ESP_LOGI(TAG, "parts: display %s, microphone %s, speaker %s, touch %s, key %s", g_parts.display, g_parts.mic,
           g_parts.speaker, g_parts.touch ? "yes" : "no", g_parts.key ? "yes" : "no");
}

void report(hg::json::Value& r) {
  const esp_app_desc_t* app = esp_app_get_description();
  char elf[17];
  for (int i = 0; i < 8; ++i) std::snprintf(elf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
  hg::json::Value build = hg::json::Value::object();
  build.set("idf", app->idf_ver).set("date", app->date).set("time", app->time).set("elf_sha256", elf);
  hg::json::Value parts = hg::json::Value::object();
  parts.set("display", g_parts.display)
      .set("microphone", g_parts.mic)
      .set("speaker", g_parts.speaker)
      .set("touch", g_parts.touch)
      .set("key", g_parts.key);
  r.set("build", build)
      .set("uptime_s", static_cast<long long>(esp_timer_get_time() / 1000000))
      .set("reset_reason", reset_reason())
      .set("memory", memory())
      .set("wifi", wifi())
      .set("parts", parts)
      .set("i2c", i2c_scan())
      .set("stacks", stacks());
}

std::string recent_log() {
  if (!g_ring || xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return {};
  std::string out;
  if (g_wrapped) out.assign(g_ring + g_head, g_cap - g_head);
  out.append(g_ring, g_head);
  xSemaphoreGive(g_lock);
  if (g_wrapped) {
    size_t nl = out.find('\n');  // the oldest line lost its start
    out.erase(0, nl == std::string::npos ? 0 : nl + 1);
  }
  return out;
}

}  // namespace hgp::diag
