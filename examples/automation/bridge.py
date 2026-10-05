"""Run network examples beside the production Linux device core."""

import argparse
import math
import signal
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from hermes_gadget.linux.client import Client, load_config
from hermes_gadget.linux.control import ControlServer, device_lock


def temperature(value):
    if isinstance(value, bool):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) and -100 <= number <= 150 else None


class Bridge:
    def __init__(self, device, backend):
        self.device, self.backend = device, backend
        self.pool = ThreadPoolExecutor(max_workers=2)
        self.reading = self.action = None
        self.next_read = 0
        self.result = {}
        device.add_action("lamp.set", "Queue an on/off request for the configured lamp. "
                          "Acceptance is not completion; check automation.status with the returned job.",
                          {"type": "object", "properties": {"on": {"type": "boolean"}},
                           "required": ["on"], "additionalProperties": False}, self.set_lamp)
        device.add_action("automation.status", "Read the latest lamp job's completion or failure.",
                          {"type": "object", "properties": {"job": {"type": "string"}},
                           "required": ["job"], "additionalProperties": False}, self.status)
        device.set_sensor("temperature_available", 0)

    def set_lamp(self, args):
        if set(args) != {"on"} or type(args["on"]) is not bool:
            raise ValueError("expected only on: true or false")
        if self.action is not None:
            raise ValueError("a lamp request is still pending")
        job = uuid.uuid4().hex
        self.result = {"job": job, "state": "pending"}
        self.action = self.pool.submit(self.backend.set_lamp, args["on"])
        return {"accepted": True, "job": job}

    def status(self, args):
        if set(args) != {"job"} or args["job"] != self.result.get("job"):
            raise ValueError("unknown job; only the latest job is retained")
        return dict(self.result)

    def step(self):
        if self.action is not None and self.action.done():
            try:
                result = self.action.result()
                self.result.update(state="completed", result=result)
            except Exception:
                # Network exception text can contain URLs or credentials.
                self.result.update(state="failed", error="request failed or timed out; outcome may be unknown")
            self.action = None
            self.device.emit_event("automation.completed", dict(self.result), notify=False)
        if self.reading is not None and self.reading.done():
            try:
                value = temperature(self.reading.result())
            except Exception:
                value = None
            self.device.set_sensor("temperature_available", int(value is not None))
            if value is not None:
                self.device.set_sensor("temperature_c", value)
            self.reading = None
        if self.reading is None and time.monotonic() >= self.next_read:
            self.next_read = time.monotonic() + 30
            self.reading = self.pool.submit(self.backend.read_temperature)

    def close(self):
        self.pool.shutdown(wait=True, cancel_futures=True)


def parser(description):
    result = argparse.ArgumentParser(description=description)
    result.add_argument("--config", type=Path, required=True, help="Linux gadget JSON configuration")
    result.add_argument("--state-dir", type=Path, required=True, help="Private identity and control socket directory")
    return result


def run(args, backend):
    stop = threading.Event()
    previous = {sig: signal.signal(sig, lambda *_: stop.set()) for sig in (signal.SIGINT, signal.SIGTERM)}
    try:
        with device_lock(args.state_dir):
            client = Client(load_config(args.config), args.state_dir)
            bridge = server = None
            try:
                bridge = Bridge(client.device, backend)
                client.start()
                server = ControlServer(args.state_dir / "control.sock", client)
                while not stop.is_set() and client.running:
                    client.step()
                    bridge.step()
                    server.poll()
            finally:
                if server:
                    server.close()
                if bridge:
                    bridge.close()
                client.close()
    finally:
        backend.close()
        for sig, handler in previous.items():
            signal.signal(sig, handler)
