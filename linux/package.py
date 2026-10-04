#!/usr/bin/env python3
"""Bundle an already-built native core and Python wheel for Linux installation."""

import argparse
import hashlib
import json
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def package(library: Path, wheel: Path, out: Path, revision: str) -> Path:
    machine = platform.machine().lower()
    architecture = {"aarch64": "arm64", "arm64": "arm64", "x86_64": "amd64"}.get(machine)
    if not architecture or platform.system() != "Linux":
        raise ValueError("build Linux packages on a native ARM64 or x86-64 Linux host")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("revision must be a full Git commit ID")
    metadata = (ROOT / "pyproject.toml").read_text(encoding="utf-8")
    version = re.search(r'(?m)^version = "([0-9.]+)"', metadata).group(1)
    if not wheel.name.startswith(f"hermes_gadget-{version}-"):
        raise ValueError("wheel version does not match the source")
    # Reject a binary from another architecture before giving it a release name.
    header = library.read_bytes()[:20]
    expected = 183 if architecture == "arm64" else 62
    if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != expected:
        raise ValueError("native library is not a matching 64-bit little-endian ELF")
    name = f"hermes-gadget-{version}-linux-{architecture}"
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hermes-package-") as temporary:
        root = Path(temporary) / name
        root.mkdir()
        shutil.copy2(library, root / "libhgsim.so")
        shutil.copy2(wheel, root / wheel.name)
        for file in ("LICENSE", "NOTICE", "README.md"):
            shutil.copy2(ROOT / file, root / file)
        shutil.copytree(ROOT / "LICENSES", root / "LICENSES")
        shutil.copy2(ROOT / "docs/linux.md", root / "linux.md")
        for file in ("install.sh", "hermes-gadget.service", "hermes-gadget-device"):
            (root / file).write_text((ROOT / "linux" / file).read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
        (root / "install.sh").chmod(0o755)
        manifest = {"version": version, "revision": revision, "architecture": architecture,
                    "wheel": wheel.name, "release_id": f"{version}-{revision[:12]}"}
        (root / "package.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        sums = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}"
                for path in sorted(root.rglob("*")) if path.is_file()]
        (root / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
        archive = out / f"{name}.tar.gz"
        with tarfile.open(archive, "w:gz") as tar:
            tar.add(root, arcname=name)
    (out / f"{archive.name}.sha256").write_text(
        f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n", encoding="utf-8")
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=ROOT / "dist")
    parser.add_argument("--revision", default=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip())
    args = parser.parse_args()
    print(package(args.library, args.wheel, args.out, args.revision))
