# Developing the SDK

## Layout

```
plugin/                 Hermes platform plugin (installed into ~/.hermes/plugins/gadget)
python/hermes_gadget/   Tools: simulator host, development server, provisioning, CLI
firmware/core/          Portable device core (C++17), shared by the ESP32 and the simulator
firmware/sim/           C ABI wrapper the simulator loads (hgsim.dll / libhgsim.so)
firmware/esp32/         ESP-IDF application and board configurations
firmware/tests/         Core unit tests (no dependencies)
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

The end-to-end test runs a real gateway with the plugin installed and a fake OpenAI-compatible server (`tests/fakes/fake_openai.py`) standing in for the model, STT and TTS. It then:

- pairs a simulated device with `hermes pairing approve`;
- sends text and voice;
- checks that transcripts, spoken replies and both agent tools reach the device.

It costs nothing and needs no API keys.

Cross-language guarantees:

- `tests/test_protocol.py` and `firmware/tests/test_basics.cpp` share identity and HMAC vectors.
- `test_sim_hub.py` runs the C++ core against the Python hub, including hostile handshakes.

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

Conventions:

- **The core is single-threaded.** Ports marshal driver events onto the app thread; never call `hg::App` from an ISR or another task.
- **No exceptions, no RTTI.** Prefer `std::string`, `std::vector` and `std::function`; avoid per-frame heap churn in hot paths (mic, audio, rendering).
- **The UI is deterministic.** Anything time-based goes through `UiModel::frame`, which the app advances.
- **New protocol messages** are additive. Update [protocol.md](protocol.md), the route table in `app.cpp`, the hub's `_routes`, and tests on both sides.
