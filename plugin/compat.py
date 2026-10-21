"""What this plugin needs from Hermes Agent beyond the public plugin API.

The adapter imports a few gateway modules directly and overrides or calls
private members of ``BasePlatformAdapter`` (see "Where the SDK leans on
behavior that is not a formal API" in docs/hermes-integration.md). Hermes does
not promise any of them. This module names them in one place, and ``check()``
runs before the adapter is created so an incompatible Hermes fails with a
message that says what is missing and which Hermes the plugin was tested
against, instead of a traceback from deep inside the gateway.

``TESTED_HERMES_COMMIT`` is the Hermes Agent commit CI verifies the plugin
against (``.github/workflows/hermes.yml`` reads the same value). To move it,
run that workflow by hand against ``main``; when it passes, put the commit it
printed here.
"""

from __future__ import annotations

import importlib
from typing import Callable

TESTED_HERMES_COMMIT = "2eceb0275501b09febdec88d08b80d1fff730cf8"

# Module -> names the plugin imports from it.
IMPORTS: dict[str, tuple[str, ...]] = {
    "agent.i18n": ("get_language", "t"),
    "gateway.config": ("Platform", "PlatformConfig", "HomeChannel", "persist_home_channel"),
    "gateway.pairing": ("PairingStore",),
    "gateway.platforms._shared": ("extra_or_secret", "get_scoped_secret"),
    "gateway.platforms.base": ("AudioFormat", "BasePlatformAdapter", "ExecApprovalPrompt", "SendResult",
                               "StreamingTTSHandle", "cache_audio_from_bytes_async", "cache_image_from_url"),
    "gateway.platforms.event": ("MessageEvent", "MessageType"),
    "gateway.session_context": ("get_session_env",),
    "plugins.plugin_storage": ("plugin_data_dir",),
    "tools.slash_confirm": ("get_pending", "resolve"),
    "tools.approval": ("resolve_gateway_approval",),
    "hermes_cli.config": ("load_config", "set_config_value"),
    "hermes_cli.setup": ("print_header", "print_info", "print_success", "print_warning", "prompt"),
}

# Private members of BasePlatformAdapter the adapter overrides or calls.
ADAPTER_HOOKS: tuple[str, ...] = (
    "_should_auto_tts_for_chat",
    "_auto_tts_disabled_chats",
    "_send_exec_approval_prompt",
    "_is_sender_authorized",
    "_set_fatal_error",
    "_mark_connected",
    "_mark_disconnected",
    "_wire_plugin_handlers",
)


def missing(resolve: Callable[[str], object] = importlib.import_module) -> list[str]:
    """What this Hermes lacks, as ``module``, ``module.name`` or ``BasePlatformAdapter.member``."""
    gaps: list[str] = []
    base = None
    for module_name, names in IMPORTS.items():
        try:
            module = resolve(module_name)
        except ImportError as exc:
            gaps.append(f"{module_name} ({exc})")
            continue
        for name in names:
            if not hasattr(module, name):
                gaps.append(f"{module_name}.{name}")
        if module_name == "gateway.platforms.base":
            base = getattr(module, "BasePlatformAdapter", None)
    if base is not None:
        # _auto_tts_disabled_chats is set per instance; the rest are class members.
        gaps.extend(f"BasePlatformAdapter.{hook}" for hook in ADAPTER_HOOKS
                    if hook != "_auto_tts_disabled_chats" and not hasattr(base, hook))
    return gaps


def check(resolve: Callable[[str], object] = importlib.import_module) -> None:
    """Raise with a clear message when this Hermes lacks something the plugin needs."""
    gaps = missing(resolve)
    if gaps:
        raise RuntimeError(
            "This Hermes Agent lacks what the Hermes Gadget plugin relies on: " + ", ".join(gaps)
            + f". The plugin was tested against Hermes commit {TESTED_HERMES_COMMIT[:12]}; "
            "update the plugin (hermes plugins install) or run that Hermes.")
