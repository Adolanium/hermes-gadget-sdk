# Architecture

```
 ┌──────────────── device (ESP32 or simulator) ───────────────┐        ┌──────────── Hermes host ───────────────────────────┐
 │                                                            │        │                                                    │
 │  drivers / HAL ──events──▶ hg::App (firmware/core, C++17)  │  WS    │  plugin/hub.py ◀──▶ plugin/adapter.py ◀──▶ gateway │
 │  Wi-Fi, WebSocket,        state machine · protocol · auth  │◀──────▶│  auth, audio,      BasePlatformAdapter     runner, │
 │  LCD, I2S mic/amp,        UI renderer · VAD · actions      │ JSON + │  pacing, images    MessageEvent · TTS seams STT,   │
 │  buttons, NVS, console                                     │ binary │                                         agent, │
 │                                                            │        │  plugin/tools.py ── gadget_* agent tools   tools   │
 └────────────────────────────────────────────────────────────┘        └────────────────────────────────────────────────────┘
```

## Design goals

1. **Hermes stays unchanged.** The SDK plugs in through the gateway's platform-plugin interface and the hooks every adapter already has; see [hermes-integration.md](hermes-integration.md).
2. **The device stays thin.**
   - Speech recognition, text-to-speech, the language model, Markdown handling and image decoding all happen on the host.
   - The device captures audio, renders text and pixels, plays PCM and runs the actions it declares.
   - Firmware therefore needs no codecs, fonts beyond ASCII, or model-specific logic, and server-side improvements reach every board without reflashing.
3. **One core, many hosts.**
   - Everything that is not a driver lives in `firmware/core`: protocol, authentication, connection lifecycle, pairing UX, push-to-talk, VAD, playback control, UI, actions, telemetry and console. It is portable C++17 with no exceptions or RTTI, compiled unchanged for the ESP32 and for the desktop simulator.
   - The Hermes side mirrors this: `plugin/hub.py` knows nothing about Hermes and runs unchanged under the gateway adapter and under the standalone development server.
4. **Small surface, explicit contracts.**
   - The device–host protocol is one WebSocket carrying JSON control frames and binary media frames ([protocol.md](protocol.md)).
   - The device–driver contract is six small C++ interfaces (`firmware/core/include/hg/hal.hpp`).

## Device core (`firmware/core`)

| Module | Responsibility |
|---|---|
| `hal.hpp` | `Display`, `AudioIn`, `AudioOut`, `Transport`, `Storage`, `System`, and the optional `Updater` (the firmware update slot): implemented per port |
| `app.hpp/.cpp` | `hg::App`, the single-threaded application. Connection phases (boot → network → connecting → handshake → online) × interaction modes (idle, listening, thinking, responding) × overlays (card, image, question). It also authorizes and checks firmware updates and confirms a new firmware once it reaches Hermes |
| `app_timer.cpp` | The timer: the three device actions (`timer.start`, `timer.status`, `timer.cancel`), the countdown and the alarm with its beeps. Registered from `App::begin()` on boards with a display or a speaker. The alarm is not an overlay: it has its own screen, survives a dropped connection, and stops on a press, a tap or swipe, `timer.cancel`, `timer.start`, a recording, or a new card or image |
| `protocol.*`, `json.*`, `crypto.*` | Message helpers, a small JSON parser/serializer, SHA-256/HMAC/Base64 for authentication and update checks |
| `ui.*`, `canvas.*`, `font5x7.cpp` | Deterministic renderer: a `UiModel` is drawn in four horizontal bands (top bar, header, content, hint bar). Only bands whose inputs changed are redrawn and flushed |
| `vad.*` | Energy VAD that ends hands-free (tap) utterances on silence |

On a round panel, the edge ring lies in the band just inside the glass edge, around the UI square, which no band owns. `Ui` derives one ring per frame from the model, the first match winning: none on the alarm or a battery face, the resting clock's marks and seconds dot, the recording limit used, update progress, the timer's countdown, or a thinking, speaking or connecting comet. So a recording or an update takes the ring from a running timer, which comes back after it. Every flush repaints the ring's pixels for the rows it sends, so no band, face or battery strip can erase it. A ring that appears or goes away repaints the panel once; without a ring, the bands own the band's pixels in the UI square's corners. Any other ring change writes only the ring's pixels that differ and sends their rows, with changed rows at most 8 apart sent as one run. A comet's step computes only the rectangle that holds what the comet lights where it was and where it is, about a quarter of the band (`Ui::comet_box`, from each degree's rectangle of band pixels, found once with the layout). On a framebuffer that holds only a band of rows (no PSRAM), any ring change repaints the panel; no round board is banded today. The alarm, the battery faces and the resting clock (`Screen::Clock`, on an online, paired Ready screen after the `clock` setting's rest) each take the whole panel instead of the bands; the clock keeps the ring, whose seconds dot is about twice as wide as the band, so on the clock the ring also owns the dot's reach on both sides of it, and each second the dot moves without redrawing the face, checking only the rows of its old and new places. The clock's marks and dot turn half a degree per pixel of the face's sideways burn-in shift, so by up to 2 degrees; the turn changes only with the minute, whose repaint of the face carries it. One full-panel face flag records which face owns the panel, and leaving it repaints everything. The clock keeps its own rest stamp, so its rest counts from the end of the last speech, reply or overlay rather than the last input.

### Threading model

`App` is not thread-safe, and that is deliberate.

- Ports call `App::tick()` every 5–20 ms and deliver every asynchronous event (Wi-Fi up, WebSocket frame, microphone chunk, button edge, motion sample, console line) on that same thread.
- On the ESP32, driver tasks post to a FreeRTOS queue that the main task drains. The motion task is the exception: it keeps only its latest sample, and the main task reads it.
- In the simulator, the WebSocket thread posts to a Python queue that the UI thread drains.

As a result the core has no locks and every test is deterministic.

### Rendering

- The port owns an RGB565 framebuffer, in PSRAM on the ESP32. A board with no PSRAM (the CYD) holds a band of rows instead and reports it from `Display::framebuffer_rows()`; the UI then renders each changed band once per window, so the renderer below is unchanged.
- `Ui::render()` hashes the inputs of each band and redraws only the bands that changed, flushing them as full-width rows (contiguous memory, so a panel driver can DMA them).
- Animations (spinner, level meter, speaking bars) live only in the 35-pixel header band, so a 10 fps animation pushes about 22 KB/s over SPI.
- The renderer reads no clocks and uses no randomness. Identical models produce identical pixels, and the simulator tests rely on that.

### The mascot

Every screen that isn't showing reply text uses a **hero layout**: the Hermes Agent mascot, as large as the screen allows, with a short caption. That covers idle, listening, thinking, speaking, boot, connecting, offline, setup errors, pairing, short cards and questions.

- **Captions:** a headline and one detail line. Pairing, setup, cards and questions may wrap to three or four lines; the renderer picks the largest mascot that still fits, and drops caption lines rather than shrink her below the smallest bitmap. A question adds its two answer buttons under the caption.
- **No scrolling needed:** a card too long for a caption gets the text layout. Long replies and long cards page by themselves (about a second per line), so a board with no scroll buttons can still read everything. UP/DOWN scrolls by hand and pauses the paging for 15 s.

- **Source:** the bitmaps are generated by `hermes-gadget face` from Hermes's own artwork (`assets/mascot`) as 1-bit frames: idle, blink and talk, at 64, 96, 144 and 192 px. They take about 27 KB of flash. With no arguments the command regenerates exactly this art; [faces.md](faces.md) covers making the face something else.
- **Effects:** drawn on top of the frames:
  - blinking every few seconds;
  - green sound waves from her headphone cup while listening, scaled by the microphone level;
  - blue thought dots while thinking;
  - a moving mouth and amber waves while speaking.
- **Partial redraws:** the hero band tracks static content (screen, caption) separately from animation state. When only the animation changes, it redraws and flushes just the rows that animation can touch: the eye rows for a blink, the wave rows while listening. A blink therefore costs about 25 rows of SPI traffic, not a full screen.
- **Speaking:** while a reply is spoken, the face talks over the reply's latest two lines, then the text layout shows the whole reply when the speech ends. A tap, UP or DOWN shows the whole text at once. On panels where two caption lines would push the face down to its smallest bitmap (240x135, 128x128 and the 240x240 round), the reply's text takes the screen as soon as it arrives, and the face talks only while no text has arrived yet.
- **Reply text:** a finished reply stays on screen for 20 s before the mascot returns. On boards with scroll buttons, UP brings it back. Until a button press or touch lands on the reply's text, it counts as unread: when the screen goes dark first, the input that wakes it shows the reply again, from the top, for another 20 s. A press or touch over the mascot does not count as reading it.
- **Waiting for Hermes:** the Thinking screen keeps the display lit. A request that Hermes hasn't started a turn for gives up after 3 minutes without a message and shows "No reply from Hermes". Once Hermes sends `turn.start`, the device waits for its `turn.end` (or a lost connection) for up to 30 minutes without a message, so a long task doesn't send the screen back to Ready and then dark.
- **Timer:** the alarm is a full-panel screen with its own layout, not a hero. While a timer runs, the Ready caption shows the time left.

### Interaction

- **Push-to-talk (default):** hold to speak, release to send. A press under 350 ms is discarded as a tap, and recording stops at 30 s.
- **Tap mode** (`set talk_mode tap`): tap to start; the VAD (or a second tap) ends the utterance.
- **Barge-in:** pressing talk while a reply plays stops playback immediately. The new message reaches Hermes, whose default "interrupt" busy mode stops the old turn.
- **Cancel** acts on release: it discards a recording, dismisses an overlay, or stops a turn (Hermes `/stop`).
- **Holding CANCEL for 2 s** starts a new session (`session.new` → Hermes `/new`). It fires while still held, after a countdown in the hint bar. The hold is the confirmation, so the plugin answers Hermes's "Confirm /new" itself.
- **Questions** from Hermes (other confirmations, dangerous-command approvals) take over the screen: TALK answers yes, CANCEL answers no. A question that arrives while you are recording waits until you finish.

### The recording buffer

On boards with PSRAM to spare (`DeviceProfile::recording_alloc`), a recording is kept whole on the device while it uploads (`hg::RecordingBuffer`, 1 MB for the 30 s limit at 16 kHz, 1.5 MB at 24 kHz, set aside at start-up, and only when about 1 MB of PSRAM stays free after it; start-up logs `recording buffer on` or why not). The microphone writes to the buffer, and the app sends from it in 40 ms frames as the link takes them:

- **A slow link** delays frames instead of dropping them. `audio.end` waits until the link has taken all of the recording.
- **A lost connection mid-recording** doesn't end it. The screen stays on Listening with "Offline, still recording", and the recording ends as usual: release, a tap, the VAD, or the 30 s limit. The app reconnects in the background, starting at the shortest backoff.
- **Back online** (welcome, paired), the whole recording goes again on a new stream: `audio.start` with the same `id` plus `resent` and `age_ms`, every frame as fast as the link takes them, then `audio.end`. A connection back before the recording ends sends what was held and streams on.
- **Ended while offline,** the device shows Thinking with "Reconnecting to send..." and waits up to 45 s (`App::kRecordingWaitMs`, about three connection attempts). Failed attempts don't extend the wait. After that it drops the recording and says "Couldn't send the recording".
- **CANCEL** discards a held recording, while recording or while waiting. A device that comes back unpaired drops it. The buffer is RAM only, so a restart loses it.
- **Until all of it has gone,** messages for an earlier turn (a reply the user talked over) don't end the new one. A new recording, a new session, going home or the Thinking timeout drops it, with `audio.cancel` while its upload is on the current connection (a recording held through a lost connection has nothing on the hub to cancel).
- **A connection lost after `audio.end` went** ends the turn as before. The protocol has no acknowledgement, so sending it again could run the turn twice.

A board without PSRAM, or whose PSRAM fails the headroom check or the allocation at start-up, keeps the old behaviour: audio streams straight to the link, frames the link refuses are counted and dropped, and a lost connection ends the recording. Raising the utterance limit is a change to `App::kMaxUtteranceMs` alone: the buffer is sized from it.

## Hermes side (`plugin/`)

| Module | Responsibility |
|---|---|
| `hub.py` | `DeviceHub` (websockets server, handshake and enrollment, heartbeats, routing) and `DeviceSession` (outbound API: replies, status, cards, images, paced `AudioOut`, `invoke_action`) |
| `adapter.py` | `GadgetAdapter`, the `HubDelegate` that maps device traffic to `MessageEvent`s and Hermes output to device frames |
| `tools.py` | Agent tools, run on agent worker threads and bridged to the hub loop with `run_coroutine_threadsafe` |
| `textfmt.py` | Markdown → plain text, typography → ASCII |
| `audio.py` | WAV packing, streaming resampler, file decoding (WAV natively, the rest through ffmpeg) |
| `imaging.py` | Any image → RGB565 that fits the device's image box (Pillow) |
| `store.py` | Device keys and pending pairing codes in the plugin's data directory |

### Audio pacing

`AudioOut` resamples whatever Hermes produces (24 kHz streaming PCM, MP3/OGG/WAV files) to the device's declared rate. It sends 40 ms frames at most 0.5 s ahead of real time. If the producer stalls (a streaming TTS provider between sentences), the pacing clock rebases, so it never bursts more than the lead. A device therefore needs about 1 s of buffer whatever the reply length.

### Security model

- **Network gate (optional):** `GADGET_ACCESS_TOKEN`, a shared secret every device presents in `hello`.
- **Device identity:**
  - Each device holds a random 32-byte key, and its id is a hash of that key.
  - The key is sent once, at enrollment. After that the device proves possession with an HMAC over a fresh server nonce, so recorded traffic cannot be replayed.
  - Another device cannot take over an enrolled id.
- **Authorization:** Hermes's own allowlists and DM pairing. Every message from a device is authorized by the gateway runner exactly like a message from a Telegram user, and pairing approval happens on the Hermes host. A device that switches profiles is approved once, in the default profile: the plugin approves it in each profile when it first switches there, and a revocation in the default profile sends it back.
- **Transport:** use `wss://` (`tls_cert`/`tls_key`) on any network you don't trust. On a home LAN, plain `ws://` exposes conversation content to anyone on the network, though not the device key after enrollment.

## Why not use an existing Hermes API instead of a plugin?

Hermes also exposes an OpenAI-compatible API server and a JSON-RPC backend for its TUI and desktop app. A thin device could post text to either one. A platform adapter is the better fit:

- **Voice:** voice messages get Hermes's STT and auto-TTS for free, including streaming PCM. The HTTP API has no audio input, and the device would need its own ASR.
- **Identity and authorization:** each device becomes a first-class chat with its own session, memory and pairing, rather than an API client sharing one key.
- **Push:** the gateway can push to the device (cron results, `send_message`, other chats driving its tools) over the open socket.
- **Reach:** the adapter gets streaming previews, status phrases, interrupts and approvals through the same paths every messaging platform uses.
