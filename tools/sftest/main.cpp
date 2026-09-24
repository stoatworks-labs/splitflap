/**
    sftest -- render Splitflap offline, and check what its flaps are doing.

    Every check here reads the picture the shipped shaders drew, through the
    real plugin class in a headless CGL context, and never the state texture.
    The flip counts are transitions of a cell's top row; the flap's angle is
    the projected height of the flap against the halves behind it; a settle
    is bit-identical frames. The plugin's own `FlapCurve` is the model for
    `--fall` because the picture is drawn from it -- the check asks whether
    the rasteriser reproduced the table, and the negative controls ask
    whether a different table would have been noticed.

        sftest --out /tmp/f.png [--size WxH] [--frames N] [--set "Name=v"]...
        sftest --list | --names | --font
        sftest --flips | --asymmetry | --settle | --fall | --resize | --prime
        sftest --negative       every check against its broken model, must FAIL
        sftest --bench
        sftest --pipe --size WxH [--fps N] [--frames N] [--script cues.txt]

    Every check runs at 640x360 and at 320x180 -- the raster CI uses -- and
    reports each.

    `--pipe` takes the fleet's frame format so one script can film any of the
    FFGL plugins: raw RGBA frames in on stdin, raw RGBA frames out on stdout.
    The cue script is `frame  Parameter Name  value` lines, held before the
    first key and after the last. Standard parameters ramp between keys;
    options, booleans, integers and events STEP, because a ramp through an
    option fires every intermediate one, and an event half-way up a ramp is a
    press nobody keyed. SIGPIPE is ignored: a reader that hangs up makes the
    write fail and the harness exit 1, not 141.
*/

#include "Controls.h"
#include "Drum.h"
#include "Flap.h"
#include "Font.h"
#include "Splitflap.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace splitflap;

namespace
{
//---------------------------------------------------------------------------
// PNG out. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	for( int shift = 24; shift >= 0; shift -= 8 )
		out.push_back( static_cast< unsigned char >( value >> shift ) );
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
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf size = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( size );
	if( compress2( compressed.data(), &size, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( size );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = std::fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// GL.
//---------------------------------------------------------------------------
CGLContextObj gContext = nullptr;

bool openGL()
{
	if( gContext != nullptr )
		return true;
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated, kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ), static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ), static_cast< CGLPixelFormatAttribute >( 0 )
	};
	CGLPixelFormatObj format = nullptr;
	GLint count              = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &count ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &count ) != kCGLNoError || format == nullptr )
		{
			std::fprintf( stderr, "sftest: could not choose a pixel format\n" );
			return false;
		}
	const CGLError error = CGLCreateContext( format, nullptr, &gContext );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
	{
		std::fprintf( stderr, "sftest: could not create an OpenGL context\n" );
		return false;
	}
	CGLSetCurrentContext( gContext );
	return true;
}

struct Image
{
	int width = 0, height = 0;
	std::vector< unsigned char > px;///< RGBA, top row first

	unsigned char at( int x, int y, int c = 0 ) const
	{
		return px[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
	}
	bool operator==( const Image& o ) const
	{
		return width == o.width && height == o.height && px == o.px;
	}
};

/// A test card: a soft gradient with a disc, a ring and a bar, so a default
/// render has tones to quantise and edges to ripple.
Image card( int width, int height )
{
	Image img;
	img.width  = width;
	img.height = height;
	img.px.resize( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width, v = ( y + 0.5f ) / height;
			float r = 0.15f + 0.6f * u, g = 0.15f + 0.6f * u, b = 0.15f + 0.6f * u;
			const float aspect = static_cast< float >( width ) / height;
			const float dx1 = ( u - 0.25f ) * aspect, dy1 = v - 0.5f;
			if( std::sqrt( dx1 * dx1 + dy1 * dy1 ) < 0.2f )
				r = g = b = 0.95f;
			const float dx2 = ( u - 0.6f ) * aspect, dy2 = v - 0.5f, d2 = std::sqrt( dx2 * dx2 + dy2 * dy2 );
			if( d2 < 0.22f && d2 > 0.14f )
			{
				r = 0.9f;
				g = 0.5f;
				b = 0.1f;
			}
			if( u > 0.82f && u < 0.94f && v > 0.15f && v < 0.85f )
				r = g = b = 0.02f;
			unsigned char* p = &img.px[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ]           = static_cast< unsigned char >( r * 255.0f + 0.5f );
			p[ 1 ]           = static_cast< unsigned char >( g * 255.0f + 0.5f );
			p[ 2 ]           = static_cast< unsigned char >( b * 255.0f + 0.5f );
			p[ 3 ]           = 255;
		}
	return img;
}

/// A picture that is one flat grey per cell of a columns x rows grid: what
/// the checks feed the board, so a cell's mean is exactly its tone.
Image cellCard( int width, int height, int columns, int rows, const std::function< float( int, int ) >& tone )
{
	Image img;
	img.width  = width;
	img.height = height;
	img.px.resize( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int col = x * columns / width;
			const int row = y * rows / height;
			const unsigned char v = static_cast< unsigned char >( std::lround( std::min( std::max( tone( col, row ), 0.0f ), 1.0f ) * 255.0f ) );
			unsigned char* p      = &img.px[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			p[ 0 ] = p[ 1 ] = p[ 2 ] = v;
			p[ 3 ]                   = 255;
		}
	return img;
}

/// The plugin in a headless context: an input texture, an output FBO and a
/// synthetic 60 fps clock. `frame()` renders one frame and reads it back.
struct Session
{
	SplitflapPlugin plugin;
	int width = 0, height = 0;
	GLuint input = 0, output = 0, fbo = 0;
	bool initialised = false;
	double fps       = 60.0;
	int frameIndex   = 0;
	float bins[ kAudioBins ] = {};

	bool init()
	{
		plugin.ForceSecondsClock();
		FFGLViewportStruct vp = {};
		vp.width              = 16;
		vp.height             = 16;
		if( plugin.InitGL( &vp ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "sftest: InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		initialised = true;
		return true;
	}

	~Session()
	{
		if( initialised )
			plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( fbo )
			glDeleteFramebuffers( 1, &fbo );
		if( output )
			glDeleteTextures( 1, &output );
		if( input )
			glDeleteTextures( 1, &input );
		fbo = output = input = 0;
	}

	/// (Re)size the picture. A resize mid-run is the photofinish trap.
	void resize( int w, int h )
	{
		if( w == width && h == height && input != 0 )
			return;
		release();
		width  = w;
		height = h;
		glGenTextures( 1, &input );
		glBindTexture( GL_TEXTURE_2D, input );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		glGenTextures( 1, &output );
		glBindTexture( GL_TEXTURE_2D, output );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glGenFramebuffers( 1, &fbo );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	}

	/// Upload a picture (top row first, as a file has it).
	void setInput( const Image& img )
	{
		resize( img.width, img.height );
		std::vector< unsigned char > flipped( img.px.size() );
		const size_t stride = static_cast< size_t >( width ) * 4;
		for( int y = 0; y < height; ++y )
			std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
			             img.px.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
		glBindTexture( GL_TEXTURE_2D, input );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	bool set( const std::string& name, float value )
	{
		for( unsigned i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* n = plugin.GetParamName( i );
			if( n && name == n )
			{
				plugin.SetFloatParameter( i, value );
				return true;
			}
		}
		return false;
	}

	bool setText( const std::string& name, const std::string& value )
	{
		for( unsigned i = 0; i < plugin.GetNumParams(); ++i )
		{
			const char* n = plugin.GetParamName( i );
			if( n && name == n )
			{
				plugin.SetTextParameter( i, value.c_str() );
				return true;
			}
		}
		return false;
	}

	void setAudio( float level )
	{
		for( int i = 0; i < kAudioBins; ++i )
			bins[ i ] = level;
	}

	/// Render the next frame of the synthetic clock and read it back.
	Image frame()
	{
		for( int i = 0; i < kAudioBins; ++i )
			plugin.SetParamElementValue( PT_AUDIO, static_cast< unsigned >( i ), bins[ i ] );
		plugin.SetTime( static_cast< double >( frameIndex ) / fps );
		++frameIndex;

		FFGLTextureStruct in = {};
		in.Width = in.HardwareWidth = static_cast< FFUInt32 >( width );
		in.Height = in.HardwareHeight = static_cast< FFUInt32 >( height );
		in.Handle                     = input;
		FFGLTextureStruct* inputs[ 1 ] = { &in };
		ProcessOpenGLStruct process    = {};
		process.numInputTextures       = 1;
		process.inputTextures          = inputs;
		process.HostFBO                = fbo;

		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			std::fprintf( stderr, "sftest: ProcessOpenGL failed on frame %d\n", frameIndex - 1 );

		Image img;
		img.width  = width;
		img.height = height;
		std::vector< unsigned char > raw( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, fbo );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data() );
		img.px.resize( raw.size() );
		const size_t stride = static_cast< size_t >( width ) * 4;
		for( int y = 0; y < height; ++y )
			std::memcpy( img.px.data() + static_cast< size_t >( y ) * stride,
			             raw.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
		return img;
	}

	/// Render `n` frames, returning the last.
	Image frames( int n )
	{
		Image last;
		for( int i = 0; i < n; ++i )
			last = frame();
		return last;
	}
};

//---------------------------------------------------------------------------
// The settings every measurement check shares: a full-frame board with no
// gap, no split line, white flaps lit straight on, a Tones drum from dark to
// light, no stagger, no bounce, updating continuously. A check that wants
// something else says so.
//---------------------------------------------------------------------------
constexpr double kFlipSeconds = 0.2;///< twelve frames at 60 fps

void plainBoard( Session& s, int columns, int rows, int flaps )
{
	s.set( "Columns", static_cast< float >( columns ) );
	s.set( "Rows", static_cast< float >( rows ) );
	s.set( "Fit", 1.0f );
	s.set( "Gap", 0.0f );
	s.set( "Split Line", 0.0f );
	s.set( "Drum", static_cast< float >( kDrumTones ) );
	s.set( "Flaps", static_cast< float >( flaps ) );
	s.set( "Drum Order", static_cast< float >( kOrderDarkToLight ) );
	s.set( "Flip Time", FlipTimeToParam( static_cast< float >( kFlipSeconds ) ) );
	s.set( "Stagger", 0.0f );
	s.set( "Module Width", 1.0f );
	s.set( "Update", static_cast< float >( kUpdateContinuous ) );
	s.set( "Flap Colour", 1.0f );
	s.set( "FlapColour_Green", 1.0f );
	s.set( "FlapColour_Blue", 1.0f );
	s.set( "Light Angle", 0.5f );
	s.set( "Bounce", 0.0f );
	s.set( "Mix", 1.0f );
}

/// The flap a Tones pixel is showing. The plate is mix( 0.06, 1, k/(N-1) )
/// under a light straight on, so the tones are 0.06 + 0.94 k / ( N - 1 ) and
/// with N <= 8 they are at least 34 levels apart -- an 8-bit rounding of one
/// is never nearer another.
int toneIndex( unsigned char value, int flaps )
{
	const float v = ( static_cast< float >( value ) / 255.0f - 0.06f ) / 0.94f;
	return std::min( std::max( static_cast< int >( std::lround( v * ( flaps - 1 ) ) ), 0 ), flaps - 1 );
}

/// The pixel a check reads for cell (col, row): the centre column of the top
/// row of the cell's top half. The first row the falling flap uncovers.
struct CellProbe
{
	int width, height, columns, rows;
	int x( int col ) const
	{
		return col * width / columns + ( width / columns ) / 2;
	}
	int top( int row ) const
	{
		return row * height / rows;
	}
	int bottom( int row ) const
	{
		return ( row + 1 ) * height / rows - 1;
	}
};

/// The rasters every check runs at.
struct Raster
{
	int width, height;
};
const Raster kRasters[] = { { 640, 360 }, { 320, 180 } };

/// The nearest flap to a tone, for a drum printed dark to light.
int nearestFlap( float tone, int flaps )
{
	return static_cast< int >( std::lround( tone * ( flaps - 1 ) ) );
}

int gFailures = 0;

void report( bool ok, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
void report( bool ok, const char* format, ... )
{
	char buffer[ 1024 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	std::printf( "  %s  %s\n", ok ? "ok  " : "FAIL", buffer );
	if( !ok )
		++gFailures;
}

//---------------------------------------------------------------------------
// --flips: between two static targets every cell makes exactly (t - c) mod N
// flips, counted off the picture.
//
// Read: the top row of each cell's top half, each frame. The flap in the air
// uncovers it a third of the way through its fall and the next flap covers
// it again only when it has itself left, so each flap on the drum shows
// there for one flip time -- twelve frames -- and a change of the tone it
// shows is one flip.
//---------------------------------------------------------------------------
void checkFlips( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		constexpr int columns = 8, rows = 4, flaps = 8;
		Session s;
		s.plugin.SetDebugForTest( debug );
		if( !s.init() )
			return;
		plainBoard( s, columns, rows, flaps );
		const CellProbe probe { r.width, r.height, columns, rows };

		auto toneA = []( int col, int row ) { return static_cast< float >( ( col + row ) % flaps ) / ( flaps - 1 ); };
		auto toneB = []( int col, int row ) { return static_cast< float >( ( col * 3 + row * 5 + 2 ) % flaps ) / ( flaps - 1 ); };

		//From flap 0 to A takes up to seven flips: let it, and check it.
		s.setInput( cellCard( r.width, r.height, columns, rows, toneA ) );
		Image img       = s.frames( static_cast< int >( 7 * kFlipSeconds * 60 ) + 10 );
		int wrongA      = 0;
		for( int row = 0; row < rows; ++row )
			for( int col = 0; col < columns; ++col )
				if( toneIndex( img.at( probe.x( col ), probe.top( row ) ), flaps ) != nearestFlap( toneA( col, row ), flaps ) )
					++wrongA;
		report( wrongA == 0, "%dx%d: every cell reached its first target (%d wrong)", r.width, r.height, wrongA );

		//Now B, counting transitions of the top row.
		s.setInput( cellCard( r.width, r.height, columns, rows, toneB ) );
		std::vector< int > shown( static_cast< size_t >( columns * rows ) ), flips( static_cast< size_t >( columns * rows ), 0 );
		for( int row = 0; row < rows; ++row )
			for( int col = 0; col < columns; ++col )
				shown[ static_cast< size_t >( row * columns + col ) ] = nearestFlap( toneA( col, row ), flaps );

		const int run = static_cast< int >( 7 * kFlipSeconds * 60 ) + 10;
		for( int f = 0; f < run; ++f )
		{
			img = s.frame();
			for( int row = 0; row < rows; ++row )
				for( int col = 0; col < columns; ++col )
				{
					const size_t i = static_cast< size_t >( row * columns + col );
					const int now  = toneIndex( img.at( probe.x( col ), probe.top( row ) ), flaps );
					if( now != shown[ i ] )
					{
						++flips[ i ];
						shown[ i ] = now;
					}
				}
		}

		int wrongCount = 0, wrongFinal = 0, wrongBottom = 0;
		for( int row = 0; row < rows; ++row )
			for( int col = 0; col < columns; ++col )
			{
				const size_t i = static_cast< size_t >( row * columns + col );
				const int a    = nearestFlap( toneA( col, row ), flaps );
				const int b    = nearestFlap( toneB( col, row ), flaps );
				const int want = ( ( b - a ) % flaps + flaps ) % flaps;
				if( flips[ i ] != want )
				{
					++wrongCount;
					if( wrongCount <= 4 )
						std::printf( "        cell (%d,%d): %d -> %d wants %d flips, saw %d\n", col, row, a, b, want, flips[ i ] );
				}
				if( shown[ i ] != b )
					++wrongFinal;
				if( toneIndex( img.at( probe.x( col ), probe.bottom( row ) ), flaps ) != b )
					++wrongBottom;
			}
		report( wrongCount == 0, "%dx%d: every cell made (t - c) mod %d flips, off the top row (%d of %d wrong)", r.width, r.height, flaps, wrongCount, columns * rows );
		report( wrongFinal == 0 && wrongBottom == 0, "%dx%d: every cell settled on its target, both halves (%d, %d wrong)", r.width, r.height, wrongFinal, wrongBottom );
	}
}

//---------------------------------------------------------------------------
// --asymmetry: dark -> light, one step brighter settles in one flip time and
// one step darker in N - 1.
//
// "Settles" is read off the picture: the first frame from which every later
// frame of the run is bit-identical and shows the target. With no bounce the
// landed flap is at exactly pi, so the picture stops the frame it lands.
// The landing frame: the flap in the air has phase (j + 1) / 12 on the j-th
// frame after the change, so the d-th flap lands on frame 12 d - 1. One
// frame either way is allowed for the float accumulation of 1/12 twelve
// times, which need not reach 1.0 exactly.
//---------------------------------------------------------------------------
int settleFrame( const std::vector< Image >& run, const std::function< bool( const Image& ) >& atTarget )
{
	//Find the last frame that differs from the final one.
	int last = -1;
	for( int i = 0; i < static_cast< int >( run.size() ); ++i )
		if( !( run[ static_cast< size_t >( i ) ] == run.back() ) )
			last = i;
	if( !atTarget( run.back() ) )
		return -1;
	return last + 1;
}

void checkAsymmetry( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		constexpr int columns = 4, rows = 3, flaps = 8;
		Session s;
		s.plugin.SetDebugForTest( debug );
		if( !s.init() )
			return;
		plainBoard( s, columns, rows, flaps );
		const CellProbe probe { r.width, r.height, columns, rows };
		const int framesPerFlip = static_cast< int >( std::lround( kFlipSeconds * 60 ) );

		auto allShow = [ & ]( const Image& img, int flap ) {
			for( int row = 0; row < rows; ++row )
				for( int col = 0; col < columns; ++col )
					if( toneIndex( img.at( probe.x( col ), probe.top( row ) ), flaps ) != flap
					    || toneIndex( img.at( probe.x( col ), probe.bottom( row ) ), flaps ) != flap )
						return false;
			return true;
		};
		auto flat = [ & ]( int flap ) { return cellCard( r.width, r.height, columns, rows, [ = ]( int, int ) { return static_cast< float >( flap ) / ( flaps - 1 ); } ); };

		s.setInput( flat( 3 ) );
		s.frames( 3 * framesPerFlip + 10 );

		auto measure = [ & ]( int flap, int expectedFlips ) {
			s.setInput( flat( flap ) );
			std::vector< Image > run;
			for( int f = 0; f < expectedFlips * framesPerFlip + 20; ++f )
				run.push_back( s.frame() );
			return settleFrame( run, [ & ]( const Image& img ) { return allShow( img, flap ); } );
		};

		const int up   = measure( 4, 1 );
		const int down = measure( 3, flaps - 1 );
		//The d-th flap lands on frame 12 d - 1 after the change (see above).
		const int wantUp = framesPerFlip - 1, wantDown = ( flaps - 1 ) * framesPerFlip - 1;
		report( up >= 0 && std::abs( up - wantUp ) <= 1, "%dx%d: one step brighter settled on frame %d after the change (one flip: %d, +-1)", r.width, r.height, up, wantUp );
		report( down >= 0 && std::abs( down - wantDown ) <= 1, "%dx%d: one step darker settled on frame %d (N - 1 = %d flips: %d, +-1)", r.width, r.height, down, flaps - 1, wantDown );
	}
}

//---------------------------------------------------------------------------
// --settle: a still input lands every cell on its nearest flap within
// max flips x flip time + stagger + the flutter, and then the picture is
// bit-identical frame to frame.
//---------------------------------------------------------------------------
void checkSettle( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		constexpr int columns = 16, rows = 9, flaps = 8;
		Session s;
		s.plugin.SetDebugForTest( debug );
		if( !s.init() )
			return;
		plainBoard( s, columns, rows, flaps );
		s.set( "Stagger", 0.3f );//0.3 s
		s.set( "Bounce", 0.4f );
		const CellProbe probe { r.width, r.height, columns, rows };

		//A tone per cell near a flap but not on it, never within 0.04 of a
		//midpoint (the flaps are 1/7 apart), so "nearest" is unambiguous even
		//against the sixteen-tap mean.
		auto tone = []( int col, int row ) {
			const uint32_t h = drum::Hash( static_cast< uint32_t >( col * 131 + row * 7919 + 17 ) );
			const int flap   = static_cast< int >( h % flaps );
			const float d    = ( static_cast< float >( ( h >> 8 ) & 0xff ) / 255.0f - 0.5f ) * 0.06f;
			return std::min( std::max( static_cast< float >( flap ) / ( flaps - 1 ) + d, 0.0f ), 1.0f );
		};
		s.setInput( cellCard( r.width, r.height, columns, rows, tone ) );

		//The stagger is measured in seconds; the flutter's tail from the curve.
		const double tail  = ( s.plugin.CurveForTest().bounceEnd() - 1.0 ) * kFlipSeconds;
		const double bound = ( flaps - 1 ) * kFlipSeconds + 0.3 + tail;
		const int boundFrames = static_cast< int >( std::ceil( bound * 60.0 ) ) + 2;

		Image settled = s.frames( boundFrames );
		int wrong     = 0;
		for( int row = 0; row < rows; ++row )
			for( int col = 0; col < columns; ++col )
				if( toneIndex( settled.at( probe.x( col ), probe.top( row ) ), flaps ) != nearestFlap( tone( col, row ), flaps ) )
					++wrong;
		report( wrong == 0, "%dx%d: every cell on its nearest flap after %d frames (7 flips + 0.3 s stagger + %.2f s flutter); %d wrong", r.width, r.height, boundFrames, tail, wrong );

		int moved = 0;
		for( int f = 0; f < 30; ++f )
			if( !( s.frame() == settled ) )
				++moved;
		report( moved == 0, "%dx%d: and then bit-identical for 30 frames (%d differed)", r.width, r.height, moved );
	}
}

//---------------------------------------------------------------------------
// The harness's own solution of the flap, independent of Flap.cpp.
//
// A different integrator (velocity Verlet, a fifth of Flap.cpp's step) on
// the same physics: theta'' = sin theta from three degrees at rest, the
// stop at pi with restitution e, time in flip units so that the first
// arrival at pi is u = 1. It exists so that --fall compares the picture
// against a solution the plugin never saw. Against the plugin's own table
// the check would only prove the rasteriser can read a texture -- which is
// exactly what happened before this was written: both negative controls
// perturbed the plugin's table and the shape comparison followed it.
//---------------------------------------------------------------------------
struct HarnessCurve
{
	std::vector< double > theta;///< per 1e-4 flip units, from release
	double bounceEnd = 1.0;

	explicit HarnessCurve( double e )
	{
		constexpr double kPi = 3.14159265358979323846;
		const double h       = 2.0e-5;
		auto accel           = []( double th ) { return std::sin( th ); };
		//First the fall alone, for the scaling.
		double th = 3.0 * kPi / 180.0, w = 0.0, t = 0.0, fall = 0.0;
		{
			double a = accel( th );
			for( ;; )
			{
				const double thPrev = th;
				th += w * h + 0.5 * a * h * h;
				const double aNext = accel( th );
				w += 0.5 * ( a + aNext ) * h;
				a = aNext;
				t += h;
				if( th >= kPi )
				{
					fall = t - h + h * ( kPi - thPrev ) / ( th - thPrev );
					break;
				}
			}
		}
		//Then the whole thing with the stop, sampled every 1e-4 flip units.
		th = 3.0 * kPi / 180.0;
		w  = 0.0;
		t  = 0.0;
		double a          = accel( th );
		const double span = 4.0 * fall;
		double nextSample = 0.0;
		bool pinned       = false;
		while( t < span )
		{
			while( nextSample <= t + 1e-12 && theta.size() < 40001 )
			{
				theta.push_back( pinned ? kPi : std::min( th, kPi ) );
				nextSample += 1.0e-4 * fall;
			}
			if( pinned )
			{
				t += h;
				continue;
			}
			const double thPrev = th, wPrev = w;
			th += w * h + 0.5 * a * h * h;
			const double aNext = accel( th );
			w += 0.5 * ( a + aNext ) * h;
			a = aNext;
			t += h;
			if( th >= kPi )
			{
				const double f    = ( kPi - thPrev ) / ( th - thPrev );
				const double wHit = -e * ( wPrev + f * ( w - wPrev ) );
				th                = kPi;
				w                 = wHit;
				a                 = accel( th );
				if( -wHit < 0.5 * kPi / 180.0 )
				{
					pinned    = true;
					bounceEnd = ( t - h + f * h ) / fall;
				}
			}
		}
		while( theta.size() < 40001 )
			theta.push_back( kPi );
	}

	double angleAt( double u ) const
	{
		const double f = std::min( std::max( u, 0.0 ), 4.0 ) * 1.0e4;
		const size_t i = std::min( static_cast< size_t >( f ), theta.size() - 2 );
		const double w = f - static_cast< double >( i );
		return theta[ i ] + ( theta[ i + 1 ] - theta[ i ] ) * w;
	}
};

//---------------------------------------------------------------------------
// --fall: the flap's angle over time, from the rendered flap's projected
// height, against the plugin's double-precision solution; and the fall takes
// one flip time.
//
// The smallest board (4 x 2), three flaps (0.06, 0.53 and 1.0 grey), a
// one-second flip from
// the middle one to the white one. The flap in the air is measured as the
// rows from the hinge that show it rather than the half behind it: in the
// top half its mid-grey front over the white next symbol, in the bottom half
// its white back over the landed mid-grey flap. A row is covered when its
// centre is under L |cos theta|, so the count is floor( L |cos theta| + 0.5 )
// and the tolerance is one pixel: half a pixel from the centre rule, the
// rest for the rasteriser.
//---------------------------------------------------------------------------
void checkFall( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		Session s;
		s.plugin.SetDebugForTest( debug );
		if( !s.init() )
			return;
		//The smallest board the controls allow, 4 x 2, and the top-left cell.
		plainBoard( s, kColumnsMin, kRowsMin, 3 );
		s.set( "Flip Time", FlipTimeToParam( 1.0f ) );
		//The light 60 degrees from above. Then the white back of a flap past
		//the horizontal is lit at 0.65 or more all the way to the stop, the
		//plate at rest is lit at 0.65, and the mid-grey front of a flap in the
		//air at 0.34 or less: 127 separates a flap from what is behind it at
		//every angle. Lit straight on, the back at 100 degrees would be as
		//dark as the mid-grey plate it is over.
		s.set( "Light Angle", 1.0f );
		const int cellH = r.height / kRowsMin;
		const int hinge = cellH / 2;//first row of the bottom half, top-down
		const int L     = cellH / 2;
		const int x     = r.width / kColumnsMin / 2;

		auto flat = [ & ]( float t ) { return cellCard( r.width, r.height, 1, 1, [ = ]( int, int ) { return t; } ); };
		//From flap 0 to the middle flap first: one flip, one second.
		s.setInput( flat( 0.5f ) );
		s.frames( 80 );

		//Rows above the hinge that are not the white next symbol.
		auto topCovered = [ & ]( const Image& img ) {
			int n = 0;
			for( int y = hinge - 1; y >= 0; --y, ++n )
				if( img.at( x, y ) > 127 )
					break;
			return n;
		};
		//Rows below the hinge that are white: the back of a flap.
		auto bottomCovered = [ & ]( const Image& img ) {
			int n = 0;
			for( int y = hinge; y < cellH; ++y, ++n )
				if( img.at( x, y ) <= 127 )
					break;
			return n;
		};

		const HarnessCurve curve( 0.0 );
		s.setInput( flat( 1.0f ) );
		double worst = 0.0;
		int worstAt  = -1;
		int landed   = -1;
		int compared = 0;
		for( int j = 0; j < 75; ++j )
		{
			const Image img = s.frame();
			//Landed: the bottom half is all white and nothing is in the air.
			if( landed < 0 && bottomCovered( img ) == L && topCovered( img ) == 0 )
			{
				landed = j;
				break;
			}
			const float p     = static_cast< float >( j + 1 ) / 60.0f;
			const double theta = curve.angleAt( p );
			const double h    = L * std::fabs( std::cos( theta ) );
			const int got     = theta < 1.5707963 ? topCovered( img ) : bottomCovered( img );
			const double d    = std::fabs( got - h );
			++compared;
			if( d > worst )
			{
				worst   = d;
				worstAt = j;
			}
			if( std::getenv( "SFTEST_DUMP" ) )
				writePng( std::string( std::getenv( "SFTEST_DUMP" ) ) + "/fall-" + std::to_string( r.width ) + "-" + std::to_string( j ) + ".png", r.width, r.height, img.px );
			if( d > 1.0 && std::getenv( "SFTEST_VERBOSE" ) )
				std::printf( "        frame %2d  p %.4f  theta %6.2f deg  model %6.2f px  drawn %3d px\n", j, p, theta * 57.29578, h, got );
		}
		report( worst <= 1.0, "%dx%d: flap height within 1 px of L|cos theta(t)| over %d frames (worst %.2f px on frame %d)", r.width, r.height, compared, worst, worstAt );
		report( landed >= 0 && std::abs( landed - 59 ) <= 1, "%dx%d: a one-second flip landed on frame %d (stated: 59, +-1)", r.width, r.height, landed );

		//The flutter, with the bounce up: the landed flap lifts from the stop
		//and the rows it uncovers follow the same curve past u = 1.
		s.set( "Bounce", 1.0f );
		s.setInput( flat( 0.0f ) );
		s.frames( 80 );//round the drum to 0, then to the middle
		s.setInput( flat( 0.5f ) );
		s.frames( 80 );
		s.setInput( flat( 1.0f ) );
		landed = -1;
		for( int j = 0; j < 75 && landed < 0; ++j )
		{
			const Image img = s.frame();
			if( topCovered( img ) == 0 && bottomCovered( img ) == L )
				landed = j;
		}
		const HarnessCurve bouncy( 0.6 );//Bounce at 1.0 is e = 0.6 (Controls.cpp)
		double worstB = 0.0;
		int comparedB = 0;
		if( landed >= 0 )
		{
			const double overshoot = static_cast< double >( landed + 1 ) / 60.0 - 1.0;
			for( int m = 1; m <= 40; ++m )
			{
				const Image img    = s.frame();
				const double since = overshoot + m / 60.0;
				const double theta = bouncy.angleAt( 1.0 + since / 1.0 );
				const double h     = L * std::fabs( std::cos( theta ) );
				const double d     = std::fabs( bottomCovered( img ) - h );
				++comparedB;
				worstB = std::max( worstB, d );
			}
		}
		report( landed >= 0 && worstB <= 1.0, "%dx%d: the flutter after landing within 1 px of the curve over %d frames (worst %.2f px; rises to %.1f deg)", r.width, r.height, comparedB, worstB,
		        landed >= 0 ? 180.0 - bouncy.angleAt( 1.0 + 0.4 ) * 180.0 / 3.14159265 : 0.0 );
	}
}

//---------------------------------------------------------------------------
// --resize: a picture resize mid-flip keeps every cell's flap index, and so
// does a change of Columns. Read as --flips does, across the change: the
// count before plus the count after is still (t - c) mod N.
//---------------------------------------------------------------------------
void checkResize( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		constexpr int columns = 8, rows = 4, flaps = 8;
		const Raster other = r.width == 640 ? Raster { 320, 180 } : Raster { 640, 360 };

		auto toneB = []( int col, int row ) { return static_cast< float >( ( col * 3 + row * 5 + 2 ) % flaps ) / ( flaps - 1 ); };

		//(a) the picture resizes.
		{
			Session s;
			s.plugin.SetDebugForTest( debug );
			if( !s.init() )
				return;
			plainBoard( s, columns, rows, flaps );
			s.setInput( cellCard( r.width, r.height, columns, rows, []( int, int ) { return 0.0f; } ) );
			s.frames( 5 );

			std::vector< int > shown( static_cast< size_t >( columns * rows ), 0 ), flips( static_cast< size_t >( columns * rows ), 0 );
			auto count = [ & ]( const Image& img, const CellProbe& probe ) {
				for( int row = 0; row < rows; ++row )
					for( int col = 0; col < columns; ++col )
					{
						const size_t i = static_cast< size_t >( row * columns + col );
						const int now  = toneIndex( img.at( probe.x( col ), probe.top( row ) ), flaps );
						if( now != shown[ i ] )
						{
							++flips[ i ];
							shown[ i ] = now;
						}
					}
			};

			const CellProbe before { r.width, r.height, columns, rows };
			const CellProbe after { other.width, other.height, columns, rows };
			s.setInput( cellCard( r.width, r.height, columns, rows, toneB ) );
			for( int f = 0; f < 30; ++f )
				count( s.frame(), before );
			s.setInput( cellCard( other.width, other.height, columns, rows, toneB ) );
			for( int f = 0; f < 100; ++f )
				count( s.frame(), after );

			int wrong = 0;
			for( int row = 0; row < rows; ++row )
				for( int col = 0; col < columns; ++col )
				{
					const size_t i = static_cast< size_t >( row * columns + col );
					if( flips[ i ] != nearestFlap( toneB( col, row ), flaps ) || shown[ i ] != nearestFlap( toneB( col, row ), flaps ) )
						++wrong;
				}
			report( wrong == 0, "%dx%d -> %dx%d on frame 30 of a run: every cell kept its flap and finished its count (%d wrong)", r.width, r.height, other.width, other.height, wrong );
		}

		//(b) the grid changes: 8 columns become 16, and each child cell
		//inherits its parent's flap, so its count from the start is still the
		//parent's (t - c) mod N.
		{
			Session s;
			s.plugin.SetDebugForTest( debug );
			if( !s.init() )
				return;
			plainBoard( s, columns, rows, flaps );
			s.setInput( cellCard( r.width, r.height, columns, rows, []( int, int ) { return 0.0f; } ) );
			s.frames( 5 );

			std::vector< int > shown( static_cast< size_t >( 2 * columns * rows ), 0 ), flips( static_cast< size_t >( 2 * columns * rows ), 0 );
			const CellProbe before { r.width, r.height, columns, rows };
			const CellProbe after { r.width, r.height, 2 * columns, rows };
			s.setInput( cellCard( r.width, r.height, columns, rows, toneB ) );
			for( int f = 0; f < 30; ++f )
			{
				const Image img = s.frame();
				for( int row = 0; row < rows; ++row )
					for( int col = 0; col < columns; ++col )
					{
						const int now = toneIndex( img.at( before.x( col ), before.top( row ) ), flaps );
						for( int child = 0; child < 2; ++child )
						{
							const size_t i = static_cast< size_t >( row * 2 * columns + 2 * col + child );
							if( now != shown[ i ] )
							{
								++flips[ i ];
								shown[ i ] = now;
							}
						}
					}
			}
			s.set( "Columns", static_cast< float >( 2 * columns ) );
			for( int f = 0; f < 100; ++f )
			{
				const Image img = s.frame();
				for( int row = 0; row < rows; ++row )
					for( int col = 0; col < 2 * columns; ++col )
					{
						const size_t i = static_cast< size_t >( row * 2 * columns + col );
						const int now  = toneIndex( img.at( after.x( col ), after.top( row ) ), flaps );
						if( now != shown[ i ] )
						{
							++flips[ i ];
							shown[ i ] = now;
						}
					}
			}
			int wrong = 0;
			for( int row = 0; row < rows; ++row )
				for( int col = 0; col < 2 * columns; ++col )
				{
					const size_t i = static_cast< size_t >( row * 2 * columns + col );
					const int want = nearestFlap( toneB( col / 2, row ), flaps );
					if( flips[ i ] != want || shown[ i ] != want )
						++wrong;
				}
			report( wrong == 0, "%dx%d: 8 -> 16 columns on frame 30 of a run: every child cell inherited its parent's flap and finished its count (%d wrong)", r.width, r.height, wrong );
		}
	}
}

//---------------------------------------------------------------------------
// --prime: in Onset mode, loud audio already playing when the clip starts
// fires no update; a real onset later does.
//---------------------------------------------------------------------------
void checkPrime( const SplitflapPlugin::Debug& debug )
{
	for( const Raster& r : kRasters )
	{
		constexpr int columns = 4, rows = 3, flaps = 8;
		Session s;
		s.plugin.SetDebugForTest( debug );
		if( !s.init() )
			return;
		plainBoard( s, columns, rows, flaps );
		s.set( "Update", static_cast< float >( kUpdateOnset ) );
		const CellProbe probe { r.width, r.height, columns, rows };
		auto flat = [ & ]( int flap ) { return cellCard( r.width, r.height, columns, rows, [ = ]( int, int ) { return static_cast< float >( flap ) / ( flaps - 1 ); } ); };

		auto anyOff = [ & ]( const Image& img, int flap ) {
			for( int row = 0; row < rows; ++row )
				for( int col = 0; col < columns; ++col )
					if( toneIndex( img.at( probe.x( col ), probe.top( row ) ), flaps ) != flap )
						return true;
			return false;
		};

		s.setAudio( 0.5f );
		s.setInput( flat( 0 ) );
		s.frame();//frame 0: the clip starts into loud audio; the board latches flap 0
		int fired = s.plugin.OnsetFiredForTest() ? 1 : 0;//a fire on frame 0 is the trap itself
		int moved = 0;
		s.setInput( flat( 5 ) );
		for( int f = 1; f <= 60; ++f )
		{
			const Image img = s.frame();
			if( s.plugin.OnsetFiredForTest() )
				++fired;
			if( anyOff( img, 0 ) )
				++moved;
		}
		report( fired == 0 && moved == 0, "%dx%d: steady loud audio from frame 0: no onset and no flip in 60 frames (fired %d, moved on %d)", r.width, r.height, fired, moved );

		s.setAudio( 1.0f );
		int firedAt = -1;
		for( int f = 0; f < 5 * 12 + 5; ++f )
		{
			s.frame();
			if( firedAt < 0 && s.plugin.OnsetFiredForTest() )
				firedAt = f;
		}
		const Image after = s.frame();
		report( firedAt == 0 && !anyOff( after, 5 ), "%dx%d: a real onset fired on its frame (%d) and the board reached flap 5", r.width, r.height, firedAt );
	}
}

//---------------------------------------------------------------------------
// --negative: every check against a model with one thing broken, and it has
// to notice.
//---------------------------------------------------------------------------
int runNegative()
{
	struct Case
	{
		const char* name;
		SplitflapPlugin::Debug debug;
		void ( *check )( const SplitflapPlugin::Debug& );
	};
	SplitflapPlugin::Debug backward;
	backward.allowBackward = true;
	SplitflapPlugin::Debug clear;
	clear.clearOnResize = true;
	SplitflapPlugin::Debug unprimed;
	unprimed.noPrime = true;
	SplitflapPlugin::Debug constant;
	constant.constantAccel = true;
	SplitflapPlugin::Debug heavy;
	heavy.gravityScale = 1.15;

	const Case cases[] = {
		{ "--flips with backward flips allowed", backward, checkFlips },
		{ "--asymmetry with backward flips allowed", backward, checkAsymmetry },
		{ "--resize with the state cleared on a resize", clear, checkResize },
		{ "--prime with the detector unprimed", unprimed, checkPrime },
		{ "--fall with theta'' = constant", constant, checkFall },
		{ "--fall with gravity 15% high", heavy, checkFall },
	};

	int missed = 0;
	for( const Case& c : cases )
	{
		std::printf( "negative: %s\n", c.name );
		const int before = gFailures;
		c.check( c.debug );
		const int caught = gFailures - before;
		gFailures        = before;
		std::printf( "  %s  %d assertion(s) failed, as they must\n\n", caught > 0 ? "ok  " : "MISS", caught );
		if( caught == 0 )
			++missed;
	}
	std::printf( "negative controls: %d of %zu caught\n", static_cast< int >( sizeof( cases ) / sizeof( cases[ 0 ] ) ) - missed, sizeof( cases ) / sizeof( cases[ 0 ] ) );
	return missed == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// --list, --names, --font
//---------------------------------------------------------------------------
int runList()
{
	SplitflapPlugin plugin;
	std::printf( "%-4s %-22s %-9s %10s   %-16s\n", "id", "name", "kind", "value", "range" );
	for( unsigned id = 0; id < plugin.GetNumParams(); ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( id >= PT_ABOUT_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "about", "-", "-" );
			continue;
		}
		const unsigned type = plugin.GetParamType( id );
		if( type == FF_TYPE_BUFFER )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "buffer", "-", "-" );
			continue;
		}
		if( type == FF_TYPE_TEXT )
		{
			std::printf( "%-4u %-22s %-9s %10s   %-16s\n", id, name ? name : "", "text", "-", plugin.GetTextParameter( id ) );
			continue;
		}
		const char* kind = "standard";
		char rangeText[ 32 ];
		float lo = 0.0f, hi = 1.0f;
		switch( type )
		{
		case FF_TYPE_BOOLEAN: kind = "boolean"; break;
		case FF_TYPE_EVENT: kind = "event"; break;
		case FF_TYPE_INTEGER:
		{
			kind                = "integer";
			const RangeStruct r = plugin.GetParamRange( id );
			lo                  = r.min;
			hi                  = r.max;
			break;
		}
		case FF_TYPE_OPTION:
			//An option's SDK range reads back 0..1 whatever its element count;
			//the real range is the index of its last element.
			kind = "option";
			hi   = static_cast< float >( plugin.GetNumParamElements( id ) - 1 );
			break;
		case FF_TYPE_RED:
		case FF_TYPE_GREEN:
		case FF_TYPE_BLUE: kind = "colour"; break;
		default: break;
		}
		std::snprintf( rangeText, sizeof( rangeText ), "[ %g .. %g ]", lo, hi );
		std::printf( "%-4u %-22s %-9s %10.4f   %-16s\n", id, name ? name : "", kind, plugin.GetFloatParameter( id ), rangeText );
	}
	return 0;
}

int runNames()
{
	SplitflapPlugin plugin;
	int bad = 0;
	std::vector< std::string > seen;
	std::printf( "names longer than FFGL's 16 characters, and duplicates:\n\n" );
	for( unsigned id = 0; id < plugin.GetNumParams(); ++id )
	{
		const char* name = plugin.GetParamName( id );
		if( !name )
			continue;
		if( std::strlen( name ) > 16 )
		{
			std::printf( "  %-3u  %-28s %zu characters\n", id, name, std::strlen( name ) );
			++bad;
		}
		if( std::find( seen.begin(), seen.end(), name ) != seen.end() )
		{
			std::printf( "  %-3u  %-28s is a duplicate\n", id, name );
			++bad;
		}
		seen.push_back( name );
		for( unsigned e = 0; e < plugin.GetNumParamElements( id ); ++e )
		{
			const char* el = plugin.GetParamElementName( id, e );
			if( el && std::strlen( el ) > 16 )
			{
				std::printf( "  %-3u  %-28s element %u: %s\n", id, name, e, el );
				++bad;
			}
		}
	}
	std::printf( "\n  %d problem(s)\n", bad );
	return bad == 0 ? 0 : 1;
}

int runFont()
{
	std::printf( "the text drum's %d flaps: %s\n\n", drum::AlphabetSize(), drum::kAlphabet );
	for( const char* c = drum::kAlphabet; *c; ++c )
	{
		std::printf( "%c (%d)\n", *c, *c );
		for( int y = 0; y < font::kHeight; ++y )
			std::printf( "    %s\n", font::Glyph( *c )[ y ] );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
int runBench()
{
	if( !openGL() )
		return 1;
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	std::printf( "60 frames each after a 20-frame warm-up, glFinish both sides, the largest grid the controls allow (%d x %d), the test card.\n\n",
	             kColumnsMax, kRowsMax );
	std::printf( "resolution   ms/frame   %% of a 60 fps frame\n" );
	for( const Size& size : sizes )
	{
		Session s;
		if( !s.init() )
			return 1;
		s.set( "Columns", static_cast< float >( kColumnsMax ) );
		s.set( "Rows", static_cast< float >( kRowsMax ) );
		s.setInput( card( size.width, size.height ) );
		s.frames( 20 );
		glFinish();
		const auto start = std::chrono::steady_clock::now();
		s.frames( 60 );
		glFinish();
		const double ms = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() * 1000.0 / 60.0;
		std::printf( "%s   %7.3f       %5.1f%%\n", size.name, ms, ms / 16.667 * 100.0 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --out and --pipe share the cue script.
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
			continue;
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

/// A standard parameter ramps between keys. Anything with discrete meaning
/// steps: the value of the latest key at or before the frame.
float valueAt( const Track& track, int frame, bool step )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( step )
				return frame >= b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

struct RunOptions
{
	int width = 640, height = 360;
	int frames  = 120;
	double fps  = 60.0;
	float audio = 0.0f;
	float drift = 0.0f;///< pixels the test card moves left per frame, so a still run has a moving picture
	std::string scriptPath, outPath;
	std::vector< std::pair< std::string, std::string > > sets;
};

struct Cues
{
	struct Bound
	{
		unsigned id;
		bool step;
		Track track;
	};
	std::vector< Bound > bound;

	bool bind( Session& s, const RunOptions& o, std::string& error )
	{
		std::map< std::string, unsigned > byName;
		for( unsigned id = 0; id < PT_ABOUT_TEXT; ++id )
			if( id != PT_AUDIO )
				if( const char* name = s.plugin.GetParamName( id ) )
					byName[ name ] = id;
		for( const auto& kv : o.sets )
		{
			const auto found = byName.find( kv.first );
			if( found == byName.end() )
			{
				error = "no parameter named '" + kv.first + "' (try --list)";
				return false;
			}
			if( s.plugin.GetParamType( found->second ) == FF_TYPE_TEXT )
				s.plugin.SetTextParameter( found->second, kv.second.c_str() );
			else
				s.plugin.SetFloatParameter( found->second, static_cast< float >( std::atof( kv.second.c_str() ) ) );
		}
		if( o.scriptPath.empty() )
			return true;
		const auto tracks = loadScript( o.scriptPath, error );
		if( !error.empty() )
			return false;
		for( const auto& entry : tracks )
		{
			const auto found = byName.find( entry.first );
			if( found == byName.end() )
			{
				error = "the script names \"" + entry.first + "\", which is not an automatable parameter (try --list)";
				return false;
			}
			const unsigned type = s.plugin.GetParamType( found->second );
			const bool step     = type == FF_TYPE_OPTION || type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT || type == FF_TYPE_INTEGER;
			bound.push_back( { found->second, step, entry.second } );
		}
		return true;
	}

	void apply( Session& s, int frame )
	{
		for( const Bound& b : bound )
		{
			const float v = valueAt( b.track, frame, b.step );
			if( s.plugin.GetParamType( b.id ) == FF_TYPE_EVENT )
			{
				//A press is a key with value 1 ON that frame, and a release the
				//frame after; a ramp never reaches here.
				bool pressed = false;
				for( const auto& key : b.track )
					pressed = pressed || ( key.first == frame && key.second >= 0.5f );
				s.plugin.SetFloatParameter( b.id, pressed ? 1.0f : 0.0f );
			}
			else
				s.plugin.SetFloatParameter( b.id, v );
		}
	}
};

int runOut( const RunOptions& o )
{
	if( !openGL() )
		return 1;
	Session s;
	if( !s.init() )
		return 1;
	Cues cues;
	std::string error;
	if( !cues.bind( s, o, error ) )
	{
		std::fprintf( stderr, "sftest: %s\n", error.c_str() );
		return 2;
	}
	s.fps = o.fps;
	s.setAudio( o.audio );
	const Image still = card( o.width, o.height );
	s.setInput( still );
	Image img;
	for( int f = 0; f < std::max( 1, o.frames ); ++f )
	{
		if( o.drift != 0.0f )
		{
			//The card, wrapped round by drift * f pixels: a moving picture for
			//the controls that only mean anything while the picture moves.
			Image moved = still;
			const int shift = static_cast< int >( std::lround( o.drift * f ) ) % o.width;
			for( int y = 0; y < o.height; ++y )
				for( int x = 0; x < o.width; ++x )
				{
					const int from = ( ( x + shift ) % o.width + o.width ) % o.width;
					std::memcpy( &moved.px[ ( static_cast< size_t >( y ) * o.width + x ) * 4 ],
					             &still.px[ ( static_cast< size_t >( y ) * o.width + from ) * 4 ], 4 );
				}
			s.setInput( moved );
		}
		cues.apply( s, f );
		img = s.frame();
	}
	if( !writePng( o.outPath, o.width, o.height, img.px ) )
	{
		std::fprintf( stderr, "sftest: could not write %s\n", o.outPath.c_str() );
		return 1;
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", o.outPath.c_str(), o.width, o.height, o.frames );
	return 0;
}

int runPipe( const RunOptions& o )
{
	// A reader that hangs up must end the take with exit 1 and a message,
	// not SIGPIPE's silent 141: write() then fails and the loop says so.
	std::signal( SIGPIPE, SIG_IGN );
	if( o.width <= 0 || o.height <= 0 || !( o.fps > 0.0 ) )
	{
		std::fprintf( stderr, "sftest: --pipe needs a positive size and --fps\n" );
		return 1;
	}
	if( !openGL() )
		return 1;
	Session s;
	if( !s.init() )
		return 1;
	Cues cues;
	std::string error;
	if( !cues.bind( s, o, error ) )
	{
		std::fprintf( stderr, "sftest: %s\n", error.c_str() );
		return 2;
	}
	s.fps = o.fps;
	s.setAudio( o.audio );
	s.resize( o.width, o.height );

	const size_t bytes = static_cast< size_t >( o.width ) * static_cast< size_t >( o.height ) * 4u;
	Image in;
	in.width  = o.width;
	in.height = o.height;
	in.px.resize( bytes );
	int status   = 0;
	bool noInput = false;
	for( int f = 0; o.frames <= 0 || f < o.frames; ++f )
	{
		if( !noInput )
		{
			size_t filled = 0;
			while( filled < bytes )
			{
				const ssize_t got = read( STDIN_FILENO, in.px.data() + filled, bytes - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			if( filled == bytes )
				s.setInput( in );
			else if( f == 0 && filled == 0 && o.frames > 0 )
			{
				//No input at all: film the test card for --frames, so a pipe can
				//be tested without a source.
				noInput = true;
				s.setInput( card( o.width, o.height ) );
			}
			else
				break;//the source ran out, whole frames only
		}
		cues.apply( s, f );
		const Image out = s.frame();
		size_t written  = 0;
		while( written < bytes )
		{
			const ssize_t put = write( STDOUT_FILENO, out.px.data() + written, bytes - written );
			if( put <= 0 )
				break;
			written += static_cast< size_t >( put );
		}
		if( written < bytes )
		{
			std::fprintf( stderr, "sftest: the reader hung up after %d frame(s)\n", f );
			status = 1;
			break;
		}
	}
	return status;
}

void usage()
{
	std::printf(
		"sftest -- render and check the Splitflap effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin\n"
		"  --size WxH        (default 640x360)   --frames N (default 120)   --fps N (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name (options by index; Text by its string)\n"
		"  --audio LEVEL     write LEVEL into every spectrum bin\n"
		"  --drift PX        move the test card PX pixels a frame (for --out)\n"
		"  --script PATH     cues: 'frame Parameter Name value'\n"
		"  --list | --names | --font\n"
		"  --flips | --asymmetry | --settle | --fall | --resize | --prime   the checks, at 640x360 and 320x180\n"
		"  --all             every check\n"
		"  --negative        every check against its broken model; each must fail\n"
		"  --bench           720p, 1080p and 4K at the largest grid\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n" );
}

int runCheck( const char* name, void ( *check )( const SplitflapPlugin::Debug& ) )
{
	if( !openGL() )
		return 1;
	std::printf( "%s\n", name );
	const int before = gFailures;
	check( SplitflapPlugin::Debug {} );
	const int failed = gFailures - before;
	std::printf( "  %s\n", failed == 0 ? "all ok" : "FAILURES" );
	return failed == 0 ? 0 : 1;
}
} // namespace

int main( int argc, char** argv )
{
	RunOptions o;
	bool pipe = false;
	const std::map< std::string, void ( * )( const SplitflapPlugin::Debug& ) > checks = {
		{ "--flips", checkFlips },   { "--asymmetry", checkAsymmetry }, { "--settle", checkSettle },
		{ "--fall", checkFall },     { "--resize", checkResize },       { "--prime", checkPrime },
	};

	for( int a = 1; a < argc; ++a )
	{
		const std::string arg = argv[ a ];
		auto next             = [ & ]() -> const char* { return a + 1 < argc ? argv[ ++a ] : ""; };
		if( arg == "--help" )
		{
			usage();
			return 0;
		}
		if( checks.count( arg ) )
			return runCheck( arg.c_str(), checks.at( arg ) );
		if( arg == "--all" )
		{
			int status = 0;
			for( const auto& c : checks )
				status |= runCheck( c.first.c_str(), c.second );
			return status;
		}
		if( arg == "--negative" )
			return openGL() ? runNegative() : 1;
		if( arg == "--list" )
			return runList();
		if( arg == "--names" )
			return runNames();
		if( arg == "--font" )
			return runFont();
		if( arg == "--bench" )
			return runBench();
		if( arg == "--pipe" )
			pipe = true;
		else if( arg == "--out" )
			o.outPath = next();
		else if( arg == "--size" )
		{
			const std::string v = next();
			const size_t x      = v.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "sftest: --size wants WxH\n" );
				return 2;
			}
			o.width  = std::atoi( v.substr( 0, x ).c_str() );
			o.height = std::atoi( v.substr( x + 1 ).c_str() );
		}
		else if( arg == "--width" )
			o.width = std::atoi( next() );
		else if( arg == "--height" )
			o.height = std::atoi( next() );
		else if( arg == "--frames" )
			o.frames = std::atoi( next() );
		else if( arg == "--fps" )
			o.fps = std::atof( next() );
		else if( arg == "--audio" )
			o.audio = static_cast< float >( std::atof( next() ) );
		else if( arg == "--drift" )
			o.drift = static_cast< float >( std::atof( next() ) );
		else if( arg == "--script" )
			o.scriptPath = next();
		else if( arg == "--set" )
		{
			const std::string v = next();
			const size_t eq     = v.find( '=' );
			if( eq == std::string::npos )
			{
				std::fprintf( stderr, "sftest: --set wants Name=Value\n" );
				return 2;
			}
			o.sets.emplace_back( v.substr( 0, eq ), v.substr( eq + 1 ) );
		}
		else
		{
			std::fprintf( stderr, "sftest: unknown argument %s\n", arg.c_str() );
			usage();
			return 2;
		}
	}

	if( pipe )
		return runPipe( o );
	if( o.outPath.empty() )
		o.outPath = "/tmp/splitflap.png";
	return runOut( o );
}
