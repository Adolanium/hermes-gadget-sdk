"""Shared fixtures.

Runs from a plain checkout: the SDK packages are imported from the source tree
(no install needed), the simulator library is used when it has been built
(``hermes-gadget build-sim``), and Hermes-dependent tests run when a Hermes
Agent checkout is importable (``HERMES_AGENT_DIR``, default ``../hermes-agent``).
"""

from __future__ import annotations

import asyncio
import importlib.util
import os
import sys
import tempfile
import threading
import time
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "python"))
sys.path.insert(0, str(REPO / "tests"))


def _ensure_plugin_package() -> None:
    """Expose plugin/ as ``hermes_gadget_plugin`` when the SDK is not pip-installed."""
    if "hermes_gadget_plugin" in sys.modules:
        return
    try:
        import hermes_gadget_plugin  # noqa: F401
        return
    except ImportError:
        pass
    plugin_dir = REPO / "plugin"
    spec = importlib.util.spec_from_file_location(
        "hermes_gadget_plugin", plugin_dir / "__init__.py", submodule_search_locations=[str(plugin_dir)])
    module = importlib.util.module_from_spec(spec)
    sys.modules["hermes_gadget_plugin"] = module
    spec.loader.exec_module(module)


_ensure_plugin_package()


def _hermes_available() -> bool:
    agent_dir = Path(os.environ.get("HERMES_AGENT_DIR", REPO.parent / "hermes-agent"))
    if not (agent_dir / "gateway" / "platforms" / "base.py").exists():
        return False
    os.environ.setdefault("HERMES_HOME", tempfile.mkdtemp(prefix="hg-test-home-"))
    if str(agent_dir) not in sys.path:
        sys.path.append(str(agent_dir))
    try:
        import gateway.platforms.base  # noqa: F401
    except Exception:
        return False
    return True


HERMES_AVAILABLE = _hermes_available()


def sim_library_available() -> bool:
    from hermes_gadget import paths

    return paths.find_sim_library() is not None


requires_sim = pytest.mark.skipif(not sim_library_available(), reason="simulator library not built (hermes-gadget build-sim)")
requires_hermes = pytest.mark.skipif(not HERMES_AVAILABLE, reason="Hermes Agent checkout not importable (set HERMES_AGENT_DIR)")


class LoopThread:
    """An asyncio loop on a background thread, for servers the sim talks to."""

    def __init__(self) -> None:
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()

    def run(self, coro, timeout: float = 15.0):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    def stop(self) -> None:
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(timeout=5)


@pytest.fixture
def loop_thread():
    lt = LoopThread()
    yield lt
    lt.stop()


@pytest.fixture
def devserver(loop_thread, tmp_path):
    """The real device hub with the development brain, on an ephemeral port."""
    from hermes_gadget.devserver import EchoBrain
    from hermes_gadget_plugin.hub import DeviceHub
    from hermes_gadget_plugin.store import DeviceStore

    def make(require_pairing: bool = False, token: str | None = None):
        brain = EchoBrain(require_pairing=require_pairing, word_delay=0.0)
        hub = DeviceHub(DeviceStore(tmp_path / "server"), brain, host="127.0.0.1", port=0,
                        access_token=token, heartbeat_s=5)
        brain.hub = hub
        loop_thread.run(hub.start())
        made.append(hub)
        return hub, brain, f"ws://127.0.0.1:{hub.bound_port}/gadget"

    made: list = []
    yield make
    for hub in made:
        loop_thread.run(hub.stop())


@pytest.fixture
def make_sim(tmp_path):
    from hermes_gadget.sim import Simulator

    sims: list = []

    def make(url: str, name: str = "Test Gadget", board: str = "sim-320x240", state: str = "device", **kw):
        sim = Simulator(url=url, name=name, board=board, state_dir=tmp_path / state, **kw)
        sims.append(sim)
        sim.start()
        return sim

    yield make
    for sim in sims:
        sim.close()
    time.sleep(0.05)
