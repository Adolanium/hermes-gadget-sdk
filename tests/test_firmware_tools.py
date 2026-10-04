"""The firmware build checks and release packaging in firmware/esp32/tools/."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import re
import struct
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1] / "firmware" / "esp32" / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses look their module up while the class is built
    spec.loader.exec_module(module)
    return module


check_config = _load("check_config")
check_size = _load("check_size")
package_release = _load("package_release")


@pytest.fixture
def configs(tmp_path):
    def write(name: str, text: str) -> Path:
        path = tmp_path / name
        path.write_text(text.strip() + "\n", encoding="utf-8")
        return path

    return write


BASE = """
# every board
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_COMPILER_CXX_EXCEPTIONS=n
"""

GENERATED = """
# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
# CONFIG_COMPILER_CXX_EXCEPTIONS is not set
CONFIG_IDF_TARGET="esp32s3"
CONFIG_SPIRAM=y
"""


def test_matching_config_passes(configs):
    base = configs("base", BASE)
    board = configs("board", """
CONFIG_IDF_TARGET="esp32s3"
# CONFIG_ESPTOOLPY_FLASHSIZE_4MB is not set
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_SPIRAM=y
""")
    assert check_config.check(configs("sdkconfig", GENERATED), [base, board]) == []


def test_choice_override_must_say_so(configs):
    base = configs("base", BASE)
    board = configs("board", "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y")
    problems = check_config.check(configs("sdkconfig", GENERATED), [base, board])
    assert len(problems) == 1
    assert "CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y, but the config has it off" in problems[0]
    assert problems[0].startswith(f"{base}:2:")


def test_dropped_and_misspelled_lines_are_reported(configs):
    board = configs("board", """
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPRAM=y
CONFIG_IDF_TARGET="esp32c3"
""")
    problems = check_config.check(configs("sdkconfig", GENERATED), [board])
    assert [p.split(": ", 1)[1] for p in problems] == [
        "CONFIG_SPIRAM_MODE_OCT=y, but the config has nothing",
        "CONFIG_SPRAM=y, but the config has nothing",
        'CONFIG_IDF_TARGET="esp32c3", but the config has CONFIG_IDF_TARGET="esp32s3"',
    ]


def test_stale_sdkconfig_is_reported(configs):
    # A newer checkout turns something off that an old sdkconfig still has on.
    board = configs("board", "# CONFIG_SPIRAM is not set")
    problems = check_config.check(configs("sdkconfig", GENERATED), [board])
    assert problems == [f"{board}:1: CONFIG_SPIRAM should be off, but the config has CONFIG_SPIRAM=y"]


def test_cli_exit_codes(configs, capsys):
    sdkconfig = configs("sdkconfig", GENERATED)
    good = configs("good", "CONFIG_SPIRAM=y")
    bad = configs("bad", "CONFIG_SPIRAM_MODE_OCT=y")
    root = sdkconfig.parent
    assert check_config.main(["--sdkconfig", str(sdkconfig), "--defaults", "good", "--root", str(root)]) == 0
    assert check_config.main(["--sdkconfig", str(sdkconfig), "--defaults", f"good;{bad}", "--root", str(root)]) == 1
    assert "delete it (or the build directory)" in capsys.readouterr().err
    assert check_config.main(["--sdkconfig", str(sdkconfig), "--defaults", "missing", "--root", str(root)]) == 2


def _table(*parts: tuple[int, int, str, int]) -> bytes:
    rows = b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", ptype, subtype, 0x10000, size, label.encode(), 0)
                    for ptype, subtype, label, size in parts)
    return rows + b"\xeb\xeb" + b"\xff" * 30 + b"\xff" * 32


@pytest.mark.parametrize("app_kb, expected", [(1400, 0), (1800, 1)])
def test_size_check_keeps_a_margin(tmp_path, app_kb, expected, capsys):
    table = tmp_path / "partitions.bin"
    table.write_bytes(_table((0x01, 0x02, "nvs", 0x6000), (0x00, 0x10, "ota_0", 0x1F0000),
                             (0x00, 0x11, "ota_1", 0x1E0000)))
    app = tmp_path / "app.bin"
    app.write_bytes(b"\0" * app_kb * 1024)
    assert check_size.main(["--app", str(app), "--partitions", str(table)]) == expected
    out = capsys.readouterr()
    assert "'ota_1' slot" in (out.out + out.err)  # the smallest app slot decides


def test_size_check_needs_an_app_partition(tmp_path):
    table = tmp_path / "partitions.bin"
    table.write_bytes(_table((0x01, 0x02, "nvs", 0x6000)))
    app = tmp_path / "app.bin"
    app.write_bytes(b"\0")
    assert check_size.main(["--app", str(app), "--partitions", str(table)]) == 2


# partitions.csv as built: (type, subtype, label, offset, size)
LAYOUT = [(0x01, 0x02, "nvs", 0x9000, 0x6000), (0x01, 0x00, "otadata", 0xF000, 0x2000),
          (0x01, 0x01, "phy_init", 0x11000, 0x1000), (0x00, 0x10, "ota_0", 0x20000, 0x1F0000),
          (0x00, 0x11, "ota_1", 0x210000, 0x1F0000)]


def _partition_table(layout=LAYOUT) -> bytes:
    rows = b"".join(struct.pack("<2sBBII16sI", b"\xaa\x50", ptype, subtype, offset, size, label.encode(), 0)
                    for ptype, subtype, label, offset, size in layout)
    return rows + b"\xeb\xeb" + b"\xff" * 30 + b"\xff" * 32


@pytest.fixture
def project(tmp_path):
    """A firmware/esp32 directory with PlatformIO build outputs for the boards it is asked for."""
    from fakes.fake_firmware import fake_image

    root = tmp_path / "esp32"
    (root / "boards").mkdir(parents=True)
    envs = []

    def build(env: str, *, board_dir: str | None = None, version: str = "0.2.0", flash: str = "4MB",
              psram: str | None = "OCT", image_board: str | None = None, meta: dict | None = None):
        board_dir = board_dir or env
        out = root / ".pio" / "build" / env
        out.mkdir(parents=True)
        (out / "bootloader.bin").write_bytes(b"\xe9\x03\x02\x20" + b"B" * 0x5000)
        (out / "partitions.bin").write_bytes(_partition_table())
        (out / "ota_data_initial.bin").write_bytes(b"\xff" * 0x2000)
        (out / "firmware.bin").write_bytes(fake_image(board=image_board or env, version=version, size=0x30000))
        sdk = ['CONFIG_IDF_TARGET="esp32s3"', "CONFIG_PARTITION_TABLE_OFFSET=0x8000",
               f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash}"']
        if psram:
            sdk += ["CONFIG_SPIRAM=y", f"CONFIG_SPIRAM_MODE_{psram}=y"]
        (root / f"sdkconfig.{env}").write_text("\n".join(sdk) + "\n", encoding="utf-8")
        (root / "boards" / board_dir).mkdir(exist_ok=True)
        if meta is not None:
            (root / "boards" / board_dir / "board.json").write_text(json.dumps(meta), encoding="utf-8")
        envs.append(f"[env:{env}]\nboard_build.cmake_extra_args =\n    "
                    f'-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/{board_dir}/sdkconfig.defaults"\n')
        (root / "platformio.ini").write_text("[env]\nframework = espidf\n\n" + "\n".join(envs), encoding="utf-8")
        return out

    build.root = root
    return build


def _package(project, *args):
    return package_release.main(["--project", str(project.root), *args])


def test_release_image_puts_every_part_at_its_address(project, tmp_path):
    out_dir = project("amoled", flash="16MB", image_board="amoled-1.75",
                      meta={"title": "Round AMOLED", "summary": "No wiring.", "ready_made": True})
    dist = tmp_path / "dist"
    assert _package(project, "--board", "amoled", "--out", str(dist), "--expect-version", "0.2.0") == 0

    image = (dist / "hermes-gadget-amoled-0.2.0.bin").read_bytes()
    app = (out_dir / "firmware.bin").read_bytes()
    assert image[:0x5004] == (out_dir / "bootloader.bin").read_bytes()
    assert image[0x8000:0x8000 + len(_partition_table())] == _partition_table()
    assert image[0x9000:0xF000] == b"\xff" * 0x6000  # the settings area is erased
    assert image[0xF000:0x11000] == b"\xff" * 0x2000
    assert image[0x20000:] == app and len(image) == 0x20000 + len(app)
    assert (dist / "hermes-gadget-amoled-0.2.0-app.bin").read_bytes() == app

    manifest = json.loads((dist / "manifest.json").read_text(encoding="utf-8"))
    build = manifest["builds"][0]
    assert manifest["version"] == "0.2.0"
    assert (build["board"], build["image_board"], build["title"], build["ready_made"]) == (
        "amoled", "amoled-1.75", "Round AMOLED", True)
    assert (build["chip"], build["flash_size"], build["psram"]) == ("ESP32-S3", "16MB", "octal")
    assert build["settings"] == {"offset": 0x9000, "size": 0x6000}
    assert build["image"]["sha256"] == hashlib.sha256(image).hexdigest() and build["image"]["size"] == len(image)
    sums = (dist / "SHA256SUMS").read_text(encoding="utf-8").splitlines()
    assert f"{hashlib.sha256(image).hexdigest()}  hermes-gadget-amoled-0.2.0.bin" in sums
    assert f"{hashlib.sha256(app).hexdigest()}  hermes-gadget-amoled-0.2.0-app.bin" in sums


def test_every_board_comes_from_platformio_ini(project, tmp_path):
    project("breadboard", psram="QUAD", meta={"title": "Breadboard"})
    project("lcd-154", board_dir="waveshare-lcd-154", meta={"title": "LCD 1.54"})  # names needn't match
    project("plain", psram=None)
    dist, notes = tmp_path / "dist", tmp_path / "notes.md"
    assert _package(project, "--all", "--out", str(dist), "--notes", str(notes)) == 0
    builds = json.loads((dist / "manifest.json").read_text(encoding="utf-8"))["builds"]
    assert [(b["board"], b["title"], b["psram"]) for b in builds] == [
        ("breadboard", "Breadboard", "quad"), ("lcd-154", "LCD 1.54", "octal"), ("plain", "plain", None)]
    text = notes.read_text(encoding="utf-8")
    assert package_release.INSTALLER_URL in text
    assert "| LCD 1.54 | `hermes-gadget-lcd-154-0.2.0.bin` | `hermes-gadget-lcd-154-0.2.0-app.bin` |" in text
    assert "hermes plugins install" not in text  # no commit given, so no pinned plugin


def test_every_real_board_has_what_the_installer_shows():
    """Each board in platformio.ini: a title, a one-line summary and a "Pins and details" link that
    lands on a heading. A ready-made board gets the installer's "Nothing to wire" badge, so its
    summary doesn't repeat it."""
    repo = TOOLS.parents[2]
    anchors = {re.sub(r"[^\w\- ]", "", line.lstrip("#").strip().lower()).replace(" ", "-")
               for line in (repo / "docs" / "hardware.md").read_text(encoding="utf-8").splitlines()
               if line.startswith("#")}
    for board, board_dir in package_release.environments(package_release.PROJECT_DIR).items():
        meta = json.loads((package_release.PROJECT_DIR / "boards" / board_dir / "board.json").read_text(encoding="utf-8"))
        assert meta.get("title") and meta.get("summary"), board
        page, _, anchor = meta["docs"].partition("#")
        assert page == "docs/hardware.md" and anchor in anchors, f"{board}: {meta['docs']} isn't a heading"
        if meta.get("ready_made"):
            assert "nothing to wire" not in meta["summary"].lower(), f"{board}: the badge already says it"


def test_release_notes_pin_the_plugin_to_the_release_commit(project, tmp_path):
    project("breadboard")
    notes = tmp_path / "notes.md"
    commit = "75b8a689bce3925d46de5c256f53d343ae6b876f"
    assert _package(project, "--all", "--out", str(tmp_path / "dist"), "--notes", str(notes), "--commit", commit) == 0
    assert (f"hermes plugins install https://github.com/Adolanium/hermes-gadget-sdk.git#plugin --ref {commit} --enable"
            in notes.read_text(encoding="utf-8"))


def test_release_refuses_what_it_cannot_vouch_for(project, tmp_path, capsys):
    project("one", version="0.2.0")
    project("two", version="0.3.0")
    dist = str(tmp_path / "dist")
    assert _package(project, "--all", "--out", dist) == 1
    assert "different versions: 0.2.0, 0.3.0" in capsys.readouterr().err

    assert _package(project, "--board", "one", "--out", dist, "--expect-version", "1.0.0") == 1
    assert "reports 0.2.0, not 1.0.0" in capsys.readouterr().err

    assert _package(project, "--board", "three", "--out", dist) == 1
    assert "not in platformio.ini: three" in capsys.readouterr().err

    (project.root / ".pio" / "build" / "one" / "firmware.bin").unlink()
    assert _package(project, "--board", "one", "--out", dist) == 1
    assert "run: pio run -e one" in capsys.readouterr().err


def test_merge_refuses_overlapping_parts():
    with pytest.raises(package_release.PackageError, match="overlap"):
        package_release.merge([(0, b"x" * 0x2000), (0x1000, b"y")])
