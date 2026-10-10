# Hardware capabilities and verification

The Raspberry Pi 4/5 Linux port is experimental. ARM64 CI covers the native
core, service, package installation and updates. USB audio, GPIO, and display
tests use software drivers or test doubles. No physical Pi report is recorded.

Firmware builds and simulator tests check software behavior. A physical verification report records what worked on a particular board revision, wiring, and firmware commit. A passing build alone does not establish that a microphone, power circuit, or display works on a device.

## Current hardware

| Model | Display and input | Audio | Power support | Verification |
|---|---|---|---|---|
| ESP32-S3-DevKitC-1 N8R8 breadboard | Wired ST7789, TALK and CANCEL buttons | Wired I2S microphone; optional MAX98357A speaker | External power | CI build; physical report not recorded |
| Waveshare ESP32-S3-LCD-1.54, SKUs 33866/33867 | ST7789 240×240; BOOT and PLUS | ES7210 microphones and ES8311 speaker | Calibrated voltage, charging signal, battery latch and screen timeout; USB bypasses shutdown | CI build; physical report not recorded |
| Waveshare ESP32-S3-Touch-AMOLED-1.75 | CO5300 466×466; CST9217 touch, BOOT and PWR | ES7210 microphones and ES8311 speaker output | AXP2101 readings and local power-off; optional screen timeout | CI build; physical report not recorded |
| Waveshare ESP32-S3-Touch-AMOLED-1.75C, SKUs 33691/33692 | CO5300 466×466; CST9217 touch and BOOT | ES7210 microphones and ES8311 onboard speaker | AXP2101 readings, audio supply and local power-off; screen timeout | Experimental; [SKU 33691 report](#waveshare-amoled-175c-physical-report) covers the checklist on a working branch, not a `main` build; PCB revision not marked |
| Waveshare ESP32-S3-Touch-AMOLED-1.8, V2 only | CO5300 368×448; CST820 touch and BOOT | ES8311 analog microphone and speaker output | AXP2101 readings and local power-off; display/touch reset through a TCA9554 expander | Experimental; physical smoke check only (boot, display, I2C devices present, codecs and the touch, speaker and microphone tasks start); touch, microphone capture, speaker playback and battery not verified; full checklist not completed |
| Waveshare ESP32-S3-Touch-LCD-1.85C V2 / PCB Rev2.0 only | ST77916 360×360 round QSPI LCD; CST816 touch and BOOT | ES8311 + ES7210 dual analog mic slots, NS4150B PA; mono transport; no software AEC | USB/battery switch; screen timeout; no battery telemetry or software shutdown | Experimental; [partial Rev2.0 report](#waveshare-185c-v2-partial-physical-report); full checklist not completed |
| Xorigin AIPI Lite | ST7789 128×128; BOOT and power keys | One ES8311 for speaker and microphone | GPIO 10 power latch and power-off; battery level and charging not reported | Experimental; contributor smoke test on an earlier revision (boot, display and audio start, online pairing); microphone capture, colors, USB console and battery not verified on this revision |
| Espressif ESP32-S3-BOX-3 | ST7789/TT21100 or ILI9342/GT911, detected through I2C; BOOT and touch gestures | ES7210 microphones and ES8311 onboard speaker | USB power; screen timeout | Experimental; physical report not recorded for either panel revision |
| M5Stack CoreS3 (K128) | ILI9342C/E 320×240, detected through touch firmware; FT6336 gestures | ES7210 microphones and AW88298 speaker | AXP2101 readings, backlight and local power-off; AW9523 reset/boost control | Experimental; physical report not recorded for either panel revision |
| LilyGO T-Display-S3 (SKU/Version H587; PCB revision 1.2) | ST7789 320×170 over 8-bit i80; BOOT and Button2 | No onboard audio | Battery divider; no fuel gauge; GPIO15 powers the panel rail, not device shutdown | Experimental; physical smoke check only (screen ready/replies, Wi-Fi, online pairing); full physical checklist not completed; hardware-verified status not claimed |
| Elecrow CrowPanel 2.1-inch HMI (ESP32-S3R8, 16 MB flash) | ST7701 480×480 RGB round panel; CST-family touch, rotary encoder, encoder button on PCF8574 | No onboard audio | USB power; screen timeout | Experimental; physical smoke check only (panel bring-up, brightness scale, Wi-Fi, online pairing, serial console); touch coordinates and encoder direction not physically verified |
| Elecrow CrowPanel 5.0-inch HMI (DIS07050H, ESP32-S3R8, 4 MB flash) | ILI6122/ILI5960 800×480 RGB panel; GT911 touch on I2C, INT not wired (polled) | No onboard audio | USB power; screen timeout | Experimental; boot smoke check only (panel initialization log, GT911 ID, Wi-Fi, online pairing); rendering, colours, brightness and touch coordinates not physically verified |

The LCD-1.54 `-EN` SKU uses the same hardware. The separate Touch-LCD-1.54 model adds a CST816 touchscreen that this port does not drive. AMOLED-1.75C has its own firmware profile; its reset and audio clock pins differ from the 1.75 model. See [hardware and wiring](hardware.md) for connections and exact model names.

CI builds and packages these profiles. The browser installer lists profiles included in the latest published release, so newly merged profiles may require a source build until the next release. Other chips, wiring, and unlisted hardware revisions are porting targets, not verified configurations.

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

## Record a physical test

Run this checklist for each board revision and release candidate. Report failed and untested steps explicitly. Do not publish Wi-Fi passwords, access tokens, device keys, or private conversation content.

1. Record the model, PCB revision, flash and PSRAM, firmware commit/version, host OS, Hermes version, power supply, and connected peripherals. Include photographs of the board label and wiring when useful.
2. Install through USB. Check the detected chip and memory, boot logs, display orientation, colors, and readable pairing code.
3. Pair with Hermes. Restart the device and confirm that its identity and pairing survive. Confirm that an unapproved device cannot issue actions.
4. Hold TALK, speak, and release. Check the microphone level, transcript, and a complete spoken reply. Cancel a recording and interrupt a playing reply. Verify every physical button and touch gesture.
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

## Support labels

- **Experimental:** the port builds and passes automated checks, but no complete physical report is linked for that configuration.
- **Hardware verified:** a linked report identifies the exact revision and tested firmware, covers the checklist, and states any limitations.

A report for one model or revision does not verify another. Keep earlier reports when adding a new one so users can find the firmware and hardware combination they own. Update this page in the same pull request that adds a port or changes its verified capabilities.
