# Troubleshooting

## Simulator build or window

| Symptom | What to check |
|---|---|
| "Simulator library not found" | Run `hermes-gadget build-sim --test` from the checkout. The default library is in `build/host/`; `HGSIM_LIBRARY` overrides it |
| No compiler found | Install the compiler for your platform in [Try the simulator](desktop.md#1-check-your-tools) |
| No Tkinter or no window | Run `python -m tkinter`. Install your Python distribution's Tk support. Headless scripts use `--headless` |
| PowerShell refuses activation | Use `.\.venv\Scripts\hermes-gadget.exe` directly, as described in the [Windows instructions](desktop.md#windows) |

## Connection and pairing

| Symptom | What to check |
|---|---|
| Device stays on Connecting | Use the URL from `hermes gadget info`. Keep the gateway running and allow its port through the host firewall. A board cannot use `127.0.0.1` to reach another computer |
| Board cannot join Wi-Fi | Check the password and use a 2.4 GHz network |
| "No pairing code yet" | An allowlist may reject unknown senders. Keep `platforms.gadget.extra.unauthorized_dm_behavior: pair`, or add the device ID to `GADGET_ALLOWED_USERS`. Check `hermes pairing list` |
| "Device key does not match" | After a factory reset, run `hermes gadget forget <device_id>` on the Hermes computer, then pair again |
| Browser cannot find a board | Use Chrome or Edge on a computer and a USB data cable. If there are two USB ports, use UART for settings. Close other serial monitors |

## Speech and replies

| Symptom | What to check |
|---|---|
| Demo only echoes text | That is the scripted demo. [Connect Hermes](connect-hermes.md) for agent replies |
| Simulator records silence | Install the audio extra and enable live audio. Check the system microphone permissions and default input |
| Voice is not transcribed | Configure speech recognition on the Hermes computer |
| Replies are text only | Configure TTS, check that `speak_replies` is not `false`, and enable simulator live audio. Non-WAV TTS needs `ffmpeg` on the Hermes computer |

## Save a board report

The [browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html) can save a report under **Troubleshooting**, **Something else**, **Save a diagnostics report**. It includes the board status and recent log.

If you already use the SDK's serial tools, run `hermes-gadget diag --port COM5`, replacing `COM5` with your board's port. Attach the saved file and the steps to reproduce the problem to a [bug report](https://github.com/Adolanium/hermes-gadget-sdk/issues).
