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

## Adapted display initialization tables

The BOX-3 panel register values follow [Espressif's BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/esp-box-3), Apache 2.0. The adapter uses zero-length sleep/display commands with explicit delays.

CoreS3's ILI9342E initialization table follows [Espressif's CoreS3 BSP](https://github.com/espressif/esp-bsp/tree/master/bsp/m5stack_core_s3), Apache 2.0. Its copyright and modification notice are retained in the source and [NOTICE](NOTICE). The power and reset adapter is original code based on the board's documented wiring and chip registers.

The AMOLED panel's start-up register values in `firmware/esp32/main/port_amoled.cpp` follow Waveshare's [board support package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) for the ESP32-S3-Touch-AMOLED-1.75 (Apache 2.0, text in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); see [NOTICE](NOTICE). The 1.75C profile reuses that driver and its license notice. Its pin map and audio supply configuration follow the [manufacturer's schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75C/ESP32-S3-Touch-AMOLED-1.75C-schematic.pdf).

## Release packages

Firmware packages include `hermes-gadget-<version>-licenses.zip` with this document, the project's notices, license texts, and notices found in the installed ESP-IDF and managed component sources. The browser installer links that archive when the release supplies it.

Python distributions and Linux device packages also include this document and the project's license texts and notices. The Linux installer copies them to `/opt/hermes-gadget/current/licenses/`.
