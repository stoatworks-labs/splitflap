#include "Splitflap.h"

#include "Diag.h"
#include "Font.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9), so it has to be asked for by name.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace ffglex;

namespace splitflap
{
namespace
{
static_assert( PT_COUNT_ - PT_ABOUT_TEXT == stoatworks::about::kParamCount,
               "the About block's size changed (a user guide was added?): add or remove a PT_ABOUT_BUTTON_n" );

const char* const kDrumNames[ kDrumCount ]     = { "Tones", "Colours", "Text" };
const char* const kOrderNames[ kOrderCount ]   = { "Dark to Light", "Light to Dark", "Shuffled" };
const char* const kUpdateNames[ kUpdateCount ] = { "Continuous", "Interval", "Onset", "Manual" };

/// Seconds of host time a single frame may advance the board by. The host's
/// clock jumps on a scrub, a retrigger and a sleep; unclamped, any of those
/// turns into every cell skipping its whole drum at once.
constexpr double kMaxFrameDelta = 0.25;

/// How much wider the flap's free edge draws when it is nearest the viewer.
constexpr float kPerspective = 0.10f;

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

int intParam( float value, int lo, int hi )
{
	return std::min( std::max( static_cast< int >( std::lround( value ) ), lo ), hi );
}
} // namespace

SplitflapPlugin::SplitflapPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. What they add up to is a board of 32 x 18 square cells that
	// fills the frame, twelve warm-white tones per drum from dark to light,
	// 80 ms a flap with a little stagger, chasing the clip continuously.
	//---------------------------------------------------------------------
	mParams[ PT_COLUMNS ]     = static_cast< float >( kColumnsDefault );
	mParams[ PT_ROWS ]        = static_cast< float >( kRowsDefault );
	mParams[ PT_FIT ]         = 1.0f;
	mParams[ PT_CELL_ASPECT ] = CellAspectToParam( 0.75f );
	mParams[ PT_GAP ]         = 0.24f; //6% of the cell
	mParams[ PT_SPLIT ]       = 0.25f; //3% of the cell's height

	mParams[ PT_DRUM ]    = static_cast< float >( kDrumTones );
	mParams[ PT_FLAPS ]   = static_cast< float >( kFlapsDefault );
	mParams[ PT_ORDER ]   = static_cast< float >( kOrderDarkToLight );
	mParams[ PT_PALETTE ] = 0.0f;
	mText                 = "DEPARTURES";

	mParams[ PT_FLIP_TIME ] = FlipTimeToParam( 0.08f );
	mParams[ PT_STAGGER ]   = 0.15f;
	mParams[ PT_MODULE ]    = static_cast< float >( kModuleDefault );
	mParams[ PT_UPDATE ]    = static_cast< float >( kUpdateContinuous );
	mParams[ PT_INTERVAL ]  = IntervalToParam( 2.0f );

	mParams[ PT_FLAP_R ] = 1.00f;
	mParams[ PT_FLAP_G ] = 0.93f;
	mParams[ PT_FLAP_B ] = 0.80f;
	mParams[ PT_LIGHT ]  = 0.60f; //12 degrees from above
	mParams[ PT_BOUNCE ] = 0.40f;
	mParams[ PT_MIX ]    = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every FF_TYPE_STANDARD is 0..1 (SetParamInfo clamps the
	// default before a range could be attached); the counts are real
	// integers, which the clamp does not touch.
	//---------------------------------------------------------------------
	SetParamInfo( PT_COLUMNS, "Columns", FF_TYPE_INTEGER, mParams[ PT_COLUMNS ] );
	SetParamRange( PT_COLUMNS, static_cast< float >( kColumnsMin ), static_cast< float >( kColumnsMax ) );
	SetParamInfo( PT_ROWS, "Rows", FF_TYPE_INTEGER, mParams[ PT_ROWS ] );
	SetParamRange( PT_ROWS, static_cast< float >( kRowsMin ), static_cast< float >( kRowsMax ) );
	SetParamInfo( PT_FIT, "Fit", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_CELL_ASPECT, "Cell Aspect", FF_TYPE_STANDARD, mParams[ PT_CELL_ASPECT ] );
	SetParamInfo( PT_GAP, "Gap", FF_TYPE_STANDARD, mParams[ PT_GAP ] );
	SetParamInfo( PT_SPLIT, "Split Line", FF_TYPE_STANDARD, mParams[ PT_SPLIT ] );

	SetOptionParamInfo( PT_DRUM, "Drum", kDrumCount, mParams[ PT_DRUM ] );
	for( int i = 0; i < kDrumCount; ++i )
		SetParamElementInfo( PT_DRUM, static_cast< unsigned >( i ), kDrumNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_FLAPS, "Flaps", FF_TYPE_INTEGER, mParams[ PT_FLAPS ] );
	SetParamRange( PT_FLAPS, static_cast< float >( kFlapsMin ), static_cast< float >( kFlapsMax ) );
	SetOptionParamInfo( PT_ORDER, "Drum Order", kOrderCount, mParams[ PT_ORDER ] );
	for( int i = 0; i < kOrderCount; ++i )
		SetParamElementInfo( PT_ORDER, static_cast< unsigned >( i ), kOrderNames[ i ], static_cast< float >( i ) );
	SetOptionParamInfo( PT_PALETTE, "Palette", drum::PaletteCount(), mParams[ PT_PALETTE ] );
	for( int i = 0; i < drum::PaletteCount(); ++i )
		SetParamElementInfo( PT_PALETTE, static_cast< unsigned >( i ), drum::PaletteName( i ), static_cast< float >( i ) );
	SetParamInfo( PT_TEXT, "Text", FF_TYPE_TEXT, mText.c_str() );

	SetParamInfo( PT_FLIP_TIME, "Flip Time", FF_TYPE_STANDARD, mParams[ PT_FLIP_TIME ] );
	SetParamInfo( PT_STAGGER, "Stagger", FF_TYPE_STANDARD, mParams[ PT_STAGGER ] );
	SetParamInfo( PT_MODULE, "Module Width", FF_TYPE_INTEGER, mParams[ PT_MODULE ] );
	SetParamRange( PT_MODULE, static_cast< float >( kModuleMin ), static_cast< float >( kModuleMax ) );
	SetOptionParamInfo( PT_UPDATE, "Update", kUpdateCount, mParams[ PT_UPDATE ] );
	for( int i = 0; i < kUpdateCount; ++i )
		SetParamElementInfo( PT_UPDATE, static_cast< unsigned >( i ), kUpdateNames[ i ], static_cast< float >( i ) );
	SetParamInfo( PT_INTERVAL, "Interval", FF_TYPE_STANDARD, mParams[ PT_INTERVAL ] );
	SetParamInfo( PT_UPDATE_NOW, "Update Now", FF_TYPE_EVENT, false );

	// An FFT buffer: Resolume shows it as an audio-source picker and writes
	// one spectrum bin per element. Zero elements by default, so with no
	// audio routed Onset mode simply never fires.
	SetBufferParamInfo( PT_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( PT_AUDIO, static_cast< unsigned >( i ), "", 0.0f );

	//Consecutive red/green/blue is what a host needs to show a swatch.
	SetParamInfo( PT_FLAP_R, "Flap Colour", FF_TYPE_RED, mParams[ PT_FLAP_R ] );
	SetParamInfo( PT_FLAP_G, "FlapColour_Green", FF_TYPE_GREEN, mParams[ PT_FLAP_G ] );
	SetParamInfo( PT_FLAP_B, "FlapColour_Blue", FF_TYPE_BLUE, mParams[ PT_FLAP_B ] );
	SetParamInfo( PT_LIGHT, "Light Angle", FF_TYPE_STANDARD, mParams[ PT_LIGHT ] );
	SetParamInfo( PT_BOUNCE, "Bounce", FF_TYPE_STANDARD, mParams[ PT_BOUNCE ] );
	SetParamInfo( PT_MIX, "Mix", FF_TYPE_STANDARD, mParams[ PT_MIX ] );

	for( FFUInt32 i = PT_COLUMNS; i <= PT_SPLIT; ++i )
		SetParamGroup( i, "Board" );
	for( FFUInt32 i = PT_DRUM; i <= PT_TEXT; ++i )
		SetParamGroup( i, "Drum" );
	for( FFUInt32 i = PT_FLIP_TIME; i <= PT_AUDIO; ++i )
		SetParamGroup( i, "Motor" );
	for( FFUInt32 i = PT_FLAP_R; i <= PT_MIX; ++i )
		SetParamGroup( i, "Look" );

	// The About block. Inline, because SetParamInfo is protected.
	SetParamInfo( PT_ABOUT_TEXT, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_TEXT + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_TEXT; i < PT_COUNT_; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Splitflap effect" );
	diag::init();
}

//---------------------------------------------------------------------------
void SplitflapPlugin::SetDebugForTest( const Debug& debug )
{
	mDebug          = debug;
	mOnset.debug.noPrime = debug.noPrime;
	mCurveDirty     = true;
}

//---------------------------------------------------------------------------
bool SplitflapPlugin::uploadFont()
{
	const std::vector< uint8_t > image = font::Texture();
	if( mFontTexture == 0 )
		glGenTextures( 1, &mFontTexture );
	glBindTexture( GL_TEXTURE_2D, mFontTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, font::kTextureWidth, font::kTextureHeight, 0, GL_RED, GL_UNSIGNED_BYTE, image.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return mFontTexture != 0;
}

bool SplitflapPlugin::uploadCurve()
{
	FlapModel model;
	model.restitution          = RestitutionFromParam( mParams[ PT_BOUNCE ] );
	model.constantAcceleration = mDebug.constantAccel;
	model.gravityScale         = mDebug.gravityScale;
	mCurve                     = SolveFlap( model );

	if( mCurveTexture == 0 )
		glGenTextures( 1, &mCurveTexture );
	glBindTexture( GL_TEXTURE_2D, mCurveTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, FlapCurve::kSamples, 1, 0, GL_RED, GL_FLOAT, mCurve.table().data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	mCurveDirty = false;
	return mCurveTexture != 0;
}

bool SplitflapPlugin::uploadDrum()
{
	const DrumMode mode   = static_cast< DrumMode >( OptionIndex( mParams[ PT_DRUM ], kDrumCount ) );
	const DrumOrder order = static_cast< DrumOrder >( OptionIndex( mParams[ PT_ORDER ], kOrderCount ) );
	const int palette     = OptionIndex( mParams[ PT_PALETTE ], drum::PaletteCount() );
	mPrint                = drum::Build( mode, intParam( mParams[ PT_FLAPS ], kFlapsMin, kFlapsMax ), order, palette );

	if( mDrumTexture == 0 )
		glGenTextures( 1, &mDrumTexture );
	glBindTexture( GL_TEXTURE_2D, mDrumTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, mPrint.flaps, 1, 0, GL_RGBA, GL_FLOAT, mPrint.rgba.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	mDrumDirty    = false;
	mMessageDirty = true;
	return mDrumTexture != 0;
}

bool SplitflapPlugin::uploadMessage()
{
	const int columns = intParam( mParams[ PT_COLUMNS ], kColumnsMin, kColumnsMax );
	const int rows    = intParam( mParams[ PT_ROWS ], kRowsMin, kRowsMax );

	//The message is laid out against the Text drum's order whatever drum is
	//on show, so switching to Text later finds it ready.
	const DrumOrder order        = static_cast< DrumOrder >( OptionIndex( mParams[ PT_ORDER ], kOrderCount ) );
	const drum::Print textPrint  = drum::Build( kDrumText, 0, order, 0 );
	const std::vector< float > m = drum::Message( mText, columns, rows, textPrint );

	if( mMessageTexture == 0 )
		glGenTextures( 1, &mMessageTexture );
	glBindTexture( GL_TEXTURE_2D, mMessageTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R32F, columns, rows, 0, GL_RED, GL_FLOAT, m.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	mMessageColumns = columns;
	mMessageRows    = rows;
	mMessageDirty   = false;
	return mMessageTexture != 0;
}

//---------------------------------------------------------------------------
FFResult SplitflapPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &mCopyShader, kCopyShader, "copy" },
		{ &mMotorShader, kMotorShader, "motor" },
		{ &mBoardShader, kBoardShader, "board" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( kVertexShader, stage.fragment ) )
			continue;
		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume. These lines are the only record.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Splitflap: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !mQuad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !uploadFont() || !uploadCurve() || !uploadDrum() || !uploadMessage() )
	{
		diag::error( "could not upload the tables" );
		DeInitGL();
		return FF_FAIL;
	}

	mStateCurrent = 0;
	mFirstFrame   = true;
	mLastSeconds  = -1.0;
	mOnset.Reset();
	mNextTick       = -1.0;
	mLastUpdateMode = -1;

	diag::info( "initialised, " + std::to_string( drum::AlphabetSize() ) + " characters on the text drum, fall time "
	            + std::to_string( mCurve.dimensionlessFallTime() ) + " in the ODE's units" );

	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
void SplitflapPlugin::decideUpdate( double now )
{
	const int mode = OptionIndex( mParams[ PT_UPDATE ], kUpdateCount );
	bool fire      = mFirstFrame || mUpdatePending;
	mUpdatePending = false;

	if( mode != mLastUpdateMode )
	{
		mLastUpdateMode = mode;
		mNextTick       = -1.0;
	}

	switch( mode )
	{
	case kUpdateContinuous:
		fire = true;
		break;
	case kUpdateInterval:
	{
		const double interval = static_cast< double >( IntervalFromParam( mParams[ PT_INTERVAL ] ) );
		if( mNextTick < 0.0 )
			mNextTick = now + interval;
		else if( now >= mNextTick )
		{
			fire = true;
			mNextTick += interval;
			if( now >= mNextTick )//a stall longer than the interval: one update, not a burst
				mNextTick = now + interval;
		}
		break;
	}
	case kUpdateOnset:
		if( mOnsetFired )
			fire = true;
		break;
	default:
		break;
	}
	mFireThisFrame = fire;
}

//---------------------------------------------------------------------------
FFResult SplitflapPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int pictureWidth  = static_cast< int >( picture.Width );
	const int pictureHeight = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// Time, in seconds whatever the host sends. The frame delta is what the
	// motor advances by; the shader never sees an absolute time, so
	// Resolume's ~499 million ms clock never reaches a float.
	//---------------------------------------------------------------------
	mClock.Tick( mHostTime, mHostTimeSeen );
	const double now = mClock.Seconds();
	double dt        = 0.0;
	if( mLastSeconds >= 0.0 )
		dt = std::min( std::max( now - mLastSeconds, 0.0 ), kMaxFrameDelta );
	mLastSeconds = now;

	//---------------------------------------------------------------------
	// Audio, and whether the board refreshes its targets this frame.
	//---------------------------------------------------------------------
	{
		float bins[ kAudioBins ] = {};
		bool any                 = false;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			const size_t n = std::min( info->elements.size(), static_cast< size_t >( kAudioBins ) );
			for( size_t i = 0; i < n; ++i )
			{
				bins[ i ] = info->elements[ i ].value;
				any       = any || bins[ i ] > 0.0f;
			}
		}
		if( any && !mAudioSeen )
		{
			mAudioSeen = true;
			diag::info( "audio reached the plugin" );
		}
		mOnsetFired = mOnset.Frame( now, bins, kAudioBins );
	}
	decideUpdate( now );

	//---------------------------------------------------------------------
	// Geometry: where the board sits in the picture.
	//---------------------------------------------------------------------
	const int columns = intParam( mParams[ PT_COLUMNS ], kColumnsMin, kColumnsMax );
	const int rows    = intParam( mParams[ PT_ROWS ], kRowsMin, kRowsMax );

	float boardOriginX = 0.0f, boardOriginY = 0.0f, boardSizeX = 1.0f, boardSizeY = 1.0f;
	if( mParams[ PT_FIT ] <= 0.5f )
	{
		const float aspect      = CellAspectFromParam( mParams[ PT_CELL_ASPECT ] );
		const float boardAspect = static_cast< float >( columns ) * aspect / static_cast< float >( rows );
		const float frameAspect = static_cast< float >( pictureWidth ) / static_cast< float >( pictureHeight );
		if( boardAspect > frameAspect )
			boardSizeY = frameAspect / boardAspect;
		else
			boardSizeX = boardAspect / frameAspect;
		boardOriginX = 0.5f * ( 1.0f - boardSizeX );
		boardOriginY = 0.5f * ( 1.0f - boardSizeY );
	}
	const float cellWidthPx  = boardSizeX * static_cast< float >( pictureWidth ) / static_cast< float >( columns );
	const float cellHeightPx = boardSizeY * static_cast< float >( pictureHeight ) / static_cast< float >( rows );
	const float pictureLod   = std::floor( std::log2( std::max( 1.0f, std::min( cellWidthPx, cellHeightPx ) / 8.0f ) ) );

	//---------------------------------------------------------------------
	// Tables and buffers, before anything binds a texture: allocating a
	// buffer unbinds the active unit (every ffglex Scoped* clears to 0 on
	// exit rather than restoring).
	//---------------------------------------------------------------------
	if( mCurveDirty && !uploadCurve() )
		return FF_FAIL;
	if( mDrumDirty && !uploadDrum() )
		return FF_FAIL;
	if( mMessageDirty || mMessageColumns != columns || mMessageRows != rows )
		if( !uploadMessage() )
			return FF_FAIL;

	const bool pictureResized = pictureWidth != mPictureWidth || pictureHeight != mPictureHeight;
	mPictureWidth             = pictureWidth;
	mPictureHeight            = pictureHeight;

	if( !mCopy.Ensure( pictureWidth, pictureHeight, GL_RGBA8, PassBuffer::Sampling::Mipmapped ) )
	{
		diag::error( "could not allocate the copy buffer" );
		return FF_FAIL;
	}

	//The state that holds the board is whatever size the last frame's grid
	//was; only the buffer about to be written is sized to this frame's grid.
	//The motor pass maps one onto the other, so a Columns drag keeps the
	//picture and a picture resize never touches it at all.
	const int current = mStateCurrent;
	const int target  = 1 - mStateCurrent;
	if( !mState[ target ].Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the state buffer" );
		return FF_FAIL;
	}
	int oldColumns = 0, oldRows = 0;
	if( !mFirstFrame && mState[ current ].IsValid() && !( pictureResized && mDebug.clearOnResize ) )
	{
		oldColumns = static_cast< int >( mState[ current ].GetWidth() );
		oldRows    = static_cast< int >( mState[ current ].GetHeight() );
	}

	//---------------------------------------------------------------------
	// 1. The clip, into a texture of ours, with a mip chain.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( mCopy.GetGLID(), ScopedFBOBinding::RB_REVERT );
		mCopy.ResizeViewPort();
		ScopedShaderBinding shader( mCopyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( picture.Handle );

		const FFGLTexCoords maxCoords = GetMaxGLTexCoords( picture );
		mCopyShader.Set( "InputTexture", 0 );
		mCopyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		mQuad.Draw();
	}
	mCopy.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. The motor: one fragment per cell.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( mState[ target ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		mState[ target ].ResizeViewPort();
		ScopedShaderBinding shader( mMotorShader.GetGLID() );

		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding prevState( mState[ current ].IsValid() ? mState[ current ].TextureID() : mState[ target ].TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding pictureBinding( mCopy.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding drumBinding( mDrumTexture );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding messageBinding( mMessageTexture );

		mMotorShader.Set( "PrevState", 0 );
		mMotorShader.Set( "Picture", 1 );
		mMotorShader.Set( "Drum", 2 );
		mMotorShader.Set( "Message", 3 );

		glUniform2i( mMotorShader.FindUniform( "Grid" ), columns, rows );
		glUniform2i( mMotorShader.FindUniform( "OldGrid" ), oldColumns, oldRows );
		mMotorShader.Set( "Fire", mFireThisFrame ? 1.0f : 0.0f );
		mMotorShader.Set( "DrumMode", OptionIndex( mParams[ PT_DRUM ], kDrumCount ) );
		mMotorShader.Set( "Flaps", mPrint.flaps );
		mMotorShader.Set( "Dt", static_cast< float >( dt ) );
		mMotorShader.Set( "FlipTime", FlipTimeFromParam( mParams[ PT_FLIP_TIME ] ) );
		mMotorShader.Set( "Stagger", StaggerFromParam( mParams[ PT_STAGGER ] ) );
		mMotorShader.Set( "ModuleWidth", intParam( mParams[ PT_MODULE ], kModuleMin, kModuleMax ) );
		mMotorShader.Set( "BoardOrigin", boardOriginX, boardOriginY );
		mMotorShader.Set( "BoardSize", boardSizeX, boardSizeY );
		mMotorShader.Set( "PictureLod", pictureLod );
		mMotorShader.Set( "AllowBackward", mDebug.allowBackward ? 1 : 0 );
		mQuad.Draw();
	}
	mStateCurrent = target;
	mFirstFrame   = false;

	//---------------------------------------------------------------------
	// 3. The board, straight to the host's framebuffer.
	//---------------------------------------------------------------------
	{
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( mBoardShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding stateBinding( mState[ target ].TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding drumBinding( mDrumTexture );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding curveBinding( mCurveTexture );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding fontBinding( mFontTexture );
		ScopedSamplerActivation sampler4( 4 );
		Scoped2DTextureBinding pictureBinding( mCopy.TextureID() );

		mBoardShader.Set( "State", 0 );
		mBoardShader.Set( "Drum", 1 );
		mBoardShader.Set( "Curve", 2 );
		mBoardShader.Set( "FontTexture", 3 );
		mBoardShader.Set( "Picture", 4 );

		glUniform2i( mBoardShader.FindUniform( "Grid" ), columns, rows );
		mBoardShader.Set( "Flaps", mPrint.flaps );
		mBoardShader.Set( "DrumMode", OptionIndex( mParams[ PT_DRUM ], kDrumCount ) );
		mBoardShader.Set( "FlipTime", FlipTimeFromParam( mParams[ PT_FLIP_TIME ] ) );
		mBoardShader.Set( "BoardOrigin", boardOriginX, boardOriginY );
		mBoardShader.Set( "BoardSize", boardSizeX, boardSizeY );
		mBoardShader.Set( "OutSize", static_cast< float >( hostViewport[ 2 ] ), static_cast< float >( hostViewport[ 3 ] ) );
		const float gap = GapFromParam( mParams[ PT_GAP ] );
		mBoardShader.Set( "Gap", gap, gap * cellWidthPx / std::max( cellHeightPx, 1.0f ) );
		mBoardShader.Set( "Split", SplitFromParam( mParams[ PT_SPLIT ] ) );
		mBoardShader.Set( "CellAspect", cellWidthPx / std::max( cellHeightPx, 1.0f ) );
		mBoardShader.Set( "FlapColour", mParams[ PT_FLAP_R ], mParams[ PT_FLAP_G ], mParams[ PT_FLAP_B ] );
		mBoardShader.Set( "LightAngle", LightAngleFromParam( mParams[ PT_LIGHT ] ) );
		mBoardShader.Set( "MixAmount", mParams[ PT_MIX ] );
		mBoardShader.Set( "Perspective", kPerspective );
		mBoardShader.Set( "CurveSamples", FlapCurve::kSamples );
		mBoardShader.Set( "CurveSpan", static_cast< float >( FlapCurve::kSpan ) );
		mQuad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SplitflapPlugin::DeInitGL()
{
	mCopyShader.FreeGLResources();
	mMotorShader.FreeGLResources();
	mBoardShader.FreeGLResources();
	mQuad.Release();
	mCopy.Destroy();
	mState[ 0 ].Destroy();
	mState[ 1 ].Destroy();
	for( GLuint* texture : { &mDrumTexture, &mMessageTexture, &mCurveTexture, &mFontTexture } )
	{
		if( *texture != 0 )
		{
			glDeleteTextures( 1, texture );
			*texture = 0;
		}
	}
	mDrumDirty    = true;
	mMessageDirty = true;
	mCurveDirty   = true;
	mFirstFrame   = true;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult SplitflapPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT_ )
		return FF_FAIL;

	if( index >= PT_ABOUT_TEXT )
		return stoatworks::about::handleParam( index - PT_ABOUT_TEXT, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_UPDATE_NOW )
	{
		//An event arrives as 1.0 on press and 0.0 on release.
		if( value >= 0.5f )
			mUpdatePending = true;
		return FF_SUCCESS;
	}

	mParams[ index ] = value;

	switch( index )
	{
	case PT_DRUM:
	case PT_FLAPS:
	case PT_ORDER:
	case PT_PALETTE:
		mDrumDirty = true;
		break;
	case PT_COLUMNS:
	case PT_ROWS:
		mMessageDirty = true;
		break;
	case PT_BOUNCE:
		mCurveDirty = true;
		break;
	default:
		break;
	}
	return FF_SUCCESS;
}

float SplitflapPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT_ )
		return 0.0f;
	return mParams[ index ];
}

FFResult SplitflapPlugin::SetTextParameter( unsigned int index, const char* value )
{
	if( index == PT_TEXT )
	{
		mText         = value ? value : "";
		mMessageDirty = true;
		return FF_SUCCESS;
	}
	//The About line is display-only, but the base class fails, and a failed
	//default deletes the instance in a real host.
	if( index == PT_ABOUT_TEXT )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

char* SplitflapPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_TEXT )
		return const_cast< char* >( mText.c_str() );
	if( index == PT_ABOUT_TEXT )
	{
		mAboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( mAboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult SplitflapPlugin::SetTime( double time )
{
	mHostTime     = time;
	mHostTimeSeen = true;
	return FF_SUCCESS;
}

} // namespace splitflap
