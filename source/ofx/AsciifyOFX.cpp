/// The OpenFX build of Asciify, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// The idea, the font and the matching maths all live once — Match.cpp,
/// Font.cpp, Alphabet.cpp, Controls.cpp — and this file links them. What is
/// mirrored here is the three GPU passes of Shaders.cpp, collapsed onto the
/// CPU: the copy pass becomes box-filtered sub-cell averages, the cell pass is
/// Measure + ChooseGlyph verbatim, and the type pass is the same compositing
/// arithmetic per output pixel. When editing a pass's arithmetic in
/// Shaders.cpp, edit the matching phase here; the `//= mirrored` constants
/// themselves still have exactly one C++ home.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsProcessing.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Alphabet.h"
#include "../Controls.h"
#include "../Font.h"
#include "../Match.h"
#include "../Presets.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.asciify";
constexpr const char* kPluginName       = "Asciify";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Re-draws the picture as characters.\n\n"
	"A character cell is a small picture, and the glyph that stands in for it "
	"is the one whose ink is distributed most like that picture — how much "
	"ink, and where. Structure at 0 is exactly the classic ASCII ramp, "
	"measured off this plugin's own font rather than written down as "
	"folklore.\n\n"
	"https://stoatworks-labs.com";

constexpr const char* kParamColumns      = "columns";
constexpr const char* kParamSet          = "characterSet";
constexpr const char* kParamCustom       = "customSet";
constexpr const char* kParamStructure    = "structure";
constexpr const char* kParamTone         = "tone";
constexpr const char* kParamContrast     = "contrast";
constexpr const char* kParamInvert       = "invert";
constexpr const char* kParamDither       = "dither";
constexpr const char* kParamTint         = "tint";
constexpr const char* kParamInk          = "inkColour";
constexpr const char* kParamPaper        = "paperColour";
constexpr const char* kParamPaperOpacity = "paperOpacity";
constexpr const char* kParamEdge         = "glyphEdge";
constexpr const char* kParamMix          = "mix";
constexpr const char* kParamPreset       = "preset";

/// What the cell pass decided, one entry per character cell.
struct CellChoice
{
	int slot = 0;      //!< atlas slot of the chosen glyph
	float r = 0.0f;    //!< the cell's own straight colour, for Tint
	float g = 0.0f;
	float b = 0.0f;
	float alpha = 0.0f;//!< the picture's average alpha over the cell
};

/// Everything the type phase needs, computed once per render.
struct TypeSetup
{
	int columns = 80;
	int rows    = 45;
	std::vector<CellChoice> cells;
	const std::vector<uint8_t>* atlas = nullptr;//!< BuildAtlasImage(), cached per process

	float inkR = 0.6f, inkG = 1.0f, inkB = 0.7f;
	float paperR = 0.02f, paperG = 0.05f, paperB = 0.03f;
	float paperOpacity = 1.0f;
	float tint         = 0.0f;
	float mix          = 1.0f;
	bool smoothEdge    = false;
};

/// The atlas bitmap, built once per process. Read-only after that, so shared
/// across render threads and instances without ceremony.
const std::vector<uint8_t>& AtlasImage()
{
	static const std::vector<uint8_t>* image = new std::vector<uint8_t>( asciify::BuildAtlasImage() );
	return *image;
}

constexpr int kAtlasW = asciify::kAtlasCols * asciify::kSlotSize;
constexpr int kAtlasH = asciify::kAtlasRows * asciify::kSlotSize;

/// One atlas texel, 0..1. Out-of-range reads land in a slot's blank border by
/// construction, but clamp anyway: arithmetic at the frame edge should never
/// be able to read outside the image.
inline float atlasTexel( const std::vector<uint8_t>& atlas, int x, int y )
{
	x = std::clamp( x, 0, kAtlasW - 1 );
	y = std::clamp( y, 0, kAtlasH - 1 );
	return atlas[ size_t( y ) * kAtlasW + x ] / 255.0f;
}

/// The atlas sampled the way the GPU samples it: NEAREST for crisp glyph
/// edges, LINEAR for smooth. `tx, ty` are continuous texel coordinates.
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

	const float top    = atlasTexel( atlas, x0, y0 ) * float( 1.0 - ax ) + atlasTexel( atlas, x0 + 1, y0 ) * float( ax );
	const float bottom = atlasTexel( atlas, x0, y0 + 1 ) * float( 1.0 - ax ) + atlasTexel( atlas, x0 + 1, y0 + 1 ) * float( ax );
	return top * float( 1.0 - ay ) + bottom * float( ay );
}

class AsciifyProcessorBase : public OFX::ImageProcessor
{
public:
	explicit AsciifyProcessorBase( OFX::ImageEffect& effect ) :
		OFX::ImageProcessor( effect )
	{
	}

	void setSetup( OFX::Image* src, const TypeSetup* v, bool premultipliedValue )
	{
		srcImg        = src;
		setup         = v;
		premultiplied = premultipliedValue;
	}

protected:
	OFX::Image* srcImg     = nullptr;
	const TypeSetup* setup = nullptr;
	bool premultiplied     = false;
};

template<class PIX, int nComponents, int maxValue>
class AsciifyProcessor : public AsciifyProcessorBase
{
public:
	explicit AsciifyProcessor( OFX::ImageEffect& effect ) :
		AsciifyProcessorBase( effect )
	{
	}

	void multiThreadProcessImages( OfxRectI window ) override
	{
		const OfxRectI dstBounds = _dstImg->getBounds();
		const int outW           = dstBounds.x2 - dstBounds.x1;
		const int outH           = dstBounds.y2 - dstBounds.y1;
		const TypeSetup& t       = *setup;
		const auto& atlas        = *t.atlas;

		for( int y = window.y1; y < window.y2; ++y )
		{
			if( _effect.abort() )
				break;

			PIX* dstPix = static_cast<PIX*>( _dstImg->getPixelAddress( window.x1, y ) );

			for( int x = window.x1; x < window.x2; ++x, dstPix += nComponents )
			{
				//The type pass, per output pixel.
				const double gx = ( x - dstBounds.x1 + 0.5 ) * t.columns / double( outW );
				const double gy = ( y - dstBounds.y1 + 0.5 ) * t.rows / double( outH );

				const int cellX = std::clamp( int( gx ), 0, t.columns - 1 );
				const int cellY = std::clamp( int( gy ), 0, t.rows - 1 );
				const double localX = std::clamp( gx - cellX, 0.0, 1.0 );
				const double localY = std::clamp( gy - cellY, 0.0, 1.0 );

				const CellChoice& cell = t.cells[ size_t( cellY ) * t.columns + cellX ];

				//Slot to atlas texel: one-texel inset inside the 10x10 slot,
				//the blank border that keeps a smooth fetch on one character
				//from picking up the ink of the next.
				const int slotCol = cell.slot % asciify::kAtlasCols;
				const int slotRow = cell.slot / asciify::kAtlasCols;
				const double tx   = slotCol * asciify::kSlotSize + 1.0 + localX * 8.0;
				const double ty   = slotRow * asciify::kSlotSize + 1.0 + localY * 8.0;

				const float ink = sampleAtlas( atlas, tx, ty, t.smoothEdge );

				const float typedR = t.paperR + ( ( t.inkR + ( cell.r - t.inkR ) * t.tint ) - t.paperR ) * ink;
				const float typedG = t.paperG + ( ( t.inkG + ( cell.g - t.inkG ) * t.tint ) - t.paperG ) * ink;
				const float typedB = t.paperB + ( ( t.inkB + ( cell.b - t.inkB ) * t.tint ) - t.paperB ) * ink;
				const float typedA = ( t.paperOpacity + ( 1.0f - t.paperOpacity ) * ink ) * cell.alpha;

				//Premultiplied, and mixed premultiplied, exactly as the shader.
				double plain[ 4 ];
				readPlain( x, y, plain );

				double r = plain[ 0 ] + ( typedR * typedA - plain[ 0 ] ) * t.mix;
				double g = plain[ 1 ] + ( typedG * typedA - plain[ 1 ] ) * t.mix;
				double b = plain[ 2 ] + ( typedB * typedA - plain[ 2 ] ) * t.mix;
				double a = plain[ 3 ] + ( typedA - plain[ 3 ] ) * t.mix;

				r = std::min( r, a );
				g = std::min( g, a );
				b = std::min( b, a );

				if( !premultiplied && nComponents == 4 && a > 0.0 )
				{
					r /= a;
					g /= a;
					b /= a;
				}

				dstPix[ 0 ] = quantise( r );
				dstPix[ 1 ] = quantise( g );
				dstPix[ 2 ] = quantise( b );
				if( nComponents == 4 )
					dstPix[ 3 ] = quantise( a );
			}
		}
	}

private:
	/// The source pixel under this output pixel, premultiplied RGBA in 0..1 —
	/// the "dry" side of the Mix control.
	void readPlain( int x, int y, double out[ 4 ] ) const
	{
		const PIX* srcPix = srcImg ? static_cast<const PIX*>( srcImg->getPixelAddress( x, y ) ) : nullptr;
		if( !srcPix )
		{
			out[ 0 ] = out[ 1 ] = out[ 2 ] = out[ 3 ] = 0.0;
			return;
		}

		out[ 0 ] = srcPix[ 0 ] / double( maxValue );
		out[ 1 ] = srcPix[ 1 ] / double( maxValue );
		out[ 2 ] = srcPix[ 2 ] / double( maxValue );
		out[ 3 ] = nComponents == 4 ? srcPix[ 3 ] / double( maxValue ) : 1.0;

		if( !premultiplied && nComponents == 4 )
		{
			out[ 0 ] *= out[ 3 ];
			out[ 1 ] *= out[ 3 ];
			out[ 2 ] *= out[ 3 ];
		}
	}

	static PIX quantise( double v )
	{
		if( maxValue == 1 )
			return PIX( v );

		v = std::clamp( v, 0.0, 1.0 );
		return PIX( v * maxValue + 0.5 );
	}
};

class AsciifyPlugin : public OFX::ImageEffect
{
public:
	explicit AsciifyPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip      = fetchClip( kOfxImageEffectOutputClipName );
		srcClip      = fetchClip( kOfxImageEffectSimpleSourceClipName );
		columns      = fetchDoubleParam( kParamColumns );
		set          = fetchChoiceParam( kParamSet );
		custom       = fetchStringParam( kParamCustom );
		structure    = fetchDoubleParam( kParamStructure );
		tone         = fetchDoubleParam( kParamTone );
		contrast     = fetchDoubleParam( kParamContrast );
		invert       = fetchBooleanParam( kParamInvert );
		dither       = fetchDoubleParam( kParamDither );
		tint         = fetchDoubleParam( kParamTint );
		ink          = fetchRGBParam( kParamInk );
		paper        = fetchRGBParam( kParamPaper );
		paperOpacity = fetchDoubleParam( kParamPaperOpacity );
		edge         = fetchChoiceParam( kParamEdge );
		mix          = fetchDoubleParam( kParamMix );
		preset       = fetchChoiceParam( kParamPreset );
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;

		using namespace asciify::presets;

		if( paramName == kParamPreset )
		{
			int chosen = 0;
			preset->getValue( chosen );
			if( chosen <= 0 || chosen > kCount || applyingPreset )
				return;

			// The copy IS the preset — same table as the FFGL build, same 0..1
			// space. One edit block so undo takes the whole preset back at once.
			const Preset& p = kPresets[ chosen - 1 ];
			applyingPreset  = true;
			beginEditBlock( "Preset" );
			setDouble( columns, p.v[ kColumns ] );
			setChoice( set, p.v[ kSet ] );
			setDouble( structure, p.v[ kStructure ] );
			setDouble( tone, p.v[ kTone ] );
			setDouble( contrast, p.v[ kContrast ] );
			setBool( invert, p.v[ kInvert ] );
			setDouble( dither, p.v[ kDither ] );
			setDouble( tint, p.v[ kTint ] );
			setRGB( ink, p.v[ kInkR ], p.v[ kInkG ], p.v[ kInkB ] );
			setRGB( paper, p.v[ kPaperR ], p.v[ kPaperG ], p.v[ kPaperB ] );
			setDouble( paperOpacity, p.v[ kPaperOpacity ] );
			setChoice( edge, p.v[ kEdge ] );
			endEditBlock();
			applyingPreset = false;
			return;
		}

		// Editing a covered control while a preset is active hands control back
		// to the sliders. Judged by value, not by the change reason: hosts are
		// not consistent about reasons, but "still equal to the preset" is
		// unambiguous and also absorbs the host echoing our own setValues.
		if( applyingPreset || args.reason == OFX::eChangeTime )
			return;

		int active = 0;
		preset->getValue( active );
		if( active <= 0 || active > kCount )
			return;

		const Preset& p    = kPresets[ active - 1 ];
		const bool covered =
			( paramName == kParamColumns && doubleDiffers( columns, p.v[ kColumns ] ) ) ||
			( paramName == kParamSet && choiceDiffers( set, p.v[ kSet ] ) ) ||
			( paramName == kParamStructure && doubleDiffers( structure, p.v[ kStructure ] ) ) ||
			( paramName == kParamTone && doubleDiffers( tone, p.v[ kTone ] ) ) ||
			( paramName == kParamContrast && doubleDiffers( contrast, p.v[ kContrast ] ) ) ||
			( paramName == kParamInvert && boolDiffers( invert, p.v[ kInvert ] ) ) ||
			( paramName == kParamDither && doubleDiffers( dither, p.v[ kDither ] ) ) ||
			( paramName == kParamTint && doubleDiffers( tint, p.v[ kTint ] ) ) ||
			( paramName == kParamInk && rgbDiffers( ink, p.v[ kInkR ], p.v[ kInkG ], p.v[ kInkB ] ) ) ||
			( paramName == kParamPaper && rgbDiffers( paper, p.v[ kPaperR ], p.v[ kPaperG ], p.v[ kPaperB ] ) ) ||
			( paramName == kParamPaperOpacity && doubleDiffers( paperOpacity, p.v[ kPaperOpacity ] ) ) ||
			( paramName == kParamEdge && choiceDiffers( edge, p.v[ kEdge ] ) );

		if( covered )
		{
			applyingPreset = true;
			preset->setValue( 0 );
			applyingPreset = false;
		}
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr<OFX::Image> dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr<OFX::Image> src( srcClip->fetchImage( args.time ) );

		const bool premultiplied = srcClip->getPreMultiplication() == OFX::eImagePreMultiplied;

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();

		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );

		//--- the copy and cell passes, once per frame ------------------------
		TypeSetup setup;
		buildSetup( args.time, *src, depth, comps, premultiplied, setup );

		//--- the type pass, threaded over the output -------------------------
		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? run<AsciifyProcessor<unsigned char, 4, 255>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<AsciifyProcessor<unsigned char, 3, 255>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? run<AsciifyProcessor<unsigned short, 4, 65535>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<AsciifyProcessor<unsigned short, 3, 65535>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? run<AsciifyProcessor<float, 4, 1>>( args, dst.get(), src.get(), setup, premultiplied )
				: run<AsciifyProcessor<float, 3, 1>>( args, dst.get(), src.get(), setup, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

private:
	/// Read one source pixel as premultiplied RGBA doubles, whatever the depth.
	static void readSrc( const void* pix, OFX::BitDepthEnum depth, int nComponents, bool premultiplied,
						 double out[ 4 ] )
	{
		double r, g, b, a;
		switch( depth )
		{
		case OFX::eBitDepthUByte:
		{
			const unsigned char* p = static_cast<const unsigned char*>( pix );
			r = p[ 0 ] / 255.0;
			g = p[ 1 ] / 255.0;
			b = p[ 2 ] / 255.0;
			a = nComponents == 4 ? p[ 3 ] / 255.0 : 1.0;
			break;
		}
		case OFX::eBitDepthUShort:
		{
			const unsigned short* p = static_cast<const unsigned short*>( pix );
			r = p[ 0 ] / 65535.0;
			g = p[ 1 ] / 65535.0;
			b = p[ 2 ] / 65535.0;
			a = nComponents == 4 ? p[ 3 ] / 65535.0 : 1.0;
			break;
		}
		default:
		{
			const float* p = static_cast<const float*>( pix );
			r = p[ 0 ];
			g = p[ 1 ];
			b = p[ 2 ];
			a = nComponents == 4 ? p[ 3 ] : 1.0;
			break;
		}
		}

		if( !premultiplied && nComponents == 4 )
		{
			r *= a;
			g *= a;
			b *= a;
		}

		out[ 0 ] = r;
		out[ 1 ] = g;
		out[ 2 ] = b;
		out[ 3 ] = a;
	}

	/// The copy and cell passes: box-filter each cell's 8x8 sub-grid off the
	/// source, run the tone pipeline, measure, and choose a glyph. Single
	/// threaded; each source pixel is visited exactly once.
	void buildSetup( double t, OFX::Image& src, OFX::BitDepthEnum depth, OFX::PixelComponentEnum comps,
					 bool premultiplied, TypeSetup& setup )
	{
		using namespace asciify;

		const OfxRectI b = src.getBounds();
		const int srcW   = b.x2 - b.x1;
		const int srcH   = b.y2 - b.y1;
		const int nComp  = comps == OFX::ePixelComponentRGBA ? 4 : 3;

		//The grid. Rows follow from columns and the output aspect, so a cell
		//is as square as an integer count allows (the output and the source
		//are the same surface in OFX, unlike FFGL's viewport).
		setup.columns = ColumnsFromParam( float( columns->getValueAtTime( t ) ) );
		setup.rows    = std::max( 1, int( std::lround( double( setup.columns ) * srcH / std::max( 1, srcW ) ) ) );
		setup.columns = std::min( setup.columns, std::max( 1, srcW ) );
		setup.rows    = std::min( setup.rows, std::max( 1, srcH ) );

		//The alphabet, measured. SlotsFor falls back to ASCII when a custom
		//string contains nothing this font draws.
		int setValue = 0;
		set->getValueAtTime( t, setValue );
		std::string customText;
		custom->getValueAtTime( t, customText );

		const std::vector<int> slots = SlotsFor( Set( setValue ), customText );
		std::vector<Moments> moments;
		moments.reserve( slots.size() );
		const std::vector<Glyph>& glyphs = Glyphs();
		for( int slot : slots )
			moments.push_back( MeasureGlyph( glyphs[ size_t( slot ) ] ) );

		float coverLow = 0.0f, coverHigh = 1.0f;
		CoverageRange( moments, coverLow, coverHigh );

		const float structureValue = float( structure->getValueAtTime( t ) );
		const float allowance      = ShapeAllowance( structureValue, coverLow, coverHigh );
		const float gamma          = GammaFromParam( float( tone->getValueAtTime( t ) ) );
		const float contrastValue  = ContrastFromParam( float( contrast->getValueAtTime( t ) ) );
		const bool invertValue     = invert->getValueAtTime( t );
		const float ditherValue    = float( dither->getValueAtTime( t ) );

		setup.cells.resize( size_t( setup.columns ) * setup.rows );

		for( int cellY = 0; cellY < setup.rows; ++cellY )
		{
			//Cell bounds in source pixels. Integer edges from the exact
			//fractions so the cells tile the picture without gaps.
			const int py0 = int( double( cellY ) * srcH / setup.rows );
			const int py1 = std::max( py0 + 1, int( double( cellY + 1 ) * srcH / setup.rows ) );

			for( int cellX = 0; cellX < setup.columns; ++cellX )
			{
				const int px0 = int( double( cellX ) * srcW / setup.columns );
				const int px1 = std::max( px0 + 1, int( double( cellX + 1 ) * srcW / setup.columns ) );

				//One quantisation step of tone: the gap between neighbours in
				//the ramp.                                       //= mirrored
				const float ditherStep = ditherValue * Dither4x4( cellX, cellY )
										 / std::max( 1.0f, float( moments.size() ) - 1.0f );

				float ink[ kGlyphSize * kGlyphSize ];
				double colourSum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };

				for( int row = 0; row < kGlyphSize; ++row )
				{
					//Sub-cell pixel range. Row 0 is the bottom, matching both
					//the glyph bitmaps and OFX's bottom-up images.
					const int sy0 = py0 + int( double( row ) * ( py1 - py0 ) / kGlyphSize );
					const int sy1 = std::max( sy0 + 1, py0 + int( double( row + 1 ) * ( py1 - py0 ) / kGlyphSize ) );

					for( int col = 0; col < kGlyphSize; ++col )
					{
						const int sx0 = px0 + int( double( col ) * ( px1 - px0 ) / kGlyphSize );
						const int sx1 = std::max( sx0 + 1, px0 + int( double( col + 1 ) * ( px1 - px0 ) / kGlyphSize ) );

						double sum[ 4 ] = { 0.0, 0.0, 0.0, 0.0 };
						int count       = 0;
						for( int sy = sy0; sy < sy1; ++sy )
						{
							for( int sx = sx0; sx < sx1; ++sx )
							{
								const void* pix = src.getPixelAddress( b.x1 + std::min( sx, srcW - 1 ),
																	   b.y1 + std::min( sy, srcH - 1 ) );
								if( !pix )
									continue;
								double p[ 4 ];
								readSrc( pix, depth, nComp, premultiplied, p );
								for( int c = 0; c < 4; ++c )
									sum[ c ] += p[ c ];
								++count;
							}
						}
						if( count > 0 )
							for( int c = 0; c < 4; ++c )
								sum[ c ] /= count;

						for( int c = 0; c < 4; ++c )
							colourSum[ c ] += sum[ c ];

						//Straight colour for the luminance: a dark pixel and a
						//transparent pixel are not the same thing. //= mirrored
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

				CellChoice& out = setup.cells[ size_t( cellY ) * setup.columns + cellX ];
				out.slot        = slots[ size_t( best ) ];

				//Straight colour, weighted by alpha: a cell that is mostly
				//transparent takes its colour from the part that is not.
				const double alphaSum = std::max( colourSum[ 3 ], 1.0 / 255.0 );
				out.r               = float( colourSum[ 0 ] / alphaSum );
				out.g               = float( colourSum[ 1 ] / alphaSum );
				out.b               = float( colourSum[ 2 ] / alphaSum );
				out.alpha           = float( colourSum[ 3 ] / 64.0 );
			}
		}

		setup.atlas = &AtlasImage();

		double r = 0.0, g = 0.0, bChannel = 0.0;
		ink->getValueAtTime( t, r, g, bChannel );
		setup.inkR = float( r );
		setup.inkG = float( g );
		setup.inkB = float( bChannel );
		paper->getValueAtTime( t, r, g, bChannel );
		setup.paperR = float( r );
		setup.paperG = float( g );
		setup.paperB = float( bChannel );
		setup.paperOpacity = float( paperOpacity->getValueAtTime( t ) );
		setup.tint         = float( tint->getValueAtTime( t ) );
		setup.mix          = float( mix->getValueAtTime( t ) );

		int edgeValue = 0;
		edge->getValueAtTime( t, edgeValue );
		setup.smoothEdge = edgeValue > 0;
	}

	template<class Processor>
	void run( const OFX::RenderArguments& args, OFX::Image* dst, OFX::Image* src,
			  const TypeSetup& setup, bool premultiplied )
	{
		Processor processor( *this );
		processor.setDstImg( dst );
		processor.setSetup( src, &setup, premultiplied );
		processor.setRenderWindow( args.renderWindow );
		processor.process();
	}

	OFX::Clip* dstClip              = nullptr;
	OFX::Clip* srcClip              = nullptr;
	OFX::DoubleParam* columns       = nullptr;
	OFX::ChoiceParam* set           = nullptr;
	OFX::StringParam* custom        = nullptr;
	OFX::DoubleParam* structure     = nullptr;
	OFX::DoubleParam* tone          = nullptr;
	OFX::DoubleParam* contrast      = nullptr;
	OFX::BooleanParam* invert       = nullptr;
	OFX::DoubleParam* dither        = nullptr;
	OFX::DoubleParam* tint          = nullptr;
	OFX::RGBParam* ink              = nullptr;
	OFX::RGBParam* paper            = nullptr;
	OFX::DoubleParam* paperOpacity  = nullptr;
	OFX::ChoiceParam* edge          = nullptr;
	OFX::DoubleParam* mix           = nullptr;
	OFX::ChoiceParam* preset        = nullptr;

	// The preset table is plain floats; these give each param type its
	// reading of one. Option values are element indices, booleans are 0/1.
	static bool doubleDiffers( OFX::DoubleParam* p, float v )
	{
		double current = 0.0;
		p->getValue( current );
		return std::fabs( current - double( v ) ) > 1e-4;
	}
	static bool boolDiffers( OFX::BooleanParam* p, float v )
	{
		bool current = false;
		p->getValue( current );
		return current != ( v > 0.5f );
	}
	static bool choiceDiffers( OFX::ChoiceParam* p, float v )
	{
		int current = 0;
		p->getValue( current );
		return current != int( std::lround( v ) );
	}
	static bool rgbDiffers( OFX::RGBParam* p, float r, float g, float b )
	{
		double cr = 0.0, cg = 0.0, cb = 0.0;
		p->getValue( cr, cg, cb );
		return std::fabs( cr - double( r ) ) > 1e-4 || std::fabs( cg - double( g ) ) > 1e-4
			   || std::fabs( cb - double( b ) ) > 1e-4;
	}
	static void setDouble( OFX::DoubleParam* p, float v )
	{
		if( doubleDiffers( p, v ) )
			p->setValue( double( v ) );
	}
	static void setBool( OFX::BooleanParam* p, float v )
	{
		if( boolDiffers( p, v ) )
			p->setValue( v > 0.5f );
	}
	static void setChoice( OFX::ChoiceParam* p, float v )
	{
		if( choiceDiffers( p, v ) )
			p->setValue( int( std::lround( v ) ) );
	}
	static void setRGB( OFX::RGBParam* p, float r, float g, float b )
	{
		if( rgbDiffers( p, r, g, b ) )
			p->setValue( double( r ), double( g ), double( b ) );
	}

	/// True while our own setValues are in flight, so the resulting
	/// changedParam callbacks are not mistaken for the operator editing.
	bool applyingPreset = false;
};

OFX::DoubleParamDescriptor* defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
										  const char* name, const char* label, const char* hint, double def )
{
	OFX::DoubleParamDescriptor* p = desc.defineDoubleParam( name );
	p->setLabels( label, label, label );
	p->setHint( hint );
	p->setRange( 0.0, 1.0 );
	p->setDisplayRange( 0.0, 1.0 );
	p->setDefault( def );
	page->addChild( *p );
	return p;
}

} // namespace

mDeclarePluginFactory( AsciifyPluginFactory, {}, {} );

void AsciifyPluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// A character is chosen for a cell, never for a pixel, and a cell reads
	// the whole of its patch of picture — so no tiles. Frames stay
	// independent of each other and of render order.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
}

void AsciifyPluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );

	// Factory presets, from the same table the FFGL build reads (Presets.h).
	// Custom is not a preset: it means the sliders are the truth.
	OFX::ChoiceParamDescriptor* presetParam = desc.defineChoiceParam( kParamPreset );
	presetParam->setLabels( "Preset", "Preset", "Preset" );
	presetParam->setHint( "Named typesetting looks. Picking one sets the covered controls; "
	                      "editing any of them afterwards falls back to Custom." );
	presetParam->appendOption( "Custom" );
	for( int i = 0; i < asciify::presets::kCount; ++i )
		presetParam->appendOption( asciify::presets::kPresets[ i ].name );
	presetParam->setDefault( 0 );
	presetParam->setIsPersistant( true );
	presetParam->setEvaluateOnChange( false );//the copied values re-render; the label itself does not
	presetParam->setAnimates( false );
	page->addChild( *presetParam );

	OFX::GroupParamDescriptor* text = desc.defineGroupParam( "Text" );
	text->setLabels( "Text", "Text", "Text" );

	defineSlider( desc, page, kParamColumns, "Columns",
				  "Characters across, 8 to 320, logarithmically. The default is 80.", 0.624 )
		->setParent( *text );

	OFX::ChoiceParamDescriptor* setParam = desc.defineChoiceParam( kParamSet );
	setParam->setLabels( "Characters", "Characters", "Characters" );
	setParam->setHint( "Which characters are allowed to appear." );
	for( int i = 0; i < int( asciify::Set::Count ); ++i )
		setParam->appendOption( asciify::SetName( asciify::Set( i ) ) );
	setParam->setDefault( 0 );
	setParam->setParent( *text );
	page->addChild( *setParam );

	OFX::StringParamDescriptor* customParam = desc.defineStringParam( kParamCustom );
	customParam->setLabels( "Custom Set", "Custom Set", "Custom Set" );
	customParam->setHint( "Characters for the Custom set. Anything this font cannot draw is skipped." );
	customParam->setDefault( "@%#*+=-:. " );
	customParam->setParent( *text );
	page->addChild( *customParam );

	OFX::GroupParamDescriptor* toneGroup = desc.defineGroupParam( "ToneGroup" );
	toneGroup->setLabels( "Tone", "Tone", "Tone" );

	defineSlider( desc, page, kParamStructure, "Structure",
				  "How far the shape of a cell's ink may override its weight. "
				  "0 is exactly the classic tone ramp.",
				  0.35 )
		->setParent( *toneGroup );
	defineSlider( desc, page, kParamTone, "Tone", "Gamma on the luminance; 0.5 is 1.0.", 0.5 )
		->setParent( *toneGroup );
	defineSlider( desc, page, kParamContrast, "Contrast", "Contrast about mid grey; 0.5 is 1.0.", 0.5 )
		->setParent( *toneGroup );

	OFX::BooleanParamDescriptor* invertParam = desc.defineBooleanParam( kParamInvert );
	invertParam->setLabels( "Invert", "Invert", "Invert" );
	invertParam->setHint( "Dark ink on light paper instead of light on dark." );
	invertParam->setDefault( false );
	invertParam->setParent( *toneGroup );
	page->addChild( *invertParam );

	defineSlider( desc, page, kParamDither, "Dither",
				  "Ordered dither of one ramp step, to break banding without inventing detail.", 0.5 )
		->setParent( *toneGroup );

	OFX::GroupParamDescriptor* colour = desc.defineGroupParam( "Colour" );
	colour->setLabels( "Colour", "Colour", "Colour" );

	OFX::RGBParamDescriptor* inkParam = desc.defineRGBParam( kParamInk );
	inkParam->setLabels( "Ink", "Ink", "Ink" );
	inkParam->setHint( "The characters' colour, before Tint." );
	inkParam->setDefault( 0.60, 1.00, 0.70 );
	inkParam->setParent( *colour );
	page->addChild( *inkParam );

	defineSlider( desc, page, kParamTint, "Tint",
				  "0 draws every character in the ink colour, 1 in the picture's own colour.", 0.0 )
		->setParent( *colour );

	OFX::RGBParamDescriptor* paperParam = desc.defineRGBParam( kParamPaper );
	paperParam->setLabels( "Paper", "Paper", "Paper" );
	paperParam->setHint( "The colour behind the characters." );
	paperParam->setDefault( 0.02, 0.05, 0.03 );
	paperParam->setParent( *colour );
	page->addChild( *paperParam );

	defineSlider( desc, page, kParamPaperOpacity, "Paper Opacity",
				  "0 floats the characters on transparency, 1 is a solid page.", 1.0 )
		->setParent( *colour );

	OFX::GroupParamDescriptor* output = desc.defineGroupParam( "Output" );
	output->setLabels( "Output", "Output", "Output" );

	OFX::ChoiceParamDescriptor* edgeParam = desc.defineChoiceParam( kParamEdge );
	edgeParam->setLabels( "Glyph Edge", "Glyph Edge", "Glyph Edge" );
	edgeParam->setHint( "Crisp is hard pixels, what a character on a screen was. "
						"Smooth stops hard pixels shimmering when heavily minified." );
	edgeParam->appendOption( "Crisp" );
	edgeParam->appendOption( "Smooth" );
	edgeParam->setDefault( 0 );
	edgeParam->setParent( *output );
	page->addChild( *edgeParam );

	defineSlider( desc, page, kParamMix, "Mix", "Dry/wet with the untouched picture.", 1.0 )
		->setParent( *output );

	// The Stoatworks About block: a read-only credit line and one push button
	// per link, in a group that starts folded. Last, so it sits under the
	// effect's own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* AsciifyPluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new AsciifyPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static AsciifyPluginFactory* factory =
		new AsciifyPluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
