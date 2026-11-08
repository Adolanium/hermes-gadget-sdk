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
| Connecting screen stays on a *Tailnet:* message | The gadget is waiting for your tailnet before it contacts Hermes. Each message is explained in [what the screen shows](tailnet.md#what-the-screen-shows); `diag` has the details under `tailnet` |
| The connection drops, often during a recording | Run `diag` on the USB console. `connection.closes` lists the last five losses, oldest first, each with its reason and what the gadget was doing (`recording`, `sending a recording`, `waiting for a reply`, `idle`...). A reason with `send in flight for 10000 ms` means the uplink stalled for 10 s and the send gave up. `transport errno 104` means the server or a proxy reset the connection. `close code` is the server's own close. A Wi-Fi loss reads `Wi-Fi lost (reason 200 beacon timeout, rssi -78)`. The `rssi` is the signal when it happened, and below about -75 it is weak. `recordings.last` gives the last recording's length and its frames not sent: those the link refused, and a part frame a lost connection cut off. On boards with the [recording buffer](architecture.md#the-recording-buffer), `recordings` also counts the recordings held through a lost connection, sent again and dropped, and `recordings.last.backlog_peak_ms` is the most audio that waited for the link at once. Each buffered recording logs one `recording:` line with those numbers. The serial log has a `connection closed:` or `network down:` line for each loss, and, while a turn runs, an `uplink:` line a second with what the device sent |
| Board cannot join Wi-Fi | Check the password and use a 2.4 GHz network |
| Screen stays dark but the USB console answers | The display failed to start and the gadget runs without it. The boot log names the call that failed (`... failed: ESP_ERR_...`); check the wiring, the panel revision and the board profile |
| "No pairing code yet" | An allowlist may reject unknown senders. Keep `platforms.gadget.extra.unauthorized_dm_behavior: pair`, or add the device ID to `GADGET_ALLOWED_USERS`. Check `hermes pairing list` |
| "Refused by a route" | A `gateway.profile_routes` entry matches a device that switched profiles before, and such a device answers to the default profile only. Remove the route that names the device ID the screen shows, as its `user_id` or `chat_id`, and restart the gateway (`hermes gateway restart`) so it loads the change. Then, if the default profile still approves the device, it is let in within a few seconds; otherwise it goes back to pairing. See [Hermes integration](hermes-integration.md#profile-switching-and-profile-routes) |
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
| Touch stops responding | [Save a board report](#save-a-board-report) while touch is still dead. Its `touch` section tells the causes apart. A `task_ms_ago` above about 1000 means the touch task stopped running. A growing `error_streak` and an old `last_read_ms_ago` mean the touch controller stopped answering. `events_dropped`, or `events.dropped`, means the touches were read but the app's event queue was full. In `stacks`, a task's `cpu_pct` is its share of one core since the previous `diag`, so run `diag` once before you reproduce the problem and once after |

## Save a board report

Save the report while the problem is happening, before you turn the board off. A restart keeps the log, but losing power clears it.

The [browser installer](https://adolanium.github.io/hermes-gadget-sdk/installer.html) can save a report under **Troubleshooting**, **Something else**, **Save a diagnostics report**. If you already use the SDK's serial tools, run `hermes-gadget diag --port COM5`, replacing `COM5` with your board's port. Attach the saved file and the steps to reproduce the problem to a [bug report](https://github.com/Adolanium/hermes-gadget-sdk/issues).

The report includes:

- The board's status, memory, Wi-Fi and the parts that started.
- Each task's free stack, state and CPU share.
- The touch counters and the event queue.
- The last crash, if there was one.
- The recent log. After a panic, a watchdog or a software restart, the log starts with the lines from before that restart, under `@log previous boot, ended by <reason>`.

If the app stops answering, the console still replies to `diag` for the port alone, with `"app": "not answering"`, and `diag log` still works.

To add detail to the log, raise one tag's log level from the console (`hermes-gadget console --port COM5`) before you reproduce the problem. For example, use `log hg.touch debug`, or `log * warn` for less output. The levels are `none`, `error`, `warn`, `info` and `debug`. A level lasts until the board restarts.

### Read a core dump

When the firmware crashes, it saves a short core dump, and `diag` reports it under `crash`. The dump includes the crashed task, its program counter (`pc`) and, on the ESP32 and ESP32-S3, its backtrace. `this_build` says whether the dump came from the firmware that is running now. The next crash replaces the dump.

A board updated over the air keeps the partition table it was flashed with, so `crash.state` may be `no partition`. To add the core dump partition, install the firmware over USB once. The settings stay.

To turn the addresses into function names and lines, use the `firmware.elf` of the build that crashed. For a release, download it from the **firmware-elf** artifact of that release's **Release** workflow run; maintainers have access to it for 90 days. For a local build, use `.pio/build/<board>/firmware.elf`. Then run:

```bash
xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf 0x42001234 0x42005678
```

List `pc` and the `backtrace` addresses. Use `xtensa-esp32-elf-addr2line` for the ESP32 and `riscv32-esp-elf-addr2line` for the ESP32-P4. On the P4, also pass `ra`.
