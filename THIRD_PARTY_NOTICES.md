# Third-party licenses and attribution

The [MIT license](LICENSE) covers only the code and documentation written for this project. Third-party material retains its own license. Copyright and modification notices are recorded in [NOTICE](NOTICE), with additional license texts in [LICENSES](LICENSES).

## Dependencies

The dependencies below keep their own licenses and are downloaded when you build or install. Adapted display initialization tables are listed separately below and in [NOTICE](NOTICE).

| Third-party code | Used for | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | The ESP32 framework and drivers | Apache 2.0 |
| [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) | ES8311 / ES7210 audio codecs | Apache 2.0 |
| [esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client) | The device's WebSocket connection | Apache 2.0 |
| Espressif [ILI9341 display](https://components.espressif.com/components/espressif/esp_lcd_ili9341), [GT911](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911) and [TT21100](https://components.espressif.com/components/espressif/esp_lcd_touch_tt21100) touch drivers | BOX-3 display revisions and touch | Apache 2.0 |
| Espressif [ST7701 display](https://components.espressif.com/components/espressif/esp_lcd_st7701) and [panel IO additions](https://components.espressif.com/components/espressif/esp_lcd_panel_io_additions) (3-wire SPI) drivers | CrowPanel 2.1 RGB panel | Apache 2.0 |
| Espressif [FT5x06 touch](https://components.espressif.com/components/espressif/esp_lcd_touch_ft5x06) driver | CoreS3 touch; ILI9341 and esp_codec_dev also provide its display and AW88298 audio drivers | Apache 2.0 |
| [esptool-js](https://github.com/espressif/esptool-js) | Flashing from the browser installer, added to the site when it's built | Apache 2.0 |
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

The AMOLED panel's start-up register values in `firmware/esp32/main/port_amoled.cpp` follow Waveshare's [board support package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) for the ESP32-S3-Touch-AMOLED-1.75 (Apache 2.0, text in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); see [NOTICE](NOTICE). The 1.75C profile reuses that driver and its license notice. The rectangular AMOLED-1.8's start-up table (`kInit368`) follows Waveshare's [AMOLED-1.8 board support package](https://github.com/waveshareteam/Waveshare-ESP32-components/tree/9f4030c6e5cb888ad4cc268bfa7584c93ad53e30/bsp/esp32_s3_touch_amoled_1_8) at commit `9f4030c6e5cb888ad4cc268bfa7584c93ad53e30` (Apache 2.0). Its pin map and audio supply configuration follow the [manufacturer's schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf).

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

The T-Display-S3 ST7789 initialization values and AW9364 backlight control follow
[LilyGO's factory example](https://github.com/Xinyuan-LilyGO/T-Display-S3/blob/ec889e789b3cf093412689a143f7f37b42b56af7/examples/factory/factory.ino),
revision `ec889e789b3cf093412689a143f7f37b42b56af7`, MIT, Copyright (c) 2022 Xinyuan-LilyGO.
The ESP-IDF adaptation and timing changes are recorded in [NOTICE](NOTICE), with
the upstream license in [LICENSES/LilyGO-MIT.txt](LICENSES/LilyGO-MIT.txt).

The AIPI Lite pin map, panel settings and power wiring follow [xiaozhi-esp32's `xorigin/aipi-lite` board](https://github.com/78/xiaozhi-esp32/tree/0d576d3d4c049c6f55eaf879725dc23e516511b4/main/boards/xorigin/aipi-lite) at commit `0d576d3d4c049c6f55eaf879725dc23e516511b4` (MIT). Only hardware facts are used; see [NOTICE](NOTICE).

The CrowPanel 2.1 ST7701 initialization values in `firmware/esp32/main/crowpanel_st7701_init.hpp`, and the RGB timings, PCF8574 power and reset sequence and pin map in `port_rgb.cpp` and `board.cpp`, follow Elecrow's [RotaryScreen_2_1 example](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen/tree/faf8ecf27ec1504b51de7dc4e15b2e7d7e87c79c/example/Arduino/RotaryScreen_2_1) at commit `faf8ecf27ec1504b51de7dc4e15b2e7d7e87c79c`. That repository has no license, so only these hardware facts are used, re-expressed for Espressif's `esp_lcd_st7701` driver; see [NOTICE](NOTICE).

The CrowPanel 5.0 ILI6122 RGB pin map, scanout timings (15 MHz PCLK, porches, `pclk_active_neg`), GPIO 2 backlight, and the GT911 I2C touch wiring in `port_rgb.cpp`, `board.cpp` and `port_touch.cpp` follow Elecrow's [crowpanel-esp32-5.0-3.0 example](https://github.com/Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480/tree/fd959ac9ac900bb536ad72eebb9d2c14c7ffc7b7/example/V1.0_and_V2.0/Arduino/Arduino-Hardware-Version-3.0/crowpanel-esp32-5.0-3.0) at commit `fd959ac9ac900bb536ad72eebb9d2c14c7ffc7b7`. That repository has no license, so only these hardware facts are used, re-expressed for Espressif's built-in `esp_lcd_rgb` and managed `esp_lcd_touch_gt911` drivers; see [NOTICE](NOTICE).

## Release packages

Firmware packages include `hermes-gadget-<version>-licenses.zip` with this document, the project's notices, license texts, and notices found in the installed ESP-IDF and managed component sources. The browser installer links that archive when the release supplies it.

Python distributions and Linux device packages also include this document and the project's license texts and notices. The Linux installer copies them to `/opt/hermes-gadget/current/licenses/`.
