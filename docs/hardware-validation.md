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

The LCD-1.54 `-EN` SKU uses the same hardware. The separate Touch-LCD-1.54 model adds a CST816 touchscreen that this port does not drive. AMOLED-1.75C is a separate model and needs its own firmware profile. See [hardware and wiring](hardware.md) for connections and exact model names.

The current release contains these three ESP32-S3 builds. Other chips, wiring, and unlisted hardware revisions are porting targets, not verified configurations.

## Record a physical test

Run this checklist for each board revision and release candidate. Report failed and untested steps explicitly. Do not publish Wi-Fi passwords, access tokens, device keys, or private conversation content.

1. Record the model, PCB revision, flash and PSRAM, firmware commit/version, host OS, Hermes version, power supply, and connected peripherals. Include photographs of the board label and wiring when useful.
2. Install through USB. Check the detected chip and memory, boot logs, display orientation, colors, and readable pairing code.
3. Pair with Hermes. Restart the device and confirm that its identity and pairing survive. Confirm that an unapproved device cannot issue actions.
4. Hold TALK, speak, and release. Check the microphone level, transcript, and a complete spoken reply. Cancel a recording and interrupt a playing reply. Verify every physical button and touch gesture.
5. Test volume and brightness where available. Run the device's hardware checks when its firmware provides them. Record speaker distortion, missing audio, display corruption, or unexpected resets.
6. Disconnect and restore Wi-Fi and the gateway. Confirm recovery without resetting the device identity. Change network settings through each setup method the port supports.
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
