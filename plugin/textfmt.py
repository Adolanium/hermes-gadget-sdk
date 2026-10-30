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


def for_device(text: str, charset: str = "ascii") -> str:
    text = strip_markdown(text or "")
    if charset == "cjk":
        text = fold_cjk(text)
    elif charset == "latin1":
        text = fold_latin1(text)
    elif charset == "ascii":
        text = fold_ascii(text)
    text = "\n".join(_SPACES.sub(" ", line).rstrip() for line in text.splitlines())
    return _BLANKS.sub("\n\n", text).strip()
