#pragma once

#include "Clock.h"
#include "Controls.h"
#include "Drum.h"
#include "Flap.h"
#include "Onset.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include <string>
#include <vector>

/**
    Splitflap -- the picture on a split-flap departures board, for Resolume.

    A split-flap unit holds a drum of N flaps, each printed with one symbol,
    and **it can only turn forward, one flap at a time, at the motor's rate**.
    To show a new symbol it passes through every flap between the one on show
    and the target. Give each cell of a board a drum -- greys, colours or
    characters -- point the board at the clip, and the look of a departures
    board changing falls out of that one constraint: a change ripples, a
    step brighter is one flip and a step darker is N - 1, a moving picture is
    never finished, and every flap in the air is a hinged plate falling under
    gravity with its shading following its angle.

    Three passes (Shaders.h): copy the clip with a mip chain; the motor, one
    fragment per cell, which latches targets and advances the drums; the
    board, which draws the halves and the flaps. The state is a float texel
    per cell carried across frames.

    See AGENTS.md for the traps and what is verified.
*/
namespace splitflap
{
class SplitflapPlugin : public CFFGLPlugin
{
public:
	SplitflapPlugin();

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	// -- Test hooks. The harness drives the real plugin class, so its knobs
	// -- are members rather than a second copy of anything.

	/// Negative controls: each breaks one claim so a check can be seen to fail.
	struct Debug
	{
		bool allowBackward   = false;///< the shorter way round the drum
		bool clearOnResize   = false;///< the photofinish bug: state cleared on a picture resize
		bool noPrime         = false;///< the onset detector unprimed on frame one
		bool constantAccel   = false;///< theta'' = constant instead of sin theta
		double gravityScale  = 1.0;  ///< g detuned without the flip time following
	};
	void SetDebugForTest( const Debug& debug );

	/// The offline harness declares its clock unit rather than letting the
	/// plugin infer one.
	void ForceSecondsClock()
	{
		mClock.ForceSeconds();
	}

	/// The flap curve the board is drawing from, for `--fall`.
	const FlapCurve& CurveForTest() const
	{
		return mCurve;
	}

	/// Whether the onset detector fired on the last frame, for `--prime`.
	bool OnsetFiredForTest() const
	{
		return mOnsetFired;
	}

	/// The drum as printed, for the harness to read tones back.
	const drum::Print& PrintForTest() const
	{
		return mPrint;
	}

private:
	bool uploadDrum();
	bool uploadMessage();
	bool uploadCurve();
	bool uploadFont();
	void decideUpdate( double now );

	ffglex::FFGLShader mCopyShader;
	ffglex::FFGLShader mMotorShader;
	ffglex::FFGLShader mBoardShader;
	ffglex::FFGLScreenQuad mQuad;

	PassBuffer mCopy;         ///< the clip, ours, mipmapped
	PassBuffer mState[ 2 ];   ///< ping-pong: one float texel per cell
	int mStateCurrent = 0;

	GLuint mDrumTexture    = 0;
	GLuint mMessageTexture = 0;
	GLuint mCurveTexture   = 0;
	GLuint mFontTexture    = 0;

	drum::Print mPrint;
	bool mDrumDirty    = true;
	bool mMessageDirty = true;
	bool mCurveDirty   = true;
	int mMessageColumns = 0, mMessageRows = 0;

	FlapCurve mCurve;
	Debug mDebug;

	Clock mClock;
	double mHostTime    = 0.0;
	bool mHostTimeSeen  = false;
	double mLastSeconds = -1.0;
	bool mFirstFrame    = true;

	Onset mOnset;
	bool mOnsetFired = false;

	// Update decisions.
	bool mFireThisFrame = false;
	bool mUpdatePending = false;///< the Update Now event
	double mNextTick    = -1.0; ///< Interval mode
	int mLastUpdateMode = -1;

	int mPictureWidth = 0, mPictureHeight = 0;
	bool mAudioSeen = false;

	float mParams[ PT_COUNT_ ] = {};
	std::string mText;
	std::string mAboutText;
};

} // namespace splitflap
