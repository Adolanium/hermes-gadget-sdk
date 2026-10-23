"""Device selection policy for Hermes's native ingress/replay hook."""
import pytest
from conftest import requires_hermes

pytestmark = [requires_hermes, pytest.mark.usefixtures("registered_platform")]


def _adapter_and_source(hermes_home, tmp_path):
    from gateway.config import Platform, PlatformConfig
    from gateway.session import SessionSource
    from hermes_gadget_plugin.adapter import GadgetAdapter
    from hermes_gadget_plugin.store import DeviceStore

    adapter = GadgetAdapter(PlatformConfig(enabled=True, extra={"profiles": "all"}))
    adapter._store = DeviceStore(tmp_path / "devices")
    adapter._owner_check = lambda user, *args: user == "hg-cccccccccccccccc"
    source = SessionSource(platform=Platform("gadget"), chat_id="hg-cccccccccccccccc",
                           user_id="hg-cccccccccccccccc", chat_type="dm", profile="ops")
    adapter._store.bind_to_owner(source.user_id)
    return adapter, source


def test_native_route_retains_selected_profile_but_rechecks_policy(hermes_home, tmp_path):
    from gateway.run import _profile_runtime_scope

    adapter, source = _adapter_and_source(hermes_home, tmp_path)
    with _profile_runtime_scope(hermes_home / "profiles" / "ops", hydrate_secrets=False):
        # No live connection is required to restore a durable queued message.
        assert adapter.validate_native_profile_route(source) is True
        assert source.profile == "ops"
        adapter._owner_check = lambda *args: False
        assert adapter.validate_native_profile_route(source) is False
        adapter._owner_check = lambda *args: True
        adapter._offer_all, adapter._offered = False, {"default"}
        assert adapter.validate_native_profile_route(source) is False


@pytest.mark.parametrize("case", ["off", "secondary", "unbound", "no_store", "wrong_user",
                                  "group", "relay", "rejected", "unknown", "parked"])
def test_native_route_refuses_invalid_selection(hermes_home, tmp_path, case):
    from hermes_gadget_plugin.store import DeviceStore

    adapter, source = _adapter_and_source(hermes_home, tmp_path)
    if case == "off":
        adapter._offer_all, adapter._offered = False, None
    elif case == "secondary":
        adapter._owner_profile = "ops"
    elif case == "unbound":
        adapter._store = DeviceStore(tmp_path / "unbound")
    elif case == "no_store":
        adapter._store = None
    elif case == "wrong_user":
        source.user_id = "hg-dddddddddddddddd"
    elif case == "group":
        source.chat_type = "group"
    elif case == "relay":
        source.delivered_via_upstream_relay = True
    elif case == "rejected":
        source.profile_route_rejected = True
    elif case == "unknown":
        source.profile = "missing"
    elif case == "parked":
        (hermes_home / "profiles" / "ops" / "gateway.parked").touch()
    assert adapter.validate_native_profile_route(source) is False
