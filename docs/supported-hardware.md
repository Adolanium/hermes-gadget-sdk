# Supported hardware

Compare the available hardware profiles below. To try Hermes Gadget without a board, start with the [desktop simulator](desktop.md).

Hardware ports remain experimental until the [verification table](hardware-validation.md) links a complete physical report for the exact model and revision. CI builds and simulator tests check software behavior; they do not verify physical hardware.

## ESP32-S3 boards

| Board | Controls | Audio |
|---|---|---|
| [Waveshare ESP32-S3-LCD-1.54](hardware.md#waveshare-esp32-s3-lcd-154) | Hold BOOT to talk | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75](hardware.md#esp32-s3-touch-amoled-175) | Hold the screen to talk; lay it face down to sleep (off by default) | Onboard microphones; speaker output |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75C](hardware.md#esp32-s3-touch-amoled-175c) | Hold the screen to talk; PWR turns the screen off and on | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-1.8 (V2)](hardware.md#esp32-s3-touch-amoled-18) | Hold the screen or BOOT to talk; PWR cancels | Onboard analog microphone; speaker output |
| [Waveshare ESP32-S3-Touch-AMOLED-2.06 (watch)](hardware.md#esp32-s3-touch-amoled-206) | Hold the screen or BOOT to talk; PWR turns the screen off and on | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-2.16](hardware.md#esp32-s3-touch-amoled-216) | Hold the screen or BOOT to talk; PWR turns the screen off and on | Onboard microphones; speaker output |
| [Waveshare ESP32-S3-Touch-LCD-1.85C V2](hardware.md#waveshare-esp32-s3-touch-lcd-185c-v2) | Hold the screen or BOOT to talk | Onboard microphones; speaker output, with a speaker in the speaker-box version |
| [Espressif ESP-VoCat v1.2](hardware.md#esp-vocat) | Hold the screen or BOOT to talk; swipe down to cancel | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](hardware.md#waveshare-esp32-s3-knob-touch-lcd-18) | Hold the screen to talk; the knob scrolls; BOOT also works as TALK | Onboard PDM microphone; **no speaker output** (the port doesn't drive the headphone DAC; replies are text only) |
| [Xorigin AIPI Lite](hardware.md#xorigin-aipi-lite) | Hold BOOT to talk; the power key cancels | Onboard microphone and speaker |
| [Spotpear ESP32-S3-1.28-BOX](hardware.md#spotpear-esp32-s3-128-box) | Hold the screen or BOOT to talk; swipe down to cancel | Onboard microphone and speaker; battery |
| [Spotpear ESP32-S3-1.54 muma](hardware.md#spotpear-esp32-s3-154-muma) | Hold the screen or BOOT to talk; swipe down to cancel | Onboard microphone and speaker |
| [Espressif ESP32-S3-BOX-3](hardware.md#esp32-s3-box-3) | Hold the screen or BOOT to talk | Onboard microphones and speaker |
| [M5Stack CoreS3](hardware.md#m5stack-cores3) | Hold the screen to talk | Onboard microphones and speaker; battery management |
| [LilyGO T-Display-S3](hardware.md#lilygo-t-display-s3) | BOOT and Button2; send text through the USB console | No onboard microphone or speaker |
| [Elecrow CrowPanel 2.1-inch HMI](hardware.md#elecrow-crowpanel-21-inch-hmi) | Touch screen and rotary knob; send text through the USB console | No onboard microphone or speaker |
| [Elecrow CrowPanel 5.0-inch HMI](hardware.md#elecrow-crowpanel-50-inch-hmi) | Touch screen; send text through the USB console | No onboard microphone or speaker |
| [ESP32-S3 breadboard build](hardware.md#esp32-s3-breadboard) | Hold the wired TALK button | Wire the microphone and optional speaker |

## Classic ESP32 boards

| Board | Controls | Audio |
|---|---|---|
| [ESP32-2432S028 CYD (Cheap Yellow Display)](hardware.md#esp32-2432s028-cyd-cheap-yellow-display), two-USB ST7789 revision | BOOT to talk; send text through the USB console | No microphone fitted; a PDM MEMS part on the 4-pin header (PDM clock GPIO 22, data GPIO 27) is driven by the profile. The SC8002B amplifier behind the speaker header plays three boot beeps. The touchscreen is not used |

## ESP32-P4 boards

| Board | Controls | Audio |
|---|---|---|
| [Elecrow CrowPanel Advanced 10.1-inch ESP32-P4](hardware.md#elecrow-crowpanel-advanced-101-inch-esp32-p4) | Hold the touch screen to talk; BOOT is configured as TALK | Onboard PDM microphone and I²S speaker output; STC8 battery/charger reporting |
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C](hardware.md#waveshare-esp32-p4-wifi6-touch-lcd-34c) | Hold the touch screen to talk; BOOT is TALK. Touch and audio aren't verified on the board yet | ES7210 microphones and ES8311 speaker; USB power, no battery. Source build only; the C6 needs its own firmware first |

Check the exact model and connections in [Hardware and wiring](hardware.md), then follow [Set up a board](setup-board.md). The browser installer lists profiles included in the latest published release. Newly merged profiles may need a [source build](development.md) until the next release.

Voice needs [speech recognition and text-to-speech configured in Hermes](connect-hermes.md#4-enable-speech). Boards without onboard audio can display replies and accept text through the USB console.

## Raspberry Pi

Raspberry Pi 4 and 5 have an experimental [Linux client](linux.md) for 64-bit Raspberry Pi OS Lite Trixie. Add USB audio, GPIO controls, or a display as needed. ARM64 release packages include the native core and a service installer.

See the [Linux guide](linux.md) for installation and peripherals, and the [verification table](hardware-validation.md) for testing status.

## Other hardware

Other boards need a [port](porting.md). Check the existing wiring and driver support before choosing hardware with a different display, audio codec, or power controller.
