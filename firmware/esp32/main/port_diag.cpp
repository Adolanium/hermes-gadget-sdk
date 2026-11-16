// Diagnostics for bug reports: a RAM copy of recent log lines that outlives a
// restart, a boot summary, the last crash's core dump, and the port's half of the
// console's `diag` report.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "i2c_scan.hpp"
#include "log_ring.hpp"
#include "sdkconfig.h"

namespace hgp::diag {
namespace {

const char* TAG = "hg.diag";
constexpr size_t kLineBytes = 192;  // longer log lines are cut in the copy, never on the console

// The log copy lives in internal RAM that startup leaves alone, so after a panic, a
// watchdog or a software restart the next boot can still read the lines before it.
// PSRAM would not do: on the ESP32-S3 a reset drops what its cache had not written back.
#if CONFIG_SPIRAM
constexpr size_t kRingBytes = 4096;
#else
constexpr size_t kRingBytes = 2048;
#endif
__NOINIT_ATTR hg::LogRing<kRingBytes> g_ring;
bool g_ready = false;
std::string g_previous;         // the previous boot's lines, when it ended in a restart
const char* g_ended = nullptr;  // how it ended
SemaphoreHandle_t g_lock = nullptr;
SemaphoreHandle_t g_cpu_lock = nullptr;
vprintf_like_t g_next = nullptr;
char g_line[kLineBytes];
Parts g_parts;

// Every ESP_LOG line passes through here on its way to the console.
int capture(const char* fmt, va_list args) {
  // Never wait: a line logged while another task holds the copy still reaches the console.
  if (g_ready && !xPortInIsrContext() && xSemaphoreTake(g_lock, 0) == pdTRUE) {
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
        else g_ring.put(c);
      }
      if (len < static_cast<size_t>(n)) g_ring.put('\n');
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
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
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

// Each task's unused stack, its state, and its share of the CPU since the previous report
// (or since boot), closest to overflowing first. The share is of one core, so on a
// dual-core chip the tasks add up to 200. A starved task shows as "ready" with a small share.
struct CpuMark {
  UBaseType_t task;
  uint64_t ran;
};
std::vector<CpuMark> g_cpu_marks;
uint64_t g_cpu_total = 0;

hg::json::Value stacks(hg::json::Value& r) {
  hg::json::Value out = hg::json::Value::array();
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
  std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
  configRUN_TIME_COUNTER_TYPE total = 0;
  tasks.resize(uxTaskGetSystemState(tasks.data(), tasks.size(), &total));
  std::sort(tasks.begin(), tasks.end(),
            [](const TaskStatus_t& a, const TaskStatus_t& b) { return a.usStackHighWaterMark < b.usStackHighWaterMark; });
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  if (g_cpu_lock) xSemaphoreTake(g_cpu_lock, portMAX_DELAY);
  const uint64_t window = total - std::min<uint64_t>(g_cpu_total, total);
  std::vector<CpuMark> marks;
  marks.reserve(tasks.size());
#endif
  static const char* const kStates[] = {"running", "ready", "blocked", "suspended", "deleted"};
  for (const auto& t : tasks) {
    hg::json::Value o = hg::json::Value::object();
    o.set("name", t.pcTaskName).set("stack_free", static_cast<unsigned>(t.usStackHighWaterMark));
    if (t.eCurrentState <= eDeleted) o.set("state", kStates[t.eCurrentState]);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    uint64_t before = 0;
    for (const auto& m : g_cpu_marks) {
      if (m.task == t.xTaskNumber) before = m.ran;
    }
    const uint64_t ran = t.ulRunTimeCounter;
    if (window) o.set("cpu_pct", static_cast<unsigned>((ran - std::min(before, ran)) * 100 / window));
    marks.push_back({t.xTaskNumber, ran});
#endif
    out.push(o);
  }
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
  r.set("cpu_window_ms", static_cast<long long>(window / 1000));
  g_cpu_marks = std::move(marks);
  g_cpu_total = total;
  if (g_cpu_lock) xSemaphoreGive(g_cpu_lock);
#endif
#endif
  (void)r;
  return out;
}

// The last crash's core dump, in brief. Its addresses name code in the ELF of the build
// that crashed (elf_sha256); docs/troubleshooting.md#read-a-core-dump turns them into lines.
hg::json::Value coredump() {
  hg::json::Value c = hg::json::Value::object();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr)) {
    // Updated over the air from a partition table without one; a USB install adds it.
    c.set("state", "no partition");
    return c;
  }
  if (esp_core_dump_image_check() != ESP_OK) {
    c.set("state", "none");
    return c;
  }
  auto* sum = static_cast<esp_core_dump_summary_t*>(heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_8BIT));
  if (!sum) {
    c.set("state", "no memory to read it");
    return c;
  }
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    const auto hex = [](uint32_t v) {
      char buf[12];
      std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(v));
      return std::string(buf);
    };
    const esp_app_desc_t* app = esp_app_get_description();
    char running[17];
    for (int i = 0; i < 8; ++i) std::snprintf(running + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    // The dump keeps only a prefix of the hash (APP_ELF_SHA256_SZ with its terminator).
    const std::string sha(reinterpret_cast<const char*>(sum->app_elf_sha256),
                          strnlen(reinterpret_cast<const char*>(sum->app_elf_sha256), sizeof(sum->app_elf_sha256)));
    c.set("state", "saved")
        .set("task", std::string(sum->exc_task, strnlen(sum->exc_task, sizeof(sum->exc_task))))
        .set("pc", hex(sum->exc_pc))
        .set("elf_sha256", sha)
        .set("this_build", !sha.empty() && std::string(running).compare(0, sha.size(), sha) == 0);
#if __XTENSA__
    c.set("cause", static_cast<unsigned>(sum->ex_info.exc_cause)).set("vaddr", hex(sum->ex_info.exc_vaddr));
    hg::json::Value bt = hg::json::Value::array();
    for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; ++i) bt.push(hex(sum->exc_bt_info.bt[i]));
    c.set("backtrace", bt).set("backtrace_corrupted", sum->exc_bt_info.corrupted);
#else
    c.set("mcause", hex(sum->ex_info.mcause))
        .set("mtval", hex(sum->ex_info.mtval))
        .set("ra", hex(sum->ex_info.ra))
        .set("sp", hex(sum->ex_info.sp));
#endif
  } else {
    c.set("state", "unreadable");
  }
  heap_caps_free(sum);
#else
  c.set("state", "off in this build");
#endif
  return c;
}

// What both reports carry: nothing here needs the app task or a bus.
void report_common(hg::json::Value& r) {
  const esp_app_desc_t* app = esp_app_get_description();
  char elf[17];
  for (int i = 0; i < 8; ++i) std::snprintf(elf + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
  hg::json::Value build = hg::json::Value::object();
  build.set("idf", app->idf_ver).set("date", app->date).set("time", app->time).set("elf_sha256", elf);
  r.set("build", build)
      .set("uptime_s", static_cast<long long>(esp_timer_get_time() / 1000000))
      .set("reset_reason", reset_reason())
      .set("memory", memory())
      .set("events", events::stats())
      .set("crash", coredump());
  if (g_parts.touch_input) r.set("touch", g_parts.touch_input->stats());
}

}  // namespace

void begin() {
  // The lines from before this boot, unless the chip lost power: then RAM holds noise,
  // which the header check almost always rejects anyway.
  const esp_reset_reason_t why = esp_reset_reason();
  const bool kept = why != ESP_RST_POWERON && why != ESP_RST_BROWNOUT && why != ESP_RST_DEEPSLEEP &&
                    why != ESP_RST_PWR_GLITCH;
  g_previous = g_ring.adopt(kept);
  if (!g_previous.empty()) g_ended = reset_reason();
  g_lock = xSemaphoreCreateMutex();
  g_cpu_lock = xSemaphoreCreateMutex();
  if (!g_lock) {
    ESP_LOGW(TAG, "no memory for the log copy; diag log stays empty");
    return;
  }
  g_ready = true;
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
  ESP_LOGI(TAG, "parts: display %s, microphone %s, speaker %s, touch %s, key %s, imu %s, rtc %s", g_parts.display,
           g_parts.mic, g_parts.speaker, g_parts.touch ? "yes" : "no", g_parts.key ? "yes" : "no", g_parts.imu,
           g_parts.rtc);
  if (!g_previous.empty())
    ESP_LOGI(TAG, "kept %u bytes of log from before the %s", static_cast<unsigned>(g_previous.size()), g_ended);
}

void report(hg::json::Value& r) {
  report_common(r);
  hg::json::Value parts = hg::json::Value::object();
  parts.set("display", g_parts.display)
      .set("microphone", g_parts.mic)
      .set("speaker", g_parts.speaker)
      .set("touch", g_parts.touch)
      .set("key", g_parts.key)
      .set("imu", g_parts.imu)
      .set("rtc", g_parts.rtc);
  r.set("wifi", wifi()).set("parts", parts).set("i2c", i2c_scan());
  r.set("stacks", stacks(r));
  if (g_parts.motion) {
    int16_t x = 0, y = 0, z = 0;
    g_parts.motion->latest(x, y, z);
    hg::json::Value motion = hg::json::Value::object();
    motion.set("x", x).set("y", y).set("z", z);
    r.set("motion", motion);
  }
  if (g_parts.time) r.set("time", g_parts.time->diag());
}

std::string report_without_app() {
  hg::json::Value r = hg::json::Value::object();
  r.set("app", "not answering");
  report_common(r);
  r.set("stacks", stacks(r));
  return "@diag " + r.dump();
}

std::string recent_log() {
  std::string out;
  if (!g_previous.empty()) {
    out = "@log previous boot, ended by " + std::string(g_ended) + "\n" + g_previous;
    if (out.back() != '\n') out += '\n';
    out += "@log this boot\n";
  }
  if (!g_ready || xSemaphoreTake(g_lock, pdMS_TO_TICKS(200)) != pdTRUE) return out;
  out += g_ring.contents();
  xSemaphoreGive(g_lock);
  return out;
}

}  // namespace hgp::diag
