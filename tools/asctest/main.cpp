/**
    asctest -- render Asciify offline, and check what it typed.

    Which character a cell chose is a fact. Not "looks about right" -- a
    specific glyph, which can be read straight back out of the rendered frame
    and compared with what the C++ says it should have been. So this harness is
    not only a previewer. It builds a headless GL 4.1 core context, drives the
    real Asciify class through the real FFGL entry sequence, and offers five
    ways of looking at the result:

        asctest --out /tmp/frame.png     a picture, on a type card
        asctest --atlas /tmp/atlas.png   the font, so the glyphs can be read
        asctest --font                   the font's own invariants
        asctest --ramp                   the measured coverage ordering
        asctest --match                  did the GPU pick what the C++ predicts?

    `--match` is the important one, and the trick that makes it possible is
    picking a size where one glyph pixel is exactly one output pixel: render at
    eight output pixels per cell, in white ink on opaque black, and every cell
    of the output frame *is* an 8x8 glyph bitmap. Threshold it, look it up in
    the font, and you know which character the GPU chose -- which the CPU can
    then predict independently from the source image. A typo in either copy of
    the matching maths shows up as a cell that disagrees.

    It is a stronger check than it first looks, because that path goes through
    everything: the mip chain the cell average is taken from, the tone curve,
    the dither, the moment measurement, the nearest-neighbour search, the
    alphabet's atlas slots, and the type pass's addressing of the atlas. Any one
    of those being wrong lands a different character in the cell.

    Ties are reported separately rather than as failures. Two characters can sit
    a few millionths apart in cost -- `.` and `,` very nearly do -- and which
    one wins then comes down to the last bit of a float, which is not the same
    question as whether the two implementations agree.
*/

#include "Alphabet.h"
#include "Asciify.h"
#include "Controls.h"
#include "Font.h"
#include "Match.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace asciify;

namespace
{
constexpr double kPi = 3.14159265358979323846;

/// How close two characters' costs have to be before "the GPU chose the other
/// one" stops being evidence of a disagreement.
///
/// Derived rather than tuned until the test went green. The copy buffer is
/// half precision, so a luminance can be off by a relative 2^-11 before the
/// matching maths ever sees it. Through the coverage mapping that is about
/// 1.5e-4 of coverage, and the tone term is squared, so it moves a cost by
/// 2 * |coverage error| * 1.5e-4 -- around 6e-6 for the largest error a
/// near-tie can have. Measured on the type card: the widest real gap between
/// the two implementations is 5.4e-6, which is that number. Three times it is
/// the tolerance below.
///
/// It is worth knowing how sharp this leaves the test. A wrong sign or a
/// mistyped scale constant in either copy of the maths moves costs by 1e-3 and
/// upwards -- two orders of magnitude clear of this -- and with the copy buffer
/// forced to 32-bit float the two implementations agree on every cell of a
/// 40x24 card with no tolerance at all. So this is the width of the half-float
/// step and nothing else.
constexpr float kTieTolerance = 2.0e-5f;

/// The copy buffer is GL_RGBA16F, so every colour the cell pass reads has been
/// through half precision on the way. That is a relative error of up to 2^-11,
/// which is invisible in the picture -- it is an eighth of an 8-bit level -- but
/// it is a hundred times larger than float noise, and predicting from the
/// original bytes instead puts a handful of cells on the wrong side of a close
/// decision. Rounding here is not a fudge to make the test pass: it is the
/// pipeline, and leaving it out was measurably predicting a different pipeline
/// from the one that ran.
///
/// The plugin keeps half precision on purpose. Thirty-two bits would double a
/// 4K copy buffer and its mip chain for a difference nobody can see.
float toHalfAndBack( float value )
{
	return static_cast< float >( static_cast< __fp16 >( value ) );
}

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The type card.
//
// Not meant to look nice. It is three bands, each of which makes a different
// kind of wrong answer visible:
//
//   left    a smooth horizontal ramp, so the whole alphabet is used in order.
//           Banding, a reversed ramp or an unused end of the set all show here.
//   middle  bars at eight angles. This is what Structure is judged on: at zero
//           they come out as blocks of even weight, and as it is wound up they
//           should pick up characters that lean the right way.
//   right   concentric rings, which have every edge direction at once and are
//           where a sign error in one moment shows up as a quadrant that reads
//           differently from its neighbour.
//
// Written top-down; GL wants bottom-up, so the caller flips.
//---------------------------------------------------------------------------
void setPixel( std::vector< unsigned char >& image, int width, int height, int x, int y,
               unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255 )
{
	if( x < 0 || y < 0 || x >= width || y >= height )
		return;
	const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ at + 0 ] = r;
	image[ at + 1 ] = g;
	image[ at + 2 ] = b;
	image[ at + 3 ] = a;
}

std::vector< unsigned char > buildTypeCard( int width, int height )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4, 0 );

	const int bandA = width / 3;
	const int bandB = ( width * 2 ) / 3;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			float value = 0.0f;

			if( x < bandA )
			{
				//A ramp with a few hard steps cut into it, so that both the
				//smooth behaviour and the quantisation are visible at once.
				const float t = static_cast< float >( x ) / static_cast< float >( std::max( 1, bandA - 1 ) );
				value = ( y % 64 ) < 48 ? t : std::floor( t * 8.0f ) / 7.0f;
			}
			else if( x < bandB )
			{
				//Eight angles, one per horizontal slab.
				const int slab      = ( y * 8 ) / height;
				const double angle  = kPi * static_cast< double >( slab ) / 8.0;
				const double nx     = std::cos( angle );
				const double ny     = std::sin( angle );
				const double across = ( x - bandA ) * nx + y * ny;
				value = ( static_cast< int >( std::floor( across / 9.0 ) ) & 1 ) ? 0.92f : 0.06f;
			}
			else
			{
				const double cx = ( bandB + width ) * 0.5;
				const double cy = height * 0.5;
				const double dx = x - cx;
				const double dy = y - cy;
				const double r  = std::sqrt( dx * dx + dy * dy );
				value = static_cast< float >( 0.5 + 0.5 * std::sin( r * 0.16 ) );
			}

			const unsigned char level = static_cast< unsigned char >( std::lround( std::min( 1.0f, std::max( 0.0f, value ) ) * 255.0f ) );

			//Tinted rather than grey, so that Tint has something to show and so
			//that a luminance weighting error is visible as a band changing
			//weight relative to its neighbours.
			if( x < bandA )
				setPixel( image, width, height, x, y, level, static_cast< unsigned char >( level * 0.85f ), static_cast< unsigned char >( level * 0.6f ) );
			else if( x < bandB )
				setPixel( image, width, height, x, y, static_cast< unsigned char >( level * 0.7f ), level, static_cast< unsigned char >( level * 0.9f ) );
			else
				setPixel( image, width, height, x, y, static_cast< unsigned char >( level * 0.75f ), static_cast< unsigned char >( level * 0.8f ), level );
		}
	}

	return image;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without a
	//GPU, where it will at least prove the shaders compile.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

bool runPass( Asciify& plugin, GLuint source, GLuint targetFBO, int width, int height, int frames )
{
	FFGLTextureStruct inputStruct = {};
	inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
	inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
	inputStruct.Handle                              = source;
	FFGLTextureStruct* inputs[ 1 ]                  = { &inputStruct };

	ProcessOpenGLStruct process = {};
	process.numInputTextures    = 1;
	process.inputTextures       = inputs;
	process.HostFBO             = targetFBO;

	for( int frame = 0; frame < frames; ++frame )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, targetFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			return false;
	}

	return true;
}

std::vector< unsigned char > flipRows( const std::vector< unsigned char >& image, int width, int height )
{
	std::vector< unsigned char > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

/// Straight out of GL, bottom row first. --match wants it this way up, because
/// that is the way the glyph bitmaps are stored and comparing them is the whole
/// point of that mode.
std::vector< unsigned char > readBackRaw( GLuint fbo, int width, int height )
{
	std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
	return pixels;
}

//---------------------------------------------------------------------------
// Parameter automation for --pipe.
//
// A plain text file of `frame  Parameter Name  value` lines. Values are held
// before the first key and after the last, and linearly interpolated between.
// Identical to porthole's and old-cathode's, on purpose: three harnesses that
// read one file format can share a build script.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;//blank or comment

		//The name is everything up to the last token, because parameters have
		//spaces in them and the value never does.
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

bool readExactly( void* into, size_t bytes )
{
	unsigned char* p = static_cast< unsigned char* >( into );
	size_t got       = 0;
	while( got < bytes )
	{
		const size_t n = fread( p + got, 1, bytes - got, stdin );
		if( n == 0 )
			return false;//clean EOF, or a short final frame we cannot use
		got += n;
	}
	return true;
}

//---------------------------------------------------------------------------
// Font-side helpers.
//---------------------------------------------------------------------------
std::string encodeUtf8( uint32_t codepoint )
{
	std::string out;
	if( codepoint < 0x80 )
	{
		out += static_cast< char >( codepoint );
	}
	else if( codepoint < 0x800 )
	{
		out += static_cast< char >( 0xC0 | ( codepoint >> 6 ) );
		out += static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
	}
	else
	{
		out += static_cast< char >( 0xE0 | ( codepoint >> 12 ) );
		out += static_cast< char >( 0x80 | ( ( codepoint >> 6 ) & 0x3F ) );
		out += static_cast< char >( 0x80 | ( codepoint & 0x3F ) );
	}
	return out;
}

/// The alphabet, measured exactly as Asciify::RebuildAlphabet does it. Two
/// copies of this would be a liability; it is short enough that keeping them
/// honest is a matter of reading them side by side, and --match would fail
/// loudly if they diverged.
struct Alphabet
{
	std::vector< int > slots;
	std::vector< Moments > moments;
	float coverLow  = 0.0f;
	float coverHigh = 1.0f;
};

Alphabet buildAlphabet( Set set, const std::string& custom )
{
	Alphabet built;
	built.slots = SlotsFor( set, custom );

	const std::vector< Glyph >& glyphs = Glyphs();
	for( int slot : built.slots )
		built.moments.push_back( MeasureGlyph( glyphs[ static_cast< size_t >( slot ) ] ) );

	CoverageRange( built.moments, built.coverLow, built.coverHigh );
	return built;
}

void usage()
{
	std::printf(
		"asctest -- render and check the Asciify character renderer\n"
		"\n"
		"  --out PATH        where to write (default /tmp/asciify.png)\n"
		"  --width N         output width (default 1280)\n"
		"  --height N        output height (default 720)\n"
		"  --set \"Name=V\"    set a parameter by its display name, 0..1\n"
		"  --custom STRING   the Custom Set text, which --set cannot reach\n"
		"  --list            print every parameter and its default, then exit\n"
		"  --atlas PATH      write the font atlas as a picture and exit\n"
		"  --font            check the font's invariants and exit\n"
		"  --ramp            print the active alphabet ordered by measured ink\n"
		"  --match           render at one output pixel per glyph pixel, read the\n"
		"                    characters back out, and compare them with what the\n"
		"                    C++ predicts. This is the real test.\n"
		"  --columns N       columns for --match (default 40)\n"
		"  --rows N          rows for --match (default 24)\n"
		"  --pipe            read raw RGBA frames from stdin, write them to stdout,\n"
		"                    so real footage can be put through the real shaders:\n"
		"                        ffmpeg ... -f rawvideo -pix_fmt rgba - \\\n"
		"                          | asctest --pipe --width 1920 --height 1080 \\\n"
		"                          | ffmpeg -f rawvideo -pix_fmt rgba ...\n"
		"  --script PATH     parameter automation for --pipe (see loadScript)\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outputPath = "/tmp/asciify.png";
	std::string atlasPath;
	std::string scriptPath;
	std::string customSet;
	bool haveCustomSet = false;
	int width       = 1280;
	int height      = 720;
	int matchCols   = 40;
	int matchRows   = 24;
	bool listOnly   = false;
	bool fontOnly   = false;
	bool rampOnly   = false;
	bool matchMode  = false;
	bool pipeMode   = false;
	bool keepAlpha  = false;
	std::vector< std::pair< std::string, float > > assignments;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		auto next             = [ & ]( const char* what ) -> std::string {
            if( i + 1 >= argc )
            {
                std::fprintf( stderr, "asctest: %s wants a value\n", what );
                std::exit( 2 );
            }
            return argv[ ++i ];
		};

		if( arg == "--out" )
			outputPath = next( "--out" );
		else if( arg == "--atlas" )
			atlasPath = next( "--atlas" );
		else if( arg == "--script" )
			scriptPath = next( "--script" );
		else if( arg == "--custom" )
		{
			//The Characters set can be driven by --set, but the string it reads
			//in Custom mode is a text parameter and cannot. Without this the
			//sweep would have to skip it and call it untestable.
			customSet     = next( "--custom" );
			haveCustomSet = true;
		}
		else if( arg == "--width" )
			width = std::atoi( next( "--width" ).c_str() );
		else if( arg == "--height" )
			height = std::atoi( next( "--height" ).c_str() );
		else if( arg == "--columns" )
			matchCols = std::atoi( next( "--columns" ).c_str() );
		else if( arg == "--rows" )
			matchRows = std::atoi( next( "--rows" ).c_str() );
		else if( arg == "--list" )
			listOnly = true;
		else if( arg == "--font" )
			fontOnly = true;
		else if( arg == "--ramp" )
			rampOnly = true;
		else if( arg == "--match" )
			matchMode = true;
		else if( arg == "--pipe" )
			pipeMode = true;
		else if( arg == "--alpha" )
			keepAlpha = true;
		else if( arg == "--set" )
		{
			const std::string assignment = next( "--set" );
			const size_t equals          = assignment.find( '=' );
			if( equals == std::string::npos )
			{
				std::fprintf( stderr, "asctest: --set wants Name=Value, got '%s'\n", assignment.c_str() );
				return 2;
			}
			assignments.emplace_back( assignment.substr( 0, equals ),
			                          std::strtof( assignment.c_str() + equals + 1, nullptr ) );
		}
		else if( arg == "--help" || arg == "-h" )
		{
			usage();
			return 0;
		}
		else
		{
			std::fprintf( stderr, "asctest: unknown argument '%s'\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	//-----------------------------------------------------------------------
	// The two modes that never touch the GPU. Both are about the font itself,
	// and a font that is wrong makes every other measurement meaningless, so
	// they are worth being able to run on their own.
	//-----------------------------------------------------------------------
	if( fontOnly )
	{
		const std::vector< Glyph >& glyphs = Glyphs();
		size_t declared                    = 0;
		FontArt( declared );

		int problems = 0;

		std::printf( "font: %zu glyphs drawn, %d slots in the atlas\n",
		             glyphs.size(), kAtlasCols * kAtlasRows );

		if( declared > glyphs.size() )
		{
			std::printf( "  *** %zu glyphs declared but only %zu fit in the atlas\n", declared, glyphs.size() );
			++problems;
		}

		std::set< uint32_t > seen;
		for( const Glyph& glyph : glyphs )
		{
			if( !seen.insert( glyph.codepoint ).second )
			{
				std::printf( "  *** U+%04X is drawn twice\n", glyph.codepoint );
				++problems;
			}
		}

		//Two identical bitmaps would make --match unable to say which character
		//was chosen, so this is a precondition of the real test rather than a
		//tidiness check.
		for( size_t a = 0; a < glyphs.size(); ++a )
		{
			for( size_t b = a + 1; b < glyphs.size(); ++b )
			{
				if( std::memcmp( glyphs[ a ].bits, glyphs[ b ].bits, sizeof( glyphs[ a ].bits ) ) == 0 )
				{
					std::printf( "  *** U+%04X and U+%04X are drawn identically\n",
					             glyphs[ a ].codepoint, glyphs[ b ].codepoint );
					++problems;
				}
			}
		}

		std::printf( problems == 0 ? "  all checks passed\n" : "  %d problem(s)\n", problems );
		return problems == 0 ? 0 : 1;
	}

	if( !atlasPath.empty() )
	{
		const std::vector< uint8_t > atlas = BuildAtlasImage();
		const int atlasWidth               = kAtlasCols * kSlotSize;
		const int atlasHeight              = kAtlasRows * kSlotSize;

		std::vector< unsigned char > rgba( atlas.size() * 4 );
		for( size_t i = 0; i < atlas.size(); ++i )
		{
			rgba[ i * 4 + 0 ] = atlas[ i ];
			rgba[ i * 4 + 1 ] = atlas[ i ];
			rgba[ i * 4 + 2 ] = atlas[ i ];
			rgba[ i * 4 + 3 ] = 255;
		}

		//The atlas is stored bottom-up for GL; a PNG is read top-down.
		rgba = flipRows( rgba, atlasWidth, atlasHeight );

		if( !writePng( atlasPath, atlasWidth, atlasHeight, rgba ) )
		{
			std::fprintf( stderr, "asctest: could not write %s\n", atlasPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s (%dx%d, %zu glyphs)\n", atlasPath.c_str(), atlasWidth, atlasHeight, Glyphs().size() );
		return 0;
	}

	Asciify plugin;

	//Names come from the plugin's own declaration rather than from a table
	//here, so a parameter that is renamed or reordered cannot leave the harness
	//quietly setting the wrong one.
	auto indexOfParameter = [ & ]( const std::string& name ) -> int {
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* declared = plugin.GetParamName( i );
			if( declared != nullptr && name == declared )
				return static_cast< int >( i );
		}
		return -1;
	};
	auto setParameter = [ & ]( const std::string& name, float value ) -> bool {
		const int index = indexOfParameter( name );
		if( index < 0 )
			return false;
		plugin.SetFloatParameter( static_cast< unsigned int >( index ), value );
		return true;
	};
	auto getParameter = [ & ]( const std::string& name ) -> float {
		const int index = indexOfParameter( name );
		return index < 0 ? 0.0f : plugin.GetFloatParameter( static_cast< unsigned int >( index ) );
	};

	if( listOnly )
	{
		for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
			std::printf( "%2u  %-16s %.3f\n", i, plugin.GetParamName( i ), plugin.GetFloatParameter( i ) );
		return 0;
	}

	for( const auto& assignment : assignments )
	{
		if( !setParameter( assignment.first, assignment.second ) )
		{
			std::fprintf( stderr, "asctest: no parameter called '%s'\n", assignment.first.c_str() );
			return 2;
		}
	}

	if( haveCustomSet )
		plugin.SetTextParameter( Asciify::PT_CUSTOM, customSet.c_str() );

	const Set activeSet = static_cast< Set >( static_cast< int >( getParameter( "Characters" ) + 0.5f ) );
	const std::string customText = plugin.GetTextParameter( Asciify::PT_CUSTOM ) != nullptr
	                                   ? plugin.GetTextParameter( Asciify::PT_CUSTOM )
	                                   : std::string();

	if( rampOnly )
	{
		const Alphabet alphabet            = buildAlphabet( activeSet, customText );
		const std::vector< Glyph >& glyphs = Glyphs();

		std::vector< size_t > order( alphabet.slots.size() );
		for( size_t i = 0; i < order.size(); ++i )
			order[ i ] = i;
		std::sort( order.begin(), order.end(), [ & ]( size_t a, size_t b ) {
			return alphabet.moments[ a ].coverage < alphabet.moments[ b ].coverage;
		} );

		std::printf( "%s: %zu characters, coverage %.4f .. %.4f\n",
		             SetName( activeSet ), alphabet.slots.size(), alphabet.coverLow, alphabet.coverHigh );
		std::printf( "  measured ink, lightest first:\n  " );
		for( size_t i : order )
			std::printf( "%s", encodeUtf8( glyphs[ static_cast< size_t >( alphabet.slots[ i ] ) ].codepoint ).c_str() );
		std::printf( "\n\n" );

		for( size_t i : order )
		{
			const Glyph& glyph  = glyphs[ static_cast< size_t >( alphabet.slots[ i ] ) ];
			const Moments& m    = alphabet.moments[ i ];
			std::printf( "  U+%04X %-3s  ink %.4f   shape % .3f % .3f % .3f % .3f % .3f\n",
			             glyph.codepoint, encodeUtf8( glyph.codepoint ).c_str(), m.coverage,
			             m.shape[ 0 ], m.shape[ 1 ], m.shape[ 2 ], m.shape[ 3 ], m.shape[ 4 ] );
		}
		return 0;
	}

	//-----------------------------------------------------------------------
	// Everything from here needs a context.
	//-----------------------------------------------------------------------
	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "asctest: could not create a GL 4.1 core context\n" );
		return 1;
	}

	FFGLViewportStruct viewport = {};
	viewport.x                  = 0;
	viewport.y                  = 0;

	//-----------------------------------------------------------------------
	// --match: one output pixel per glyph pixel, so the frame is a grid of
	// glyph bitmaps that can be read straight back.
	//-----------------------------------------------------------------------
	if( matchMode )
	{
		matchCols = std::max( 1, matchCols );
		matchRows = std::max( 1, matchRows );
		width     = matchCols * kGlyphSize;
		height    = matchRows * kGlyphSize;

		//Columns is declared 0..1 and mapped logarithmically, so ask for the
		//slider position that lands on exactly this many columns. If the
		//mapping cannot hit it, say so rather than measuring a different grid
		//from the one being predicted.
		float columnsParam = 0.0f;
		bool found         = false;
		for( int step = 0; step <= 2000; ++step )
		{
			const float t = static_cast< float >( step ) / 2000.0f;
			if( ColumnsFromParam( t ) == matchCols )
			{
				columnsParam = t;
				found        = true;
				break;
			}
		}
		if( !found )
		{
			std::fprintf( stderr, "asctest: --columns %d is not reachable on the Columns slider\n", matchCols );
			return 2;
		}

		//White ink on opaque black, crisp edges, fully wet. Anything else and
		//the readback is not a bitmap.
		setParameter( "Columns", columnsParam );
		setParameter( "Ink", 1.0f );
		setParameter( "Ink_Green", 1.0f );
		setParameter( "Ink_Blue", 1.0f );
		setParameter( "Paper", 0.0f );
		setParameter( "Paper_Green", 0.0f );
		setParameter( "Paper_Blue", 0.0f );
		setParameter( "Paper Opacity", 1.0f );
		setParameter( "Tint", 0.0f );
		setParameter( "Glyph Edge", 0.0f );
		setParameter( "Mix", 1.0f );

		const float structure = getParameter( "Structure" );
		const float dither    = getParameter( "Dither" );
		const float gamma     = GammaFromParam( getParameter( "Tone" ) );
		const float contrast  = ContrastFromParam( getParameter( "Contrast" ) );
		const float invert    = getParameter( "Invert" );

		viewport.width  = static_cast< FFUInt32 >( width );
		viewport.height = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "asctest: InitGL failed -- see the diagnostics log\n" );
			return 1;
		}

		//The source is built top-down and uploaded bottom-up, so that the
		//readback, the glyph bitmaps and this all agree on which way is up.
		const std::vector< unsigned char > cardTopDown = buildTypeCard( width, height );
		const std::vector< unsigned char > card        = flipRows( cardTopDown, width, height );

		GLuint source = makeTexture( width, height, card.data() );
		GLuint target = makeTexture( width, height, nullptr );
		GLuint fbo    = makeFramebuffer( target );

		if( !runPass( plugin, source, fbo, width, height, 1 ) )
		{
			std::fprintf( stderr, "asctest: ProcessOpenGL failed\n" );
			return 1;
		}

		const std::vector< unsigned char > rendered = readBackRaw( fbo, width, height );

		const Alphabet alphabet            = buildAlphabet( activeSet, customText );
		const std::vector< Glyph >& glyphs = Glyphs();
		const float allowance              = ShapeAllowance( structure, alphabet.coverLow, alphabet.coverHigh );

		//Bitmap -> atlas slot, so a rendered cell can be named.
		std::map< std::string, int > bySignature;
		for( size_t slot = 0; slot < glyphs.size(); ++slot )
			bySignature[ std::string( reinterpret_cast< const char* >( glyphs[ slot ].bits ), kGlyphSize ) ] =
				static_cast< int >( slot );

		int agreed      = 0;
		int tied        = 0;
		int disagreed   = 0;
		int unreadable  = 0;
		int reported    = 0;
		float widestTie = 0.0f;

		for( int cellY = 0; cellY < matchRows; ++cellY )
		{
			for( int cellX = 0; cellX < matchCols; ++cellX )
			{
				//--- what the GPU drew ---------------------------------------
				uint8_t drawn[ kGlyphSize ] = { 0 };
				for( int row = 0; row < kGlyphSize; ++row )
				{
					for( int col = 0; col < kGlyphSize; ++col )
					{
						const int x     = cellX * kGlyphSize + col;
						const int y     = cellY * kGlyphSize + row;
						const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;
						if( rendered[ at ] >= 128 )
							drawn[ row ] |= static_cast< uint8_t >( 1u << col );
					}
				}

				auto foundGlyph = bySignature.find( std::string( reinterpret_cast< const char* >( drawn ), kGlyphSize ) );
				if( foundGlyph == bySignature.end() )
				{
					++unreadable;
					continue;
				}
				const int drawnSlot = foundGlyph->second;

				//--- what the C++ predicts -----------------------------------
				float ink[ kGlyphSize * kGlyphSize ];
				const float ditherStep = dither * Dither4x4( cellX, cellY )
				                         / std::max( 1.0f, static_cast< float >( alphabet.moments.size() ) - 1.0f );

				for( int row = 0; row < kGlyphSize; ++row )
				{
					for( int col = 0; col < kGlyphSize; ++col )
					{
						const int x     = cellX * kGlyphSize + col;
						const int y     = cellY * kGlyphSize + row;
						const size_t at = ( static_cast< size_t >( y ) * width + x ) * 4;

						const float r = toHalfAndBack( card[ at + 0 ] / 255.0f );
						const float g = toHalfAndBack( card[ at + 1 ] / 255.0f );
						const float b = toHalfAndBack( card[ at + 2 ] / 255.0f );
						const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;

						float tone = invert < 0.5f ? luma : 1.0f - luma;
						tone = std::min( 1.0f, std::max( 0.0f, ( tone - 0.5f ) * contrast + 0.5f ) );
						tone = std::pow( tone, gamma );
						tone = std::min( 1.0f, std::max( 0.0f, tone + ditherStep ) );

						ink[ row * kGlyphSize + col ] = alphabet.coverLow + ( alphabet.coverHigh - alphabet.coverLow ) * tone;
					}
				}

				const Moments cell    = Measure( ink );
				const int predicted   = ChooseGlyph( alphabet.moments, cell, allowance );
				const int predictedSlot = alphabet.slots[ static_cast< size_t >( predicted ) ];

				if( predictedSlot == drawnSlot )
				{
					++agreed;
					continue;
				}

				//A different character is not automatically a disagreement. If
				//the one the GPU drew costs the same as the one predicted, to
				//within what a float can express, then the two implementations
				//agree and the tie was broken by rounding.
				int drawnIndex = -1;
				for( size_t i = 0; i < alphabet.slots.size(); ++i )
				{
					if( alphabet.slots[ i ] == drawnSlot )
					{
						drawnIndex = static_cast< int >( i );
						break;
					}
				}

				float gap = 1.0f;
				if( drawnIndex >= 0 )
				{
					const float costPredicted = MatchCost( alphabet.moments[ static_cast< size_t >( predicted ) ], cell, allowance );
					const float costDrawn     = MatchCost( alphabet.moments[ static_cast< size_t >( drawnIndex ) ], cell, allowance );
					gap                       = std::fabs( costDrawn - costPredicted );
					if( gap <= kTieTolerance )
					{
						++tied;
						if( gap > widestTie )
							widestTie = gap;
						continue;
					}
				}

				++disagreed;
				if( reported < 12 )
				{
					++reported;
					std::printf( "  cell %3d,%-3d  drew '%s' (U+%04X), predicted '%s' (U+%04X), cost gap %.3e\n",
					             cellX, cellY,
					             encodeUtf8( glyphs[ static_cast< size_t >( drawnSlot ) ].codepoint ).c_str(),
					             glyphs[ static_cast< size_t >( drawnSlot ) ].codepoint,
					             encodeUtf8( glyphs[ static_cast< size_t >( predictedSlot ) ].codepoint ).c_str(),
					             glyphs[ static_cast< size_t >( predictedSlot ) ].codepoint,
					             static_cast< double >( gap ) );
				}
			}
		}

		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &target );
		glDeleteTextures( 1, &source );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );

		const int total = matchCols * matchRows;
		std::printf( "match: %d cells, %d agreed, %d tied, %d disagreed, %d unreadable\n",
		             total, agreed, tied, disagreed, unreadable );
		if( tied > 0 )
			std::printf( "  widest accepted tie %.3e, tolerance %.3e\n",
			             static_cast< double >( widestTie ), static_cast< double >( kTieTolerance ) );

		//Unreadable cells mean the readback is not a bitmap at all -- the
		//colours, the edge filter or the cell size are wrong -- which is a
		//failure of the harness's own assumptions and worth a distinct exit
		//code from a genuine mismatch.
		if( unreadable > 0 )
			return 3;
		return disagreed == 0 ? 0 : 1;
	}

	//-----------------------------------------------------------------------
	// Render: a picture, or a stream of them.
	//-----------------------------------------------------------------------
	viewport.width  = static_cast< FFUInt32 >( width );
	viewport.height = static_cast< FFUInt32 >( height );
	if( plugin.InitGL( &viewport ) != FF_SUCCESS )
	{
		std::fprintf( stderr, "asctest: InitGL failed -- see the diagnostics log\n" );
		return 1;
	}

	GLuint target = makeTexture( width, height, nullptr );
	GLuint fbo    = makeFramebuffer( target );

	if( pipeMode )
	{
		std::map< std::string, Track > tracks;
		if( !scriptPath.empty() )
		{
			std::string error;
			tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "asctest: %s\n", error.c_str() );
				return 2;
			}
		}

		const size_t frameBytes = static_cast< size_t >( width ) * height * 4;
		std::vector< unsigned char > frame( frameBytes );
		GLuint source = makeTexture( width, height, nullptr );

		int frameNumber = 0;
		while( readExactly( frame.data(), frameBytes ) )
		{
			for( const auto& track : tracks )
				setParameter( track.first, valueAt( track.second, frameNumber ) );

			//ffmpeg hands over top-down rows; GL wants bottom-up.
			const std::vector< unsigned char > flipped = flipRows( frame, width, height );
			glBindTexture( GL_TEXTURE_2D, source );
			glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
			glBindTexture( GL_TEXTURE_2D, 0 );

			if( !runPass( plugin, source, fbo, width, height, 1 ) )
			{
				std::fprintf( stderr, "asctest: ProcessOpenGL failed on frame %d\n", frameNumber );
				return 1;
			}

			std::vector< unsigned char > out = flipRows( readBackRaw( fbo, width, height ), width, height );
			if( fwrite( out.data(), 1, out.size(), stdout ) != out.size() )
				break;

			++frameNumber;
		}

		glDeleteTextures( 1, &source );
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &fbo );
		glDeleteTextures( 1, &target );
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		std::fprintf( stderr, "asctest: %d frames\n", frameNumber );
		return 0;
	}

	const std::vector< unsigned char > cardTopDown = buildTypeCard( width, height );
	const std::vector< unsigned char > card        = flipRows( cardTopDown, width, height );
	GLuint source                                  = makeTexture( width, height, card.data() );

	if( !runPass( plugin, source, fbo, width, height, 1 ) )
	{
		std::fprintf( stderr, "asctest: ProcessOpenGL failed\n" );
		return 1;
	}

	std::vector< unsigned char > pixels = flipRows( readBackRaw( fbo, width, height ), width, height );

	if( !keepAlpha )
	{
		//Composite onto black so the PNG shows what the operator sees over a
		//black composition rather than a checkerboard of nothing.
		for( size_t i = 0; i < pixels.size(); i += 4 )
			pixels[ i + 3 ] = 255;
	}

	if( !writePng( outputPath, width, height, pixels ) )
	{
		std::fprintf( stderr, "asctest: could not write %s\n", outputPath.c_str() );
		return 1;
	}

	std::printf( "wrote %s (%dx%d, %d columns)\n", outputPath.c_str(), width, height,
	             ColumnsFromParam( getParameter( "Columns" ) ) );

	plugin.DeInitGL();
	glDeleteFramebuffers( 1, &fbo );
	glDeleteTextures( 1, &target );
	glDeleteTextures( 1, &source );
	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return 0;
}
