"""Private Unix socket controls; all core calls stay on the service thread."""

from __future__ import annotations

import json
import selectors
import socket
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path

from .client import Client

MAX_REQUEST = 16384
MAX_RESPONSE = 1 << 20


@contextmanager
def device_lock(directory: Path):
    if sys.platform != "linux":
        raise RuntimeError("the device service requires Linux")
    import fcntl

    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    directory.chmod(0o700)
    with (directory / "service.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("another gadget service is using this state directory") from None
        yield


class ControlServer:
    def __init__(self, path: Path, client: Client):
        self.path, self.client = path, client
        self.selector = selectors.DefaultSelector()
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.peers: dict[socket.socket, dict] = {}
        # The caller holds the state directory lock before removing a stale socket.
        path.unlink(missing_ok=True)
        try:
            self.listener.bind(str(path))
            path.chmod(0o600)
            self.listener.listen(8)
            self.listener.setblocking(False)
            self.selector.register(self.listener, selectors.EVENT_READ)
        except Exception:
            self.listener.close()
            self.selector.close()
            raise

    def _close_peer(self, peer: socket.socket) -> None:
        self.selector.unregister(peer)
        self.peers.pop(peer)
        peer.close()

    def poll(self, timeout: float = 0.02) -> None:
        for key, _events in self.selector.select(timeout):
            peer = key.fileobj
            if peer is self.listener:
                connection, _ = self.listener.accept()
                connection.setblocking(False)
                if len(self.peers) >= 16:
                    connection.close()
                    continue
                self.peers[connection] = {"input": bytearray(), "output": b"", "deadline": time.monotonic() + 2}
                self.selector.register(connection, selectors.EVENT_READ)
                continue
            state = self.peers[peer]
            try:
                if state["output"]:
                    sent = peer.send(state["output"])
                    state["output"] = state["output"][sent:]
                    if not state["output"]:
                        self._close_peer(peer)
                    continue
                data = peer.recv(4096)
                if not data:
                    self._close_peer(peer)
                    continue
                state["input"].extend(data)
                if len(state["input"]) > MAX_REQUEST:
                    self._close_peer(peer)
                    continue
                if b"\n" not in state["input"]:
                    continue
                try:
                    request = json.loads(state["input"].split(b"\n", 1)[0])
                    result = self.client.command(request)
                except (ValueError, TypeError) as exc:
                    result = {"error": str(exc)}
                state["output"] = json.dumps(result, allow_nan=False).encode() + b"\n"
                self.selector.modify(peer, selectors.EVENT_WRITE)
            except (ConnectionError, OSError):
                self._close_peer(peer)
        for peer, state in list(self.peers.items()):
            if time.monotonic() >= state["deadline"]:
                self._close_peer(peer)

    def close(self) -> None:
        for peer in list(self.peers):
            self._close_peer(peer)
        self.selector.close()
        self.listener.close()
        self.path.unlink(missing_ok=True)


def request(directory: Path, message: dict) -> dict:
    raw = json.dumps(message, allow_nan=False).encode() + b"\n"
    if len(raw) > MAX_REQUEST:
        raise ValueError("control request is too large")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(3)
        sock.connect(str(directory / "control.sock"))
        sock.sendall(raw)
        data = bytearray()
        while b"\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError("device service closed the connection")
            data.extend(chunk)
            if len(data) > MAX_RESPONSE:
                raise RuntimeError("control response is too large")
        return json.loads(data.split(b"\n", 1)[0])


def run(config: dict, directory: Path, stop: threading.Event) -> None:
    with device_lock(directory):
        client = Client(config, directory)
        try:
            client.start()
            server = ControlServer(directory / "control.sock", client)
            try:
                while not stop.is_set():
                    client.step()
                    server.poll()
            finally:
                server.close()
        finally:
            client.close()
