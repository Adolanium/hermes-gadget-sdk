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

Install the plugin into your Hermes home, enable it and start the gateway:

```bash
hermes-gadget plugin install          # copies plugin/ to ~/.hermes/plugins/gadget (or $HERMES_HOME)
hermes plugins enable gadget
hermes config set platforms.gadget.enabled true
hermes gateway run                    # or restart your gateway service
hermes gadget info                    # prints the URL devices should use
```

Speech needs Hermes's STT and TTS configured: `hermes tools` / `hermes setup`, or the `stt:` and `tts:` sections of `config.yaml`. Without STT, voice messages still reach the agent, but as untranscribed audio notes.

Point the simulator at the gateway:

```bash
hermes-gadget sim --url ws://127.0.0.1:8765/gadget --name "Desk Gadget"
```

Approve the pairing code it shows:

```bash
hermes pairing approve gadget <CODE>
```

The device switches to **Ready** within about 2 seconds. Hold Space to talk, or type, and replies come from your Hermes with its normal tools, memory and skills. Try:

- "Show a reminder on my screen to water the plants". The model uses `gadget_display`.
- "Turn the LED purple". The model uses `gadget_action`.

## 4. Real hardware

The firmware builds in CI but has not run on a real board yet, so the first flash may need debugging. See [hardware.md](hardware.md) for the reference wiring and for flashing with `idf.py` or PlatformIO. Once the board is flashed, configure it over USB serial:

```bash
pip install "hermes-gadget[serial]"
hermes-gadget provision --port COM5 --wifi-ssid MyWifi --wifi-pass secret \
    --server ws://192.168.1.20:8765/gadget --name "Kitchen"
```

It pairs exactly like the simulator.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Simulator says "simulator library not found" | Run `hermes-gadget build-sim` from the repo checkout |
| Device stuck on "Connecting" | Check the URL from `hermes gadget info`, the firewall on the Hermes host, and that the gateway log shows `✓ gadget connected` |
| "No pairing code yet" notice | Hermes is ignoring unknown senders because an allowlist is configured. Keep `platforms.gadget.extra.unauthorized_dm_behavior: pair`, or add the device id to `GADGET_ALLOWED_USERS`. `hermes pairing list` shows pending codes |
| "device key does not match" error | The device was factory-reset. Run `hermes gadget forget <device_id>` on the host |
| Replies are text only | Configure TTS (`hermes tools`), or check that `speak_replies` is not `false`. Non-WAV TTS output needs `ffmpeg` on the Hermes host |
