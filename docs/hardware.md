# Hardware

The firmware is an ESP-IDF application (`firmware/esp32`) built on the portable core. The reference board uses common modules you can wire on a breadboard. Other boards need a configuration and, where necessary, drivers; see [porting.md](porting.md).

Start with [Supported hardware](supported-hardware.md) for a board comparison, then check [capabilities and verification](hardware-validation.md). The release builds are experimental until a physical report is recorded for the exact model and revision.

## Requirements

- **Chip:** the release profiles target ESP32-S3. Other ESP32 variants need their own configuration and validation. On a board without PSRAM, the framebuffer (width × height × 2 bytes) must fit in internal RAM next to Wi-Fi and audio buffers.
- **Display:** an SPI ST7789 panel (240×320, 240×240 or 240×135). Other controllers need a different `esp_lcd` panel driver.
- **Microphone:** an I2S MEMS microphone (INMP441, ICS-43434, SPH0645 style).
- **Speaker (optional):** an I2S class-D amplifier (MAX98357A) and a 4–8 Ω speaker.
- **Buttons:** at least one (TALK). The DevKit's BOOT button works; a second button for CANCEL is recommended.

## ESP32-S3 breadboard

Board option `esp32s3-breadboard`, for an ESP32-S3-DevKitC-1 N8R8 and the modules above.

- **Flash:** 4 MB or more. The same image runs on 4, 8 and 16 MB modules.
- **PSRAM:** octal, as on the N8R8 and N16R8. On an N8R2 (quad PSRAM) or a module without PSRAM, the firmware still starts and says so in its log, but the display may not: the framebuffer needs PSRAM. For an N8R2, build with `CONFIG_SPIRAM_MODE_QUAD=y` instead.

| Module | Signal | ESP32-S3 GPIO |
|---|---|---|
| ST7789 LCD | MOSI / SDA | 11 |
| | SCLK / SCL | 12 |
| | CS | 10 |
| | DC | 9 |
| | RST | 8 |
| | BL (backlight) | 7 |
| | VCC / GND | 3V3 / GND |
| INMP441 mic | SCK | 4 |
| | WS | 5 |
| | SD | 6 |
| | L/R | GND (left channel) |
| | VDD / GND | 3V3 / GND |
| MAX98357A amp | BCLK | 15 |
| | LRC | 16 |
| | DIN | 17 |
| | VIN / GND | 5V / GND |
| Buttons (to GND) | TALK | 0 (BOOT) |
| | CANCEL | 14 |

All pins avoid the S3's flash/PSRAM pins (26–37) and native USB (19/20). Use **Custom pins** in menuconfig to change any of them.

## Waveshare ESP32-S3-LCD-1.54

Board option `esp32s3-lcd-154`, for Waveshare's all-in-one 1.54" board (SKUs 33866/33867; the `-EN` SKU is the same hardware): an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 240×240 ST7789 panel over SPI, an ES8311 DAC and an ES7210 microphone ADC with two microphones, an NS4150B amplifier, a speaker, a QMI8658 6-axis IMU, a TF card slot, a battery charger and the BOOT / PLUS / PWR keys. Nothing to wire and nothing to connect, it works out of the box. The touch version (`ESP32-S3-Touch-LCD-1.54`, SKUs 33868/33869) adds a CST816 touchscreen that this port does not use.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI | SCLK 38, MOSI 39, CS 21, DC 45, RST 40, BL 46 (LEDC) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 8, BCLK 9, WS 10, DOUT 12; amplifier enable 7 |
| Microphones | ES7210 | I2C 0x40; I2S DIN 11 (shares the bus above), MIC1 + MIC2 |
| IMU | QMI8658 | I2C 0x6B |
| I2C bus | | SDA 42, SCL 41, 400 kHz |
| Buttons (to GND) | | TALK = BOOT (0), CANCEL = PLUS (4) |
| Battery | ETA6098 | GPIO 1 (BAT_ADC), GPIO 2 (power latch), GPIO 3 (CHG_STAT) |

The PWR key remains part of the power circuit. Firmware asserts GPIO 2 at startup to keep the battery path enabled. The TF card, QMI8658 and touchscreen are unused.

The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) shows calibrated battery-node voltage and the charging signal. GPIO 1 samples the R27/R32 divider, whose ratio is three. GPIO 3 reads the active-low charging signal. This board has no fuel gauge, battery presence detector or USB-status input, so those readings are omitted. Voltage is unavailable if the ESP32's ADC calibration cannot start. An unplugged battery can leave voltage on this node while USB powers the charger; voltage alone does not prove a battery is attached.

Select **Power off** twice to lower GPIO 2 and disconnect battery power. **USB continues to power the board.** Disconnect USB to turn it off, and hold PWR to turn it back on from battery. BOOT and PLUS retain their TALK and CANCEL roles. The optional screen timeout dims and darkens the display while keeping Wi-Fi connected.

Wiring follows the [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.54/ESP32-S3-LCD-1.54-Schematic.pdf). For physical verification, compare the displayed voltage with a meter, test with and without a battery, check the charging signal, and test shutdown separately with USB and battery power. Record the board revision in the [hardware checklist](hardware-validation.md).

**Build and flash it** with PlatformIO (`pio run -e esp32s3-lcd-154 -t upload -t monitor`) or with `idf.py`:

```bash
cd firmware/esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-esp32s3-lcd-154/sdkconfig.defaults" build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

The USB-C port is the S3's own USB Serial/JTAG, so flashing and the serial console (115200 baud) both use it, exactly like the AMOLED board.

### First flash: what to check

1. **Boot log:** `ST7789 240x240 ready`, `codecs: speaker ready, microphones ready`, and in `hermes-gadget diag` an `i2c` list with `0x18` (ES8311) and `0x40` (ES7210). `0x6b` (QMI8658) answers too.
2. **Screen:** the mascot is centred, upright and not mirrored, and the colours are right (amber accents, not blue). If the image is mirrored, flip `mirror_x`; a one-pixel stripe at an edge means the gap is off. The panel is written for SPI mode 0; Waveshare's own demo uses mode 3, so a blank or garbled screen is a candidate for changing `io_cfg.spi_mode` in `port_display.cpp`.
3. **Microphone:** say something while holding BOOT; the waves move with your voice and Hermes's transcript is right. The ES7210 is configured here the same way as the AMOLED board (I2S standard mode, MIC1 + MIC2). If the capture is silent or wrong, Waveshare's factory demo drives the ADC in I2S TDM mode (4 slots, `bclk_div` 8) instead, which is the first thing to try.
4. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.
5. **Buttons:** BOOT holds to talk, PLUS cancels, and holding PLUS for 2 s starts a new conversation.

## Waveshare ESP32-S3-Touch-LCD-1.85C V2

Use `esp32s3-touch-lcd-185c-v2` for the V2 board, PCB Rev2.0: an ESP32-S3R8 with 16 MB flash and 8 MB octal PSRAM, a round 360×360 ST77916 LCD over QSPI, CST816 touch, an ES8311 DAC, an ES7210 microphone ADC with two microphones and an NS4150B amplifier. A V2 board says Rev2.0 on the PCB or has a V2 sticker on the case. V1 has different audio hardware and must not use this image. The standard SKU needs a speaker on the SPK connector; the speaker-box version includes one. This port is experimental; see the [partial physical report](hardware-validation.md#waveshare-185c-v2-partial-physical-report).

| Part | Connection |
|---|---|
| ST77916 LCD, QSPI | CS 21, clock 40, D0–D3 46/45/42/41, backlight 5 (LEDC); reset through TCA9554 P1 |
| CST816 touch | I2C 0x15, polled; reset through TCA9554 P0 |
| TCA9554 expander | I2C 0x20; the firmware changes only P0 and P1 |
| I2C | SDA 11, SCL 10, 400 kHz |
| ES8311 speaker / ES7210 microphones | I2C 0x18 / 0x40; MCLK 2, BCLK 48, WS 38, DOUT 47, DIN 39; amplifier enable 15 |
| BOOT | GPIO 0, TALK |

**Audio.** Each I2S frame has two 32-bit slots. The ES7210 packs four 16-bit channels into them: the speaker's playback reference, a microphone, an unused channel and the second microphone. The firmware averages the two microphones into mono and ignores the reference, so there is no echo cancellation. The amplifier is on only while audio plays. Start with a low volume.

Hold the screen or BOOT to talk, tap to answer yes, and swipe down to cancel. RESET is not a CANCEL button. The IMU, RTC, TF card and battery ADC are not used, and the battery switch keeps its hardware function.

```bash
cd firmware/esp32
pio run -e esp32s3-touch-lcd-185c-v2 -t upload -t monitor
```

For ESP-IDF, use `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-esp32-s3-touch-lcd-1.85c-v2/sdkconfig.defaults"` with a separate build directory and sdkconfig. The USB-C port handles flashing and the serial console (115200 baud). The `sim-360x360-round` simulator board has the same screen.

### First flash: what to check

1. **Boot log:** `expander resets 0x03 ready`, `ST77916 panel ID …`, `st77916 360x360 ready`, `codecs: speaker ready, microphones ready` and `touch ready`. An unknown panel ID leaves the display off. In `hermes-gadget diag`, the `i2c` list has `0x15`, `0x18`, `0x20` and `0x40`.
2. **Screen:** the mascot is centred, upright and not mirrored, the whole circle is visible, and the accents are amber, not blue. Brightness and screen timeout work.
3. **Touch:** holding the centre and each edge starts a recording; a tap answers yes and a swipe down cancels.
4. **Microphone:** the waves move with your voice and Hermes's transcript is right.
5. **Speaker:** replies are clear, with no hiss or pop between them. Cancelling a reply stops it at once, and the next reply plays.

Record the results with the [hardware checklist](hardware-validation.md#record-a-physical-test).

References: [Waveshare V2 documentation](https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.85C), [V2 schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.85C/ESP32-S3-Touch-LCD-1.85C_V2.pdf) and the [factory demo at 8ead4a96](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C/tree/8ead4a96bf3a278fc4ebd8ef4768657e17fa2880). The demo numbers the expander pins from one (EXIO1 is P0). The LCD initialization tables and their licenses are listed in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## ESP32-S3-BOX-3

Use `esp32-s3-box-3` for Espressif's BOX-3 with 16 MB flash and 16 MB octal PSRAM. The original BOX and BOX-Lite need different profiles. This port is experimental, with no physical report recorded.

| Part | Connection |
|---|---|
| 320×240 LCD | SPI MOSI 6, clock 7, CS 5, DC 4, shared reset 48 active high, backlight 47 |
| Touch | TT21100 at 0x24 or GT911 at 0x5D/0x14; interrupt 3 |
| I2C | SDA 8, SCL 18 |
| ES8311 speaker / ES7210 microphones | MCLK 2, BCLK 17, WS 45, DOUT 15, DIN 16; amplifier enable 46 |
| BOOT | GPIO 0, TALK |

The driver identifies the display revision through the touch controller, following Espressif's board definitions. TT21100 selects ST7789; GT911 selects the ILI9342 initialization sequence through the ILI9341 driver. An unknown controller leaves the display unavailable and records an error. The display resets the shared line once before touch initialization.

Hold the screen or BOOT to talk, swipe down to cancel, and hold the title for one second to open settings. The top mute button keeps its hardware function; it is not a CANCEL button. The front home key is unused. Dock sensors, expansion outputs and storage are not exposed by this profile.

```bash
cd firmware/esp32
pio run -e esp32-s3-box-3 -t upload -t monitor
```

The USB-C port handles flashing and the serial console. For ESP-IDF, use `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32-s3-box-3/sdkconfig.defaults"` with a separate build directory and sdkconfig. Phone setup, pairing and OTA use the same flows as the other ESP32 boards.

Run the [hardware checklist](hardware-validation.md) for the exact panel revision. Check touch alignment in all corners, display colors, audio interruption, the physical mute button, USB recovery and Wi-Fi setup. The settings menu provides local mic, speaker, display and input checks. No battery readings are advertised.

References: [Espressif hardware overview](https://github.com/espressif/esp-box/blob/master/docs/hardware_overview/esp32_s3_box_3/hardware_overview_for_box_3.md) and [board definitions](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3). Display and touch drivers keep their Apache 2.0 license; see [README](../README.md#license) and the license archive distributed with firmware releases.

## M5Stack CoreS3

Use `m5stack-cores3` for the CoreS3 K128 with 16 MB flash and 8 MB **quad** PSRAM. Core, Core2 and CoreS3 SE are not covered by this profile. The port is experimental for both LCD revisions; physical reports remain outstanding.

| Part | Connection |
|---|---|
| 320×240 LCD | MOSI 37, clock 36, CS 3, DC 35; reset through AW9523 P1_1 |
| FT6336 touch | I2C 0x38; reset through AW9523 P0_0; polled |
| I2C | SDA 12, SCL 11 |
| AW88298 speaker / ES7210 microphones | MCLK 0, BCLK 34, WS 33, DOUT 13, DIN 14; reset through AW9523 P0_2 |
| Power and backlight | AXP2101 at 0x34; AW9523 at 0x58; boost enable P1_7 |

The display selects ILI9342C for touch firmware ID 0x10 and ILI9342E for 0x12. An unrecognized ID leaves the display unavailable and logs an error. M5Stack changed the LCD to ILI9342E in August 2026, so record the panel revision with test results.

Hold the screen to talk, swipe down to cancel, and hold the title for one second to open settings. GPIO 0 carries the audio clock and is not a TALK button. The hardware power and reset buttons retain their functions. Camera, IMU, RTC, SD storage and expansion outputs are not exposed.

The power adapter enables the audio supplies (ALDO1 at 1.8 V and ALDO2 at 3.3 V), the onboard boost, and peripheral reset lines. Brightness controls the DLDO1 backlight supply between 2.5 and 3.3 V; zero disables that rail. It preserves charger settings, other rail settings and USB/bus output-enable bits. Settings provide battery readings, local power-off, screen timeout and hardware checks.

```bash
cd firmware/esp32
pio run -e m5stack-cores3 -t upload -t monitor
```

For ESP-IDF, use `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/m5stack-cores3/sdkconfig.defaults"` with a separate build directory and sdkconfig. Use the USB-C data port for flashing and the console. To enter download mode, hold RESET for about three seconds until the green indicator lights. Phone Wi-Fi setup, pairing and OTA use the standard ESP32 flows.

Before relying on the port, run the [physical checklist](hardware-validation.md), including all touch corners, LCD colors, microphone level, speaker playback and interruption, backlight range, USB recovery, and battery shutdown/wake. Test USB and battery power separately.

References: [M5Stack hardware and recovery instructions](https://docs.m5stack.com/en/core/CoreS3), [Espressif board definitions](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3), [AW9523 registers](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/docs/products/core/CoreS3/AW9523B-EN.pdf), and [AXP2101 registers](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf). See [README](../README.md#license) for driver licenses and the adapted ILI9342E table's notice.

## ESP32-S3-Touch-AMOLED-1.75C

Use `esp32s3-touch-amoled-175c` for SKUs 33691/33692, the enclosed model with 32 MB flash and 8 MB octal PSRAM. This is an experimental port. Use its exact image; the 1.75 model's image has different pins.

| Part | Connection |
|---|---|
| CO5300 466×466 AMOLED | QSPI CS 12, clock 38, D0–D3 4/5/6/7, reset 1; column offset 6 |
| CST9217 touch | I2C 0x5A, reset 2; interrupt 11 unused because input is polled |
| I2C | SDA 15, SCL 14, 400 kHz |
| ES8311 speaker / ES7210 microphones | MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10; amplifier enable 46 |
| AXP2101 | I2C 0x34; ALDO1 at 3.3 V supplies analog audio; reports PWR short presses |
| BOOT | GPIO 0, TALK |

Hold the screen or BOOT to talk. Swipe down to cancel. Hold the **SETTINGS** target at the top of the screen for one second to open settings. A short press of PWR turns the screen off, and another press, a touch or BOOT turns it back on; Wi-Fi and the connection to Hermes stay up. PWR reaches only the AXP2101 on this model, so the firmware reads its short-press interrupt there; holding PWR keeps the PMIC's own power behavior. There is no TCA9554 expander. The IMU and RTC are not exposed.

The firmware enables ALDO1 for audio while preserving the other rails and charging settings. Battery readings and local power-off use the existing AXP2101 driver. An unavailable ADC or gauge reading remains absent. Use the settings menu for microphone, speaker, display, touch, volume, brightness and power checks.

Build and flash over the board's USB-C data port:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-175c -t upload -t monitor
```

For ESP-IDF, use `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-touch-amoled-175c/sdkconfig.defaults"` with a separate build directory and sdkconfig. The release workflow packages a distinct image and board identity, which prevents installing another model's OTA image.

Before relying on the port, run the [physical checklist](hardware-validation.md). Check screen orientation and edges, touch alignment, microphone level, a spoken reply, Wi-Fi setup and USB recovery. Test charging, shutdown and wake separately on USB and battery. A [physical report for SKU 33691](hardware-validation.md#waveshare-amoled-175c-physical-report) is recorded. It ran on a working branch rather than a build of `main`, so the port stays experimental.

Pin and supply references: [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf) and [manufacturer board definitions](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/master/bsp/esp32_s3_touch_amoled_1_75c). The port reuses the existing CO5300, CST9217 and Espressif codec drivers; see the [license notes](../README.md#license).

## LilyGO T-Display-S3

Board option `tdisplay-s3`, for LilyGO's 1.9" module: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 170×320 ST7789 panel, two buttons, a battery charger and a battery voltage divider. **This board has no microphone, speaker or audio codec**, so it works as a text gadget: Hermes replies, cards and prompts appear on the screen and typed messages go out. Holding TALK shows that no microphone is available. Nothing needs wiring.

Unlike the SPI panels above, this one puts the ST7789 on an **8-bit i80 parallel bus**, so the port drives the LCD_CAM unit instead of SPI.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, i80 | D0–D7 39/40/41/42/45/46/47/48, DC 7, WR 8, CS 6, RST 5; gap (0, 35); 320×170 landscape |
| Backlight | AW9364 | GPIO 38, 16-level pulse-count brightness control |
| Panel power | | GPIO 15, high before the panel starts |
| Battery | | GPIO 4 ADC through a 1:2 divider; no fuel gauge, so no percentage |
| Buttons (to GND) | | TALK = BOOT (0), CANCEL = Button2 (14) |

**Build and flash** it with PlatformIO:

```bash
cd firmware/esp32
pio run -e tdisplay-s3 -t upload -t monitor
```

Or with `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/tdisplay-s3/sdkconfig.defaults" build`. The USB-C port is the S3's own USB Serial/JTAG, so flashing and the serial console (115200 baud) both use it.

### First flash: what to check

This port is written from LilyGO's published pinout and examples. On the first flash:

1. **Boot log:** `ST7789 320x170 ready on the i80 bus (gap 0,35)`. In `hermes-gadget diag` the parts line should read `display st7789-i80`.
2. **Screen:** the mascot is centred, upright and not mirrored. If it is mirrored, swap `mirror_x`/`mirror_y`. A thin stripe at one edge means the gap is off — LilyGO warns that it is panel-specific even within one driver IC, so try neighbouring values and record what your panel needs.
3. **GPIO 15:** the panel rail. On battery power the screen stays dark unless this pin is high; the driver raises it before initialising the panel.
4. **Buttons:** BOOT is TALK and Button2 cancels. With no microphone, holding TALK flashes "Microphone unavailable" — that is expected on this board, not a fault.
5. **Text:** `say hello` over the console, or a paired Hermes, should render on the display.
6. **Backlight:** check low and full brightness, then let the idle timer turn the screen off and wake it immediately. Brightness should return to the selected setting.

Free GPIOs after the panel, buttons and battery: 1, 2, 3, 10, 11, 12, 13. An I2S microphone and amplifier need six of them, so voice is possible with external modules; the i80 bus uses LCD_CAM and audio uses I2S, so the two do not collide.

Pins and the panel setup follow LilyGO's [T-Display-S3 examples](https://github.com/Xinyuan-LilyGO/T-Display-S3) (`examples/factory`) and the [pin map](https://lilygo.cc). No physical verification report is recorded yet; see the [hardware validation table](hardware-validation.md).

## ESP32-S3-Touch-AMOLED-1.75

Board option `esp32s3-touch-amoled-175`, for Waveshare's all-in-one board: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 1.75" 466×466 AMOLED, touch, two microphones, a speaker output, a battery charger and an optional case. Nothing needs wiring; plug a small 8 Ω speaker into the **SPK** connector to hear replies.

| Part | Chip | Connection |
|---|---|---|
| Display | CO5300, QSPI | CS 12, SCLK 38, D0–D3 4/5/6/7, RST 39; column offset 6 |
| Touch | CST9217 | I2C 0x5A, RST 40 (INT 11 unused: polled) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 42, BCLK 9, WS 45, DOUT 8; amplifier enable 46 |
| Microphones | ES7210 | I2C 0x40; I2S DIN 10 (shares the bus above), MIC1 + MIC2 |
| Power | AXP2101 | I2C 0x34; battery/USB readings and local power-off; charging and rails keep their defaults |
| I/O expander | TCA9554 | I2C 0x20; P4 mirrors the PWR key |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| BOOT key | | GPIO 0 |

**Controls.** The screen is the main input:

| Do this | Does |
|---|---|
| Hold the screen | TALK: speak while holding, lift to send |
| Tap the screen | Answer "yes" to a question |
| Swipe down | CANCEL: discard a recording, close a card, stop a turn, answer "no" |
| Press the side PWR key | CANCEL as well; hold it 2 s for a new conversation |
| Hold BOOT | TALK, like holding the screen |

`set touch_cancel swipe` keeps only the swipe as CANCEL, `set touch_cancel pwr` only the PWR key, and `set touch_cancel both` restores the default. The same gestures work on the `sim-466x466-round` simulator board with the mouse.

The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) shows battery voltage, the PMIC's estimated percentage, charging and USB power. When the gauge reports no valid estimate (it is never initialized on the AMOLED-1.75C, where it reads 0 on a full cell), the percentage is estimated from the cell voltage on a linear 3.3 to 4.2 V scale instead; that estimate reads high while charging. Readings refresh every five seconds and appear in `status`, `diag` and Hermes sensor telemetry. Failed reads become unavailable; they do not retain a stale percentage. A low gauge estimate, 10 percent or less without USB, adds a reminder on the idle screen.

Select **Power off** twice to request shutdown through the AXP2101. Use PWR to turn the board on again. This is a local control; Hermes has no power-off action. USB power may affect shutdown and wake behavior, so test both power sources on your board revision. The physical PWR key retains its existing Cancel behavior and the PMIC's own long-hold behavior.

The driver follows [X-Powers' AXP2101 register documentation](https://files.waveshare.com/wiki/common/X-power-AXP2101_SWcharge_V1.0.pdf). It reads status, enabled measurements and gauge estimates. It does not change charging current, battery protection, gauge calibration or supply voltages. Battery estimates depend on the fitted cell. Physical power tests remain outstanding in the [verification table](hardware-validation.md).

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-175 -t upload -t monitor
```

The USB-C port is the S3's own USB. It shows up as a "USB JTAG/serial debug unit" (a COM port on Windows), and both flashing and the serial console use it.

### First flash: what to check

This port is written from Waveshare's published pinout and drivers. On the first flash, go through this list, and for anything that looks wrong send the report from `hermes-gadget diag --port COMx`:

1. **Boot log:** `CO5300 466x466 ready`, `codecs: speaker ready, microphones ready` and `touch ready, key ready`. A `did not answer` or `missing` line names the part to look at. The `hg.diag` lines sum it up: reset reason, memory, and `parts: display co5300, microphone es7210, speaker es8311, touch yes, key yes`. In the `diag` report, `i2c` should include `0x18` (ES8311), `0x20` (TCA9554), `0x34` (AXP2101), `0x40` (ES7210) and `0x5a` (CST9217).
2. **Console:** `hermes-gadget console --port COMx` on the USB-C port answers `status`. If the log shows but commands get no answer, the console is still on UART0.
3. **Screen:** the mascot is centred, upright and not mirrored, and the colours are right (amber accents, not blue). A thin stripe at one edge means the column offset is off.
4. **Touch:** hold the screen and the listening waves appear; a swipe *down* (not up) cancels. A reversed swipe means the touch mirroring needs flipping.
5. **PWR key:** a short press cancels and holding 2 s starts a new conversation without powering the board off.
6. **Microphone:** say something; the waves move with your voice, and Hermes's transcript is right.
7. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.

## ESP32-S3-Touch-AMOLED-1.8

Board option `esp32s3-touch-amoled-18`, for Waveshare's rectangular 1.8" board, V2 only (CO5300 display, CST820 touch): an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 368×448 AMOLED, one analog microphone, a speaker output and a battery. Nothing needs wiring; plug a small 8 Ω speaker into the **SPK** connector to hear replies.

The V1 board uses a different panel and touch controller, and this image doesn't support it. The 1.75" models also need their own profile: different pins, a round panel and different codecs.

| Part | Chip | Connection |
|---|---|---|
| Display | CO5300, QSPI | CS 12, SCLK 11, D0–D3 4/5/6/7; reset through the TCA9554 (LCD_RST); column offset 16 |
| Touch | CST820 | I2C 0x15, read with the CST816 report format; reset through the TCA9554 (TOUCH_RST); INT 21 unused (polled) |
| Speaker DAC + microphone ADC | ES8311 | I2C 0x18; I2S MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10; amplifier enable 46 |
| Microphone | ES8311 analog input | One analog electret microphone into the ES8311's own ADC (not an ES7210) |
| Power | AXP2101 | I2C 0x34; battery/USB readings and local power-off; charging and rails keep their defaults |
| Reset expander | TCA9554 | I2C 0x20; drives LCD_RST (P0), the display power rail (P1) and TOUCH_RST (P2) |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| BOOT key | | GPIO 0 |

The display and touch controllers are held in reset by the TCA9554, not by direct GPIOs. Before the display starts, the firmware pulls P0–P2 low for 20 ms, releases them and waits 150 ms; the other expander pins keep their state.

**Controls.** Hold the screen to talk, tap to answer "yes", swipe down to cancel; hold BOOT to talk as well.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-18 -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for both flashing and the serial console.

### First flash: what to check

A smoke test on a V2 board confirmed the boot, the display and the I2C devices, and the codec, touch, speaker and microphone tasks started; touch, microphone capture, speaker playback and battery are untested (see [hardware validation](hardware-validation.md)). On the first flash, check:

1. **Boot log:** `CO5300 368x448 ready`, `expander resets 0x07 ready`, `codecs: speaker ready, microphones ready (es8311 analog)` and `touch ready, key off, encoder off`. The `hg.diag` lines should read `parts: display co5300, microphone es8311, speaker es8311, touch yes, key no`. In the `diag` report, `i2c` should include `0x15` (CST820), `0x18` (ES8311), `0x20` (TCA9554) and `0x34` (AXP2101). There is no ES7210 on this board.
2. **Screen:** the mascot is upright and not mirrored, and the colours are right (amber accents, not blue); a stripe at one edge means the column offset is off. If the image is shifted or the panel stays dark, the vendor init table is the first thing to revisit.
3. **Touch:** holding the screen shows the listening waves and a swipe *down* cancels. If the coordinates are mirrored, flip `touch.mirror_x`/`mirror_y` in `board.cpp`.
4. **Microphone:** say something; the waves move with your voice and Hermes's transcript is right. The ES8311 analog input path is the part most likely to need its gain adjusted.
5. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.

The CO5300 start-up table comes from Waveshare's [board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9f4030c6e5cb888ad4cc268bfa7584c93ad53e30/bsp/esp32_s3_touch_amoled_1_8) at commit `9f4030c`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## Xorigin AIPI Lite

Board option `aipi-lite`, for Xorigin's AIPI Lite: an ESP32-S3 with 16 MB flash and 8 MB octal PSRAM, a 128×128 ST7789 panel, one ES8311 codec for both the speaker and the microphone, BOOT and power keys, and a battery. This port is experimental.

**Back up the factory data before the first flash.** The AIPI Lite keeps its credentials for Xorigin's own service in a factory partition at `0x9000`, where this firmware's settings start, so flashing Hermes Gadget overwrites it. Save it first:

```bash
esptool.py --chip esp32s3 read_flash 0x9000 16384 nvsfactory.bin
```

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI | MOSI 17, SCLK 16, CS 15, DC 7, RST 18; 20 MHz, BGR; backlight GPIO 3 |
| Speaker DAC + microphone ADC | ES8311 | I2C 0x18 on SDA 5, SCL 4; I2S MCLK 6, BCLK 14, WS 12, DOUT 11, DIN 13; amplifier enable 9 |
| Power | | GPIO 10 latches the supply on and powers the codec; charge signal on GPIO 8; battery divider on GPIO 2 (ADC1 channel 1) |
| Keys | | BOOT GPIO 42 (TALK), power GPIO 1 (CANCEL) |

The battery level and charging state aren't reported yet: the divider ratio on GPIO 2 hasn't been measured, and the charge signal's polarity isn't confirmed.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e aipi-lite -t upload -t monitor
```

The profile expects the USB-C port to be the S3's own USB Serial/JTAG, used for flashing and the serial console. That is inferred from the pin map and not yet confirmed on a board.

### First flash: what to check

1. **Boot log:** `battery latch enabled; no battery voltage reading` and `codecs: speaker ready, microphones ready (es8311 analog)`. The `hg.diag` parts line should read `display st7789, microphone es8311, speaker es8311`.
2. **Screen:** the mascot is upright and not mirrored, and the colors are right (amber accents, not blue). Swapped red and blue means `lcd.bgr` in `board.cpp` is wrong.
3. **Microphone:** hold BOOT and speak; the waves move with your voice and Hermes's transcript is right.
4. **Speaker:** replies are clear and loud enough (`set volume 80`).
5. **Power:** unplug USB with the battery in, and the device stays on. **Power off** in settings turns it off.
6. **Console:** `hermes-gadget diag` answers over the USB-C port.

The pin map follows xiaozhi-esp32's `xorigin/aipi-lite` board at commit `0d576d3`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## Elecrow CrowPanel 2.1-inch HMI

Board option `crowpanel-21`, for Elecrow's 2.1" round rotary display: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 480×480 round ST7701 panel on a 16-bit RGB bus, a CST-family capacitive touchscreen, a rotary encoder with a push button, and a PCF8574 I/O expander for panel power, resets and the encoder button. **This board has no microphone or speaker**, so it works as a text gadget: Hermes replies, cards and prompts appear on the screen and typed messages go out through the USB console.

Unlike the SPI and i80 panels above, the ST7701 takes its initialization commands over a 3-wire SPI link and then streams pixels continuously over the **16-bit RGB bus** (`esp_lcd_st7701` + `esp_lcd_panel_io_additions` managed components).

| Part | Chip | Connection |
|---|---|---|
| Display | ST7701, 16-bit RGB | DE 40, VSYNC 7, HSYNC 15, PCLK 41 at 12 MHz; data 46/3/8/18/17/14/13/12/11/10/9/5/45/48/47/21; 480×480 round |
| Panel init | 3-wire SPI | CS 16, SCLK 2, SDA 1 |
| Backlight | PWM | GPIO 6, active-high |
| I/O expander | PCF8574 @ 0x21 | P0 touch reset, P2 touch IRQ, P3 LCD power, P4 LCD reset, P5 encoder button |
| Touch | CST826 @ 0x15 | I2C SDA 38, SCL 39; read with the CST816 report format (`TouchController::Cst816`) |
| Rotary encoder | | A 42, B 4; button through the expander |

**Build and flash** it with PlatformIO:

```bash
cd firmware/esp32
pio run -e crowpanel-21 -t upload -t monitor
```

Or with `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/crowpanel-21/sdkconfig.defaults" build`. The USB-C port is the S3's own USB Serial/JTAG, so flashing and the serial console (115200 baud) both use it.

### First flash: what to check

This port is written from Elecrow's published factory example ([CrowPanel repository](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen)). On the first flash:

1. **Boot log:** `ST7701 480x480 RGB panel initialized at 12000000 Hz`, `touch ready, key ready, encoder ready`, and `parts: display st7701-rgb, ... touch yes, key yes`.
2. **Screen:** the mascot is centred inside the circle and not mirrored; colours are right (amber accents, not blue). A red/blue swap means the RGB565 channel order needs flipping.
3. **Brightness:** cycle 10 → 100 in settings; brightness must rise with the number. The backlight PWM is active-high — do not set `backlight_invert`.
4. **Touch:** hold the screen and the listening waves appear; a swipe down cancels; hold the title bar one second for settings.
5. **Knob:** each detent moves one settings item or scrolls a long reply, and the first detent wakes a dark screen; pressing the knob cancels. If the direction is reversed, swap the encoder's A and B pins in `board.cpp`.
6. **Text:** `say hello` over the console, or a paired Hermes, should render on the display.

Panel timings (12 MHz PCLK, 10/4/20 porches) and the ST7701 init table follow the factory `RotaryScreen_2_1` Arduino example at commit `faf8ecf`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md). Display bring-up, the brightness scale, Wi-Fi, pairing and the serial console are smoke-checked on real hardware; touch coordinates and encoder direction are not yet physically verified — see the [hardware validation table](hardware-validation.md).

## Elecrow CrowPanel 5.0-inch HMI

Board option `crowpanel-5.0`, for Elecrow's 5.0" landscape display (DIS07050H): an ESP32-S3-WROOM-1-N4R8 (8 MB octal PSRAM) with 4 MB flash, an 800×480 ILI6122/ILI5960 panel on a 16-bit RGB bus, and a GT911 capacitive touchscreen. **This board has no microphone or speaker**, so it works as a text gadget: Hermes replies, cards and prompts appear on the screen and typed messages go out through the USB console.

Unlike the CrowPanel 2.1-inch, the ILI6122 needs **no command bring-up**: it is scanned out continuously over the 16-bit RGB bus through the built-in `esp_lcd_rgb` driver, with no PCF8574 expander and no panel-enable line. The `RgbDisplay` port picks this path from `RgbController::Ili6122`. GPIO 38 is the user `GPIO_D` header pin, and the firmware leaves it alone.

| Part | Chip | Connection |
|---|---|---|
| Display | ILI6122/ILI5960, 16-bit RGB | DE 40, VSYNC 41, HSYNC 39, PCLK 0 at 15 MHz; data 8/3/46/9/1/5/6/7/15/16/4/45/48/47/21/14 (B0..B4, G0..G5, R0..R4); 800×480 |
| Backlight | PWM | GPIO 2, active-high |
| Touch | GT911 @ 0x5D/0x14 | I2C SDA 19, SCL 20; INT not wired, so the driver polls (`TouchController::Box3` managed GT911) |

**Build and flash** it with PlatformIO:

```bash
cd firmware/esp32
pio run -e crowpanel-5 -t upload -t monitor
```

Or with `idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/crowpanel-5.0/sdkconfig.defaults" build`. The USB-C port reaches the S3's UART0 through a USB-UART bridge, so flashing and the serial console (115200 baud) both use it. The S3's own USB pins, GPIO 19 and 20, carry the touch I2C instead. On the v3.0 hardware revision the board also carries a PCA9557 GPIO expander (@ 0x18) that gates touch timing; this port polls touch and does not drive that expander.

### First flash: what to check

This port is written from Elecrow's published factory example ([CrowPanel 5.0 repository](https://github.com/Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480) at commit `fd959ac`). On the first flash:

1. **Boot log:** `ILI6122 800x480 RGB panel initialized at 15000000 Hz` and `touch ready`; `parts: display ili6122-rgb, ... touch yes`.
2. **Screen:** the mascot renders and colours are correct (amber accents, not blue). A red/blue swap means `b.lcd.rgb.swap_red_blue` in this board's `board.cpp` entry needs flipping.
3. **Brightness:** cycle 10 → 100 in settings; brightness must rise with the number. The backlight is active-high — do not set `backlight_invert`.
4. **Touch:** hold the screen and the listening waves appear; a swipe down cancels; hold the title bar one second for settings.
5. **Text:** `say hello` over the console, or a paired Hermes, should render on the display.

Panel timings (15 MHz PCLK, 8/4/43 and 8/4/12 porches, `pclk_active_neg`) and the pin map follow the factory `crowpanel-esp32-5.0-3.0` Arduino example at commit `fd959ac`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md). A boot smoke check passed on a real board: the panel initialization log, the GT911's ID, Wi-Fi and pairing. Rendering, colours, brightness and touch coordinates are not yet verified; see the [hardware validation table](hardware-validation.md).

## Build and flash

**No toolchain needed:** the [browser installer](https://adolanium.github.io/hermes-gadget-sdk/) flashes each release's prebuilt firmware from Chrome or Edge, then sets up Wi-Fi and pairing. The release files are also on the [releases page](https://github.com/Adolanium/hermes-gadget-sdk/releases), for `esptool.py write_flash 0x0 hermes-gadget-<board>-<version>.bin`, which also erases the board's settings.

To build it yourself, with ESP-IDF 5.3 or later installed (`. $IDF_PATH/export.sh`):

```bash
cd firmware/esp32
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-breadboard/sdkconfig.defaults" build
idf.py -p COM5 flash monitor
```

Or with PlatformIO, which downloads ESP-IDF itself:

```bash
cd firmware/esp32
pio run -e esp32s3-breadboard -t upload -t monitor
```

The firmware is written for ESP-IDF 5.3 and later. It builds with ESP-IDF 6.1 through PlatformIO:

- ESP32-S3 breadboard: app image 1.13 MB in a 1.94 MB app slot, 14% of static RAM;
- ESP32-S3-Touch-AMOLED-1.75: 1.12 MB, otherwise the same;
- classic ESP32 with custom pins.

The flash holds two app slots (`partitions.csv`), so later firmware can arrive over the air. A board flashed with an earlier release gets the new partition table with its next USB flash. NVS stays where it was, so its settings and device key survive.

## Updates over the air

Once a board runs this firmware, new firmware can reach it over Wi-Fi. On the Hermes host:

```bash
hermes gadget update "Kitchen" --latest     # the newest release's firmware for Kitchen's board
hermes gadget update "Kitchen" firmware/esp32/.pio/build/esp32s3-touch-amoled-175/firmware.bin
```

- `--latest` reads the newest [release](https://github.com/Adolanium/hermes-gadget-sdk/releases)'s manifest, picks the image for the device's board, and checks its size and SHA-256 against the manifest before using it. A device that already runs that version is left alone; `--force` installs it again. `hermes gadget devices` shows the version each device runs.
- The gateway installs the image as soon as the device is online, and the command waits and reports progress (`--no-wait` returns at once).
- The device shows the progress, restarts into the new firmware, and keeps it once it reaches Hermes again.
- If the new firmware doesn't reach Hermes within 5 minutes, or crashes before then, the device goes back to the previous one by itself.
- Only the Hermes that enrolled the device can update it: every image is authorized with the device's own key. The command also refuses an image built for another board.
- The dev server takes the same images: `update <path>` in its console.

On Windows, keep the project on a short path such as `C:\src\hermes-gadget-sdk`. ESP-IDF's linker-script step can exceed the Windows command-line length limit when the build directory is deeply nested.

`idf.py menuconfig` → **Hermes Gadget** sets the board, default Wi-Fi, server URL and device name. All of them can also be changed at run time over serial.

## Serial console

Open the console at 115200 baud (`idf.py monitor`, `pio device monitor`, or `hermes-gadget console --port COM5`). It shares its command set with the simulator:

```
gadget> set wifi_ssid MyWifi
@ok wifi_ssid
gadget> set wifi_pass secret
@ok wifi_pass
gadget> set server ws://192.168.1.20:8765/gadget
@ok server
gadget> status
@status {"device_id":"hg-...","phase":"online","screen":"pairing","pairing_code":"ABCD2345",...}
```

| Command | Meaning |
|---|---|
| `status` | JSON status |
| `diag` | JSON diagnostics: build, reset reason, memory, Wi-Fi, which parts came up, I2C addresses that answer, task stacks, connection |
| `diag log` | The last few KB of log lines, kept in RAM since boot |
| `get <key>` | Read a setting (secrets are masked) |
| `set <key> <value>` | Write a setting; an empty value clears it |
| `say <text>` | Send a typed message |
| `talk` / `release` | Press or release TALK (bench automation) |
| `cancel` | Press CANCEL |
| `new-session` | Start a fresh conversation (same as holding CANCEL for 2 s) |
| `settings` / `settings close` | Open or close the local settings and hardware checks |
| `wifi-setup` / `wifi-setup close` | Start or cancel phone setup; the start response includes temporary network credentials |
| `yes` / `no` | Answer the question on screen |
| `reconnect` | Drop and re-open the Hermes connection |
| `forget-key` | New device identity on next boot (re-enrollment and re-pairing) |
| `factory-reset` | Erase the device key and all settings |

The keys are `name`, `server`, `token`, `talk_mode` (`hold` or `tap`), `volume`, `brightness`, `screen_timeout` (0..3600 seconds, 0 disables it), `wifi_ssid` and `wifi_pass`. The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) saves volume, brightness, talk mode and screen timeout without a console.

In the `diag` report, `i2c` lists the addresses that answered, such as `0x18`. A probe can time out while another task uses the bus, so the scan tries each address up to three times. An address that times out on every try appears as `timeout at 0x19`, and the scan continues. Four such addresses in a row end the scan with `bus stuck at` the first of them, such as `bus stuck at 0x08`. That usually means a device or a short holds SDA or SCL low.

Machine-readable lines start with `@`, so tools can drive a bench device. `hermes-gadget provision` is a thin wrapper over these commands.

## Power-on sequence

1. **Boot:** about 1 s.
2. **Wi-Fi:** credentials from NVS, else from menuconfig. Without credentials the device opens a temporary network for [phone setup](setup-board.md#set-up-wi-fi-with-your-phone).
3. **Connect:** the device opens the WebSocket to `server`, retrying 1 → 30 s with backoff.
4. **Authenticate:** it enrolls its key on first contact and proves it with an HMAC afterwards.
5. **Pair or ready:** an unpaired device shows a pairing code (approve with `hermes gadget pair`); a paired one goes to **Ready**.

## Known limits of the reference firmware

- Push-to-talk or tap with energy VAD. There is no wake word; the protocol leaves room for one (`audio.start.mode`).
- Wi-Fi setup works over USB or a temporary password-protected network. There is no BLE setup or dedicated phone app.
- Firmware updates are authorized with the device key, but images aren't signed: the bootloader runs whatever a trusted Hermes installs. Secure Boot isn't enabled.
- The text font is ASCII only; the host folds other characters.
