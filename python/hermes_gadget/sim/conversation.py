"""The simulator's in-memory conversation, fed by device protocol messages."""

from dataclasses import dataclass
from typing import Literal


@dataclass
class Message:
    role: Literal["You", "Hermes"]
    text: str


class Conversation:
    def __init__(self):
        self.messages: list[Message] = []
        self.revision = 0
        self._turn = ""
        self._reply: Message | None = None
        self._voice: Message | None = None

    def _add(self, role: Literal["You", "Hermes"], text: str) -> Message:
        message = Message(role, text)
        self.messages.append(message)
        del self.messages[:-100]
        self.revision += 1
        return message

    def receive(self, direction: str, message: dict) -> None:
        kind = message.get("type")
        if direction == "sent":
            if kind == "text":
                self._add("You", str(message.get("text", "")))
                self._voice = None
            elif kind == "audio.end":
                self._voice = self._add("You", "Voice message")
            elif kind == "session.new":
                self.messages.clear()
                self._reply = self._voice = None
                self._turn = ""
                self.revision += 1
            return
        if kind == "turn.start":
            self._turn = str(message.get("turn", ""))
            self._reply = None
        elif kind == "transcript":
            text = str(message.get("text", ""))
            if self._voice is None:
                self._voice = self._add("You", text)
            else:
                self._voice.text = text
                self.revision += 1
        elif kind in ("reply.delta", "reply") and not message.get("interim"):
            turn = str(message.get("turn") or self._turn)
            if self._turn and turn != self._turn:
                return
            text = str(message.get("text", ""))
            if self._reply is None:
                self._reply = self._add("Hermes", text)
            else:
                self._reply.text = text  # Deltas contain the whole reply so far.
                self.revision += 1

    def latest_reply(self) -> str:
        return next((m.text for m in reversed(self.messages) if m.role == "Hermes"), "")
