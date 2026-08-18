#!/usr/bin/env python3
# Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
# All rights reserved.
# Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
#
# Regenerates the embedded font data in common/fonts/.
#
# The overlay carries its own font because it draws inside other people's
# processes: there is no toolkit to ask, no fontconfig call that would be safe on
# the present path, and no guarantee a given machine has anything in particular
# installed. Inter is used because it is licensed under the OFL, is designed for
# user interfaces at small sizes, and sits close to what the Discord client itself
# looks like -- gg sans is not redistributable.
#
# Three fonts, not one. Inter carries the letters, in two weights, instantiated
# from the variable font so the weights are exactly the ones we ask for. Noto
# Emoji carries the emoji -- monochrome, because a colour emoji font is a bitmap
# format that ImGui can only read through FreeType, which is a library the injected
# code has no business linking. Noto Sans JP carries the handful of CJK and
# fullwidth punctuation marks a Discord channel name is full of and Inter has none
# of: the "・" in "🔊・stanza" was drawn as a question mark until this existed.
#
# The output of this script is committed, so building the project needs neither
# Python nor fonttools.
#
#     python3 scripts/make-fonts.py
#
# Requires: fonttools, and imgui's binary_to_compressed_c (built here on the fly).

import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "third_party" / "fonts"
OUT_DIR = ROOT / "common" / "fonts"
COMPRESSOR_SOURCE = ROOT / "third_party" / "imgui" / "misc" / "fonts" / "binary_to_compressed_c.cpp"

GOOGLE = "https://raw.githubusercontent.com/google/fonts/main/ofl"
SOURCE = f"{GOOGLE}/inter/Inter%5Bopsz,wght%5D.ttf"
EMOJI_SOURCE = f"{GOOGLE}/notoemoji/NotoEmoji%5Bwght%5D.ttf"
PUNCTUATION_SOURCE = f"{GOOGLE}/notosansjp/NotoSansJP%5Bwght%5D.ttf"
# The three symbol fonts, all OFL like everything above. They exist because a
# Discord server decorates its channel and display names from symbol-picker
# sites (the owner's uses coolsymbol.com, whose full symbol table is the corpus
# these ranges were measured against, 2026-08-10): box drawing, circled
# letters, fancy math alphabets, arrows beyond Inter's. Measured on that
# corpus of 3199 distinct codepoints: the shipped trio drew 1674, and the
# blocks below take the coverage to ~94% -- what stays out is out by decision
# (whole CJK, scripts that need shaping, and the Private Use Area, where no
# font of ours could draw the glyph the site means).
MATH_SOURCE = f"{GOOGLE}/notosansmath/NotoSansMath-Regular.ttf"
SYMBOLS2_SOURCE = f"{GOOGLE}/notosanssymbols2/NotoSansSymbols2-Regular.ttf"
SYMBOLS_SOURCE = f"{GOOGLE}/notosanssymbols/NotoSansSymbols%5Bwght%5D.ttf"

# Latin with its extensions, Greek and Cyrillic: what a display name is realistically
# written in. Then every symbol Inter carries, which is most of the ones a name or a
# channel gets decorated with -- arrows, stars, ticks, bullets, card suits, fractions
# -- and which used to be left out for no reason but the range list being short.
# Whole CJK is deliberately absent: it would multiply the atlas size for a case the
# overlay cannot serve well anyway.
# ...plus IPA and the modifier letters, the phonetic extensions and Greek
# Extended, which Inter carries and fancy display names use (the superscript
# alphabets are modifier letters).
RANGES = ("U+0020-00FF,U+0100-024F,U+0250-02FF,U+0370-03FF,U+0400-04FF,"
          "U+1D00-1DBF,U+1F00-1FFF,U+2000-2BFF")

# Everything Noto Emoji has, which is every emoji with a single code point. Sequences
# joined with U+200D -- 👨‍👩‍👧 and the skin tones -- draw as their parts, which is what
# a renderer without a shaper can do.
EMOJI_RANGES = None  # the font's whole cmap

# The punctuation a Discord channel name is written with and Inter has none of:
# CJK punctuation, the katakana middle dot, and the fullwidth forms.
# The original punctuation, plus what the corpus above actually meets in
# names: full box drawing and block elements (complete in this font where the
# symbol fonts carry fragments), the kana, the compatibility jamo, and the
# enclosed/compatibility CJK blocks (circled ideographs, katakana words).
# Whole CJK ideographs stay out, as the header says.
PUNCTUATION_RANGES = ("U+2500-259F,U+3000-303F,U+3041-30FF,U+3131-318E,"
                      "U+3200-33FF,U+FE30-FE6F,U+FF01-FF60")
# Arabic-Indic digits (which shape alone), the letterlike and symbol blocks,
# and the mathematical alphanumerics -- the "fancy font" alphabets.
MATH_RANGES = "U+0660-0669,U+06F0-06F9,U+2100-2BFF,U+1D400-1D7FF"
SYMBOL_RANGES = "U+2100-2BFF"

WEIGHTS = ((400, "Regular", "InterRegular", "inter_regular.inc"),
           (600, "SemiBold", "InterSemiBold", "inter_semibold.inc"))


def subset_weight(variable_font: Path, weight: int, name: str) -> Path:
    from fontTools import subset, ttLib
    from fontTools.varLib import instancer

    font = ttLib.TTFont(variable_font)
    # opsz 20 is the optical size Inter intends for interface text.
    instancer.instantiateVariableFont(font, {"wght": weight, "opsz": 20},
                                      inplace=True, updateFontNames=True)
    options = subset.Options(layout_features=[], notdef_outline=True,
                             drop_tables=["FFTM"], name_IDs=[1, 2, 3, 4, 5, 6],
                             glyph_names=False, hinting=False, desubroutinize=True)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=subset.parse_unicodes(RANGES))
    subsetter.subset(font)

    destination = FONT_DIR / f"Inter-{name}.ttf"
    font.save(destination)
    return destination


def subset_static(variable_font: Path, ranges, destination: Path, weight: int = 400) -> Path:
    """One weight out of a variable font, cut down to the code points asked for.

    `ranges` of None keeps the font's whole character map, which is what the emoji
    font wants: every emoji it has is one we want.
    """
    from fontTools import subset, ttLib
    from fontTools.varLib import instancer

    font = ttLib.TTFont(variable_font)
    if "fvar" in font:
        instancer.instantiateVariableFont(font, {"wght": weight}, inplace=True,
                                          updateFontNames=True)
    if ranges is None:
        unicodes = sorted({c for table in font["cmap"].tables for c in table.cmap})
    else:
        unicodes = subset.parse_unicodes(ranges)
    options = subset.Options(layout_features=[], notdef_outline=True,
                             drop_tables=["FFTM"], name_IDs=[1, 2, 3, 4, 5, 6],
                             glyph_names=False, hinting=False, desubroutinize=True)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(unicodes=unicodes)
    subsetter.subset(font)
    font.save(destination)
    return destination


def fetch(url: str, destination: Path) -> Path:
    if not destination.exists():
        print(f"fetching {url}")
        subprocess.run(["curl", "-sL", "-o", str(destination), url], check=True)
    return destination


def main() -> int:
    variable_font = fetch(SOURCE, FONT_DIR / "Inter-Variable.ttf")
    emoji_font = fetch(EMOJI_SOURCE, FONT_DIR / "NotoEmoji-Variable.ttf")
    punctuation_font = fetch(PUNCTUATION_SOURCE, FONT_DIR / "NotoSansJP-Variable.ttf")
    math_font = fetch(MATH_SOURCE, FONT_DIR / "NotoSansMath-Regular.ttf")
    symbols2_font = fetch(SYMBOLS2_SOURCE, FONT_DIR / "NotoSansSymbols2-Regular.ttf")
    symbols_font = fetch(SYMBOLS_SOURCE, FONT_DIR / "NotoSansSymbols-Variable.ttf")

    with tempfile.TemporaryDirectory() as work:
        compressor = Path(work) / "binary_to_compressed_c"
        subprocess.run(["g++", "-O2", "-o", str(compressor), str(COMPRESSOR_SOURCE)], check=True)

        def emit(ttf: Path, symbol: str, output: str) -> None:
            # -u8 emits an unsigned char array rather than base85, which compiles
            # faster and is easier to read in a diff.
            result = subprocess.run([str(compressor), "-nostatic", "-u8", str(ttf), symbol],
                                    check=True, capture_output=True, text=True)
            # The tool names its input by the path it was given, which is absolute
            # here: the committed headers used to carry whoever's home directory
            # generated them into every clone. Say where the file is in the tree.
            text = result.stdout.replace(f"{ROOT}/", "")
            (OUT_DIR / output).write_text(text)
            print(f"{output}: {ttf.stat().st_size} bytes of TTF")

        for weight, name, symbol, output in WEIGHTS:
            emit(subset_weight(variable_font, weight, name), symbol, output)

        emit(subset_static(emoji_font, EMOJI_RANGES, FONT_DIR / "NotoEmoji.ttf"),
             "NotoEmoji", "noto_emoji.inc")
        emit(subset_static(punctuation_font, PUNCTUATION_RANGES, FONT_DIR / "NotoSansJP.ttf"),
             "NotoPunctuation", "noto_punctuation.inc")
        emit(subset_static(math_font, MATH_RANGES, FONT_DIR / "NotoSansMath.ttf"),
             "NotoMath", "noto_math.inc")
        emit(subset_static(symbols2_font, SYMBOL_RANGES, FONT_DIR / "NotoSansSymbols2.ttf"),
             "NotoSymbols2", "noto_symbols2.inc")
        emit(subset_static(symbols_font, SYMBOL_RANGES, FONT_DIR / "NotoSansSymbols.ttf"),
             "NotoSymbols", "noto_symbols.inc")

    variable_font.unlink()
    emoji_font.unlink()
    punctuation_font.unlink()
    math_font.unlink()
    symbols2_font.unlink()
    symbols_font.unlink()
    return 0


if __name__ == "__main__":
    sys.exit(main())
