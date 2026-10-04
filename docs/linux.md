# Run a Linux gadget

The Linux client runs the same device core as the ESP32 firmware. It keeps a
device identity, pairs with Hermes, reconnects after network interruptions, and
accepts local text messages and events. It runs without a desktop or display.

The initial target is Raspberry Pi 4 or 5 with 64-bit Raspberry Pi OS Lite Trixie.
This port is experimental. No physical Pi verification report is recorded yet.
CI runs the native core and Linux socket tests on x86-64 and ARM64 Ubuntu.

## Install on Raspberry Pi

Use Raspberry Pi Imager to install 64-bit Raspberry Pi OS Lite Trixie. Configure
your network and SSH access there, then boot the Pi with a suitable power supply.
The Linux client uses the OS network settings.

Download the `hermes-gadget-VERSION-linux-arm64.tar.gz` archive and its `.sha256`
file from a [release](https://github.com/Adolanium/hermes-gadget-sdk/releases) that
includes the Pi package. Until the first such release is tagged, the same files
are available in the `linux-arm64` artifact of a successful `main`
[CI run](https://github.com/Adolanium/hermes-gadget-sdk/actions/workflows/ci.yml).
Extract the artifact ZIP first if downloading from CI.

In a directory containing just the chosen archive and checksum file:

```bash
sudo apt update
sudo apt install python3-venv libportaudio2 libstdc++6
sha256sum --check hermes-gadget-*-linux-arm64.tar.gz.sha256
tar -xzf hermes-gadget-*-linux-arm64.tar.gz
cd hermes-gadget-*-linux-arm64
sudo sh install.sh
sudoedit /etc/hermes-gadget/config.json
```

Set `server` to your Hermes host, such as `ws://192.168.1.20:8765/gadget`, and
choose a `name`. Add `token` if your gateway requires it. The installer downloads
Python dependencies into a private environment, creates the `hermes-gadget`
service account, and installs the native core. No compiler is needed on the Pi.
Configuration and device state stay outside the installed release directory.

```bash
sudo systemctl enable --now hermes-gadget
sudo hermes-gadget-device status
```

Approve the pairing code on the Hermes host with `hermes gadget approve CODE`.
Then use the installed device:

```bash
sudo hermes-gadget-device send "Hello from the Pi"
sudo hermes-gadget-device messages
```

`hermes-gadget-device` runs controls as the service account and selects the
correct state directory. Use it in place of `hermes-gadget linux` in the examples
below when working with a package installation. To diagnose startup, use
`sudo journalctl -u hermes-gadget -n 50`.

## Build from source

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

The Pi installer already installs these dependencies. For a source checkout:

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
The Pi installer's environment already includes GPIO Zero and can import
system packages. Install `python3-lgpio`, then restart the service. The installer
adds the service account to existing `audio` and `gpio` groups.

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

## Add a screen

Add `display` to the configuration to show the firmware's device screen:

```json
"display": {
  "width": 320,
  "height": 240,
  "fullscreen": true,
  "rotation": 0,
  "touch": true
}
```

The dimensions describe the device canvas. The client scales that canvas to
fit the monitor and leaves black borders where needed. Both dimensions must
be even numbers between 160 and 800. Set `round` to `true` for a circular
canvas with equal width and height. Rotation accepts 0, 90, 180 or 270 degrees
counterclockwise. Use the same orientation as your touch input.

On a Linux desktop, install `.[display]`, set `fullscreen` to `false` for a
window, and start the client from your desktop terminal. Space is TALK, Escape
is Cancel, and the arrow keys scroll. Touch or hold the screen to interact;
swipe down to cancel. Closing the window stops the client. Omitting `display`
keeps the service headless.

On Raspberry Pi OS Lite, the optional screen needs an SDL build with KMS/DRM
support. Prefer the OS package so it uses the system graphics drivers:

```bash
sudo apt install python3-pygame
```

Use a virtual environment created with `--system-site-packages`. From the
local console, set `SDL_VIDEODRIVER=kmsdrm` before starting the client. If SDL
reports that the driver is unavailable, use a desktop session or install a
system SDL/pygame build with KMS/DRM support. A pip wheel's available video
drivers can differ from the OS package.

Direct display access requires the graphics device and an active local seat.
Test from the Pi's local console before configuring unattended display startup.
The supplied system service runs headlessly by default; an SSH session or a
background service does not automatically get permission to own the display.
Do not run the client as root to bypass a display error. For a desktop kiosk,
start the client in the logged-in user's graphical session with its own state
directory. Never run it alongside the system service with the same identity.

The screen uses the existing firmware renderer for pairing, conversations,
cards, prompts and images. Audio and GPIO still use the configured real
devices. Software rendering and touch mapping pass automated tests; monitor,
touch-controller and direct-console behavior still need physical verification.

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
environment at `/opt/hermes-gadget/current`, plus a configuration file at
`/etc/hermes-gadget/config.json`. The package installer creates these paths.

The client only advertises configured display, audio and output actions. It
reports no battery or ESP32 update slot.

## Update or roll back

For a package installation, download and verify the new archive, extract it,
then run its `install.sh` with sudo. The installer validates the new native
library before stopping the current service. It switches the `current` link and
restarts a previously running service. Reinstalling the same package is safe.
The installer preserves `/etc/hermes-gadget/config.json` and
`/var/lib/hermes-gadget/device.json`, including the pairing identity.

Back up those two files securely before updating. Installed releases remain in
`/opt/hermes-gadget/releases`; the `previous` link identifies the prior release.
To roll back, rerun the installer from the previous release's archive. Check
`sudo hermes-gadget-device status` after either operation. An interrupted
dependency installation leaves a named incomplete directory; move that specific
directory aside before retrying, as the installer instructs.

For a source checkout, stop the process, update the checkout, rebuild the native
library, and restart with the same state directory.

To stop using the installed service, run `sudo systemctl disable --now
hermes-gadget`. Remove its unit from `/etc/systemd/system/hermes-gadget.service`
and its helper from `/usr/local/bin/hermes-gadget-device`, then run
`sudo systemctl daemon-reload`. Application files are under
`/opt/hermes-gadget`. Retain configuration and state if you may reinstall later.
