"""Agent tools for gadgets: inspect devices, show content on a screen, invoke
device-declared actions.

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

from . import runtime, textfmt

TOOLSET = "gadget"
CALL_TIMEOUT_S = 20.0

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
    "description": "Show a short card (title and text) on a gadget's screen, e.g. a list or note. "
                   "For timers use the device's timer.* actions through gadget_action when it lists them. "
                   "Screens are small: keep text brief and plain (no Markdown). While the screen is off "
                   "the card waits and appears when it lights up.",
    "parameters": {
        "type": "object",
        "properties": {
            "text": {"type": "string", "description": "Body text to show."},
            "title": {"type": "string", "description": "Optional short title."},
            "seconds": {"type": "integer", "description": "How long to show it; 0 keeps it until dismissed. Default 20."},
            "device": _DEVICE_PARAM,
        },
        "required": ["text"],
    },
}

ACTION_SCHEMA = {
    "name": "gadget_action",
    "description": "Invoke an action a gadget exposes (for example an LED, relay, buzzer, volume control or camera capture). "
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


WIDGET_SCHEMA = {
    "name": "gadget_widget",
    "description": "Set or clear a full-screen widget on gadgets' home screens: glanceable data the user pages "
                   "through, which stays until cleared or its ttl runs out. Kinds: 'value' (a big value with a "
                   "unit, e.g. a temperature), 'progress' (a percent bar), 'list' (up to 4 short lines, e.g. the "
                   "next calendar events) and 'headline' (a title and a second line, e.g. weather or now "
                   "playing). Setting a name again replaces that widget. Unlike gadget_display it does not "
                   "take over the screen. Keep text brief and plain (no Markdown).",
    "parameters": {
        "type": "object",
        "properties": {
            "action": {"type": "string", "enum": ["set", "clear", "list"], "description": "Default: set."},
            "name": {"type": "string", "description": "The widget's name, e.g. 'weather'. For clear, omit to "
                                                      "remove every widget of that device scope."},
            "kind": {"type": "string", "enum": ["value", "progress", "list", "headline"]},
            "label": {"type": "string", "description": "Small heading above the content. Default: the name."},
            "value": {"type": "string", "description": "kind=value: the big value, e.g. '21.5'."},
            "unit": {"type": "string", "description": "kind=value: its unit, e.g. 'C'."},
            "percent": {"type": "number", "description": "kind=progress: 0 to 100."},
            "items": {"type": "array", "items": {"type": "string"}, "description": "kind=list: up to 4 lines."},
            "title": {"type": "string", "description": "kind=headline: the first line."},
            "text": {"type": "string", "description": "kind=headline: the second line."},
            "state": {"type": "string", "enum": ["ok", "warn", "bad"], "description": "Colours it green, yellow or red."},
            "ttl_s": {"type": "number", "description": "Seconds until it disappears unless set again. Give one "
                                                       "for data that goes stale."},
            "device": {"type": "string", "description": "Gadget name or device id. Omit for every gadget."},
        },
        "required": [],
    },
}


def _error(message: str) -> str:
    return json.dumps({"success": False, "error": message})


def _adapter():
    from gateway.session_context import get_session_env

    return runtime.active(get_session_env("HERMES_SESSION_PROFILE") or None)


def _run(adapter, coro) -> Any:
    loop = adapter.loop
    if loop is None or loop.is_closed():
        coro.close()
        raise RuntimeError("gadget hub is not running")
    if getattr(loop, "_thread_id", None) == threading.get_ident():
        coro.close()
        raise RuntimeError("gadget tools cannot block the gateway event loop")
    return asyncio.run_coroutine_threadsafe(coro, loop).result(CALL_TIMEOUT_S)


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
    if not text and not title:
        return _error("text is required")
    seconds = args.get("seconds", 20)
    try:
        seconds = max(0, int(seconds))
    except (TypeError, ValueError):
        seconds = 20
    try:
        shown = _run(adapter, session.show_card(title, text, seconds))
    except Exception as exc:
        return _error(f"could not reach {session.name}: {exc}")
    result = {"success": True, "device": session.name, "shown_for_s": seconds or "until dismissed"}
    if shown is False:
        result["held"] = ("the screen is off; the card is shown when it lights up, "
                          "for what is left of its time, unless a newer card or image replaces it")
    return json.dumps(result)


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


def gadget_widget(args: dict, **_kw) -> str:
    from . import dashboard

    adapter = _adapter()
    if adapter is None or adapter.hub is None:
        return _error("gadget hub is not running in this process")
    store = getattr(adapter, "_tiles", None)
    if store is None:
        return _error("the gadget dashboard is turned off (platforms.gadget.extra.dashboard)")
    device, where = None, "every gadget"
    ref = str(args.get("device") or "").strip()
    if ref:  # an enrolled gadget, connected or not: it gets the widget when it next connects
        known = adapter.hub.store.devices()
        matches = [d for d, rec in known.items() if d == ref or (rec.get("name") or "").lower() == ref.lower()]
        if len(matches) != 1:
            names = ", ".join(f"{rec.get('name') or d} ({d})" for d, rec in known.items()) or "none"
            return _error(f"no single gadget matches {ref!r}; known: {names}")
        device, where = matches[0], known[matches[0]].get("name") or matches[0]
    action = str(args.get("action") or "set")
    name = str(args.get("name") or "").strip()
    if action == "list":
        try:
            return json.dumps({"success": True, "widgets": store.widgets(device), "device": where})
        except OSError as exc:
            return _error(f"could not read the widgets: {exc}")
    if action == "clear":
        try:
            cleared = store.clear_widgets(name or None, device=device)
        except OSError as exc:
            return _error(f"could not update the widgets: {exc}")
        adapter._board_changed()  # devices hear of it now rather than at the next look at the file
        return json.dumps({"success": True, "cleared": cleared, "device": where})
    if action != "set":
        return _error("action must be set, clear or list")
    try:
        widget = store.set_widget(
            name, str(args.get("kind") or ""), device=device, label=args.get("label"), value=args.get("value"),
            unit=args.get("unit"), percent=args.get("percent"), items=args.get("items"), title=args.get("title"),
            text=args.get("text"), state=args.get("state") or None, ttl_s=args.get("ttl_s"))
    except ValueError as exc:
        return _error(str(exc))
    except OSError as exc:  # the file is locked or unwritable: say so instead of failing the tool call
        return _error(f"could not save the widget: {exc}")
    adapter._board_changed()
    drawing = [s.name for s in adapter.hub.sessions.values()
               if s.paired and s.dashboard_version and (device is None or s.device_id == device)]
    return json.dumps({"success": True, "device": where, "widget": name, "shows": dashboard.widget_summary(widget),
                       "connected_gadgets_showing_it": drawing})


def available() -> bool:
    """Reachability: tools work only in the process hosting the gadget hub."""
    return runtime.any_active()


def register_tools(ctx) -> None:
    for schema, handler, emoji in (
        (DEVICES_SCHEMA, gadget_devices, "📟"),
        (DISPLAY_SCHEMA, gadget_display, "🖥️"),
        (ACTION_SCHEMA, gadget_action, "🎛️"),
        (WIDGET_SCHEMA, gadget_widget, "🧩"),
    ):
        ctx.register_tool(
            name=schema["name"], toolset=TOOLSET, schema=schema, handler=handler,
            check_fn=available, description=schema["description"], emoji=emoji)
