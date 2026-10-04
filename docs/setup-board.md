# Set up a board

Install prebuilt firmware, connect Wi-Fi, and pair the board with your Hermes. The computer that flashes the board does not need Python or a compiler.

## 1. Check your board and cable

Have these ready:

- Chrome or Edge on a computer.
- A USB cable that carries data.
- A 2.4 GHz Wi-Fi network and its password.
- Your Hermes Agent computer, with its gateway running.

| Supported board | Controls and audio |
|---|---|
| [Waveshare ESP32-S3-LCD-1.54](hardware.md#waveshare-esp32-s3-lcd-154) | BOOT to talk, PLUS to cancel; onboard microphones and speaker |
| [Waveshare ESP32-S3-Touch-AMOLED-1.75](hardware.md#esp32-s3-touch-amoled-175) | Hold the screen to talk, swipe down to cancel; microphones and a speaker output |
| [ESP32-S3 breadboard build](hardware.md) | Wire the display, microphone, buttons, and optional speaker first |

Match the exact model printed on the board. For another model, read [Add a board](porting.md).

## 2. Prepare Hermes

Follow [Connect Hermes](connect-hermes.md#1-install-the-plugin) to install and enable the Gadget plugin. Keep the device URL from `hermes gadget info` ready. Its installer link fills the address in for you.

## 3. Install and connect

Open the [browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html). Plug in the board and select its model. The next step shows the Hermes commands. If you completed them above, choose **Hermes is ready**.

1. Install the firmware. Keep the cable connected until the installer says it has finished.
2. Enter the Wi-Fi network, password, Hermes address, and device name.
3. Save the settings and watch the connection checks.
4. When a code appears, run `hermes gadget pair` on your Hermes computer and approve that device.

**You know it worked when:** Wi-Fi, Hermes, and pairing checks complete, and the device shows Ready. Hold TALK or the touchscreen, speak, and release. Speech needs the [Hermes audio configuration](connect-hermes.md#4-enable-speech).

## Manage an existing gadget

To change Wi-Fi or the Hermes address, connect USB and use the installer's **skip to Wi-Fi** option. Its firmware reinstall option can keep the existing settings.

To update firmware over the air, run this on the Hermes computer, replacing the name with your device's name or ID:

```bash
hermes gadget devices
hermes gadget update "Kitchen" --latest
```

For a custom build, follow [Build and flash](hardware.md#build-and-flash). For USB configuration from the SDK checkout, install the serial extra with `python -m pip install -e ".[serial]"` and use `hermes-gadget provision --help`.

## Get help

Open **Troubleshooting** in the browser installer. Under **Something else**, choose **Save a diagnostics report**. Attach the file to a [bug report](https://github.com/Adolanium/hermes-gadget-sdk/issues).

Next: [Talk, type, and interrupt](using-gadget.md) or [troubleshooting](troubleshooting.md).
