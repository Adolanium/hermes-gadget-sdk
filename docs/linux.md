# Run a Linux gadget

The Linux client runs the same device core as the ESP32 firmware. It keeps a
device identity, pairs with Hermes, reconnects after network interruptions, and
accepts local text messages and events. It runs without a desktop or display.

The initial target is Raspberry Pi 4 or 5 with 64-bit Raspberry Pi OS Lite.
This port is experimental. No physical Pi verification report is recorded yet.
CI runs the native core and Linux socket tests on x86-64 and ARM64 Ubuntu.

## Build and start

On Linux, install Python 3.10 or later, a C++17 compiler, CMake and Git. For
Raspberry Pi OS:

```bash
sudo apt update
sudo apt install git python3-venv cmake build-essential
git clone https://github.com/Adolanium/hermes-gadget-sdk.git
cd hermes-gadget-sdk
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -e .
hermes-gadget build-sim --test
```

`build-sim` builds the shared native library used by both the Linux client and
desktop simulator. Keep this checkout available while running the client.

Create `device-config.json` with your Hermes host address:

```json
{
  "server": "ws://192.168.1.20:8765/gadget",
  "name": "Kitchen Gadget"
}
```

Add a `token` string if your Hermes gateway requires an access token. Protect the
configuration with `chmod 600 device-config.json`. Use `wss://` outside a trusted
local network. Follow [Connect Hermes](connect-hermes.md) to enable the plugin.

```bash
hermes-gadget linux run --config device-config.json
```

In another terminal with the environment activated:

```bash
hermes-gadget linux status
```

Approve the reported pairing code with `hermes gadget approve CODE` on your
Hermes host. Then send a message and read the response:

```bash
hermes-gadget linux send "Hello from the kitchen"
hermes-gadget linux messages
```

## Local controls

```bash
hermes-gadget linux event door.opened --data '{"room":"kitchen"}' --notify
hermes-gadget linux button cancel press
hermes-gadget linux button cancel release
```

`send` and `event` require a paired, connected device. They do not queue messages
while offline. `--notify` asks the agent to respond to the event; omit it to
report the event without starting a turn. The service keeps the latest 20
replies and notices in memory. `messages` includes sequence numbers so a local
consumer can ignore entries it already read. Restarting clears that history.

## Add USB audio

Install PortAudio and the audio extra, then list the connected devices:

```bash
sudo apt install libportaudio2
python -m pip install -e '.[audio]'
hermes-gadget linux audio-devices
```

Add `audio` to your configuration. Use a device number from the list or a unique
part of its name. Names are preferable when USB device numbers change:

```json
{
  "server": "ws://192.168.1.20:8765/gadget",
  "name": "Kitchen Gadget",
  "audio": {"input": "USB Audio", "output": "USB Audio", "rate": 16000}
}
```

Omit `input` or `output` when that device is absent. Supported sample rates are
8000, 16000, 24000, 32000, 44100 and 48000 Hz, mono PCM16. The selected devices
must support the configured rate. Stop the service and run:

```bash
hermes-gadget linux audio-check --config device-config.json
```

Speak for three seconds. The check prints the captured peak level and plays the
clip back at half volume. It sends nothing to Hermes and saves no recording.
If the check rejects 16000 Hz, try 48000 Hz or an ALSA device that supports
conversion. Check capture levels with `alsamixer` if the signal is silent.
The systemd service account needs access to `/dev/snd`, typically through the
`audio` group. Test as that account when deploying a service.

Restart the client, press TALK with `hermes-gadget linux button talk press`,
then release with `hermes-gadget linux button talk release`. Use physical
buttons for everyday voice interaction. `status` reports audio errors. A failed
microphone stops the recording; the client does not substitute silent input.
Reattach an unplugged audio device and start a new recording to retry it.

## Add Raspberry Pi buttons and outputs

Use GPIO Zero with the lgpio backend on Pi 4 and Pi 5. On Raspberry Pi OS, install
`python3-lgpio` and create the virtual environment with `--system-site-packages`
so it can import that system package, then install `.[gpio]`.

Add this object to the configuration:

```json
"gpio": {
  "chip": 0,
  "talk": 17,
  "cancel": 27,
  "status_led": 22,
  "outputs": {"desk_light": 23}
}
```

The numbers are BCM GPIO numbers, not physical header positions. Each pin must
be unique. Wire each momentary button between its GPIO and ground; the client
enables pull-ups and debounces presses. Connect LEDs through a suitable series
resistor. Use a driver circuit for loads a GPIO cannot supply. Only use
3.3 V-compatible logic on the header.

The status LED stays on when paired and connected, blinks slowly while offline
or awaiting pairing, and blinks quickly during recording. `gpio.desk_light`
becomes a device action with one boolean parameter, `on`. Only configured
outputs are exposed. Outputs start off and return off on a clean shutdown.
Do not use this software as a safety controller; a power failure cannot promise
a controlled output transition.

The service account needs access to `/dev/gpiochip0`, normally through the
`gpio` group. Older Pi 5 kernels may expose the header on gpiochip4; set `chip`
to 4 in that case. Verify the header controller with `gpioinfo` before wiring.
See [GPIO Zero's pin documentation](https://gpiozero.readthedocs.io/en/stable/api_pins.html)
for the lgpio backend and permissions.

## State and recovery

The default state directory is `$XDG_STATE_HOME/hermes-gadget`, or
`~/.local/state/hermes-gadget`. To use another directory, put
`--state-dir /path/to/state` immediately after `linux` in every command.

The directory is private to its owner. It contains `device.json`, a process
lock, and `control.sock`. The socket accepts one newline-terminated JSON request
per connection. For example, `{"command":"status"}` returns the same JSON as
the CLI. Only local processes with permission to access the directory can use
it. The control API cannot execute shell commands or reset the device identity.

Back up `device.json` securely. It contains the device key and optional access
token. A damaged state file stops startup rather than replacing your identity.
Restore your backup, or stop the service and move the state directory aside to
enroll a new device. Configuration changes take effect on the next start.

Ctrl+C or SIGTERM closes the connection and exits. Only one service can use a
state directory at a time. `status` reports connection state and uptime even
when Hermes is unavailable. Pairing failures appear in `messages`.

For systemd deployments, [linux/hermes-gadget.service](../linux/hermes-gadget.service)
defines a dedicated `hermes-gadget` user, a private `/var/lib/hermes-gadget` state
directory, and restart on failure. It expects an installation and virtual
environment at `/opt/hermes-gadget`, plus a configuration file at
`/etc/hermes-gadget/config.json`. Create those paths and the service account
before installing the unit. Run control commands as the service account with
`--state-dir /var/lib/hermes-gadget`.

The client only advertises configured audio and output actions. It reports no
display, battery or ESP32 update slot. Software updates require stopping the process, updating the
checkout, rebuilding the native library, and restarting with the same state
directory.
