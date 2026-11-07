// TCA8418 keyboard adapter for Cardputer ADV. Poll on the app task: all App
// calls, display updates and shared I2C transactions remain serialized here.
#include "port_cardputer.hpp"
#if CONFIG_HG_BOARD_CARDPUTER_ADV
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace hgp {
bool CardputerKeyboard::read(uint8_t reg, uint8_t& value) {
  return i2c_master_transmit_receive(dev_, &reg, 1, &value, 1, 20) == ESP_OK;
}
bool CardputerKeyboard::write(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(dev_, data, sizeof(data), 20) == ESP_OK;
}
/** Drain at most ten hardware FIFO entries, then reset modifiers and IRQ flags.
 * Return false on an I2C failure; callers performing recovery also clear held
 * keys before draining so an unread release cannot leave a modifier pressed.
 */
bool CardputerKeyboard::flush() {
  for (int n = 0; n < 10; ++n) {
    uint8_t event;
    if (!read(0x04, event)) return false;
    if (!event) break;
  }
  keys_.reset();
  return write(0x02, 0x1f);
}
void CardputerKeyboard::recover() {
  keys_.reset();  // an unread release must never leave Shift/Fn held
  flush();
}
/** Probe address 0x34 and configure its 7x8 keypad, debounce and GPIO11 IRQ.
 * A partially configured device is removed from the bus on failure. Call from
 * the app task before polling, using the board's shared I2C bus.
 */
bool CardputerKeyboard::begin(i2c_master_bus_handle_t bus) {
  if (!bus || i2c_master_probe(bus, 0x34, 50) != ESP_OK) return false;
  i2c_device_config_t cfg{};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = 0x34;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;
  // Seven scan rows, eight scan columns (56 switches). No GPI events;
  // hardware debounce on. KEY_INT and overflow interrupts, stop FIFO on full.
  bool ok = write(0x01, 0) && write(0x1d, 0x7f) && write(0x1e, 0xff) && write(0x1f, 0);
  for (uint8_t reg : {0x20, 0x21, 0x22, 0x29, 0x2a, 0x2b}) ok = write(reg, 0) && ok;
  ok = ok && flush() && write(0x01, 0x09);
  gpio_config_t irq{};
  irq.pin_bit_mask = 1ULL << 11;
  irq.mode = GPIO_MODE_INPUT;
  irq.pull_up_en = GPIO_PULLUP_ENABLE;
  ok = ok && gpio_config(&irq) == ESP_OK;
  if (!ok) { i2c_master_bus_rm_device(dev_); dev_ = nullptr; }
  ESP_LOGI("hg.keyboard", "TCA8418 %s", ok ? "ready" : "initialization failed");
  return ok;
}
/** Poll the FIFO on the app task, at 20 ms with an IRQ or a 100 ms fallback.
 * Translate fresh presses into context-appropriate App controls or ASCII draft
 * edits. Lost FIFO state resets modifiers, and leaving an editable screen
 * discards its draft so setup, prompts and active turns remain visible.
 */
void CardputerKeyboard::poll(hg::App& app, hg::CardputerCompose& display) {
  const auto can_type = [&] {
    return app.online() && !app.in_turn() && !app.settings_open() &&
        (app.screen() == hg::Screen::Ready || app.screen() == hg::Screen::Responding);
  };
  // Never cover permission questions, alarms, OTA progress or setup screens.
  if (display.active() && !can_type()) display.close();
  if (!dev_) return;
  const uint32_t now = esp_timer_get_time() / 1000;
  if (now - last_poll_ < 20) return;
  // Check count periodically even with IRQ high, to recover a missed edge.
  if (gpio_get_level(GPIO_NUM_11) && now - last_poll_ < 100) return;
  last_poll_ = now;
  uint8_t status, count;
  if (!read(0x02, status) || !read(0x03, count)) { recover(); return; }
  if (status & 0x08) {  // FIFO overflow: discard the incomplete chord
    recover();
    ESP_LOGW("hg.keyboard", "FIFO overflow; key state reset");
    return;
  }
  for (int n = 0; n < (count & 0x0f) && n < 10; ++n) {
    uint8_t event;
    if (!read(0x04, event)) { recover(); return; }
    const auto key = keys_.feed(event);
    using K = hg::CardputerKeys::Kind;
    if (key.kind == K::None) continue;
    if (app.wake_display()) continue;
    const auto pulse = [&](hg::Button b) { app.on_button(b, true); app.on_button(b, false); };
    switch (key.kind) {
      case K::Character:
        if (can_type()) display.append(key.text);
        break;
      case K::Backspace: if (display.active()) display.backspace(); break;
      case K::Enter:
        if (display.active()) {
          const std::string message = display.draft();
          display.close();
          if (!message.empty() && app.online() && !app.in_turn()) app.submit_text(message);
        } else if (app.settings_open() || app.screen() == hg::Screen::Prompt || app.profile_picker_open()) {
          pulse(hg::Button::Talk);
        }
        break;
      case K::Cancel:
        if (display.active()) display.close();
        else pulse(hg::Button::Cancel);
        break;
      case K::Up: if (!display.active()) pulse(hg::Button::Up); break;
      case K::Down: if (!display.active()) pulse(hg::Button::Down); break;
      case K::Settings: display.close(); app.open_settings(); break;
      case K::Power: display.close(); app.on_power_key(); break;
      case K::Profile: display.close(); app.next_agent_profile(); break;
      default: break;
    }
  }
  // W1C after draining; new queued events are also caught by the count poll.
  if (!write(0x02, 0x01)) recover();
}
}  // namespace hgp
#endif
