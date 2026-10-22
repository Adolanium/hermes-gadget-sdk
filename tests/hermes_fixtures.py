"""Hermes registration and adapter fixtures, shared by the adapter suites."""
import inspect
import types

import pytest

PAIRING_TEXT = ("Hi! I don't recognise you yet. Your pairing code is ABCD2345 (valid for 1 hour). "
                "Ask the owner to run: hermes pairing approve gadget ABCD2345")


class _Ctx:
    """The slice of Hermes's PluginContext that ``register(ctx)`` uses; registers the platform for
    real (PlatformEntry rejects unknown keyword arguments, so this also pins our register() call)."""

    def __init__(self):
        self.tools, self.cli, self.hooks = [], [], []

    def register_platform(self, name, label, adapter_factory, check_fn, validate_config=None,
                          required_env=None, install_hint="", **kw):
        from gateway.platform_registry import PlatformEntry, platform_registry

        platform_registry.register(PlatformEntry(
            name=name, label=label, adapter_factory=adapter_factory, check_fn=check_fn,
            validate_config=validate_config, required_env=required_env or [], install_hint=install_hint,
            source="builtin", **kw))

    def register_tool(self, **kw):
        self.tools.append(kw["name"])

    def register_cli_command(self, **kw):
        self.cli.append(kw["name"])

    def register_hook(self, name, callback):
        from hermes_cli.plugins import VALID_HOOKS

        assert name in VALID_HOOKS, name  # a hook this Hermes doesn't fire would never be called
        self.hooks.append(name)


@pytest.fixture(scope="module")
def registered_platform():
    import hermes_gadget_plugin

    ctx = _Ctx()
    hermes_gadget_plugin.register(ctx)
    assert set(ctx.tools) == {"gadget_devices", "gadget_display", "gadget_action"}
    assert ctx.cli == ["gadget"]
    assert "pre_llm_call" in ctx.hooks and "pre_approval_request" in ctx.hooks
    return ctx


@pytest.fixture
def gadget(registered_platform, loop_thread, tmp_path, monkeypatch, request):
    import plugins.plugin_storage as storage
    from gateway.config import PlatformConfig
    from hermes_gadget_plugin.adapter import GadgetAdapter

    monkeypatch.setattr(storage, "plugin_data_dir", lambda name: tmp_path / "plugin-data" / name)
    import gateway.config as gateway_config

    saved_homes = []  # never write the developer's real config.yaml
    monkeypatch.setattr(gateway_config, "persist_home_channel", lambda home, **kw: saved_homes.append(home))
    monkeypatch.delenv("GADGET_HOME_CHANNEL", raising=False)
    extra = {"host": "127.0.0.1", "port": 0}
    if "hermes_home" in request.fixturenames:  # the profile suites: switching on, every served profile
        extra["profiles"] = "all"
    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra=extra))
    state = types.SimpleNamespace(adapter=adapter, authorized=set(), events=[], reply=lambda e: f"echo: {e.text}",
                                  saved_homes=saved_homes)

    adapter.set_authorization_check(
        lambda user_id, chat_type=None, chat_id=None, **kw: user_id in state.authorized)
    # The default profile's verdict, which profile switching trusts (no runner here to build it).
    adapter._owner_check = lambda user_id, chat_type=None, chat_id=None, **kw: user_id in state.authorized

    async def handler(event):
        # Stands in for the gateway runner: unauthorized senders get a pairing code.
        state.events.append(event)
        if event.source.user_id not in state.authorized:
            await adapter.send(event.source.chat_id, PAIRING_TEXT)
            return None
        reply = state.reply(event)
        return await reply if inspect.isawaitable(reply) else reply  # an async reply keeps the turn running

    adapter.set_message_handler(handler)
    assert loop_thread.run(adapter.connect())
    state.url = f"ws://127.0.0.1:{adapter.hub.bound_port}/gadget"
    state.run = loop_thread.run
    state.loop = loop_thread.loop
    yield state
    loop_thread.run(adapter.cancel_background_tasks())  # as the gateway does on shutdown
    loop_thread.run(adapter.disconnect())


def _paired_sim(gadget, make_sim, **kw):
    sim = make_sim(gadget.url, **kw)
    assert sim.wait_screen("pairing", "ready", timeout=10)
    gadget.authorized.add(sim.status()["device_id"])
    assert sim.wait_screen("ready", timeout=10)
    return sim


@pytest.fixture
def hermes_home(tmp_path, monkeypatch):
    """A multiplexed Hermes home serving 'default' and 'ops', with a parked 'old' profile that must not be offered."""
    import agent.secret_scope

    home = tmp_path / "home"
    for name in ("ops", "old"):
        (home / "profiles" / name).mkdir(parents=True)
        (home / "profiles" / name / "config.yaml").write_text("{}\n")  # identity marker
    (home / "profiles" / "old" / "gateway.parked").touch()
    monkeypatch.setenv("HERMES_HOME", str(home))
    monkeypatch.delenv("GADGET_ALLOWED_USERS", raising=False)  # restored after Hermes loads a profile's .env
    monkeypatch.setattr(agent.secret_scope, "is_multiplex_active", lambda: True)
    return home
