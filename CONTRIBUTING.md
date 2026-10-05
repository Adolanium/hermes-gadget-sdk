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

## Licensing

Contributions are under the [MIT license](LICENSE). Don't copy code from other projects. Hardware facts such as pin numbers and register values from a vendor's documentation are fine; say where they came from so they can be credited in [NOTICE](NOTICE).
