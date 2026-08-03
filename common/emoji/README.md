# The colour emoji bank

`emoji_bank.rgba` is the colour emoji the overlay can draw: 1460 single-codepoint
emoji as raw RGBA records at one fixed 32-pixel size, extracted from the CBDT
bitmaps of **Noto Color Emoji** by `scripts/make-emoji-bank.py`. The format --
both ends of it -- lives in `include/vocem/emoji_bank.h`; why the extraction
happens offline instead of shipping a PNG parser into games is written in
DESIGN.md ("What the overlay can draw").

## Licence

The bank is a **Modified Version** of the Noto Color Emoji font software under
the SIL Open Font License 1.1, and is distributed under that same licence:
`OFL-NotoColorEmoji.txt` in this directory (Copyright 2013 Google LLC), which
the package installs beside the bank. Not a mere rendering: the OFL's own
definition of Modified Version includes "changing formats", and its FAQ calls a
format change (2.2) and the removal of parts (2.6) modification -- extracting
the font's embedded glyph bitmaps into a codepoint-indexed store is both.
The OFL applies to the bank alone, not to the rest of this project.

Noto Color Emoji declares **no Reserved Font Names**, so naming the source font
here is unrestricted; "Noto is a trademark of Google Inc." (the font's own name
table, ID 7) -- the name identifies the source, never this project.

## Provenance

The committed bank was generated from:

| | |
| --- | --- |
| Source | Noto Color Emoji, tag `v2.051` of [googlefonts/noto-emoji](https://github.com/googlefonts/noto-emoji) |
| URL | `https://raw.githubusercontent.com/googlefonts/noto-emoji/v2.051/fonts/NotoColorEmoji.ttf` |
| sha256 | `72a635cb3d2f3524c51620cdde406b217204e8a6a06c6a096ff8ed4b5fd6e27b` |
| Version string (name table ID 5) | `Version 2.051;GOOG;noto-emoji:20250818:e92753bfa55fd449e427d4d325f9c8c40408c74e` |

The pinned URL is byte-identical to the system font the bank was first built
from (`cmp` against `/usr/share/fonts/noto/NotoColorEmoji.ttf`, verified
2026-07-30), so the bank is reproducible without vendoring the 10 MB font:
`make-emoji-bank.py` verifies the input's sha256 against the pin and refuses a
font it does not recognise, so a silent rebuild from a different font version
cannot ship pixels this table does not describe.
