"""Command-line entry points for a persistent Linux gadget."""

import json
import os
import signal
import sys
import threading
from pathlib import Path


def add_parser(sub) -> None:
    parser = sub.add_parser("linux", help="Run and control a Linux gadget")
    parser.add_argument("--state-dir", type=Path,
                        default=Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "hermes-gadget")
    commands = parser.add_subparsers(dest="linux_command", required=True)
    service = commands.add_parser("run", help="Run the persistent device service")
    service.add_argument("--config", type=Path, required=True)
    commands.add_parser("audio-devices", help="List available input and output devices")
    audio_check = commands.add_parser("audio-check", help="Record a short clip and play it back locally")
    audio_check.add_argument("--config", type=Path, required=True)
    commands.add_parser("status", help="Show connection, pairing code and service uptime")
    commands.add_parser("messages", help="Show the last 20 replies and notices")
    send = commands.add_parser("send", help="Send a text message to Hermes")
    send.add_argument("text")
    event = commands.add_parser("event", help="Report a device event")
    event.add_argument("name")
    event.add_argument("--data", default="{}", help="JSON object")
    event.add_argument("--notify", action="store_true", help="Ask Hermes to handle the event")
    button = commands.add_parser("button", help="Press or release a device control")
    button.add_argument("button", choices=("talk", "cancel", "up", "down"))
    button.add_argument("state", choices=("press", "release"))
    parser.set_defaults(func=main)


def main(args) -> int:
    from .client import load_config
    from .control import request, run

    if sys.platform != "linux":
        print("The device service requires Linux.", file=sys.stderr)
        return 1
    try:
        if args.linux_command == "audio-devices":
            from .audio import backend

            sd = backend()
            try:
                print(sd.query_devices())
            except sd.PortAudioError as exc:
                raise RuntimeError(str(exc)) from exc
            return 0
        if args.linux_command == "audio-check":
            from .audio import backend, check_loopback

            sd = backend()
            try:
                check_loopback(load_config(args.config).get("audio", {}))
            except sd.PortAudioError as exc:
                raise RuntimeError(str(exc)) from exc
            return 0
        if args.linux_command == "run":
            config = load_config(args.config)
            stop = threading.Event()
            previous = {sig: signal.signal(sig, lambda *_: stop.set()) for sig in (signal.SIGINT, signal.SIGTERM)}
            try:
                run(config, args.state_dir, stop)
            finally:
                for sig, handler in previous.items():
                    signal.signal(sig, handler)
            return 0
        message = {"command": args.linux_command}
        if args.linux_command == "send":
            message["text"] = args.text
        elif args.linux_command == "event":
            message.update(name=args.name, data=json.loads(args.data), notify=args.notify)
        elif args.linux_command == "button":
            message.update(button=args.button, pressed=args.state == "press")
        result = request(args.state_dir, message)
        print(json.dumps(result, indent=2))
        return 1 if "error" in result else 0
    except (OSError, ValueError, RuntimeError, ImportError) as exc:
        print(f"Linux gadget: {exc}", file=sys.stderr)
        return 1
