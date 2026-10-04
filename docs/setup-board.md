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
| [Waveshare ESP32-S3-Touch-AMOLED-1.75C](hardware.md#esp32-s3-touch-amoled-175c) | Hold the screen to talk, swipe down to cancel; onboard microphones and speaker; experimental |
| [Espressif ESP32-S3-BOX-3](hardware.md#esp32-s3-box-3) | Hold the screen or BOOT to talk, swipe down to cancel; onboard microphones and speaker; experimental |
| [M5Stack CoreS3](hardware.md#m5stack-cores3) | Hold the screen to talk, swipe down to cancel; onboard audio and battery management; experimental |
| [ESP32-S3 breadboard build](hardware.md) | Wire the display, microphone, buttons, and optional speaker first |

Match the exact model printed on the board. For another model, read [Add a board](porting.md).

Read the [capability and verification table](hardware-validation.md). A firmware build does not replace testing on your board revision.

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

To change Wi-Fi or the Hermes address, use [phone setup](#set-up-wi-fi-with-your-phone) or connect USB and use the installer's **skip to Wi-Fi** option. Its firmware reinstall option can keep the existing settings.

To update firmware over the air, run this on the Hermes computer, replacing the name with your device's name or ID:

```bash
hermes gadget devices
hermes gadget update "Kitchen" --latest
```

For a custom build, follow [Build and flash](hardware.md#build-and-flash). For USB configuration from the SDK checkout, install the serial extra with `python -m pip install -e ".[serial]"` and use `hermes-gadget provision --help`.

## Set up Wi-Fi with your phone

Phone setup configures a board that already runs the firmware. A board without a saved Wi-Fi network opens setup automatically at startup. To change an existing connection, open [device settings](using-gadget.md#device-settings-and-hardware-checks) and select **Wi-Fi setup**.

1. Join the `Hermes-XXXX` network shown on the gadget. Enter its temporary password, also shown on the screen. Each setup session gets a new password.
2. Keep this network selected if your phone warns that it has no internet. Open `http://192.168.4.1` in the phone's browser.
3. Enter your 2.4 GHz network name, its password, and the device URL from `hermes gadget info`. Leave the password empty only for an open network.
4. Choose **Check connection and save**. The board allows up to 30 seconds to connect. If your phone disconnects during the check, rejoin the gadget's network and reload the page.
5. After the page reports success, reconnect your phone to its usual network. The gadget connects to Hermes; approve its pairing code if this is its first setup.

The board saves the new Wi-Fi credentials only after it receives an IP address on that network. A failed attempt restores the previous settings and lets you try again. This check confirms Wi-Fi connectivity; Hermes must also be running at the address you entered. Pairing and device identity stay in place.

The temporary network closes ten seconds after success, after ten minutes, or when you press CANCEL or swipe down on the gadget. If the new network overlaps the setup subnet, setup closes immediately after saving and the phone may not show a success message. Setup uses a local page and needs no phone app. Saved Wi-Fi passwords are never sent back to the page. The temporary password is absent from diagnostics and is discarded when setup closes.

USB remains available if phone setup cannot connect. The console accepts `wifi-setup` to start it and show its temporary credentials, or `wifi-setup close` to cancel it. Use USB for networks that overlap the setup subnet `192.168.4.0/24` and for gateway access-token changes. Firmware supports open networks and WPA2-compatible personal networks; enterprise Wi-Fi is not supported. Raspberry Pi networking is configured in Raspberry Pi OS, not through this page.

## Get help

Open **Troubleshooting** in the browser installer. Under **Something else**, choose **Save a diagnostics report**. Attach the file to a [bug report](https://github.com/Adolanium/hermes-gadget-sdk/issues).

Next: [Talk, type, and interrupt](using-gadget.md) or [troubleshooting](troubleshooting.md).
