# How the SDK integrates with Hermes Agent

The SDK treats Hermes Agent as an external dependency and needs **no changes to Hermes**. Everything goes through surfaces Hermes already publishes for third-party platforms. This page lists those surfaces, explains how each one is used, and states plainly where the SDK relies on behavior that is not a formal API. It ends with the few small upstream changes that would remove those workarounds.

## The integration point: a platform plugin

Hermes's gateway runs "platform adapters": Telegram, Slack and about twenty others. A directory plugin with `kind: platform` can register a new adapter through `ctx.register_platform(...)` without touching core code; see `gateway/platforms/ADDING_A_PLATFORM.md` in Hermes. The SDK's `plugin/` directory is such a plugin:

| Plugin file | Role |
|---|---|
| `plugin.yaml` | Manifest: `kind: platform`, optional env vars surfaced in `hermes config` |
| `__init__.py` | `register(ctx)`: platform entry, agent tools, `hermes gadget` CLI |
| `adapter.py` | `GadgetAdapter(BasePlatformAdapter)`: the Hermes-facing side |
| `hub.py` | WebSocket server, auth, audio, images, actions (no Hermes imports) |
| `tools.py` | `gadget_devices`, `gadget_display`, `gadget_action` |
| `store.py`, `protocol.py`, `audio.py`, `textfmt.py`, `imaging.py` | Support modules (no Hermes imports) |

Each device is a direct-message chat whose `chat_id` and `user_id` are the device id. Everything Hermes does for a DM on any platform then applies unchanged: sessions and memory, `/new`, `/stop`, `/voice`, model routing, approvals and delivery.

### Configuration

```yaml
# ~/.hermes/config.yaml
plugins:
  enabled: [gadget]          # or: hermes plugins enable gadget
platforms:
  gadget:
    enabled: true
    extra:
      host: 0.0.0.0          # interface devices connect to
      port: 8765
      path: /gadget
      speak_replies: true    # spoken replies for devices with a speaker
      auto_home: true        # first approved device becomes the gadget home channel
      unauthorized_dm_behavior: pair
      # heartbeat_s: 20
      # max_utterance_s: 60
      # tls_cert: /path/cert.pem   # serve wss://
      # tls_key: /path/key.pem
```

Secrets go in `~/.hermes/.env`, following Hermes's rule that `.env` is only for secrets:

- `GADGET_ACCESS_TOKEN`: optional shared token every device must present.
- `GADGET_ALLOWED_USERS`: comma-separated device ids admitted without pairing.
- `GADGET_ALLOW_ALL_USERS=true`: admit any device (development only).

> If you set `GADGET_ALLOWED_USERS` (or `GATEWAY_ALLOWED_USERS`), Hermes switches unknown senders from "pair" to "ignore". Keep `unauthorized_dm_behavior: pair` in the platform's `extra` so new devices still receive pairing codes.

### Change the gadget port

The adapter reads `platforms.gadget.extra` when the gateway creates it. Editing the configuration or toggling the plugin does not rebind a running gadget listener. Restart the gateway to apply changes to the port, host, path, or TLS settings.

If another service uses the default port, 8765, choose a free port on the Hermes computer. For example:

```bash
hermes config set platforms.gadget.extra.port 9100
```

Restart the gateway service with `hermes gateway restart`. If you run `hermes gateway run` in a terminal, stop that process and start it again. Allow the new port through the host firewall when devices connect from other computers.

After the restart, run `hermes gadget info` and use the new URL to update each device's server address. For a local simulator, this example becomes `hermes-gadget sim --url ws://127.0.0.1:9100/gadget`. Devices configured with the old port cannot reconnect until their server address changes.

`hermes gadget info` prints the configured URL. It does not confirm that the running gateway has rebound to that address. Check the gateway's startup log for the listening port and confirm that a device reconnects.

## Hermes surfaces the SDK uses

| Need | Hermes surface | Notes |
|---|---|---|
| Register a transport | `ctx.register_platform(...)` → `PlatformEntry` | `allowed_users_env`, `allow_all_env`, `platform_hint`, `max_message_length`, `parse_target_ref_fn` |
| Setup wizard | `PlatformEntry.setup_fn` | `hermes gateway setup` lists Hermes Gadget and runs its step: enable the platform, pick the port (written with `hermes config set`'s writer), print the device URL and the installer link. The wizard then offers the gateway restart |
| Inbound messages | `BasePlatformAdapter.handle_message(MessageEvent)` | text → `MessageType.TEXT`; voice → `MessageType.VOICE` with a WAV in `media_urls` |
| Store voice audio | `cache_audio_from_bytes_async` | The gateway's own audio cache |
| Speech-to-text | Gateway STT for VOICE messages (`stt.*` config) | Hermes transcribes; the device never runs ASR |
| Transcript echo | Gateway STT echo (`stt_echo_transcripts`) | Forwarded to the device as `transcript` |
| Per-device context | `MessageEvent.channel_prompt` | Stable text: screen size, speaker, action names. The ephemeral prompt is pinned per session, so prompt caching holds |
| Turn boundaries | `on_processing_start` / `on_processing_complete` | Become `turn.start` / `turn.end` |
| Live status | `supports_status_text = True` + `set_status_text()` | Becomes `status` frames |
| Streaming text | `supports_draft_streaming()` + `send_draft()`; `edit_message()` | Used when streaming is enabled; becomes `reply.delta` |
| Final replies | `send()` | Markdown stripped and folded to ASCII for small screens |
| Whole-file TTS | `play_tts()` / `send_voice()` | WAV decoded in Python; other formats through ffmpeg; resampled to the device rate |
| Streaming TTS | `supports_streaming_tts` / `begin_` / `write_` / `finish_` / `abort_streaming_tts` | PCM chunks forwarded as they are generated, for TTS providers Hermes can stream |
| Images from the agent | `send_image_file()` / `send_image()` | Converted to RGB565 to fit the screen (needs Pillow) |
| Confirmations (`/new`, `/undo`, model switches) | `send_slash_confirm()` + `tools.slash_confirm.resolve()` | Shown as a `prompt`; TALK resolves `once`, CANCEL `cancel`. The handler's reply goes back with `send()` |
| Dangerous-command approvals | `_send_exec_approval_prompt(ExecApprovalPrompt)` + `tools.approval.resolve_gateway_approval()` | Shown as a `prompt`; TALK resolves `once`, CANCEL `deny`. On timeout Hermes calls `edit_message()` on the prompt, which withdraws it |
| Pairing | Hermes DM pairing (`gateway/pairing.py`, `hermes pairing approve`; `hermes gadget pair` approves through the same `PairingStore`) | See below |
| Authorization state | `BasePlatformAdapter._is_sender_authorized()` | The runner-installed check; used to mirror approval and revocation to the device |
| Agent tools | `ctx.register_tool(toolset="gadget")` | Part of the implicit `hermes-gadget` toolset; also usable from other chats |
| Tool session context | `gateway.session_context.get_session_env` | `HERMES_SESSION_PLATFORM` / `HERMES_SESSION_CHAT_ID` pick the default device |
| Durable state | `plugins.plugin_storage.plugin_data_dir("gadget")` | Enrolled device keys, pending pairing codes, and firmware staged for updates (`updates/`) |
| Host CLI | `ctx.register_cli_command("gadget", ...)` | `hermes gadget devices / forget / info / pair / update`. `pair` waits for a device to show a code and approves it after asking. `update` stages an image in the plugin's data directory (a file, or with `--latest` the newest release's image for the device's board, checked against the release manifest); the gateway's adapter installs it once the device is online and writes progress back for the command |
| Send and cron targets | `parse_target_ref_fn`, `cron_deliver_env_var` | `gadget:hg-0123456789abcdef` works with send_message and cron delivery; `GADGET_HOME_CHANNEL` supplies a default cron device |

### Scheduled delivery

Set a cron job's `deliver` to `gadget:hg-0123456789abcdef`, replacing the example ID with one from `hermes gadget devices`. The gadget must be paired and online, with the gateway running, to receive the scheduled message.

To use `deliver="gadget"`, configure the gadget home channel. By default, `auto_home: true` makes the first approved device the home channel. An explicit `GADGET_HOME_CHANNEL` device ID in the profile's `.env` overrides that default. A target with an explicit device ID does not require this environment variable.

### Voice round trip

1. **Device:** push-to-talk streams PCM16 to the hub at the microphone rate the device declares in `hello` (24 kHz on ESP32 boards with codec chips, 16 kHz by default otherwise), and the hub assembles a WAV.
2. **Adapter:** caches the WAV with `cache_audio_from_bytes_async`, then calls `handle_message(MessageEvent(VOICE, media_urls=[wav]))`.
3. **Gateway:** `_enrich_message_with_transcription` runs the configured STT provider. The transcript is echoed (device shows "> what you said"), then the agent turn runs.
4. **Reply audio:**
   - When the TTS provider streams, `StreamingTTSConsumer` → `write_streaming_tts` → PCM to the device while text is still being generated.
   - Otherwise whole-file auto-TTS → `play_tts(file)` → decode → resample → paced PCM.
5. **Reply text:** `send(text)` → `reply` frame. Then `turn.end`.

Because a gadget is voice-first, `GadgetAdapter` answers `True` from `_should_auto_tts_for_chat` for devices that declare a speaker. The gateway then reads every reply aloud for that chat, both typed and spoken input. `/voice off` in the device's chat, or `speak_replies: false`, turns it off.

### Pairing

The SDK reuses Hermes's DM pairing instead of inventing its own:

1. When an unknown device connects, the adapter dispatches a harmless `/status` message from it.
2. Hermes's unauthorized-sender path answers with a pairing code. The adapter recognizes the `hermes pairing approve <platform> <CODE>` command in that reply and sends the device a `pairing` frame.
3. The device shows the code. The owner runs `hermes gadget pair` on the Hermes host, which finds the waiting device and approves its code after asking, or runs the `hermes pairing approve` command the device shows.
4. The adapter polls the runner's authorization check every 2 s, so approval reaches the device as `paired` without a reconnect. Revocation (`hermes pairing revoke gadget <id>`) reaches it as `unpaired`.

**Home channel.** Hermes opens each new session on a platform without a home channel with a "type /sethome" notice, which a device without a keyboard can't act on. So when a device is approved and the gadget platform has no home channel (`platforms.gadget.home_channel` or `GADGET_HOME_CHANNEL`), the adapter makes that device the home channel, as `/sethome` would, and saves it to `config.yaml`. Cron results and messages sent to `gadget` then go to that device. Run `/sethome` from another device to move it, or set `auto_home: false` to turn this off.

The pairing code authorizes the chat in Hermes. The device key, enrolled on first contact and proven by HMAC afterwards, keeps another device from impersonating an approved one.

Until Hermes approves a device, its record is *pending*: at most 16 devices may wait at once, at most 4 from one network address, and a pending record expires an hour after the device's last contact (it simply re-enrolls on its next connection). The hub refuses further strangers with an `error` of code `busy`. `hermes gadget devices` marks pending devices with "waiting to pair". `hermes gadget pair --yes` approves without asking only when exactly one device is waiting; with several, name one: `hermes gadget pair --yes <device>`.

### Confirmations

Hermes asks before destructive commands (`/new`, `/undo`), costly model switches and dangerous shell commands. Chat platforms render those as buttons; the gadget renders them as a yes/no `prompt` (see [protocol.md](protocol.md#questions)):

- **Holding CANCEL** sends `session.new`, which the adapter turns into `/new`. Hermes then asks "Confirm /new". The hold already was a deliberate confirmation, so when the confirmation for `new` arrives within 15 s of the device's own request, the adapter resolves it with `once` and forwards the reset reply. A `/new` typed or spoken any other way still asks.
- **Everything else** goes to the device as a question. The adapter keeps only the header and detail of Hermes's message, dropping the choice list and typed-reply instructions meant for chat apps.
- **One at a time:** questions queue per device in arrival order, which matches Hermes's first-in, first-out approval queue. A question asked while the device was offline is shown when it reconnects.
- **Elsewhere:** answering with `/approve` or `/deny` from another chat still works; the device's copy then reports "That question has expired" if pressed.

### Agent tools and Hermes tool search

Hermes keeps the core tool schema narrow. Plugin tools are therefore deferred behind its tool-search bridge (`tool_search` / `tool_describe` / `tool_call`), and their names and descriptions appear in the bridge's manifest. A model invokes `gadget_action` through `tool_call`, and the end-to-end test exercises exactly that. The per-device context names the available actions so the model knows they exist.

## Making replies fast

A voice turn is speech-to-text, the model turn, then text-to-speech. Most of the wait is usually the model, and the settings that matter live in Hermes:

| Setting | Effect |
|---|---|
| `/reasoning low` (or `none`), typed on the device | Session-only reasoning override for the gadget's chat. A deep-reasoning default (`agent.reasoning_effort: high`) can spend 10–20 s thinking about a one-line answer. Other chats keep their setting |
| `streaming.enabled: true` | Streams reply text to messaging platforms as it is generated, so words appear on the device within a second or two. Hermes's per-platform `display.platforms.<name>.streaming` can only narrow this global switch, so on a shared gateway set the global on and switch the other platforms off. The Desktop app streams on its own and is not affected |
| A TTS provider Hermes can stream | Speech starts after the first sentence instead of after the whole reply. While whole-file TTS synthesizes, the reply text waits for it, so a working streaming provider also shows the text sooner. Check the gateway log for `streaming TTS clause failed`; it usually means a bad API key |
| `stt.provider` | Cloud STT takes about 2–3 s for a short clip. Local `faster-whisper` can be quicker on a fast CPU |

## Where the SDK leans on behavior that is not a formal API

`plugin/compat.py` lists every gateway module the adapter imports and every private `BasePlatformAdapter` member it overrides or calls, with the Hermes commit CI tests against. The gateway runs that check before it creates the adapter, so a Hermes that has changed one of them fails with a message naming it, instead of a traceback from inside the gateway. A test keeps the list complete.

These work on current Hermes and are covered by `tests/test_adapter_hermes.py` and `tests/test_gateway_e2e.py`, but they are conventions rather than documented contracts:

1. **Spoken replies by default.** The adapter overrides `BasePlatformAdapter._should_auto_tts_for_chat`, a private method the runner consults. Hermes's own docs endorse overriding private hooks such as `_keep_typing` for platform UX, but this one is not listed.
2. **Pairing trigger and code capture.** Starting pairing takes a synthetic message, and the code is extracted from the human-readable reply by matching the approve command.
3. **Approval detection** polls `_is_sender_authorized` every 2 s, because there is no "pairing approved" event.
4. **Transcript echo detection** matches the echo format: microphone emoji plus quoted text. If the format changes, the transcript is shown as an ordinary reply; nothing breaks.
5. **Self-confirming `/new`** reads `tools.slash_confirm.get_pending()` to check that the pending confirmation is for the `new` command. It relies on the runner registering a confirmation before it calls `send_slash_confirm`, which Hermes does on purpose so that fast button presses can't race it. If that changes, the device simply asks.
6. **Question text** is cut down from Hermes's chat-formatted confirmation by dropping the bullet-list and italic paragraphs. A different layout only means a longer question.

## Suggested upstream changes (optional; nothing here blocks the SDK)

Each would replace a workaround above with a small, generic hook that every platform plugin could use. None of them is specific to gadgets. The first four are open pull requests on Hermes Agent; until they merge, the SDK keeps the workarounds.

1. **Pairing API for adapters:** [NousResearch/hermes-agent#132448](https://github.com/NousResearch/hermes-agent/pull/132448), open.
   - *What:*
     - `BasePlatformAdapter.request_pairing(source) -> PairingOffer | None` returns the code the DM path would send (`code`, `command`, `expires_in`), behind the same gates and limits, without sending anything to the chat.
     - `on_pairing_changed(user_id, approved)` is called once for each approval and revocation, from one watcher in the runner.
   - *Why:* devices, kiosks and any screen-first platform could show codes natively, without synthetic messages, text parsing or polling.
   - *SDK change afterwards:* `on_ready` calls `request_pairing`, and `_watch_pairing` is deleted.
2. **Voice-first platforms:** [NousResearch/hermes-agent#132441](https://github.com/NousResearch/hermes-agent/pull/132441), open.
   - *What:* two adapter class attributes. `speaks_replies_by_default = True` speaks replies without `voice.auto_tts`, and `/voice off` still silences a chat. `supports_voice_replies = False` keeps replies text, replacing the name checks for A2A.
   - *Why:* voice-native platforms (gadgets, phone bridges) want spoken replies by default without overriding a private method.
   - *SDK change afterwards:* the `_should_auto_tts_for_chat` override is removed, and `speak_replies` maps onto the attribute.
3. **Transcript echoes for adapters:** [NousResearch/hermes-agent#132435](https://github.com/NousResearch/hermes-agent/pull/132435), open.
   - *What:* `BasePlatformAdapter.send_transcript_echo(chat_id, transcript, metadata=None)`. By default it makes the same `send()` call as today; an adapter that overrides it gets the raw transcript.
   - *Why:* adapters could render transcripts distinctly without matching localized text.
   - *SDK change afterwards:* the adapter overrides it, and the echo-text matching is deleted.
4. **Direct toolsets for a platform's own sessions:** [NousResearch/hermes-agent#132449](https://github.com/NousResearch/hermes-agent/pull/132449), open.
   - *What:* `PlatformEntry.direct_toolsets`. A toolset listed there is direct only in sessions on that platform.
   - *Why:* a platform-specific tool (here, controlling the device the user is holding) is part of that surface, and a search round trip costs latency on voice turns. Sessions on other platforms keep deferring it, so the core schema stays narrow.
   - *SDK change afterwards:* `register_platform(..., direct_toolsets=("gadget",))`.

5. **Per-platform reasoning default.**
   - *What:* a `platforms.<name>.reasoning_effort`, resolved after the session override and before the per-model and global settings.
   - *Why:* voice devices want fast, shallow turns while desktop chats keep deep reasoning. Today that needs a `/reasoning` per session.
   - *Upstream status (October 2026):* an open pull request, [NousResearch/hermes-agent#65576](https://github.com/NousResearch/hermes-agent/pull/65576), adds `reasoning_effort` to `channel_overrides`, which is per chat (per device). A platform-wide default would be a small follow-up to it rather than a separate change.
   - *SDK change afterwards:* document a `platforms.gadget.reasoning_effort: low` default.

## Running against a profile or a multiplexed gateway

The adapter binds its own port, so give each profile that serves gadgets a different `platforms.gadget.extra.port`.

Device keys and pending pairing codes live in the plugin data directory. It is resolved with `plugin_data_dir("gadget")` when the adapter connects, so it follows whichever Hermes home the adapter is started under. The gateway re-reads `devices.json` before each lookup or change, so `hermes gadget forget` and `hermes gadget pair` from another terminal take effect without a restart.

The agent tools look up the adapter for the session's profile (`HERMES_SESSION_PROFILE`). Multi-profile setups have only been exercised in unit tests so far.
