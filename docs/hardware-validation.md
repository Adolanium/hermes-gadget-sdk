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
| Waveshare ESP32-S3-Touch-AMOLED-1.75C, SKUs 33691/33692 | CO5300 466×466; CST9217 touch and BOOT | ES7210 microphones and ES8311 onboard speaker | AXP2101 readings, audio supply and local power-off; screen timeout | Experimental; physical report not recorded |
| Espressif ESP32-S3-BOX-3 | ST7789/TT21100 or ILI9342/GT911, detected through I2C; BOOT and touch gestures | ES7210 microphones and ES8311 onboard speaker | USB power; screen timeout | Experimental; physical report not recorded for either panel revision |
| M5Stack CoreS3 (K128) | ILI9342C/E 320×240, detected through touch firmware; FT6336 gestures | ES7210 microphones and AW88298 speaker | AXP2101 readings, backlight and local power-off; AW9523 reset/boost control | Experimental; physical report not recorded for either panel revision |
| LilyGO T-Display-S3 (exact board revision unknown) | ST7789 320×170 over 8-bit i80; BOOT and Button2 | No onboard audio | Battery divider; no fuel gauge; GPIO15 powers the panel rail, not device shutdown | Experimental; physical smoke check only (screen ready/replies, Wi-Fi, online pairing); exact revision unknown; full physical checklist not completed; hardware-verified status not claimed |

The LCD-1.54 `-EN` SKU uses the same hardware. The separate Touch-LCD-1.54 model adds a CST816 touchscreen that this port does not drive. AMOLED-1.75C has its own firmware profile; its reset and audio clock pins differ from the 1.75 model. See [hardware and wiring](hardware.md) for connections and exact model names.

CI builds and packages these profiles. The browser installer lists profiles included in the latest published release, so newly merged profiles may require a source build until the next release. Other chips, wiring, and unlisted hardware revisions are porting targets, not verified configurations.

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
