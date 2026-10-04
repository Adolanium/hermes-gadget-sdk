"""`hermes gadget update <device> --latest`, against a local server laid out like GitHub's release downloads."""

from __future__ import annotations

import functools
import hashlib
import http.server
import json
import threading
import types

import pytest

from fakes.fake_firmware import fake_image
from hermes_gadget_plugin import cli, ota, releases
from hermes_gadget_plugin.store import DeviceStore

DEVICE_ID = "hg-630dcd2966c43366"
KEY = bytes(range(32))
BOARD = "esp32s3-touch-amoled-1.75"


@pytest.fixture
def github(tmp_path, monkeypatch):
    """Serves <root>/latest/download/manifest.json and <root>/download/v<version>/<file>."""
    root = tmp_path / "releases"

    def publish(version="0.2.0", board=BOARD, *, corrupt=False):
        app = fake_image(board=board, version=version, size=40_000)
        name = f"hermes-gadget-esp32s3-touch-amoled-175-{version}-app.bin"
        (root / "download" / f"v{version}").mkdir(parents=True, exist_ok=True)
        (root / "download" / f"v{version}" / name).write_bytes(app + (b"x" if corrupt else b""))
        manifest = {"version": version, "builds": [{
            "board": "esp32s3-touch-amoled-175", "image_board": board, "title": "Waveshare ESP32-S3-Touch-AMOLED-1.75",
            "app": {"path": name, "size": len(app), "sha256": hashlib.sha256(app).hexdigest()}}]}
        (root / "latest" / "download").mkdir(parents=True, exist_ok=True)
        (root / "latest" / "download" / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        return app

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(root))
    handler.log_message = lambda *a, **k: None
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    root.mkdir()
    monkeypatch.setattr(releases, "RELEASES", f"http://127.0.0.1:{server.server_address[1]}")
    yield publish
    server.shutdown()


@pytest.fixture
def store(tmp_path, monkeypatch):
    store = DeviceStore(tmp_path / "plugin-data")
    store.enroll(DEVICE_ID, KEY, name="Desk", board=BOARD)
    store.touch(DEVICE_ID, firmware="0.1.0")
    monkeypatch.setattr(cli, "_store", lambda: store)
    return store


def _update(**kw):
    args = {"device": "Desk", "image": None, "latest": True, "force": False, "no_wait": True, "timeout": 1, **kw}
    cli._cmd_update(types.SimpleNamespace(**args))


def _staged(store):
    loaded = ota.UpdateQueue(store.path.parent).load(DEVICE_ID)
    return None if loaded is None else loaded[0]


def test_latest_stages_the_release_image_for_the_device_board(github, store, capsys):
    app = github("0.2.0")
    _update()
    assert _staged(store) == app
    out = capsys.readouterr().out
    assert "Downloading firmware 0.2.0 for Waveshare ESP32-S3-Touch-AMOLED-1.75" in out
    assert "Staged firmware 0.2.0" in out


def test_a_device_on_the_latest_release_is_left_alone_unless_forced(github, store, capsys):
    app = github("0.2.0")
    store.touch(DEVICE_ID, firmware="0.2.0")
    _update()
    assert _staged(store) is None
    assert "Desk already runs 0.2.0, the latest release. --force installs it again." in capsys.readouterr().out
    _update(force=True)
    assert _staged(store) == app


def test_a_board_the_release_lacks_is_named(github, store):
    github("0.2.0", board="esp32s3-breadboard")
    with pytest.raises(SystemExit) as stop:
        _update()
    assert "has no firmware for esp32s3-touch-amoled-1.75. Its boards: esp32s3-breadboard." in str(stop.value)
    assert _staged(store) is None


def test_a_download_that_fails_its_checksum_is_not_staged(github, store):
    github("0.2.0", corrupt=True)
    with pytest.raises(SystemExit) as stop:
        _update()
    assert "doesn't match the checksum" in str(stop.value)
    assert _staged(store) is None


def test_no_release_or_no_network_says_so(github, store, monkeypatch):
    with pytest.raises(SystemExit) as stop:  # nothing published yet
        _update()
    assert "answered HTTP 404" in str(stop.value)
    monkeypatch.setattr(releases, "RELEASES", "http://127.0.0.1:9")  # nothing listens there
    with pytest.raises(SystemExit) as stop:
        _update()
    assert "Couldn't reach" in str(stop.value)


def test_an_image_and_latest_are_exclusive(store):
    for kw in ({"image": "firmware.bin"}, {"latest": False}):
        with pytest.raises(SystemExit) as stop:
            _update(**kw)
        assert "Give either a firmware image or --latest." in str(stop.value)


def test_devices_shows_each_firmware_version(store, capsys):
    cli._cmd_devices(types.SimpleNamespace())
    line = next(line for line in capsys.readouterr().out.splitlines() if line.startswith(DEVICE_ID))
    assert "Desk" in line and BOARD in line and "0.1.0" in line
