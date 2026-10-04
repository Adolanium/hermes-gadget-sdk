# Developing the SDK

## Layout

```
plugin/                 Hermes platform plugin (installed into ~/.hermes/plugins/gadget)
python/hermes_gadget/   Tools: simulator host, development server, provisioning, CLI
firmware/core/          Portable device core (C++17), shared by the ESP32 and the simulator
firmware/sim/           C ABI wrapper the simulator loads (hgsim.dll / libhgsim.so)
firmware/esp32/         ESP-IDF application and board configurations
firmware/tests/         Core unit tests (no dependencies)
site/                   The browser installer (GitHub Pages): flashing, Wi-Fi setup, pairing
tests/                  Python tests: plugin units, simulator ↔ hub, adapter on Hermes, gateway E2E
docs/                   Architecture, protocol, integration, guides
tools/                  gen_mascot.py (mascot bitmaps), make_logo.py (the logo), capture_window.py (README screenshots of the simulator window)
assets/mascot/          The mascot master image and its attribution
```

## Test suites

| Suite | Command | Needs |
|---|---|---|
| Core (C++) | `hermes-gadget build-sim --test`, or `ctest --test-dir build/host -C Release` | CMake + compiler |
| Plugin units, simulator ↔ hub | `pytest` | Built simulator library (else skipped) |
| Adapter on real Hermes classes | `HERMES_AGENT_DIR=../hermes-agent ../hermes-agent/.venv/bin/python -m pytest tests/test_adapter_hermes.py` | A Hermes checkout and its virtualenv |
| Full gateway end to end | `HERMES_GADGET_E2E=1 pytest tests/test_gateway_e2e.py` | The above; spawns `hermes gateway run` with a temporary `HERMES_HOME` |
| Browser installer | `npm ci && npm test` in `site/` | Node.js 22 |

CI runs the last two rows in `.github/workflows/hermes.yml`: against the Hermes commit pinned there (`HERMES_REF`) on every push and pull request, and against Hermes `main` once a day. To move the pin, run that workflow by hand with `hermes_ref: main`. When it passes, put the commit it printed into `HERMES_REF` and the README's status line.

The Linux jobs run on `ubuntu-24.04` rather than `ubuntu-latest`, so a new runner image arrives as a deliberate change instead of a surprise. Dependabot proposes the workflows' actions, the installer's npm packages and the Python dependencies weekly, a week after each release.

The end-to-end test runs a real gateway with the plugin installed and a fake OpenAI-compatible server (`tests/fakes/fake_openai.py`) standing in for the model, STT and TTS. It then:

- pairs a simulated device with `hermes pairing approve`;
- sends text and voice;
- checks that transcripts, spoken replies and both agent tools reach the device.

It costs nothing and needs no API keys.

Cross-language guarantees:

- `tests/test_protocol.py` and `firmware/tests/test_basics.cpp` share identity and HMAC vectors.
- `test_sim_hub.py` runs the C++ core against the Python hub, including hostile handshakes.
- `test_installer_console.py` runs the installer's console client (`site/src/lib/console.js`) against the C++ core, through a model of ESP-IDF's console (`tests/fakes/esp_console.py`): its echo, its ASCII-only input and its argument splitting.

### The browser installer

`site/` is a static page with no server, published to GitHub Pages by `.github/workflows/pages.yml` with the latest release's firmware. It flashes boards with [esptool-js](https://github.com/espressif/esptool-js) over Web Serial (Chrome and Edge), checks the chip against the release manifest first, and then talks to the firmware's serial console to set Wi-Fi, the Hermes address and the name. To try it locally with your own builds:

```bash
cd site && npm ci
node scripts/build.mjs --firmware ../firmware/esp32/dist   # from tools/package_release.py
python -m http.server 8000 --directory _site               # then open http://localhost:8000
```

Web Serial needs a secure context, which `localhost` counts as.

## Working on the plugin against a live Hermes

```bash
hermes-gadget plugin install --link   # symlink, so edits apply after a gateway restart
hermes gateway run
```

`--link` falls back to copying where symlinks need extra privileges (Windows without Developer Mode). Re-run the install after editing in that case.

If your Hermes is a source checkout whose virtualenv Python differs from the Python Hermes's package manager prepares for plugins, `hermes plugins enable` can create an environment the gateway cannot import from. Adding `plugins: {enabled: [gadget]}` to `config.yaml` directly avoids that; the plugin has no Python dependencies of its own.

## Working on the firmware

Almost everything in `firmware/core` can be developed with the simulator and the core tests:

1. Change `firmware/core`.
2. Run `hermes-gadget build-sim --test`.
3. Run `hermes-gadget sim`.
4. Flash hardware only for driver work in `firmware/esp32/main/port_*.cpp`.

### Build checks

Two checks stop a firmware build that would run with settings nobody asked for. Both run in every PlatformIO build, locally and in CI:

- **`tools/check_config.py`**, from `CMakeLists.txt` after the configuration is generated (so `idf.py` builds run it too). It fails when a line of `sdkconfig.defaults` or the board's defaults didn't reach the generated sdkconfig. That happens when a symbol is misspelled, belongs to another chip, or has an unmet dependency, and when an sdkconfig from an older checkout outlives a change to the defaults. In the last case, delete `sdkconfig.<board>` (PlatformIO) or the build directory, then build again. A board file that picks another option of a choice the base file sets says so with `# CONFIG_<base option> is not set`.
- **`tools/check_size.py`**, after linking (`tools/pio_checks.py`). It fails when the app leaves less than 10% of its smallest app partition free. For `idf.py` builds, run it by hand: `python tools/check_size.py --app build/hermes_gadget.bin --partitions build/partition_table/partition-table.bin`.

### Releases

`tools/package_release.py` turns PlatformIO builds into release files. For each board:

- **`hermes-gadget-<board>-<version>.bin`:** bootloader, partition table, OTA data and app in one image, from address 0. It is byte for byte what `esptool merge_bin` makes from the same build. Flashed at `0x0`, it also erases the device's settings.
- **`hermes-gadget-<board>-<version>-app.bin`:** the app alone, for `hermes gadget update`.

It also writes `SHA256SUMS` and `manifest.json`, which tells the browser installer each board's chip, flash size and PSRAM, and where the settings partition is. The installer writes everything around the settings, so a device you reinstall keeps its Wi-Fi, server and key. CI packages every board on every change and keeps the files as workflow artifacts.

To publish a release:

1. Set the new version in all four files that carry it: `PROJECT_VER` in `firmware/esp32/CMakeLists.txt`, `pyproject.toml`, `python/hermes_gadget/__init__.py` and `plugin/plugin.yaml`. `python tools/check_versions.py` says whether they agree, and CI fails when they don't.
2. Merge, then push a tag: `git tag v0.2.0 && git push origin v0.2.0`.
3. The **Release** workflow checks the four files against the tag, builds every board in `platformio.ini`, and publishes the files as a GitHub release. Its notes include the command that installs the plugin from the same commit (`hermes plugins install … --ref <commit>`).

To package local builds: `pio run && python tools/package_release.py --all --out dist`.

Conventions:

- **The core is single-threaded.** Ports marshal driver events onto the app thread; never call `hg::App` from an ISR or another task.
- **No exceptions, no RTTI.** Prefer `std::string`, `std::vector` and `std::function`; avoid per-frame heap churn in hot paths (mic, audio, rendering).
- **The UI is deterministic.** Anything time-based goes through `UiModel::frame`, which the app advances.
- **New protocol messages** are additive. Update [protocol.md](protocol.md), the route table in `app.cpp`, the hub's `_routes`, and tests on both sides.
