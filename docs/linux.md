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

The headless client reports no display, microphone, speaker, battery or ESP32
update slot. Software updates require stopping the process, updating the
checkout, rebuilding the native library, and restarting with the same state
directory.
