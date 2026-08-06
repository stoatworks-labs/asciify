#pragma once

/// The two phases of Asciify, over bare pointers, with no FxPlug in it.
///
/// Same split as the other ports: `renderDestinationImage:` can only be called
/// by a host, so everything that can go wrong on its own lives out here where a
/// test can drive it. `FxSurface.h` handles pixel layouts; the alphabet,
/// matching and control curves come from `../source/`, shared with the FFGL and
/// OpenFX builds.
///
/// A straight transcription of the OpenFX processor
/// (`../source/ofx/AsciifyOFX.cpp`). Where the two differ, that one is right
/// and this is the bug.
///
/// Unlike Luma Key, and like Porthole, this reads outside its tile — a cell
/// spans many source pixels — so the plug-in declares
/// `kFxPropertyKey_NeedsFullBuffer` and asks for the whole source image.
///
/// The phases cannot be merged: every output pixel needs the glyph chosen for
/// its cell, and choosing it needs the whole cell. So cells are built first,
/// over the full picture, and the typing pass reads that.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "FxSurface.h"

#include "Alphabet.h"
#include "Controls.h"
#include "Font.h"
#include "Match.h"

namespace asciify
{

using fxsurface::Layout;

/// The longest custom alphabet carried across the pluginState blob. That blob
/// is raw bytes, so the string cannot be a std::string; 256 is far past any
/// usable alphabet and keeps the struct a POD.
constexpr int kMaxCustomSet = 256;

/// Everything the two phases need, snapshotted from the parameters. POD of
/// fixed-width fields, because this crosses to the render threads as raw bytes.
struct AsciifyState
{
	float columns;
	float structure;
	float tone;
	float contrast;
	float dither;
	float tint;
	float inkR, inkG, inkB;
	float paperR, paperG, paperB;
	float paperOpacity;
	float mix;
	int32_t set;
	int32_t smoothEdge;
	uint32_t invert;
	char customSet[ kMaxCustomSet ];
};

/// One cell's decision: which glyph, and the colour to tint it with.
struct CellChoice
{
	int slot    = 0;   //!< atlas slot of the chosen glyph
	float r     = 0.0f;//!< the cell's own straight colour, for Tint
	float g     = 0.0f;
	float b     = 0.0f;
	float alpha = 0.0f;//!< the picture's average alpha over the cell
};

/// The grid, and the glyph chosen for every cell in it.
struct CellGrid
{
	int columns = 80;
	int rows    = 45;
	std::vector<CellChoice> cells;
};

/// The atlas bitmap, built once per process. Read-only after that, so it is
/// shared across render threads and instances without ceremony.
inline const std::vector<uint8_t>& AtlasImage()
{
	static const std::vector<uint8_t>* image = new std::vector<uint8_t>( BuildAtlasImage() );
	return *image;
}

constexpr int kAtlasW = kAtlasCols * kSlotSize;
constexpr int kAtlasH = kAtlasRows * kSlotSize;

/// One atlas texel, 0..1. Out-of-range reads land in a slot's blank border by
/// construction, but clamp anyway: arithmetic at the frame edge should never be
/// able to read outside the image.
inline float atlasTexel( const std::vector<uint8_t>& atlas, int x, int y )
{
	x = std::clamp( x, 0, kAtlasW - 1 );
	y = std::clamp( y, 0, kAtlasH - 1 );
	return atlas[ size_t( y ) * kAtlasW + x ] / 255.0f;
}

/// The atlas sampled the way the GPU samples it: NEAREST for crisp glyph edges,
/// LINEAR for smooth.
inline float sampleAtlas( const std::vector<uint8_t>& atlas, double tx, double ty, bool smooth )
{
	if( !smooth )
		return atlasTexel( atlas, int( std::floor( tx ) ), int( std::floor( ty ) ) );

	const double fx = tx - 0.5;
	const double fy = ty - 0.5;
	const int x0    = int( std::floor( fx ) );
	const int y0    = int( std::floor( fy ) );
	const double ax = fx - x0;
	const double ay = fy - y0;

	const float top = atlasTexel( atlas, x0, y0 ) * float( 1.0 - ax )
					  + atlasTexel( atlas, x0 + 1, y0 ) * float( ax );
	const float bottom = atlasTexel( atlas, x0, y0 + 1 ) * float( 1.0 - ax )
						 + atlasTexel( atlas, x0 + 1, y0 + 1 ) * float( ax );
	return top * float( 1.0 - ay ) + bottom * float( ay );
}

/// A source image addressed in pixels, premultiplied RGBA out. FxPlug images
/// are already premultiplied, so unlike the OpenFX build there is nothing to
/// undo here.
class SourceImage
{
public:
	SourceImage( const uint8_t* base, size_t stride, Layout layout, int width, int height ) :
		_base( base ), _stride( stride ), _layout( layout ),
		_bpp( fxsurface::bytesPerPixel( layout ) ), _width( width ), _height( height )
	{
	}

	int width() const { return _width; }
	int height() const { return _height; }

	void read( int x, int y, double out[ 4 ] ) const
	{
		x = std::clamp( x, 0, _width - 1 );
		y = std::clamp( y, 0, _height - 1 );
		float rgba[ 4 ];
		fxsurface::readPixel( _base + size_t( y ) * _stride + size_t( x ) * _bpp, _layout, rgba );
		for( int c = 0; c < 4; ++c )
			out[ c ] = rgba[ c ];
	}

private:
	const uint8_t* _base;
	size_t _stride;
	Layout _layout;
	size_t _bpp;
	int _width;
	int _height;
};

// ------------------------------------------------------------------ phase one

/// Measure the picture and choose a glyph for every cell.
inline CellGrid buildCells( const SourceImage& src, const AsciifyState& state )
{
	const int srcW = src.width();
	const int srcH = src.height();

	CellGrid grid;

	// The grid. Rows follow from columns and the output aspect, so a cell is as
	// square as an integer count allows.
	grid.columns = ColumnsFromParam( state.columns );
	grid.rows    = std::max( 1, int( std::lround( double( grid.columns ) * srcH / std::max( 1, srcW ) ) ) );
	grid.columns = std::min( grid.columns, std::max( 1, srcW ) );
	grid.rows    = std::min( grid.rows, std::max( 1, srcH ) );

	// The alphabet, measured. SlotsFor falls back to ASCII when a custom string
	// contains nothing this font draws.
	const std::string customText( state.customSet,
								  strnlen( state.customSet, kMaxCustomSet ) );
	const std::vector<int> slots = SlotsFor( Set( state.set ), customText );

	std::vector<Moments> moments;
	moments.reserve( slots.size() );
	const std::vector<Glyph>& glyphs = Glyphs();
	for( int slot : slots )
		moments.push_back( MeasureGlyph( glyphs[ size_t( slot ) ] ) );

	float coverLow = 0.0f, coverHigh = 1.0f;
	CoverageRange( moments, coverLow, coverHigh );

	const float allowance     = ShapeAllowance( state.structure, coverLow, coverHigh );
	const float gamma         = GammaFromParam( state.tone );
	const float contrastValue = ContrastFromParam( state.contrast );
	const bool invertValue    = state.invert != 0;

	grid.cells.resize( size_t( grid.columns ) * grid.rows );

	for( int cellY = 0; cellY < grid.rows; ++cellY )
	{
		// Cell bounds in source pixels. Integer edges from the exact fractions
		// so the cells tile the picture without gaps.
		const int py0 = int( double( cellY ) * srcH / grid.rows );
		const int py1 = std::max( py0 + 1, int( double( cellY + 1 ) * srcH / grid.rows ) );

		for( int cellX = 0; cellX < grid.columns; ++cellX )
		{
			const int px0 = int( double( cellX ) * srcW / grid.columns );
			const int px1 = std::max( px0 + 1, int( double( cellX + 1 ) * srcW / grid.columns ) );

			// One quantisation step of tone: the gap between neighbours in the
			// ramp.
			const float ditherStep = state.dither * Dither4x4( cellX, cellY )
									 / std::max( 1.0f, float( moments.size() ) - 1.0f );

			float ink[ kGlyphSize * kGlyphSize ];
			double colourSum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

			for( int row = 0; row < kGlyphSize; ++row )
			{
				// Sub-cell pixel range. Row 0 is the bottom, matching both the
				// glyph bitmaps and FxPlug's bottom-up images.
				const int sy0 = py0 + int( double( row ) * ( py1 - py0 ) / kGlyphSize );
				const int sy1 = std::max( sy0 + 1,
										  py0 + int( double( row + 1 ) * ( py1 - py0 ) / kGlyphSize ) );

				for( int col = 0; col < kGlyphSize; ++col )
				{
					const int sx0 = px0 + int( double( col ) * ( px1 - px0 ) / kGlyphSize );
					const int sx1 = std::max( sx0 + 1,
											  px0 + int( double( col + 1 ) * ( px1 - px0 ) / kGlyphSize ) );

					double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
					int count       = 0;
					for( int sy = sy0; sy < sy1; ++sy )
						for( int sx = sx0; sx < sx1; ++sx )
						{
							double p[ 4 ];
							src.read( std::min( sx, srcW - 1 ), std::min( sy, srcH - 1 ), p );
							for( int c = 0; c < 4; ++c )
								sum[ c ] += p[ c ];
							++count;
						}
					if( count > 0 )
						for( int c = 0; c < 4; ++c )
							sum[ c ] /= count;

					for( int c = 0; c < 4; ++c )
						colourSum[ c ] += sum[ c ];

					// Straight colour for the luminance: a dark pixel and a
					// transparent pixel are not the same thing.
					const double straightR = sum[ 0 ] / std::max( sum[ 3 ], 1.0 / 255.0 );
					const double straightG = sum[ 1 ] / std::max( sum[ 3 ], 1.0 / 255.0 );
					const double straightB = sum[ 2 ] / std::max( sum[ 3 ], 1.0 / 255.0 );
					const float luma = float( 0.2126 * straightR + 0.7152 * straightG + 0.0722 * straightB );

					float toneValue = invertValue ? 1.0f - luma : luma;
					toneValue       = std::clamp( ( toneValue - 0.5f ) * contrastValue + 0.5f, 0.0f, 1.0f );
					toneValue       = std::pow( toneValue, gamma );
					toneValue       = std::clamp( toneValue + ditherStep, 0.0f, 1.0f );

					ink[ row * kGlyphSize + col ] = coverLow + ( coverHigh - coverLow ) * toneValue;
				}
			}

			const Moments cellMoments = Measure( ink );
			const int best            = ChooseGlyph( moments, cellMoments, allowance );

			CellChoice& out = grid.cells[ size_t( cellY ) * grid.columns + cellX ];
			out.slot        = slots[ size_t( best ) ];

			// Straight colour, weighted by alpha: a cell that is mostly
			// transparent takes its colour from the part that is not.
			const double alphaSum = std::max( colourSum[ 3 ], 1.0 / 255.0 );
			out.r                 = float( colourSum[ 0 ] / alphaSum );
			out.g                 = float( colourSum[ 1 ] / alphaSum );
			out.b                 = float( colourSum[ 2 ] / alphaSum );
			out.alpha             = float( colourSum[ 3 ] / 64.0 );
		}
	}

	return grid;
}

// ------------------------------------------------------------------ phase two

/// Type the chosen glyphs into one destination tile.
///
/// `dstOriginX/Y` is where this tile sits inside the full destination image and
/// `imageW/H` is that image's size — the grid is defined over the whole
/// picture, so a tile cannot be treated as an image in its own right.
inline void typeTile( const SourceImage& src, const CellGrid& grid,
					  uint8_t* dstBase, size_t dstStride, Layout dstLayout,
					  int dstOriginX, int dstOriginY, int tileW, int tileH,
					  int imageW, int imageH, const AsciifyState& state )
{
	const size_t dstBpp        = fxsurface::bytesPerPixel( dstLayout );
	const std::vector<uint8_t>& atlas = AtlasImage();
	const bool smooth          = state.smoothEdge != 0;

	for( int y = 0; y < tileH; ++y )
	{
		uint8_t* dstRow = dstBase + size_t( y ) * dstStride;

		for( int x = 0; x < tileW; ++x )
		{
			const int imgX = dstOriginX + x;
			const int imgY = dstOriginY + y;

			const double gx = ( imgX + 0.5 ) * grid.columns / double( imageW );
			const double gy = ( imgY + 0.5 ) * grid.rows / double( imageH );

			const int cellX     = std::clamp( int( gx ), 0, grid.columns - 1 );
			const int cellY     = std::clamp( int( gy ), 0, grid.rows - 1 );
			const double localX = std::clamp( gx - cellX, 0.0, 1.0 );
			const double localY = std::clamp( gy - cellY, 0.0, 1.0 );

			const CellChoice& cell = grid.cells[ size_t( cellY ) * grid.columns + cellX ];

			// Slot to atlas texel: one-texel inset inside the 10x10 slot, the
			// blank border that keeps a smooth fetch on one character from
			// picking up the ink of the next.
			const int slotCol = cell.slot % kAtlasCols;
			const int slotRow = cell.slot / kAtlasCols;
			const double tx   = slotCol * kSlotSize + 1.0 + localX * 8.0;
			const double ty   = slotRow * kSlotSize + 1.0 + localY * 8.0;

			const float ink = sampleAtlas( atlas, tx, ty, smooth );

			const float typedR = state.paperR + ( ( state.inkR + ( cell.r - state.inkR ) * state.tint ) - state.paperR ) * ink;
			const float typedG = state.paperG + ( ( state.inkG + ( cell.g - state.inkG ) * state.tint ) - state.paperG ) * ink;
			const float typedB = state.paperB + ( ( state.inkB + ( cell.b - state.inkB ) * state.tint ) - state.paperB ) * ink;
			const float typedA = ( state.paperOpacity + ( 1.0f - state.paperOpacity ) * ink ) * cell.alpha;

			// Premultiplied, and mixed premultiplied, exactly as the shader.
			double plain[ 4 ];
			src.read( imgX, imgY, plain );

			double r = plain[ 0 ] + ( typedR * typedA - plain[ 0 ] ) * state.mix;
			double g = plain[ 1 ] + ( typedG * typedA - plain[ 1 ] ) * state.mix;
			double b = plain[ 2 ] + ( typedB * typedA - plain[ 2 ] ) * state.mix;
			double a = plain[ 3 ] + ( typedA - plain[ 3 ] ) * state.mix;

			r = std::min( r, a );
			g = std::min( g, a );
			b = std::min( b, a );

			const float out[ 4 ] = { float( r ), float( g ), float( b ), float( a ) };
			fxsurface::writePixel( dstRow + size_t( x ) * dstBpp, dstLayout, out );
		}
	}
}

} // namespace asciify
