#include "Onset.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace splitflap
{
namespace
{
constexpr double kFloorSeconds  = 1.0;  ///< the floor follows the flux over this
constexpr double kRatio         = 2.5;  ///< flux must clear the floor by this
constexpr double kMinFlux       = 0.02; ///< and this, so silence never fires
constexpr double kRefractory    = 0.10; ///< seconds between fires
constexpr double kPrimeFraction = 0.125;///< the floor's seed, as a fraction of the level
} // namespace

void Onset::Reset()
{
	std::memset( mPrev, 0, sizeof( mPrev ) );
	mFloor       = 0.0;
	mFlux        = 0.0;
	mFluxPrev    = 0.0;
	mLastFire    = -1.0e9;
	mLastSeconds = 0.0;
	mPrimed      = false;
}

bool Onset::Frame( double seconds, const float* bins, int count )
{
	double m[ kBins ];
	double level = 0.0;
	for( int i = 0; i < kBins; ++i )
	{
		const float v = ( bins != nullptr && i < count ) ? bins[ i ] : 0.0f;
		// A host may hand back a negative or a NaN in a buffer it has not
		// filled; either would poison the floor for good.
		const double clean = ( v > 0.0f ) ? static_cast< double >( v ) : 0.0;
		// Root magnitude: bin values bunch near zero, and a flux on the raw
		// numbers hears nothing but the kick drum.
		m[ i ] = std::sqrt( clean );
		level += m[ i ];
	}

	if( !mPrimed )
	{
		if( debug.noPrime )
		{
			std::memset( mPrev, 0, sizeof( mPrev ) );
			mFloor = 0.0;
		}
		else
		{
			std::memcpy( mPrev, m, sizeof( mPrev ) );
			mFloor = level * kPrimeFraction;
		}
		mFluxPrev    = 0.0;
		mLastSeconds = seconds;
		mLastFire    = seconds - kRefractory;
		mPrimed      = true;
		if( !debug.noPrime )
			return false;
	}

	const double dt = std::max( seconds - mLastSeconds, 0.0 );
	mLastSeconds    = seconds;

	double flux = 0.0;
	for( int i = 0; i < kBins; ++i )
		flux += std::max( m[ i ] - mPrev[ i ], 0.0 );
	std::memcpy( mPrev, m, sizeof( mPrev ) );

	bool fired = false;
	if( flux > std::max( kRatio * mFloor, kMinFlux ) && flux > mFluxPrev && seconds - mLastFire >= kRefractory )
	{
		fired     = true;
		mLastFire = seconds;
	}

	// The floor follows the flux, and only when time has passed: a zero
	// interval must not snap it.
	if( dt > 0.0 )
	{
		const double a = 1.0 - std::exp( -dt / kFloorSeconds );
		mFloor += ( flux - mFloor ) * a;
	}
	mFluxPrev = flux;
	mFlux     = flux;
	return fired;
}

} // namespace splitflap
