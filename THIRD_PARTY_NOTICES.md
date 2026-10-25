# Third-party licenses and attribution

The [MIT license](LICENSE) covers only the code and documentation written for this project. Third-party material retains its own license. Copyright and modification notices are recorded in [NOTICE](NOTICE), with additional license texts in [LICENSES](LICENSES).

## Dependencies

The dependencies below keep their own licenses and are downloaded when you build or install. Adapted display initialization tables are listed separately below and in [NOTICE](NOTICE).

| Third-party code | Used for | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | The ESP32 framework and drivers | Apache 2.0 |
| [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) | ES8311 / ES7210 audio codecs | Apache 2.0 |
| [esp-opus](https://components.espressif.com/components/78/esp-opus) (Xiph's [libopus](https://opus-codec.org/) built for ESP-IDF) | Decoding Opus reply audio on the device | BSD 3-Clause (libopus `COPYING`); the registry lists the package as MIT |
| [esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client) | The device's WebSocket connection | Apache 2.0 |
| [esp_hosted 2.12.6](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.6) | ESP32-C6 Wi-Fi co-processor transport over SDIO (CrowPanel P4, Waveshare P4 Touch-LCD-3.4C) | Apache 2.0; [license](https://components.espressif.com/components/espressif/esp_hosted/versions/2.12.6/license?language=en) |
| [esp_wifi_remote 1.6.5](https://components.espressif.com/components/espressif/esp_wifi_remote/versions/1.6.5) | ESP-IDF Wi-Fi API over the P4 SDIO co-processor link | Apache 2.0; [license](https://components.espressif.com/components/espressif/esp_wifi_remote/versions/1.6.5/license?language=en) |
| [esp_lcd_ek79007 2.0.2](https://components.espressif.com/components/espressif/esp_lcd_ek79007/versions/2.0.2) | EK79007 MIPI-DSI panel driver (CrowPanel P4) | Apache 2.0; [license](https://components.espressif.com/components/espressif/esp_lcd_ek79007/versions/2.0.2/license?language=en) |
| [esp_lcd_jd9365 2.0.2](https://components.espressif.com/components/espressif/esp_lcd_jd9365/versions/2.0.2) | JD9365 MIPI-DSI panel driver (Waveshare P4 Touch-LCD-3.4C) | Apache 2.0; [license](https://components.espressif.com/components/espressif/esp_lcd_jd9365/versions/2.0.2/license?language=en) |
| Espressif [GC9A01 display](https://components.espressif.com/components/espressif/esp_lcd_gc9a01) driver | Spotpear 1.28-BOX round panel | Apache 2.0 |
| Espressif [SH8601 display](https://components.espressif.com/components/espressif/esp_lcd_sh8601) driver | Knob-Touch-LCD-1.8 round panel | Apache 2.0 |
| Espressif [ILI9341 display](https://components.espressif.com/components/espressif/esp_lcd_ili9341), [GT911](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911) and [TT21100](https://components.espressif.com/components/espressif/esp_lcd_touch_tt21100) touch drivers | BOX-3 display revisions and touch | Apache 2.0 |
| Espressif ST7701 [2.0.2](https://components.espressif.com/components/espressif/esp_lcd_st7701/versions/2.0.2) on IDF >=5.4, [1.1.0](https://components.espressif.com/components/espressif/esp_lcd_st7701/versions/1.1.0) earlier, and [panel IO additions](https://components.espressif.com/components/espressif/esp_lcd_panel_io_additions) (3-wire SPI) | CrowPanel 2.1 RGB panel | Apache 2.0 |
| Espressif [FT5x06 touch](https://components.espressif.com/components/espressif/esp_lcd_touch_ft5x06) driver | CoreS3 touch; ILI9341 and esp_codec_dev also provide its display and AW88298 audio drivers | Apache 2.0 |
| [esptool-js](https://github.com/espressif/esptool-js) | Flashing from the browser installer, added to the site when it's built | Apache 2.0 |
| [libopus](https://opus-codec.org/) on the Hermes host, loaded at run time and not distributed | Optional Opus encoding of reply audio in the plugin | BSD 3-Clause |
| Python packages (`websockets`, and optionally `Pillow`, `sounddevice`, `pyserial`) | Plugin, simulator and tools | Their own licenses; see each project |
| [GPIO Zero](https://github.com/gpiozero/gpiozero/blob/master/LICENSE.rst) | Optional Raspberry Pi buttons and digital outputs | BSD 3-Clause |
| [pygame](https://github.com/pygame/pygame/blob/main/docs/LGPL.txt), [SDL2](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt) | Optional Linux device display | LGPL 2.1; zlib for SDL2 |
| [lgpio](https://github.com/joan2937/lg/blob/master/UNLICENCE) | Linux GPIO access, installed from Raspberry Pi OS | Unlicense |
| [python-sounddevice](https://github.com/spatialaudio/python-sounddevice/blob/master/LICENSE), [PortAudio](https://www.portaudio.com/license.html) | Optional live microphone and speaker on Linux | MIT licenses |
| [Paho MQTT](https://github.com/eclipse-paho/paho.mqtt.python/blob/v2.1.0/LICENSE.txt) | Optional MQTT sensor/action example | EPL 2.0 / EDL 1.0 dual license; installed package includes both texts |

## Artwork

The mascot artwork, and the logo and device bitmaps drawn from it, come from [Hermes Agent](https://github.com/NousResearch/hermes-agent) (MIT, © 2025 Nous Research); see [assets/mascot](assets/mascot) and [NOTICE](NOTICE).

## Vendored code

The QR code encoder in `firmware/core/third_party/qrcode/` is vendored from [ricmoo/QRCode](https://github.com/ricmoo/QRCode) (MIT, © 2017 Richard Moore and © 2017 Project Nayuki), which in turn derives from [Project Nayuki's QR Code generator](https://www.nayuki.io/page/qr-code-generator-library). It is adapted for this project: the C99 variable-length arrays become fixed, bounded buffers so MSVC accepts the file, and two bounds checks are added so an oversized payload cannot corrupt memory or produce an unscannable code. The modification notice is in [NOTICE](NOTICE) and the upstream license in [LICENSES/QRCode-MIT.txt](LICENSES/QRCode-MIT.txt).

## Adapted display initialization tables

The BOX-3 panel register values follow [Espressif's BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3), Apache 2.0. The adapter uses zero-length sleep/display commands with explicit delays.

CoreS3's ILI9342E initialization table follows [Espressif's CoreS3 BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3), Apache 2.0. Its copyright and modification notice are retained in the source and [NOTICE](NOTICE). The power and reset adapter is original code based on the board's documented wiring and chip registers.

The AMOLED panel's start-up register values in `firmware/esp32/main/port_amoled.cpp` follow Waveshare's [board support package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) for the ESP32-S3-Touch-AMOLED-1.75 (Apache 2.0, text in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); see [NOTICE](NOTICE). The 1.75C profile reuses that driver and its license notice. The rectangular AMOLED-1.8's start-up table (`kInit368`) follows Waveshare's [AMOLED-1.8 board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9f4030c6e5cb888ad4cc268bfa7584c93ad53e30/bsp/esp32_s3_touch_amoled_1_8) at commit `9f4030c6e5cb888ad4cc268bfa7584c93ad53e30` (Apache 2.0). Its pin map and audio supply configuration follow the [manufacturer's schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf). The AMOLED-2.06 watch's start-up table (`kInit410`) and pin map follow Waveshare's [AMOLED-2.06 board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9daffb7168b3c093a330da700988a22092294eef/bsp/esp32_s3_touch_amoled_2_06) 2.0.0 at commit `9daffb7168b3c093a330da700988a22092294eef` (Apache 2.0), checked against the [manufacturer's schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf). The AMOLED-2.16's start-up table (`kInit480`), pin map and touch orientation follow Waveshare's [AMOLED-2.16 board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/63ac675334db62509082e182ff900bf60f21f009/bsp/esp32_s3_touch_amoled_2_16) 2.0.1 at commit `63ac675334db62509082e182ff900bf60f21f009` (Apache 2.0). The Waveshare P4 Touch-LCD-3.4C's JD9365 start-up table (`kJd9365InitCmds` in `firmware/esp32/main/port_dsi.cpp`) follows Waveshare's [ESP32-P4-WIFI6-Touch-LCD-XC board support package](https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_xc/versions/2.0.0) 2.0.0, its 800x800 table, unchanged (Apache 2.0).

The Waveshare 1.85C V2 port uses Espressif's managed
[ST77916 driver](https://components.espressif.com/components/espressif/esp_lcd_st77916)
2.0.2 (Apache 2.0). `firmware/esp32/main/panel_ws185.hpp` adapts the two active
initialization tables from Waveshare's factory demo at commit
`8ead4a96bf3a278fc4ebd8ef4768657e17fa2880` in
[ESP32-S3-Touch-LCD-1.85C](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.85C),
`ESP-IDF/ESP32-S3-Touch-LCD-1.85C-Test/main/LCD_Driver/ST77916.c` and
`esp_lcd_st77916/esp_lcd_st77916.c` (Apache 2.0). Changes: C++ byte-string
payloads, removal of commented-out entries, named tables selected by panel ID.
The upstream driver copyright is Espressif Systems (Shanghai) CO LTD, 2023;
see the source header and [NOTICE](NOTICE). The reset, CST816 register reader
and PCM conversion adapters are original code using documented hardware facts.
The existing `LICENSES/Apache-2.0.txt` supplies the license text; packaging
includes it, this attribution, and the managed driver licenses.

The ESP-VoCat port uses the same managed ST77916 driver.
`firmware/esp32/main/panel_vocat.hpp` adapts the initialization table from
Espressif's [ESP-VoCat BSP](https://github.com/espressif/esp-bsp/tree/f259a6abd5808019b1903a422092a4234ca95a51/bsp/esp_vocat)
at commit `f259a6abd5808019b1903a422092a4234ca95a51`,
`bsp/esp_vocat/priv_include/disp_init_data.h` (Apache 2.0, Copyright 2026
Espressif Systems (Shanghai) CO LTD). Changes: C++ byte-string payloads; the
values, order and delays are unchanged. The board's pin map follows the same BSP
and Espressif's [ESP-VoCat v1.2 schematic](https://dl.espressif.com/AE/esp-dev-kits/ESP-VoCat_SCH_V1_2.pdf);
see [NOTICE](NOTICE).

The T-Display-S3 ST7789 initialization values and AW9364 backlight control follow
[LilyGO's factory example](https://github.com/Xinyuan-LilyGO/T-Display-S3/blob/ec889e789b3cf093412689a143f7f37b42b56af7/examples/factory/factory.ino),
revision `ec889e789b3cf093412689a143f7f37b42b56af7`, MIT, Copyright (c) 2022 Xinyuan-LilyGO.
The ESP-IDF adaptation and timing changes are recorded in [NOTICE](NOTICE), with
the upstream license in [LICENSES/LilyGO-MIT.txt](LICENSES/LilyGO-MIT.txt).

The AIPI Lite pin map, panel settings and power wiring follow [xiaozhi-esp32's `xorigin/aipi-lite` board](https://github.com/78/xiaozhi-esp32/tree/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/boards/xorigin/aipi-lite) at commit `0d576d3d4c049c6f55eaf879725dc23e516511b4` (MIT). Only hardware facts are used; see [NOTICE](NOTICE).

The Spotpear ESP32-S3-1.28-BOX pin map, panel settings, power wiring and GC9A01 register changes (`panel_spotpear128.hpp`) follow [xiaozhi-esp32's `spotpear/sp-esp32-s3-1.28-box` board](https://github.com/78/xiaozhi-esp32/tree/d91d6824309d12df5fc3db5edf8145ee829135d7/main/boards/spotpear/sp-esp32-s3-1.28-box) at commit `d91d6824309d12df5fc3db5edf8145ee829135d7` (MIT). Only hardware facts are used; see [NOTICE](NOTICE).

The Spotpear ESP32-S3-1.54 muma pin map, panel settings and power wiring follow [xiaozhi-esp32's `spotpear/sp-esp32-s3-1.54-muma` board](https://github.com/78/xiaozhi-esp32/tree/d91d6824309d12df5fc3db5edf8145ee829135d7/main/boards/spotpear/sp-esp32-s3-1.54-muma) at commit `d91d6824309d12df5fc3db5edf8145ee829135d7` (MIT). Only hardware facts are used; see [NOTICE](NOTICE).

The Knob-Touch-LCD-1.8 SH8601 initialization values in `firmware/esp32/main/panel_sh8601.hpp` and the pin map in `board.cpp` follow Waveshare's [ESP32-S3-Knob-Touch-LCD-1.8 demo](https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip) (last modified 2025-06-20, downloaded 2026-10-19; SHA-256 in [NOTICE](NOTICE)): `lcd_init_cmds` in `ESP-IDF/08_LVGL_Test/main/main.c` and the demos' `user_config.h` pins. The archive has no license, so only these hardware facts are used, re-expressed as C++ byte strings for Espressif's `esp_lcd_sh8601` driver; the values, order and delays are unchanged.

The CrowPanel 2.1 ST7701 initialization values in `firmware/esp32/main/crowpanel_st7701_init.hpp`, and the RGB timings, PCF8574 power and reset sequence and pin map in `port_rgb.cpp` and `board.cpp`, follow Elecrow's [RotaryScreen_2_1 example](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen/tree/faf8ecf27ec1504b51de7dc4e15b2e7d7e87c79c/example/Arduino/RotaryScreen_2_1) at commit `faf8ecf27ec1504b51de7dc4e15b2e7d7e87c79c`. That repository has no license, so only these hardware facts are used, re-expressed for Espressif's `esp_lcd_st7701` driver; see [NOTICE](NOTICE).

The CrowPanel 5.0 ILI6122 RGB pin map, scanout timings (15 MHz PCLK, porches, `pclk_active_neg`), GPIO 2 backlight, and the GT911 I2C touch wiring in `port_rgb.cpp`, `board.cpp` and `port_touch.cpp` follow Elecrow's [crowpanel-esp32-5.0-3.0 example](https://github.com/Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480/tree/fd959ac9ac900bb536ad72eebb9d2c14c7ffc7b7/example/V1.0_and_V2.0/Arduino/Arduino-Hardware-Version-3.0/crowpanel-esp32-5.0-3.0) at commit `fd959ac9ac900bb536ad72eebb9d2c14c7ffc7b7`. That repository has no license, so only these hardware facts are used, re-expressed for Espressif's built-in `esp_lcd_rgb` and managed `esp_lcd_touch_gt911` drivers; see [NOTICE](NOTICE).

## Hardware references used as facts

The CrowPanel Advanced 10.1-inch P4 pin map, DSI timings, backlight polarity, and STC8 register layout are hardware facts referenced from Elecrow V1.1 commit `6c7c851f3105349dc24b12ad058655ba6afa98ee`: [pinned repository](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/tree/6c7c851f3105349dc24b12ad058655ba6afa98ee), [board config.h](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/blob/6c7c851f3105349dc24b12ad058655ba6afa98ee/example/V1.1/idf-code/7_9_10.1_P4_HMI_AI/7inch_9inch_10inch_P4_HMI_AI/main/boards/elecrow-p4-board/config.h), and [V1.1 schematic files](https://github.com/Elecrow-RD/CrowPanel-Advanced-10.1inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen/tree/6c7c851f3105349dc24b12ad058655ba6afa98ee/Eagle_SCH%26PCB). The vendor repository has no license file; only factual hardware information is used, and no Elecrow source code is copied.

The ESP32-2432S028 (CYD, two-USB revision) pin map and panel settings in `firmware/esp32/main/board.cpp` follow Brian Lough's [ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/tree/0564a140c76de76ef34a8b7115044e7c6c852695) reference at commit `0564a140c76de76ef34a8b7115044e7c6c852695`: [`PINS.md`](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/0564a140c76de76ef34a8b7115044e7c6c852695/PINS.md) and [`DisplayConfig/CYD2USB/User_Setup.h`](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/0564a140c76de76ef34a8b7115044e7c6c852695/DisplayConfig/CYD2USB/User_Setup.h). The repository is under the MIT License (Copyright (c) 2023 Brian Lough); only these hardware facts are used and no code is copied. The panel uses ESP-IDF's built-in ST7789 driver and the speaker beeps use ESP-IDF's DAC driver, so the board adds no managed component.

The QMI8658 driver's register values follow the [QST QMI8658A datasheet Rev A](https://qstcorp.com/upload/pdf/202301/13-52-25%20QMI8658A%20Datasheet%20Rev%20A.pdf) (© 2022 QST Corporation), and the PCF85063 driver's follow the [NXP PCF85063A datasheet](https://www.nxp.com/docs/en/data-sheet/PCF85063A.pdf). Only hardware facts (register addresses, values and data layouts) were taken; no vendor code is copied. See [NOTICE](NOTICE).

## Release packages

Firmware packages include `hermes-gadget-<version>-licenses.zip` with this document, the project's notices, license texts, and notices found in the installed ESP-IDF and managed component sources. The browser installer links that archive when the release supplies it.

Python distributions and Linux device packages also include this document and the project's license texts and notices. The Linux installer copies them to `/opt/hermes-gadget/current/licenses/`.
