# Changelog

## 0.2.0

### Hardware and setup

- Add firmware profiles for Waveshare AMOLED-1.75C, Espressif ESP32-S3-BOX-3, M5Stack CoreS3, and LilyGO T-Display-S3. The release now covers seven ESP32-S3 profiles.
- Add phone-based Wi-Fi setup and recovery, including the dual-stack socket fix that allows IPv4 setup clients.
- Add on-device settings, hardware checks, and board-specific battery, brightness, and power controls.
- Provide Linux ARM64 packages for the experimental Raspberry Pi 4/5 client, including a service installer, updates, optional USB audio, GPIO controls, and display support.

### Tools and documentation

- Add a face generator and interactive feature picker. Copied commands preserve the image path, crop, rendering settings, and selected feature positions.
- Update the website, guided browser installer, and simulator conversation view. Publish setup, troubleshooting, hardware, and development guides with search.
- Add Home Assistant and MQTT examples and a Tailscale Funnel remote-access guide.
- Bundle third-party licenses and notices with firmware and Linux packages, with a license archive link in the installer.
- Add contribution requirements and automated documentation checks. CI discovers firmware profiles directly from PlatformIO.

### Upgrade and verification

Use the [browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html) for USB installation. Existing devices can use `hermes gadget update <device> --latest`. Install the matching plugin using the commit-pinned command in the release notes. See [Linux installation and updates](docs/linux.md) for Raspberry Pi packages.

Hardware ports remain experimental until a complete physical report is recorded for the exact revision. The T-Display-S3 has a contributor-reported smoke check, but no complete verification report. Software tests and successful builds do not establish physical hardware verification; see [hardware capabilities and verification](docs/hardware-validation.md).

### Contributors

Thanks to [Angel Moreno (@Angel-M-R)](https://github.com/Angel-M-R) for the phone setup fix, [@rushter777](https://github.com/rushter777) for the face generator and picker, [Richard Ahlquist (@rahlquist)](https://github.com/rahlquist) for the T-Display-S3 port, and [Don F. Irwin (@0xdfi)](https://github.com/0xdfi) for the Funnel guide.

Earlier release notes: [0.1.1](https://github.com/Adolanium/hermes-gadget-sdk/releases/tag/v0.1.1) and [0.1.0](https://github.com/Adolanium/hermes-gadget-sdk/releases/tag/v0.1.0).
