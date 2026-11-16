"""firmware/esp32/tools/ci_boards.py: the boards a pull request's firmware build covers."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

TOOLS = Path(__file__).resolve().parents[1] / "firmware" / "esp32" / "tools"
spec = importlib.util.spec_from_file_location("ci_boards", TOOLS / "ci_boards.py")
ci_boards = importlib.util.module_from_spec(spec)
sys.modules["ci_boards"] = ci_boards
spec.loader.exec_module(ci_boards)

BOARD_CPP = """\
#include "board.hpp"
#if CONFIG_HG_BOARD_ALPHA
#define HG_BOARD_NAME "alpha"
#elif CONFIG_HG_BOARD_BETA || CONFIG_HG_BOARD_P4
#define HG_BOARD_NAME "beta"
#else
#error "no board"
#endif
int shared() { return 1; }
#if !CONFIG_HG_BOARD_ALPHA
int everyone_else() { return 2; }
#endif
#if CONFIG_HG_BOARD_ALPHA
int alpha_pin() {
#if CONFIG_SPIRAM
  return 4;
#endif
  return 5;
}
#endif
"""

INI = """\
[env]
platform = espressif32

[env:alpha]
board = a
board_build.cmake_extra_args =
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/alpha/sdkconfig.defaults"

[env:beta-env]
board = b
board_build.cmake_extra_args =
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/beta/sdkconfig.defaults"

[env:esp32s3-breadboard]
board = c
board_build.cmake_extra_args =
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-breadboard/sdkconfig.defaults"
"""


def test_a_line_belongs_to_the_boards_its_conditionals_name():
    named = ci_boards.line_symbols(BOARD_CPP)
    lines = BOARD_CPP.splitlines()
    at = {text: named[i] for i, text in enumerate(lines)}
    assert at['#define HG_BOARD_NAME "alpha"'] == {"HG_BOARD_ALPHA"}
    assert at['#define HG_BOARD_NAME "beta"'] == {"HG_BOARD_BETA", "HG_BOARD_P4"}
    assert at['#error "no board"'] is None  # the #else of a board chain is every other board
    assert at["int shared() { return 1; }"] is None
    assert at["int everyone_else() { return 2; }"] is None  # a negated board is every other board
    assert at["  return 4;"] == {"HG_BOARD_ALPHA"}  # a plain #if inside a board's block
    assert at["  return 5;"] == {"HG_BOARD_ALPHA"}


@pytest.fixture
def repo(tmp_path, monkeypatch):
    """A repository with three PlatformIO boards and one idf.py board, and its first commit."""
    project = tmp_path / "firmware" / "esp32"
    for directory, symbol in (("alpha", "ALPHA"), ("beta", "BETA"), ("esp32s3-breadboard", "ESP32S3_BREADBOARD"),
                              ("p4", "P4")):
        (project / "boards" / directory).mkdir(parents=True)
        (project / "boards" / directory / "sdkconfig.defaults").write_text(f"CONFIG_HG_BOARD_{symbol}=y\n")
    files = {"firmware/esp32/platformio.ini": INI, "firmware/esp32/main/board.cpp": BOARD_CPP,
             "firmware/esp32/main/Kconfig.projbuild": "config HG_BOARD_ALPHA\n    bool \"Alpha\"\n",
             "firmware/esp32/sdkconfig.defaults": "CONFIG_A=y\n", "firmware/core/src/app.cpp": "int app;\n",
             "firmware/tests/test_app.cpp": "int test;\n", "plugin/cli.py": "x = 1\n"}

    def git(*args):
        subprocess.run(["git", "-C", str(tmp_path), "-c", "user.name=t", "-c", "user.email=t@example.com",
                        "-c", "core.autocrlf=false", *args], check=True, capture_output=True)

    def write(path, text):
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / path).write_text(text, encoding="utf-8", newline="")

    git("init", "-q")
    for path, text in files.items():
        write(path, text)
    git("add", "-A")
    git("commit", "-q", "-m", "base")
    monkeypatch.setattr(ci_boards, "REPO", tmp_path)

    def change(edits: dict[str, str | None]) -> dict[str, list[str]]:
        """Commit the edits (None deletes) and return the boards selected against the first commit."""
        for path, text in edits.items():
            if text is None:
                (tmp_path / path).unlink()
            else:
                write(path, text)
        git("add", "-A")
        git("commit", "-q", "-m", "change")
        return ci_boards.select(project, "HEAD~1")

    change.project = project
    return change


SHARED = {"pio": ["esp32s3-breadboard"], "idf": []}  # the boards of ci_boards.SHARED this repository has


def test_without_a_base_every_board_is_built(repo):
    assert ci_boards.select(repo.project, None) == {"pio": ["alpha", "beta-env", "esp32s3-breadboard"], "idf": ["p4"]}


def test_a_boards_own_directory_selects_that_board_by_its_environment_name(repo):
    assert repo({"firmware/esp32/boards/beta/board.json": "{}"}) == {"pio": ["beta-env"], "idf": []}
    assert repo({"firmware/esp32/boards/p4/board.json": "{}"}) == {"pio": [], "idf": ["p4"]}


def test_a_line_in_a_boards_block_selects_the_boards_the_block_names(repo):
    assert repo({"firmware/esp32/main/board.cpp": BOARD_CPP.replace("return 5;", "return 6;")}) == \
        {"pio": ["alpha"], "idf": []}
    assert repo({"firmware/esp32/main/board.cpp": BOARD_CPP.replace("return 5;", "return 6;").replace('"beta"', '"b"')}) == \
        {"pio": ["beta-env"], "idf": ["p4"]}


def test_shared_code_selects_the_representative_boards(repo):
    assert repo({"firmware/core/src/app.cpp": "int app = 2;\n"}) == SHARED
    assert repo({"firmware/esp32/main/board.cpp": BOARD_CPP.replace("return 1;", "return 3;")}) == SHARED
    # A deleted line counts where it stood: here inside alpha's block, with shared code changed too.
    both = BOARD_CPP.replace("return 1;", "return 3;").replace("  return 5;\n", "").replace("return 3;", "return 7;")
    assert repo({"firmware/esp32/main/board.cpp": both}) == {"pio": ["alpha", "esp32s3-breadboard"], "idf": []}
    assert repo({"firmware/core/src/app.cpp": None}) == SHARED


def test_a_new_boards_kconfig_and_environment_select_it(repo):
    selected = repo({
        "firmware/esp32/main/Kconfig.projbuild": "config HG_BOARD_ALPHA\n    bool \"Alpha\"\nconfig HG_BOARD_BETA\n",
        "firmware/esp32/platformio.ini": INI.replace("board = b", "board = b2"),
    })
    assert selected == {"pio": ["beta-env"], "idf": []}


def test_a_change_to_the_build_itself_selects_every_board(repo):
    everything = {"pio": ["alpha", "beta-env", "esp32s3-breadboard"], "idf": ["p4"]}
    assert repo({"firmware/esp32/sdkconfig.defaults": "CONFIG_A=n\n"}) == everything
    assert repo({"firmware/esp32/platformio.ini": INI.replace("espressif32", "espressif32@8")}) == everything
    assert repo({".github/workflows/ci.yml": "name: CI\n"}) == everything


def test_host_only_and_other_files_select_no_board(repo):
    assert repo({"firmware/tests/test_app.cpp": "int test = 2;\n", "plugin/cli.py": "x = 2\n"}) == {"pio": [], "idf": []}


def test_the_representative_boards_exist():
    every = ci_boards.boards(ci_boards.PROJECT_DIR)
    assert set(ci_boards.SHARED) <= set(every)
    assert {every[name]["build"] for name in ci_boards.SHARED} == {"pio", "idf"}
    assert all(board["symbol"] for board in every.values())
