# Developing the SDK

## Layout

```
plugin/                 Hermes platform plugin (installed into ~/.hermes/plugins/gadget)
python/hermes_gadget/   Tools: simulator host, development server, provisioning, CLI
firmware/core/          Portable device core (C++17), shared by the ESP32 and the simulator
firmware/sim/           C ABI wrapper the simulator loads (hgsim.dll / libhgsim.so)
firmware/esp32/         ESP-IDF application and board configurations
firmware/tests/         Core unit tests (no dependencies)
site/                   Product homepage, hosted guides, and browser installer (GitHub Pages)
tests/                  Python tests: plugin units, simulator ↔ hub, adapter on Hermes, gateway E2E
docs/                   Architecture, protocol, integration, guides
tools/                  make_logo.py (the logo), check_versions.py (the release version check), capture_window.py (README screenshots of the simulator window)
assets/mascot/          The mascot master image and its attribution
```

## Test suites

| Suite | Command | Needs |
|---|---|---|
| Core (C++) | `hermes-gadget build-sim --test`, or `ctest --test-dir build/host -C Release` | CMake + compiler |
| Plugin units, simulator ↔ hub, desktop controls | `pytest` | Built simulator library; a desktop or Xvfb for window tests |
| Adapter on real Hermes classes | `HERMES_AGENT_DIR=../hermes-agent ../hermes-agent/.venv/bin/python -m pytest tests/test_adapter_hermes.py` | A Hermes checkout and its virtualenv |
| Full gateway end to end | `HERMES_GADGET_E2E=1 pytest tests/test_gateway_e2e.py` | The above; spawns `hermes gateway run` with a temporary `HERMES_HOME` |
| Browser installer | `npm ci && npm test` in `site/` | Node.js 22 |
| Home Assistant / MQTT examples | `pytest tests/test_automation_examples.py` | Built native library and the `mqtt` extra; local peers need no external accounts |

The Python jobs in `.github/workflows/ci.yml` run under `xvfb-run` so the desktop controls run against the simulator core and development hub. On a headless Linux machine, use `xvfb-run -a pytest`. Without a display, the window tests skip; set `HERMES_GADGET_UI_TESTS=1` to require them.

CI runs the adapter and gateway suites in `.github/workflows/hermes.yml`: against the Hermes commit pinned there (`HERMES_REF`) on every push and pull request, and against Hermes `main` once a day. To move the pin, run that workflow by hand with `hermes_ref: main`. When it passes, put the commit it printed into `HERMES_REF` and into `TESTED_HERMES_COMMIT` in `plugin/compat.py`; a test checks that the two agree.

The Raspberry Pi package installs its runtime dependencies from `linux/requirements.txt`, a lock with hashes compiled from `linux/requirements.in` (the `[audio,gpio]` extras). Change the extras in `pyproject.toml` and `requirements.in` together, then regenerate the lock as the file's header says; Dependabot proposes updates to it weekly.

The Linux jobs run on `ubuntu-24.04` rather than `ubuntu-latest`, so a new runner image arrives as a deliberate change instead of a surprise. Every action is pinned to a commit, with the release tag in a comment, and PlatformIO to a version, so a job runs the same code until a change says otherwise. Every job has a timeout. Dependabot proposes the workflows' actions (updating the pinned commits), the installer's npm packages and the Python dependencies weekly, a week after each release.

A pull request also needs a line in `CHANGELOG.md` under `## Unreleased`, checked by the `Changelog entry` job. Label the PR `no-changelog` when nothing user-facing changed (internal refactors, test-only fixes, dependency bumps).

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

`site/` contains the product homepage, browser installer, and documentation, published to GitHub Pages by `.github/workflows/pages.yml` with the latest release's firmware. The build renders `docs/*.md` into the hosted guides with `marked`; edit the Markdown rather than the generated pages. Search runs in the browser and sends no queries to a server.

The installer flashes boards with [esptool-js](https://github.com/espressif/esptool-js) over Web Serial (Chrome and Edge), checks the chip against the release manifest first, and then talks to the firmware's serial console to set Wi-Fi, the Hermes address, and the name. To try it locally with your own builds:

```bash
cd site && npm ci
node scripts/build.mjs --firmware ../firmware/esp32/dist   # from tools/package_release.py
python -m http.server 8000 --directory _site               # then open http://localhost:8000
```

Web Serial needs a secure context, which `localhost` counts as.

For homepage and docs work, `npm run build` works without firmware. Run `npm run preview` to serve it at `http://127.0.0.1:8768`. The installer is at `installer.html`; old root links with `#server=...` still reach it with the address intact.

Run `npm test` for the Node tests. After building, run `npx playwright install chromium` and `npm run test:browser` for browser tests. CI installs Chromium and runs both suites. Set `BROWSER_EXECUTABLE` to use an existing Chromium binary locally.

The site build checks relative links and Markdown section anchors in root Markdown files and `docs/*.md`, including Markdown images and quoted HTML `href`/`src` attributes. Fenced examples, external URLs, and URLs starting with `/` are not checked. Every top-level guide in `docs/` must be registered once in `site/scripts/docs.mjs`; that registration publishes the page in navigation and search. Failures name the source file and missing target or registration.

CI discovers firmware profiles from `firmware/esp32/platformio.ini` using the release packager's environment reader. Adding a profile includes it in the firmware build/package matrix without maintaining a second list. Firmware tooling tests also check installer metadata and verify that every file under `LICENSES/` reaches the release license archive.

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

### Measure frame timing

Before and after a renderer change, measure the same workload. The core times each screen update (`Ui::render`) with `System::now_us`. That is `esp_timer` on the ESP32 and the host's steady clock in the simulator. It records:

| Measurement | What it covers |
|---|---|
| `render_us` | Drawing into the framebuffer: the whole update minus the flushes |
| `flush_us` | Time inside `Display::flush`. On SPI panels this includes waiting for each band's DMA to finish |
| `frame_us` | The whole update, drawing and flushing |
| `bands` | Regions sent to the panel in one update |
| `input_us` | From `App::on_button` to the end of the first update after it that sent anything. A touch gesture enters when it becomes a button (a hold after 120 ms). Controller polling and the event queue come before that and are not included |

Each measurement keeps the last 256 updates (64 for `input_us`) and reports `min`, `avg`, `p50`, `p95` and `max`. `frames` counts updates that sent something since the last reset. `missed` counts the ones longer than the budget, 16667 µs (one 60 Hz refresh) by default. `idle` counts updates that had nothing to send; their timings are not recorded.

Timing is off by default. While it is off, the renderer never reads the clock: one check per update is the only cost.

Run the bench in the simulator. It needs no Hermes:

```bash
hermes-gadget build-sim
hermes-gadget sim --headless --script examples/scripts/perf.txt
```

On a board, open `hermes-gadget console --port <port>` and run the same commands:

| Command | Effect |
|---|---|
| `perf bench [scroll \| mascot \| redraw \| all]` | Draws a fixed workload, 120 updates each, one per app tick, with the device's own bars. `scroll` moves a 60-paragraph reply one line per update, `mascot` animates the talking mascot, and `redraw` repaints the whole screen every update. `all` runs the three in turn. The normal screen returns afterwards |
| `perf` | `@perf` JSON: the live statistics while timing is on, and the last bench under `bench` |
| `perf on` / `perf off` | Time the device's own screen updates |
| `perf reset` | Clear the live statistics |
| `perf budget <us>` | The budget for `missed`, until restart |

The bench draws the same frames on every run, so compare its numbers between builds on the same board or computer. While timing is on, the log gets a summary line every 10 seconds when something was drawn, for example `perf: frames 120 (missed 0 > 16.7 ms, idle 4), ms p50/p95/max: render ...`. The `diag` report includes the live statistics under `frames`. On the ESP32, `CONFIG_HG_FRAME_STATS` turns timing on from boot, and `CONFIG_HG_FRAME_BUDGET_US` and `CONFIG_HG_FRAME_LOG_S` set the budget and the log interval (menuconfig: Hermes Gadget).

Simulator numbers measure the core on your computer, not the board's CPU or panel. Label each result as simulator or hardware, and name the board and build when you report it.

### Translate the device text

The text the device shows by itself is listed once, with its English, in `firmware/core/include/hg/i18n.hpp` (`HG_I18N_MESSAGES`). Each other language is a table in `firmware/core/src/i18n_<code>.cpp` that maps message ids to text; `i18n_es.cpp` is the example to copy. To add a language:

1. Copy `i18n_es.cpp` to `i18n_<code>.cpp`, rename its table, and translate each line. Keep `{0}`, `{1}` and the rest: the device fills them with a button name, a number or a code, in the order the sentence needs. Write the text in UTF-8 with its accents; until the font has them, the device shows the nearest ASCII letter.
2. Add the language to `Lang` in `i18n.hpp`, and its code, its name in that language and its table to `kLangs` and `table_for()` in `i18n.cpp`.
3. Add the file to `HG_CORE_SOURCES` in `firmware/core/CMakeLists.txt`.
4. Run `hermes-gadget build-sim --test`. The tests fail when a message has no translation, when a translation drops or adds a placeholder, and when a hint or headline no longer fits on a screen where the English fits. Shorten it until it does: a hint cut with `..` on a small screen is worse than a terser phrase.
5. Look at the screens in the simulator with `set language <code>` on its console, on a small board profile such as `sim-240x240-round`.

A message a language leaves out shows in English, so a new message in `i18n.hpp` doesn't break the other languages; the completeness test says which are missing. Messages from Hermes are not in the catalogue.

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

1. Set the new version in all four files that carry it: `PROJECT_VER` in `firmware/esp32/CMakeLists.txt`, `pyproject.toml`, `python/hermes_gadget/__init__.py` and `plugin/plugin.yaml`, and move the `## Unreleased` entries in `CHANGELOG.md` under a `## <version>` heading. `python tools/check_versions.py --tag v<version>` says whether they agree and the section exists.
2. Merge, wait for CI and the Hermes run to pass on `main`, then tag that commit: `git tag v0.2.0 && git push origin v0.2.0`.
3. The **Release** workflow refuses a commit without successful CI and Hermes runs, checks the four files and the changelog against the tag, builds every board in `platformio.ini`, and publishes the files as a GitHub release. Its notes open with the changelog section and include the command that installs the plugin from the same commit (`hermes plugins install … --ref <commit>`).

To package local builds: `pio run && python tools/package_release.py --all --out dist`.

Each package also includes `hermes-gadget-<version>-licenses.zip`. It contains `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/`, and license/notice files from the installed managed components and ESP-IDF source. Packaging finds ESP-IDF through `IDF_PATH`, or the PlatformIO framework directory under `PLATFORMIO_CORE_DIR`/`~/.platformio`. Keep the matching dependency sources installed when packaging release binaries. The archive has a checksum in the manifest and `SHA256SUMS`; the site copies it and links it from the installer. When adding a driver, update [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) and [NOTICE](../NOTICE) in the same PR.

Conventions:

- **The core is single-threaded.** Ports marshal driver events onto the app thread; never call `hg::App` from an ISR or another task.
- **No exceptions, no RTTI.** Prefer `std::string`, `std::vector` and `std::function`; avoid per-frame heap churn in hot paths (mic, audio, rendering).
- **The UI is deterministic.** Anything time-based goes through `UiModel::frame`, which the app advances.
- **New protocol messages** are additive. Update [protocol.md](protocol.md), the route table in `app.cpp`, the hub's `_routes`, and tests on both sides.
