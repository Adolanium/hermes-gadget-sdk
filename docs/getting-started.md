# Getting started

You need no hardware for any of this. You will:

1. build the simulator;
2. talk to the development server (no Hermes needed);
3. connect the simulator to your real Hermes;
4. flash a board when you have one.

## Prerequisites

- Python 3.10+.
- CMake 3.16+ and a C++17 compiler. On Windows that is Visual Studio Build Tools; on macOS, Xcode command-line tools; on Linux, gcc or clang.
- A Hermes Agent install (for step 3).
- Optional:
  - `ffmpeg`, so the plugin can play TTS formats other than WAV;
  - `pip install "hermes-gadget[audio]"`, for the PC microphone and speakers in the simulator.

## 1. Install the tools and build the simulator

```bash
git clone <this repo> hermes-gadget-sdk && cd hermes-gadget-sdk
python -m venv .venv && . .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -e ".[dev]"
hermes-gadget build-sim --test     # compiles firmware/core + the simulator library, runs the core tests
```

`build-sim` writes the library to `build/host/`, which is where the simulator looks for it. Set `HGSIM_LIBRARY` to load it from somewhere else.

## 2. First conversation, no Hermes required

The development server runs the plugin's real device hub with a scripted brain. It echoes text, reports voice clips and plays them back, and simulates pairing.

```bash
hermes-gadget devserver --pairing          # terminal 1
hermes-gadget sim --url ws://127.0.0.1:8765/gadget   # terminal 2
```

1. The simulator window shows a pairing code. Type `approve <CODE>` in the devserver terminal.
2. Type a message in the simulator's input box and press Enter. You get a streamed echo back.
3. Hold **Space** (or the TALK button) and release. The server reports what it received and plays it back through the simulated speaker. With `--live-audio` it records your PC microphone; otherwise it records silence. Use **Speak WAV...** to send a recording.
4. In the devserver terminal, try:
   - `card Shopping | eggs, milk`
   - `action led.set {"color": "red"}` (watch the LED in the simulator window)
   - `notice hello`

## 3. Connect to Hermes

Install the plugin into your Hermes and turn the platform on:

```bash
hermes plugins install https://github.com/Adolanium/hermes-gadget-sdk/tree/main/plugin --enable
hermes gateway setup                  # pick Hermes Gadget; restart the gateway when it asks
```

The setup step enables the platform, asks which port devices connect to (8765 by default), and prints the URL devices should use with a link to the browser installer. `hermes gadget info` shows the same later.

That installs the plugin from `main`. To match a firmware release instead, use the install command in the [release's notes](https://github.com/Adolanium/hermes-gadget-sdk/releases), which pins the plugin to the release's commit with `--ref`.

Working on the plugin itself? `hermes-gadget plugin install --link` links your checkout's `plugin/` into Hermes instead, so edits take effect on the next gateway start.

Speech needs Hermes's STT and TTS configured: `hermes tools` / `hermes setup`, or the `stt:` and `tts:` sections of `config.yaml`. Without STT, voice messages still reach the agent, but as untranscribed audio notes.

Point the simulator at the gateway:

```bash
hermes-gadget sim --url ws://127.0.0.1:8765/gadget --name "Desk Gadget"
```

Approve the pairing code it shows:

```bash
hermes gadget pair                    # or: hermes pairing approve gadget <CODE>
```

`hermes gadget pair` waits for a device to show a code, says which device it is, and asks before approving it.

The device switches to **Ready** within about 2 seconds. Hold Space to talk, or type, and replies come from your Hermes with its normal tools, memory and skills. Try:

- "Show a reminder on my screen to water the plants". The model uses `gadget_display`.
- "Turn the LED purple". The model uses `gadget_action`.

## 4. Real hardware

Plug the board into a computer and open the **[browser installer](https://adolanium.github.io/hermes-gadget-sdk/)** in Chrome or Edge. Use the link `hermes gateway setup` or `hermes gadget info` prints, and the Hermes address is filled in for you. The installer:

1. checks that the board matches the firmware you picked, then installs it, keeping the board's settings if it already runs Hermes Gadget;
2. gives it your Wi-Fi network, the Hermes address and a name, over USB;
3. shows it joining Wi-Fi, reaching Hermes and asking to pair; approve it with `hermes gadget pair`.

The breadboard build needs the wiring in [hardware.md](hardware.md) first. To build and flash the firmware yourself, see [hardware.md](hardware.md#build-and-flash). Then set it up from the installer's **skip to Wi-Fi** option, or over USB serial:

```bash
pip install "hermes-gadget[serial]"
hermes-gadget provision --port COM5 --wifi-ssid MyWifi --wifi-pass secret \
    --server ws://192.168.1.20:8765/gadget --name "Kitchen"
```

It pairs exactly like the simulator.

If something on the board doesn't work, save a diagnostics report and attach it to an [issue](https://github.com/Adolanium/hermes-gadget-sdk/issues):

```bash
hermes-gadget diag --port COM5
```

It prints a short summary (reset reason, which parts came up, I2C chips that answer, Wi-Fi, the Hermes connection) and saves the full report with the recent log to a file.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Simulator says "simulator library not found" | Run `hermes-gadget build-sim` from the repo checkout |
| Device stuck on "Connecting" | Check the URL from `hermes gadget info`, the firewall on the Hermes host, and that the gateway log shows `✓ gadget connected` |
| "No pairing code yet" notice | Hermes is ignoring unknown senders because an allowlist is configured. Keep `platforms.gadget.extra.unauthorized_dm_behavior: pair`, or add the device id to `GADGET_ALLOWED_USERS`. `hermes pairing list` shows pending codes |
| "device key does not match" error | The device was factory-reset. Run `hermes gadget forget <device_id>` on the host |
| Replies are text only | Configure TTS (`hermes tools`), or check that `speak_replies` is not `false`. Non-WAV TTS output needs `ffmpeg` on the Hermes host |
