"""Optional Raspberry Pi controls and explicitly configured digital outputs."""

from contextlib import ExitStack

from ..sim.native import BUTTONS


class Gpio:
    def __init__(self, config: dict, device, *, pin_factory=None):
        from gpiozero import LED, Button

        self.resources = ExitStack()
        self.buttons = {}
        self.outputs = {}
        self.status_led = None
        self.device = device
        try:
            if pin_factory is None:
                from gpiozero.pins.lgpio import LGPIOFactory

                pin_factory = LGPIOFactory(chip=config.get("chip", 0))
                self.resources.callback(pin_factory.close)
            for name in ("talk", "cancel"):
                if name in config:
                    button = self.resources.enter_context(Button(config[name], pull_up=True, pin_factory=pin_factory))
                    self.buttons[name] = {"button": button, "raw": False, "pressed": False, "changed": 0}
            if "status_led" in config:
                self.status_led = self.resources.enter_context(LED(config["status_led"], pin_factory=pin_factory))
            for name, pin in config.get("outputs", {}).items():
                output = self.resources.enter_context(LED(pin, initial_value=False, pin_factory=pin_factory))
                self.outputs[name] = output

                def set_output(args, output=output):
                    on = args.get("on")
                    if not isinstance(on, bool):
                        raise TypeError("on must be a boolean")
                    output.value = on
                    return {"on": bool(output.value)}

                device.add_action(f"gpio.{name}", f"Switch the configured {name} output.",
                                  {"type": "object", "properties": {"on": {"type": "boolean"}},
                                   "required": ["on"], "additionalProperties": False}, set_output)
        except Exception:
            self.close()
            raise

    def step(self, now_ms: int, status: dict) -> None:
        for name, state in self.buttons.items():
            pressed = state["button"].is_pressed
            if pressed != state["raw"]:
                state.update(raw=pressed, changed=now_ms)
            if pressed != state["pressed"] and now_ms - state["changed"] >= 30:
                state["pressed"] = pressed
                self.device.button(BUTTONS[name], pressed)
        if self.status_led is not None:
            online = status.get("phase") == "online" and status.get("paired")
            listening = status.get("screen") == "listening"
            self.status_led.value = (now_ms // (125 if listening else 500)) % 2 if listening or not online else 1

    def close(self) -> None:
        try:
            for output in self.outputs.values():
                output.off()
            if self.status_led is not None:
                self.status_led.off()
        finally:
            self.resources.close()
