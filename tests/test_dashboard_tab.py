"""The Hermes dashboard's Gadgets tab: the gateway's status file, the tab's API routes and its auth.

The status file and the routes run without Hermes (the routes need FastAPI). The auth check mounts
the plugin in Hermes's own dashboard app, and the adapter check runs the gateway adapter, so both
need a Hermes checkout (``HERMES_AGENT_DIR``).
"""

from __future__ import annotations

import importlib.util
import json
import shutil
import sys
import time
import types

import pytest

from conftest import REPO, requires_hermes

from hermes_gadget_plugin import status as live_status
from hermes_gadget_plugin.store import DeviceStore

DASHBOARD = REPO / "plugin" / "dashboard"
DEVICE = "hg-0123456789abcdef"
OTHER = "hg-284c3f4387c04fb8"


# -- the status file ------------------------------------------------------------------------


def test_status_round_trip(tmp_path):
    live_status.write(tmp_path, {DEVICE: {"online": True, "battery": {"battery_percent": 80}}}, now=1000.0)
    got = live_status.read(tmp_path, now=1010.0)
    assert got == {"running": True, "updated_at": 1000.0,
                   "devices": {DEVICE: {"online": True, "battery": {"battery_percent": 80}}}}
    assert not list(tmp_path.glob("*.tmp"))  # written to a temporary name, then replaced


def test_status_missing_unreadable_stopped_or_stale_reads_as_not_running(tmp_path):
    assert live_status.read(tmp_path) == {"running": False, "updated_at": None, "devices": {}}
    (tmp_path / live_status.FILENAME).write_text("{not json")
    assert live_status.read(tmp_path)["running"] is False

    live_status.write(tmp_path, {}, running=False, now=1000.0)
    assert live_status.read(tmp_path, now=1001.0) == {"running": False, "updated_at": 1000.0, "devices": {}}

    # A gateway that died without saying so: its last word goes stale and its devices with it.
    live_status.write(tmp_path, {DEVICE: {"online": True}}, now=1000.0)
    assert live_status.read(tmp_path, now=1000.0 + live_status.STALE_AFTER_S)["running"] is True
    stale = live_status.read(tmp_path, now=1000.0 + live_status.STALE_AFTER_S + 1)
    assert stale == {"running": False, "updated_at": 1000.0, "devices": {}}


def test_battery_keeps_power_readings_only():
    sensors = {"battery_mv": 3950, "battery_percent": 72, "charging": 0, "external_power": 1,
               "battery_present": 1, "temperature_c": 21.5, "note": "x"}
    assert live_status.battery(sensors) == {"battery_mv": 3950, "battery_percent": 72, "charging": False,
                                            "external_power": True, "battery_present": True}
    assert live_status.battery({"temperature_c": 21.5}) is None
    assert live_status.battery({"battery_pct": 87}) == {"battery_percent": 87}  # docs/protocol.md's name
    assert live_status.battery({"battery_pct": 87, "battery_percent": 60}) == {"battery_percent": 60}
    assert live_status.battery({}) is None


def test_battery_drops_non_finite_readings_so_one_device_cannot_break_the_tab(tmp_path):
    # json.loads accepts NaN and Infinity in a device's state frame; the dashboard's JSON response
    # does not, so one such reading from any connected device would fail /devices for all of them.
    sensors = json.loads('{"battery_percent": NaN, "battery_mv": Infinity, "battery_pct": -Infinity, "charging": 1}')
    assert live_status.battery(sensors) == {"charging": True}
    live_status.write(tmp_path, {DEVICE: {"online": True, "battery": live_status.battery(sensors)}}, now=1000.0)
    json.loads((tmp_path / live_status.FILENAME).read_text(), parse_constant=lambda c: pytest.fail(c))


def test_two_stores_in_one_process_can_write_at_once(tmp_path):
    # The gateway's store, and the CLI's or the dashboard's, in one process: each save uses its own
    # temporary file, so neither replaces the other's and both writes land.
    import threading

    a, b = DeviceStore(tmp_path), DeviceStore(tmp_path)
    errors = []

    def save(store):
        try:
            for _ in range(200):
                with store._lock:
                    store._save()
        except OSError as exc:
            errors.append(exc)

    threads = [threading.Thread(target=save, args=(s,)) for s in (a, b)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
    assert not list(tmp_path.glob("*.tmp"))


def test_the_wheel_carries_the_dashboard_files():
    # hermes-gadget plugin install copies the installed package directory, so a file missing from
    # package-data never reaches Hermes. Every file the tab needs must match one of its globs.
    import fnmatch
    import re

    text = (REPO / "pyproject.toml").read_text(encoding="utf-8")
    line = re.search(r"^hermes_gadget_plugin = \[(.*)\]$", text, re.M).group(1)
    globs = re.findall(r'"([^"]+)"', line)
    for path in ("dashboard/manifest.json", "dashboard/plugin_api.py", "dashboard/dist/index.js"):
        assert (REPO / "plugin" / path).is_file()
        assert any(fnmatch.fnmatch(path, g) for g in globs), path


# -- the tab's API routes ---------------------------------------------------------------------


def _load_plugin_api():
    """Import plugin_api.py the way the dashboard does: a standalone file, not part of a package."""
    spec = importlib.util.spec_from_file_location("hermes_dashboard_plugin_gadget_test", DASHBOARD / "plugin_api.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def api(tmp_path, monkeypatch):
    pytest.importorskip("fastapi")
    pytest.importorskip("httpx")
    from fastapi import FastAPI
    from fastapi.testclient import TestClient

    module = _load_plugin_api()
    monkeypatch.setattr(module, "_data_dir", lambda: tmp_path)
    app = FastAPI()
    app.include_router(module.router, prefix="/api/plugins/gadget")
    return types.SimpleNamespace(client=TestClient(app), dir=tmp_path)


def _enroll(directory, device_id, *, name, board="waveshare-amoled-175", firmware="0.4.1", paired=True):
    store = DeviceStore(directory)
    store.enroll(device_id, b"k" * 32, name=name, board=board, address="192.0.2.7")
    store.touch(device_id, firmware=firmware)
    if paired:
        store.confirm(device_id)


def test_devices_lists_known_devices_without_their_keys(api):
    assert api.client.get("/api/plugins/gadget/devices").json() == {
        "gateway": {"running": False, "updated_at": None, "stale_after_s": live_status.STALE_AFTER_S},
        "devices": []}

    _enroll(api.dir, DEVICE, name="Desk")
    _enroll(api.dir, OTHER, name="Attic", board="m5stack-sticks3", paired=False)
    body = api.client.get("/api/plugins/gadget/devices").json()
    assert [d["id"] for d in body["devices"]] == [OTHER, DEVICE]  # by name
    desk = body["devices"][1]
    assert desk["name"] == "Desk" and desk["board"] == "waveshare-amoled-175" and desk["firmware"] == "0.4.1"
    assert desk["pending"] is False and body["devices"][0]["pending"] is True
    assert isinstance(desk["last_seen"], float)
    # Without a running gateway nothing is online, whatever the store says.
    assert desk["online"] is False and desk["battery"] is None
    text = json.dumps(body)
    assert "key" not in desk and "address" not in desk and "192.0.2.7" not in text


def test_devices_takes_online_and_battery_from_a_fresh_status_file(api):
    _enroll(api.dir, DEVICE, name="Desk")
    _enroll(api.dir, OTHER, name="Attic")
    battery = {"battery_percent": 64, "battery_mv": 3870, "charging": True}
    live_status.write(api.dir, {DEVICE: {"online": True, "connected_at": 1234.5, "battery": battery}})
    body = api.client.get("/api/plugins/gadget/devices").json()
    assert body["gateway"]["running"] is True
    by_id = {d["id"]: d for d in body["devices"]}
    assert by_id[DEVICE]["online"] is True and by_id[DEVICE]["battery"] == battery
    assert by_id[DEVICE]["connected_at"] == 1234.5
    assert by_id[OTHER]["online"] is False and by_id[OTHER]["battery"] is None

    # The same file, once stale, says nothing about the devices.
    live_status.write(api.dir, {DEVICE: {"online": True, "battery": battery}},
                      now=time.time() - live_status.STALE_AFTER_S - 5)
    body = api.client.get("/api/plugins/gadget/devices").json()
    assert body["gateway"]["running"] is False
    assert all(d["online"] is False and d["battery"] is None for d in body["devices"])


def test_the_dashboard_folder_leaves_the_plugin_dashboard_module_importable():
    # plugin/dashboard/ (the tab) sits next to plugin/dashboard.py (the device's dashboard); the
    # gateway imports the module, and a directory without __init__.py must not shadow it.
    assert not (DASHBOARD / "__init__.py").exists()
    from hermes_gadget_plugin import dashboard

    assert hasattr(dashboard, "BOARD") and hasattr(dashboard, "TileStore")


def test_manifest_names_the_plugin_and_its_files():
    manifest = json.loads((DASHBOARD / "manifest.json").read_text())
    assert manifest["name"] == "gadget"  # the plugins.enabled name gates the routes, and the URL is /api/plugins/gadget
    assert (DASHBOARD / manifest["entry"]).is_file() and (DASHBOARD / manifest["api"]).is_file()
    bundle = (DASHBOARD / manifest["entry"]).read_text()
    assert 'register("gadget"' in bundle and bundle.startswith("// Generated by site/scripts/dashboard-tab.mjs")
    assert "version" not in manifest  # plugin.yaml carries the plugin's version; one place to bump at release


# -- auth, in Hermes's own dashboard ----------------------------------------------------------


@requires_hermes
def test_hermes_dashboard_refuses_the_tab_api_without_a_session(tmp_path, monkeypatch):
    """Mounted by Hermes's dashboard from an installed, enabled plugin: an unauthenticated request is
    refused before the route runs, and an authenticated one gets the devices."""
    pytest.importorskip("fastapi")
    home = tmp_path / "home"
    shutil.copytree(REPO / "plugin", home / "plugins" / "gadget", ignore=shutil.ignore_patterns("__pycache__"))
    (home / "config.yaml").write_text(json.dumps({"plugins": {"enabled": ["gadget"]}}))
    monkeypatch.setenv("HERMES_HOME", str(home))
    _enroll(home / "plugin-data" / "gadget", DEVICE, name="Desk")

    from starlette.testclient import TestClient

    from hermes_cli import web_server
    from hermes_cli import web_server_dashboard

    routes = web_server.app.router.routes
    before = list(routes)
    web_server._dashboard_plugins_cache = None
    try:
        web_server_dashboard._mount_plugin_api_routes()
        added = [r for r in routes if r not in before]
        assert added, "Hermes did not mount the gadget plugin's API"
        # Mounted after startup, the routes land behind the SPA's catch-all; put them where a
        # startup mount puts them, ahead of it.
        catch_all = next((i for i, r in enumerate(routes) if getattr(r, "path", "") == "/{full_path:path}"), None)
        if catch_all is not None:
            routes[:] = [r for r in routes[:catch_all] if r not in added] + added + [
                r for r in routes[catch_all:] if r not in added]

        client = TestClient(web_server.app)
        refused = client.get("/api/plugins/gadget/devices")
        assert refused.status_code == 401
        assert "Desk" not in refused.text

        client.headers[web_server._SESSION_HEADER_NAME] = web_server._SESSION_TOKEN
        allowed = client.get("/api/plugins/gadget/devices")
        assert allowed.status_code == 200
        assert [d["name"] for d in allowed.json()["devices"]] == ["Desk"]
    finally:
        routes[:] = before
        web_server._dashboard_plugins_cache = None
        for name in [m for m in sys.modules if m.startswith(("hermes_dashboard_plugin_gadget", "hermes_gadget_dashboard_plugin"))]:
            del sys.modules[name]


# -- the gateway writes the status file ----------------------------------------------------------


@requires_hermes
def test_gateway_writes_live_status_for_the_tab(registered_platform, loop_thread, tmp_path, monkeypatch):
    import plugins.plugin_storage as storage
    from gateway.config import PlatformConfig

    import hermes_gadget_plugin.adapter as adapter_module
    from hermes_gadget_plugin.adapter import GadgetAdapter

    data = tmp_path / "plugin-data" / "gadget"
    monkeypatch.setattr(storage, "plugin_data_dir", lambda name: tmp_path / "plugin-data" / name)
    monkeypatch.setattr(adapter_module, "STATUS_POLL_S", 0.05)
    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra={"host": "127.0.0.1", "port": 0}))
    assert loop_thread.run(adapter.connect())

    def wait_for(check, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = live_status.read(data)
            if check(got):
                return got
            time.sleep(0.02)
        raise AssertionError(f"status never matched: {live_status.read(data)}")

    try:
        wait_for(lambda s: s["running"] and s["devices"] == {})
        session = types.SimpleNamespace(
            device_id=DEVICE, closed=False, paired=True, profile="default", connected_at=1000.0,
            sensors={"battery_percent": 72, "battery_mv": 3950, "charging": 0, "temperature_c": 21.5},
            sensors_at=1001.0)
        loop_thread.run(_add_session(adapter, session))
        got = wait_for(lambda s: DEVICE in s["devices"])
        assert got["devices"][DEVICE] == {
            "online": True, "paired": True, "profile": "default", "connected_at": 1000.0,
            "battery": {"battery_percent": 72, "battery_mv": 3950, "charging": False}, "sensors_at": 1001.0}

        session.sensors = {**session.sensors, "charging": 1}  # plugged in: written at once
        wait_for(lambda s: (s["devices"].get(DEVICE) or {}).get("battery", {}).get("charging") is True)

        _enroll(data, DEVICE, name="Desk")
        DeviceStore(data).touch(DEVICE)
        came = DeviceStore(data).devices()[DEVICE]["last_seen"]
        time.sleep(0.01)
        loop_thread.run(_drop_session(adapter, DEVICE))
        loop_thread.run(adapter.on_disconnect(session))
        wait_for(lambda s: s["running"] and s["devices"] == {})
        assert DeviceStore(data).devices()[DEVICE]["last_seen"] > came  # last seen is when it left
    finally:
        loop_thread.run(adapter.disconnect())
    raw = json.loads((data / live_status.FILENAME).read_text())
    assert raw["running"] is False and raw["devices"] == {}  # stopped on purpose


async def _add_session(adapter, session):
    adapter.hub.sessions[session.device_id] = session


async def _drop_session(adapter, device_id):
    adapter.hub.sessions.pop(device_id, None)
