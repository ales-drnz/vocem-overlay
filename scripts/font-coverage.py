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
    # All FOUR range functions and all SIX fonts, which is what the atlas is
    # built from (common/src/fonts.cpp's add_weight). This asked for three of
    # each: it never parsed symbol_ranges and never opened the three symbol
    # faces entry 128 added -- so the one tool that answers "what can the
    # overlay not draw" over-reported by three times (6472 missing codepoints
    # against 2180) and was blind to the 3860 codepoints of the very blocks it
    # was last extended for.
    letters = parse_ranges(fonts_cpp, "letter_ranges")
    emoji = parse_ranges(fonts_cpp, "emoji_ranges")
    punct = parse_ranges(fonts_cpp, "punctuation_ranges")
    symbols = parse_ranges(fonts_cpp, "symbol_ranges")
    requested = letters + emoji + punct + symbols

    faces = ["Inter-Regular.ttf", "NotoEmoji.ttf", "NotoSansJP.ttf",
             "NotoSansMath.ttf", "NotoSansSymbols.ttf", "NotoSansSymbols2.ttf"]
    cmaps = set()
    for name in faces:
        path = ROOT / "third_party/fonts" / name
        if not path.exists():
            raise SystemExit(
                f"{name} is not in third_party/fonts: the atlas is built from six faces and "
                "this would measure a subset of them while printing a total")
        cmaps |= set(TTFont(path).getBestCmap())

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
    #    monochrome fallback would not even shape. Keys from U+F0000 up are the
    #    sequence glyphs (vocem/emoji_bank.h), not codepoints: counted apart.
    sequence_keys = sum(1 for cp in bank if cp >= 0xF0000)
    print(f"bank sequence keys (ZWJ sequences, flags, keycaps, tags): {sequence_keys}")
    bank_outside = sorted(cp for cp in bank if cp < 0xF0000 and not covered(cp, emoji + letters))
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
    # "in the atlas" and not "drawable": ImGui does no shaping, so a script
    # whose letters join or reorder is out of scope however many of its
    # codepoints a face happens to carry (DESIGN says so of Arabic and Indic).
    # Since the symbol faces joined the atlas some of those codepoints ARE
    # present, which is why this line says what it measures.
    shaping = {"Arabic (U+0600-06FF)", "Hebrew (U+0590-05FF)", "Thai (U+0E00-0E7F)",
               "Devanagari (U+0900-097F)"}
    for label, (a, b) in classes.items():
        have = any(covered(cp, requested) and cp in cmaps for cp in range(a, b + 1))
        note = "  (in the atlas, but unshaped: out of scope)" if have and label in shaping else ""
        print(f"{'in atlas' if have else 'ABSENT  '} {label}{note}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
