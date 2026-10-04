"""Hermes Gadget's step in ``hermes gateway setup``.

The wizard runs it when the user picks Hermes Gadget (``register_platform(setup_fn=...)``) and
offers to restart the gateway afterwards, so this only enables the platform, asks for the port
and points at the browser installer and ``hermes gadget pair``.
"""

from __future__ import annotations

from . import cli

DEFAULT_PORT = 8765


def _ui():
    """Hermes's setup prompts, as the built-in platforms use them; plain input and print if they move."""
    try:
        from hermes_cli.setup import print_header, print_info, print_success, print_warning, prompt

        return print_header, print_info, print_success, print_warning, prompt
    except ImportError:
        pass

    def prompt(question: str, default: str | None = None, password: bool = False) -> str:
        answer = input(f"{question}{f' [{default}]' if default else ''}: ").strip()
        return answer or (default or "")

    def say(text: str) -> None:
        print(f"  {text}")

    return (lambda title: print(f"\n  {title}"), say, say, lambda text: say(f"Warning: {text}"), prompt)


def _set_config(key: str, value: str) -> bool:
    """``hermes config set``: Hermes's own config writer, so comments and other settings survive."""
    try:
        from hermes_cli.config import set_config_value
    except ImportError:
        return False
    set_config_value(key, value)
    return True


def interactive_setup() -> None:
    print_header, print_info, print_success, print_warning, prompt = _ui()
    print_header("Hermes Gadget")
    print_info("Small ESP32 devices with a screen, microphone and speaker that talk to this Hermes over your network.")

    extra = cli._gadget_extra()
    current = int(extra.get("port") or DEFAULT_PORT)
    answer = prompt("Port devices connect to", default=str(current))
    try:
        port = int(answer)
        if not 0 < port < 65536:
            raise ValueError
    except ValueError:
        print_warning(f"{answer!r} isn't a port number; keeping {current}")
        port = current

    settings = [("platforms.gadget.enabled", "true")]
    if port != current:
        settings.append(("platforms.gadget.extra.port", str(port)))
    if all(_set_config(key, value) for key, value in settings):
        print_success("Hermes Gadget is enabled")
    else:
        print_warning("Couldn't update config.yaml from here. Run: hermes config set platforms.gadget.enabled true")

    url = cli.device_url({**extra, "port": port})
    print_info(f"Devices connect to {url}")
    print_info("Once the gateway has restarted, set up a device from Chrome or Edge:")
    print_info(f"  {cli.installer_link(url)}")
    print_info("When the device shows a pairing code, approve it with: hermes gadget pair")
