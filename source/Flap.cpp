#include "Flap.h"

#include <algorithm>
#include <cmath>

namespace splitflap
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

/// RK4 step, dimensionless. The model's acceleration only.
struct Ode
{
	const FlapModel& model;
	double accel( double theta ) const
	{
		if( model.constantAcceleration )
			return 1.0;
		return model.gravityScale * std::sin( theta );
	}
	void step( double& theta, double& omega, double h ) const
	{
		const double k1t = omega, k1w = accel( theta );
		const double k2t = omega + 0.5 * h * k1w, k2w = accel( theta + 0.5 * h * k1t );
		const double k3t = omega + 0.5 * h * k2w, k3w = accel( theta + 0.5 * h * k2t );
		const double k4t = omega + h * k3w, k4w = accel( theta + h * k3t );
		theta += h / 6.0 * ( k1t + 2.0 * k2t + 2.0 * k3t + k4t );
		omega += h / 6.0 * ( k1w + 2.0 * k2w + 2.0 * k3w + k4w );
	}
};

constexpr double kStep = 1.0e-4;

/// The dimensionless time from release to the first arrival at pi, with no
/// bounce: the reference every table is scaled by.
double fallTime( const FlapModel& model )
{
	Ode ode { model };
	double theta = model.releaseAngle, omega = 0.0, t = 0.0;
	for( int i = 0; i < 20000000; ++i )
	{
		const double prevTheta = theta;
		ode.step( theta, omega, kStep );
		t += kStep;
		if( theta >= kPi )
		{
			// Interpolate the crossing inside the step rather than taking the
			// step's end: a sampled comparator errs in one direction.
			const double f = ( kPi - prevTheta ) / ( theta - prevTheta );
			return t - kStep + f * kStep;
		}
	}
	return t;
}
} // namespace

FlapCurve SolveFlap( const FlapModel& model )
{
	// The scaling is always the NOMINAL model's fall time, so a perturbed
	// model lands at a different flip unit -- which is what the negative
	// control asserts on.
	FlapModel nominal;
	nominal.releaseAngle    = model.releaseAngle;
	const double reference  = fallTime( nominal );

	Ode ode { model };
	double theta = model.releaseAngle, omega = 0.0, t = 0.0;
	const double tEnd = FlapCurve::kSpan * reference;

	// Half a degree of rebound amplitude, as an angular velocity: for a small
	// swing about the stop, 1/2 w^2 = 1 - cos(phi) ~ phi^2 / 2, so w = phi.
	const double stopVelocity = 0.5 * kPi / 180.0;

	std::vector< double > fine;
	fine.reserve( static_cast< size_t >( tEnd / kStep ) + 2 );
	fine.push_back( theta );

	bool pinned      = false;
	double pinnedAt  = 0.0;
	while( t < tEnd )
	{
		if( pinned )
		{
			fine.push_back( kPi );
			t += kStep;
			continue;
		}
		const double prevTheta = theta, prevOmega = omega;
		ode.step( theta, omega, kStep );
		t += kStep;
		if( theta >= kPi )
		{
			// Impact inside this step. Land at the interpolated instant, reverse
			// and shrink the velocity, then finish the step from there.
			const double f     = ( kPi - prevTheta ) / ( theta - prevTheta );
			const double tHit  = f * kStep;
			double wHit        = prevOmega + f * ( omega - prevOmega );
			wHit               = -model.restitution * wHit;
			if( -wHit < stopVelocity )
			{
				pinned   = true;
				pinnedAt = t - kStep + tHit;
				theta    = kPi;
				omega    = 0.0;
			}
			else
			{
				theta = kPi;
				omega = wHit;
				ode.step( theta, omega, kStep - tHit );
				if( theta > kPi )
					theta = kPi;
			}
		}
		fine.push_back( theta );
	}

	FlapCurve curve;
	curve.mFallTime  = reference;
	curve.mBounceEnd = pinned ? pinnedAt / reference : FlapCurve::kSpan;
	curve.mTable.resize( static_cast< size_t >( FlapCurve::kSamples ) );
	for( int i = 0; i < FlapCurve::kSamples; ++i )
	{
		const double u  = FlapCurve::kSpan * static_cast< double >( i ) / static_cast< double >( FlapCurve::kSamples - 1 );
		const double tt = u * reference;
		const double fi = tt / kStep;
		const size_t k  = static_cast< size_t >( std::floor( fi ) );
		double value;
		if( k + 1 >= fine.size() )
			value = fine.back();
		else
		{
			const double w = fi - static_cast< double >( k );
			value          = fine[ k ] + ( fine[ k + 1 ] - fine[ k ] ) * w;
		}
		curve.mTable[ static_cast< size_t >( i ) ] = static_cast< float >( std::min( value, kPi ) );
	}
	// The stop is exactly pi from the last bounce on, so a settled board is
	// bit-identical frame to frame rather than creeping by an ulp.
	if( pinned )
	{
		const int from = static_cast< int >( std::ceil( curve.mBounceEnd / FlapCurve::kSpan * ( FlapCurve::kSamples - 1 ) ) );
		for( int i = std::max( from, 0 ); i < FlapCurve::kSamples; ++i )
			curve.mTable[ static_cast< size_t >( i ) ] = static_cast< float >( kPi );
	}
	return curve;
}

float FlapCurve::angleAt( double u ) const
{
	// The same arithmetic as the board shader's curveAngle(): float index,
	// floor, two fetches, mix( a, b, w ) = a * ( 1 - w ) + b * w.
	const float f = static_cast< float >( std::min( std::max( u / kSpan, 0.0 ), 1.0 ) ) * static_cast< float >( kSamples - 1 );
	int i         = static_cast< int >( std::floor( f ) );
	i             = std::min( std::max( i, 0 ), kSamples - 1 );
	const int j   = std::min( i + 1, kSamples - 1 );
	const float w = f - static_cast< float >( i );
	const float a = mTable[ static_cast< size_t >( i ) ];
	const float b = mTable[ static_cast< size_t >( j ) ];
	return a * ( 1.0f - w ) + b * w;
}

} // namespace splitflap
