#!/usr/bin/env python3
# Regenerates common/emoji/emoji_bank.rgba from Noto Color Emoji.
#
# The colour emoji in the font are PNG bitmaps (CBDT), and a PNG parser has no
# business inside somebody's game -- the avatar cache fought exactly this fight
# (DESIGN, entry 34's family). So the decoding happens here, offline, and what
# ships is the least interpretable thing there is: fixed-size raw RGBA records,
# sorted by codepoint, binary-searched by the reader in vocem/emoji_bank.h.
# Record: u32 little-endian codepoint + 32*32*4 bytes of straight (unmultiplied)
# RGBA. Only single-codepoint emoji: the ZWJ sequences and flags live in GSUB
# ligatures, which ImGui cannot shape -- the same limit the monochrome fallback
# has always had.
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

# The bank is a Modified Version of Noto Color Emoji under the OFL 1.1
# (common/emoji/OFL-NotoColorEmoji.txt, README.md beside it). The input is
# pinned so the committed bank stays reproducible from a named font version
# rather than from whatever the build machine has today: the pinned URL is
# byte-identical to the system font this was first built from.
PIN_URL = ("https://raw.githubusercontent.com/googlefonts/noto-emoji/"
           "v2.051/fonts/NotoColorEmoji.ttf")
PIN_SHA256 = "72a635cb3d2f3524c51620cdde406b217204e8a6a06c6a096ff8ed4b5fd6e27b"

def main() -> int:
    root = Path(__file__).resolve().parent.parent
    out_path = root / "common" / "emoji" / "emoji_bank.rgba"
    out_path.parent.mkdir(parents=True, exist_ok=True)

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

    records = []
    skipped = 0
    for codepoint in sorted(cmap):
        glyph_name = cmap[codepoint]
        entry = strike.get(glyph_name)
        if entry is None:
            skipped += 1
            continue
        data = entry.data
        # The PNG starts after the small glyph metrics header; find its magic.
        magic = data.find(b"\x89PNG")
        if magic < 0:
            skipped += 1
            continue
        image = Image.open(io.BytesIO(data[magic:])).convert("RGBA")
        image = image.resize((SIZE, SIZE), Image.LANCZOS)
        records.append(struct.pack("<I", codepoint) + image.tobytes())

    with open(out_path, "wb") as out:
        for record in records:
            out.write(record)

    size = out_path.stat().st_size
    print(f"wrote {out_path}: {len(records)} emoji, {size / 1e6:.1f} MB "
          f"({skipped} cmap entries without a strike)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
