<p align="center">
  <img src="docs/images/logo.png" width="64" alt="Hermes Gadget logo">
</p>

<h1 align="center">Hermes Gadget</h1>

<p align="center">
  <b>Hold a button. Ask Hermes. Hear the answer.</b><br>
  A small device for your own <a href="https://github.com/NousResearch/hermes-agent">Hermes Agent</a>, with its tools, memory, and skills.
</p>

<p align="center">
  <a href="https://adolanium.github.io/hermes-gadget-sdk/installer.html"><b>Set up a board</b></a> ·
  <a href="docs/desktop.md"><b>Try the simulator</b></a> ·
  <a href="docs/getting-started.md">Read the docs</a>
</p>

<p align="center">
  <img src="docs/images/screen-ready.png" width="226" alt="The round simulator display, ready for a question">
  <br><sub>The desktop simulator runs the same device core. No board needed to try it.</sub>
</p>

## Choose your starting point

| I have a board | I want to try it first | I want to build with it |
|---|---|---|
| [Open the browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html) | [Start the desktop simulator](docs/desktop.md) | [Explore the SDK](docs/development.md) |
| Install over USB, connect Wi-Fi, and pair with your Hermes. | Try a scripted demo, then connect your own Hermes. | Add a board, device actions, sensors, or your own face. |
| Chrome or Edge, a USB data cable, 2.4 GHz Wi-Fi, and Hermes. No firmware toolchain. | Python 3.10+, CMake 3.16+, and a C++17 compiler. | Start with the development guide and tests. |

## What you can do

- **Talk and listen.** Hold TALK to speak, release to send, and hear your Hermes reply.
- **See what is happening.** Read replies, answer confirmation questions, and receive cards from your agent.
- **Let Hermes act.** Expose the gadget's LEDs, sensors, and other controls as agent tools.

Your Hermes does the thinking. Real conversations need Hermes Agent and the Gadget plugin. Voice also needs speech recognition and text-to-speech configured. The demo server gives scripted replies. Follow [Connect Hermes](docs/connect-hermes.md) when you are ready.

## Supported hardware

| Board | How you talk | Audio |
|---|---|---|
| [Waveshare ESP32-S3-LCD-1.54](docs/hardware.md#waveshare-esp32-s3-lcd-154) | Hold BOOT | Onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75](docs/hardware.md#esp32-s3-touch-amoled-175) | Hold the screen | Onboard microphones; speaker output |
| [ESP32-S3 breadboard build](docs/hardware.md) | Hold TALK | Wire the microphone and optional speaker |

Check the exact model and connections in the [hardware guide](docs/hardware.md). Other boards need a [port](docs/porting.md).

Raspberry Pi 4 and 5 have an experimental [Linux client](docs/linux.md) for
64-bit Raspberry Pi OS Lite Trixie. Add USB audio, GPIO controls, or a display
as needed. ARM64 release packages include the native core and a service installer.

These ports build in CI. Physical verification reports are not yet recorded; treat them as experimental until the [hardware verification table](docs/hardware-validation.md) links a report for your revision.

## Try the demo from a checkout

<details>
<summary>Already have Python, CMake, and a compiler? Start here.</summary>

In your activated environment at the repository root:

```bash
python -m pip install -e ".[dev]"
hermes-gadget build-sim --test
hermes-gadget devserver --pairing
```

In a second terminal with the same environment activated:

```bash
hermes-gadget sim --url ws://127.0.0.1:8765/gadget --board sim-466x466-round
```

Type `approve <CODE>` in the first terminal, using the code on the device. Type a message in the simulator to receive a streamed echo. The [desktop guide](docs/desktop.md) covers platform-specific prerequisites and live audio.

</details>

## On the screen

| Ready | Listening | Thinking | Speaking |
|:---:|:---:|:---:|:---:|
| ![Ready](docs/images/screen-ready.png) | ![Listening](docs/images/screen-listening.png) | ![Thinking](docs/images/screen-thinking.png) | ![Speaking](docs/images/screen-speaking.png) |

These images show the simulator's device display. The [simulator guide](docs/simulator.md) covers its desktop controls, board profiles, and scripted runs.

## Find the right guide

| Use a gadget | Build with the SDK |
|---|---|
| [Choose a starting point](docs/getting-started.md) | [Development and tests](docs/development.md) |
| [Set up a board](docs/setup-board.md) | [Add a board, actions, or sensors](docs/porting.md) |
| [Try the simulator](docs/desktop.md) | [Customize the face](docs/faces.md) |
| [Run a Linux gadget](docs/linux.md) | [Hardware verification](docs/hardware-validation.md) |
| [Connect Hermes](docs/connect-hermes.md) | [Architecture](docs/architecture.md) |
| [Talk, type, and interrupt](docs/using-gadget.md) | [Protocol](docs/protocol.md) |
| [Change Wi-Fi or update firmware](docs/setup-board.md#manage-an-existing-gadget) | [Hermes integration reference](docs/hermes-integration.md) |
| [Troubleshooting](docs/troubleshooting.md) | [Hardware and wiring](docs/hardware.md) |

## Project status

[Releases](https://github.com/Adolanium/hermes-gadget-sdk/releases/latest) include prebuilt firmware for the three boards above. After the first USB flash, `hermes gadget update` installs new firmware over the air. A build that cannot reach Hermes rolls itself back.

The simulator and firmware share a portable C++17 core. CI tests the core, Python tools, installer, and plugin against a real Hermes gateway, and builds every supported board. The [development guide](docs/development.md) describes the test suites and pinned Hermes version.

After installing firmware, you can [configure Wi-Fi from your phone](docs/setup-board.md#set-up-wi-fi-with-your-phone) through the gadget's temporary network. USB setup remains available. Wake-word activation is not included.

[![CI](https://github.com/Adolanium/hermes-gadget-sdk/actions/workflows/ci.yml/badge.svg)](https://github.com/Adolanium/hermes-gadget-sdk/actions/workflows/ci.yml)
[![Hermes integration](https://github.com/Adolanium/hermes-gadget-sdk/actions/workflows/hermes.yml/badge.svg)](https://github.com/Adolanium/hermes-gadget-sdk/actions/workflows/hermes.yml)

## Contributing

Bug reports from real boards, new boards, fixes, and docs are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md). Report security problems privately through [SECURITY.md](SECURITY.md).

## Affiliation and trademarks

Hermes Gadget is an **independent, community-made project**. It is not affiliated with, endorsed by, sponsored by or supported by Nous Research.

"Hermes", "Hermes Agent", "Nous Research" and the Hermes Agent mascot (the girl with the headphones, sometimes called "Nous Girl") are trademarks or brand assets of Nous Research. They appear here only to describe compatibility with Hermes Agent.

## License

The MIT license ([LICENSE](LICENSE)) covers only the code and documentation written for this project. It grants no rights to Nous Research's names or marks.

Everything else keeps its own license. None of it is copied into this repository; it is downloaded when you build or install:

| Third-party code | Used for | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | The ESP32 framework and drivers | Apache 2.0 |
| [esp_codec_dev](https://components.espressif.com/components/espressif/esp_codec_dev) | ES8311 / ES7210 audio codecs | Apache 2.0 |
| [esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client) | The device's WebSocket connection | Apache 2.0 |
| [esptool-js](https://github.com/espressif/esptool-js) | Flashing from the browser installer, added to the site when it's built | Apache 2.0 |
| Python packages (`websockets`, and optionally `Pillow`, `sounddevice`, `pyserial`) | Plugin, simulator and tools | Their own licenses; see each project |
| [GPIO Zero](https://github.com/gpiozero/gpiozero/blob/master/LICENSE.rst) | Optional Raspberry Pi buttons and digital outputs | BSD 3-Clause |
| [pygame](https://github.com/pygame/pygame/blob/main/docs/LGPL.txt), [SDL2](https://github.com/libsdl-org/SDL/blob/SDL2/LICENSE.txt) | Optional Linux device display | LGPL 2.1; zlib for SDL2 |
| [lgpio](https://github.com/joan2937/lg/blob/master/UNLICENCE) | Linux GPIO access, installed from Raspberry Pi OS | Unlicense |
| [python-sounddevice](https://github.com/spatialaudio/python-sounddevice/blob/master/LICENSE), [PortAudio](https://www.portaudio.com/license.html) | Optional live microphone and speaker on Linux | MIT licenses |

The mascot artwork, and the logo and device bitmaps drawn from it, come from [Hermes Agent](https://github.com/NousResearch/hermes-agent) (MIT, © 2025 Nous Research); see [assets/mascot](assets/mascot) and [NOTICE](NOTICE).

The AMOLED panel's start-up register values in `firmware/esp32/main/port_amoled.cpp` follow Waveshare's [board support package](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_75) for the ESP32-S3-Touch-AMOLED-1.75 (Apache 2.0, text in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)); see [NOTICE](NOTICE).
