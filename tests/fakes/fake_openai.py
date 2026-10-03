"""A tiny OpenAI-compatible server for end-to-end tests (no keys, no cost).

Serves just enough of the API for Hermes to run a real gateway turn:

* ``POST /v1/chat/completions`` (streaming and not): a scripted model. Messages
  that mention the screen trigger a ``gadget_display`` tool call, messages
  that mention the LED trigger ``gadget_action``; anything else is echoed.
* ``POST /v1/audio/transcriptions``: returns ``TRANSCRIPT``.
* ``POST /v1/audio/speech``: returns a short tone (raw PCM for
  ``response_format=pcm``, WAV otherwise).

Run standalone:  python tests/fakes/fake_openai.py --port 8999
"""

from __future__ import annotations

import argparse
import io
import json
import math
import struct
import threading
import time
import uuid
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TRANSCRIPT = "what is on my screen"
CRLF = b"\r\n"
MODEL = "fake-gadget-model"


def _tone(rate: int, seconds: float, freq: float = 523.0) -> bytes:
    n = int(rate * seconds)
    return b"".join(struct.pack("<h", int(6000 * math.sin(2 * math.pi * freq * i / rate))) for i in range(n))


def _wav(pcm: bytes, rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return buf.getvalue()


def _last_user_text(messages: list) -> str:
    for m in reversed(messages):
        if m.get("role") == "user":
            content = m.get("content")
            if isinstance(content, list):
                return " ".join(p.get("text", "") for p in content if isinstance(p, dict))
            return str(content or "")
    return ""


def plan_reply(body: dict) -> dict:
    """Decide the assistant message: {"content": str} or {"tool_call": (name, args)}."""
    messages = body.get("messages") or []
    tools = {t.get("function", {}).get("name") for t in body.get("tools") or []}
    if messages and messages[-1].get("role") == "tool":
        result = str(messages[-1].get("content") or "")
        ok = '"success": true' in result.replace('\\"', '"')
        return {"content": "Done - check the gadget." if ok else f"That did not work: {result[:120]}"}
    text = _last_user_text(messages)
    low = text.lower()
    wanted = None
    if "screen" in low and "note" in low:
        wanted = ("gadget_display", {"title": "Note", "text": "Hello from Hermes", "seconds": 30})
    elif "led" in low:
        wanted = ("gadget_action", {"action": "led.set", "args": {"color": "green"}})
    if wanted and wanted[0] in tools:
        return {"tool_call": wanted}
    if wanted and "tool_call" in tools:
        # Plugin tools sit behind Hermes's tool-search bridge; call through it.
        return {"tool_call": ("tool_call", {"calls": [{"name": wanted[0], "arguments": wanted[1]}]})}
    # Hermes may wrap the user's words with context notes; echo the lines that are not notes.
    said = " ".join(line for line in text.strip().splitlines() if line.strip() and not line.startswith("["))
    return {"content": f"Hello from the test model. You said: {said[:200]}"}


class Handler(BaseHTTPRequestHandler):
    server_version = "fake-openai/1"
    requests_log: list = []  # (path, summary) of every POST, for test assertions
    log_file: str | None = None

    def log_message(self, fmt, *args):  # quiet
        pass

    def _json(self, code: int, obj) -> None:
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _body(self) -> bytes:
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            self._json(200, {"object": "list", "data": [{"id": MODEL, "object": "model", "owned_by": "test"}]})
        else:
            self._json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        raw = self._body()
        path = self.path.split("?")[0].rstrip("/")
        summary: dict = {"path": path}
        if path.endswith("/chat/completions"):
            body = json.loads(raw or b"{}")
            summary.update(tools=sorted(t.get("function", {}).get("name", "") for t in body.get("tools") or []),
                           user=_last_user_text(body.get("messages") or [])[-200:])
        self._record(summary)
        if path.endswith("/chat/completions"):
            self._chat(body)
        elif path.endswith("/audio/transcriptions"):
            self._transcription(raw)
        elif path.endswith("/audio/speech"):
            body = json.loads(raw or b"{}")
            seconds = min(2.0, 0.3 + 0.02 * len(str(body.get("input") or "")))
            pcm = _tone(24000, seconds)
            fmt = body.get("response_format") or "mp3"
            data = pcm if fmt == "pcm" else _wav(pcm, 24000)
            self.send_response(200)
            self.send_header("Content-Type", "audio/pcm" if fmt == "pcm" else "audio/wav")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self._json(404, {"error": {"message": f"unsupported path {path}"}})

    def _record(self, summary: dict) -> None:
        Handler.requests_log.append(summary)
        if Handler.log_file:
            with open(Handler.log_file, "a", encoding="utf-8") as f:
                f.write(json.dumps(summary) + "\n")

    def _transcription(self, raw: bytes) -> None:
        # Multipart form; only response_format matters here.
        fmt = "json"
        marker = b'name="response_format"'
        if marker in raw:
            value = raw.split(marker, 1)[1].split(CRLF + CRLF, 1)[1]
            fmt = value.split(CRLF, 1)[0].decode().strip()
        if fmt == "text":
            data = TRANSCRIPT.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            self._json(200, {"text": TRANSCRIPT})

    def _chat(self, body: dict) -> None:
        plan = plan_reply(body)
        cid = "chatcmpl-" + uuid.uuid4().hex[:12]
        created = int(time.time())
        if plan.get("tool_call"):
            name, args = plan["tool_call"]
            call = {"id": "call_" + uuid.uuid4().hex[:8], "type": "function",
                    "function": {"name": name, "arguments": json.dumps(args)}}
            message, finish = {"role": "assistant", "content": None, "tool_calls": [call]}, "tool_calls"
        else:
            message, finish = {"role": "assistant", "content": plan["content"]}, "stop"
        usage = {"prompt_tokens": 10, "completion_tokens": 10, "total_tokens": 20}
        if not body.get("stream"):
            self._json(200, {"id": cid, "object": "chat.completion", "created": created, "model": MODEL,
                             "choices": [{"index": 0, "message": message, "finish_reason": finish}], "usage": usage})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        def emit(delta: dict, finish_reason=None, extra=None):
            chunk = {"id": cid, "object": "chat.completion.chunk", "created": created, "model": MODEL,
                     "choices": [{"index": 0, "delta": delta, "finish_reason": finish_reason}]}
            if extra:
                chunk.update(extra)
            self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
            self.wfile.flush()

        emit({"role": "assistant", "content": ""})
        if message.get("tool_calls"):
            call = message["tool_calls"][0]
            emit({"tool_calls": [{"index": 0, "id": call["id"], "type": "function",
                                  "function": {"name": call["function"]["name"], "arguments": ""}}]})
            emit({"tool_calls": [{"index": 0, "function": {"arguments": call["function"]["arguments"]}}]})
        else:
            words = message["content"].split(" ")
            for i, w in enumerate(words):
                emit({"content": w if i == 0 else " " + w})
                time.sleep(0.02)
        emit({}, finish, {"usage": usage})
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True


def start(port: int = 0) -> tuple[ThreadingHTTPServer, int]:
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, server.server_address[1]


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8999)
    ap.add_argument("--log", help="append a JSON line per request to this file")
    a = ap.parse_args()
    Handler.log_file = a.log
    srv, port = start(a.port)
    print(f"fake OpenAI API on http://127.0.0.1:{port}/v1")
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        srv.shutdown()
