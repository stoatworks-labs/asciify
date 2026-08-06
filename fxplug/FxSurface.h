#pragma once

/// Reading and writing FxPlug image surfaces, with no plugin in it.
///
/// FxPlug hands every plugin the same small set of IOSurface pixel layouts, so
/// this half of the render is identical in all of them. It is vendored into each
/// plugin repo rather than shared from one place, the same way the fleet's other
/// cross-repo helpers are - but it is the SAME FILE each time. Fix a bug here
/// and it has to be copied out to the others; there is no build-time link
/// between the copies to catch drift.
///
/// Nothing here knows what effect is being applied. The per-plugin maths lives
/// beside it in <Name>Tile.h.

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace fxsurface
{

/// The pixel layouts FxPlug hosts actually hand us. Anything else is declined
/// rather than rendered as garbage.
enum class Layout
{
	BGRA8,
	RGBA8,
	RGBAh,
	RGBAf,
	Unsupported
};

/// Half-float decode/encode, for the FLOAT16 surfaces a wide-gamut timeline
/// produces. Written out rather than pulled from a library because the XPC
/// service should link as little as possible.
inline float halfToFloat( uint16_t h )
{
	const uint32_t sign     = uint32_t( h >> 15 ) << 31;
	const uint32_t exponent = ( h >> 10 ) & 0x1F;
	const uint32_t mantissa = h & 0x3FF;

	uint32_t bits;
	if( exponent == 0 )
	{
		if( mantissa == 0 )
			bits = sign;// +/- zero
		else
		{
			// Subnormal: renormalise into a float32 normal.
			int e      = -1;
			uint32_t m = mantissa;
			do {
				++e;
				m <<= 1;
			} while( ( m & 0x400 ) == 0 );
			bits = sign | ( uint32_t( 127 - 15 - e ) << 23 ) | ( ( m & 0x3FF ) << 13 );
		}
	}
	else if( exponent == 0x1F )
		bits = sign | 0x7F800000u | ( mantissa << 13 );// Inf / NaN
	else
		bits = sign | ( ( exponent + ( 127 - 15 ) ) << 23 ) | ( mantissa << 13 );

	float out;
	__builtin_memcpy( &out, &bits, sizeof( out ) );
	return out;
}

inline uint16_t floatToHalf( float f )
{
	uint32_t bits;
	__builtin_memcpy( &bits, &f, sizeof( bits ) );

	const uint32_t sign = ( bits >> 16 ) & 0x8000;
	int32_t exponent    = int32_t( ( bits >> 23 ) & 0xFF ) - 127 + 15;
	uint32_t mantissa   = bits & 0x7FFFFF;

	if( exponent >= 0x1F )
		return uint16_t( sign | 0x7C00 );// overflow to infinity
	if( exponent <= 0 )
	{
		if( exponent < -10 )
			return uint16_t( sign );// underflow to zero
		mantissa |= 0x800000;
		const uint32_t shift = uint32_t( 14 - exponent );
		return uint16_t( sign | ( mantissa >> shift ) );
	}
	return uint16_t( sign | ( uint32_t( exponent ) << 10 ) | ( mantissa >> 13 ) );
}

inline size_t bytesPerPixel( Layout layout )
{
	switch( layout )
	{
	case Layout::BGRA8:
	case Layout::RGBA8: return 4;
	case Layout::RGBAh: return 8;
	case Layout::RGBAf: return 16;
	default:            return 0;
	}
}

/// Read one pixel as float RGBA, in R,G,B,A order whatever the layout.
inline void readPixel( const uint8_t* p, Layout layout, float rgba[ 4 ] )
{
	switch( layout )
	{
	case Layout::BGRA8:
		rgba[ 0 ] = p[ 2 ] / 255.0f;
		rgba[ 1 ] = p[ 1 ] / 255.0f;
		rgba[ 2 ] = p[ 0 ] / 255.0f;
		rgba[ 3 ] = p[ 3 ] / 255.0f;
		break;
	case Layout::RGBA8:
		rgba[ 0 ] = p[ 0 ] / 255.0f;
		rgba[ 1 ] = p[ 1 ] / 255.0f;
		rgba[ 2 ] = p[ 2 ] / 255.0f;
		rgba[ 3 ] = p[ 3 ] / 255.0f;
		break;
	case Layout::RGBAh:
	{
		const uint16_t* h = reinterpret_cast<const uint16_t*>( p );
		for( int c = 0; c < 4; ++c )
			rgba[ c ] = halfToFloat( h[ c ] );
		break;
	}
	case Layout::RGBAf:
	{
		const float* f = reinterpret_cast<const float*>( p );
		for( int c = 0; c < 4; ++c )
			rgba[ c ] = f[ c ];
		break;
	}
	default:
		rgba[ 0 ] = rgba[ 1 ] = rgba[ 2 ] = rgba[ 3 ] = 0.0f;
		break;
	}
}

inline uint8_t quantise8( float v )
{
	v = std::min( std::max( v, 0.0f ), 1.0f );
	return uint8_t( v * 255.0f + 0.5f );
}

inline void writePixel( uint8_t* p, Layout layout, const float rgba[ 4 ] )
{
	switch( layout )
	{
	case Layout::BGRA8:
		p[ 2 ] = quantise8( rgba[ 0 ] );
		p[ 1 ] = quantise8( rgba[ 1 ] );
		p[ 0 ] = quantise8( rgba[ 2 ] );
		p[ 3 ] = quantise8( rgba[ 3 ] );
		break;
	case Layout::RGBA8:
		for( int c = 0; c < 4; ++c )
			p[ c ] = quantise8( rgba[ c ] );
		break;
	case Layout::RGBAh:
	{
		uint16_t* h = reinterpret_cast<uint16_t*>( p );
		for( int c = 0; c < 4; ++c )
			h[ c ] = floatToHalf( rgba[ c ] );
		break;
	}
	case Layout::RGBAf:
	{
		// Float pipeline: pass through unclamped, as the OpenFX build does.
		float* f = reinterpret_cast<float*>( p );
		for( int c = 0; c < 4; ++c )
			f[ c ] = rgba[ c ];
		break;
	}
	default:
		break;
	}
}
} // namespace fxsurface
