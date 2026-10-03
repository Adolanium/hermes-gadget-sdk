# Mascot art

The device and this project's logo use the Hermes Agent mascot: the girl with the headphones.

`nous-girl-white-1024.png` is Hermes Agent's dark-background artwork (`assets/nous-girl-white.svg` in the Hermes Agent repository), rendered to 1024×1024 RGBA. The art is slightly taller than wide, so it sits inside the square with transparent padding.

The artwork comes from the Hermes Agent repository (MIT License, Copyright (c) 2025 Nous Research). The mascot is Nous Research's character and part of their brand; this project is unofficial and not affiliated with Nous Research (see [NOTICE](../../NOTICE)). Check with Nous Research before using the mascot outside Hermes-related projects.

## What is generated from it

| Output | Tool |
|---|---|
| `firmware/core/src/mascot_data.cpp`: 1-bit idle, blink and talk frames at 64, 96, 144 and 192 px (about 26 KB of flash) | `python tools/gen_mascot.py` |
| `docs/images/logo.png` | `python tools/make_logo.py` |

Both need Pillow (`pip install "hermes-gadget[images]"`).

The blink and talk frames are drawn by `gen_mascot.py` on top of the master. Their eye, mouth and ear-cup coordinates are in 1024-pixel master space, at the top of the script. Adjust them there if the master changes.
