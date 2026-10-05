# Supported hardware

Compare the available hardware profiles below. To try Hermes Gadget without a board, start with the [desktop simulator](desktop.md).

Hardware ports remain experimental until the [verification table](hardware-validation.md) links a complete physical report for the exact model and revision. CI builds and simulator tests check software behavior; they do not verify physical hardware.

## ESP32-S3 boards

| Board | Controls | Audio |
|---|---|---|
| [Waveshare ESP32-S3-LCD-1.54](hardware.md#waveshare-esp32-s3-lcd-154) | Hold BOOT to talk | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75](hardware.md#esp32-s3-touch-amoled-175) | Hold the screen to talk | Onboard microphones; speaker output |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75C](hardware.md#esp32-s3-touch-amoled-175c) | Hold the screen to talk | Onboard microphones and speaker |
| [Espressif ESP32-S3-BOX-3](hardware.md#esp32-s3-box-3) | Hold the screen or BOOT to talk | Onboard microphones and speaker |
| [M5Stack CoreS3](hardware.md#m5stack-cores3) | Hold the screen to talk | Onboard microphones and speaker; battery management |
| [LilyGO T-Display-S3](hardware.md#lilygo-t-display-s3) | BOOT and Button2; send text through the USB console | No onboard microphone or speaker |
| [ESP32-S3 breadboard build](hardware.md#esp32-s3-breadboard) | Hold the wired TALK button | Wire the microphone and optional speaker |

Check the exact model and connections in [Hardware and wiring](hardware.md), then follow [Set up a board](setup-board.md). The browser installer lists profiles included in the latest published release. Newly merged profiles may need a [source build](development.md) until the next release.

Voice needs [speech recognition and text-to-speech configured in Hermes](connect-hermes.md#4-enable-speech). Boards without onboard audio can display replies and accept text through the USB console.

## Raspberry Pi

Raspberry Pi 4 and 5 have an experimental [Linux client](linux.md) for 64-bit Raspberry Pi OS Lite Trixie. Add USB audio, GPIO controls, or a display as needed. ARM64 release packages include the native core and a service installer.

See the [Linux guide](linux.md) for installation and peripherals, and the [verification table](hardware-validation.md) for testing status.

## Other hardware

Other boards need a [port](porting.md). Check the existing wiring and driver support before choosing hardware with a different display, audio codec, or power controller.
