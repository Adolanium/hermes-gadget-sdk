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


def test_tailtiny_is_reset_only_at_boot_before_it_starts():
    # tailtiny_factory_reset() must run before tailtiny_init() (tailtiny.h): on a running
    # node it frees the peer cache TailTiny's tasks still use, and the node can write its
    # keys back. factory-reset only leaves a flag (TailnetLink::forget()); TailnetLink::begin()
    # erases at the next boot, before Wi-Fi starts the node.
    call = re.compile(r"\btailtiny_factory_reset\s*\(")
    found = []
    for path in sorted((FIRMWARE / "esp32" / "main").glob("*.cpp")):
        function = None
        for number, code in _code_lines(path):
            definition = re.match(r"^\S.*?\b(\w+::\w+)\s*\(", code)
            if definition:
                function = definition.group(1)
            if call.search(code):
                found.append((path.name, function))
    assert found == [("port_tailnet.cpp", "TailnetLink::begin")], found
