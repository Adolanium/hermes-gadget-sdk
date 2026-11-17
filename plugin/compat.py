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
from typing import Callable, Optional

TESTED_HERMES_COMMIT = "2eceb0275501b09febdec88d08b80d1fff730cf8"

# Module -> names the plugin imports from it.
IMPORTS: dict[str, tuple[str, ...]] = {
    "agent.i18n": ("get_language", "t"),
    "gateway.config": ("Platform", "PlatformConfig", "HomeChannel", "persist_home_channel"),
    "gateway.pairing": ("PairingStore",),
    "gateway.platforms._shared": ("extra_or_secret", "get_scoped_secret"),
    "gateway.platforms.base": ("AudioFormat", "BasePlatformAdapter", "ExecApprovalPrompt", "SendResult",
                               "StreamingTTSHandle", "cache_audio_from_bytes_async", "cache_image_from_url",
                               "get_audio_cache_dir"),
    "gateway.platforms.event": ("MessageEvent", "MessageType"),
    "gateway.session_context": ("get_session_env",),
    "plugins.plugin_storage": ("plugin_data_dir",),
    "tools.slash_confirm": ("get_pending", "resolve"),
    "tools.approval": ("resolve_gateway_approval",),
    "tools.url_safety": ("create_ssrf_safe_client", "is_safe_url", "redirect_target_from_response"),
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


# Hermes's limit for a command approval (approvals.timeout, 300 s by default). It is optional: a
# Hermes without the function, or the standalone development server, gets the default.
APPROVAL_TIMEOUT_DEFAULT_S = 300


def approval_timeout_s() -> int:
    """How long Hermes waits for an answer to a command approval, in seconds."""
    try:
        from gateway.platforms.base_exec_approval import approval_timeout_seconds

        return int(approval_timeout_seconds())
    except Exception:
        return APPROVAL_TIMEOUT_DEFAULT_S


def resolve_approval(session_key: str, choice: str, request_id: Optional[str]) -> int:
    """Answer the command approval a device question was asked for, and only that one.

    With a request id, Hermes's card path resolves exactly that request: a YES for a question whose
    request already settled resolves nothing instead of the next one queued in the session, and a
    request Hermes keeps Deny-only refuses it. A Hermes without the card path (or a question without a
    request id) gets the older answer, which resolves the session's oldest open approval."""
    from tools import approval

    card = getattr(approval, "resolve_gateway_approval_card", None)
    if request_id and card is not None:
        return card(session_key, choice, request_id)
    return approval.resolve_gateway_approval(session_key, choice)


def approval_deny_only(session_key: str, request_id: Optional[str]) -> None:
    """Keep a request Deny-only in Hermes when the device cannot show its whole command: Hermes lifted
    the mark for its own card text, but the device's screen is narrower. Optional: older Hermes lacks it."""
    if not request_id:
        return
    try:
        from tools.approval import restrict_gateway_approval_card_to_deny
    except ImportError:
        return
    restrict_gateway_approval_card_to_deny(session_key, request_id)
