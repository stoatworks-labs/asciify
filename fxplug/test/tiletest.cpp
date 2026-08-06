/// Headless test of the FxPlug render path — no SDK, no host.
///
/// Asciify's own invariants are already covered by `asctest --match`, `--font`
/// and `--ramp`, which test the shared cores. What is untested until here is
/// what the FxPlug port adds on top: the two phases wired to real surfaces,
/// the fixed-size custom-alphabet blob, and tile placement.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../AsciifyTile.h"

namespace
{
int failures = 0;

void check( bool ok, const std::string& what )
{
	if( !ok )
	{
		std::printf( "  FAIL  %s\n", what.c_str() );
		++failures;
	}
	else
		std::printf( "  ok    %s\n", what.c_str() );
}

const int kW = 96;
const int kH = 72;

/// A picture with a luminance gradient plus colour, so both the tone response
/// and the Tint path have something to bite on.
std::vector<uint8_t> makeSource()
{
	std::vector<uint8_t> px( size_t( kW ) * kH * 4 );
	for( int y = 0; y < kH; ++y )
		for( int x = 0; x < kW; ++x )
		{
			uint8_t* p = px.data() + ( size_t( y ) * kW + x ) * 4;
			const int v = x * 255 / ( kW - 1 );
			p[ 0 ] = uint8_t( v );
			p[ 1 ] = uint8_t( y * 255 / ( kH - 1 ) );
			p[ 2 ] = uint8_t( 255 - v );
			p[ 3 ] = 255;
		}
	return px;
}

asciify::AsciifyState defaultState()
{
	asciify::AsciifyState s = {};
	s.columns      = 0.624f;// 80 columns
	s.structure    = 0.35f;
	s.tone         = 0.5f;
	s.contrast     = 0.5f;
	s.dither       = 0.5f;
	s.tint         = 0.0f;
	s.inkR = 0.60f; s.inkG = 1.00f; s.inkB = 0.70f;
	s.paperR = 0.02f; s.paperG = 0.05f; s.paperB = 0.03f;
	s.paperOpacity = 1.0f;
	s.mix          = 1.0f;
	s.set          = int( asciify::Set::Ascii );
	s.smoothEdge   = 0;
	s.invert       = 0;
	std::strncpy( s.customSet, "@%#*+=-:. ", asciify::kMaxCustomSet - 1 );
	return s;
}

std::vector<uint8_t> render( const std::vector<uint8_t>& src, const asciify::AsciifyState& state )
{
	std::vector<uint8_t> dst( size_t( kW ) * kH * 4, 0 );
	const asciify::SourceImage source( src.data(), size_t( kW ) * 4,
									   fxsurface::Layout::RGBA8, kW, kH );
	const asciify::CellGrid grid = asciify::buildCells( source, state );
	asciify::typeTile( source, grid, dst.data(), size_t( kW ) * 4, fxsurface::Layout::RGBA8,
					   0, 0, kW, kH, kW, kH, state );
	return dst;
}

// ----------------------------------------------------------------- the phases

void testItActuallyTypes()
{
	std::printf( "the two phases\n" );

	const std::vector<uint8_t> src = makeSource();
	const asciify::AsciifyState s  = defaultState();

	const asciify::SourceImage source( src.data(), size_t( kW ) * 4,
									   fxsurface::Layout::RGBA8, kW, kH );
	const asciify::CellGrid grid = asciify::buildCells( source, s );

	check( grid.columns > 1 && grid.rows > 1, "the grid has cells in both axes" );
	check( grid.cells.size() == size_t( grid.columns ) * grid.rows,
		   "every cell got a decision" );

	// A luminance gradient must not choose the same character everywhere — if
	// it does, the matching phase is not seeing the picture.
	int distinct = 0;
	std::vector<int> seen;
	for( const auto& c : grid.cells )
	{
		bool found = false;
		for( int s2 : seen )
			if( s2 == c.slot )
				found = true;
		if( !found )
		{
			seen.push_back( c.slot );
			++distinct;
		}
	}
	std::printf( "        %d distinct glyphs over %d cells\n", distinct, int( grid.cells.size() ) );
	check( distinct > 3, "a gradient chooses a range of characters, not one" );

	const std::vector<uint8_t> out = render( src, s );
	check( out != src, "the output is not the input" );
}

/// Mix at zero is the null. This is the cheapest way to catch a phase that
/// writes when it should not.
void testMixZeroIsTheOriginal()
{
	std::printf( "mix\n" );

	const std::vector<uint8_t> src = makeSource();
	asciify::AsciifyState s        = defaultState();
	s.mix                          = 0.0f;

	const std::vector<uint8_t> out = render( src, s );

	int worst = 0;
	for( size_t i = 0; i < src.size(); ++i )
		worst = std::max( worst, std::abs( int( src[ i ] ) - int( out[ i ] ) ) );
	check( worst <= 1, "Mix 0 leaves the picture alone" );
}

/// Invert must change which characters are chosen, not merely the colours.
void testInvertChangesTheChoice()
{
	std::printf( "invert\n" );

	const std::vector<uint8_t> src = makeSource();
	const asciify::SourceImage source( src.data(), size_t( kW ) * 4,
									   fxsurface::Layout::RGBA8, kW, kH );

	asciify::AsciifyState plain = defaultState();
	plain.dither                = 0.0f;// dither would mask the comparison
	asciify::AsciifyState flipped = plain;
	flipped.invert                = 1;

	const asciify::CellGrid a = asciify::buildCells( source, plain );
	const asciify::CellGrid b = asciify::buildCells( source, flipped );

	int differing = 0;
	for( size_t i = 0; i < a.cells.size(); ++i )
		if( a.cells[ i ].slot != b.cells[ i ].slot )
			++differing;

	check( differing > int( a.cells.size() ) / 4,
		   "Invert changes the glyph chosen for most cells" );
}

// -------------------------------------------------------------- custom set

/// The FxPlug-specific one. The custom alphabet crosses to the render threads
/// inside a fixed-size char array, so it must survive the trip and must be
/// truncated rather than overrun when someone pastes an essay into the field.
void testCustomAlphabet()
{
	std::printf( "custom alphabet through the fixed-size blob\n" );

	const std::vector<uint8_t> src = makeSource();
	const asciify::SourceImage source( src.data(), size_t( kW ) * 4,
									   fxsurface::Layout::RGBA8, kW, kH );

	asciify::AsciifyState s = defaultState();
	s.set                   = int( asciify::Set::Custom );
	std::memset( s.customSet, 0, asciify::kMaxCustomSet );
	std::strncpy( s.customSet, ".:oO", asciify::kMaxCustomSet - 1 );

	const asciify::CellGrid grid = asciify::buildCells( source, s );

	// Only characters from the custom set may appear.
	const std::vector<int> allowed = asciify::SlotsFor( asciify::Set::Custom, ".:oO" );
	bool allInSet = true;
	for( const auto& c : grid.cells )
	{
		bool found = false;
		for( int slot : allowed )
			if( slot == c.slot )
				found = true;
		if( !found )
			allInSet = false;
	}
	check( allInSet, "only the custom characters are used" );

	// An over-long alphabet must truncate cleanly, not run off the end.
	asciify::AsciifyState big = defaultState();
	big.set                   = int( asciify::Set::Custom );
	std::memset( big.customSet, 'W', asciify::kMaxCustomSet );
	big.customSet[ asciify::kMaxCustomSet - 1 ] = '\0';
	const asciify::CellGrid grid2 = asciify::buildCells( source, big );
	check( grid2.cells.size() == size_t( grid2.columns ) * grid2.rows,
		   "a full-length custom set renders without running off the array" );
}

// ---------------------------------------------------------------- tile placement

/// The grid is defined over the whole picture, so a destination tile must be
/// placed by its position in that picture. Rendering in strips and rendering
/// whole must agree, or the characters step at tile boundaries in the host and
/// nowhere else.
void testTiledMatchesWhole()
{
	std::printf( "tiled render matches whole render\n" );

	const std::vector<uint8_t> src = makeSource();
	const asciify::AsciifyState s  = defaultState();

	const asciify::SourceImage source( src.data(), size_t( kW ) * 4,
									   fxsurface::Layout::RGBA8, kW, kH );
	const asciify::CellGrid grid = asciify::buildCells( source, s );

	std::vector<uint8_t> whole( size_t( kW ) * kH * 4, 0 );
	asciify::typeTile( source, grid, whole.data(), size_t( kW ) * 4, fxsurface::Layout::RGBA8,
					   0, 0, kW, kH, kW, kH, s );

	std::vector<uint8_t> strips( size_t( kW ) * kH * 4, 0 );
	const int stripH = kH / 4;
	for( int strip = 0; strip < 4; ++strip )
	{
		const int y0 = strip * stripH;
		asciify::typeTile( source, grid,
						   strips.data() + size_t( y0 ) * kW * 4, size_t( kW ) * 4,
						   fxsurface::Layout::RGBA8,
						   0, y0, kW, stripH, kW, kH, s );
	}

	int worst = 0;
	for( size_t i = 0; i < whole.size(); ++i )
		worst = std::max( worst, std::abs( int( whole[ i ] ) - int( strips[ i ] ) ) );
	check( worst == 0, "four strips are byte-identical to one whole render" );
}

// -------------------------------------------------------------------- layouts

/// The channel-order trap, on this plugin's own output: Ink is a saturated
/// green, so a BGRA/RGBA swap is unmissable.
void testChannelOrder()
{
	std::printf( "channel order\n" );

	std::vector<uint8_t> srcRGBA = makeSource();
	std::vector<uint8_t> srcBGRA = srcRGBA;
	for( size_t i = 0; i < srcBGRA.size(); i += 4 )
		std::swap( srcBGRA[ i ], srcBGRA[ i + 2 ] );

	const asciify::AsciifyState s = defaultState();

	std::vector<uint8_t> outRGBA( size_t( kW ) * kH * 4, 0 );
	std::vector<uint8_t> outBGRA( size_t( kW ) * kH * 4, 0 );

	const asciify::SourceImage a( srcRGBA.data(), size_t( kW ) * 4, fxsurface::Layout::RGBA8, kW, kH );
	const asciify::SourceImage b( srcBGRA.data(), size_t( kW ) * 4, fxsurface::Layout::BGRA8, kW, kH );

	asciify::typeTile( a, asciify::buildCells( a, s ), outRGBA.data(), size_t( kW ) * 4,
					   fxsurface::Layout::RGBA8, 0, 0, kW, kH, kW, kH, s );
	asciify::typeTile( b, asciify::buildCells( b, s ), outBGRA.data(), size_t( kW ) * 4,
					   fxsurface::Layout::BGRA8, 0, 0, kW, kH, kW, kH, s );

	bool same = true;
	for( size_t i = 0; i < outRGBA.size(); i += 4 )
	{
		if( outRGBA[ i ] != outBGRA[ i + 2 ] ) same = false;
		if( outRGBA[ i + 1 ] != outBGRA[ i + 1 ] ) same = false;
		if( outRGBA[ i + 2 ] != outBGRA[ i ] ) same = false;
	}
	check( same, "the same picture in BGRA8 and RGBA8 types identically" );

	// Ink is green by default, so green must dominate somewhere.
	bool greenSomewhere = false;
	for( size_t i = 0; i < outRGBA.size(); i += 4 )
		if( outRGBA[ i + 1 ] > outRGBA[ i ] + 20 && outRGBA[ i + 1 ] > outRGBA[ i + 2 ] + 20 )
			greenSomewhere = true;
	check( greenSomewhere, "the green ink comes out green, not red or blue" );
}

} // namespace

int main()
{
	std::printf( "Asciify FxPlug tile render tests\n\n" );

	testItActuallyTypes();
	testMixZeroIsTheOriginal();
	testInvertChangesTheChoice();
	testCustomAlphabet();
	testTiledMatchesWhole();
	testChannelOrder();

	std::printf( "\n%s\n", failures == 0 ? "all passed" : "FAILURES PRESENT" );
	return failures == 0 ? 0 : 1;
}
