"""Turn chat-flavoured agent text into something a tiny screen can show.

Devices render a small bitmap font, so the server strips Markdown, folds
typographic punctuation, folds accents to what the device's ``charset`` draws
(``ascii``; ``latin1`` on firmware whose font has Latin-1; ``cjk`` on firmware built
with the Chinese font, which draws Latin-1 plus CJK), and drops emoji.
Doing it here keeps the firmware simple and lets every board benefit from
improvements without a reflash.
"""

from __future__ import annotations

import re
import unicodedata

_PUNCT = {
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u2033": '"', "\u2032": "'",
    "\u2013": "-", "\u2014": "-", "\u2015": "-", "\u2212": "-", "\u2010": "-", "\u2011": "-",
    "\u2026": "...", "\xa0": " ", "\u2009": " ", "\u202f": " ", "\u200b": "",
    "\u2022": "*", "\xb7": "*", "\u25cf": "*", "\u2192": "->", "\u2190": "<-", "\u21d2": "=>",
    "\xb0": " deg", "\xd7": "x", "\xf7": "/", "\u2264": "<=", "\u2265": ">=", "\u2260": "!=",
    "\xa9": "(c)", "\xae": "(r)", "\u2122": "(tm)", "\u20ac": "EUR", "\xa3": "GBP", "\xa5": "JPY",
    "\xbd": "1/2", "\xbc": "1/4", "\xbe": "3/4", "\xdf": "ss", "\xe6": "ae", "\xc6": "AE",
    "\u0153": "oe", "\u0152": "OE", "\xf8": "o", "\xd8": "O", "\u0142": "l", "\u0141": "L",
}

_FENCE = re.compile(r"```[^\n]*\n(.*?)```", re.DOTALL)
_INLINE_CODE = re.compile(r"`([^`]*)`")
_LINK = re.compile(r"!?\[([^\]]*)\]\(([^)]*)\)")
_BOLD = re.compile(r"(\*\*|__)(.+?)\1")
_ITALIC = re.compile(r"(?<![\w*])([*_])(?!\s)(.+?)(?<!\s)\1(?![\w*])")
_HEADING = re.compile(r"^\s{0,3}#{1,6}\s+", re.MULTILINE)
_QUOTE = re.compile(r"^\s{0,3}>\s?", re.MULTILINE)
_BULLET = re.compile(r"^(\s*)[-*+]\s+", re.MULTILINE)
_RULE = re.compile(r"^\s*([-*_])(\s*\1){2,}\s*$", re.MULTILINE)
_TABLE_SEP = re.compile(r"^\s*\|?\s*:?-{2,}:?\s*(\|\s*:?-{2,}:?\s*)*\|?\s*$", re.MULTILINE)
_THINK = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_BLANKS = re.compile(r"\n{3,}")
_SPACES = re.compile(r"[ \t]{2,}")


def strip_markdown(text: str) -> str:
    text = _THINK.sub("", text)
    text = _FENCE.sub(lambda m: m.group(1).rstrip("\n"), text)
    text = _INLINE_CODE.sub(r"\1", text)
    text = _LINK.sub(lambda m: m.group(1) or m.group(2), text)
    text = _TABLE_SEP.sub("", text)
    text = _RULE.sub("", text)
    text = _HEADING.sub("", text)
    text = _QUOTE.sub("", text)
    text = _BULLET.sub(r"\1* ", text)
    text = _BOLD.sub(r"\2", text)
    text = _ITALIC.sub(r"\2", text)
    text = text.replace("|", " ")
    return text


def fold_ascii(text: str) -> str:
    out = []
    for ch in text:
        if ch == "\n" or " " <= ch <= "~":
            out.append(ch)
            continue
        if ch in _PUNCT:
            out.append(_PUNCT[ch])
            continue
        decomposed = unicodedata.normalize("NFKD", ch)
        ascii_part = "".join(c for c in decomposed if " " <= c <= "~")
        if ascii_part:
            out.append(ascii_part)
        elif ch == "\t":
            out.append(" ")
        # Anything else (emoji, symbols, CJK without a fallback font) is dropped.
    return "".join(out)


def fold_latin1(text: str) -> str:
    """For a device whose font draws Latin-1 too: keep ASCII and U+00A1..U+00FF, fold the rest."""
    out = []
    for ch in text:
        if ch == "\n" or " " <= ch <= "~" or ("\xa1" <= ch <= "\xff" and ch != "\xad"):
            out.append(ch)
            continue
        if ch == "\xa0":  # the device breaks lines only at plain spaces
            out.append(" ")
            continue
        if ch == "\xad":  # soft hyphen: invisible unless a line breaks there, and the device never does
            continue
        if ch in _PUNCT:
            out.append(_PUNCT[ch])
            continue
        decomposed = unicodedata.normalize("NFKD", ch)
        kept = "".join(c for c in decomposed if " " <= c <= "~" or "\xa1" <= c <= "\xff")
        if kept:
            out.append(kept)
        elif ch == "\t":
            out.append(" ")
        # Anything else (emoji, symbols, other scripts) is dropped.
    return "".join(out)


def _is_ru(ch: str) -> bool:
    return "\u0410" <= ch <= "\u044f" or ch in "\u0401\u0451"


_RU_COMPOSITIONS = (
    ("\u0438\u0306", "\u0439"),  # и + combining breve -> й
    ("\u0418\u0306", "\u0419"),  # И + combining breve -> Й
    ("\u0435\u0308", "\u0451"),  # е + combining diaeresis -> ё
    ("\u0415\u0308", "\u0401"),  # Е + combining diaeresis -> Ё
)


def fold_latin1_cyrillic(text: str) -> str:
    """For a device whose font draws Latin-1 and Russian Cyrillic (U+0410..U+044F, Ё/ё)."""
    # й and ё may arrive as и/е plus a combining mark; the mark alone would be folded away by
    # fold_latin1, leaving the wrong letter. Compose just those, so Latin text folds exactly as
    # it does for "latin1".
    for decomposed, composed in _RU_COMPOSITIONS:
        text = text.replace(decomposed, composed)
    out, run = [], []
    for ch in text:
        if _is_ru(ch):
            if run:
                out.append(fold_latin1("".join(run)))
                run = []
            out.append(ch)
        else:
            run.append(ch)
    if run:
        out.append(fold_latin1("".join(run)))
    return "".join(out)


def _is_wide(ch: str) -> bool:
    """CJK, kana, hangul and fullwidth forms: the characters the CJK font draws two columns wide."""
    o = ord(ch)
    return (0x2E80 <= o <= 0xA4CF or 0xAC00 <= o <= 0xD7A3 or 0xF900 <= o <= 0xFAFF
            or 0xFE30 <= o <= 0xFE4F or 0xFF00 <= o <= 0xFF60 or 0xFFE0 <= o <= 0xFFE6
            or 0x20000 <= o <= 0x3FFFD)


def fold_cjk(text: str) -> str:
    """For a device whose font draws Latin-1 and CJK: keep wide characters, fold the rest like Latin-1."""
    out = []
    run = []
    for ch in text:
        if _is_wide(ch):
            if run:
                out.append(fold_latin1("".join(run)))
                run = []
            out.append(ch)
        else:
            run.append(ch)
    if run:
        out.append(fold_latin1("".join(run)))
    return "".join(out)


def _columns(text: str) -> int:
    return sum(2 if _is_wide(ch) else 1 for ch in text)


def _cut(word: str, cols: int) -> int:
    """How many characters of ``word`` fit ``cols`` columns: one at least."""
    used = 0
    for n, ch in enumerate(word):
        used += 2 if _is_wide(ch) else 1
        if used > cols:
            return max(1, n)
    return len(word)


def wrap(text: str, cols: int) -> list[str]:
    """The lines the device breaks one line of text into (the firmware's ``wrap_text``): words
    joined by a space while they fit, a word longer than the line cut at its width."""
    lines: list[str] = []
    current = ""
    for word in text.split():
        while _columns(word) > cols:
            if current:
                lines.append(current)
                current = ""
            n = _cut(word, cols)
            lines.append(word[:n])
            word = word[n:]
        if not word:
            continue
        if current and _columns(current) + 1 + _columns(word) > cols:
            lines.append(current)
            current = word
        else:
            current = f"{current} {word}" if current else word
    if current:
        lines.append(current)
    return lines


def clip(text: str, cols: int, lines: int) -> tuple[str, bool]:
    """``text`` as it fits ``lines`` lines of ``cols`` columns on the device, and whether it was
    cut. A cut text ends in "..." and still fits."""
    if len(wrap(text, cols)) <= lines:
        return text, False
    kept = " ".join(wrap(text, cols)[:lines])
    while kept and len(wrap(kept + "...", cols)) > lines:
        kept = kept[:-1].rstrip()
    return kept + "...", True


def line(text: str, charset: str = "ascii", cols: int | None = None) -> str:
    """One row of a list (a card's item, a choice's answer): shaped like ``for_device``, on one
    line, and no longer than the device's line when it reports ``text_cols``."""
    text = " ".join(for_device(text, charset).split())
    # By columns, as the device counts them: a wide character takes two, and what the device cut
    # further would be cut unseen by the agent.
    return text[:_cut(text, cols)] if isinstance(cols, int) and cols > 4 else text


def for_device(text: str, charset: str = "ascii") -> str:
    text = strip_markdown(text or "")
    if charset == "cjk":
        text = fold_cjk(text)
    elif charset == "latin1-cyrillic":
        text = fold_latin1_cyrillic(text)
    elif charset == "latin1":
        text = fold_latin1(text)
    elif charset == "ascii":
        text = fold_ascii(text)
    text = "\n".join(_SPACES.sub(" ", line).rstrip() for line in text.splitlines())
    return _BLANKS.sub("\n\n", text).strip()
