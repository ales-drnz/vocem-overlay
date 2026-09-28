#!/usr/bin/env python3
# Regenerates common/emoji/emoji_bank.rgba and common/emoji/emoji_sequences.bin
# from Noto Color Emoji.
#
# The colour emoji in the font are PNG bitmaps (CBDT), and a PNG parser has no
# business inside somebody's game. So the decoding happens here, offline, and
# what ships is the least interpretable thing there is: fixed-size raw RGBA
# records, sorted by key, binary-searched by the reader in vocem/emoji_bank.h.
# Record: u32 little-endian key + 32*32*4 bytes of straight (unmultiplied) RGBA.
#
# Two kinds of key. A codepoint, for every emoji the font's cmap maps to a glyph
# with a bitmap. And a private-use codepoint from U+F0000 up, for every glyph
# the font reaches only through a GSUB ligature -- the ZWJ sequences (🍋‍🟩 is
# 🍋 + ZWJ + 🟩, the families, the professions, the skin tones), the flags
# (pairs of regional indicators), the keycaps, the tag sequences -- because
# ImGui shapes nothing and would draw the parts (entry 142). The sequences
# file says which sequence of codepoints each private-use key stands for, in fixed-size
# records too: u32 length, u32 key, 12 x u32 codepoints (zero-padded), sorted
# by the codepoint sequence. The overlay reads both files and rewrites a known
# sequence into its key before the text reaches the atlas.
#
# The keys are assigned in this script and mean nothing outside the pair of
# files written together: that is why the sequence table is a FILE beside the
# bank and not a table compiled into the libraries. The layer inside the Flatpak
# extension and the bank the host package installs are updated separately, and
# a compiled-in table meeting a bank of another version would draw the wrong
# picture, silently -- which is worse than the parts.
#
# A ligature whose glyph is also a cmap glyph would get that codepoint as its
# key and no record of its own; this font has none (the report says how many).
# U+FE0F, the emoji presentation selector, is not in the font's cmap at all: a
# shaper drops it before the ligatures apply, so no sequence here carries it and
# the reader skips it while matching (❤️‍🔥 is 2764 FE0F 200D 1F525 in a name and
# 2764 200D 1F525 in the table).
#
# Checked in like the shader header: whoever regenerates it reviews and commits
# it, and the build needs no fonttools or Pillow.

import hashlib
import io
import struct
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image

FONT = "/usr/share/fonts/noto/NotoColorEmoji.ttf"
SIZE = 32
# The format's fixed record shape for sequences (vocem/emoji_bank.h agrees):
# the longest sequence in the font is measured below and must fit.
SEQUENCE_MAX_LENGTH = 12
# Where the private-use keys start: plane 15, which no text from Discord uses.
PRIVATE_USE_FIRST = 0xF0000
PRIVATE_USE_LAST = 0x10FFFD

# The bank is a Modified Version of Noto Color Emoji under the OFL 1.1
# (common/emoji/OFL-NotoColorEmoji.txt, README.md beside it). The input is
# pinned so the committed bank stays reproducible from a named font version
# rather than from whatever the build machine has today.
PIN_URL = ("https://raw.githubusercontent.com/googlefonts/noto-emoji/"
           "v2.051/fonts/NotoColorEmoji.ttf")
PIN_SHA256 = "72a635cb3d2f3524c51620cdde406b217204e8a6a06c6a096ff8ed4b5fd6e27b"


def private_use(codepoint):
    return 0xE000 <= codepoint <= 0xF8FF or codepoint >= 0xF0000


def ligatures(font):
    """Every ligature in the font's GSUB, as {ligature glyph: [component glyph tuples]}."""
    table = {}
    gsub = font["GSUB"].table
    for lookup in gsub.LookupList.Lookup:
        for subtable in lookup.SubTable:
            if lookup.LookupType == 7:  # extension: the real subtable is inside
                subtable = subtable.ExtSubTable
            if getattr(subtable, "LookupType", lookup.LookupType) != 4:
                continue
            for first, entries in subtable.ligatures.items():
                for entry in entries:
                    table.setdefault(entry.LigGlyph, []).append((first,) + tuple(entry.Component))
    return table


def resolve(glyph_sequence, reverse_cmap, ligature_table, depth=0):
    """A tuple of glyph names into every codepoint sequence it can stand for.

    A component that is itself a ligature glyph (a font may build a long
    sequence in stages) expands into each of its own sequences."""
    if depth > 6:
        return []
    options = [[]]
    for glyph in glyph_sequence:
        if glyph in reverse_cmap:
            options = [o + [reverse_cmap[glyph]] for o in options]
        elif glyph in ligature_table:
            expanded = []
            for parts in ligature_table[glyph]:
                for inner in resolve(parts, reverse_cmap, ligature_table, depth + 1):
                    expanded.extend(o + list(inner) for o in options)
            options = expanded
        else:
            return []
    return [tuple(o) for o in options]


def glyph_rgba(strike, glyph_name):
    entry = strike.get(glyph_name)
    if entry is None:
        return None
    data = entry.data
    # The PNG starts after the small glyph metrics header; find its magic.
    magic = data.find(b"\x89PNG")
    if magic < 0:
        return None
    image = Image.open(io.BytesIO(data[magic:])).convert("RGBA")
    return image.resize((SIZE, SIZE), Image.LANCZOS).tobytes()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    out_dir = root / "common" / "emoji"
    out_dir.mkdir(parents=True, exist_ok=True)
    bank_path = out_dir / "emoji_bank.rgba"
    sequences_path = out_dir / "emoji_sequences.bin"

    digest = hashlib.sha256(Path(FONT).read_bytes()).hexdigest()
    if digest != PIN_SHA256:
        if "--repin" not in sys.argv[1:]:
            print(f"{FONT}: sha256 {digest}\n"
                  f"does not match the pinned {PIN_SHA256} ({PIN_URL}).\n"
                  "A bank built from an unreviewed font version must not ship "
                  "silently: pass --repin to accept this font, then update "
                  "PIN_SHA256 above and the provenance table in "
                  "common/emoji/README.md in the same commit.",
                  file=sys.stderr)
            return 1
        print(f"repinning to {FONT} (sha256 {digest})", file=sys.stderr)

    font = TTFont(FONT)
    cmap = font.getBestCmap()
    strike = font["CBDT"].strikeData[0]

    # The codepoint half: one record per cmap entry with a bitmap. The font's
    # cmap also names glyphs at private-use codepoints of its own (the flags at
    # U+FE4E5 up, the keycaps at U+FE82C up, for an Android that predates the
    # sequences); no text carries those, so they are not codepoints here, and
    # their glyphs are reached through the sequences like every other ligature.
    # Which keeps the invariant the reader relies on: every key from U+F0000 up
    # is a sequence key from the table, and no key below it is.
    records = {}
    skipped = 0
    private_in_cmap = 0
    for codepoint in sorted(cmap):
        if private_use(codepoint):
            private_in_cmap += 1
            continue
        rgba = glyph_rgba(strike, cmap[codepoint])
        if rgba is None:
            skipped += 1
            continue
        records[codepoint] = rgba

    # The sequence half. Several glyph names can map to one codepoint (the
    # blanks the joiner and the selectors share); for reading a ligature back
    # the smallest codepoint of a glyph is the one a text would carry.
    reverse_cmap = {}
    for codepoint, glyph in cmap.items():
        if private_use(codepoint):
            continue
        if glyph not in reverse_cmap or codepoint < reverse_cmap[glyph]:
            reverse_cmap[glyph] = codepoint
    ligature_table = ligatures(font)
    sequences = {}  # codepoint tuple -> ligature glyph
    unresolved = 0
    too_long = 0
    for glyph, parts_list in ligature_table.items():
        for parts in parts_list:
            options = resolve(parts, reverse_cmap, ligature_table)
            if not options:
                unresolved += 1
            for sequence in options:
                if len(sequence) < 2:
                    continue
                if len(sequence) > SEQUENCE_MAX_LENGTH:
                    too_long += 1
                    continue
                sequences[sequence] = glyph

    # Keys: a ligature that draws a cmap glyph keeps that codepoint; every other
    # ligature glyph with a bitmap gets a private-use key, in the order of its
    # first sequence, so the numbering is a function of the font alone.
    key_of_glyph = {}
    next_key = PRIVATE_USE_FIRST
    without_bitmap = 0
    drawn_as_codepoint = 0
    for sequence in sorted(sequences):
        glyph = sequences[sequence]
        if glyph in key_of_glyph:
            continue
        if glyph in reverse_cmap and reverse_cmap[glyph] in records:
            key_of_glyph[glyph] = reverse_cmap[glyph]
            drawn_as_codepoint += 1
            continue
        rgba = glyph_rgba(strike, glyph)
        if rgba is None:
            without_bitmap += 1
            key_of_glyph[glyph] = None
            continue
        if next_key > PRIVATE_USE_LAST:
            print("out of private-use keys", file=sys.stderr)
            return 1
        key_of_glyph[glyph] = next_key
        records[next_key] = rgba
        next_key += 1

    sequence_records = []
    longest = 0
    for sequence in sorted(sequences):
        key = key_of_glyph[sequences[sequence]]
        if key is None:
            continue
        longest = max(longest, len(sequence))
        padded = list(sequence) + [0] * (SEQUENCE_MAX_LENGTH - len(sequence))
        sequence_records.append(struct.pack("<II", len(sequence), key) +
                                struct.pack(f"<{SEQUENCE_MAX_LENGTH}I", *padded))

    with open(bank_path, "wb") as out:
        for key in sorted(records):
            out.write(struct.pack("<I", key) + records[key])
    with open(sequences_path, "wb") as out:
        for record in sequence_records:
            out.write(record)

    private = next_key - PRIVATE_USE_FIRST
    print(f"wrote {bank_path}: {len(records)} glyphs "
          f"({len(records) - private} codepoints, {private} sequence glyphs), "
          f"{bank_path.stat().st_size / 1e6:.1f} MB "
          f"({skipped} cmap entries without a strike, {private_in_cmap} at private-use "
          f"codepoints left to the sequences)")
    print(f"wrote {sequences_path}: {len(sequence_records)} sequences, longest {longest} "
          f"codepoints, {sequences_path.stat().st_size / 1e3:.0f} kB "
          f"({unresolved} ligatures with an unknown component, {too_long} longer than "
          f"{SEQUENCE_MAX_LENGTH}, {without_bitmap} ligature glyphs without a bitmap, "
          f"{drawn_as_codepoint} ligature glyphs that are a codepoint's own)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
