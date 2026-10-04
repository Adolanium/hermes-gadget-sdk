"""ESP-IDF's console between a serial port and App::console, for tests.

What a typed line goes through on a real board:
  1. linenoise (dumb mode) keeps printable ASCII only, and echoes it after the prompt;
  2. esp_console_split_argv splits it at spaces, honouring double quotes and backslash escapes;
  3. port_console.cpp joins the arguments with single spaces and hands the line to App::console.
"""

from __future__ import annotations

PROMPT = "gadget>"


def split_argv(line: str) -> list[str]:
    """esp_console_split_argv (components/console/split_argv.c), character for character."""
    args: list[str] = []
    current: list[str] | None = None
    quoted = escaped = False
    for ch in line:
        if current is None:  # between arguments
            if ch == " ":
                continue
            current = []
            if ch == '"':
                quoted = True
            elif ch == "\\":
                escaped = True
            else:
                current.append(ch)
            continue
        if escaped:  # only \\, \" and "\ " mean anything; any other escaped character is dropped
            if ch in ('\\', '"', " "):
                current.append(ch)
            escaped = False
        elif ch == "\\":
            escaped = True
        elif quoted and ch == '"':
            args.append("".join(current))
            current, quoted = None, False
        elif not quoted and ch == " ":
            args.append("".join(current))
            current = None
        else:
            current.append(ch)
    if current is not None:
        args.append("".join(current))
    return args


def console_line(raw: str) -> str:
    """The line App::console receives when `raw` is typed into the board's console."""
    printable = "".join(ch for ch in raw if " " <= ch <= "~")
    return " ".join(split_argv(printable))


def respond(raw: str, app) -> str:
    """What the board prints for a typed line: the echo after the prompt, then App::console's reply."""
    line = console_line(raw)
    reply = app(line) if line else ""
    return f"{PROMPT}{''.join(ch for ch in raw if ' ' <= ch <= '~')}\n" + (f"{reply}\n" if reply else "")
