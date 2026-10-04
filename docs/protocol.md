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
   "display": {"width": 320, "height": 240, "color": true, "charset": "ascii",
               "text_cols": 25, "text_rows": 8,
               "image": {"width": 320, "height": 196, "format": "rgb565"}},
   "mic": {"rate": 16000, "format": "pcm16"},
   "speaker": {"rate": 16000, "format": "pcm16"},
   "inputs": ["talk", "cancel", "up", "down"],
   "talk_mode": "hold"},
 "actions": [{"name": "led.set", "description": "Set the status LED colour.",
              "params": {"type": "object", "properties": {"color": {"type": "string"}},
                         "required": ["color"]}}],
 "sensors": {"battery_pct": 87}}
```

- Every `caps` member is optional. A device without a speaker omits `speaker`, and the server then never sends it audio.
- `caps.ota` (`{"max_size": 2031616}`) means the device installs firmware updates over this connection, up to `max_size` bytes.
- `actions` is the device's tool manifest. Each action has a JSON-Schema `params` object and a `description` written for the model.
- `token` is required only when the host sets `GADGET_ACCESS_TOKEN`. A mismatch is rejected with error code `bad_token`.

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
{"type": "welcome", "session": "3f2a9c1b7d4e", "paired": false, "heartbeat_s": 20, "server": "hermes", "proto": 1}
```

`paired` reports whether Hermes will accept messages from this device. Authorization is enforced by Hermes itself on every message; the flag only drives the device UI.

### Pairing

| Message | Direction | Meaning |
|---|---|---|
| `{"type": "pairing", "code": "ABCD2345", "command": "hermes pairing approve gadget ABCD2345"}` | server → device | Show this code; the owner approves it on the Hermes host |
| `{"type": "paired"}` | server → device | Approved (no reconnect needed) |
| `{"type": "unpaired"}` | server → device | Approval was revoked |

## Conversation

### Device → server

| Message | Meaning |
|---|---|
| `{"type": "text", "id": "t3", "text": "..."}` | A typed message (keyboards, simulator, serial `say`) |
| `{"type": "audio.start", "id": "a4", "stream": 4, "rate": 16000, "format": "pcm16", "mode": "hold"}` | An utterance begins; binary audio frames follow on `stream` |
| `{"type": "audio.end", "id": "a4", "stream": 4, "duration_ms": 2100}` | Utterance complete; the server turns it into a WAV voice message |
| `{"type": "audio.cancel", "id": "a4", "stream": 4, "reason": "too short"}` | Discard the utterance |
| `{"type": "cancel"}` | Stop the current turn (Hermes `/stop`) |
| `{"type": "session.new"}` | Start a fresh conversation (Hermes `/new`). The reference firmware sends it when CANCEL is held for 2 s |
| `{"type": "prompt.reply", "id": "q1", "answer": "yes"}` | The answer (`yes` or `no`) to a `prompt` |

- Utterances shorter than 0.25 s are dropped with a `notice`.
- The server caps an utterance at `max_utterance_s` (60 s by default).

### Server → device

| Message | Meaning |
|---|---|
| `{"type": "turn.start", "turn": "..."}` | Hermes started working on a message |
| `{"type": "transcript", "text": "..."}` | What speech-to-text heard |
| `{"type": "status", "text": "Searching the web"}` | Live working-state phrase (empty clears it) |
| `{"type": "reply.delta", "turn": "...", "text": "partial..."}` | Streaming preview; `text` is cumulative |
| `{"type": "reply", "turn": "...", "text": "...", "interim": true}` | A reply. With `interim`, it is progress commentary rather than the answer |
| `{"type": "turn.end", "turn": "...", "outcome": "success\|failure\|cancelled"}` | The turn is over |
| `{"type": "notice", "text": "...", "ttl_s": 8}` | Transient one-line message |
| `{"type": "prompt", "id": "q1", "title": "Confirm /new", "text": "...", "ttl_s": 300}` | A yes/no question; see [Questions](#questions) |
| `{"type": "prompt.close", "id": "q1"}` | The question was withdrawn (timed out or answered elsewhere) |
| `{"type": "error", "code": "...", "message": "..."}` | Protocol or auth error; the server usually closes the connection next |

Reply text is already shaped for the device: Markdown is stripped and the text is folded to ASCII when `charset` is `"ascii"`. Devices render it as-is.

### Audio to the device

```json
{"type": "audio.start", "stream": 7, "rate": 16000, "format": "pcm16", "turn": "..."}
```

1. The server sends `audio.start`.
2. Binary channel-1 frames follow, about 40 ms each.
3. `{"type": "audio.end", "stream": 7}` marks normal completion; `{"type": "audio.abort", "stream": 7}` means stop now.

The server resamples to the device's declared speaker rate. It also paces frames to real time, at most 0.5 s ahead, so a jitter buffer of about 1 s is enough. A new `audio.start` replaces the stream that is playing.

### Display

| Message | Meaning |
|---|---|
| `{"type": "display", "title": "Timer", "body": "Pasta: 9 min", "ttl_s": 15}` | Show a card; `ttl_s: 0` keeps it until dismissed |
| `{"type": "image.start", "stream": 9, "width": 196, "height": 196, "format": "rgb565", "ttl_s": 30}` | Image rows follow on binary channel 2, then `image.end` |
| `{"type": "image.end", "stream": 9}` | End of image data |

The server fits images inside `caps.display.image` before sending them, so devices never decode JPEG or PNG.

### Questions

Hermes asks before some actions: destructive commands such as `/new`, a costly model switch, or a dangerous shell command the agent wants to run. The server turns each one into a `prompt`:

- The device shows the title and text with two answers. The reference firmware maps TALK to yes and CANCEL to no, and ignores presses in the first 0.6 s so a press meant for something else doesn't answer it.
- The device answers with `prompt.reply` exactly once, or not at all if the question expires (`ttl_s`, when present) or is withdrawn with `prompt.close`.
- The server sends one question at a time. A question asked while a device was offline is sent again when it reconnects.
- Text is shaped like replies: short, plain and already folded to the device's charset.

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
| `{"type": "event", "name": "button.long_press", "data": {...}, "notify": false}` | With `notify: true`, the event is delivered to the agent as a message |

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
| `{"type": "ota.error", "code": "checksum", "message": "..."}` | device → server | The update is abandoned. Codes: `unsupported`, `bad_offer`, `too_large`, `no_offer`, `unauthorized`, `flash`, `sequence`, `size`, `checksum`, `invalid`, `timeout`, `no_update` |
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

## Versioning

- `proto` is a single integer, and a server rejects versions it does not speak.
- Adding optional fields or new message types does not change the version. Receivers ignore unknown types and fields.
- Changing the meaning of an existing field does.
