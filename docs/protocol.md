# Hermes Gadget Protocol, version 1

This is the contract between a device and the gadget plugin running inside the Hermes gateway. The device side lives in `firmware/core` (C++) and the server side in `plugin/hub.py` (Python). Both test suites pin the same vectors, so the two sides cannot drift apart without a test failing.

## Transport

- One WebSocket per device: `ws://<hermes-host>:<port><path>`. The defaults are port `8765` and path `/gadget`. Run `hermes gadget info` on the host to print the exact URL.
- Use `wss://` when the plugin is configured with `tls_cert`/`tls_key`.
- Subprotocol: `hermes-gadget.v1`. Devices should offer it, and the server accepts connections that don't.
- **Text frames** carry UTF-8 JSON objects. Every object has a `"type"` field. Fields that aren't needed are omitted rather than sent as `null`.
- **Binary frames** carry media and start with a 4-byte header:

  | Offset | Size | Field                                 |
  |--------|------|---------------------------------------|
  | 0      | u8   | channel: `0x01` audio, `0x02` image, `0x03` firmware |
  | 1      | u8   | stream id (from the matching `*.start` or `ota.offer`) |
  | 2      | u16 LE | sequence number (wraps)             |
  | 4      | ...  | payload                               |

  - Audio payload: mono PCM16, little-endian, at the rate announced by `audio.start`.
  - Image payload: RGB565, little-endian, row-major. Each chunk continues where the previous one stopped.
  - Firmware payload: the next bytes of the image; see [Firmware updates](#firmware-updates).

## Handshake

```
device                                   server
  | -- hello ------------------------------> |   identity, capabilities, actions
  | <------------------------- challenge --- |   nonce, enrolled?
  | -- auth -------------------------------> |   key (first contact) or HMAC
  | <--------------------------- welcome --- |   paired?, heartbeat
  | <------------- pairing (when unpaired) - |   code + approve command
  | <------------- paired (after approval) - |
```

### Identity

Each device generates a random 32-byte **device key** on first boot and keeps it in NVS. Its id is derived from the key:

```
device_id = "hg-" + hex(sha256(key))[0:16]
```

Because the id is derived from the key, nobody can claim another device's id without that device's key.

### `hello` (device → server)

```json
{"type": "hello", "proto": 1, "device_id": "hg-630dcd2966c43366",
 "name": "Kitchen Gadget", "board": "esp32s3-breadboard", "firmware": "0.1.0",
 "token": "<optional access token>",
 "caps": {
   "display": {"width": 320, "height": 240, "color": true, "charset": "latin1",
               "text_cols": 25, "text_rows": 8,
               "image": {"width": 320, "height": 196, "format": "rgb565"}},
   "mic": {"rate": 16000, "format": "pcm16"},
   "speaker": {"rate": 16000, "format": "pcm16"},
   "inputs": ["talk", "cancel", "up", "down"],
   "talk_mode": "hold",
   "profiles": true, "faces": 1},
 "profile": "ops",
 "actions": [{"name": "led.set", "description": "Set the status LED colour.",
              "params": {"type": "object", "properties": {"color": {"type": "string"}},
                         "required": ["color"]}}],
 "sensors": {"battery_pct": 87}}
```

- Every `caps` member is optional. A device without a speaker omits `speaker`, and the server then never sends it audio.
- `caps.speaker.formats` (`["pcm16", "opus"]`) lists the speaker audio formats the device decodes. `format` stays `"pcm16"` for servers that predate the list, and PCM16 is always accepted. See [Audio to the device](#audio-to-the-device).
- `caps.ota` (`{"max_size": 2031616}`) means the device installs firmware updates over this connection, up to `max_size` bytes.
- `caps.dashboard` (`{"v": 1, "rows": 6}`) means the device draws the [dashboard](#dashboard). `v` is the newest layout it draws, and `rows` is how many lines fit on its screen at once.
- `actions` is the device's tool manifest. Each action has a JSON-Schema `params` object and a `description` written for the model.
- `caps.profiles: true` means the device can switch Hermes profiles (see [Profiles](#profiles)). `profile` is the profile it used last; it is omitted for `default`.
- `caps.faces: 1` means the device draws a face that the server sends for the profile answering it (see [Faces](#faces)). `face` is the `hash` of the face it holds from an earlier connection; it is omitted when it shows the mascot.
- `token` is required only when the host sets `GADGET_ACCESS_TOKEN`. A mismatch is rejected with error code `bad_token`.
- `display_sleeping: true` means the screen is dark as the device connects; it is omitted while the screen is lit. See [Screen sleep](#screen-sleep).

### `challenge` (server → device)

```json
{"type": "challenge", "nonce": "<base64 16 bytes>", "enrolled": true}
```

### `auth` (device → server)

- **First contact** (`enrolled: false`): `{"type": "auth", "key": "<base64 32-byte key>"}`. The server checks that the key hashes to `device_id`, then stores it. This is trust on first use: the key crosses the network exactly once.
- **Later connections** (`enrolled: true`): `{"type": "auth", "mac": "<base64 HMAC>"}`, where:

  ```
  mac = HMAC-SHA256(key, "hermes-gadget/v1|" + device_id + "|" + nonce)
  ```

If authentication fails, the server sends `{"type": "error", "code": "auth_failed", ...}` and closes the connection. A device that lost its key (factory reset) re-enrolls after the owner runs `hermes gadget forget <device_id>`.

### `welcome` (server → device)

```json
{"type": "welcome", "session": "3f2a9c1b7d4e", "paired": false, "heartbeat_s": 20, "server": "hermes", "proto": 1,
 "language": "es"}
```

`language` is optional: the language id Hermes uses (`"es"`, `"pt-br"`, `"zh-hant"`), from the plugin's `language` setting or else Hermes's own (`HERMES_LANGUAGE`, then `display.language`). A device whose `language` setting is `auto` shows its own text in it: the id itself if the firmware has it, else its primary subtag (`es-mx` as `es`), else English. Other devices ignore it, as does older firmware.

`paired` reports whether Hermes will accept messages from this device. Authorization is enforced by Hermes itself on every message; the flag only drives the device UI.

```json
{"type": "welcome", "session": "3f2a9c1b7d4e", "paired": true, "heartbeat_s": 20, "server": "hermes", "proto": 1,
 "profile": "ops", "profiles": [{"id": "default", "name": "Hermes"}, {"id": "ops", "name": "Ops"}]}
```

`profile` and `profiles` are present only when the device sent `caps.profiles` and the server offers profiles. The Hermes plugin offers the profiles a multiplexed gateway serves that `platforms.gadget.extra.profiles` lists (all of them for `all`), with `default` first; without that setting it offers none. The device keeps the first 8 entries of `profiles` and ignores the rest.

`face` is present only when the device named a face in `hello.face` and may keep it; see [Faces](#faces).

### Pairing

| Message | Direction | Meaning |
|---|---|---|
| `{"type": "pairing", "code": "ABCD2345", "command": "hermes pairing approve gadget ABCD2345"}` | server → device | Show this code; the owner approves it on the Hermes host |
| `{"type": "paired"}` | server → device | Approved (no reconnect needed) |
| `{"type": "unpaired"}` | server → device | Approval was revoked |

### Profiles

A Hermes gateway can serve several profiles (each with its own memory, personality, skills and model). A device that sends `caps.profiles: true` gets the profiles it may use in `welcome.profiles` and the one answering it in `welcome.profile`, and can switch:

| Message | Direction | Meaning |
|---|---|---|
| `{"type": "profile.select", "profile": "ops"}` | device → server | Ask to talk to another profile |
| `{"type": "profile", "profile": "ops"}` | server → device | The profile now answering this device; the device stores it and sends it as `hello.profile` next time |
| `{"type": "profile", "profile": "default", "error": "busy"}` | server → device | The request was refused; `profile` is unchanged. `error` is `unknown`, `busy` (a turn or question is pending), `unpaired` or `routed` |

A refusal changes nothing on the device. `error` is `routed` when the server's configuration routes this device to a profile; such a device gets no `profiles` in `welcome`. The server may also send `profile` on its own, for example `default` after the device's approval is revoked. A device that does not send `caps.profiles` keeps the gateway's existing routing (normally the default profile).

### Faces

A device that sends `caps.faces: 1` can show each profile with its own face instead of the built-in mascot:

```json
{"type": "face", "profile": "ops", "hash": "3f2a91c0d4e5b6a7", "bits": "<base64 26612 bytes>"}
```

| Field | Meaning |
|---|---|
| `profile` | The device's own profile: the last `profile` or `welcome.profile` it got, else the `hello.profile` it sent (`default` without one). The face is that of the profile answering the device, which a gadget route can make another one |
| `hash` | Names this face. The device sends it back as `hello.face` |
| `bits` | The face. Without `bits` (and `hash`), the device shows the mascot again |

`bits` holds the three frames (idle, blink, talk) at each of the four sizes (64, 96, 144 and 192 px), smallest size first and idle first within a size: 26,592 bytes in the [layout of the built-in mascot](faces.md#how-it-works). Ten little-endian int16 effect anchors follow, in thousandths of the drawn size: the listening waves' x and y, the speaking waves' x and y, the thinking dots' x and y, the first and last row a blink changes, and the first and last row talking changes. Each anchor is 0 to 1000, and each pair of rows has the first at or above the last. A device ignores a face of any other length, or with an anchor out of range, and shows the mascot.

- The server sends `face` after `welcome`, after each `profile` that is not a refusal, and after `paired` and `unpaired`. An unpaired device gets the mascot: on `unpaired`, or a `welcome` with `paired: false`, the device drops its face, and it ignores `face` until it is paired again.
- The device keeps its face in memory, through reconnects, while `welcome.face` names it. Before the welcome, the server looks up the `hash` of the face it would send for the profile answering the device, and names the held face only when it has that hash, so an unchanged face stays through reconnects and server restarts and is not sent again. The Hermes plugin names a face file by its bytes and an avatar by its picture's bytes, so the lookup reads one file and converts nothing; one that takes longer than 1.5 s counts as no match. The face itself is made and sent after the welcome. A welcome that does not name the held face, after a new gadget route, a changed face or from a server without faces, makes the device drop it, as does a welcome with `paired: false`.
- The device also drops its face after a `profile` that names another profile. Either way it shows the mascot until the next `face` arrives, never the previous profile's face. It ignores a `face`, with or without `bits`, whose `profile` is not its own, as the table defines it.
- The server sends nothing when the device already has the right face: when the welcome named it, or when the device shows the mascot and the profile has no face.
- A device without `caps.faces` gets no `face` messages. The reference firmware sends `caps.faces` only on boards with PSRAM: a face takes about 27 KB, and its message about 36 KB more while it arrives.

The Hermes plugin sends a profile's `gadget-face.bin`, or else the profile's Hermes avatar made into a still face; see [Faces](faces.md#a-face-for-each-hermes-profile).

## Conversation

### Device → server

| Message | Meaning |
|---|---|
| `{"type": "text", "id": "t3", "text": "..."}` | A typed message (keyboards, simulator, serial `say`) |
| `{"type": "audio.start", "id": "a4", "stream": 4, "rate": 16000, "format": "pcm16", "mode": "hold"}` | An utterance begins; binary audio frames follow on `stream` |
| `{"type": "audio.end", "id": "a4", "stream": 4, "duration_ms": 2100}` | Utterance complete; the server turns it into a WAV voice message |
| `{"type": "audio.cancel", "id": "a4", "stream": 4, "reason": "too short"}` | Discard the utterance |
| `{"type": "cancel"}` | Stop the current turn (Hermes `/stop`). With no turn running it only stops the reply audio the server is still sending, and no `/stop` is sent |
| `{"type": "session.new"}` | Start a fresh conversation (Hermes `/new`). The reference firmware sends it when CANCEL is held for 2 s |
| `{"type": "prompt.reply", "id": "q1", "answer": "yes"}` | The answer (`yes` or `no`) to a `prompt` |

- Utterances shorter than 0.25 s are dropped with a `notice`, and the server then sends a `turn.end` with an empty `turn` and outcome `cancelled`, so the device stops waiting.
- The server caps an utterance at `max_utterance_s` (60 s by default).

### Server → device

| Message | Meaning |
|---|---|
| `{"type": "turn.start", "turn": "..."}` | Hermes started working on a message |
| `{"type": "transcript", "text": "..."}` | What speech-to-text heard |
| `{"type": "status", "text": "Searching the web"}` | Live working-state phrase (empty clears it) |
| `{"type": "reply.delta", "turn": "...", "text": "partial..."}` | Streaming preview; `text` is cumulative |
| `{"type": "reply", "turn": "...", "text": "...", "interim": true}` | A reply. With `interim`, it is progress commentary rather than the answer |
| `{"type": "turn.end", "turn": "...", "outcome": "success\|failure\|cancelled"}` | The turn is over. An empty `turn` ends whatever the device is waiting for; the server sends one, as `cancelled`, after dropping a too-short utterance |
| `{"type": "notice", "text": "...", "ttl_s": 8}` | Transient one-line message |
| `{"type": "prompt", "id": "q1", "title": "Confirm /new", "text": "...", "ttl_s": 300}` | A yes/no question; see [Questions](#questions) |
| `{"type": "prompt.close", "id": "q1"}` | The question was withdrawn (timed out or answered elsewhere) |
| `{"type": "error", "code": "...", "message": "..."}` | Protocol or auth error; the server usually closes the connection next |

Reply text is already shaped for the device: Markdown is stripped and the text is folded to what its `charset` draws. `"ascii"` gets printable ASCII only. `"latin1"` gets ASCII and Latin-1 (U+00A1 to U+00FF, so accented letters, ¿ ¡ and the like), with no-break spaces as plain spaces; anything else is folded to the nearest letter or dropped. Text is UTF-8 either way, and devices render it as-is. The reference firmware reports `"latin1"` since its font gained Latin-1; older firmware reports `"ascii"`.

### Audio to the device

```json
{"type": "audio.start", "stream": 7, "rate": 16000, "format": "pcm16", "turn": "..."}
```

1. The server sends `audio.start`.
2. Binary channel-1 frames follow, about 40 ms each.
3. `{"type": "audio.end", "stream": 7}` marks normal completion; `{"type": "audio.abort", "stream": 7}` means stop now.

`format` is `"pcm16"` (little-endian mono samples) unless the device lists `"opus"` in `caps.speaker.formats`: then the server may send `"opus"`, and each binary frame carries one Opus packet (mono, at `rate`, one 40 ms frame; the last frame of a reply is padded with silence). The plugin sends Opus at 24 kbps when the host has libopus, about a tenth of PCM16 at 16 kHz, and falls back to PCM16 without it.

The server resamples to the device's declared speaker rate. It also paces frames to real time, at most 0.5 s ahead, so a jitter buffer of about 1 s is enough. A new `audio.start` replaces the stream that is playing.

### Display

| Message | Meaning |
|---|---|
| `{"type": "display", "title": "Shopping", "body": "Milk, eggs, bread", "ttl_s": 15}` | Show a card; `ttl_s: 0` keeps it until dismissed |
| `{"type": "image.start", "stream": 9, "width": 196, "height": 196, "format": "rgb565", "ttl_s": 30}` | Image rows follow on binary channel 2, then `image.end` |
| `{"type": "image.end", "stream": 9}` | End of image data |

The server fits images inside `caps.display.image` before sending them, so devices never decode JPEG or PNG. A device that can't show images (such as a board whose framebuffer holds only a band of rows) leaves `image` out of `caps.display`.

### Screen sleep

A device whose screen goes dark (a timeout, the power key, or lying face down) tells the server, and the server holds back screen updates until the screen lights up again. This saves the radio time and the screen wake-ups that cards and images would otherwise cost while nobody is looking.

- The device sends `{"type": "state", "display_sleeping": true}` when the screen goes dark and `{"type": "state", "display_sleeping": false}` when it lights up. Only changes are sent, as they happen, without the rate limit sensor readings have. A device that connects in the dark says so in `hello` instead.
- While the screen is dark the server keeps the latest `display` card or image (a newer one replaces an older one, as it would on screen), the latest `status` phrase and the latest `dashboard`, and sends none of them.
- When the screen lights up, the server sends the held `status` phrase, then the held `dashboard`, then the held card or image. A card or image with a `ttl_s` keeps only the time it has left, counted from when it was sent, and one whose time ran out is dropped. A held status phrase is dropped when its turn ends. A held dashboard is dropped when a newer one goes out first, and is not sent to a device that is unpaired or that reconnects without `caps.dashboard`.
- What is held survives a reconnect: it goes out once the device connects with its screen lit.
- Everything else is sent at once: `prompt` and `prompt.close`, `pairing`, turns, transcripts, replies, audio, `notice`, actions and firmware updates. A question or a reply still lights the screen, except while the device lies face down: then it waits for the device to be turned over.
- A device that never sends `display_sleeping` counts as lit, so nothing is held back for it.

### Dashboard

A device that advertises `caps.dashboard` gets a live summary of Hermes: the agent's sessions, and status tiles the owner sets on the host (`hermes gadget tile`). Each message is a complete snapshot that replaces the last one:

```json
{"type": "dashboard", "v": 1,
 "sessions": [{"id": "3f2a", "label": "telegram 3f2a", "state": "busy"},
              {"id": "91c0", "label": "cli 91c0", "state": "waiting"}],
 "tiles": [{"id": "nas", "label": "NAS", "value": "up", "state": "ok"}]}
```

| Field | Meaning |
|---|---|
| `v` | Layout version. A device ignores a dashboard whose `v` it doesn't draw. The server sends only to devices whose `caps.dashboard.v` is at least the version it sends |
| `sessions[].state` | `idle`, `busy` (a turn is running) or `waiting` (the turn waits for the user to approve a command) |
| `sessions[].label` | Text to show: the platform and the end of the session id |
| `tiles[].label`, `tiles[].value` | Text to show for a tile |
| `tiles[].state` | `ok`, `warn` or `bad`; omitted for a neutral tile |
| `id` | Stable within the snapshot; devices may ignore it |

- Text is shaped like replies: folded to the device's `charset` and cut to its `text_cols`. Waiting sessions come first, then busy ones, then idle ones; at most 8 sessions and 8 tiles.
- The server sends a dashboard when a paired device connects, and again only when something in it changes. An unpaired device gets none.
- A dashboard is data, not a screen takeover: the device keeps showing what it showed. The reference firmware opens it with CANCEL (a swipe down on touch boards) on the idle screen and closes it with CANCEL again. Talking, a question, a card or an image take the screen over. It forgets the dashboard when the connection drops, and the next connection sends it again.
- While the screen is dark, only the latest dashboard is kept; see [Screen sleep](#screen-sleep).

### Questions

Hermes asks before some actions: destructive commands such as `/new`, a costly model switch, or a dangerous shell command the agent wants to run. The server turns each one into a `prompt`:

- The device shows the title and text with two answers. The reference firmware maps TALK to yes and CANCEL to no, and ignores presses in the first 0.6 s so a press meant for something else doesn't answer it.
- The device answers with `prompt.reply` exactly once, or not at all if the question expires (`ttl_s`, when present) or is withdrawn with `prompt.close`. A command approval carries Hermes's approval timeout (`approvals.timeout`, 300 s by default) as its `ttl_s`.
- The server sends one question at a time. A question asked while a device was offline is sent again when it reconnects.
- Text is shaped like replies: short, plain and already folded to the device's charset. A dangerous-command approval is the exception: its text is the command as it will run, unchanged, or a notice to approve on the host when the command doesn't fit one line of `text_cols`, less two columns, as-is. The server ignores a yes to that notice.

### Device actions

| Message | Direction |
|---|---|
| `{"type": "action", "id": "x1", "name": "led.set", "args": {"color": "red"}}` | server → device |
| `{"type": "action.result", "id": "x1", "ok": true, "result": {"led": "red"}}` | device → server |
| `{"type": "action.result", "id": "x1", "ok": false, "error": "color is required"}` | device → server |

Devices must answer every `action` exactly once. The server times out after about 18 s.

### Telemetry and events (device → server)

| Message | Meaning |
|---|---|
| `{"type": "state", "sensors": {"battery_pct": 80, "temperature_c": 21.5}}` | Latest readings (rate limited by the device) |
| `{"type": "state", "display_sleeping": true}` | The screen went dark (`false`: it lit up); see [Screen sleep](#screen-sleep) |
| `{"type": "event", "name": "button.long_press", "data": {...}, "notify": false}` | With `notify: true`, the event is delivered to the agent as a message |
| `{"type": "event", "name": "timer.done", "data": {"label": "Pasta"}, "notify": false}` | The device's timer ran out (the label is empty when none was set). It always has `notify: false`: the alarm is the device's own, and Hermes is not interrupted. Dropped while the device is offline |

### Firmware updates

A device that advertises `caps.ota` installs a new firmware image the server streams to it. Only the server holding the device's enrolled key can authorize an image:

```
server                                   device
  | -- ota.offer -------------------------> |   size, SHA-256, version, stream
  | <------------------------- ota.ready -- |   a fresh nonce
  | -- ota.begin -------------------------> |   MAC over nonce, SHA-256 and size
  | <--------------------- ota.ack (0) ---- |
  | == binary channel 3, 4 KB frames =====> |
  | <-------- ota.ack (every 16 KB) ------- |
  | -- ota.end ---------------------------> |
  | <-------------------------- ota.done -- |   then the device restarts into the new firmware
```

| Message | Direction | Meaning |
|---|---|---|
| `{"type": "ota.offer", "stream": 12, "size": 1172496, "sha256": "<64 hex>", "version": "0.2.0"}` | server → device | An image is coming. The device answers `ota.ready`, or `ota.error` (`too_large`, `unsupported`, ...) |
| `{"type": "ota.ready", "nonce": "<base64 16 bytes>"}` | device → server | A fresh nonce for this update only |
| `{"type": "ota.begin", "mac": "<base64 HMAC>"}` | server → device | Authorizes the image (below); the device answers `ota.ack` with offset 0 |
| `{"type": "ota.ack", "offset": 16384}` | device → server | Bytes written so far: every 16 KB and at the end |
| `{"type": "ota.end"}` | server → device | All bytes sent; the device checks the size and SHA-256, then the image itself |
| `{"type": "ota.done", "version": "0.2.0"}` | device → server | Installed; the device restarts about a second later |
| `{"type": "ota.error", "code": "checksum", "message": "..."}` | device → server | The update is abandoned. Codes: `unsupported`, `bad_offer`, `too_large`, `no_offer`, `unauthorized`, `flash` (also an image whose `HGBOARD=` tag names another board), `sequence`, `size`, `checksum`, `invalid` (also an image with no board tag), `timeout`, `no_update` |
| `{"type": "ota.abort"}` | server → device | Abandon the update in progress |

```
mac = HMAC-SHA256(key, "hermes-gadget/v1|ota|" + device_id + "|" + nonce + "|" + sha256_hex + "|" + size)
```

- `sha256_hex` is the lowercase hex SHA-256 of the whole image, and `size` its length in decimal.
- The server keeps at most 64 KB unacknowledged. Frames carry consecutive sequence numbers from 0; a gap abandons the update (`sequence`).
- An update that gets no data for 30 s is abandoned (`timeout`), and so is one whose connection drops.
- The new firmware boots on probation. If it doesn't reach a server (`welcome`) within 5 minutes, or crashes first, the device goes back to the previous firmware.

### Heartbeat

- The server sends `{"type": "ping", "ts": 1727950000000}` every `heartbeat_s`, and the device answers `{"type": "pong", "ts": ...}`.
- Either side may also send `ping`.
- Either side treats 3 × `heartbeat_s` without any inbound frame as a dead connection.

## Phone app over Bluetooth

Boards whose firmware enables Bluetooth (none yet; see [hardware validation](hardware-validation.md#bluetooth)) also accept a phone app over Bluetooth LE. It is a local channel to the device's console, not part of the Hermes protocol: the phone sends the same commands as the [USB console](hardware.md#serial-console) and gets the same `@` replies, so an app can set up Wi-Fi (`set wifi_ssid`, `set wifi_pass`), change settings and read `status`.

| | UUID | Properties |
|---|---|---|
| Service | `dc530001-7a2a-400f-b8f6-e87185576192` | Primary, advertised |
| Command | `dc530002-7a2a-400f-b8f6-e87185576192` | Write with response; needs an encrypted, authenticated link |
| Reply | `dc530003-7a2a-400f-b8f6-e87185576192` | Notify |

- **Pairing:** the phone must pair before writing. The device accepts pairing only while **Phone app** pairing is open (device settings, or `phone pair`; two minutes). It shows a six-digit passkey, which the phone's user types: LE Secure Connections with passkey entry. The device refuses pairing outside the window, refuses Just Works pairing, and keeps up to three phones, forgetting the oldest. `phone forget` removes them all and drops a connected phone.
- **Advertising:** the device advertises its name and the service while pairing is open, about once a second while a phone is paired, and not at all otherwise. One phone connects at a time.
- **Commands:** subscribe to Reply, then write one console line (UTF-8, up to 256 bytes) to Command. Wait for its reply before writing the next; a third pending line is refused with "insufficient resources".
- **Replies:** the reply text arrives in notifications sized to the link's MTU, ending with a single `0x00` byte. `@error device busy` means the device didn't answer within 3 s.
- **State:** `status` includes `"phone": {"paired": 1, "connected": true, "pairing": false}` on these boards; `phone` alone returns the same object as `@phone {...}`.

## Versioning

- `proto` is a single integer, and a server rejects versions it does not speak.
- Adding optional fields or new message types does not change the version. Receivers ignore unknown types and fields.
- Changing the meaning of an existing field does.
