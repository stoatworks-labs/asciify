#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace asciify
{
namespace
{
constexpr int kMinColumns  = 8;
constexpr int kMaxColumns  = 320;
constexpr float kToneRange = 4.0f;

float Clamp01( float value )
{
	return std::min( 1.0f, std::max( 0.0f, value ) );
}
} // namespace

int ColumnsFromParam( float value )
{
	const float t = Clamp01( value );
	const float ratio = static_cast< float >( kMaxColumns ) / static_cast< float >( kMinColumns );
	const float columns = static_cast< float >( kMinColumns ) * std::pow( ratio, t );

	return std::max( kMinColumns, std::min( kMaxColumns, static_cast< int >( std::lround( columns ) ) ) );
}

float GammaFromParam( float value )
{
	//Centre of the slider is exactly 1, which matters: it is the only position
	//at which the tone curve is doing nothing at all, and it should be findable
	//by dropping the slider to the middle rather than by hunting.
	return std::pow( kToneRange, 1.0f - 2.0f * Clamp01( value ) );
}

float ContrastFromParam( float value )
{
	return std::pow( kToneRange, 2.0f * Clamp01( value ) - 1.0f );
}

} // namespace asciify
