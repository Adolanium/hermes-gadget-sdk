// Wall time for the gadget. The PCF85063 seeds the system clock at boot (it
// holds UTC; the conversion uses days-from-civil, never mktime, which would
// apply the local timezone). The `timezone` setting (a POSIX TZ string,
// default UTC0) is applied through TZ + tzset(). SNTP (pool.ntp.org) sets the
// clock while the clock face is wanted and the network is up; its callback
// posts a TimeSynced event and the app task writes the RTC back.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"

namespace hgp {
namespace {

const char* TAG = "hg.time";

}  // namespace

void TimePort::begin(const RtcConfig& cfg, i2c_master_bus_handle_t bus, hg::Storage& storage) {
  if (bus && cfg.enabled) {
    if (i2c_master_probe(bus, cfg.addr, 50) == ESP_OK) {
      i2c_device_config_t dev = {};
      dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
      dev.device_address = cfg.addr;
      dev.scl_speed_hz = 400000;
      if (i2c_master_bus_add_device(bus, &dev, &dev_) == ESP_OK) {
        chip_ = std::make_unique<hg::Pcf85063>(
            [this](uint8_t reg, uint8_t* data, size_t size) {
              return i2c_master_transmit_receive(dev_, &reg, 1, data, size, 50) == ESP_OK;
            },
            [this](uint8_t reg, const uint8_t* data, size_t size) {
              uint8_t buf[8];  // register address + the 7-byte time table
              if (size > sizeof(buf) - 1) return false;
              buf[0] = reg;
              for (size_t i = 0; i < size; ++i) buf[i + 1] = data[i];
              return i2c_master_transmit(dev_, buf, size + 1, 50) == ESP_OK;
            });
        hg::WallTime utc{};
        if (chip_->read(utc)) {
          const int64_t secs = hg::days_from_civil(utc.year, utc.month, utc.day) * 86400 +
                               utc.hour * 3600 + utc.minute * 60 + utc.second;
          const struct timeval tv{static_cast<time_t>(secs), 0};
          settimeofday(&tv, nullptr);
          ESP_LOGI(TAG, "clock seeded from the RTC (UTC %04u-%02u-%02u %02u:%02u:%02u)", utc.year, utc.month,
                   utc.day, utc.hour, utc.minute, utc.second);
        } else {
          ESP_LOGW(TAG, "the RTC answers but its time is invalid (backup power lost?); waiting for network time");
        }
      }
    } else {
      ESP_LOGW(TAG, "no RTC at 0x%02x; the clock starts with network time", cfg.addr);
    }
  }
  apply_timezone(storage);
}

void TimePort::apply_timezone(hg::Storage& storage) {
  auto stored = storage.get("timezone");
  const std::string tz = stored && !stored->empty() ? *stored : "UTC0";
  setenv("TZ", tz.c_str(), 1);
  tzset();
  ESP_LOGI(TAG, "timezone %s", tz.c_str());
}

void TimePort::set_network(bool up) {
  network_up_ = up;
  sync_sntp();
}

void TimePort::set_wanted(bool on) {
  wanted_ = on;
  sync_sntp();
}

void TimePort::sync_sntp() {
  if (wanted_ && network_up_) {
    if (sntp_started_) return;
    sntp_started_ = true;
    // The lwIP SNTP client sets the system clock (UTC), then calls back; the
    // callback only posts the event, so no driver state crosses tasks.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = &TimePort::on_sntp;
    const esp_err_t err = esp_netif_sntp_init(&cfg);  // config.start = true
    if (err != ESP_OK) {
      sntp_started_ = false;
      ESP_LOGE(TAG, "SNTP did not start (%s); the clock stays as seeded", esp_err_to_name(err));
    } else {
      ESP_LOGI(TAG, "asking pool.ntp.org for the time");
    }
    return;
  }
  // Stop only a service that started: deinit on a never-started one fails. Init
  // runs again once the clock is wanted and the network is up, so a reconnect
  // asks for the time at once instead of on the old client's retry schedule.
  if (sntp_started_) {
    esp_netif_sntp_deinit();
    sntp_started_ = false;
  }
}

void TimePort::on_sntp(struct timeval* tv) {
  (void)tv;  // the system clock is already set when the callback runs
  events::post(EventType::TimeSynced);
}

void TimePort::on_synced() {
  synced_ = true;
  if (!chip_) return;
  const time_t now = time(nullptr);
  struct tm utc{};
  if (!gmtime_r(&now, &utc)) return;
  hg::WallTime w;
  w.year = static_cast<uint16_t>(utc.tm_year + 1900);
  w.month = static_cast<uint8_t>(utc.tm_mon + 1);
  w.day = static_cast<uint8_t>(utc.tm_mday);
  w.weekday = static_cast<uint8_t>(utc.tm_wday);
  w.hour = static_cast<uint8_t>(utc.tm_hour);
  w.minute = static_cast<uint8_t>(utc.tm_min);
  w.second = static_cast<uint8_t>(utc.tm_sec);
  if (chip_->write(w)) ESP_LOGI(TAG, "RTC set from network time");
  else ESP_LOGW(TAG, "writing the RTC failed");
}

hg::json::Value TimePort::diag() const {
  hg::json::Value t = hg::json::Value::object();
  hg::WallTime w{};
  const bool valid = local_wall_time(w);
  t.set("valid", valid).set("synced", synced_);
  if (valid) {
    const time_t now = time(nullptr);
    struct tm utc{};
    if (gmtime_r(&now, &utc)) {
      char buf[24];
      std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
      t.set("utc", buf);
    }
  }
  return t;
}

}  // namespace hgp
