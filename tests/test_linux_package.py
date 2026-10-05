"""Release package contents and architecture checks without requiring a Pi."""

import hashlib
import importlib.util
import json
import tarfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("linux_package", ROOT / "linux/package.py")
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)


def test_package_has_matching_binary_checksums_licenses_and_service(tmp_path, monkeypatch):
    monkeypatch.setattr(packaging.platform, "system", lambda: "Linux")
    monkeypatch.setattr(packaging.platform, "machine", lambda: "aarch64")
    library = tmp_path / "libhgsim.so"
    library.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00" + b"test binary")
    from hermes_gadget import __version__

    wheel = tmp_path / f"hermes_gadget-{__version__}-py3-none-any.whl"
    wheel.write_bytes(b"test wheel")
    archive = packaging.package(library, wheel, tmp_path / "out", "a" * 40)
    with tarfile.open(archive) as tar:
        files = {Path(member.name).relative_to(archive.name.removesuffix(".tar.gz")).as_posix():
                 tar.extractfile(member).read() for member in tar.getmembers() if member.isfile()}
    manifest = json.loads(files["package.json"])
    assert manifest["architecture"] == "arm64"
    assert manifest["revision"] == "a" * 40
    assert files["libhgsim.so"] == b"\x7fELF\x02\x01" + bytes(12) + b"\xb7\x00" + b"test binary"
    assert files[wheel.name] == b"test wheel"
    assert b"MIT License" in files["LICENSE"]
    assert b"Waveshare" in files["NOTICE"]
    assert b"[Paho MQTT]" in files["THIRD_PARTY_NOTICES.md"]
    assert b"Apache License" in files["LICENSES/Apache-2.0.txt"]
    assert b"User=hermes-gadget" in files["hermes-gadget.service"]
    for line in files["SHA256SUMS"].decode().splitlines():
        digest, name = line.split("  ", 1)
        assert hashlib.sha256(files[name]).hexdigest() == digest
    checksum = archive.with_suffix(archive.suffix + ".sha256").read_text().split()[0]
    assert hashlib.sha256(archive.read_bytes()).hexdigest() == checksum
    library.write_bytes(b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00")
    with pytest.raises(ValueError, match="matching"):
        packaging.package(library, wheel, tmp_path / "out", "a" * 40)
