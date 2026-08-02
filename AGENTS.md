# asciify — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that re-draws the
incoming clip as characters. C++17 + GLSL 4.1, CMake, universal macOS `.bundle`
and a Windows `.dll`. Public, MIT, `github.com/stoatworks-labs/asciify`.

`CLAUDE.md` is the command reference — build, install, verify. This file is the
*why*: read it before touching the matching maths, the font, or the pass
structure.

---

## The one idea

**A character cell is a small picture, and the character that stands in for it
is the one whose ink is distributed most like that picture.**

Not "a brightness looked up in a ramp". The distinction is the whole plugin, and
almost every design decision below falls out of it rather than having been
arranged.

Two things are measured, on the same 8×8 grid, for the cell and for every glyph:

- **how much ink** — the mean, which is coverage
- **where the ink is** — five moments: `u`, `v`, `uv`, `u²−c`, `v²−c`

Those five are enough to tell `|` from `-` (the last two against each other),
`/` from `\` (the sign of `uv`), and `.` from `'` (the sign of `v`). The constant
`c` is the mean of `u²` over the grid, which is what makes the last two blind to
coverage — so the shape terms carry no tone and the tone term carries no shape.

**Tone is matched in absolute terms; shape is matched as a direction.** Only the
direction, because a glyph's ink is hard-edged and binary while a cell's is
soft; their shape vectors are never the same *length* even when they plainly
agree, and comparing lengths would reject every directional glyph and quietly
collapse the effect into a tone ramp.

### What falls out of that

- **Structure = 0 is exactly the classic ASCII ramp.** There is no second code
  path. Weight the direction term to nothing and the cost is coverage error
  alone, which is a nearest-neighbour lookup in a ramp.
- **Flat cells ignore Structure.** The confidence factor is the length of the
  cell's own shape vector. A cell with nothing in it has no direction to be
  wrong about, and the term fades out on its own. Nothing detects "is there an
  edge here"; there is no threshold to tune.
- **Every alphabet uses its whole range.** Tone is mapped into the measured
  `[min, max]` coverage of whatever set is selected, so the Blocks set reaches
  solid black and white and the ASCII set (whose heaviest character is `#` at
  0.31) still spans its own range instead of rendering everything dark.
- **The box-drawing set works at all.** Its characters all weigh within a hair
  of each other, so it has essentially no tone to match on and is chosen almost
  entirely on structure. Turn Structure down with it selected and the picture
  collapses — which is the clearest demonstration in the plugin of what that
  control does.

### The ramp is measured, not written down

The traditional `" .:-=+*#%@"` ordering is folklore: one person's eye applied to
one person's font. Here `FontData.cpp` draws the glyphs, `MeasureGlyph` measures
their ink, and the ordering is a consequence. Redraw a glyph and it moves.

It is not a theoretical point. On this font the measured order of the Classic
set is `.-:+=*%#@` — `-` outweighs `:`, and `%` is lighter than `#`, both
against the traditional string. `asctest --ramp` prints it.

---

## The traps

Ordered by how much time they will cost you.

**The matching maths exists twice.** In C++ in `Match.cpp`, because it has to be
readable and testable, and in GLSL in the cell shader in `Shaders.cpp`, because
it runs per cell on the GPU. Every mirrored constant and step carries a
`//= mirrored` comment. Change one, change both, and run `asctest --match` —
which is the only thing that will notice. Deliberately desyncing one constant
during development produced 296 disagreements out of 960 cells at 37× the
tolerance, so the test is not decorative.

**A GLSL uniform name that does not match the C++ is silently ignored.**
`glGetUniformLocation` returns −1 and `glUniform` on −1 is a documented no-op,
so a control can be stone dead while everything compiles, links, loads and
renders. `--match` will not catch it: it checks the GPU and the C++ agree about
the matching, and a dead uniform makes them agree perfectly on the wrong thing.
`tools/sweep.py` is what catches it.

**The cell buffer must be sampled with `GL_NEAREST`.** Its alpha channel is a
glyph index, not a quantity. Interpolate it and a pixel on a cell boundary reads
the average of two indices, which addresses a third character that neither cell
chose. `PassBuffer::Sampling` exists for this.

**The copy buffer is `GL_RGBA16F`, and that is visible in measurements.** Half
precision is a relative error of 2⁻¹¹ on every colour the cell pass reads —
invisible in the picture (an eighth of an 8-bit level) but a hundred times
larger than float noise, and enough to put a handful of cells on the wrong side
of a close decision. `asctest` models it (`toHalfAndBack`) rather than
pretending it is not there. Forcing the buffer to 32-bit float makes the two
implementations agree on every cell of a 40×24 card with no tolerance at all;
the plugin keeps 16F because 32F would double a 4K copy buffer and its mip chain
for a difference nobody can see.

**`FFGLScopedFBOBinding.h` is not in the umbrella header.** `FFGLSDK.h` includes
every other scoped binding and omits that one (SDK `b1afaf9`). Include it by
hand; the symptom is an unknown-type error on `ScopedFBOBinding` and nothing
else.

**`ffglex::FFGLFBO::Release()` leaks the colour texture.** It deletes the
framebuffer and the depth renderbuffer, then tests `depthBufferID` a second time
where it plainly meant `colorTextureID`. `PassBuffer::Destroy()` deletes it
first. This matters here rather than being pedantry: the cell buffer is
reallocated every time Columns moves, and an operator drags that.

**A ranged parameter cannot have a ranged default.** `SetParamInfo` clamps an
`FF_TYPE_STANDARD` default into 0..1 *before* returning, and `SetParamRange` can
only be called afterwards. So every host parameter here is 0..1 and the
conversions live in `Controls.cpp`.

**The plugin registers itself from a file-scope constructor.** `CFFGLPluginInfo`
is never referenced by name, so in a **STATIC** archive the linker may drop the
whole translation unit, giving a bundle that loads, exports `plugMain`, and
reports that it contains no plugins. The core is an **OBJECT** library for that
reason. Verify with `nm -gU … | grep _plugMain` plus an actual host load.

**The input texture can be bigger than the picture.** `MaxUV` is the fraction
really drawn. The copy pass resolves it once, and every pass after that works on
a texture we allocated where the picture does fill the texture.

**`flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are GLSL
reserved words**, and a shader that fails to compile surfaces only at runtime,
as "the effect does nothing". The diagnostics log names which pass.

**Non-ASCII string literals are a compiler-encoding question.** The block and
box alphabets in `Alphabet.cpp` are listed by code point, not as literals, for
that reason; getting the encoding wrong does not fail the build, it silently
produces an alphabet of characters the font does not draw. `/utf-8` is set for
MSVC anyway, for the comments.

---

## Shape of the code

    source/FontData.cpp   the typeface, drawn as ASCII art. The only description
                          of the font anywhere. 123 glyphs.
    source/Font.{h,cpp}   parse it (this is where top-down art becomes bottom-up
                          GL), build the atlas, decode UTF-8.
    source/Match.{h,cpp}  the moments and the cost function. Mirrored in GLSL.
    source/Alphabet.*     which characters are in play. Lists only; no ordering.
    source/Controls.*     0..1 host parameters to physical units.
    source/Shaders.cpp    the three passes. The cell shader mirrors Match.cpp.
    source/PassBuffer.*   FFGLFBO with the leak fixed and filtering it owns.
    source/Asciify.*      the plugin: parameters, alphabet upload, the passes.
    source/Diag.*         a log file, for the shader that will not compile.
    tools/asctest/        the offline harness. See below.
    tools/sweep.py        no control is silently dead.
    tools/verify.sh       all of it.

Three passes, at three resolutions, because a character is chosen for a cell and
not for a pixel:

1. **copy** — picture size. Resolves `MaxUV`, then has mipmaps generated on it.
   The mip chain is the point: a cell may be four source pixels across or four
   hundred, and a hardware box filter is right at any ratio for one fetch.
2. **cell** — one pixel per character. Measures the cell as an 8×8 patch and
   compares it against every glyph in the alphabet. The expensive pass, running
   at a few tens of thousands of pixels rather than a few million, which is what
   makes an exhaustive search affordable.
3. **type** — output size. Looks up what its cell chose and draws that part of
   it. No decisions, so two pixels in one cell cannot disagree.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (M4 Max, macOS 26.4):**

- **The GPU picks the character the C++ predicts.** `asctest --match` renders at
  exactly one output pixel per glyph pixel, so each cell of the frame *is* an
  8×8 glyph bitmap that can be read back and named. Across five Structure
  settings × eight character sets × three custom alphabets — 43 runs of 960
  cells — **zero disagreements**. Ties (two characters within the half-float
  step) are reported separately and are not counted as passes.
- **The font's invariants.** 123 glyphs, all inside the 128-slot atlas, no code
  point drawn twice, no two glyphs drawn identically. The last is a precondition
  of `--match` being able to attribute a rendered cell at all.
- **No dead controls.** All 18 parameters measurably change the picture,
  including the text parameter, which `--set` cannot reach and which
  `asctest --custom` exists for.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`.
- **Windows compiles**, proven by a `workflow_dispatch` run that built the macOS
  universal bundle, the Windows x64 `.dll` and the NSIS installer green before
  anything was tagged. Dispatching that workflow is the cheap way to test an
  FFGL build without publishing: the release job is gated on `refs/tags/v*`, so
  a manual run builds and skips publication.
- **The shade ramp is exact quarters.** ░▒▓█ measure 0.25/0.50/0.75/1.00. This
  was wrong first time — ░ was drawn at 0.125 and ▓ at 0.875 — and the
  lopsidedness showed up as a single knife-edge cell that `--match` failed on,
  which is how it was found.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** The bundle installs to
  `~/Documents/Resolume Arena/Extra Effects/` and is universal, but nothing here
  has driven the host. Everything about how the parameters *present* — whether
  the Ink/Paper triples show as colour swatches, whether the `FF_TYPE_TEXT`
  Custom Set field appears at all, whether the groups read sensibly — is
  untested. The text parameter is the least certain: the SDK supports it and
  Resolume's own example uses it, but that is not the same as having seen it.
- **Never built on Linux.** There is no CI job for it, and nothing has tried. The
  code uses nothing platform-specific outside `Diag.cpp`, but that is an
  argument rather than a build.
- **Nothing timed.** The cell pass compares every glyph in the alphabet against
  every cell — up to 95 comparisons per cell for the ASCII set, ~123 for a full
  custom alphabet. At 320 columns on a 4K frame that is 320×180 cells × 95, which
  ought to be nothing on a modern GPU, but "ought to be" is not a measurement.
- **The `Glyph Edge` control is judged by eye.** Crisp aliases at small cell
  sizes and Smooth blurs at large ones; where the crossover sits has not been
  measured, and it is left as an explicit choice rather than being guessed from
  the cell size.

---

## Siblings

The CMake MODULE + FFGL-submodule pattern, the `Diag` logger, the offline-harness
shape and `sweep.py` all come from **porthole** and **old-cathode**, which are
the other two FFGL effects in the fleet. `PassBuffer` is old-cathode's, with the
sampling mode added. The `--pipe` frame format and the `--script` automation file
are identical across all three on purpose, so one build script can film any of
them.
