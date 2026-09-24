#pragma once

/**
    An onset detector over the host's 64 spectrum bins, primed on frame one.

    Spectral flux -- the sum over bins of the positive change in root
    magnitude since the last frame -- against a floor that follows the flux
    over one second. It fires on a rising frame that clears the floor by a
    ratio, and then not again for a refractory interval. Nothing in it maps a
    bin to a frequency: nobody has measured what Resolume's bins are, so the
    detector treats them as 64 unnamed channels and asks only whether more of
    them just got louder.

    **Frame one primes it.** The previous spectrum is set to the current one
    and the floor is seeded from the level, so a clip that triggers into loud
    audio does not read every bin as risen from silence. Without that the
    board would clatter on every clip trigger and then be deaf for the second
    it takes the floor to settle. The floor only moves when time has passed:
    a zero interval leaves it where it is rather than snapping it, which is
    the other half of the same trap.

    `debug.noPrime` is the fleet's old habit kept as a negative control;
    `sftest --prime` requires it to fire falsely.
*/
namespace splitflap
{
class Onset
{
public:
	struct Debug
	{
		bool noPrime = false;
	};

	static constexpr int kBins = 64;

	void Reset();

	/// One frame of the host's spectrum. Returns true when an onset fires.
	bool Frame( double seconds, const float* bins, int count );

	bool Primed() const
	{
		return mPrimed;
	}
	double Flux() const
	{
		return mFlux;
	}
	double Floor() const
	{
		return mFloor;
	}

	Debug debug;

private:
	double mPrev[ kBins ] = {};
	double mFloor         = 0.0;
	double mFlux          = 0.0;
	double mFluxPrev      = 0.0;
	double mLastFire      = -1.0e9;
	double mLastSeconds   = 0.0;
	bool mPrimed          = false;
};

} // namespace splitflap
