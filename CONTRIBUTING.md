# Contributing

Thanks for helping. Bug reports from real boards, new board support, fixes and documentation are all welcome.

## Reporting a bug

Open an [issue](https://github.com/Adolanium/hermes-gadget-sdk/issues/new/choose) and say which board, firmware version and Hermes version you're on. For anything on a device, attach a diagnostics report:

```bash
hermes-gadget diag --port COM5      # or /dev/ttyACM0, /dev/cu.usbmodem101, ...
```

The [browser installer](https://adolanium.github.io/hermes-gadget-sdk/) saves the same report without Python: **Troubleshooting** → **Something else** → **Save a diagnostics report**. If the installer was involved, the **Technical details** log at the bottom of its page helps too.

Security problems go through [SECURITY.md](SECURITY.md) instead, not a public issue.

## Setting up

```bash
pip install -e ".[dev]"
hermes-gadget build-sim --test        # the C++ core and its tests (needs CMake and a C++17 compiler)
pytest                                # Python tests; Hermes ones skip without a Hermes checkout
```

[docs/development.md](docs/development.md) has every test suite, including the ones that run against a real Hermes, the firmware build checks and the browser installer's tests. Most firmware work can be done in the simulator; flash a board for driver changes.

## Adding a board

[docs/porting.md](docs/porting.md) walks through it. A complete board PR has:

- [ ] `firmware/esp32/main/board.cpp`: the board's pins and parts, and its name in `HG_BOARD_NAME`
- [ ] `firmware/esp32/main/Kconfig.projbuild`: a choice for it
- [ ] `firmware/esp32/boards/<board>/sdkconfig.defaults`: target, flash size, PSRAM mode and the board choice. The build checks that every line took effect.
- [ ] `firmware/esp32/boards/<board>/board.json`: the name and one-line description the browser installer shows
- [ ] `firmware/esp32/platformio.ini`: an environment for it, which also puts it in the next release
- [ ] `.github/workflows/ci.yml`: the environment in the firmware build matrix
- [ ] `docs/hardware.md`: a section with the pins and a first-flash checklist
- [ ] `docs/supported-hardware.md`: the board's controls, audio, and setup link
- [ ] `docs/hardware-validation.md`: capabilities, exact revision, and experimental or verified status
- [ ] Third-party drivers: upstream source/version, entry in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), required license texts and notices in source and release packages
- [ ] Optionally, a simulator profile with the same screen in `python/hermes_gadget/sim/runner.py`

Use the [physical test checklist](docs/hardware-validation.md#record-a-physical-test) and link the report in the PR. A port without a complete report stays experimental.

## Pull requests

- **Keep each PR to one change.** A title like `Board: add the Waveshare ESP32-S3-LCD-1.54` or `Plugin: show the firmware version in hermes gadget devices` says what it does.
- **Say why, then what, then how you know it works.** Tests you ran, boards you tried it on, anything you couldn't check.
- **CI must pass before a PR is merged.** `main` is protected. For a first-time contributor, CI waits until a maintainer approves the run.
- **Update the docs** that describe what you changed.
- **Code style:** match the code around you. The core's conventions (single-threaded, no exceptions or RTTI, deterministic UI) are in [docs/development.md](docs/development.md#working-on-the-firmware).

### Choose the relevant documentation and checks

| Change | Documentation to review | Verification |
|---|---|---|
| Board or hardware driver | Complete the board checklist above and review [driver requirements](docs/porting.md#check-a-hardware-driver) | Build the affected firmware with this project's ESP-IDF version. Shared driver changes must pass every firmware profile in CI. Record physical checks separately. |
| CLI or simulator behavior | Update the command examples and the relevant [desktop](docs/desktop.md), [simulator](docs/simulator.md), or feature guide | Exercise the changed command or interaction, including a regression test when fixing a bug. |
| Plugin, protocol, or Linux client | Review [Hermes setup](docs/connect-hermes.md), [protocol](docs/protocol.md), and [Linux](docs/linux.md) instructions as applicable | Run the relevant integration tests. Say whether a real Hermes gateway, peripherals, or test doubles were used. |
| Website or published guide | Keep setup instructions and links consistent; register new guides in `site/scripts/docs.mjs` for navigation and search | Run the site tests and build; run browser tests for interaction or layout changes. |
| Dependency or vendor-derived material | Update `NOTICE`, `THIRD_PARTY_NOTICES.md`, and required texts under `LICENSES/` | Check that distribution packages contain the required notices and licenses. |

User-facing behavior changes need documentation in the same PR. If no documentation changes are needed, explain why in the PR description. Internal refactors and test-only fixes do not need unrelated prose edits. Keep the README brief and put detailed instructions in the relevant guide.

Report exact commands and outcomes, including failed or skipped checks and their reasons. For bugs, show the failing behavior before the fix and the expected behavior afterward where practical. Tests should exercise observable results rather than merely check that a helper was called.

Treat physical testing as separate evidence: identify the board revision, firmware commit, and functions tested. A successful compilation or simulator run does not establish hardware verification. Keep incomplete ports experimental and follow the physical checklist.

Preserve contributor authorship when updating another person's PR. Credit salvaged contributions in the resulting PR and commit history.

## Licensing

Contributions are under the [MIT license](LICENSE). Don't copy code from other projects. Hardware facts such as pin numbers and register values from a vendor's documentation are fine; say where they came from so they can be credited in [NOTICE](NOTICE).

When incorporating manufacturer drivers or adapting vendor initialization data, record the upstream URL and pinned version or commit, its license, and what was adapted. Retain applicable copyright notices and license texts in source and release packages. A link to a moving branch alone is not enough to identify the version used.
