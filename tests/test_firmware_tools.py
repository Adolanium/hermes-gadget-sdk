"""The firmware build checks in firmware/esp32/tools/."""

from __future__ import annotations

import importlib.util
import struct
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1] / "firmware" / "esp32" / "tools"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


check_config = _load("check_config")
check_size = _load("check_size")


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
