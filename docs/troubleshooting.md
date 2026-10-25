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
| `hermes plugins install` is blocked with a "dangerous" verdict | Install from the `plugin` directory: use the command in [Connect Hermes](connect-hermes.md#1-install-the-plugin). The repository root is not a plugin, and its scan includes the Raspberry Pi installer |
| Device stays on Connecting | Use the URL from `hermes gadget info`. Keep the gateway running and allow its port through the host firewall. A board cannot use `127.0.0.1` to reach another computer |
| Device stops answering after Hermes, its proxy or a tunnel restarts | Firmware 0.2.0 could stop for good when the server dropped the WebSocket; its log fills with `event queue full; dropping event 6`. Update the firmware. Newer firmware reconnects by itself, and restarts if its WebSocket send task gets stuck (`the WebSocket send task is stuck; restarting` in the log). If it still stops, [save a board report](#save-a-board-report) |
| `No matching trusted root certificate found` in the log for a `wss://` address | The server's certificate chain does not reach a root certificate the firmware trusts. List the chain with `openssl s_client -connect <host>:443 -servername <host> -showcerts`. Firmware 0.2.0 rejected Cloudflare tunnels (`*.trycloudflare.com`); update the firmware. See [remote access](tailscale-funnel.md#security-and-troubleshooting) |
| Board cannot join Wi-Fi | Check the password and use a 2.4 GHz network |
| Screen stays dark but the USB console answers | The display failed to start and the gadget runs without it. The boot log names the call that failed (`... failed: ESP_ERR_...`); check the wiring, the panel revision and the board profile |
| "No pairing code yet" | An allowlist may reject unknown senders. Keep `platforms.gadget.extra.unauthorized_dm_behavior: pair`, or add the device ID to `GADGET_ALLOWED_USERS`. Check `hermes pairing list` |
| "Refused by a route" | A `gateway.profile_routes` entry matches a device that switched profiles before, and such a device answers to the default profile only. Remove the route; the device is let in within a few seconds. See [Hermes integration](hermes-integration.md#profile-switching-and-profile-routes) |
| "Device key does not match" | After a factory reset, run `hermes gadget forget <device_id>` on the Hermes computer, then pair again |
| Browser cannot find a board | Use Chrome or Edge on a computer and a USB data cable. If there are two USB ports, use UART for settings. Close other serial monitors |

## Speech and replies

| Symptom | What to check |
|---|---|
| Demo only echoes text | That is the scripted demo. [Connect Hermes](connect-hermes.md) for agent replies |
| Simulator records silence | Install the audio extra and enable the microphone in **Settings** or use `--live-audio`. Check the system microphone permissions and default input. The audio status under the conversation reports microphone errors |
| Voice is not transcribed | Configure speech recognition on the Hermes computer |
| Replies are text only | Configure TTS, check that `speak_replies` is not `false`, and enable simulator live audio. Non-WAV TTS needs `ffmpeg` on the Hermes computer |

## Screen

| Symptom | What to check |
|---|---|
| Screen freezes while voice, Wi-Fi and touch keep working | [Save a board report](#save-a-board-report). `LCD transfer timed out` in its log means the panel's SPI transfer did not finish; the screen resumes by itself if it does. `LCD transfer failed` stops the screen until the board restarts. Restart the board and send the report with a bug report |

## Save a board report

The [browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html) can save a report under **Troubleshooting**, **Something else**, **Save a diagnostics report**. It includes the board status and recent log.

If you already use the SDK's serial tools, run `hermes-gadget diag --port COM5`, replacing `COM5` with your board's port. Attach the saved file and the steps to reproduce the problem to a [bug report](https://github.com/Adolanium/hermes-gadget-sdk/issues).
