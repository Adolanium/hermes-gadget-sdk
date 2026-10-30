"""The firmware's source: patterns that broke the device and must not come back."""

from __future__ import annotations

import re

from conftest import REPO

FIRMWARE = REPO / "firmware"


def _code_lines(path):
    """Each line without its // comment, with its number."""
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        yield number, line.split("//", 1)[0]


def test_firmware_never_calls_the_websocket_close_that_ignores_its_timeout():
    # esp_websocket_client_close() sends its close frame with no time limit (esp_websocket_client
    # 1.8.0), whatever timeout it is given. With Wi-Fi gone that write never finishes, and the app task
    # that called it stopped drawing, reading touch and retrying Wi-Fi until a hard reset. Closing goes
    # through hg::ws::process() (drivers/ws_link.hpp), which limits every wait.
    call = re.compile(r"\besp_websocket_client_close(_with_[a-z_]+)?\s*\(")
    found = []
    for path in sorted(FIRMWARE.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".h", ".hpp") or "managed_components" in path.parts \
                or ".pio" in path.parts:
            continue
        for number, code in _code_lines(path):
            if call.search(code):
                found.append(f"{path.relative_to(REPO)}:{number}: {code.strip()}")
    assert not found, "\n".join(found)


def test_the_websocket_transport_never_waits_without_a_limit_on_the_app_task():
    # The app task hands clients to the send task and queues messages for it. A wait with no limit
    # there wedges the device when the send task is stuck: the event queue fills and only a reset
    # clears it (#136). A stuck send task is caught by WsTransport::stuck() and restarts the device.
    path = FIRMWARE / "esp32" / "main" / "port_ws.cpp"
    found = []
    for number, code in _code_lines(path):
        if re.search(r"\bxQueueSend\w*\s*\(", code) and "portMAX_DELAY" in code:
            found.append(f"{path.relative_to(REPO)}:{number}: {code.strip()}")
    assert not found, "\n".join(found)


# wireguard-lwip's device state is shared by lwIP's thread (netif output) and TailTiny's WireGuard
# task (timers, incoming packets); TailTiny's UDP send waits for lwIP's thread. So in
# wireguardif.c the keys, counters, handshake state and peer table are only touched under
# WG_LOCK, and nothing that sends or enters lwIP runs under it (components/TAILTINY.md, patch 07).
WG_STATE_CALLS = (
    "wireguard_encrypt_packet", "wireguard_decrypt_packet", "keypair_destroy", "keypair_update",
    "wireguard_start_session", "wireguard_create_handshake_initiation",
    "wireguard_create_handshake_response", "wireguard_process_handshake_response",
    "wireguard_process_initiation_message", "wireguard_process_cookie_message",
    "wireguardif_initiate_handshake", "wireguardif_lookup_peer", "peer_lookup_by_allowed_ip",
    "peer_lookup_by_receiver", "peer_lookup_by_handshake", "peer_lookup_by_pubkey", "peer_alloc",
    "wireguardif_seal_locked", "wireguardif_process_data_message_locked",
    "wireguardif_make_handshake_response_locked",
)
WG_UNLOCKED_CALLS = (
    "wireguardif_endpoint_output", "wireguardif_device_output", "wireguardif_send_sealed",
    "wireguardif_send_keepalive", "wireguardif_output_to_peer", "wireguard_start_handshake",
    "wg_netif_set_link", "tcpip_api_call", "udp_sendto", "derp_output_fn", "udp_output_fn",
    "netif->input",
)
# Helpers that only ever run with the lock already held (each is checked as a caller too).
WG_LOCKED_HELPERS = ("wireguardif_initiate_handshake", "wireguardif_lookup_peer")


def _wg_functions(path=None):
    """(name, lines) for each function in wireguardif.c. A function's brace may be on its
    definition line or the next; any other top-level brace that opens neither a type nor an
    initializer fails, so a layout change can't drop a function from the lock check."""
    path = path or FIRMWARE / "esp32" / "components" / "wireguard_lwip" / "src" / "wireguardif.c"
    name, pending, depth, body = None, None, 0, []
    for number, code in _code_lines(path):
        code = re.sub(r"/\*.*?\*/", "", code)
        if depth == 0:
            definition = re.match(r"^(?:static\s+)?[\w\s\*]+?\b(\w+)\s*\([^;]*$", code)
            if definition and "{" in code:
                name, body, pending = definition.group(1), [], None
            elif definition:
                pending = definition.group(1)
            elif pending and "{" in code:  # the brace after a signature that spans lines
                name, body, pending = pending, [], None
            elif "{" in code and not re.match(r"\s*#|\s*(?:static\s+|const\s+)*(?:struct|union|enum)\b|.*=\s*\{", code):
                raise AssertionError(f"{path.name}:{number}: a top-level brace the lock check can't place")
        if name:
            body.append((number, code))
        depth += code.count("{") - code.count("}")
        if name and depth == 0 and body:
            yield name, body
            name = None


def test_the_lock_check_reads_a_function_whose_brace_is_on_its_own_line(tmp_path):
    source = tmp_path / "wireguardif.c"
    source.write_text("static void wireguardif_process_response_message(struct wireguard_device *device)\n"
                      "{\n\twireguard_start_session(peer, true);\n}\n", encoding="utf-8")
    assert [name for name, _ in _wg_functions(source)] == ["wireguardif_process_response_message"]


def test_wireguard_state_is_used_only_under_its_lock_and_never_sends_under_it():
    state = re.compile(r"\b(" + "|".join(WG_STATE_CALLS) + r")\s*\(")
    unlocked = re.compile(r"(\b(" + "|".join(c for c in WG_UNLOCKED_CALLS if "->" not in c)
                          + r")\s*\(|->input\s*\()")
    problems, checked = [], set()
    for name, body in _wg_functions():
        held = name.endswith("_locked") or name in WG_LOCKED_HELPERS
        for number, code in body[1:]:
            if re.search(r"\bWG_LOCK\s*\(", code):
                if held:
                    problems.append(f"wireguardif.c:{number}: {name} locks while holding the lock")
                held = True
            if state.search(code) and not held:
                problems.append(f"wireguardif.c:{number}: {name} calls {state.search(code).group(1)} without the lock")
            if unlocked.search(code) and held:
                problems.append(f"wireguardif.c:{number}: {name} sends or enters lwIP under the lock")
            if re.search(r"\bWG_UNLOCK\s*\(", code):
                held = False
            checked.add(name)
    assert {"wireguardif_output", "wireguardif_network_rx", "wireguardif_run_timers",
            "wireguard_start_handshake", "wireguardif_connect_derp"} <= checked
    assert not problems, "\n".join(problems)
