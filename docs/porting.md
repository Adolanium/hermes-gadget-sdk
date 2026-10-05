# Porting: new boards, displays, audio, inputs, sensors and actions

The core (`firmware/core`) never changes for a new board. A port is drivers plus configuration.

## Add a board with existing drivers

The common case is an SPI ST7789 panel, an I2S microphone, an I2S amplifier and GPIO buttons.

1. Add a Kconfig choice in `firmware/esp32/main/Kconfig.projbuild`:

   ```kconfig
   config HG_BOARD_MY_BOARD
       bool "My Board (ST7789 240x240, ICS-43434, MAX98357A)"
   ```

2. Return its description from `firmware/esp32/main/board.cpp`:

   ```cpp
   #elif CONFIG_HG_BOARD_MY_BOARD
   BoardConfig make() {
     BoardConfig b{};
     b.name = "my-board";
     b.lcd = {true, 240, 240, /*swap_xy*/ false, false, false, /*invert*/ true, 0, 0,
              /*mosi*/ 23, /*sclk*/ 18, /*cs*/ 5, /*dc*/ 16, /*rst*/ 17, /*bl*/ 4, 40};
     b.mic = {true, 26, 25, 33};
     b.speaker = {true, 27, 14, 12};
     b.buttons = {0, 35, -1, -1};
     return b;
   }
   ```

3. Add `firmware/esp32/boards/my-board/sdkconfig.defaults` with the target, flash size, PSRAM mode, and `CONFIG_HG_BOARD_MY_BOARD=y`. Where it picks another option of a choice the base file sets, such as the flash size, also turn the base option off (`# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set`); the build checks that every line took effect ([build checks](development.md#build-checks)). Add a PlatformIO env if you use it; every env is part of the next release.
4. Add `firmware/esp32/boards/my-board/board.json` with the name and one-line description the browser installer shows: `{"title": "...", "summary": "...", "docs": "docs/hardware.md#my-board"}`. Add `"ready_made": true` for an all-in-one board with nothing to wire; the installer then shows a "Nothing to wire" badge, so the summary needn't say it.

5. Add a simulator profile with the same screen size and peripherals to `BOARDS` in `python/hermes_gadget/sim/runner.py`, so UI work happens on the desktop.

6. Document the wiring in [hardware.md](hardware.md).

Before flashing, use **Custom pins** in menuconfig to try a wiring without writing code.

## A different display

Four display configurations ship: `SpiDisplay` supports ST7789 and ILI9342 variants over SPI; `ParallelDisplay` supports the ST7789 over an 8-bit I80 parallel bus; `AmoledDisplay` supports CO5300 over QSPI, including round AMOLED modules. For a round panel set `round` in the board config: the UI then keeps to the square inside the circle. BOX-3 uses the managed TT21100/GT911 touch drivers. Its LCD and touch share one reset line, so initialize the display before touch. CoreS3 uses FT5x06 for touch and detects the LCD revision through its firmware ID.

`ParallelDisplay` is the T-Display-S3 reference. It holds the panel’s active-low RD input, GPIO9, high, configures an 8-bit bus with `esp_lcd_new_i80_bus()`, creates its I80 panel IO with `esp_lcd_new_panel_io_i80()`, and attaches the ST7789 using `esp_lcd_new_panel_st7789()`. Panel reset, initialization, inversion, axis swap, mirroring, address gap and display enable use the `esp_lcd_panel_*` operations; the board-specific ST7789 power/gamma registers are sent through `esp_lcd_panel_io_tx_param()`. Frame rows are transferred through the I80 panel IO in DMA-capable chunks. The T-Display-S3 backlight uses an AW9364 one-wire pulse-counter protocol on its backlight GPIO, not LEDC PWM: drive low for 3 ms to turn it off; drive high to wake/enable it, then send clock pulses to select one of 16 brightness steps. The port maps requested brightness to those steps.

Implement `hg::Display` (`firmware/core/include/hg/hal.hpp`):

| Method | What it must do |
|---|---|
| `info()` | Width, height, whether to store pixels byte-swapped (most SPI panels want big-endian RGB565, so return `swap_bytes = true`), whether a backlight can be dimmed, and whether the panel is `round` |
| `framebuffer()` | A width × height RGB565 buffer you own (PSRAM on ESP32) |
| `flush(y0, y1)` | Push full-width rows `[y0, y1)` to the panel |
| `set_backlight(percent)` | Optional |

`SpiDisplay` in `port_display.cpp` is the reference. To add ILI9341, GC9A01 or another panel, swap `esp_lcd_new_panel_st7789` for the matching `esp_lcd` driver (most are managed components). For RGB/parallel or QSPI AMOLED panels, the same interface applies with that panel's `esp_lcd` IO.

**Round panels** (for example a 1.75" 466×466 AMOLED): set `round = true`. The UI then draws inside the square inscribed in the circle, keeps everything else dark, centres the status row, and tells the host `"shape": "round"`. Try it with the `sim-466x466-round` simulator board.

- **Monochrome or e-paper:** convert RGB565 to your format in `flush()`. The UI uses dark backgrounds with light text and accents, so thresholding the luminance works.
- **Very small screens** (128×64): the layout scales text to 1×. You may want a slimmer layout; `Ui` reads only `DisplayInfo`.

## Battery and power

Set `Hal::power` only after the board's power driver starts successfully. `Power::read()` returns current readings or `nullopt` on a failed read. Each measurement is optional; omit a percentage when the hardware has no fuel gauge. `power_off()` requests a local shutdown and reports whether the request succeeded. The settings menu asks for a second selection before calling it.

The AXP2101 implementation is in `firmware/drivers/axp2101.cpp`, with the ESP32 I2C connection in `port_power.cpp`. It leaves charger settings and supply rails unchanged. The core reports unavailable sensor values as JSON `null` to replace earlier readings at the gateway.

CoreS3 has a separate `CoreS3Control` in `firmware/drivers/cores3.cpp`. It enables the required audio supplies, boost and AW9523 reset outputs before display, touch and audio initialization. Its masked writes preserve unrelated settings. `SpiDisplay::board_backlight` routes brightness through DLDO1 on this board. Native tests cover supply values, reset timing, preservation of other registers, brightness and failed I2C access. `CodecAudioConfig::speaker` selects the AW88298 driver for CoreS3; other profiles keep ES8311.

A display that advertises `has_backlight` must accept zero percent to turn dark. `screen_timeout` dims and then darkens an idle display while keeping the device connected. Raw touch drivers should use `TouchGestures`, which consumes the first touch when waking. Button input through `App::on_button` does the same.

## Audio through a codec chip

Boards like the ESP32-S3-BOX family and the ESP32-S3-Touch-AMOLED-1.75 route audio through codecs (ES7210 ADC, ES8311 DAC) configured over I2C. `CodecAudio`, `CodecMic` and `CodecSpeaker` in `port_codec.cpp` implement `hg::AudioIn` and `hg::AudioOut` on top of `esp_codec_dev` for that pair: fill in `BoardConfig::codec` and `BoardConfig::i2c`. Other codecs follow the same contracts:

- **`AudioIn`:**
  - `start(rate)` begins capture.
  - Deliver mono PCM16 chunks of about 20 ms with `App::on_mic_samples` through the event queue (`EventType::Mic`). Never call `App` from a driver task.
  - `stop()` ends capture.
- **`AudioOut`:**
  - `begin(rate)` opens a stream.
  - `write()` must not block: buffer about 1.5 s; the server keeps 0.5 s of lead.
  - `end()` drains the buffer.
  - `abort()` drops it immediately.
  - `busy()` stays true until the buffered audio has played out.

`I2sMic` and `I2sSpeaker` in `port_audio.cpp` are the reference implementations. The device declares its rates in `DeviceProfile`, and the server resamples to the speaker rate, so a 24 kHz or 48 kHz codec works without host changes.

## Inputs

`App::on_button(Button, pressed)` takes four logical buttons: `Talk`, `Cancel`, `Up`, `Down`. Map any physical input onto them:

- **Touch screen:** `hg::TouchGestures` (`firmware/core/include/hg/touch.hpp`) turns raw touch samples into the buttons: hold anywhere is `Talk`, a quick tap answers "yes", a swipe down is `Cancel`. Feed it from the touch driver and set `DeviceProfile::touch_screen` so the hints say "Hold the screen to talk". `port_touch.cpp` does this for a CST9217.
- **Rotary encoder:** detents → `Up` / `Down`; push → `Talk`.
- **A single button:** `Talk` only. Long-press handling for `Cancel` belongs in the port.

Set `DeviceProfile::has_cancel_button`, `has_scroll_buttons` and the labels so the hint bar and the `hello` capabilities match the hardware.

## Sensors

Call `app.set_sensor("co2_ppm", value)` whenever you have a reading; the core rate-limits reporting. The agent sees the latest values (with their age) through `gadget_devices`. Sensor names are free-form; include the unit (`temperature_c`, `humidity_pct`).

For events worth an agent reaction ("doorbell pressed", "motion"), use `app.emit_event(name, data, /*notify_agent=*/true)`. It arrives in Hermes as a message from the device.

## Device actions (agent → hardware)

An action is a named capability with a JSON-Schema parameter object and a description written for the model. Register it before `app.begin()`:

```cpp
hg::Action relay;
relay.name = "relay.set";
relay.description = "Switch the desk lamp relay on or off.";
hg::json::parse(R"({"type":"object","properties":{"on":{"type":"boolean"}},"required":["on"]})",
                relay.params);
relay.handler = [](const hg::json::Value& args, hg::json::Value& result, std::string& error) {
  if (!args["on"].is_bool()) { error = "on must be true or false"; return false; }
  gpio_set_level(GPIO_NUM_21, args["on"].as_bool());
  result.set("on", args["on"].as_bool());
  return true;
};
app.add_action(std::move(relay));
```

The device declares its actions in `hello`. Hermes's model discovers them through the per-device context and `gadget_devices`, and calls them with `gadget_action`. No Hermes or plugin change is needed. Handlers run on the app task: keep them short, and post long work to another task and report completion with an event.

The firmware ships `speaker.volume` and `screen.brightness` when the hardware allows. The simulator adds `led.set` and `buzzer.beep` as examples.

For runnable Linux examples, see [Home Assistant and MQTT](home-automation.md). They expose fixed lamp targets, report temperature availability and return a job ID for work that completes asynchronously.

## A non-ESP32 device

Anything that can run C++17 can host the core: a Raspberry Pi with a small display, a Zephyr board, a Linux handheld. Implement the six HAL interfaces and an event loop that calls `App::tick()`. The simulator's `firmware/sim/src/hgsim.cpp` is the smallest complete port. Devices that cannot run the core can speak [the protocol](protocol.md) directly.
