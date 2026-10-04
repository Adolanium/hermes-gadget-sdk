"""WebSocket transport for the simulator.

Runs an asyncio loop on a background thread; inbound frames and connection
events are queued for the simulator thread (the device core is single
threaded, exactly like on hardware where the WebSocket task posts to the app
task).
"""

from __future__ import annotations

import asyncio
import logging
import queue
import threading
from dataclasses import dataclass
from typing import Any

log = logging.getLogger("hermes_gadget.sim.transport")


@dataclass
class Event:
    kind: str  # "open" | "text" | "binary" | "closed"
    generation: int
    data: Any = None


class WsTransport:
    def __init__(self) -> None:
        self.events: "queue.Queue[Event]" = queue.Queue()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, name="gadget-ws", daemon=True)
        self._thread.start()
        self._generation = 0
        self._outbox: asyncio.Queue | None = None
        self._ws = None
        self._lock = threading.Lock()

    @property
    def generation(self) -> int:
        return self._generation

    def connect(self, url: str, subprotocol: str) -> None:
        with self._lock:
            self._generation += 1
            gen = self._generation
        asyncio.run_coroutine_threadsafe(self._run(url, subprotocol, gen), self._loop)

    def close(self) -> None:
        with self._lock:
            self._generation += 1
        outbox = self._outbox
        if outbox is not None:
            self._loop.call_soon_threadsafe(outbox.put_nowait, None)

    def send(self, data: str | bytes) -> bool:
        outbox = self._outbox
        if outbox is None:
            return False
        self._loop.call_soon_threadsafe(outbox.put_nowait, data)
        return True

    def shutdown(self) -> None:
        """Close the socket with a proper close handshake, then stop the loop thread."""
        with self._lock:
            self._generation += 1
        if not self._loop.is_running():
            return
        try:
            asyncio.run_coroutine_threadsafe(self._drain(), self._loop).result(5)
        except Exception:
            pass
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(timeout=2)

    async def _drain(self) -> None:
        ws = self._ws
        if ws is not None:
            try:
                await asyncio.wait_for(ws.close(), 2)
            except Exception:
                pass
        current = asyncio.current_task()
        tasks = [t for t in asyncio.all_tasks() if t is not current]
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

    async def _run(self, url: str, subprotocol: str, gen: int) -> None:
        from websockets.asyncio.client import connect

        outbox: asyncio.Queue = asyncio.Queue()
        reason = "closed"
        try:
            async with connect(url, subprotocols=[subprotocol] if subprotocol else None, open_timeout=8,
                               ping_interval=None, max_size=1 << 20) as ws:
                if gen != self._generation:
                    return
                self._outbox = outbox
                self._ws = ws
                self.events.put(Event("open", gen))
                sender = asyncio.create_task(self._pump(ws, outbox))
                try:
                    async for msg in ws:
                        if gen != self._generation:
                            break
                        self.events.put(Event("binary" if isinstance(msg, bytes) else "text", gen, msg))
                finally:
                    sender.cancel()
                reason = f"closed ({ws.close_code or 'no code'}{': ' + ws.close_reason if ws.close_reason else ''})"
        except Exception as exc:
            reason = f"{type(exc).__name__}: {exc}"
        finally:
            if self._outbox is outbox:
                self._outbox = None
                self._ws = None
            self.events.put(Event("closed", gen, reason))

    async def _pump(self, ws, outbox: asyncio.Queue) -> None:
        while True:
            item = await outbox.get()
            if item is None:
                await ws.close()
                return
            await ws.send(item)
