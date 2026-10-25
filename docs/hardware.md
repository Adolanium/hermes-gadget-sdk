# Hardware

The firmware is an ESP-IDF application (`firmware/esp32`) built on the portable core. The reference board uses common modules you can wire on a breadboard. Other boards need a configuration and, where necessary, drivers; see [porting.md](porting.md).

Start with [Supported hardware](supported-hardware.md) for a board comparison, then check [capabilities and verification](hardware-validation.md). The release builds are experimental until a physical report is recorded for the exact model and revision.

## Requirements

- **Chip:** the release profiles target ESP32-S3. Other ESP32 variants need their own configuration and validation. On a board without PSRAM, either the whole-panel framebuffer (width × height × 2 bytes) fits in internal RAM next to Wi-Fi and audio buffers, or the board holds a band of rows and lets the UI render in bands (see `Display::framebuffer_rows()` and the CYD below).
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

Board option `esp32s3-lcd-154`, for Waveshare's all-in-one 1.54" board (SKUs 33866/33867; the `-EN` SKU is the same hardware): an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 240×240 ST7789 panel over SPI, an ES8311 DAC and an ES7210 microphone ADC with two microphones, an NS4150B amplifier, a speaker, a QMI8658 6-axis IMU, a TF card slot, a battery charger and the BOOT / PLUS / PWR keys. Nothing to wire and nothing to connect, it works out of the box. The touch version (`ESP32-S3-Touch-LCD-1.54`, SKUs 33868/33869) adds a CST816 touchscreen, which the same profile drives; on the non-touch SKUs nothing answers at its address and the touchscreen is reported absent.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI | SCLK 38, MOSI 39, CS 21, DC 45, RST 40, BL 46 (LEDC) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 8, BCLK 9, WS 10, DOUT 12; amplifier enable 7 |
| Microphones | ES7210 | I2C 0x40; I2S DIN 11 (shares the bus above), MIC1 + MIC2 |
| Touch (touch version) | CST816 | I2C 0x15 on the bus below; RST 47 |
| IMU | QMI8658 | I2C 0x6B |
| I2C bus | | SDA 42, SCL 41, 400 kHz |
| Buttons (to GND) | | TALK = BOOT (0), CANCEL = PLUS (4), power key = PWR (5) |
| Battery | ETA6098 | GPIO 1 (BAT_ADC), GPIO 2 (power latch), GPIO 3 (CHG_STAT) |

The PWR key is part of the power circuit and also reads low on GPIO 5 while pressed (`latch_power.key`). A short press turns the screen off and on; holding it for 2.5 seconds lowers GPIO 2, which powers the board off on battery. Firmware asserts GPIO 2 at startup to keep the battery path enabled. On the touch version, holding the screen talks and a swipe down cancels, as on other touch boards. The TF card and QMI8658 are unused.

The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) shows calibrated battery-node voltage and the charging signal. GPIO 1 samples the R27/R32 divider, whose ratio is three. GPIO 3 reads the active-low charging signal. This board has no fuel gauge, battery presence detector or USB-status input, so those readings are omitted. External power is reported while CHG_STAT reads charging or a USB host is connected to the USB-C port (`latch_power.usb_external_power`), and battery otherwise, so Wi-Fi modem sleep applies on battery; a USB charger without data that has finished charging counts as battery. Voltage is unavailable if the ESP32's ADC calibration cannot start. An unplugged battery can leave voltage on this node while USB powers the charger; voltage alone does not prove a battery is attached.

After confirming a compatible single-cell 3.7 V Li-ion battery is fitted, opt into a rough display-only percentage with `set battery_estimate liion`, then `set battery_display percentage`. See [battery indicator settings and limitations](using-gadget.md#device-settings-and-hardware-checks). It is off by default, stays visible while charging, and does not change raw gateway telemetry or establish charge completion. Do not enable it merely because this node has voltage; USB can energize the node without a battery. Simulator checks and firmware compilation do not calibrate or physically verify this estimate.

Select **Power off** twice to lower GPIO 2 and disconnect battery power. **USB continues to power the board.** Disconnect USB to turn it off, and hold PWR to turn it back on from battery. Holding PWR for 2.5 seconds while the board runs does the same as **Power off**; on USB the screen goes dark instead. BOOT and PLUS retain their TALK and CANCEL roles. The optional screen timeout dims and darkens the display while keeping Wi-Fi connected.

While the screen is dark, on battery and idle, the chip dozes: the CPU drops to 80 MHz when idle at any time, and while dozing it takes light sleep between Wi-Fi beacons, waking every 100 ms to read the keys and, on the touch version, the touchscreen. The battery latch, the panel's control lines and the amplifier enable keep their level through light sleep. Light sleep never runs while a USB host is connected (the console and JTAG would stop), so a doze shows only on battery.

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

## Waveshare ESP32-S3-Knob-Touch-LCD-1.8

Use `esp32s3-knob-touch-lcd-1.8` for the knob board: an ESP32-S3R8 with 16 MB flash and 8 MB octal PSRAM, a round 360×360 SH8601 LCD over QSPI (not the ST77916 of the 1.85C V2), CST816S touch, one PDM microphone, a rotary encoder around the screen, a DRV2605 haptic driver and a Li-ion battery with charging management.

**This port is text-only.** It doesn't drive the board's PCM5100A DAC and 3.5 mm headphone jack, so replies are shown on the screen and never spoken. Waveshare's ESP-IDF audio demo plays through that DAC from the S3 (I2S BCLK 39, WS 40, DOUT 41); this port has not tried it. The clone Guition JC3636K518 uses the same hardware and should work the same way. This port is experimental; display, touch and microphone were verified on hardware (360×360 round, hold-to-talk).

| Part | Connection |
|---|---|
| SH8601 LCD, QSPI | CS 14, clock 13, D0–D3 15/16/17/18, reset 21, backlight 47 (LEDC) |
| CST816S touch | I2C 0x15, INT 9, reset 10 |
| I2C | SDA 11, SCL 12, 400 kHz |
| PDM microphone | CLK 45, DATA 46 |
| Rotary encoder | A 8, B 7 (each detent scrolls; the knob's push button is not wired) |
| BOOT | GPIO 0, TALK |

Hold the screen to talk, release to send, swipe down to cancel. The IMU, RTC, TF card, haptic driver and battery ADC are not used.

```bash
cd firmware/esp32
pio run -e esp32s3-knob-touch-lcd-18 -t upload -t monitor
```

For ESP-IDF, use `SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-knob-touch-lcd-1.8/sdkconfig.defaults"` with a separate build directory and sdkconfig. The USB-C port handles flashing and the serial console (115200 baud); flipping the cable flashes the other MCU. The `sim-360x360-knob` simulator board has the same screen and audio capabilities.

References: [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8) and its [ESP-IDF and Arduino demo](https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip), the source of the pins above and the SH8601 initialization table. The table's source is listed in [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

### First flash: what to check

- The panel logs `sh8601 360x360 ready`. The knob ships an SH8601 glass; if a future unit logs an ST77916 panel ID instead, it has a different panel and needs the ST77916 bring-up table.
- The encoder scrolls the reply list; each detent is one step.
- The microphone captures on a hold: check the VU feedback on screen, and that released audio is recognized.

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

The firmware enables ALDO1 for audio while preserving the other rails and charging settings. To save battery, the amplifier (GPIO 46) is powered only while sound plays, the ES7210 microphones only while recording, and the CPU drops from 240 to 80 MHz when idle (ESP-IDF power management). On battery, with the screen off and nothing happening, it also light-sleeps between events while staying connected to Wi-Fi and Hermes. It stays awake while it reconnects to Hermes, including for as long as Wi-Fi is up but Hermes can't be reached; on USB it never light-sleeps, so the serial console, flashing and JTAG keep working. Battery readings and local power-off use the existing AXP2101 driver. An unavailable ADC or gauge reading remains absent. Use the settings menu for microphone, speaker, display, touch, volume, brightness and power checks.

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
| Motion sensor | QMI8658 | I2C 0x6B; accelerometer only, read at 10 Hz (face-down sleep) |
| Clock chip | PCF85063 | I2C 0x51; keeps the time in UTC across resets, set from network time (resting clock) |
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

The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) shows battery voltage, the PMIC's estimated percentage, charging and USB power. When the gauge reports no valid estimate, the percentage is estimated from the cell voltage on a linear 3.3 to 4.2 V scale instead; that estimate reads high while charging. The AMOLED-1.75C uses curves measured on its 500 mAh cell instead. On battery, the percentage follows a discharge curve at the median voltage of the last minute and never rises until the board is charged. While charging, it follows a charge curve, never falls, and stays at most 99% until the AXP2101 reports charging done, then shows 100, including while the AXP2101 later tops up the cell on USB. The discharge curve was measured at about 93 mA with the screen off, so a heavier load reads a few points low, and its 80 to 95% range is extrapolated. The charge curve assumes the charge current stays constant; `diag` reports the AXP2101's limit as `charge_current_ma` (200 mA on the measured board, set in eFuse; the datasheet's charger section gives 300 mA). Readings refresh every five seconds and appear in `status`, `diag` and Hermes sensor telemetry. Failed reads become unavailable; they do not retain a stale percentage. A low gauge estimate, 10 percent or less without USB, adds a reminder on the idle screen. The [battery indicator](using-gadget.md#device-settings-and-hardware-checks), off by default, can show the same percentage at the top of the screen.

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

1. **Boot log:** `CO5300 466x466 ready`, `codecs: speaker ready, microphones ready` and `touch ready, key ready`. A `did not answer` or `missing` line names the part to look at. The `hg.diag` lines sum it up: reset reason, memory, and `parts: display co5300, microphone es7210, speaker es8311, touch yes, key yes, imu qmi8658, rtc pcf85063`. In the `diag` report, `i2c` should include `0x18` (ES8311), `0x20` (TCA9554), `0x34` (AXP2101), `0x40` (ES7210), `0x51` (PCF85063), `0x5a` (CST9217) and `0x6b` (QMI8658). The `diag` report also has a `time` object once the clock chip or the network supplies the time.
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
| Reset expander | TCA9554 | I2C 0x20; drives LCD_RST (P0), the display power rail (P1) and TOUCH_RST (P2); reads the PWR key (P4, high while pressed) |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| BOOT key | | GPIO 0 |

The display and touch controllers are held in reset by the TCA9554, not by direct GPIOs. Before the display starts, the firmware pulls P0–P2 low for 20 ms, releases them and waits 150 ms; the other expander pins keep their state.

**Controls.** Hold the screen to talk, tap to answer "yes", swipe down to cancel; hold BOOT to talk as well. PWR is CANCEL: a press cancels, and holding it for two seconds starts a new conversation.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-18 -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for both flashing and the serial console.

### First flash: what to check

A smoke test on a V2 board confirmed the boot, the display and the I2C devices, and the codec, touch, speaker and microphone tasks started; touch, microphone capture, speaker playback and battery are untested (see [hardware validation](hardware-validation.md)). On the first flash, check:

1. **Boot log:** `CO5300 368x448 ready`, `expander resets 0x07 ready`, `codecs: speaker ready, microphones ready (es8311 analog)` and `touch ready, key off, encoder off`. The `hg.diag` lines should read `parts: display co5300, microphone es8311, speaker es8311, touch yes, key no, imu none`. In the `diag` report, `i2c` should include `0x15` (CST820), `0x18` (ES8311), `0x20` (TCA9554) and `0x34` (AXP2101). There is no ES7210 on this board.
2. **Screen:** the mascot is upright and not mirrored, and the colours are right (amber accents, not blue); a stripe at one edge means the column offset is off. If the image is shifted or the panel stays dark, the vendor init table is the first thing to revisit.
3. **Touch:** holding the screen shows the listening waves and a swipe *down* cancels. If the coordinates are mirrored, flip `touch.mirror_x`/`mirror_y` in `board.cpp`.
4. **Microphone:** say something; the waves move with your voice and Hermes's transcript is right. The ES8311 analog input path is the part most likely to need its gain adjusted.
5. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.

The CO5300 start-up table comes from Waveshare's [board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9f4030c6e5cb888ad4cc268bfa7584c93ad53e30/bsp/esp32_s3_touch_amoled_1_8) at commit `9f4030c`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## ESP32-S3-Touch-AMOLED-2.06

Board option `esp32s3-touch-amoled-206`, for Waveshare's 2.06" AMOLED touch watch: an ESP32-S3R8 (8 MB octal PSRAM) with 32 MB flash, a 410×502 AMOLED with rounded corners, two microphones, an onboard speaker, a battery, an IMU, an RTC and a TF slot. Nothing needs wiring.

| Part | Chip | Connection |
|---|---|---|
| Display | CO5300, QSPI | CS 12, SCLK 11, D0–D3 4/5/6/7, reset 8; column offset 22; tearing-effect output on 13 (unused) |
| Touch | FT3168 | I2C 0x38, read with the FT5x06 driver; reset 9; INT 38, read on its falling edge (the idle chip dozes, refuses reads and reports its waking touch only briefly) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 16, BCLK 41, WS 45, DOUT 40; NS4150B amplifier enable 46 |
| Microphones | ES7210 | I2C 0x40; two microphones on MIC1/MIC2 (MIC3 is the speaker loopback, unused); I2S DIN 42 |
| Power | AXP2101 | I2C 0x34; battery/USB readings, PWR short press and local power-off; charging and rails keep their defaults |
| Clock chip | PCF85063 | I2C 0x51; keeps the time in UTC across resets, set from network time (resting clock) |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| BOOT key | | GPIO 0 |
| Not used | QMI8658 IMU (I2C 0x6B), TF card | |

The audio codecs and amplifier run from the always-on 3.3 V rail, so no AXP2101 rail is switched for audio. The UI uses rows 8–493 of the panel. The glass corners are rounded with a radius of about 100 px (measured with a test grid), so the status row also keeps its text 64 px (`corner_inset`) in from the sides.

The microphones are quiet: speech averaged about 170 RMS at the default 24 dB gain, under tap mode's usual 500 floor. This profile sets 30 dB and a floor of 200 (`codec.mic_gain_db`, `codec.vad_min_rms`), and tap mode's noise estimate still raises the threshold in a loud room.

The CPU drops to 80 MHz when idle, but light sleep stays disabled. The FT3168's short interrupt pulse is not configured as a CPU wake source, and its report can expire before a sleeping CPU reads it. Enabling light sleep needs GPIO wake support and a physical wake test on battery. The [partial physical report](hardware-validation.md#waveshare-amoled-206-partial-physical-report) covers USB operation; the port stays experimental.

Two more hardware facts for anyone building on this board. The QMI8658 is mounted a quarter turn from the screen: its +x axis points to the screen's bottom, +y to its left and +z out of the back (checked by tilting the watch). The schematic's vibration motor is not fitted: there is a driver (an NPN from GPIO 18) and two pads, P1 (the AXP2101's ALDO3) and P2, for a motor.

**Controls.** Hold the screen to talk, tap to answer "yes", swipe down to cancel; hold BOOT to talk as well. A short press of PWR is read from the AXP2101; holding PWR for 6 s turns the watch off in hardware.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-206 -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for both flashing and the serial console. If the port does not appear, hold BOOT while plugging in USB to enter download mode.

### First flash: what to check

1. **Boot log:** `CO5300 410x486 ready`, `codecs: speaker ready, microphones ready`, `touch read on INT (GPIO 38)` and `touch ready, key off, encoder off`. In the `diag` report, `i2c` should include `0x18` (ES8311), `0x34` (AXP2101), `0x40` (ES7210), `0x51` (PCF85063) and `0x6b` (QMI8658). `0x38` (FT3168) is listed only while the touch chip is awake: it dozes after a few idle seconds and doesn't answer then. When the PCF85063 answers, the parts line adds `rtc pcf85063`, and `diag` reports a `time` object once the time is known.
2. **Screen:** the mascot is upright and not mirrored; text is white on dark slate bars, and the link dot is green once online. A stripe at one edge means the column offset is off. Text clipped by a rounded corner means `corner_inset` needs raising.
3. **Touch:** holding the screen shows the listening waves, and a swipe *down* cancels. Report mirrored coordinates with the board revision; this FT5x06 adapter does not apply `touch.mirror_x` or `touch.mirror_y`. If they are 8 rows off, adjust `touch.offset_y`.
4. **Microphones:** say something. The waves move with your voice and the transcript is right; in tap mode, a sentence spoken at arm's length starts and ends the recording.
5. **Speaker:** replies are clear and loud enough (`set volume 80`), with no hiss between replies.
6. **Power:** a short PWR press toggles the screen, and the battery reading appears with a cell attached.

The CO5300 start-up table and the pin map come from Waveshare's [board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9daffb7168b3c093a330da700988a22092294eef/bsp/esp32_s3_touch_amoled_2_06) 2.0.0 at commit `9daffb7`, checked against the [schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf); see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## ESP32-S3-Touch-AMOLED-2.16

Board option `esp32s3-touch-amoled-216`, for Waveshare's 2.16" AMOLED touch development board (SKUs 33969 and 33970, the -EN version): an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a square 480×480 AMOLED, two microphones with echo-cancellation circuitry, a speaker output, an AXP2101 PMIC with an MX1.25 header for a 3.7 V lithium battery, an IMU, an RTC and a TF slot. Nothing needs wiring.

| Part | Chip | Connection |
|---|---|---|
| Display | CO5300, QSPI | CS 12, SCLK 38, D0–D3 4/5/6/7, reset 39; full 480×480 window, no column offset |
| Touch | CST9220 | I2C 0x5A, read with the CST9217 report; reset 40; INT 11; y mirrored, then x and y swapped, to match the display's MADCTL 0xA0, as in Waveshare's BSP |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 42, BCLK 9, WS 45, DOUT 8; amplifier enable 46 |
| Microphones | ES7210 | I2C 0x40; two microphones plus echo cancellation; I2S DIN 10 |
| Power | AXP2101 | I2C 0x34; battery/USB readings, PWR short press and local power-off; charging and rails keep their defaults |
| Clock chip | PCF85063 | I2C 0x51; keeps the time in UTC across resets, set from network time (resting clock) |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| KEY3 | | GPIO 18 (not wired as a gadget control) |
| BOOT key | | GPIO 0 |
| Not used | QMI8658 IMU (I2C 0x6B), TF card | |

No AXP2101 output is switched for audio, as in Waveshare's BSP, which does not configure the PMIC. Battery operation has not been tested. This port stays **experimental** until a full [physical verification report](hardware-validation.md) is recorded for the exact board.

**Controls.** Hold the screen or BOOT to talk, tap to answer "yes", swipe down to cancel. A short press on PWR turns the screen off and on. KEY3 (GPIO18) is not wired as a gadget control in this profile.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-216 -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for both flashing and the serial console. If the port does not appear, hold BOOT while plugging in USB to enter download mode.

### First flash: what to check

1. **Boot log:** `CO5300 480x480 ready`, `codecs: speaker ready, microphones ready`, plus `touch ready, key off, encoder off`. In the `diag` report, `i2c` should include `0x18` (ES8311), `0x34` (AXP2101), `0x40` (ES7210), `0x51` (PCF85063), `0x5a` (CST9220) and `0x6b` (QMI8658). When the PCF85063 answers, the parts line adds `rtc pcf85063`, and `diag` reports a `time` object once the time is known.
2. **Screen:** the mascot is upright and not mirrored; text is white on dark slate bars, and the link dot is green once online. A stripe at one edge means the column offset is off.
3. **Touch:** holding the screen shows the listening waves, and a swipe *down* cancels. Report mirrored or offset coordinates with the board revision.
4. **Microphones:** say something. The waves move with your voice and the transcript is right; in tap mode, a sentence spoken at arm's length starts and ends the recording.
5. **Speaker:** replies are clear and loud enough (`set volume 80`), with no hiss between replies.

The CO5300 start-up table and the pin map come from Waveshare's [board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/63ac675334db62509082e182ff900bf60f21f009/bsp/esp32_s3_touch_amoled_2_16) 2.0.1 at commit `63ac675`, checked against the [product documentation](https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16); see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

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

## Spotpear ESP32-S3-1.54 muma

Board option `spotpear-muma`, for Spotpear's 1.54" "muma" (little wooden horse) board, SKU `sp-esp32-s3-1.54-muma`: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 240×240 ST7789 panel over SPI, one ES8311 codec for both the speaker and the microphone, a speaker, a BOOT key, a battery connector and, on the touch version, a CST816D touchscreen. It ships with the XiaoZhi firmware, whose boot log prints `SKU=sp-esp32-s3-1.54-muma`. This port is experimental.

**Back up the whole flash before the first flash.** The board has no other copy of its factory firmware:

```bash
esptool.py --chip esp32s3 read_flash 0 0x1000000 muma-factory.bin
```

Writing that file back at `0x0` restores the original firmware.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI | MOSI 2, SCLK 4, CS 5, DC 47, RST 38; colors inverted; backlight GPIO 42, active low |
| Speaker DAC + microphone ADC | ES8311 | I2C 0x18 on SDA 15, SCL 14; I2S MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10; amplifier enable 46 |
| Touch (touch version) | CST816D | I2C 0x15 on its own bus, SDA 11, SCL 7; RST 6, INT 12 |
| Power | | GPIO 3 latches the battery path on; charge signal on GPIO 41 (high while charging); battery divider by two on GPIO 1 (ADC1 channel 0) |
| Keys | | BOOT GPIO 0 (TALK) |

The touchscreen has an I2C bus of its own, so the profile sets `touch.bus` and the firmware opens it on the second I2C controller. Without touch there is no CANCEL input: BOOT is the only key. Hold the screen or BOOT to talk, and swipe down to cancel.

The settings menu's **Battery and power** page shows the calibrated battery voltage and the charging signal. The divider ratio of two is inferred from upstream's battery table (raw 1980 to 2480 at 12 dB, about 3.1 to 4.0 V at the cell), not measured with a meter. The charge signal reads high while charging (`latch_power.charging_active_high`). This board has no fuel gauge; a percentage needs the opt-in estimate described under [device settings](using-gadget.md#device-settings-and-hardware-checks).

The board reports external power while the charging signal is high or a USB host is connected to the USB-C port (`latch_power.usb_external_power`), and battery otherwise, so Wi-Fi modem sleep applies on battery. A USB charger without data that has finished charging counts as battery.

While the screen is dark, on battery and idle, the chip dozes: the CPU drops to 80 MHz when idle at any time, and while dozing it takes light sleep between Wi-Fi beacons, waking every 100 ms to read the keys and the touchscreen. The battery latch, the panel's control lines and the amplifier enable keep their level through light sleep. Light sleep never runs while a USB host is connected (the console and JTAG would stop), so a doze shows only on battery.

Upstream also writes `0xBB 0x38` (VCOM) to the panel; this profile keeps the ST7789 default.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e spotpear-muma -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for flashing and the serial console.

### First flash: what to check

1. **Boot log:** `battery latch enabled; calibrated voltage available` and `codecs: speaker ready, microphones ready (es8311 analog)`. The `hg.diag` parts line should read `display st7789, microphone es8311, speaker es8311`, with touch ready on the touch version.
2. **Screen:** the mascot is upright and not mirrored, and the colors are right (amber accents, not blue). Swapped red and blue means `lcd.bgr` is wrong; a negative-looking image means `lcd.invert` is.
3. **Brightness:** the settings menu's brightness steps dim the screen, and 100 percent is the brightest. Reversed steps mean `lcd.backlight_invert` is wrong.
4. **Touch:** holding the screen records, a swipe down cancels, and taps land where you touch.
5. **Microphone:** hold BOOT and speak; the waves move with your voice and Hermes's transcript is right.
6. **Speaker:** replies are clear and loud enough (`set volume 80`).
7. **Power:** unplug USB with the battery in, and the device stays on. **Battery and power** then shows the voltage and "Not charging", and `diag` reports `"external_power":false`; plugged into a computer it reports charging and external power. **Power off** in settings turns it off.

The pin map follows xiaozhi-esp32's `spotpear/sp-esp32-s3-1.54-muma` board at commit `d91d682`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## Spotpear ESP32-S3-1.28-BOX

Board option `spotpear-box128`, for Spotpear's round robot, SKU `sp-esp32-s3-1.28-box`: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 1.28" 240×240 round GC9A01 panel over SPI, a CST816D touchscreen, one ES8311 codec for both the speaker and the microphone, an NS4150B amplifier, a BOOT key and a battery. It ships with the XiaoZhi firmware. Apart from the panel, it is wired like the [muma](#spotpear-esp32-s3-154-muma). This port is experimental.

**Back up the whole flash before the first flash.** The board has no other copy of its factory firmware:

```bash
esptool.py --chip esp32s3 read_flash 0 0x1000000 box128-factory.bin
```

Writing that file back at `0x0` restores the original firmware.

| Part | Chip | Connection |
|---|---|---|
| Display | GC9A01, SPI | MOSI 2, SCLK 4, CS 5, DC 47, RST 38; 40 MHz; BGR, mirrored on X, colors inverted; backlight GPIO 42, active low |
| Speaker DAC + microphone ADC | ES8311 | I2C 0x18 on SDA 15, SCL 14; I2S MCLK 16, BCLK 9, WS 45, DOUT 8, DIN 10; amplifier enable 46 |
| Touch | CST816D | I2C 0x15 on its own bus, SDA 11, SCL 7; RST 6 |
| Power | | GPIO 3 latches the battery path on; charge signal on GPIO 41 (high while charging); battery divider on GPIO 1 (ADC1 channel 0), ratio 2 |
| Status LED | | GPIO 48 |
| Keys | | BOOT GPIO 0 (TALK) |

After Espressif's default GC9A01 bring-up, the firmware writes this panel's gate timing (`0x62`, `0x63`) and supply levels (`0xC3`, `0xC4`), as the factory firmware does (`panel_spotpear128.hpp`).

The battery reading assumes the divider halves the cell voltage. At that ratio, the factory firmware's charge table, read at the same attenuation, puts a full cell near 4.0 V, but nobody has measured it on a board yet. **Power off** in settings lowers GPIO 3; on USB power the board stays on.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e spotpear-box128 -t upload -t monitor
```

The USB-C port is the S3's own USB Serial/JTAG, used for flashing and the serial console. To preview the round layout without the board, run `hermes-gadget sim --board sim-240x240-round`.

### First flash: what to check

1. **Boot log:** `battery latch enabled; calibrated voltage available` and `codecs: speaker ready, microphones ready (es8311 analog)`. The `hg.diag` parts line should read `display gc9a01, microphone es8311, speaker es8311`, with touch ready.
2. **Screen:** the mascot is upright, not mirrored, and centred in the circle, and the colors are right (amber accents, not blue). Swapped red and blue means `lcd.bgr` is wrong; a negative-looking image means `lcd.invert` is; a mirrored picture means `lcd.mirror_x` is. Faint or washed-out colors point at the register changes in `panel_spotpear128.hpp`.
3. **Brightness:** the settings menu's brightness steps dim the screen, and 100 percent is the brightest. Reversed steps mean `lcd.backlight_invert` is wrong.
4. **Touch:** holding the screen records, a swipe down cancels, and taps land where you touch. A swipe that reads backwards means the touch needs `touch.mirror_x` or `touch.mirror_y`.
5. **Microphone:** hold BOOT and speak; the waves move with your voice and Hermes's transcript is right.
6. **Speaker:** replies are clear and loud enough (`set volume 80`).
7. **Battery:** with a charged cell, `hermes-gadget diag` reports about 4.0 to 4.2 V. A reading far off means the divider ratio is wrong. The charging mark shows on USB and goes away on battery; if it is the wrong way round, clear `latch_power.charging_active_high`.
8. **Power:** unplug USB with the battery in, and the device stays on. **Power off** in settings turns it off.

The pin map follows xiaozhi-esp32's `spotpear/sp-esp32-s3-1.28-box` board at commit `d91d682`; see [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

## ESP-VoCat

Board option `esp-vocat`, for Espressif's ESP-VoCat AI companion kit, **v1.2 only** (the version is on the mainboard's silkscreen). It has an ESP32-S3-WROOM-1 module with 16 MB flash and 16 MB octal PSRAM, a round 1.85" 360×360 ST77916 panel over QSPI, a CST816S touchscreen, an ES8311 DAC driving an NS4150B 3 W amplifier, an ES7210 with two microphones, a BMI270 IMU, a BQ27220 fuel gauge, a green LED, two capacitive touch pads on its head, a microSD slot, and RST, BOOT and POWER keys. It ships with an ESP-Brookesia chatbot firmware. This port is experimental.

v1.0 boards are not supported: v1.2 moved the microphone data line (GPIO 15 to 3), the amplifier enable (4 to 15) and the LCD reset (3 to 47), and added the codec supply switch on GPIO 48.

**Back up the whole flash before the first flash.** The board has no other copy of its factory firmware:

```bash
esptool.py --chip esp32s3 read_flash 0 ALL vocat-factory.bin
```

Writing that file back at `0x0` restores the original firmware.

| Part | Chip | Connection |
|---|---|---|
| Display | ST77916, QSPI | SCLK 18, D0 46, D1 13, D2 11, D3 12, CS 14; RST 47, active high (through an inverting FET); backlight GPIO 44, active high |
| Power rails | | GPIO 9 low switches the LCD/SD 3.3 V rail on; GPIO 48 high switches the codec supply on |
| Touch | CST816S | I2C 0x15 on SDA 2, SCL 1; INT 10; no reset line |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 42, BCLK 40, WS 39, DOUT 41; NS4150B amplifier enable GPIO 15, active high |
| Microphone ADC | ES7210 | I2C 0x40; I2S DIN 3; microphones on MIC1 and MIC2, the speaker output looped back on MIC3 |
| Keys | | BOOT GPIO 0 (TALK), under the base; RST and POWER act on the hardware only |
| Unused by this profile | | BMI270 IMU (I2C 0x68, INT1 GPIO 21), BQ27220 fuel gauge (I2C 0x55), touch pads GPIO 6 and 7, green LED GPIO 43 (active low), microSD (CLK 16, CMD 38, D0 17), UART1 on the magnetic connector (RX 4, TX 5) |

All I2C parts share one bus. GPIO 43 and 44 are UART0's pins, so the serial console is the S3's USB Serial/JTAG on the USB-C port. Hold the screen or BOOT to talk, and swipe down to cancel. The firmware doesn't read the battery or the head pads yet, and doesn't use the MIC3 loopback for echo cancellation.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp-vocat -t upload -t monitor
```

### First flash: what to check

1. **Boot log:** the `hg.lcd` line reads `st77916 360x360 ready` after the panel ID, and the `hg.diag` parts line reads `display st77916, microphone es7210, speaker es8311`, with touch ready.
2. **Screen:** the mascot is upright, centred in the circle and not mirrored, and the colors are right (amber accents, not blue). A negative-looking image means `lcd.invert` is wrong.
3. **Brightness:** the settings menu's brightness steps dim the screen, and 100 percent is the brightest.
4. **Touch:** holding the screen records, a swipe down cancels, and taps land where you touch.
5. **Microphone:** hold the screen and speak; the waves move with your voice and Hermes's transcript is right.
6. **Speaker:** replies are clear and loud enough (`set volume 80`).

The pin map follows Espressif's esp-bsp `bsp/esp_vocat` at commit `f259a6a` and the ESP-VoCat v1.2 schematic; the panel's start-up table comes from the same BSP. See [THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md).

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

## Elecrow CrowPanel Advanced 10.1-inch ESP32-P4

Use board profile `crowpanel-p4-101` for the Elecrow CrowPanel Advanced 10.1-inch ESP32-P4, V1.1. Elecrow identifies this panel as 1024×600; the profile is for that resolution and is based on the V1.1 documentation and schematic. The profile is experimental and builds with ESP-IDF 6.1.0 via `idf.py` and the dedicated CI job; this project has no ESP-IDF PlatformIO environment for P4. The PlatformIO Arduino framework listing is a different build path. The optional camera is not implemented.

| Part | Interface and profile wiring |
|---|---|
| Display | 1024×600 MIPI-DSI, EK79007 bridge, 2 lanes; PHY LDO3 at 2.5 V; backlight GPIO 31, active high; panel reset is not bonded |
| Touch | GT911; I2C0 SDA 45 / SCL 46 at 400 kHz; reset GPIO 40 active low; interrupt GPIO 42 is polled; driver probes 0x14 and 0x5D after reset |
| Microphone | PDM RX on I2S0; clock GPIO 24, data GPIO 26; 16 kHz PCM |
| Speaker output | I2S1; BCLK GPIO 22, WS GPIO 21, DATA GPIO 23; stereo, left-justified / bit-shift framing; active-low amplifier-control GPIO 30 |
| Battery / charger monitor | STC8 on I2C0 address 0x2F at 400 kHz; reads registers 0x00–0x0B |
| Wi-Fi | ESP32-C6 companion over 4-bit SDIO; CMD 19, CLK 18, D0–D3 17/16/15/14, reset GPIO 32 |

GPIO 45 and 46 are on the P4 VDD_IO_5 domain. Firmware acquires internal LDO4 at 3.3 V before creating I2C0; the DSI driver separately configures LDO3 for the DSI PHY. Touch and display initialization depend on these rails. Hardware facts are referenced from the pinned [Elecrow V1.1 repository commit](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/tree/6c7c851f3105349dc24b12ad058655ba6afa98ee), including the [board config.h](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/blob/6c7c851f3105349dc24b12ad058655ba6afa98ee/example/V1.1/idf-code/7_9_10.1_P4_HMI_AI/7inch_9inch_10inch_P4_HMI_AI/main/boards/elecrow-p4-board/config.h) and the [V1.1 schematic files](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/tree/6c7c851f3105349dc24b12ad058655ba6afa98ee/Eagle_SCH%26PCB). The upstream repository has no license; only hardware facts are used here, and no Elecrow source code is copied.

**Audio.** The profile uses the existing Gadget audio interface: the upstream I2S microphone driver is configured for PDM on I2S0, and a P4-specific I2S1 speaker adapter handles the amplifier and stereo framing. No local microphone loopback or raw-audio diagnostic path is added.

**Battery limitations.** The STC8 voltage and percentage are uncalibrated estimates. The adapter exposes only the existing Gadget `PowerStatus` fields: `battery_mv`, `battery_percent`, and `charging`; a reported STC8 error leaves `charging` unknown. These readings do not quantify current or calibrated state of charge, and `external_power` remains unknown. Validate charging with a meter rather than inferring current from percentage or an LED.

**Build with ESP-IDF 6.1.0** (the P4 Wi-Fi and EK79007 dependencies are pinned in `main/idf_component.yml`):

```bash
cd firmware/esp32
idf.py -B build/crowpanel-p4-101 \
  -D IDF_TARGET=esp32p4 \
  -D SDKCONFIG=build/crowpanel-p4-101/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/crowpanel-p4-101/sdkconfig.defaults" \
  build
```

Releases include a merged image and an OTA app for this profile, and every CI run keeps them in its `firmware-crowpanel-p4-101` artifact. To package a local build the same way:

```bash
python tools/stage_idf_build.py crowpanel-p4-101 build/crowpanel-p4-101
python tools/package_release.py --board crowpanel-p4-101 --out dist
```

For physical results and explicit untested items, see the [CrowPanel P4 validation record](hardware-validation.md#crowpanel-advanced-p4-partial-physical-test-record). Resolution reference: [Elecrow product page](https://www.elecrow.com/crowpanel-advanced-10-1inch-esp32-p4-hmi-ai-display-1024x600-ips-touch-screen-wifi-6.html).

## ESP32-2432S028 CYD (Cheap Yellow Display)

Board option `esp32-2432s028-cyd`, for the ESP32-2432S028 "Cheap Yellow Display" revision with **two USB ports** (micro-USB and USB-C) and an **ST7789** panel. The original CYD, with only a micro-USB port, has an ILI9341 panel; this profile does not drive it. The two-USB board is a **classic ESP32-WROOM-32** with 4 MB flash and **no PSRAM**, a 2.8" 240×320 ST7789 panel driven in landscape, the BOOT key, and an SC8002B amplifier fed by the chip's own DAC on GPIO26. The board ships with **no microphone**, and this profile does not use its resistive touchscreen, but if you fit a **PDM MEMS microphone** on the 4-pin header (IO22 carries the PDM clock, IO27 the data, SELECT tied to ground) the profile drives it: the ESP32's PDM-to-PCM converter on I2S0 turns its bitstream into the 16 kHz PCM the app records, and the amplifier lets the beeps and later replies be heard. Without a microphone it works as a text gadget: Hermes replies, cards and prompts appear on the screen and typed messages go out through the USB console. Only a two-wire PDM part fits this header — an I²S microphone such as the INMP441 or ICS-43434 needs three signal lines and does not. On boot the speaker header plays three short beeps through that DAC (`port_dac.cpp`), which shows the amplifier and a connected speaker work; replies are not routed there yet. It is the only classic-ESP32 profile, so the RGB, i80-parallel, DSI and codec drivers are compiled out; the panel uses the same SPI `SpiDisplay` path as the S3 boards, with ESP-IDF's built-in ST7789 driver.

Without PSRAM the full 320×240 framebuffer (150 KB) does not fit: about 137 KB of internal DRAM is free at boot, and Wi-Fi and the app need their share too. The port therefore holds a **band of 120 rows** (75 KB) instead, reserved in an early constructor before the heap fragments (`port_display.cpp`), and flushes through a four-row bounce buffer. The UI detects the band through `Display::framebuffer_rows()` and draws each band once per 120-row window, moving the window with `set_frame_origin()`, so the screen looks the same; a flush that times out asks the UI to repaint the whole screen, and keeps asking until a repaint reaches the panel, because the band keeps no rows to resend. For the same reason the board doesn't offer images: Hermes shows its unsupported-image notice instead, and an `image.start` sent anyway is refused. If Wi-Fi cannot start for lack of heap, lower `kCydFrameRows` in `port_display.cpp`.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI2 | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2; no reset line (the panel resets on power-up); 320×240, swapped axes, both axes mirrored |
| Backlight | PWM | GPIO 21, active-high |
| Speaker | SC8002B amplifier | the chip's own DAC, channel 1 on GPIO 26; the speaker connects to the board's two-pin header |
| Microphone | PDM MEMS (optional, not fitted) | I2S0: PDM clock on GPIO 22, data on GPIO 27, on the 4-pin header |
| Button | BOOT | GPIO 0 (TALK) |

The board also carries a resistive XPT2046 touchscreen on its own pins (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36), a microSD slot on VSPI (SS 5, SCK 18, MISO 19, MOSI 23), an RGB LED on GPIO 4, 16 and 17 and a light sensor on GPIO 34; this profile does not use them. The amplifier plays the three boot beeps only, and the microphone header is wired for a PDM part but the stock board has none fitted, so a recording without one captures silence. Probe your own board before assuming it: the CYD ships in several revisions whose backlight pin, panel controller and colour order differ.

**Build and flash** it with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32-2432s028-cyd -t upload -t monitor
```

Or with `idf.py set-target esp32` followed by `idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32-2432s028-cyd/sdkconfig.defaults" build`. The micro-USB port reaches UART0 through a USB-UART bridge (usually CH340), so flashing and the serial console (115200 baud) both use it.

### First flash: what to check

The pin map comes from the community [ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/tree/0564a140c76de76ef34a8b7115044e7c6c852695) reference (`PINS.md`, and `DisplayConfig/CYD2USB/User_Setup.h` for the two-USB revision's ST7789). The contributor ran an earlier build of this port on a Guition ESP32-2432S028: its display (in monochrome), the DAC speaker, and Wi-Fi and pairing set with `menuconfig` worked. Touch was off and no microphone was fitted. The merged profile draws in colour through the banded framebuffer and has not run on a board yet, so the colour order, inversion and mirroring are the contributor's settings and still unchecked. On the first flash:

1. **Boot log:** `st7789 320x240 ready` and the `parts:` line naming the display, the microphone (`pdm` once a PDM mic is fitted, `none` without one) and no touch.
2. **Sound:** about a second into boot the speaker header plays three short beeps, and the log shows `three beeps on GPIO26 (DAC channel 1)`. Silence points at the amplifier wiring or the speaker, not the panel; a log line about GPIO26 not being a DAC output means `b.speaker_dac` in `board.cpp` is wrong.
3. **Memory:** no `the reserved 120-row band does not fit` error, and `diag` reports a plausible free heap. The band is reserved before Wi-Fi starts, so a Wi-Fi failure to allocate is the signal to lower `kCydFrameRows`.
4. **Screen:** the mascot renders in landscape and colours are right (amber accents, not blue). A red/blue swap means `b.lcd.bgr` in this board's `board.cpp` entry needs flipping; an inverted picture means `b.lcd.invert` does.
5. **Brightness:** cycle 10 → 100 in settings; brightness must rise with the number. The backlight is active-high — do not set `backlight_invert`.
6. **Orientation:** the picture should not be mirrored or upside down; adjust `b.lcd.swap_xy`, `mirror_x` and `mirror_y` if it is.
7. **Button and microphone:** holding BOOT records a voice turn. With a PDM mic on the header the log shows `microphone ready (pdm)` and speaking into it shows in the level meter; `microphone did not start: not enough internal RAM for its buffers` means the I2S0 DMA did not fit beside the framebuffer band, so lower `kCydFrameRows`. Without a microphone the recording is silent and replies are text only.
8. **Text:** `say hello` over the console should render on the display.

Only hardware facts (pin numbers and panel settings) come from that reference; no code is copied. The firmware builds, but it has not been run on a board and has no physical report; see the [verification table](hardware-validation.md).

## Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C

Board option `ws-p4-touch-lcd-3.4c`, for Waveshare's ESP32-P4-WIFI6-Touch-LCD-3.4C (SKU 31523): a round 800x800 IPS panel on a MIPI-DSI bus (JD9365 controller), GT911 capacitive touch, ES8311 speaker codec plus ES7210 microphone ADC, and an ESP32-C6 companion for Wi-Fi 6 over a 4-bit SDIO link.

| Part | Chip | Connection |
|---|---|---|
| Display | JD9365, 800x800 MIPI-DSI | 2 data lanes @ 1500 Mbps, DPI clock 80 MHz, RGB565; DSI PHY from internal LDO channel 3 @ 2500 mV |
| Backlight | PWM | GPIO 26, active-high |
| Touch | GT911 | I2C SDA 7, SCL 8 (port 1), reset/int not bonded |
| Microphone | ES7210 ADC (dual mic) | I2S MCLK 13, BCLK 12, WS 10, DIN 11 |
| Speaker | ES8311 codec + amp | I2S DOUT 9; amplifier enable GPIO 53 (active high) |
| Wi-Fi / BLE | ESP32-C6 companion | standard P4 function-board SDIO pins |
| LCD reset | | GPIO 27 |

Build with `idf.py` (no PlatformIO env):

```bash
cd firmware/esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/ws-p4-touch-lcd-3.4c/sdkconfig.defaults" \
    set-target esp32p4 build
```

Releases don't include this board yet, so the browser installer doesn't list it. CI builds the profile on every change.

Experimental. The owner confirmed the display, C6-hosted Wi-Fi, pairing, reconnect after restart, factory reset and a one-hour run; touch coordinates, the microphone, the speaker, network recovery and OTA are not verified. See the [partial physical report](hardware-validation.md#waveshare-p4-touch-lcd-34c-partial-physical-report).

Wi-Fi caveat: the ESP32-C6 companion on this board does not ship with an esp_hosted slave image that this host (esp_hosted 2.12.6) can talk to. `g_wifi.begin()` times out after about 10 s and the gadget runs offline until the C6 is flashed. Flash `network_adapter.bin` (CrowPanel C6 firmware, V2.12.3, in `CrowPanel_Advance_ESP32-P4 Display_Firmware_Upgrade_Guide/.../`) to the C6 through its SH1.0 UART header with esptool before expecting Wi-Fi. The [report](hardware-validation.md#getting-the-c6-working-on-this-board) lists every step that was needed.

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

- `--latest` reads the newest [release](https://github.com/Adolanium/hermes-gadget-sdk/releases)'s manifest, picks the image for the device's board, and checks its size and SHA-256 against the manifest before using it. A device that already runs that version or a newer one, including a source build such as `0.4.0-12-gabc1234`, is left alone, and a device whose version can't be read is refused; `--force` installs the release anyway. `hermes gadget devices` shows the version each device runs.
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
When the board has a power adapter, `status` includes a `power` object; the state sensor message mirrors its battery fields. On the CrowPanel P4, the STC8 adapter samples its 12-byte record every 5 seconds through the app power poller and maps it to the existing Gadget fields `battery_mv`, `battery_percent`, and `charging`. The adapter does not expose raw registers, battery-state enums, `battery_present`, or `external_power`.

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
| `wifi` / `wifi list` | The [saved Wi-Fi networks](#saved-wi-fi-networks) as `@wifi` JSON, without passwords |
| `wifi add <ssid> [password]` | Save a network without joining it; leave out the password for an open network |
| `wifi join <ssid>` | Move a saved network first and join it now |
| `wifi forget <ssid>` | Remove a saved network |
| `phone` / `phone pair` / `phone pair close` / `phone forget` | On boards with Bluetooth: phone app state, open or close pairing, or remove every paired phone ([protocol](protocol.md#phone-app-over-bluetooth)) |
| `perf` / `perf on` / `perf off` / `perf bench` | Frame timing and the renderer bench ([measure frame timing](development.md#measure-frame-timing)) |
| `yes` / `no` | Answer the question on screen |
| `reconnect` | Drop and re-open the Hermes connection |
| `forget-key` | New device identity on next boot (re-enrollment and re-pairing) |
| `factory-reset` | Erase the device key and all settings |

The keys are `name`, `server`, `token`, `talk_mode` (`hold` or `tap`), `volume`, `brightness`, `screen_timeout` (0..3600 seconds, 0 disables it), `clock` (0 or 5..3600 seconds of rest before the resting clock shows; 0, the default, turns it off), `time_format` (`12h` or `24h`, the clock face), `timezone` (a POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3`; the default `UTC0`; ESP32 boards), `battery_display` (`icon`, `percentage`, `both` or `off`), `language` (`en`, `es`, or `auto` to follow Hermes; see [Language](using-gadget.md#language)), `wifi_ssid` and `wifi_pass`. The [device settings menu](using-gadget.md#device-settings-and-hardware-checks) saves volume, brightness, talk mode, battery indicator, screen timeout, clock face and language without a console.

In the `diag` report, `i2c` lists the addresses that answered, such as `0x18`. A probe can time out while another task uses the bus, so the scan tries each address up to three times. An address that times out on every try appears as `timeout at 0x19`, and the scan continues. Four such addresses in a row end the scan with `bus stuck at` the first of them, such as `bus stuck at 0x08`. That usually means a device or a short holds SDA or SCL low.

### Saved Wi-Fi networks

The device keeps up to five Wi-Fi networks and joins whichever is in range, so it can move between home, an office and a phone hotspot without new setup:

```
gadget> wifi add "Home Wi-Fi" "correct horse"
@ok wifi add
gadget> wifi add Phone hotspot-pass
@ok wifi add
gadget> wifi
@wifi {"networks":[{"ssid":"Home Wi-Fi","tested":true,"open":false},{"ssid":"Phone","tested":false,"open":false}],"max":5,"current":"Home Wi-Fi"}
```

- Put a name or password with spaces in double quotes. Inside quotes, `\"` is a quote and `\\` a backslash.
- The list is in order: the network that last worked comes first and is tried first at startup.
- `tested` turns true once the device has joined the network with its saved password. Saving a new password for a network clears it again. `wifi add` does not check the password, so a network saved out of range shows `false` until the device first joins it.
- When the link drops, the device retries the same network once. If that fails too, it scans and joins the first network if the scan saw it, else the strongest saved network it saw. A network that failed twice is passed over while another saved one is in range. When the scan finds no saved network, for example because it is hidden, the device tries the next one in the list. With one saved network it never scans and retries as before.
- `set wifi_ssid` and `set wifi_pass` work on the first network: setting a new name saves that network first and keeps the others. If five are saved, the last one is dropped. An empty `wifi_ssid` forgets the first network. [Phone setup](setup-board.md#set-up-wi-fi-with-your-phone) saves its network first the same way.
- `factory-reset` forgets every network, and the access point it last joined.

Machine-readable lines start with `@`, so tools can drive a bench device. `hermes-gadget provision` is a thin wrapper over these commands.

### Wi-Fi reconnect

A join that names no access point first scans every channel. To skip that, the device records the access point it joined after each good connection: its BSSID, its channel and the DHCP lease (key `wifi_fast`, written only when something changed).

- **At boot and after the link drops,** the first attempt on the same network aims at that access point on that channel, with no scan.
- **If that attempt fails,** for example because the router moved to another channel or the device is somewhere else, the device joins normally at once: the usual scan and retries in [saved Wi-Fi networks](#saved-wi-fi-networks). The failed fast attempt doesn't count against the network or lengthen the battery backoff. The next good join replaces the record.
- **DHCP** asks for the last address first (`CONFIG_LWIP_DHCP_RESTORE_LAST_IP`, on for every board), which skips the discovery round when the router still has the lease.
- **Static address (opt-in):** with `CONFIG_HG_WIFI_REUSE_IP` (menuconfig, **Hermes Gadget**), a fast join reuses the last lease as a static address and skips DHCP. Turn it on only when the router reserves that address for the device. Nothing renews the lease, and an address the router gave to someone else in the meantime conflicts. Every other join uses DHCP.
- Forgetting a network forgets its record.

How long a reconnect takes has not been measured on hardware for this firmware.

## Power-on sequence

1. **Boot:** about 1 s.
2. **Wi-Fi:** the first [saved network](#saved-wi-fi-networks), else the one from menuconfig. Without credentials the device opens a temporary network for [phone setup](setup-board.md#set-up-wi-fi-with-your-phone).
3. **Connect:** the device opens the WebSocket to `server`, retrying 1 → 30 s with backoff.
4. **Authenticate:** it enrolls its key on first contact and proves it with an HMAC afterwards.
5. **Pair or ready:** an unpaired device shows a pairing code (approve with `hermes gadget pair`); a paired one goes to **Ready**.

## Known limits of the reference firmware

- Push-to-talk or tap with energy VAD. There is no wake word; the protocol leaves room for one (`audio.start.mode`).
- Wi-Fi setup works over USB or a temporary password-protected network. A Bluetooth link for a phone app exists, but no board enables it yet: each board turns it on after it passes the [Bluetooth checks](hardware-validation.md#bluetooth). There is no phone app in this repository.
- Firmware updates are authorized with the device key, but images aren't signed: the bootloader runs whatever a trusted Hermes installs. Secure Boot isn't enabled.
- The text font is ASCII only; the host folds other characters.
