#!/usr/bin/env python3
"""Turn source/FontData.cpp into demo/font.js.

The font is source code — 8x8 ASCII art, one entry per glyph, top row first —
and it is the only description of the typeface anywhere. The demo needs the same
bitmaps, so they are **extracted** rather than redrawn: a hand-copied font would
drift from the plugin's the first time a glyph was fixed, and because the
alphabet's ordering is *measured* off the bitmaps rather than written down, a
drifted glyph would silently reorder the ramp.

Run after any change to FontData.cpp:

    python3 demo/tools/extract_font.py

`--check` exits non-zero if demo/font.js is out of date, which is what a release
step should call.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
SOURCE = REPO / "source" / "FontData.cpp"
TARGET = REPO / "demo" / "font.js"

GLYPH_SIZE = 8

# { 0x0041, { "........",   //A
#             "........",
#             ... } },
ENTRY = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*\{\s*((?:\"[^\"]*\"\s*,?\s*){%d})\}\s*\}" % GLYPH_SIZE
)
ROW = re.compile(r'"([^"]*)"')

# Each glyph is labelled with the character it draws, so the comment on the
# double-quote glyph is literally `//"`. Left in place, that unbalanced quote
# pairs with the next line's opening quote and every glyph after it comes out
# scrambled — which is invisible in the numbers and obvious only in the atlas.
# The art contains nothing but '.', '#' and spaces, so there is no '//' inside a
# string literal for this to damage.
LINE_COMMENT = re.compile(r"//[^\n]*")


def parse() -> list[tuple[int, list[int]]]:
    text = SOURCE.read_text(encoding="utf-8")

    # Only the kArt array, so a stray brace-and-string elsewhere in the file
    # cannot be mistaken for a glyph.
    start = text.index("const GlyphArt kArt[]")
    end = text.index("} // namespace", start)
    body = LINE_COMMENT.sub("", text[start:end])

    glyphs: list[tuple[int, list[int]]] = []
    for match in ENTRY.finditer(body):
        codepoint = int(match.group(1), 16)
        rows = ROW.findall(match.group(2))
        if len(rows) != GLYPH_SIZE:
            raise SystemExit(f"glyph {codepoint:#06x} has {len(rows)} rows, expected {GLYPH_SIZE}")

        # ParseGlyph in Font.cpp flips here and nowhere else: row 0 of the art is
        # the TOP of the character, row 0 of a Glyph is the BOTTOM. Everything
        # past this point is bottom-up, to match GL's texture origin.
        bits = [0] * GLYPH_SIZE
        for art_row, line in enumerate(rows):
            row = GLYPH_SIZE - 1 - art_row
            value = 0
            for col, ch in enumerate(line[:GLYPH_SIZE]):
                # A short line is padded with paper rather than rejected: the art
                # is source code, and a missing trailing dot should not be fatal.
                if ch not in (".", " "):
                    value |= 1 << col
            bits[row] = value
        glyphs.append((codepoint, bits))

    return glyphs


def render(glyphs: list[tuple[int, list[int]]]) -> str:
    lines = [
        "/**",
        " * The drawn typeface, extracted from source/FontData.cpp.",
        " *",
        " * GENERATED — do not edit. Run `python3 demo/tools/extract_font.py` after",
        " * changing the font, and `--check` to prove this copy is current.",
        " *",
        " * One entry per glyph: [codepoint, ...eight rows of bits]. **Row 0 is the",
        " * bottom**, and bit c of a row is column c counting from the left — the same",
        " * convention Font.cpp's ParseGlyph leaves behind, so nothing downstream needs",
        " * to know the art was written top row first.",
        " */",
        "",
        f"export const GLYPH_SIZE = {GLYPH_SIZE};",
        "",
        "/// Each glyph sits in a 10x10 atlas slot with a one-texel blank border. The",
        "/// border is load-bearing: with GL_LINEAR the type pass would otherwise fetch",
        "/// a neighbouring character's ink along every glyph edge.",
        "export const SLOT_SIZE = 10;",
        "export const ATLAS_COLS = 16;",
        "export const ATLAS_ROWS = 8;",
        "",
        "/// In declaration order. A glyph's index here is its atlas slot.",
        "export const GLYPHS = [",
    ]
    for codepoint, bits in glyphs:
        packed = ", ".join(str(b) for b in bits)
        lines.append(f"  [0x{codepoint:04X}, {packed}],")
    lines.append("];")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="exit non-zero if font.js is stale")
    args = parser.parse_args()

    glyphs = parse()
    if not glyphs:
        raise SystemExit("no glyphs parsed — has FontData.cpp's layout changed?")

    maximum = 16 * 8
    if len(glyphs) > maximum:
        raise SystemExit(f"{len(glyphs)} glyphs will not fit {maximum} atlas slots")

    text = render(glyphs)

    if args.check:
        if not TARGET.exists() or TARGET.read_text(encoding="utf-8") != text:
            print(f"demo/font.js is out of date — run {pathlib.Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"demo/font.js matches FontData.cpp ({len(glyphs)} glyphs)")
        return 0

    TARGET.write_text(text, encoding="utf-8")
    print(f"wrote {TARGET.relative_to(REPO)} — {len(glyphs)} glyphs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
