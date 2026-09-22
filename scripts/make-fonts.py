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

import hashlib
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


# What each source was when the committed .inc files were generated. The
# generator beside this one, make-emoji-bank.py, refuses an input whose sha256
# it does not recognise unless it is told --repin, and this one pinned nothing
# at all: it fetches six fonts from a MOVING branch of google/fonts, so a
# regeneration could silently change what every shipped library draws with --
# in a project where a licence obligation (entry 50) and a coverage claim
# (entry 128, "3100 of 3199") are both properties of these exact files.
#
# Every entry is empty TODAY, and deliberately so: the originals are deleted
# after each run, so the only honest digest is one taken at a regeneration, and
# writing down whatever upstream serves now would pin the wrong file while
# looking like a pin. The script prints the sha256 of each font it fetches; the
# next person to regenerate copies those six lines in here, and from then on a
# moved upstream stops the run instead of quietly changing what ships.
# `--repin` accepts a digest that differs from the table.
PINS = {
    "Inter-Variable.ttf": "",
    "NotoEmoji-Variable.ttf": "",
    "NotoSansJP-Variable.ttf": "",
    "NotoSansMath-Regular.ttf": "",
    "NotoSansSymbols2-Regular.ttf": "",
    "NotoSansSymbols-Variable.ttf": "",
}


def fetch(url: str, destination: Path, repin: bool = False) -> Path:
    if not destination.exists():
        print(f"fetching {url}")
        # --fail, because curl without it exits 0 on an HTTP 404 and writes the
        # error page to the destination -- which this function would then reuse
        # for ever, since it only fetches what is not already there.
        subprocess.run(["curl", "-sL", "--fail", "-o", str(destination), url], check=True)
    digest = hashlib.sha256(destination.read_bytes()).hexdigest()
    expected = PINS.get(destination.name, "")
    if not expected:
        print(f"  {destination.name}: sha256 {digest} (not pinned)")
    elif digest != expected:
        if not repin:
            raise SystemExit(
                f"{destination.name} is not the file this project was built from:\n"
                f"  expected {expected}\n"
                f"  fetched  {digest}\n"
                "Upstream moved. Look at what changed, then re-run with --repin to accept it "
                "-- the licence texts and entry 128's coverage figures are properties of these "
                "exact files."
            )
        print(f"  {destination.name}: repinned {expected} -> {digest}")
    return destination


def main() -> int:
    repin = "--repin" in sys.argv
    # One name per font, and the cleanup at the bottom of the file walks the
    # same PINS table rather than repeating them: two of the six were not
    # matched by .gitignore's "-Variable.ttf" pattern, and the unlinks used to
    # be the last statements of main() with no try/finally, so an interrupted
    # run left whatever it had fetched sitting untracked in third_party/fonts.
    def take(url: str, name: str) -> Path:
        # PINS is the list, not a table beside one. The names are written twice
        # -- here and in PINS -- and a seventh font added without an entry
        # would be fetched, never pinned, and never deleted by the `finally`
        # below, which walks PINS. Two of the six are not matched by
        # .gitignore's "-Variable.ttf" pattern, so that leftover is one
        # `git add -A` from a 10 MB binary in a public repository (entry 175).
        if name not in PINS:
            raise SystemExit(
                f"{name} has no entry in PINS, so it would be fetched without a pin and "
                "left behind after the run. Add it to the table, empty if there is no "
                "digest to write yet."
            )
        return fetch(url, FONT_DIR / name, repin)

    variable_font = take(SOURCE, "Inter-Variable.ttf")
    emoji_font = take(EMOJI_SOURCE, "NotoEmoji-Variable.ttf")
    punctuation_font = take(PUNCTUATION_SOURCE, "NotoSansJP-Variable.ttf")
    math_font = take(MATH_SOURCE, "NotoSansMath-Regular.ttf")
    symbols2_font = take(SYMBOLS2_SOURCE, "NotoSansSymbols2-Regular.ttf")
    symbols_font = take(SYMBOLS_SOURCE, "NotoSansSymbols-Variable.ttf")

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

    return 0


if __name__ == "__main__":
    # The fetched originals go whatever happens -- an exception, a Ctrl-C, a
    # refused pin. They are large, two of the six are ignored only by name
    # rather than by .gitignore's pattern, and what is committed is the
    # subsets under common/fonts/.
    try:
        status = main()
    finally:
        for name in PINS:
            leftover = FONT_DIR / name
            if leftover.exists():
                leftover.unlink()
    sys.exit(status)
