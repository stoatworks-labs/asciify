#include "Alphabet.h"

#include "Font.h"

#include <algorithm>

namespace asciify
{
namespace
{
/// Append the slot for a code point if the font draws it, ignoring repeats. A
/// repeated character is not an error -- somebody weighting a ramp by typing
/// "..::##" is being perfectly reasonable -- but a repeat in the alphabet costs
/// a comparison per cell per frame and can never win one, so it is dropped.
void Add( std::vector< int >& slots, uint32_t codepoint )
{
	const int slot = SlotForCodepoint( codepoint );
	if( slot < 0 )
		return;
	if( std::find( slots.begin(), slots.end(), slot ) != slots.end() )
		return;
	slots.push_back( slot );
}

void AddRange( std::vector< int >& slots, uint32_t first, uint32_t last )
{
	for( uint32_t c = first; c <= last; ++c )
		Add( slots, c );
}

void AddString( std::vector< int >& slots, const char* utf8 )
{
	for( uint32_t c : DecodeUtf8( utf8 ) )
		Add( slots, c );
}
} // namespace

std::vector< int > SlotsFor( Set set, const std::string& custom )
{
	std::vector< int > slots;

	//First, always. See the header.
	Add( slots, U' ' );

	switch( set )
	{
	case Set::Ascii:
		AddRange( slots, 0x21, 0x7E );
		break;

	case Set::Letters:
		AddRange( slots, U'A', U'Z' );
		AddRange( slots, U'a', U'z' );
		break;

	case Set::Digits:
		AddRange( slots, U'0', U'9' );
		break;

	case Set::Symbols:
		AddRange( slots, 0x21, 0x2F );
		AddRange( slots, 0x3A, 0x40 );
		AddRange( slots, 0x5B, 0x60 );
		AddRange( slots, 0x7B, 0x7E );
		break;

	case Set::Classic:
		//The ramp everybody knows. Kept because it is a recognisable look and
		//because somebody will want it -- not because the ordering means
		//anything here. It is re-measured like every other set, and on this
		//font the measured order is not quite the traditional one.
		AddString( slots, ".:-=+*#%@" );
		break;

	case Set::Binary:
		AddString( slots, "01" );
		break;

	//The block and box sets are listed by code point rather than as literals.
	//A non-ASCII literal is only as good as the compiler's idea of what
	//encoding this file is in, and getting that wrong does not fail the build:
	//it silently produces an alphabet of characters the font does not draw,
	//which looks exactly like the set being broken. The names are in the
	//comments, and FontData.cpp draws them under the same numbers.
	case Set::Blocks:
		Add( slots, 0x2591 );//light shade
		Add( slots, 0x2592 );//medium shade
		Add( slots, 0x2593 );//dark shade
		Add( slots, 0x2588 );//full block
		Add( slots, 0x2580 );//upper half
		Add( slots, 0x2584 );//lower half
		Add( slots, 0x258C );//left half
		Add( slots, 0x2590 );//right half
		Add( slots, 0x2598 );//quadrant upper left
		Add( slots, 0x259D );//quadrant upper right
		Add( slots, 0x2596 );//quadrant lower left
		Add( slots, 0x2597 );//quadrant lower right
		Add( slots, 0x259A );//quadrants upper left and lower right
		Add( slots, 0x259E );//quadrants upper right and lower left
		break;

	case Set::Box:
		Add( slots, 0x2500 );//horizontal
		Add( slots, 0x2502 );//vertical
		Add( slots, 0x250C );//down and right
		Add( slots, 0x2510 );//down and left
		Add( slots, 0x2514 );//up and right
		Add( slots, 0x2518 );//up and left
		Add( slots, 0x251C );//vertical and right
		Add( slots, 0x2524 );//vertical and left
		Add( slots, 0x252C );//down and horizontal
		Add( slots, 0x2534 );//up and horizontal
		Add( slots, 0x253C );//vertical and horizontal
		Add( slots, 0x2571 );//diagonal rising
		Add( slots, 0x2572 );//diagonal falling
		Add( slots, 0x2573 );//diagonal cross
		break;

	case Set::Custom:
		for( uint32_t c : DecodeUtf8( custom ) )
			Add( slots, c );
		//Nothing typed, or nothing typed that this font can draw. Falling back
		//to ASCII beats rendering an empty frame: an operator who has mistyped
		//sees a picture that is wrong rather than a picture that is missing.
		if( slots.size() <= 1 )
			return SlotsFor( Set::Ascii, std::string() );
		break;

	default:
		return SlotsFor( Set::Ascii, std::string() );
	}

	return slots;
}

const char* SetName( Set set )
{
	switch( set )
	{
	case Set::Ascii:   return "ASCII";
	case Set::Letters: return "Letters";
	case Set::Digits:  return "Digits";
	case Set::Symbols: return "Symbols";
	case Set::Classic: return "Classic ramp";
	case Set::Binary:  return "Binary";
	case Set::Blocks:  return "Blocks";
	case Set::Box:     return "Box drawing";
	case Set::Custom:  return "Custom";
	default:           return "ASCII";
	}
}

} // namespace asciify
