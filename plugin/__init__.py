"""Hermes Gadget — a Hermes platform plugin that lets small hardware devices
(ESP32 boards with a screen, microphone and speaker) talk to Hermes.

Import-light on purpose: the adapter (and the gateway modules it needs) load
only when the gateway asks for the platform.
"""

from __future__ import annotations

PLATFORM_HINT = (
    "You are talking through a Hermes Gadget: a small physical device with a tiny screen "
    "(roughly 25-40 characters per line, under ten lines) that often reads your replies aloud. "
    "The user usually speaks to it, so their message is a speech transcript and may contain "
    "recognition errors. Reply in plain conversational text: no Markdown, tables, code blocks "
    "or links unless asked. Keep it to one to three short sentences unless the user wants more."
)


def check_requirements() -> bool:
    try:
        import websockets.asyncio.server  # noqa: F401
    except ImportError:
        return False
    return True


def _make_adapter(config):
    from .adapter import GadgetAdapter

    return GadgetAdapter(config)


def _is_connected(config) -> bool:
    # No credential is required: enabling the platform in config.yaml is the opt-in.
    return True


def _parse_target(raw: str):
    """``gadget:<device-id>`` targets for send_message and cron delivery."""
    from .protocol import DEVICE_ID_RE

    ref = (raw or "").strip().lower()
    return (ref, None) if DEVICE_ID_RE.match(ref) else None


def register(ctx) -> None:
    from . import cli
    from .tools import register_tools

    ctx.register_platform(
        name="gadget",
        label="Hermes Gadget",
        adapter_factory=_make_adapter,
        check_fn=check_requirements,
        is_connected=_is_connected,
        parse_target_ref_fn=_parse_target,
        install_hint="websockets is a core Hermes dependency; reinstall Hermes if it is missing",
        allowed_users_env="GADGET_ALLOWED_USERS",
        allow_all_env="GADGET_ALLOW_ALL_USERS",
        max_message_length=4000,
        emoji="📟",
        pii_safe=True,
        allow_update_command=False,
        platform_hint=PLATFORM_HINT,
    )
    register_tools(ctx)
    ctx.register_cli_command(
        name="gadget",
        help="Manage Hermes gadgets (ESP32 devices)",
        setup_fn=cli.setup_argparse,
        handler_fn=cli.handle,
    )
