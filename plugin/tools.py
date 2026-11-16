"""Agent tools for gadgets: inspect devices, show content on a screen, ask the
user to pick an answer there, invoke device-declared actions.

Schemas are static (prompt caching); what a device can do is discovered at run
time through ``gadget_devices`` and the per-device context Hermes receives with
each message. Tools are available in any session served by the gateway process
that hosts the gadget adapter, so a user can also drive a gadget from another
chat ("show the grocery list on the kitchen gadget").
"""

from __future__ import annotations

import asyncio
import json
import threading
from typing import Any

from . import playback, runtime, textfmt
from .hub import MAX_CARD_ITEMS, MAX_CHOICE_OPTIONS

TOOLSET = "gadget"
CALL_TIMEOUT_S = 20.0
ASK_DEFAULT_S = 60    # how long gadget_ask waits for an answer, unless told otherwise
ASK_MAX_S = 300       # ...and at most: Hermes's own approval timeout
QR_MAX_BYTES = 287    # the longest text any gadget draws as a code
ROW_INDENT_COLS = 2   # a list row or an answer starts after a bullet: that much of text_cols is not its own

_DEVICE_PARAM = {
    "type": "string",
    "description": "Gadget name or device id. Omit to use the gadget the user is talking through "
                   "(or the only connected gadget).",
}

DEVICES_SCHEMA = {
    "name": "gadget_devices",
    "description": "List connected Hermes gadgets (small hardware devices) with their screen, audio, "
                   "available device actions and latest sensor readings.",
    "parameters": {"type": "object", "properties": {}, "required": []},
}

DISPLAY_SCHEMA = {
    "name": "gadget_display",
    "description": "Show a short card on a gadget's screen: a title and text, and optionally a list (items), "
                   "a progress bar (progress) or a QR code the user can scan with a phone (qr). To update a "
                   "card that is still on screen, such as a progress bar, send it again with the same id. "
                   "For timers use the device's timer.* actions through gadget_action when it lists them. "
                   "Screens are small: keep text brief and plain (no Markdown). While the screen is off "
                   "the card waits and appears when it lights up.",
    "parameters": {
        "type": "object",
        "properties": {
            "text": {"type": "string", "description": "Body text to show. With qr it is the caption under the code."},
            "title": {"type": "string", "description": "Optional short title."},
            "items": {"type": "array", "items": {"type": "string"},
                      "description": "Optional list rows, one short line each (up to 12), e.g. a shopping list."},
            "progress": {"type": "integer", "description": "Optional progress bar, 0 to 100."},
            "qr": {"type": "string",
                   "description": "Optional text to show as a QR code, e.g. a link (short: small screens fit "
                                  "about 40 characters). The card then shows the code, not the items."},
            "id": {"type": "string",
                   "description": "Optional name for this card. A later card with the same id replaces it in place."},
            "seconds": {"type": "integer", "description": "How long to show it; 0 keeps it until dismissed. Default 20."},
            "device": _DEVICE_PARAM,
        },
        "required": [],
    },
}

ASK_SCHEMA = {
    "name": "gadget_ask",
    "description": "Ask the user to pick one of a few answers on a gadget's screen and wait for the choice. "
                   "Use it for a quick decision between 2 to 6 short options; the user taps one or picks it "
                   "with the buttons, or cancels. Returns the chosen option. Not for yes/no confirmations of "
                   "risky actions: Hermes asks those itself.",
    "parameters": {
        "type": "object",
        "properties": {
            "question": {"type": "string", "description": "The question, a few words: it is the screen's title."},
            "options": {"type": "array", "items": {"type": "string"}, "minItems": 2, "maxItems": 6,
                        "description": "2 to 6 short answers, one line each."},
            "seconds": {"type": "integer",
                        "description": "How long to wait for the answer, up to 300. Default 60."},
            "device": _DEVICE_PARAM,
        },
        "required": ["question", "options"],
    },
}

ACTION_SCHEMA = {
    "name": "gadget_action",
    "description": "Invoke an action a gadget exposes (for example an LED, relay, buzzer, volume control or camera capture). "
                   "face.set, where a device lists it, gives the face on its screen an expression "
                   "(happy, confused, alert, sleepy; neutral clears it): use it to show a mood, sparingly. "
                   "Use gadget_devices first if you do not know the action names and parameters.",
    "parameters": {
        "type": "object",
        "properties": {
            "action": {"type": "string", "description": "Action name as listed by the device."},
            "args": {"type": "object", "description": "Arguments matching the action's parameter schema."},
            "device": _DEVICE_PARAM,
        },
        "required": ["action"],
    },
}

PLAY_SCHEMA = {
    "name": "gadget_play",
    "description": "Play audio on a gadget's speaker: a sound file on the Hermes computer, or the http(s) URL "
                   "of an audio file, a podcast episode or an internet radio stream. It starts once the gadget "
                   "has finished speaking the current reply and plays in the background until it ends, "
                   "is stopped (stop: true, or CANCEL on the gadget), or the user talks to the gadget. "
                   "For spoken text, reply as usual; this tool is for other audio.",
    "parameters": {
        "type": "object",
        "properties": {
            "source": {"type": "string",
                       "description": "A file path on the Hermes computer, inside a folder playback is allowed "
                                      "from, or an http(s) URL of the audio itself (not a web page)."},
            "stop": {"type": "boolean", "description": "Stop what gadget_play is playing instead of starting something."},
            "volume": {"type": "integer",
                       "description": "Loudness of this playback, 1-100 percent of the source's level. Default 100. "
                                      "The speaker's own volume is the device's speaker.volume action."},
            "title": {"type": "string", "description": "Optional short name shown on the screen."},
            "device": _DEVICE_PARAM,
        },
        "required": [],
    },
}


def _error(message: str) -> str:
    return json.dumps({"success": False, "error": message})


def _adapter():
    from gateway.session_context import get_session_env

    return runtime.active(get_session_env("HERMES_SESSION_PROFILE") or None)


def _run(adapter, coro, timeout: float | None = None) -> Any:
    loop = adapter.loop
    if loop is None or loop.is_closed():
        coro.close()
        raise RuntimeError("gadget hub is not running")
    if getattr(loop, "_thread_id", None) == threading.get_ident():
        coro.close()
        raise RuntimeError("gadget tools cannot block the gateway event loop")
    future = asyncio.run_coroutine_threadsafe(coro, loop)
    try:
        return future.result(CALL_TIMEOUT_S if timeout is None else timeout)
    except BaseException:
        # The caller reports a failure: the work must not finish behind its back, and a choice left
        # on screen is withdrawn.
        future.cancel()
        raise


def _resolve(adapter, ref: str | None):
    from gateway.session_context import get_session_env

    hub = adapter.hub
    if hub is None:
        return None, "gadget hub is not running"
    if ref:
        session = hub.find(ref)
        if session is None:
            known = ", ".join(f"{s.name} ({s.device_id})" for s in hub.sessions.values()) or "none connected"
            return None, f"no connected gadget matches {ref!r}; connected: {known}"
        return session, None
    if get_session_env("HERMES_SESSION_PLATFORM") == "gadget":
        # The device behind the chat, a virtual <device>~<profile> one included (profile_mode: virtual).
        session = hub.get(adapter._device_id(get_session_env("HERMES_SESSION_CHAT_ID") or ""))
        if session is not None:
            return session, None
    sessions = list(hub.sessions.values())
    if len(sessions) == 1:
        return sessions[0], None
    if not sessions:
        return None, "no gadget is connected"
    return None, "several gadgets are connected; pass device: " + ", ".join(s.name for s in sessions)


def _row_cols(session) -> int | None:
    """Characters a list row or an answer has on the device's line, after its bullet."""
    cols = session.display.get("text_cols")
    return cols - ROW_INDENT_COLS if isinstance(cols, int) and cols > 4 + ROW_INDENT_COLS else None


def _usable(session) -> str | None:
    return None if session.paired else f"{session.name} is waiting for pairing approval"


def gadget_devices(args: dict, **_kw) -> str:
    adapter = _adapter()
    if adapter is None or adapter.hub is None:
        return _error("gadget hub is not running in this process")
    online = {s.device_id: s.describe() for s in adapter.hub.sessions.values()}
    offline = []
    for device_id, rec in adapter.hub.store.devices().items():
        if device_id not in online:
            offline.append({"device_id": device_id, "name": rec.get("name") or device_id,
                            "board": rec.get("board"), "last_seen": rec.get("last_seen")})
    return json.dumps({"success": True, "connected": list(online.values()), "offline": offline})


def gadget_display(args: dict, **_kw) -> str:
    adapter = _adapter()
    if adapter is None:
        return _error("gadget hub is not running in this process")
    session, err = _resolve(adapter, args.get("device"))
    if err:
        return _error(err)
    if (err := _usable(session)):
        return _error(err)
    text = textfmt.for_device(str(args.get("text") or ""), session.charset)
    title = textfmt.for_device(str(args.get("title") or ""), session.charset)
    cols = _row_cols(session)
    items = args.get("items")
    if items is not None and not isinstance(items, list):
        return _error("items must be a list of short strings")
    items = [row for row in (textfmt.line(str(item), session.charset, cols) for item in items or []) if row]
    progress = args.get("progress")
    if progress is not None:
        try:
            progress = max(0, min(100, int(progress)))
        except (TypeError, ValueError):
            return _error("progress must be a number from 0 to 100")
    qr = str(args.get("qr") or "")  # as given, spaces too: a code must hold exactly what was asked for
    if not qr.strip():
        qr = ""
    if len(qr.encode()) > QR_MAX_BYTES:
        return _error(f"qr holds at most {QR_MAX_BYTES} bytes")
    if not (text or title or items or progress is not None or qr):
        return _error("text is required")
    seconds = args.get("seconds", 20)
    try:
        seconds = max(0, int(seconds))
    except (TypeError, ValueError):
        seconds = 20
    extras = {"items": items[:MAX_CARD_ITEMS], "progress": progress, "qr": qr}
    try:
        shown = _run(adapter, session.show_card(
            title, text, seconds, card_id=str(args.get("id") or "").strip()[:64], **extras))
    except Exception as exc:
        return _error(f"could not reach {session.name}: {exc}")
    result = {"success": True, "device": session.name, "shown_for_s": seconds or "until dismissed"}
    shown_as = session.card_shown_as(**extras)
    if shown_as == "text":
        result["note"] = f"{session.name} draws plain cards only: the list, progress and code are shown as text"
    elif shown_as == "qr_as_text":
        result["note"] = (f"the code is too long for {session.name}'s screen and is shown as text; "
                          f"it draws codes of up to {session.card_qr_bytes} bytes")
    kept = session.card_items or MAX_CARD_ITEMS  # a plain card lists them all, up to the hub's limit
    if len(items) > kept:
        result["items_shown"] = kept
    if shown is False:
        result["held"] = ("the screen is off; the card is shown when it lights up, "
                          "for what is left of its time, unless a newer card or image replaces it")
    return json.dumps(result)


def gadget_ask(args: dict, **_kw) -> str:
    adapter = _adapter()
    if adapter is None:
        return _error("gadget hub is not running in this process")
    session, err = _resolve(adapter, args.get("device"))
    if err:
        return _error(err)
    if (err := _usable(session)):
        return _error(err)
    given = args.get("options")
    if not isinstance(given, list) or not 2 <= len(given) <= MAX_CHOICE_OPTIONS:
        return _error(f"options must be a list of 2 to {MAX_CHOICE_OPTIONS} short answers")
    options = [textfmt.line(str(option), session.charset, _row_cols(session)) for option in given]
    if not all(options):
        return _error("every option needs text the gadget's screen can draw")
    title = textfmt.line(str(args.get("question") or ""), session.charset)
    # What the user saw, where it is not what the agent wrote: the turn waits on this screen alone.
    seen: dict = {}
    notes = []
    if (box := session.choice_title_box):
        title, cut = textfmt.clip(title, *box)
        if cut:
            seen["question_shown"] = title
            notes.append("the question was cut to fit the screen; question_shown is what the user saw")
    if options != [textfmt.line(str(option), session.charset) for option in given]:
        seen["options_shown"] = options
        notes.append("some options were cut to fit the screen; options_shown is what the user saw")
    if notes:
        seen["note"] = "; ".join(notes)
    try:
        seconds = max(5, min(ASK_MAX_S, int(args.get("seconds", ASK_DEFAULT_S))))
    except (TypeError, ValueError):
        seconds = ASK_DEFAULT_S
    limit = session.choice_options
    if len(options) > limit:
        # No list to pick from on this screen: the options go up as a plain card instead.
        if limit:
            return _error(f"{session.name} takes at most {limit} options")
        body = "\n".join(f"{n}. {option}" for n, option in enumerate(options, 1))
        try:
            _run(adapter, session.show_card(title, body, seconds))
        except Exception as exc:
            return _error(f"could not reach {session.name}: {exc}")
        return json.dumps({"success": False, "device": session.name, "shown_as_card": True,
                           "error": f"{session.name} cannot take a choice on its screen. The options are "
                                    "shown there as a numbered card: ask the user to say their answer."})
    try:
        index = _run(adapter, session.ask_choice(title, options, timeout=seconds), timeout=seconds + 5)
    except Exception as exc:
        return _error(str(exc) or f"{session.name} did not answer")
    if index is None:
        return json.dumps({"success": True, "device": session.name, "cancelled": True, **seen})
    return json.dumps({"success": True, "device": session.name, "answer": str(given[index]), "index": index, **seen})


def gadget_action(args: dict, **_kw) -> str:
    adapter = _adapter()
    if adapter is None:
        return _error("gadget hub is not running in this process")
    session, err = _resolve(adapter, args.get("device"))
    if err:
        return _error(err)
    if session is None:
        return _error("no connected gadget is available")
    if (err := _usable(session)):
        return _error(err)
    action = str(args.get("action") or "").strip()
    if not action:
        return _error("action is required")
    call_args = args.get("args") or {}
    if not isinstance(call_args, dict):
        return _error("args must be an object")
    try:
        result = _run(adapter, session.invoke_action(action, call_args, timeout=CALL_TIMEOUT_S - 2))
    except Exception as exc:
        return _error(str(exc))
    if action == "camera.capture":
        try:
            from .camera_capture import save_camera_capture

            capture = save_camera_capture(result, session.device_id)
        except Exception as exc:
            return _error(f"could not save the camera capture: {exc}")
        return json.dumps({"success": True, "device": session.name, "action": action, **capture})
    return json.dumps({"success": True, "device": session.name, "action": action, "result": result})


def _play_roots(extra: dict) -> list:
    """Where gadget_play reads local files: Hermes's audio cache (its own text-to-speech and
    received audio) and the folders ``play_dirs`` names. Nothing else on the host is opened."""
    from gateway.platforms.base import get_audio_cache_dir

    return [get_audio_cache_dir(), *playback.play_dirs(extra)]


def gadget_play(args: dict, **_kw) -> str:
    adapter = _adapter()
    if adapter is None:
        return _error("gadget hub is not running in this process")
    session, err = _resolve(adapter, args.get("device"))
    if err:
        return _error(err)
    if (err := _usable(session)):
        return _error(err)
    try:
        if args.get("stop"):
            stopped = _run(adapter, playback.stop(session))
            return json.dumps({"success": True, "device": session.name, "stopped": bool(stopped)})
        if not session.has_speaker:
            return _error(f"{session.name} has no speaker")
        source = str(args.get("source") or "").strip()
        if not source:
            return _error("source is required: a file path or an http(s) URL (or stop: true)")
        try:
            volume = int(args.get("volume", 100))
        except (TypeError, ValueError):
            volume = 100
        extra = getattr(getattr(adapter, "config", None), "extra", None) or {}
        max_s = playback.max_seconds(extra)
        # Fetching and decoding start here, in the tool's own thread, so a source that cannot be
        # played is the tool's error and not a silent device.
        opened = playback.open_source(source, session.speaker_rate, roots=_play_roots(extra),
                                      fetch=playback.hermes_fetch)
        title = textfmt.for_device(str(args.get("title") or ""), session.charset) or playback.title_of(source)
        try:
            _run(adapter, playback.play(
                session, opened, title=title[:40], volume=volume, max_s=max_s,
                idle=lambda deadline: adapter._device_idle(session.device_id, deadline)))
        except BaseException:
            opened.close()
            raise
    except playback.PlaybackError as exc:
        return _error(str(exc))
    except Exception as exc:
        return _error(f"could not reach {session.name}: {exc}")
    return json.dumps({
        "success": True, "device": session.name, "playing": title[:40],
        "starts": "when the gadget has finished the current reply",
        "stops": f"at the end, after {max_s / 60:g} minutes, on stop: true, or when the gadget is used"})


def available() -> bool:
    """Reachability: tools work only in the process hosting the gadget hub."""
    return runtime.any_active()


def register_tools(ctx) -> None:
    for schema, handler, emoji in (
        (DEVICES_SCHEMA, gadget_devices, "📟"),
        (DISPLAY_SCHEMA, gadget_display, "🖥️"),
        (ASK_SCHEMA, gadget_ask, "🔘"),
        (ACTION_SCHEMA, gadget_action, "🎛️"),
        (PLAY_SCHEMA, gadget_play, "🔊"),
    ):
        ctx.register_tool(
            name=schema["name"], toolset=TOOLSET, schema=schema, handler=handler,
            check_fn=available, description=schema["description"], emoji=emoji)
