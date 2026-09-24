#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace splitflap
{
namespace
{
inline float clamp01( float value )
{
	return std::min( std::max( value, 0.0f ), 1.0f );
}

inline float lerp( float from, float to, float t )
{
	return from + ( to - from ) * clamp01( t );
}

/// Geometric interpolation: equal slider movements are equal *ratios*, right
/// for any quantity where the question is "how many times more".
inline float geometric( float from, float to, float t )
{
	return from * std::pow( to / from, clamp01( t ) );
}

inline float geometricInverse( float from, float to, float x )
{
	return clamp01( std::log( x / from ) / std::log( to / from ) );
}
} // namespace

int OptionIndex( float value, int count )
{
	if( count <= 1 )
		return 0;
	// An index arrives as 0, 1, 2...; a normalised value as 0..1. Anything at
	// or below 1 that is not a whole number can only be the latter.
	float v = value;
	if( v > 0.0f && v <= 1.0f && std::fabs( v - std::round( v ) ) > 1e-4f )
		v = v * static_cast< float >( count - 1 );
	const int index = static_cast< int >( std::lround( v ) );
	return std::min( std::max( index, 0 ), count - 1 );
}

float CellAspectFromParam( float value )
{
	return lerp( 0.5f, 1.5f, value );
}

float CellAspectToParam( float aspect )
{
	return clamp01( ( aspect - 0.5f ) / 1.0f );
}

float GapFromParam( float value )
{
	return lerp( 0.0f, 0.25f, value );
}

float SplitFromParam( float value )
{
	return lerp( 0.0f, 0.12f, value );
}

float FlipTimeFromParam( float value )
{
	return geometric( 0.03f, 1.0f, value );
}

float FlipTimeToParam( float seconds )
{
	return geometricInverse( 0.03f, 1.0f, seconds );
}

float StaggerFromParam( float value )
{
	return lerp( 0.0f, 1.0f, value );
}

float IntervalFromParam( float value )
{
	return geometric( 0.1f, 10.0f, value );
}

float IntervalToParam( float seconds )
{
	return geometricInverse( 0.1f, 10.0f, seconds );
}

float LightAngleFromParam( float value )
{
	constexpr float kSixtyDegrees = 1.0471975512f;
	return lerp( -kSixtyDegrees, kSixtyDegrees, value );
}

float RestitutionFromParam( float value )
{
	return lerp( 0.0f, 0.6f, value );
}

} // namespace splitflap
