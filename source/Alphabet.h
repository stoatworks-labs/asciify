#pragma once

#include <string>
#include <vector>

/**
    Which characters are allowed to appear.

    An alphabet is just a list of atlas slots. Nothing here orders them by
    weight or decides which is darkest -- that is measured in Match.cpp -- so
    these lists are only ever "what is in play", and they can be written in
    whatever order reads best in this file.

    Every set includes the space, deliberately and without exception. A space is
    the only character that can render zero ink, so an alphabet without one has
    no true black (or no true white, inverted) and the darkest part of the
    picture comes out as a field of full stops. That is a mistake worth making
    impossible rather than documenting.
*/
namespace asciify
{
enum class Set : int
{
	Ascii = 0,  ///< every printable character, 32..126
	Letters,    ///< A-Z a-z
	Digits,     ///< 0-9
	Symbols,    ///< punctuation and operators
	Classic,    ///< the traditional " .:-=+*#%@" ramp, for when that is the look
	Binary,     ///< 0 and 1
	Blocks,     ///< block elements: the only set with a full tonal range
	Box,        ///< box drawing: almost no tonal range, all structure
	Custom,     ///< whatever was typed into the Characters field

	Count
};

/// The atlas slots in play. `custom` is only read for Set::Custom, and if it
/// contains nothing this font can draw, the ASCII set is returned instead --
/// an empty alphabet would render a blank frame with no clue as to why.
std::vector< int > SlotsFor( Set set, const std::string& custom );

/// Name as shown in the host's parameter list.
const char* SetName( Set set );

} // namespace asciify
