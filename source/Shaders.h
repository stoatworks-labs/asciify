#pragma once

/**
    The three passes.

    Typesetting a picture is not a per-pixel operation, which is the whole
    reason this is not one shader. A character is chosen for a *cell*, from
    everything in that cell, and then the same choice is written across every
    output pixel the cell covers. So the work happens at three different
    resolutions and each one gets its own pass:

    1. **Copy**, at picture size. Resolves MaxUV into a texture of our own, and
       then has mipmaps generated on it. The mipmaps are the point: a cell may
       be four source pixels across or four hundred, and a hardware box filter
       gets the average right at any ratio for one fetch, where a fixed tap grid
       is either too slow or wrong. The host's own texture is never mipmapped --
       adding levels to somebody else's texture and changing its filter is not
       ours to do.

    2. **Cell**, at one pixel per character. Reads the cell as an 8x8 patch,
       measures it, and picks the character. This is the expensive pass and the
       only one that thinks; it runs at a few tens of thousands of pixels
       instead of a few million, which is what makes comparing every glyph in
       the alphabet against every cell affordable.

    3. **Type**, at output size. Looks up which character its cell chose and
       draws that part of it. No decisions -- and so no risk of two pixels in
       one cell disagreeing.

    Things in here that will catch you out:

    - **The cell buffer must be sampled with GL_NEAREST.** Its alpha channel is
      a glyph index, not a quantity. Interpolate it and a pixel on a cell
      boundary reads the average of two indices, which addresses a third
      character that neither cell chose.

    - **The matching maths is a mirror of Match.cpp.** Every line marked
      `//= mirrored` there is a line in here. `asctest --match` measures one
      against the other; nothing else will notice them drifting apart.

    - **Uniform names must match the C++ exactly.** A mismatch is not an error
      anywhere: glGetUniformLocation returns -1 and glUniform on -1 is a
      documented no-op, so the control is simply dead while everything compiles,
      links, loads and renders. `tools/sweep.py` is the only thing that catches
      that.

    - `flat`, `active`, `filter`, `input`, `output`, `sample` and `common` are
      GLSL reserved words, and a shader that fails to compile shows up only at
      runtime, as "the effect does nothing".
*/
namespace asciify
{
/// Shared by all three passes: passes UV through in 0..1 frame space.
extern const char* const kVertexShader;

/// Pass 1. Input texture, MaxUV resolved, into a texture we own.
extern const char* const kCopyShader;

/// Pass 2. One pixel per character cell. Writes (cell colour, glyph slot).
extern const char* const kCellShader;

/// Pass 3. Draws the chosen characters at output resolution.
extern const char* const kTypeShader;

} // namespace asciify
