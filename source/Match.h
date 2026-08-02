#pragma once

#include "Font.h"

#include <vector>

/**
    How a cell chooses its character.

    This is the whole idea of the plugin, so it is worth stating plainly before
    the arithmetic. A character cell is not "a brightness looked up in a ramp".
    It is a small picture, and the glyph that should stand in for it is the one
    whose ink is distributed most like that picture. Both halves of that -- how
    much ink, and where -- are measured off the same 8x8 grid, for the cell and
    for every glyph, using the same six numbers:

        m0 = 1           how much ink there is           (coverage)
        m1 = u           how far left or right it sits
        m2 = v           how far up or down it sits
        m3 = u*v         which diagonal it leans along
        m4 = u^2 - c     whether it is a column or spread across
        m5 = v^2 - c     whether it is a row or spread down

    That is the constant, linear and quadratic moments of the ink, and it is
    enough to tell `|` from `-` (m4 against m5), `/` from `\` (the sign of m3),
    and `.` from `'` (the sign of m2). The constant c is the mean of u^2 over
    the grid, which is what makes m4 and m5 blind to coverage -- so the five
    shape terms carry no tone and the tone term carries no shape.

    **Tone is matched in absolute terms; shape is matched as a direction.** The
    cost of a glyph is the squared coverage error, plus a penalty for the angle
    between the cell's shape vector and the glyph's. Only the angle, because a
    glyph's ink is hard-edged and binary while a cell's is soft, so their shape
    vectors are never the same length even when they plainly agree; comparing
    them by length would reject every directional glyph and quietly reduce the
    effect to a tone ramp.

    Two consequences fall out of that rather than being arranged, and both are
    load-bearing:

    - **Structure = 0 is exactly the classic ASCII ramp.** No separate code
      path: with the angle term weighted to nothing, cost is coverage error
      alone, which is a nearest-neighbour lookup in a ramp -- except that the
      ramp was measured off this font instead of being somebody's opinion.
    - **Flat cells ignore the control.** The confidence factor is the length of
      the cell's own shape vector, so a cell with nothing in it has no direction
      to be wrong about and the angle term fades out on its own. Nothing has to
      detect "is there an edge here".

    A `//= mirrored` comment marks every constant and every step that also
    exists in GLSL, in Shaders.cpp. Two copies of one formula drift, and the
    thing that stops it here is `asctest --match`, which renders through the
    real shader and checks the GPU picked the same character this file predicts,
    cell by cell.
*/
namespace asciify
{
/// mean( u^2 ) over the eight sample centres of the 8x8 grid.       //= mirrored
constexpr float kMomentC = 0.328125f;

/// Each moment divided by its own RMS over the grid, so that the five shape
/// terms are on one footing and the angle between two shape vectors means what
/// it looks like it means. Values are 1/sqrt(0.328125), 1/0.328125 and
/// 1/sqrt(0.08203125).                                              //= mirrored
constexpr float kScaleLinear = 1.745743f;
constexpr float kScaleCross  = 3.047619f;
constexpr float kScaleQuad   = 3.491486f;

/// A shape vector this long is taken as fully meant. Below it the angle term is
/// faded out in proportion, which is how flat cells opt out.         //= mirrored
constexpr float kShapeFloor = 0.04f;

/// How far the shape term is allowed to pull a cell away from its correct
/// weight, at Structure = 1, as a fraction of the alphabet's own tonal range.
///
/// Expressing it this way rather than as a cost weight is what makes the
/// Structure control usable. Cost is squared coverage error, so a weight that
/// reads as a sensible number covers an enormous span of behaviour: the first
/// tenth of the slider already overrides the fine end of the ramp, and the rest
/// changes almost nothing until it starts throwing tone away wholesale. The
/// first version of this was a plain weight and measured exactly that -- 18% of
/// the picture changed between Structure 0 and 0.25, and 0.7% over the whole
/// remaining three quarters. Sliding an allowance instead spreads the useful
/// range across the control, because the allowance is the thing the eye is
/// actually judging.
///
/// Relative to the alphabet's range, not absolute, so the control means the
/// same thing whatever is selected. A third of the range is a third of the ramp
/// whether that is the ASCII set spanning 0 to 0.31 or the block set spanning
/// the whole of 0 to 1 -- and it is the only reason the box-drawing set, whose
/// characters all weigh within a hair of each other, responds to this slider at
/// all.                                                             //= mirrored
constexpr float kShapeAllowance = 0.30f;

/// The six numbers, for a glyph or for a cell. Same measurement either way.
struct Moments
{
	float coverage = 0.0f;
	float shape[ 5 ] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
};

/// Ordered dither, in units of one quantisation step: -0.5..+0.5 over a 4x4
/// tile of cells. Mirrored rather than switched off for testing, because a
/// dither that is right on the GPU and wrong on the CPU is exactly the kind of
/// half-degree difference that makes a rendered frame look fine and a
/// measurement disagree.                                            //= mirrored
float Dither4x4( int cellX, int cellY );

/// Measure an 8x8 patch. `ink` is row-major with **row 0 at the bottom**, in
/// coverage units: the same scale a glyph's own ink is measured on, so that the
/// coverage term of a cell and of a glyph are directly comparable.  //= mirrored
Moments Measure( const float ink[ kGlyphSize * kGlyphSize ] );

/// Measure a glyph's bitmap. Ink is 1, paper is 0.
Moments MeasureGlyph( const Glyph& glyph );

/// The coverage error the shape term may buy, for a Structure setting and an
/// alphabet. Both MatchCost and the cell shader take this rather than the raw
/// control, so the control's meaning lives in exactly one place. //= mirrored
float ShapeAllowance( float structure, float coverLow, float coverHigh );

/// The cost of standing this glyph in for this cell. Lower is better.
///                                                                  //= mirrored
float MatchCost( const Moments& glyph, const Moments& cell, float allowance );

/// Index into `alphabet` of the cheapest glyph. Ties go to the earlier entry,
/// which is why the alphabets in Alphabet.cpp are declared in a sensible order.
int ChooseGlyph( const std::vector< Moments >& alphabet, const Moments& cell, float allowance );

/// Least and greatest coverage in an alphabet. Tone is mapped into this range
/// rather than into 0..1, so a set whose densest character is `@` still uses
/// its whole range instead of rendering everything in the dark half.
void CoverageRange( const std::vector< Moments >& alphabet, float& lowest, float& highest );

} // namespace asciify
