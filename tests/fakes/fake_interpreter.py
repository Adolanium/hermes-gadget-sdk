"""Stand-ins for what Hermes does in interpreter mode: speech-to-text and the translating agent.

The development server's brain (``hermes_gadget.devserver.EchoBrain``) runs interpreter mode with
the plugin's own state (``plugin/interpret.py``) and asks two callables for the rest. This fake
supplies them from a script, so a simulated device can hold a two-language exchange without a
model or an STT provider:

* ``transcribe(seconds, language)`` returns the next line of the person who speaks ``language``,
  as a speech-to-text provider told to transcribe in that language would, and records the
  language it was asked for;
* ``translate(text, session)`` looks the line up in a phrasebook, in whichever direction holds it.
"""

from __future__ import annotations

from typing import Optional

# What each person says, in the order they say it.
LINES = {
    "es": ["¿Dónde está la estación?", "Muchas gracias."],
    "en": ["Go straight and turn left.", "You're welcome."],
    "ja": ["駅はどこですか"],
}

PHRASEBOOK = {
    "¿Dónde está la estación?": "Where is the station?",
    "Muchas gracias.": "Thank you very much.",
    "Go straight and turn left.": "Siga recto y gire a la izquierda.",
    "You're welcome.": "De nada.",
    "Where is the station?": "駅はどこですか",
    "buenos días": "good morning",
}


class FakeInterpreter:
    def __init__(self) -> None:
        self.transcribed: list[Optional[str]] = []  # the language each recording was transcribed in
        self.translated: list[tuple[str, str]] = []  # (text, the session's label when it was translated)
        self._next = {language: 0 for language in LINES}

    def transcribe(self, seconds: float, language: Optional[str]) -> str:
        self.transcribed.append(language)
        if language not in LINES:
            return "(unintelligible)"
        line = LINES[language][self._next[language] % len(LINES[language])]
        self._next[language] += 1
        return line

    def translate(self, text: str, session) -> str:
        self.translated.append((text, session.label))
        return PHRASEBOOK.get(text, f"(no translation for {text!r})")

    def install(self, brain) -> "FakeInterpreter":
        brain.transcribe, brain.translate = self.transcribe, self.translate
        return self
