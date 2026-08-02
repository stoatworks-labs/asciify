#include "Match.h"

#include <cmath>
#include <limits>

namespace asciify
{
namespace
{
/// Sample centre k of eight, mapped to -1..1. Centres, not edges: the grid is
/// symmetric about zero, which is what makes the odd moments vanish for a
/// symmetric glyph.                                                 //= mirrored
inline float Axis( int k )
{
	return ( static_cast< float >( k ) + 0.5f ) / 4.0f - 1.0f;
}
} // namespace

float Dither4x4( int cellX, int cellY )
{
	static const float kMatrix[ 16 ] = {
		 0.0f,  8.0f,  2.0f, 10.0f,
		12.0f,  4.0f, 14.0f,  6.0f,
		 3.0f, 11.0f,  1.0f,  9.0f,
		15.0f,  7.0f, 13.0f,  5.0f
	};

	//Truncating division, to match GLSL's integer division on negatives. Cell
	//indices are never negative in practice; this is so that the mirror holds
	//without having to say "as long as".
	const int x = cellX - 4 * ( cellX / 4 );
	const int y = cellY - 4 * ( cellY / 4 );

	return ( kMatrix[ y * 4 + x ] + 0.5f ) / 16.0f - 0.5f;
}

Moments Measure( const float ink[ kGlyphSize * kGlyphSize ] )
{
	Moments result;

	float sum[ 6 ] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	for( int row = 0; row < kGlyphSize; ++row )
	{
		const float v = Axis( row );
		for( int col = 0; col < kGlyphSize; ++col )
		{
			const float u = Axis( col );
			const float x = ink[ row * kGlyphSize + col ];

			sum[ 0 ] += x;
			sum[ 1 ] += x * u;
			sum[ 2 ] += x * v;
			sum[ 3 ] += x * u * v;
			sum[ 4 ] += x * ( u * u - kMomentC );
			sum[ 5 ] += x * ( v * v - kMomentC );
		}
	}

	constexpr float kCells = static_cast< float >( kGlyphSize * kGlyphSize );

	result.coverage   = sum[ 0 ] / kCells;
	result.shape[ 0 ] = sum[ 1 ] / kCells * kScaleLinear;
	result.shape[ 1 ] = sum[ 2 ] / kCells * kScaleLinear;
	result.shape[ 2 ] = sum[ 3 ] / kCells * kScaleCross;
	result.shape[ 3 ] = sum[ 4 ] / kCells * kScaleQuad;
	result.shape[ 4 ] = sum[ 5 ] / kCells * kScaleQuad;

	return result;
}

Moments MeasureGlyph( const Glyph& glyph )
{
	float ink[ kGlyphSize * kGlyphSize ];
	for( int row = 0; row < kGlyphSize; ++row )
		for( int col = 0; col < kGlyphSize; ++col )
			ink[ row * kGlyphSize + col ] = glyph.Ink( col, row ) ? 1.0f : 0.0f;

	return Measure( ink );
}

float ShapeAllowance( float structure, float coverLow, float coverHigh )
{
	const float range = coverHigh - coverLow;
	return structure * kShapeAllowance * ( range > 0.0f ? range : 0.0f );
}

float MatchCost( const Moments& glyph, const Moments& cell, float allowance )
{
	const float toneError = glyph.coverage - cell.coverage;
	float cost            = toneError * toneError;

	if( allowance <= 0.0f )
		return cost;

	float dot        = 0.0f;
	float cellLength = 0.0f;
	float glyphLength = 0.0f;
	for( int k = 0; k < 5; ++k )
	{
		dot += cell.shape[ k ] * glyph.shape[ k ];
		cellLength += cell.shape[ k ] * cell.shape[ k ];
		glyphLength += glyph.shape[ k ] * glyph.shape[ k ];
	}
	cellLength  = std::sqrt( cellLength );
	glyphLength = std::sqrt( glyphLength );

	//A glyph with no direction of its own -- a space, a full block, anything
	//symmetric -- is neither aligned nor opposed, so it scores zero rather than
	//being treated as a mismatch. It still pays the full penalty in a cell that
	//does have a direction, which is correct: a structured cell should not come
	//out blank.
	float alignment = 0.0f;
	if( cellLength > 1e-6f && glyphLength > 1e-6f )
		alignment = dot / ( cellLength * glyphLength );

	const float confidence = cellLength < kShapeFloor ? cellLength / kShapeFloor : 1.0f;

	//Squared, because it is being added to a squared coverage error: an
	//allowance of 0.09 is exactly the cost of being 0.09 out in coverage.
	return cost + allowance * allowance * confidence * ( 1.0f - alignment );
}

int ChooseGlyph( const std::vector< Moments >& alphabet, const Moments& cell, float allowance )
{
	if( alphabet.empty() )
		return 0;

	int best        = 0;
	float bestCost  = std::numeric_limits< float >::max();

	for( size_t i = 0; i < alphabet.size(); ++i )
	{
		const float cost = MatchCost( alphabet[ i ], cell, allowance );
		if( cost < bestCost )
		{
			bestCost = cost;
			best     = static_cast< int >( i );
		}
	}

	return best;
}

void CoverageRange( const std::vector< Moments >& alphabet, float& lowest, float& highest )
{
	lowest  = 0.0f;
	highest = 1.0f;

	if( alphabet.empty() )
		return;

	lowest  = alphabet[ 0 ].coverage;
	highest = alphabet[ 0 ].coverage;
	for( const Moments& glyph : alphabet )
	{
		if( glyph.coverage < lowest )
			lowest = glyph.coverage;
		if( glyph.coverage > highest )
			highest = glyph.coverage;
	}

	//An alphabet of one, or one whose characters all weigh the same -- the box
	//drawing set very nearly does -- would otherwise map every tone onto a
	//single point and render a flat field. Open the range out so the tone term
	//still separates them by whatever little there is.
	if( highest - lowest < 1e-4f )
	{
		highest = lowest + 1e-4f;
	}
}

} // namespace asciify
