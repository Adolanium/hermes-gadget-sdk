# Hardware capabilities and verification

The Raspberry Pi 4/5 Linux port is experimental. ARM64 CI covers the native
core, service, package installation and updates. USB audio, GPIO, and display
tests use software drivers or test doubles. No physical Pi report is recorded.

Firmware builds and simulator tests check software behavior. A physical verification report records what worked on a particular board revision, wiring, and firmware commit. A passing build alone does not establish that a microphone, power circuit, or display works on a device.

## Current hardware

| Model | Display and input | Audio | Power support | Verification |
|---|---|---|---|---|
| ESP32-S3-DevKitC-1 N8R8 breadboard | Wired ST7789, TALK and CANCEL buttons | Wired I2S microphone; optional MAX98357A speaker | External power | CI build; physical report not recorded |
| Waveshare ESP32-S3-LCD-1.54, SKUs 33866/33867, and Touch-LCD-1.54, SKUs 33868/33869 | ST7789 240×240; BOOT, PLUS and PWR (screen on/off, hold to power off); CST816 touch on the touch SKUs | ES7210 microphones and ES8311 speaker | Calibrated voltage, charging signal, battery latch and screen timeout; idle CPU at 80 MHz and light sleep while dozing on battery; USB bypasses shutdown | CI build; physical report not recorded |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | CO5300 466×466; CST9217 touch, BOOT and PWR; QMI8658 motion sensor; PCF85063 RTC | ES7210 microphones and ES8311 speaker output | AXP2101 readings and local power-off; optional screen timeout; face-down sleep (off by default); resting clock (off by default) | CI build; physical report not recorded; the face-down sleep axes were checked on one unit (x standing, z face down; y derived, not measured; the 1.75C and the other QMI8658 boards not checked); the resting clock was checked on one unit (the PCF85063 sets the time at boot, network time sets the PCF85063, the face shows after the rest, a tap brings Ready back without recording; the settings item, hold to talk, the 12-hour format, the backlight level while the face shows and the 2.06 and 2.16 RTCs not checked) |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C, SKUs 33691/33692 | CO5300 466×466; CST9217 touch, BOOT, and PWR short press for screen sleep | ES7210 microphones and ES8311 onboard speaker | AXP2101 readings, audio supply and local power-off; screen timeout; amplifier off between sounds; idle CPU at 80 MHz; battery percent from measured curves; light sleep while dozing on battery | Experimental; [SKU 33691 report](#waveshare-amoled-175c-physical-report) covers the checklist on a working branch, not a `main` build; PCB revision not marked |
| Waveshare ESP32-S3-Touch-AMOLED-1.8, V2 only | CO5300 368×448; CST820 touch, BOOT and PWR (CANCEL, through the TCA9554) | ES8311 analog microphone and speaker output | AXP2101 readings and local power-off; display/touch reset through a TCA9554 expander | Experimental; physical smoke check only (boot, display, I2C devices present, codecs and the touch, speaker and microphone tasks start); touch, microphone capture, speaker playback and battery not verified; full checklist not completed |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 (watch) | CO5300 410×502; FT3168 touch, BOOT and PWR short press for screen sleep | ES7210 microphones and ES8311 onboard speaker | AXP2101 readings and local power-off; screen timeout; amplifier off between sounds; idle CPU at 80 MHz; light sleep disabled pending touch wake support | Experimental; [partial report](#waveshare-amoled-206-partial-physical-report): display, touch, buttons, voice turns and tap mode on USB power; network loss, OTA, battery operation and a long session not verified |
| Waveshare ESP32-S3-Touch-AMOLED-2.16, SKU 33969 | CO5300 480×480; CST9220 touch, BOOT and PWR short press for screen sleep | ES7210 microphones and ES8311 speaker output | AXP2101 readings and local power-off; battery header, battery operation not tested | Experimental; [physical smoke check](#waveshare-amoled-216-physical-smoke-check) on USB power only: boot, display, both codecs, the touch task, the AXP2101 and the Wi-Fi setup network start; touch, microphone capture, speaker playback, pairing and battery not verified; full checklist not completed |
| Waveshare ESP32-S3-Touch-LCD-1.85C V2 / PCB Rev2.0 only | ST77916 360×360 round QSPI LCD; CST816 touch and BOOT | ES8311 + ES7210 dual analog mic slots, NS4150B PA; mono transport; no software AEC | USB/battery switch; screen timeout; no battery telemetry or software shutdown | Experimental; [partial Rev2.0 report](#waveshare-185c-v2-partial-physical-report); full checklist not completed |
| Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM) | SH8601 360×360 round QSPI LCD; CST816S touch, rotary encoder and BOOT | One PDM microphone; no speaker output (the PCM5100A headphone DAC is not driven) | USB power; battery ADC, charging and haptics not read | Experimental; contributor smoke test (display, hold-to-talk touch, microphone capture through a voice turn with a text reply); full checklist not completed |
| Espressif ESP-VoCat, v1.2 only (ESP32-S3 rev v0.2, 16 MB flash, 16 MB octal PSRAM) | ST77916 360×360 round QSPI LCD; CST816S touch and BOOT | ES7210 microphones (MIC1 captured, mono transport) and ES8311 speaker through an NS4150B; MIC3 speaker loopback unused | GPIO power switches for the LCD/SD rail and the codec supply; battery, BQ27220 gauge and head touch pads not read; no software shutdown | Experimental; contributor smoke test on USB power (boot, display, both codecs, touch, Wi-Fi, pairing, a hold-to-talk voice turn); full checklist not completed |
| Elecrow CrowPanel Advanced 10.1-inch ESP32-P4 V1.1 | 1024×600 MIPI-DSI; GT911 touch; BOOT/TALK | PDM microphone; I²S speaker output | STC8 battery/charger status; uncalibrated | Experimental; [partial physical report](#crowpanel-advanced-p4-partial-physical-test-record): display/touch, normal speech submissions and speaker playback confirmed by the owner on an earlier build of the port, not this one; battery-only Wi-Fi startup issue remains unexplained; full checklist incomplete |
| Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (SKU 31523; ESP32-P4 rev v1.3) | JD9365 800×800 round MIPI-DSI; GT911 touch; BOOT | ES8311 speaker + ES7210 microphone | USB power only; no battery connector or telemetry | Experimental; [partial physical report](#waveshare-p4-touch-lcd-34c-partial-physical-report): display and C6-hosted Wi-Fi confirmed by the owner; touch, microphone capture, speaker playback, OTA and the full checklist not completed |
| Xorigin AIPI Lite | ST7789 128×128; BOOT and power keys | One ES8311 for speaker and microphone | GPIO 10 power latch and power-off; battery level and charging not reported | Experimental; contributor smoke test on an earlier revision (boot, display and audio start, online pairing); microphone capture, colors, USB console and battery not verified on this revision |
| Spotpear ESP32-S3-1.28-BOX (SKU `sp-esp32-s3-1.28-box`) | GC9A01 240×240 round; CST816D touch on its own I2C bus, BOOT | One ES8311 for speaker and microphone | GPIO 3 battery latch and power-off; battery voltage on GPIO 1 (divider ratio 2, not measured) and charging on GPIO 41, active high | Experimental; firmware builds; not run on a board; full checklist not completed |
| Spotpear ESP32-S3-1.54 muma (touch version, SKU `sp-esp32-s3-1.54-muma`; ESP32-S3 rev v0.2; PCB revision not marked) | ST7789 240×240; CST816D touch on its own I2C bus, BOOT | One ES8311 for speaker and microphone | GPIO 3 battery latch and power-off; battery voltage (divider by two, inferred from upstream, not metered) and active-high charging signal; external power from the charging signal or a USB host; idle CPU at 80 MHz and light sleep while dozing on battery | Experimental; owner smoke test on two units on USB power (boot, display, touch, phone Wi-Fi setup, pairing, voice turns, agent-driven volume and brightness); battery readings on one unit: on USB `diag` showed 3418 mV, charging and external power; on battery the latch held and the Power page showed 3512 mV and Not charging; colors, touch coordinates, power-off, external power on battery and OTA not verified; full checklist not completed |
| Espressif ESP32-S3-BOX-3 | ST7789/TT21100 or ILI9342/GT911, detected through I2C; BOOT and touch gestures | ES7210 microphones and ES8311 onboard speaker | USB power; screen timeout | Experimental; physical report not recorded for either panel revision |
| M5Stack CoreS3 (K128) | ILI9342C/E 320×240, detected through touch firmware; FT6336 gestures | ES7210 microphones and AW88298 speaker | AXP2101 readings, backlight and local power-off; AW9523 reset/boost control | Experimental; physical report not recorded for either panel revision |
| LilyGO T-Display-S3 (SKU/Version H587; PCB revision 1.2) | ST7789 320×170 over 8-bit i80; BOOT and Button2 | No onboard audio | Battery divider; no fuel gauge; GPIO15 powers the panel rail, not device shutdown | Experimental; physical smoke check only (screen ready/replies, Wi-Fi, online pairing); full physical checklist not completed; hardware-verified status not claimed |
| Elecrow CrowPanel 2.1-inch HMI (ESP32-S3R8, 16 MB flash) | ST7701 480×480 RGB round panel; CST-family touch, rotary encoder, encoder button on PCF8574 | No onboard audio | USB power; screen timeout | Experimental; physical smoke check only (panel bring-up, brightness scale, Wi-Fi, online pairing, serial console); touch coordinates and encoder direction not physically verified |
| Elecrow CrowPanel 5.0-inch HMI (DIS07050H, ESP32-S3R8, 4 MB flash) | ILI6122/ILI5960 800×480 RGB panel; GT911 touch on I2C, INT not wired (polled) | No onboard audio | USB power; screen timeout | Experimental; boot smoke check only (panel initialization log, GT911 ID, Wi-Fi, online pairing); rendering, colours, brightness and touch coordinates not physically verified |
| ESP32-2432S028 CYD (Cheap Yellow Display), two-USB revision (micro-USB and USB-C); classic ESP32-WROOM-32, 4 MB flash, no PSRAM | ST7789 320×240 on SPI2; BOOT only (the XPT2046 touchscreen is not driven) | No microphone fitted; a PDM MEMS microphone on the 4-pin header (PDM clock GPIO 22, data GPIO 27, on I2S0's PDM-to-PCM converter) is driven by the profile. The SC8002B amplifier behind the speaker header plays three boot beeps through the chip's DAC | USB power; screen timeout | Experimental; contributor smoke test on a Guition ESP32-2432S028 (two-USB, ST7789) with an earlier build of the port, not this one: the display (that build drew in monochrome), DAC speaker output, and Wi-Fi and pairing set with `menuconfig`; touch was off and no microphone was fitted. This build runs on no board yet; full checklist not completed. The banded framebuffer (120 rows; the UI renders in bands) is checked only by host core tests that compare it with whole-panel rendering; colours, orientation, brightness, free heap beside the band, the boot beeps and the PDM microphone are not verified |

The LCD-1.54 `-EN` SKU uses the same hardware. The separate Touch-LCD-1.54 model adds a CST816 touchscreen, which the same profile drives. AMOLED-1.75C has its own firmware profile; its reset and audio clock pins differ from the 1.75 model. See [hardware and wiring](hardware.md) for connections and exact model names.

A [partial LCD-1.54 physical report](#waveshare-lcd-154-partial-physical-report) covers the Touch-LCD-1.54 SKU 33869 running the LCD-1.54 profile. It does not verify the separate non-touch SKUs.

CI builds and packages every profile: the PlatformIO profiles with PlatformIO, and the CrowPanel P4 with ESP-IDF 6.1.0 in a separate job. Releases include the CrowPanel P4 image from the next tagged release on; its bootloader sits at 0x2000, as the ESP32-P4 requires, so the merged image is still flashed at 0x0. The browser installer lists profiles included in the latest published release, so newly merged profiles may require a source build until the next release. Other chips, wiring, and unlisted hardware revisions are porting targets, not verified configurations.

## Waveshare LCD-1.54 partial physical report

- **Board / revision:** owner-identified Waveshare ESP32-S3-Touch-LCD-1.54, SKU 33869. PCB revision not established. USB discovery reported ESP32-S3 (QFN56) revision v0.2, 16 MB flash and 8 MB embedded PSRAM. This report predates touchscreen support.
- **Firmware:** local version 0.2.0 with the opt-in battery-estimate patch, built with ESP-IDF 6.1.0 using `esp32s3-lcd-154`; not a published release. The installed application SHA-256 is `38c40ee787633aad3056385ec66ea84839cef4dacbea8a3a72bfc89c556591a1`.
- **Host / Hermes:** USB installation from Linux. The live Hermes version was not independently recorded; gateway configuration was not changed.
- **Supply / battery:** USB during installation. The owner confirmed a battery was physically connected and reported its package label verbatim: `SKU 33228 3.7V 1000mA MX1.25 2P 50mm`. Capacity and chemistry were not independently established; the label's `1000mA` is not treated as a measured capacity.
- **USB install / recovery:** Secure Boot and flash encryption were disabled. Two fresh full 16 MiB flash reads matched byte-for-byte and by SHA-256; the installed partition table matched the build before upload. The PlatformIO upload passed write verification, and a separate application readback verification matched. Backup restoration was not attempted.
- **Identity / settings / pairing:** the original device identity, name, gateway setting and all pre-existing logical NVS values were preserved. NVS was not byte-identical after boot: the firmware added the saved-network migration entry `wifi_nets`. The device reconnected online and paired. `battery_estimate=liion` and `battery_display=percentage` were read back after a hardware reset.
- **Display / audio:** boot logs reported ST7789, ES7210 and ES8311 initialization. The owner confirmed the physical header percentage was visible and a short real spoken question and spoken reply worked. This is owner-reported physical evidence, not a simulator result or an independent screen/audio measurement.
- **Power / estimate limitations:** activation followed the owner's fitted-battery confirmation. One raw reading was 4131 mV with charging false, which maps to 92% using the linear estimate; raw telemetry still omitted battery percentage, presence and external-power state. The owner subsequently observed green 100% with USB connected and an immediate 88% after unplugging. This is consistent with a voltage estimate changing with charging/load conditions, not evidence that 12% of stored energy was lost. No simultaneous meter measurements, calibrated state-of-charge accuracy or charge-completion claim is established.
- **Network / serial:** normal boot and restart reconnection were observed. Early status could show no network before startup settled. One post-reset console query timed out; subsequent startup capture and console synchronization succeeded. Forced Wi-Fi/gateway loss and recovery were not exercised.
- **Later check, touch and PWR:** on the same unit, with firmware from the change that added them, holding the screen recorded voice turns and a swipe down cancelled; a short PWR press turned the screen off and on, and holding PWR on battery powered the board off. Owner-reported; tap coordinates were not checked.
- **Untested checklist items:** exact PCB revision; meter comparison and charging accuracy; controlled battery-versus-USB operation and runtime; full display/color/button checks; individual microphones, audio interruption and distortion measurements; screen timeout, sleep and shutdown; phone setup; hostile/unapproved pairing; OTA/rollback; backup restoration; and a two-hour stability session.

The port stays experimental. This report covers a bounded feature smoke check, not the complete physical checklist or battery calibration.

## CrowPanel Advanced P4 partial physical test record

This record describes the pre-port hardware test on one Elecrow V1.1 unit. It does **not** validate the current upstream-based port branch, which has only been host-tested and built, not flashed. The official [Elecrow product page](https://www.elecrow.com/crowpanel-advanced-10-1inch-esp32-p4-hmi-ai-display-1024x600-ips-touch-screen-wifi-6.html) specifies 1024×600; older notes saying 1280×800 were incorrect.

```text
Model / PCB revision: Elecrow CrowPanel Advanced 10.1-inch ESP32-P4 V1.1; finer PCB revision not recorded. Owner confirmed IC5 and IC6 shown on the schematic are not populated on this unit.
Firmware version / commit: Prior port-branch build ID d7c7fe9-stc8-register-list, based on source commit prefix d7c7fe9; ESP-IDF 5.5.4. This is not a build of the current upstream-based port. App-only image at 0x20000; SHA-256 51b5be8a8ebe4f933a56ff5c4ce07f9f36f4762401224c8a2ab5ca8d71882f96; readback was reported verified. Gateway reported the build ID; an on-screen ID confirmation was not recorded.
Hermes version / host OS: Test-time Hermes version not recorded. Test notes identify a Linux/Manjaro host; no private hostname, address, or path is included here.
Flash / PSRAM: Elecrow specification and profile: 16 MB flash and 32 MB PSRAM. A physical boot memory report was not preserved.
Power supply / battery / peripherals: USB-C and a single-cell battery on J3 were used in different tests. Cell make and capacity were not recorded. Built-in 1024×600 display, GT911 touch, PDM microphone, speaker output and ESP32-C6 Wi-Fi companion.
USB install and recovery: App-only flash at 0x20000 with readback verification; bootloader, partition table and NVS were not flashed in these updates. A rollback image was retained, but physical rollback and full-flash recovery were not tested.
Pairing and persistence: Device was paired and appeared online at the Gateway after Wi-Fi setup. Identity persistence across reboot/update and refusal of an unapproved device were not formally tested.
Display / buttons / touch: Owner confirmed display and touch operation during bring-up. The official resolution is 1024×600. Full-screen touch accuracy, color calibration and all physical buttons were not recorded as checklist tests.
Microphone / speaker / interruption: Normal speech submissions worked, as reported by the owner. A speaker tone was physically heard; the owner later reported a reply played without breakup and said playback was perfect. The PDM reader produced about 50 blocks/s with no read errors in a diagnostic run, but that software rate alone is not acoustic proof. No calibrated acoustic measurement or full interrupt/cancel checklist was recorded.
Network loss and recovery: Wi-Fi and Gateway pairing were established. Deliberate Wi-Fi/Gateway loss and recovery, phone setup failure/cancellation/expiry, and reachability of the temporary setup page were not tested. One earlier reply cutoff was not diagnosed; later playback was reported normal.
Power and battery: The STC8 readings are not calibrated state-of-charge measurements. With USB connected, the display reported 4218 mV / 100% while a meter at the cell terminals read 4.114 V; after unplugging, the display reported 3779 mV / 39%, without a simultaneous cell-terminal reading. A separate live sample reported 4093 mV, ADC 2945 mV, 84%, fully_charged and charging=false; USB state was not recorded. With USB and no battery, the user measured J3 at 3.29 V and the R187/R188 junction at 3.32 V; these are open-node readings. With battery attached and USB disconnected, board power off, the user measured 4.07 V at the cell and 2.92 V at the divider junction. J13 measured 3.29 V only when the board was on. During a later USB/battery observation the charge LED changed briefly from red to green; cell terminals read 4.10 V. The user reported an 85.3 mV differential from board-side J3 positive to cell positive at the same time, with 0.3 mV between J3 negative and J13 ground. This is consistent with current flowing toward the cell in the stated probe orientation, but it does not quantify current; the user considered it sufficient for now. At 100% brightness, battery-only Wi-Fi startup powered the screen down; a separate 50% test rebooted as Wi-Fi connected, with reported cell voltage about 4.11 V to 4.08 V and J13 about 3.29 V to 3.28 V. Cause and reset reason remain unknown. A 1 F capacitor was suggested but not tested; compatibility and electrical limits have not been reviewed.
OTA / rollback: OTA, settings persistence through OTA and physical rollback were not tested.
Two-hour session: Not run.
Failed or untested steps: Battery-only Wi-Fi startup powerdown/reboot cause remains unknown; no calibrated battery estimate or measured charge-current magnitude; no full touch/button/color test; no deliberate network recovery; no OTA/rollback; no two-hour stability run.
Sanitized logs and diagnostics: No sanitized diag report or log bundle is attached.
```

The port remains experimental. The partial physical observations above belong to the prior firmware and must not be read as validation of this upstream port.

## Waveshare P4 Touch-LCD-3.4C partial physical report

- **Board / revision:** Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C, SKU 31523. ESP32-P4 rev v1.3, 16 MB flash (QIO, 80 MHz) and 32 MB PSRAM (HEX, 200 MHz). PCB revision not marked. No battery connector; USB-C power only.
- **Firmware:** local build `0.2.0-ws34c-usbconsole`, ESP-IDF 5.5.4, built with `idf.py` from the `ws-p4-touch-lcd-3.4c` board profile; not a published release. App-only image at `0x20000`, SHA-256 `34fcec816590ce6cea6254dc19179e3a62b2b3d7e5a01f2893794a5f5189cfa6`. The board profile is not merged yet; this report describes the working tree that produced the image.
- **Host / Hermes:** Linux. The live Hermes version was not recorded; gateway configuration was not changed.
- **Display:** boot logs reported `jd9365-dsi`. The owner confirmed the panel renders: Waveshare's own color-bar example drew hardware color bars, and the Hermes Gadget UI then drew correctly. A central streak instead of a full image appeared on builds that had reverted to the CrowPanel EK79007 driver; restoring the JD9365 path fixed it.
- **Touch:** GT911 detected on I2C. Coordinates and orientation not verified.
- **Audio:** ES8311 and ES7210 start. Microphone capture, speaker playback and levels not verified.
- **Wi-Fi / C6:** the ESP32-C6 companion carries Wi-Fi over 4-bit SDIO. With a working C6 image the boot log reports `Identified slave [esp32c6]` and `Base transport is set-up`; the owner confirmed the device joined the network and reached the normal Gadget UI.
- **Pairing and persistence:** paired. Reconnect after restart confirmed by the owner; identity and pairing survived.
- **Factory reset:** confirmed by the owner.
- **Long run:** a one-hour run confirmed by the owner.
- **Untested:** touch coordinates and orientation; microphone capture and level; speaker playback; deliberate network loss and recovery; OTA and rollback; the two-hour session; the full checklist.
- **Not implemented:** battery telemetry and software shutdown (no battery on this board).

### Getting the C6 working on this board

The C6 companion does not ship with an esp_hosted slave image this host can talk to, and the host's `g_wifi.begin()` gives up after about 10 s, leaving the gadget offline. Every step below was needed before Wi-Fi came up, and each one is its own trap.

1. **Flash a C6 slave image.** The image that worked was `network_adapter.bin` from the CrowPanel C6 firmware package (V2.12.3). The slave image already on the board did not work.
2. **The C6 is programmed over its own UART, not the P4's USB-JTAG.** On this board that is the SH1.0 header (J4). Wire a USB-UART bridge to it and flash with esptool. This UART is a separate physical link from the P4↔C6 hosted SDIO link used in normal operation, so it can be unplugged afterwards without affecting Wi-Fi.
3. **C6 reset is P4 GPIO54, not GPIO32.** The schematic routes C6 `CHIP_PU` to P4 GPIO54. An earlier configuration pulsed GPIO32, which resets nothing on this board. `CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54`, active high.
4. **SDIO lane mapping.** P4 side: CMD 19, CLK 18, D0 14, D1 15, D2 16, D3 17. C6 side: D0..D3 are GPIO20..23. 4-bit bus at 40 MHz.
5. **`g_wifi.begin(g_storage)` must actually run.** It was found commented out in the test tree, so Wi-Fi never started even with a good C6 image. Check it before blaming the C6.
6. **The console is on the P4 USB Serial/JTAG, not UART0.** The board profile defaulted to `CONFIG_ESP_CONSOLE_UART_DEFAULT`, so console commands typed over the connected USB-JTAG never reached the app and every `set`/`get` looked ignored. Set `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in the board's `sdkconfig.defaults`.
7. **Non-interactive `idf_monitor` needs `--eol CRLF`.** The USB Serial/JTAG REPL expects CRLF; with the default LF the monitor reports `Unknown command` for every line.
8. **Silicon revision.** This unit is rev v1.3. `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` with `CONFIG_ESP32P4_REV_MIN_100=y` and `CONFIG_ESP32P4_REV_MAX_FULL=199` keeps the bootloader and esptool revision check happy.
9. **Flash the app only.** `esptool write_flash 0x20000 hermes_gadget.bin` preserves the bootloader, partition table and NVS, so Wi-Fi settings and pairing survive an app update.

The port stays experimental.

## Waveshare 1.85C V2 partial physical report

- **Board:** PCB Rev2.0 speaker-box version with ESP32-S3, 16 MB flash and 8 MB PSRAM, powered over USB.
- **Firmware:** `fb8f81e`, built with ESP-IDF 6.1 and tested on 2026-10-07 after an app-only update to `ota_0` on the same board.
- **Earlier firmware:** combined builds of this port's branch up to `edcbd66`. All of them had the SPI flush-timeout change for every `SpiDisplay` panel. The later ones also had the round settings target. The network tests used the branch's own mapped-IPv4 provisioning guard, which main's `ipv4_of` replaces.
- **Confirmed on `fb8f81e`:** a readable, upright display; touch and brightness; the local test tone; a spoken question with an audible Hermes reply; and swipe-down cancel during a reply. Playback stopped, and the next reply played.
- **Earlier results:** the combined builds passed display, touch, brightness, microphone level, test tone, Wi-Fi setup, WSS, pairing and spoken reply checks.
- **Updates:** Wi-Fi and pairing survived the app-only update to `fb8f81e`. The NVS Wi-Fi, server and device-key entries did not change, and the device reconnected online and paired without setup. Earlier app-only updates passed readback verification with byte-identical NVS and the same paired identity.
- **Earlier power cycle:** on the combined builds, after USB was unplugged and plugged back in, the device reconnected without Wi-Fi setup or re-pairing. This does not test losing Wi-Fi while powered.
- **Not verified:** battery operation, OTA and rollback, long-run stability, interrupting touch or audio, recovery from injected faults, Wi-Fi loss while powered, each microphone on its own, and touch accuracy across the whole screen.
- **Not implemented:** software echo cancellation, battery telemetry and software shutdown.

The port stays experimental.

## Waveshare AMOLED-1.75C physical report

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-1.75C, SKU 33691 (enclosed model). The PCB revision is not marked on the board or the case. ESP32-S3 (QFN56) revision v0.2, eFuse block v1.4; 32 MB GigaDevice flash in DIO at 80 MHz; 8 MB octal PSRAM (AP Memory, 64 Mbit, generation 3). A 500 mAh Li-ion cell; USB from a laptop and a USB-C charger.
- **Firmware:** [`30f8bc0`](https://github.com/tozes/hermes-gadget-sdk/commit/30f8bc061db8ff0c39c9a5191213eb660210ef5f) on `tozes/hermes-gadget-sdk`, a branch based on `main` (`a321be5`) that also carried other changes for this board, built with ESP-IDF 6.1.0; tested 2026-10-08 to 2026-10-09. Among them were the fixes for two bugs that made OTA and recovery from a lost network fail on this board, since merged as [#104](https://github.com/Adolanium/hermes-gadget-sdk/pull/104) and [#105](https://github.com/Adolanium/hermes-gadget-sdk/pull/105) (see below).
- **Hermes:** Hermes Agent v0.21.6+199.g1744a19 on Fedora 44, over WSS.
- **USB install and recovery:** installed with `pio run -e esp32s3-touch-amoled-175c -t upload`. esptool verified every write. Wi-Fi, pairing and the server setting survived, since the settings partition is not erased. The device booted, upright with correct colours, and reconnected without setup.
- **Pairing and persistence:** the identity and pairing survived every reboot, the OTA updates, the USB install and a rollback. An unapproved device could not issue actions (checked before this unit was paired).
- **Display, buttons and touch:** display, touch, BOOT as TALK, swipe-down cancel and the settings hold work. Holding PWR turns the board off through the AXP2101.
- **Microphone, speaker and interruption:** spoken questions were transcribed and answered with complete spoken replies. Cancelling a recording, interrupting a reply, volume, brightness and the hardware checks behaved as expected, with no distortion.
- **Network loss and recovery:**
  - Wi-Fi was lost by covering the board, at −85 to −91 dBm, and by carrying a phone hotspot out of range. The gateway was also restarted. With the branch's WebSocket fix the device recovered each time without a reset.
  - **Before [#105](https://github.com/Adolanium/hermes-gadget-sdk/pull/105), `main` froze after a Wi-Fi loss** until a hard reset, because the WebSocket close blocked the app task.
  - Phone setup worked with valid and wrong passwords, cancellation and the ten-minute expiry, and fell back to the previous network. The setup page was not reachable from the station address. USB setup worked after phone setup closed.
- **Power and battery:** battery voltage, percentage, charging and USB power are reported. From empty, charging took 2.5 h to "charge done"; the AXP2101's charge limit reads 200 mA. Screen-timeout dimming and Power off from the settings menu work. USB and battery operation were checked separately.
- **OTA and rollback:**
  - With the branch's board-tag fix, 11 updates over Wi-Fi installed and were kept, with settings intact.
  - An image that never reaches Hermes rolled back to the previous firmware when its 5-minute probation expired, and the device came back online and paired.
  - **Before [#104](https://github.com/Adolanium/hermes-gadget-sdk/pull/104), `main` refused every OTA image** ("the image is for , not this board").
- **Two-hour session:** in use from the evening of 2026-10-08 to the afternoon of 2026-10-09, with repeated voice turns, reconnects and updates. On the final build, with both fixes, there were no resets, freezes or audio failures.
- **Not verified:** the PCB revision; touch accuracy across the whole screen; each microphone on its own.
- **Logs:** a sanitized `diag` report and the serial logs of the USB install and the rollback are attached to the pull request.

Both fixes are in `main`. The port stays experimental: the PCB revision is not marked, and the tested firmware was a working branch rather than a build of `main`.

## Waveshare AMOLED-2.06 partial physical report

- **Board:** Waveshare ESP32-S3-Touch-AMOLED-2.06 in its watch case, with a 400 mAh Li-ion cell fitted; the PCB revision was not recorded. ESP32-S3 revision v0.2, eFuse block v1.4; 32 MB GigaDevice flash (DIO, 80 MHz); 8 MB octal PSRAM (AP Memory, generation 3). The vibration motor footprint is not populated on this unit. Powered over USB from the Hermes host for the whole test.
- **Firmware:** [`567cae2`](https://github.com/Geofresh/hermes-gadget-sdk/commit/567cae2) on `Geofresh/hermes-gadget-sdk`, the branch of this pull request, based on `main` (`3a24180`); built with ESP-IDF 6.1.0 and tested on 2026-10-14. Earlier builds of the same branch found the three defects below.
- **Hermes:** Hermes Agent v0.21.5+7527.g4787e4d on Fedora 43, over a plain WebSocket on the local network (not WSS).
- **USB install:** installed with `pio run -e esp32s3-touch-amoled-206 -t upload`; esptool verified every write. The unit had been running other firmware with a larger settings partition; its Wi-Fi, server and pairing entries were still read after the install, and it came up online and paired without setup. Reflashed about ten times during the test with the same result.
- **Display:** upright, not mirrored, correct colours. Found and fixed during the test:
  - Green noise strips along the top and bottom edges: the rows above and below the 486-row window were never written (blanked at start-up since the earlier branch revision [`af3ead6`](https://github.com/Geofresh/hermes-gadget-sdk/commit/af3ead614a7e642fee5ba84a0758cf8952f38353)).
  - The status row's title and link label were clipped by the rounded corners. A test grid over the whole panel showed all 410×502 pixels lit, with corners of about 100 px radius; `corner_inset` is now 64.
- **Touch and buttons:** holding the screen records, a swipe down cancels a recording and stops a playing reply, BOOT records, a short PWR press turns the screen off and on, and a touch or BOOT wakes it. Settings open and close by holding the title bar. Found and fixed during the test: the FT3168 dozes after a few idle seconds. It then refused most reads, and polled every 20 ms an idle watch logged hundreds of I2C errors a minute. It also reports the touch that wakes it with one short INT pulse, so the first tap after a pause was lost. Reads now follow INT's falling edge (`567cae2`): no touch errors in 6 minutes of use and idle, and the first tap after 1.5 to 2 minutes idle registered every time.
- **Microphone, speaker and interruption:** spoken questions were transcribed and answered with complete spoken replies. Tap mode heard speech at a normal speaking volume with the profile's 30 dB gain and 200 RMS floor; with the default floor of 500, it had treated speech as silence. `playback ended:` lines after replies: `0 underruns`; `1 underruns, 32 ms in total, longest 32 ms`.
- **Diagnostics:** `hermes-gadget diag` listed `0x18` (ES8311), `0x34` (AXP2101), `0x40` (ES7210), `0x51` (PCF85063) and `0x6b` (QMI8658). `0x38` (FT3168) did not answer: the scan ran while the touch chip was dozing.
- **Logged on every recording start, not fixed here:** `i2s_channel_disable(1516): the channel has not been enabled yet`, from esp_codec_dev 1.5.11, which disables the receive channel before reconfiguring it without checking that it runs. Any board that powers its ES7210 down between recordings triggers it; recording works.
- **Not verified:** network loss and recovery, phone setup, OTA and rollback, battery operation and charging, screen timeout and dimming, light sleep while dozing on battery, touch accuracy across the whole screen, volume and brightness steps, the settings hardware checks, each microphone on its own, a two-hour session.

After this report, review disabled light sleep for this board. Its FT3168 interrupt is not configured as a CPU wake source, so a report can expire while the CPU sleeps. Light sleep needs GPIO wake support and a physical battery wake test before it can be enabled. Review also made display initialization fail when margin clearing fails or times out, so drawing cannot overwrite a pending DMA transfer. These follow-ups were not tested on the physical board. The port stays experimental.

## Waveshare AMOLED-2.16 physical smoke check

- **Board / revision:** Waveshare ESP32-S3-Touch-AMOLED-2.16, SKU 33969. USB discovery reported ESP32-S3 chip revision v0.2, 16 MB flash and 8 MB octal PSRAM (gen-3 die, 80 MHz). PCB revision not established.
- **Firmware:** local version 0.2.0 on branch `feat/waveshare-esp32s3-touch-amoled-216`, built with ESP-IDF 6.1.0 via PlatformIO using `esp32s3-touch-amoled-216`. Not a published release.
- **Host:** macOS. USB install through the board's own USB Serial/JTAG port; live Hermes gateway not connected for this smoke check (device entered its unpaired softAP setup mode as intended).
- **Boot / display:** 2nd-stage bootloader and app load; `CO5300 480x480 ready`; PSRAM found and memory test OK; internal heap 211 KB free plus 7673 KB PSRAM. The boot log is summarized here; the full log was not attached to the PR.
- **Codecs:** `codecs: speaker ready, microphones ready (es7210)`; ES8311 (speaker) and ES7210 (dual MIC1/MIC2) both open; ES7210 unmuted with both inputs enabled.
- **Touch:** `touch ready, key off, encoder off`; CST9220 touch task started on its I2C 0x5A bus.
- **Power:** `AXP2101 reporting ready`; charging and supply settings unchanged. USB power only; no battery was connected.
- **Identity:** device ready with board tag `esp32s3-touch-amoled-2.16`, fw 0.2.0.
- **Not verified in this smoke check:** Wi-Fi connection to a real gateway, pairing and persistence, microphone capture level, speaker playback, touch coordinate accuracy and orientation, the PWR key, battery operation, volume/brightness steps, OTA, and a long session. The touch orientation was changed during review to match Waveshare's BSP and has not been checked on the board. The port stays experimental until the full [checklist](#record-a-physical-test) is completed.

## Record a physical test

Run this checklist for each board revision and release candidate. Report failed and untested steps explicitly. Do not publish Wi-Fi passwords, access tokens, device keys, or private conversation content.

1. Record the model, PCB revision, flash and PSRAM, firmware commit/version, host OS, Hermes version, power supply, and connected peripherals. Include photographs of the board label and wiring when useful.
2. Install through USB. Check the detected chip and memory, boot logs, display orientation, colors, and readable pairing code.
3. Pair with Hermes. Restart the device and confirm that its identity and pairing survive. Confirm that an unapproved device cannot issue actions.
4. Hold TALK, speak, and release. Check the microphone level, transcript, and a complete spoken reply. The serial log's `playback ended:` line after the reply gives the number and length of underruns, the times the speaker ran out of audio; include it in the report. Cancel a recording and interrupt a playing reply. Verify every physical button and touch gesture.
5. Test volume and brightness where available. Run the device's hardware checks when its firmware provides them. Record speaker distortion, missing audio, display corruption, or unexpected resets.
6. Disconnect and restore Wi-Fi and the gateway. Confirm recovery without resetting the device identity. Test phone setup with valid and wrong passwords, cancellation, its ten-minute expiry, and recovery to the previous network. Confirm the temporary page is unreachable from the station address. Check USB setup after phone setup closes.
7. On battery-capable ports, record charging, voltage/percentage, dimming, sleep, wake, and power-button results. Mark unsupported power functions as not implemented. Check USB and battery operation separately.
8. Install an OTA update for this exact board. Confirm that settings survive and the new version reaches Hermes. On a recoverable test unit, verify rollback with a candidate that cannot reach the gateway. Keep a USB recovery path ready.
9. Run a two-hour session with repeated voice turns and reconnects. Record resets, audio failures, and memory trends rather than only the final state.
10. Attach a sanitized `hermes-gadget diag --port PORT` report and the relevant logs to the pull request or issue.

Use this report format:

```text
Model / PCB revision:
Firmware version / commit:
Hermes version / host OS:
Flash / PSRAM:
Power supply / battery / peripherals:
USB install and recovery:
Pairing and persistence:
Display / buttons / touch:
Microphone / speaker / interruption:
Network loss and recovery:
Power and battery:
OTA / rollback:
Two-hour session:
Failed or untested steps:
Sanitized logs and diagnostics:
```

## Bluetooth

The [phone app link](protocol.md#phone-app-over-bluetooth) is off on every board. Turning it on adds NimBLE, ESP-IDF's Bluetooth LE stack, to the image: about 204 KB of flash on the AMOLED-1.75C (measured: 1,335,152 → 1,543,968 bytes) plus its RAM once it runs. Bluetooth also shares the antenna with Wi-Fi. So each board turns it on only after someone has checked it on that board.

To try it on a board, build it yourself with two changes, then flash over USB:

1. In `firmware/esp32/main/board.cpp`, set `b.bluetooth = true;` in the board's section.
2. Append these lines to `firmware/esp32/boards/<board>/sdkconfig.defaults`:

   ```text
   # Bluetooth phone link (docs/hardware-validation.md#bluetooth).
   CONFIG_BT_ENABLED=y
   # CONFIG_BT_BLUEDROID_ENABLED is not set
   CONFIG_BT_NIMBLE_ENABLED=y
   # CONFIG_BT_NIMBLE_ROLE_CENTRAL is not set
   # CONFIG_BT_NIMBLE_ROLE_OBSERVER is not set
   CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
   CONFIG_BT_NIMBLE_NVS_PERSIST=y
   ```

Delete the generated `firmware/esp32/sdkconfig.<board>` before building, or the configuration check stops the build. A board with the flag but without these lines logs `this board enables Bluetooth, but the build has no NimBLE` and runs without the link.

With no app yet, use a generic Bluetooth LE app such as nRF Connect or LightBlue. Check:

1. **Boot:** the log shows `hg.phone: Bluetooth ready`. Record `diag` free memory with Bluetooth on and off.
2. **Silence:** with no phone paired, the phone's scanner doesn't see the device.
3. **Pairing:** **Phone app** in device settings shows the pairing screen and the device appears in the scanner. Writing to the command characteristic makes the phone ask for a code; the screen shows six digits; typing them pairs. A wrong code fails, and so does pairing after the two minutes are up or after CANCEL.
4. **Commands:** subscribe to the reply characteristic, write `status`, `get name`, `set volume 40` and `diag log`. Each reply arrives complete and ends with a `0x00` byte, including the long `diag log`.
5. **Wi-Fi setup:** `set wifi_ssid` and `set wifi_pass` from the phone join a network.
6. **Reconnect:** after a restart, the paired phone reconnects and sends commands without pairing again.
7. **Refusal:** a second phone that hasn't paired can't write commands, and can't pair while pairing is closed.
8. **Forget:** `phone forget` drops the connected phone and the device stops advertising; `factory-reset` removes paired phones too.
9. **Coexistence:** with a phone connected, a voice turn and its spoken reply play without new dropouts, and an OTA update still installs.
10. **Long run:** a phone connected for an hour of normal use, with no resets and stable free memory.

Use this report format:

```text
Model / PCB revision:
Firmware commit (with the two changes above):
Phone model / OS / app used:
Image size and free memory, Bluetooth on and off:
Silence, pairing, wrong code, closed window:
Commands and long replies:
Wi-Fi setup over Bluetooth:
Reconnect after restart:
Refusal of an unpaired phone:
Forget / factory reset:
Voice and OTA with a phone connected:
One-hour run:
Failed or untested steps:
```

Attach the report to a pull request that makes the two changes for that board and adds Bluetooth to its row in the table above. The board's next release then ships with the link. A report for one board doesn't turn it on for another.

## Support labels

- **Experimental:** the port builds and passes automated checks, but no complete physical report is linked for that configuration.
- **Hardware verified:** a linked report identifies the exact revision and tested firmware, covers the checklist, and states any limitations.

A report for one model or revision does not verify another. Keep earlier reports when adding a new one so users can find the firmware and hardware combination they own. Update this page in the same pull request that adds a port or changes its verified capabilities.
