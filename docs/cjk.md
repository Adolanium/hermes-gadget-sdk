# Chinese (CJK) text on the screen

The device core draws UTF-8. ASCII and Latin-1 keep the 5x7 font. Chinese characters,
Japanese kana, Korean hangul and fullwidth punctuation use a built-in 12x12 bitmap font of
about 7,000 glyphs: the common Traditional characters (Big5), the Simplified ones in GB2312
level 1, the CJK symbols and the fullwidth forms. A wide character takes two 6 px text
columns, wrapping may break between any two of them, and truncation never splits a
character. A character the font lacks is drawn as `?`.

## Build option

12x12 glyphs need taller lines than the 9 px ASCII pitch (13 px at scale 1, with ASCII
sitting 3 rows lower so both share a baseline). Taller lines change every layout, so the
font is a build option and **off by default**; existing boards look as they did.

```bash
hermes-gadget build-sim --cjk                       # simulator
cmake -S firmware -B build -DHG_CJK_FONT=ON         # host build and tests
idf.py -DHG_CJK_FONT=ON build                       # ESP32 firmware (PlatformIO: add it to board_build.cmake_extra_args)
```

With the option off the font is not compiled in at all: no flash is spent, and Chinese
characters are one column wide and drawn as `?`, as before. With it on, the glyph data adds
about 160 KB to the image. Fourteen layout tests in `test_app.cpp` assume the 9 px pitch and
do not pass with the option on. The Chinese-text tests (`hg_cjk_tests`, `firmware/tests/test_cjk.cpp`)
build against a core with the font in and run on every host build, the option on or off.

## Telling the plugin

The plugin shapes reply text to what the device can draw (`display.charset` in the
`hello`, see [protocol](protocol.md#hello-device--server)). A firmware built with `HG_CJK_FONT`
reports `"cjk"`, and the plugin then keeps wide characters and folds everything else as for
`"latin1"`. Older firmware reports `"ascii"` or `"latin1"` and the plugin drops CJK for it, as
before.

## Regenerating the font

```bash
python3 firmware/tools/gen_cjk_font.py <dir with fusion-pixel-12px-monospaced-*.ttf>
```

The script needs Pillow and the `zh_tw` and `zh_hans` files from the
[Fusion Pixel Font 2026.10.25 release](https://github.com/TakWolf/fusion-pixel-font/releases/tag/2026.10.25).
The font is under the SIL Open Font License 1.1; its notices are in
[THIRD_PARTY_NOTICES](../THIRD_PARTY_NOTICES.md) and [NOTICE](../NOTICE).
