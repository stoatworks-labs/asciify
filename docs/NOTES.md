# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*asciify — ASCII art renderer as an FFGL effect for Resolume; PUBLIC MIT, matches cells on ink coverage AND shape moments, verified by reading the chosen characters back out of the rendered frame*

**asciify** (started 2026-08-02) — a **character renderer**, not a brightness
ramp, as an FFGL effect for Resolume Arena/Avenue. `~/Projects/asciify`,
**PUBLIC MIT**, github.com/stoatworks-labs/asciify, **v0.1.0 released 2026-08-02**
(dmg + setup.exe + both zips, cut by the tag-triggered CI). Site page and guide
**LIVE** at stoatworks-labs.com/software/asciify; video **LIVE** at
youtube.com/watch?v=Hzy60YIhKpg. Instagram Reel **LIVE** at instagram.com/reel/Dbj0uT5AuBG/.
Effect ID `AS01` (siblings: PH01 porthole, OC01 old-cathode, LK01 luma-keyer).

**The design idea that governs everything:** a character cell is a *small
picture*, so the glyph that stands in for it is the one whose ink is distributed
most like it. Coverage **and** five moments of the ink (`u`, `v`, `uv`, `u²−c`,
`v²−c`), measured on the same 8×8 grid for the cell and for every glyph. Tone is
matched absolutely; shape is matched as a **direction** only — a glyph's ink is
binary and a cell's is soft, so their shape vectors are never the same length
even when they agree, and comparing lengths collapses the effect back to a ramp.

Three properties **fall out** rather than being arranged:
- **Structure = 0 is exactly the classic ramp**, with no separate code path.
- **Flat cells ignore Structure** — confidence is the length of the cell's own
  shape vector, so there is no edge detector and no threshold.
- **Every alphabet uses its whole range**, because tone maps into the *measured*
  min/max coverage of the selected set. This is why Blocks reaches solid and Box
  drawing (all characters within a hair of each other) is chosen almost purely on
  structure.

**The ramp is measured, never written down.** Font drawn by hand in
`FontData.cpp` (123 glyphs, 5×7 body in an 8×8 cell, so MIT with nothing
vendored); `MeasureGlyph` measures ink; ordering is a consequence. Real
consequence, not theory: the measured order of the traditional set on this font
is **`.-:+=*%#@`** — `-` outweighs `:` and `%` is lighter than `#`.

**Verification is the interesting part.** `asctest --match` renders at **exactly
one output pixel per glyph pixel** (white on opaque black), so every cell of the
frame *is* an 8×8 glyph bitmap that can be read back and named, then compared
against an independent C++ implementation. **43 runs × 960 cells, zero
disagreements** across structure × every character set. It covers the mip chain,
tone curve, dither, moments, search and atlas addressing at once. Deliberately
desyncing one mirrored constant produced 296 disagreements at 37× tolerance, so
the test is proven, not assumed.

**Two findings that only that test could have produced:**
- The copy buffer is `GL_RGBA16F`, and **half precision is measurable**: 2⁻¹¹
  relative error, invisible in the picture but 100× float noise, enough to flip
  close cells. The harness models it (`toHalfAndBack`, `__fp16`) rather than
  pretending otherwise; forcing 32F gives exact agreement with no tolerance.
- A **single knife-edge cell** turned out to be a genuine font bug: ░ was drawn
  at 0.125 and ▓ at 0.875 instead of true quarters. Fixing the glyphs fixed the
  test.

**Structure had to be reformulated.** As a plain cost weight it was useless above
a quarter of the slider (18% of the picture changed over 0→0.25, 0.7% over the
remaining three quarters). It is now an **allowance** — how far the match may
stray from the correct weight, as a fraction of the alphabet's own tonal range.
Front-loading remains (~17% then ~8%) and that is honest: the smallest non-zero
allowance already changes every near-tie.

**Windows and macOS both compile in CI**, proven by a `workflow_dispatch` run
(macOS universal bundle + x64 `.dll` + NSIS installer, all green, nothing
published because the release job is gated on `refs/tags/v*`). The first
dispatch **failed at configure with "Could NOT find GLEW"** — scaffolding from a
sibling repo copied Info.plist/LICENSE/release-lib but **missed `vcpkg.json`**,
so vcpkg manifest mode never engaged. macOS cannot catch that; only the Windows
job can. Worth checking first thing on any new FFGL repo.

**Not verified:** never loaded into Resolume (so how the parameters *present* is
untested — especially the `FF_TYPE_TEXT` Custom Set field, which the SDK supports
and Resolume's own example uses but which nobody here has seen render); never
built on Linux; nothing timed.

**The OpenFX build now ships for Linux too** (2026-08-25), because Resolve runs
there. Built in an AlmaLinux 8 container for glibc 2.28 -- Resolve's supported
Linux is Rocky 8.6, and a newer glibc floor is refused by the loader on exactly
that distro. Proven by `dlopen` on Rocky 8, not by argument, and that mattered:
four plugins in the fleet failed the load test with `undefined symbol:
pthread_create`, because on glibc 2.28 pthreads are still in libpthread (they
merged into libc in 2.34) and nothing linked it. It compiles, links, exports
`OfxGetPlugin` and passes a glibc-version assertion before failing at load --
the version check reads what a binary *asks for*, never what provides it. FFGL
is still macOS/Windows only; Resolume has no Linux build. Installed to
`~/Documents/Resolume Arena/Extra Effects/`.

**The release video is rendered, not filmed** (no window to film), like porthole's
and old-cathode's — but with **one `--pipe` per beat** instead of one for the whole
sequence, because the character set is per beat and Custom Set is a text parameter
that only `--custom` reaches and that is read once at startup.

**Resolume's demo folder is mostly a bright object on black**, which typesets as an
island of characters in a field of spaces — correct, and a useless shot. Four beats
played that way first time. Cropping in fixes it and **costs nothing**, because a
cell is an average of the pixels under it. Trinity's circles and Enter5's corridor
are the obvious picks for a Structure beat and are exactly wrong (thin lines, empty
cells); **OrganicMotions** has a contour in every cell. Only NoHopeJustFear (skulls)
and Ethnik2_23 (fire) fill a frame unaided.

**Thumbnails: judge at ~210px, not full size.** The opening skulls beat is the
obvious pick and is mush at every column count from 24 to 160 — busy source makes
texture, not a subject. The fire in blocks is the only frame with the contrast to
survive.

Built on the CMake MODULE + FFGL-submodule pattern from
[resolume luma keyer](https://github.com/stoatworks-labs/resolume-luma-keyer/blob/main/docs/NOTES.md) (`resolume-luma-keyer`), [porthole](https://github.com/stoatworks-labs/porthole/blob/main/docs/NOTES.md) (`porthole`) and
[old cathode](https://github.com/stoatworks-labs/old-cathode/blob/main/docs/NOTES.md) (`old-cathode`); `PassBuffer` is old-cathode's with a sampling mode
added. SDK traps in [ffgl sdk bugs](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffgl_sdk_bugs.md). Traps in the repo's AGENTS.md,
see [agents md convention](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_agents_md_convention.md). **disclaimer scope** (working-practice note, kept in Claude memory) applies.
