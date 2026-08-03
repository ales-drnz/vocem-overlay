#!/usr/bin/env python3
# What the overlay can and cannot draw, measured against the shipped fonts.
#
# Compares the ranges fonts.cpp asks the atlas for with the cmaps of the fonts
# it asks them of, plus the colour emoji bank, and reports the classes of text
# that fall through -- so "the symbol coverage is complete" is a measurement
# that can be retaken, not a claim. Run from the repository root.

import re
import struct
import sys
from pathlib import Path

from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent

def parse_ranges(source: str, function: str) -> list[tuple[int, int]]:
    body = re.search(function + r"\(\) \{.*?\n    \};", source, re.S).group(0)
    pairs = re.findall(r"0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+),", body)
    return [(int(a, 16), int(b, 16)) for a, b in pairs]

def covered(cp: int, ranges) -> bool:
    return any(a <= cp <= b for a, b in ranges)

def main() -> int:
    fonts_cpp = (ROOT / "common/src/fonts.cpp").read_text()
    letters = parse_ranges(fonts_cpp, "letter_ranges")
    emoji = parse_ranges(fonts_cpp, "emoji_ranges")
    punct = parse_ranges(fonts_cpp, "punctuation_ranges")
    requested = letters + emoji + punct

    cmaps = set()
    for name in ["Inter-Regular.ttf", "NotoEmoji.ttf", "NotoSansJP.ttf"]:
        cmaps |= set(TTFont(ROOT / "third_party/fonts" / name).getBestCmap())

    bank = set()
    data = (ROOT / "common/emoji/emoji_bank.rgba").read_bytes()
    record = 4 + 32 * 32 * 4
    for i in range(len(data) // record):
        bank.add(struct.unpack_from("<I", data, i * record)[0])

    # 1. Requested but not present in any shipped font: silent question marks.
    holes = []
    for a, b in requested:
        for cp in range(a, b + 1):
            if cp not in cmaps and 0x20 <= cp:
                holes.append(cp)
    print(f"requested-but-missing codepoints (drawn as '?'): {len(holes)}")

    # 2. The bank against the requested emoji ranges: colour emoji that the
    #    monochrome fallback would not even shape.
    bank_outside = sorted(cp for cp in bank if not covered(cp, emoji + letters))
    print(f"bank emoji outside every requested range: {len(bank_outside)}"
          + (f" e.g. {[hex(c) for c in bank_outside[:6]]}" if bank_outside else ""))

    # 3. Whole classes with no coverage at all, named rather than implied.
    classes = {
        "CJK ideographs (U+4E00-9FFF)": (0x4E00, 0x9FFF),
        "Hangul syllables (U+AC00-D7AF)": (0xAC00, 0xD7AF),
        "Arabic (U+0600-06FF)": (0x0600, 0x06FF),
        "Hebrew (U+0590-05FF)": (0x0590, 0x05FF),
        "Thai (U+0E00-0E7F)": (0x0E00, 0x0E7F),
        "Devanagari (U+0900-097F)": (0x0900, 0x097F),
    }
    for label, (a, b) in classes.items():
        have = any(covered(cp, requested) and cp in cmaps for cp in range(a, b + 1))
        print(f"{'covered ' if have else 'ABSENT  '} {label}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
