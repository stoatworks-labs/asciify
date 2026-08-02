#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
    The typeface, and what is measured off it.

    Asciify ships its own bitmap font rather than rasterising a system one.
    That is not stubbornness about dependencies: a font rasteriser would have to
    be carried on three platforms, the plugin would need a file parameter and a
    path that survives being moved to another machine, and the result would
    still have to be reduced to exactly this -- a small binary bitmap per
    character. So the font is source code, in `FontData.cpp`, drawn as ASCII art
    that can be read and edited in place.

    Every glyph is an 8x8 binary bitmap. Letters, digits and punctuation are
    drawn on a 5x7 body (columns 1..5, rows 0..6 from the top) with descenders
    reaching row 7, which is the classic dot-matrix metric and is why they look
    like a terminal rather than like a typeface. Block and box-drawing glyphs
    use the whole 8x8 cell, because that is what they are for.

    **Nothing downstream knows what a character looks like.** The alphabet is
    ordered by *measured* ink coverage, and the glyph for a cell is chosen by
    comparing measured moments -- see Match.h. The traditional " .:-=+*#%@"
    ramp is folklore: it is somebody's eye ordering somebody else's font, and it
    is wrong for any other font, including this one. Here the ordering is a
    consequence of the bitmaps. Redraw a glyph and its place in the ramp moves
    on its own; add a character and it slots in where its ink says it belongs.
    `asctest --ramp` prints that ordering, and `asctest --atlas` draws the
    bitmaps, so both halves of the claim are checkable.

    One convention worth stating once, because getting it wrong is invisible
    until the picture is upside down: `FontData.cpp` draws each glyph top row
    first, the way you read it. Everything past the parser is **bottom-up**, to
    match OpenGL's texture origin. The flip happens exactly once, in
    ParseGlyph.
*/
namespace asciify
{
/// Glyph bitmaps are 8x8. Not a tunable: the moment basis in Match.h, the cell
/// sampling grid in the shader and the atlas layout all assume it.
constexpr int kGlyphSize = 8;

/// Each glyph sits in a 10x10 atlas slot with a one-texel blank border. The
/// border is load-bearing -- with GL_LINEAR the type pass would otherwise fetch
/// a neighbouring character's ink along every glyph edge.
constexpr int kSlotSize = 10;

/// Atlas dimensions in slots. 16 x 8 is 128 places for the ~121 glyphs drawn.
constexpr int kAtlasCols = 16;
constexpr int kAtlasRows = 8;

struct Glyph
{
	uint32_t codepoint = 0;
	/// Row 0 is the **bottom** of the glyph. bits[ row ] bit c is column c,
	/// counting from the left.
	uint8_t bits[ kGlyphSize ] = { 0 };

	bool Ink( int col, int row ) const
	{
		return ( bits[ row ] >> col ) & 1u;
	}
};

/// The raw drawn form, exactly as FontData.cpp writes it: eight rows of eight
/// characters, **top row first**, '#' for ink. Nothing but the parser should
/// touch this -- use Glyphs() instead, which is the same font the right way up.
struct GlyphArt
{
	uint32_t codepoint;
	const char* rows[ kGlyphSize ];
};

/// The font as drawn. Defined in FontData.cpp and nowhere else.
const GlyphArt* FontArt( size_t& count );

/// The drawn inventory, in the order FontData.cpp declares it. A glyph's index
/// here is its atlas slot for the whole life of the process.
const std::vector< Glyph >& Glyphs();

/// Atlas slot for a Unicode code point, or -1 if the font does not draw it.
int SlotForCodepoint( uint32_t codepoint );

/// The atlas as a single-channel image, kAtlasCols*kSlotSize wide by
/// kAtlasRows*kSlotSize high, 255 for ink and 0 for paper, **bottom row first**
/// so it can be handed to glTexImage2D unchanged.
std::vector< uint8_t > BuildAtlasImage();

/// Split a UTF-8 string into code points. Malformed bytes are skipped rather
/// than guessed at: this parses whatever an operator typed into a text field in
/// somebody else's application, so it has to be unable to fail.
std::vector< uint32_t > DecodeUtf8( const std::string& text );

} // namespace asciify
