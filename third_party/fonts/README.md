# Fonts

The overlay draws inside other applications' processes, so it cannot ask a toolkit
for a font and must not call fontconfig on the present path. It carries its own --
three of them, because no single freely licensed font covers what a Discord display
name and a Discord channel name are written with.

| File | What it carries | Licence |
| --- | --- | --- |
| `Inter-Regular.ttf`, `Inter-SemiBold.ttf` | Latin, Greek, Cyrillic, and every symbol Inter has: arrows, stars, ticks, bullets, card suits, fractions | `OFL.txt` |
| `NotoEmoji.ttf` | every emoji with a single code point, monochrome | `OFL-NotoEmoji.txt` |
| `NotoSansJP.ttf` | CJK punctuation, the katakana middle dot, fullwidth forms | `OFL-NotoSansJP.txt` |

All three are under the SIL Open Font License 1.1. The subsets are Modified
Versions in the OFL's sense (removing glyphs is modification, per the OFL FAQ),
which is permitted; the one Reserved Font Name among them is `Source`, declared
by Noto Sans JP's Adobe copyright line, and nothing here uses that name. The
package installs these licence texts under `/usr/share/licenses/`.

[Inter](https://github.com/rsms/inter), by Rasmus Andersson, is the body typeface.
Discord's own is *gg sans*, which is not redistributable; Inter is the closest
freely licensed equivalent -- a neutral interface grotesque designed for small
sizes.

**The emoji font is monochrome, and that is not a compromise made lightly.** A
colour emoji font is a bitmap or layered table (CBDT, COLR) that ImGui can only
read through FreeType, and FreeType is a library the injected code has no business
linking into somebody else's game. Monochrome emoji rasterise through the same
stb_truetype path as every other glyph and cost nothing beyond their own pixels.

The `.ttf` files here are subsets. Regenerate them, and the `.inc` files under
`common/fonts/` that are compiled into the overlay, with:

    python3 scripts/make-fonts.py

which fetches each variable font from the `google/fonts` repository, instantiates
the weight it needs and subsets it. The generated files are committed, so building
the project requires neither Python nor fonttools.

The same `.ttf` files are compiled into the configuration window's resources: a
preview whose text is drawn with a different font is a preview whose boxes end in a
different place, and the desktop's own colour emoji font is a different width from
this one.
