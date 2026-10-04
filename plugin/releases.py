"""Firmware from this project's GitHub releases, for ``hermes gadget update --latest``.

Every release carries ``manifest.json`` (``firmware/esp32/tools/package_release.py``), which names
each board's app image with its size and SHA-256. A download is used only if it matches both.
"""

from __future__ import annotations

import hashlib
import json
import urllib.error
import urllib.request

RELEASES = "https://github.com/Adolanium/hermes-gadget-sdk/releases"
TIMEOUT_S = 30
MAX_MANIFEST_BYTES = 256 * 1024
MAX_APP_BYTES = 8 * 1024 * 1024


class ReleaseError(Exception):
    pass


def _get(url: str, limit: int) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "hermes-gadget"})
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT_S) as response:
            data = response.read(limit + 1)
    except urllib.error.HTTPError as exc:
        raise ReleaseError(f"{url} answered HTTP {exc.code}.") from exc
    except (urllib.error.URLError, OSError) as exc:
        raise ReleaseError(f"Couldn't reach {url}: {getattr(exc, 'reason', exc)}.") from exc
    if len(data) > limit:
        raise ReleaseError(f"{url} is larger than any release file should be.")
    return data


def latest_manifest() -> dict:
    """The newest release's manifest (drafts and pre-releases aren't "latest")."""
    data = _get(f"{RELEASES}/latest/download/manifest.json", MAX_MANIFEST_BYTES)
    try:
        manifest = json.loads(data)
        if not isinstance(manifest["version"], str) or not isinstance(manifest["builds"], list):
            raise TypeError
    except (ValueError, KeyError, TypeError) as exc:
        raise ReleaseError("The latest release's manifest.json isn't one this version understands.") from exc
    return manifest


def build_for(manifest: dict, board: str) -> dict:
    """The release's build for a device's board, as the device names it (``image_board``)."""
    for build in manifest["builds"]:
        if build.get("image_board") == board:
            return build
    boards = ", ".join(str(b.get("image_board")) for b in manifest["builds"]) or "none"
    raise ReleaseError(f"The latest release ({manifest['version']}) has no firmware for {board}. Its boards: {boards}.")


def download_app(manifest: dict, build: dict) -> bytes:
    """The build's app image, checked against the manifest's size and SHA-256."""
    app = build["app"]
    data = _get(f"{RELEASES}/download/v{manifest['version']}/{app['path']}", MAX_APP_BYTES)
    if len(data) != app["size"] or hashlib.sha256(data).hexdigest() != app["sha256"]:
        raise ReleaseError(f"{app['path']} doesn't match the checksum in the release's manifest, so it wasn't used.")
    return data
