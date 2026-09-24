#pragma once

#include <vector>

/**
    The flap is a hinged plate, and its fall is solved once, in double.

    A split-flap unit's top flap is a thin rigid plate hinged along the split
    line. Released just past vertical it falls under gravity about the hinge,
    I theta'' = m g (L/2) sin theta with I = m L^2 / 3 for a plate about its
    edge, so theta'' = (3g / 2L) sin theta. That is an inverted pendulum let
    go: it hesitates near the top, whips through the horizontal and slams on
    to the stop at 180 degrees, where it is caught with a coefficient of
    restitution e and flutters to rest. That hesitate-and-slap is the whole
    look of a real board, and it is what the shading follows.

    Nothing in it is tuned by hand. `Flip Time` is the only knob: it scales g
    so that the fall from the release angle to the stop takes exactly one flip
    time. In the table below time is therefore in *flip units* -- u = 1 is the
    landing -- and the bounce that follows is in the same units, so a shorter
    flip time is a proportionally quicker flutter, as a stiffer, faster board
    would show.

    The profile is integrated here with RK4 at a fine step, the impact times
    found by interpolating the crossing of theta = pi inside the step it
    happens in, and the result sampled into a table the shader reads with the
    same linear interpolation `angleAt` uses on the CPU. The harness's
    `--fall` compares the rendered flap's projected height against
    `angleAt`, and its negative controls perturb `FlapModel` -- a constant
    angular acceleration in place of sin theta, or gravity detuned without
    the flip time following -- and require the comparison to fail.
*/
namespace splitflap
{

struct FlapModel
{
	/// Radians past vertical at which the drum's finger lets the flap go, at
	/// rest. Three degrees: at exactly vertical an inverted pendulum never
	/// falls, and the log-slow departure from a few degrees is the hesitation
	/// a real flap shows.
	double releaseAngle = 0.05235987755982988;

	/// Coefficient of restitution at the stop. The angular velocity reverses
	/// and shrinks by this on every impact; the flutter ends when a rebound
	/// would rise less than half a degree.
	double restitution = 0.24;

	// -- Negative controls, for the harness only --------------------------------

	/// theta'' = constant instead of sin theta: the same fall time, a
	/// different shape. `--fall` must fail with it.
	bool constantAcceleration = false;

	/// Multiplies g in the fall without the flip time following it, so the
	/// flap lands early or late. `--fall` must fail with it away from 1.
	double gravityScale = 1.0;
};

class FlapCurve
{
public:
	/// Table samples and the span they cover, in flip units. The shader holds
	/// the same two constants as uniforms and reads the table with
	/// texelFetch and its own mix(), so `angleAt` and the picture agree to
	/// float rounding.
	static constexpr int kSamples  = 2048;
	static constexpr double kSpan  = 4.0;

	/// The flap's angle from vertical-up, in radians, at `u` flip units after
	/// release: 0 at the top, pi on the stop. Past the last bounce it is
	/// exactly pi, and past kSpan it is pinned there.
	float angleAt( double u ) const;

	/// Flip units after release at which the flap has stopped moving for
	/// good. 1.0 with no bounce.
	double bounceEnd() const
	{
		return mBounceEnd;
	}

	/// The fall time in the dimensionless ODE's own units (g = L = 1), which is
	/// what `Flip Time` divides by to get the scaling of g. For the record.
	double dimensionlessFallTime() const
	{
		return mFallTime;
	}

	const std::vector< float >& table() const
	{
		return mTable;
	}

private:
	friend FlapCurve SolveFlap( const FlapModel& model );

	std::vector< float > mTable;
	double mBounceEnd = 1.0;
	double mFallTime  = 0.0;
};

/// Integrate the model and sample it. A few milliseconds; the plugin calls it
/// at InitGL and whenever `Bounce` moves.
FlapCurve SolveFlap( const FlapModel& model );

} // namespace splitflap
