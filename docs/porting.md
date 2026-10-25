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

Complete the [board contribution checklist](../CONTRIBUTING.md#adding-a-board), including CI, the supported-hardware comparison, and verification status. Before flashing, use **Custom pins** in menuconfig to try a wiring without writing code.

## Check a hardware driver

- Confirm pin functions against the manufacturer's schematic for the exact board revision. Guard absent pins before GPIO access or bit shifts. Record voltage-divider ratios and signal polarity explicitly.
- Expose only capabilities the circuit supports. A panel power rail does not imply device shutdown, and a battery ADC does not establish charging state or battery percentage.
- Check timing against the configured FreeRTOS tick rate. `pdMS_TO_TICKS(3)` is zero at 100 Hz; use a delay that guarantees the hardware's minimum interval. Exercise immediate sleep/wake and repeated brightness changes, not only a normal boot.
- Use the APIs from this project's ESP-IDF version. Build the affected profile locally, and require the full firmware matrix for shared code changes.
- Pin the manufacturer's source revision for initialization sequences and drivers. Follow the [licensing requirements](../CONTRIBUTING.md#licensing), including source notices and distribution packages.
- Record physical results with the [hardware checklist](hardware-validation.md#record-a-physical-test). If hardware is unavailable or testing is incomplete, state that limitation and retain experimental status.

## A different display

Five display configurations ship: `SpiDisplay` supports ST7789 and ILI9342 variants over SPI, and the ST77916 over QSPI; `ParallelDisplay` supports the ST7789 over an 8-bit I80 parallel bus; `AmoledDisplay` supports CO5300 over QSPI, including round AMOLED modules; `DsiDisplay` supports the CrowPanel P4 EK79007 panel over MIPI-DSI. The P4 audio path is a board-specific adapter; the shared Gadget audio and power APIs remain unchanged. For a round panel set `round` in the board config: the UI then keeps to the square inside the circle. BOX-3 uses the managed TT21100/GT911 touch drivers. Its LCD and touch share one reset line, so initialize the display before touch. CoreS3 uses FT5x06 for touch and detects the LCD revision through its firmware ID. Where the LCD and touch resets are TCA9554 expander outputs, set `expander_reset.mask` to those outputs; the other expander bits keep their values. An ST77916 panel gets Waveshare's 1.85C tables, chosen by the panel ID the driver reads, unless the board names its own with `lcd.st77916_panel` (the ESP-VoCat's table is in `panel_vocat.hpp`). Supplies switched straight from a GPIO, such as the ESP-VoCat's LCD/SD rail (GPIO 9, on when low) and codec supply (GPIO 48, on when high), go in `power_rails` with the level that turns each on; the firmware sets them before any peripheral starts and waits `power_rails_settle_ms`.

`ParallelDisplay` is the T-Display-S3 reference. It holds the panel’s active-low RD input, GPIO9, high, configures an 8-bit bus with `esp_lcd_new_i80_bus()`, creates its I80 panel IO with `esp_lcd_new_panel_io_i80()`, and attaches the ST7789 using `esp_lcd_new_panel_st7789()`. Panel reset, initialization, inversion, axis swap, mirroring, address gap and display enable use the `esp_lcd_panel_*` operations; the board-specific ST7789 power/gamma registers are sent through `esp_lcd_panel_io_tx_param()`. Frame rows are transferred through the I80 panel IO in DMA-capable chunks. The T-Display-S3 backlight uses an AW9364 one-wire pulse-counter protocol on its backlight GPIO, not LEDC PWM: drive low for 3 ms to turn it off; drive high to wake/enable it, then send clock pulses to select one of 16 brightness steps. The port maps requested brightness to those steps.

Implement `hg::Display` (`firmware/core/include/hg/hal.hpp`):

| Method | What it must do |
|---|---|
| `info()` | Width, height, whether to store pixels byte-swapped (most SPI panels want big-endian RGB565, so return `swap_bytes = true`), whether a backlight can be dimmed, and whether the panel is `round` |
| `framebuffer()` | An RGB565 buffer you own (PSRAM on ESP32). It holds the whole panel, or a band of rows when `framebuffer_rows()` says so |
| `flush(y0, y1)` | Push full-width rows `[y0, y1)` to the panel |
| `set_backlight(percent)` | Optional |
| `framebuffer_rows()` | Optional. Rows the framebuffer holds, or `0` (the default) when it holds the whole panel. Return a band on a board that cannot spare a whole-panel buffer: the UI then draws each changed band once per window (see below) |
| `set_frame_origin(y)` | Optional. Maps the framebuffer onto panel rows `[y, y + framebuffer_rows())` for later draws and flushes. Only means something with a banded `framebuffer_rows()` |
| `take_redraw_request()` | Optional. `true` once when a lost transfer left the panel stale and the buffer holds no rows to resend; the UI answers by redrawing the whole screen |

A **banded framebuffer** is how a board with no PSRAM fits: the port owns only `width × framebuffer_rows()` pixels and reports the row count from `framebuffer_rows()`. The UI then renders each changed band once per window it spans, moving the window with `set_frame_origin()` between draws, so no drawing code needs to know about the banding. `SpiDisplay` on the CYD (`firmware/esp32/main/port_display.cpp`) is the reference: it reserves a 320×120 band in an early constructor, before the heap fragments, and keeps `take_redraw_request()` set after a flush times out, until a repaint reaches the panel, because the band holds no rows to resend. An image overlay (`image.start`) is composed in the framebuffer over several packets, so a banded display leaves `display.image` out of its hello and refuses an image sent anyway.

`SpiDisplay` in `port_display.cpp` is the reference. It drives ST7789, ILI9341/ILI9342, ST77916 (QSPI) and GC9A01 panels; set `b.lcd.controller`. A GC9A01 whose glass wants different gate timing or supply levels gets a `Gc9a01Panel` entry with those register changes, sent after the driver's default bring-up (`panel_spotpear128.hpp` is the example). To add another panel, swap `esp_lcd_new_panel_st7789` for the matching `esp_lcd` driver (most are managed components). For RGB/parallel or QSPI AMOLED panels, the same interface applies with that panel's `esp_lcd` IO.

`SpiDisplay` sends rows through one DMA staging buffer with `hg::BandFlush` (`firmware/drivers/band_flush.hpp`). It never refills the buffer while a transfer may still read it, and a transfer that does not finish pauses the screen without blocking the app. A new panel that stages rows for DMA can use it the same way.

For an active-low GPIO backlight driven by `SpiDisplay`, set `b.lcd.backlight_invert = true` in the board's `make()`. The default is `false` for active-high backlights. LEDC inverts the output so brightness still ranges from 0 percent off to 100 percent on. This flag is separate from `b.lcd.invert`, which controls pixel colors, and does not apply to board-specific backlight callbacks or the T-Display-S3's AW9364 driver. Verify off, intermediate brightness, full brightness, and screen-timeout wake on the physical board.

**Round panels** (for example a 1.75" 466×466 AMOLED): set `round = true`. The UI then draws inside the square inscribed in the circle, keeps everything else dark, centres the status row, and tells the host `"shape": "round"`. Try it with the `sim-466x466-round` simulator board.

- **Monochrome or e-paper:** convert RGB565 to your format in `flush()`. The UI uses dark backgrounds with light text and accents, so thresholding the luminance works.
- **Very small screens** (128×64): the layout scales text to 1×. You may want a slimmer layout; `Ui` reads only `DisplayInfo`.

## Battery and power

Set `Hal::power` only after the board's power driver starts successfully. `Power::read()` returns current readings or `nullopt` on a failed read. Each measurement is optional; omit a percentage when the hardware has no fuel gauge. `power_off()` requests a local shutdown and reports whether the request succeeded. The settings menu asks for a second selection before calling it.

The AXP2101's fuel gauge can read well off on a given cell. A board can set `BoardConfig::battery_curve` and `battery_charge_curve` to curves measured on its own cell (`firmware/drivers/battery_curve.hpp`, with the AMOLED-1.75C's as the example): the driver then reports the percentage from the voltage on those curves instead of the gauge.

While the screen is off, nothing is happening and `Power::external_power()` reports battery power, the app calls `System::set_dozing(true)`, and `set_dozing(false)` before it draws again. The ESP32 port lets the chip light-sleep then only on boards that set `BoardConfig::light_sleep` (the AMOLED-1.75C). Such a board also needs `CONFIG_PM_ENABLE` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE` in its `sdkconfig.defaults`, and its panel, touch-reset, amplifier and I2C pins must hold their level through sleep: `keep_pins_through_sleep()` in `main.cpp` does that for the AMOLED panel's pins and any `power_rails`.

The AXP2101 implementation is in `firmware/drivers/axp2101.cpp`, with the ESP32 I2C connection in `port_power.cpp`. It leaves charger settings and supply rails unchanged. The core reports unavailable sensor values as JSON `null` to replace earlier readings at the gateway.

CoreS3 has a separate `CoreS3Control` in `firmware/drivers/cores3.cpp`. It enables the required audio supplies, boost and AW9523 reset outputs before display, touch and audio initialization. Its masked writes preserve unrelated settings. `SpiDisplay::board_backlight` routes brightness through DLDO1 on this board. Native tests cover supply values, reset timing, preservation of other registers, brightness and failed I2C access. `CodecAudioConfig::speaker` selects the AW88298 driver for CoreS3; other profiles keep ES8311.

A display that advertises `has_backlight` must accept zero percent to turn dark. An OLED or AMOLED driver sets `emissive`, so the UI draws its background true black and leaves those pixels unlit. `screen_timeout` dims and then darkens an idle display while keeping the device connected. Raw touch drivers should use `TouchGestures`, which consumes the first touch when waking. Button input through `App::on_button` does the same.

## Motion

Boards with an accelerometer can offer face-down sleep. Set `DeviceProfile::motion_sensor` when the sensor starts, and call `App::on_motion(x_mg, y_mg, z_mg)` from the app task 5-25 times a second with lightly smoothed milli-g values in the device frame: `+x` towards the right of the picture, `+y` towards its top, `+z` out of the screen towards the viewer. A board lying flat with the screen up reads about `(0, 0, +1000)`. Samples older than two seconds are ignored, so a stalled sensor cannot keep the screen dark.

`firmware/drivers/qmi8658.cpp` with `port_motion.cpp` is the QMI8658 reference: a 10 Hz task reads the chip on its own device handle on the shared I2C bus, and the app loop polls it and forwards only new samples. Fill in `BoardConfig::imu` (`ImuConfig`): the I2C address, which chip axis feeds each device axis (`x_from` and friends), and its sign. Add the `imu` entry only after `diag` confirms the axes on a real board: lying face down, `motion.z` reads near `-1000`; lying screen up, near `+1000`; standing with the picture's top edge up, `motion.y` near `+1000`. The `face_down_sleep` setting turns the sleep on; it is off by default. On a board that can sleep face down, the console's `status` includes `"face_down": true|false`.

A 10 Hz motion task wakes a light-sleeping chip every 100 ms. A slower poll while dozing must stay under the core's two-second stale limit, or the screen lights.

## Clock

Boards with a display can offer the resting clock ([using-gadget.md](using-gadget.md#resting-clock)). Implement `System::wall_time(WallTime&)` (`hal.hpp`) to return the local time; it returns false until the port knows the time, and the clock face stays off until then. On the ESP32 port, `port_time.cpp` is the reference: a board declares its clock chip with `BoardConfig::rtc` (`RtcConfig`, I2C address), and at boot the port probes the PCF85063 on the shared bus and seeds the system clock from its UTC time — a chip that does not answer, one whose backup power was lost, or one that holds an impossible time (a digit past 9, a day past the month's end) is skipped and the port waits for the network instead. While the `clock` setting is above 0 and the network is up, SNTP (`pool.ntp.org`) runs, and each sync writes the time back to the chip. The `timezone` extra setting is a POSIX TZ string applied through `TZ` and `tzset()`; a string newlib cannot read means UTC.

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
  - `busy()` stays true until the buffered audio has played out, and no longer once a closed stream has nothing left to play.
  - Audio written right after `begin()` must survive the previous stream's `abort()`: a port whose task empties its queue later waits for that in `begin()`, as `I2sSpeaker` does.

`I2sMic` and `I2sSpeaker` in `port_audio.cpp` are the reference implementations. The device declares its rates in `DeviceProfile`, and the server resamples to the speaker rate, so a 24 kHz or 48 kHz codec works without host changes.

## Inputs

`App::on_button(Button, pressed)` takes four logical buttons: `Talk`, `Cancel`, `Up`, `Down`. Map any physical input onto them:

- **Touch screen:** `hg::TouchGestures` (`firmware/core/include/hg/touch.hpp`) turns raw touch samples into the buttons: hold anywhere is `Talk`, a quick tap answers "yes", a swipe down is `Cancel`. Feed it from the touch driver and set `DeviceProfile::touch_screen` so the hints say "Hold the screen to talk". `port_touch.cpp` does this for a CST9217. A touch controller wired to its own I2C pins sets `BoardConfig::touch.bus`; the firmware then opens that bus on the second I2C controller, as the Spotpear muma and 1.28-BOX do. Set `touch.ppi` to the glass's pixels per inch (resolution over the nominal diagonal, or the diameter of a round panel) so tap targets such as the profile list rows come out about 7 mm tall; without it the UI assumes 200.
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

The firmware ships `speaker.volume` and `screen.brightness` when the hardware allows, and `screen.language` on every board with a display. The simulator adds `led.set` and `buzzer.beep` as examples.

The firmware also ships `timer.start`, `timer.status` and `timer.cancel` on every board with a display or a speaker. The alarm beeps only where the board has a speaker. See [timers](using-gadget.md#timers) for what the device shows.

For runnable Linux examples, see [Home Assistant and MQTT](home-automation.md). They expose fixed lamp targets, report temperature availability and return a job ID for work that completes asynchronously.

## A non-ESP32 device

Anything that can run C++17 can host the core: a Raspberry Pi with a small display, a Zephyr board, a Linux handheld. Implement the six HAL interfaces and an event loop that calls `App::tick()`. The simulator's `firmware/sim/src/hgsim.cpp` is the smallest complete port. Devices that cannot run the core can speak [the protocol](protocol.md) directly.
