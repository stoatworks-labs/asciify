#include "Font.h"

#include <map>

namespace asciify
{
namespace
{
/// The one place the drawn font is turned the right way up for OpenGL. Row 0 of
/// the art is the top of the character; row 0 of a Glyph is the bottom.
Glyph ParseGlyph( const GlyphArt& art )
{
	Glyph glyph;
	glyph.codepoint = art.codepoint;

	for( int artRow = 0; artRow < kGlyphSize; ++artRow )
	{
		const char* line = art.rows[ artRow ];
		const int row    = kGlyphSize - 1 - artRow;

		uint8_t bits = 0;
		for( int col = 0; col < kGlyphSize; ++col )
		{
			//A short line is padded with paper rather than rejected. The art is
			//source code, and a missing trailing dot should not be a crash.
			if( line[ col ] == '\0' )
				break;
			if( line[ col ] != '.' && line[ col ] != ' ' )
				bits |= static_cast< uint8_t >( 1u << col );
		}
		glyph.bits[ row ] = bits;
	}

	return glyph;
}

const std::map< uint32_t, int >& SlotIndex()
{
	static const std::map< uint32_t, int > index = [] {
		std::map< uint32_t, int > built;
		const std::vector< Glyph >& glyphs = Glyphs();
		for( size_t i = 0; i < glyphs.size(); ++i )
			built[ glyphs[ i ].codepoint ] = static_cast< int >( i );
		return built;
	}();
	return index;
}
} // namespace

const std::vector< Glyph >& Glyphs()
{
	static const std::vector< Glyph > glyphs = [] {
		size_t count            = 0;
		const GlyphArt* art     = FontArt( count );
		const size_t maxGlyphs  = static_cast< size_t >( kAtlasCols ) * kAtlasRows;

		//Silently dropping glyphs past the end of the atlas is better than
		//addressing outside it, and the assert that matters is in the test
		//harness, which counts them.
		if( count > maxGlyphs )
			count = maxGlyphs;

		std::vector< Glyph > built;
		built.reserve( count );
		for( size_t i = 0; i < count; ++i )
			built.push_back( ParseGlyph( art[ i ] ) );
		return built;
	}();
	return glyphs;
}

int SlotForCodepoint( uint32_t codepoint )
{
	const std::map< uint32_t, int >& index = SlotIndex();
	auto found                             = index.find( codepoint );
	return found == index.end() ? -1 : found->second;
}

std::vector< uint8_t > BuildAtlasImage()
{
	const int width  = kAtlasCols * kSlotSize;
	const int height = kAtlasRows * kSlotSize;

	//Cleared to paper, which is what makes the one-texel border round every
	//slot: the glyph is drawn inset by one, and the frame it leaves behind is
	//what stops GL_LINEAR reaching into the character next door.
	std::vector< uint8_t > image( static_cast< size_t >( width ) * height, 0 );

	const std::vector< Glyph >& glyphs = Glyphs();
	for( size_t slot = 0; slot < glyphs.size(); ++slot )
	{
		const int slotX = static_cast< int >( slot ) % kAtlasCols;
		const int slotY = static_cast< int >( slot ) / kAtlasCols;

		for( int row = 0; row < kGlyphSize; ++row )
		{
			for( int col = 0; col < kGlyphSize; ++col )
			{
				if( !glyphs[ slot ].Ink( col, row ) )
					continue;

				const int x = slotX * kSlotSize + 1 + col;
				const int y = slotY * kSlotSize + 1 + row;
				image[ static_cast< size_t >( y ) * width + x ] = 255;
			}
		}
	}

	return image;
}

std::vector< uint32_t > DecodeUtf8( const std::string& text )
{
	std::vector< uint32_t > points;
	points.reserve( text.size() );

	size_t i = 0;
	while( i < text.size() )
	{
		const unsigned char lead = static_cast< unsigned char >( text[ i ] );

		int extra        = 0;
		uint32_t decoded = 0;
		if( lead < 0x80 )
		{
			decoded = lead;
		}
		else if( ( lead & 0xE0 ) == 0xC0 )
		{
			extra   = 1;
			decoded = lead & 0x1Fu;
		}
		else if( ( lead & 0xF0 ) == 0xE0 )
		{
			extra   = 2;
			decoded = lead & 0x0Fu;
		}
		else if( ( lead & 0xF8 ) == 0xF0 )
		{
			extra   = 3;
			decoded = lead & 0x07u;
		}
		else
		{
			//A stray continuation byte or an illegal lead. Skip it: this string
			//came from a text field in another application, and refusing to
			//parse it would mean an operator's typo silently emptying the
			//alphabet.
			++i;
			continue;
		}

		if( i + static_cast< size_t >( extra ) >= text.size() )
			break;

		bool valid = true;
		for( int k = 1; k <= extra; ++k )
		{
			const unsigned char continuation = static_cast< unsigned char >( text[ i + k ] );
			if( ( continuation & 0xC0 ) != 0x80 )
			{
				valid = false;
				break;
			}
			decoded = ( decoded << 6 ) | ( continuation & 0x3Fu );
		}

		if( valid )
		{
			points.push_back( decoded );
			i += static_cast< size_t >( extra ) + 1;
		}
		else
		{
			++i;
		}
	}

	return points;
}

} // namespace asciify
