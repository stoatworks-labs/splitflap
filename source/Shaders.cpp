#include "Shaders.h"

namespace splitflap
{

//---------------------------------------------------------------------------
// Vertex: straight through in 0..1 picture space. MaxUV is folded in once in
// the copy pass, and every later pass works on a texture we allocated.
//---------------------------------------------------------------------------
const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyShader = R"(#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( InputTexture, uv * MaxUV );
}
)";

//---------------------------------------------------------------------------
// Pass 2: motor. One fragment per cell of the grid.
//
// The state texel, RGBA32F:
//   r  c      the flap on show, 0..N-1
//   g  p      the fall phase of the flap in the air, 0 < p < 1; 0 when none is
//   b  t      the latched target flap
//   a  a      seconds: > 0 is the stagger delay still to wait; <= 0 is minus
//             the seconds since the last flap landed, for the flutter
//
// So: idle      t == c && p == 0
//     waiting   t != c && p == 0 && a > 0
//     falling   p > 0
//     and t != c && p == 0 && a <= 0 is a cell whose wait has ended, which
//     starts falling this frame.
//---------------------------------------------------------------------------
const char* const kMotorShader = R"(#version 410 core

uniform sampler2D PrevState;
uniform sampler2D Picture;    //the clip, mipmapped
uniform sampler2D Drum;       //N x 1: what each flap is printed with
uniform sampler2D Message;    //Columns x Rows: the Text drum's index per cell

uniform ivec2 Grid;
uniform ivec2 OldGrid;        //the previous state's size; (0,0) means no state yet
uniform float Fire;           //1 when the targets refresh this frame
uniform int DrumMode;         //0 tones, 1 colours, 2 text
uniform int Flaps;            //N
uniform float Dt;             //seconds since the last frame
uniform float FlipTime;       //seconds per flap
uniform float Stagger;        //seconds of start delay at most
uniform int ModuleWidth;      //cells in a row that share one drive
uniform vec2 BoardOrigin;     //the board's rectangle in picture uv
uniform vec2 BoardSize;
uniform float PictureLod;     //the whole mip level whose texel is at most an eighth of a cell
uniform int AllowBackward;    //negative control: the shorter way round

out vec4 fragColor;

//The PCG output permutation, exact in 32-bit integers.
uint hashInt( uint x )
{
	x = x * 747796405u + 2891336453u;
	x = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( x >> 22u ) ^ x;
}

float hash01( uint x )
{
	return float( hashInt( x ) & 0x00ffffffu ) / 16777216.0;
}

//Row 0 is the top of the board, as text reads; picture v runs upward.
vec2 cellUV( ivec2 cell, vec2 offset )
{
	vec2 f = ( vec2( cell ) + 0.5 + offset ) / vec2( Grid );
	return BoardOrigin + vec2( f.x, 1.0 - f.y ) * BoardSize;
}

//The cell's mean: sixteen bilinear taps at a whole mip level whose texel
//is no wider than an eighth of the cell, at +-1/8 and +-3/8 of the cell
//from its centre. A tap's footprint then reaches the cell's edge and no
//further, so a cell's mean is its own and not its neighbours'. Trilinear
//sampling at the cell's own level would blend a texel wider than the cell
//across the boundary.
vec4 cellMean( ivec2 cell )
{
	vec4 sum = vec4( 0.0 );
	for( int j = 0; j < 4; ++j )
		for( int i = 0; i < 4; ++i )
			sum += textureLod( Picture, cellUV( cell, vec2( -0.375 + 0.25 * float( i ), -0.375 + 0.25 * float( j ) ) ), PictureLod );
	return sum / 16.0;
}

int targetFor( ivec2 cell )
{
	vec4 m = cellMean( cell );
	float luma = dot( m.rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
	if( DrumMode == 2 )
	{
		//The message is the target where the clip is bright, the blank where it
		//is not. A quarter of full luma, not half: a cell's MEAN is low even
		//where a bright object sits on black, and at 0.5 no bundled demo clip
		//lit more than 18 of 120 cells (the release survey, 2026-09-24); at
		//0.25 the brightest lights 92 and the dark ones stay blank.
		if( luma < 0.25 )
			return 0;
		return int( texelFetch( Message, cell, 0 ).r + 0.5 );
	}
	int best = 0;
	float bestDistance = 1.0e9;
	for( int k = 0; k < Flaps; ++k )
	{
		vec4 printed = texelFetch( Drum, ivec2( k, 0 ), 0 );
		float d = DrumMode == 0 ? abs( printed.a - luma ) : distance( printed.rgb, m.rgb );
		if( d < bestDistance )
		{
			bestDistance = d;
			best = k;
		}
	}
	return best;
}

void main()
{
	ivec2 cell = ivec2( gl_FragCoord.xy );
	int N = max( Flaps, 2 );

	vec4 s;
	if( OldGrid.x <= 0 || OldGrid.y <= 0 )
	{
		//No state yet: flap 0, idle, landed long ago.
		s = vec4( 0.0, 0.0, 0.0, -100.0 );
	}
	else
	{
		//A grid that changed size keeps its picture: each new cell takes the
		//state of the old cell at the same place on the board.
		ivec2 old = ( cell * OldGrid ) / Grid;
		s = texelFetch( PrevState, clamp( old, ivec2( 0 ), OldGrid - 1 ), 0 );
	}

	int c = int( s.r + 0.5 );
	float p = s.g;
	int t = int( s.b + 0.5 );
	float a = s.a;

	//Flaps reduced live: stay on the drum.
	c = c % N;
	t = t % N;

	if( Fire > 0.5 )
	{
		int tn = targetFor( cell );
		if( tn != t )
		{
			if( c == t && p == 0.0 && tn != c )
			{
				//Idle, and asked to move: wait the stagger, decided per module
				//so a row of cells on one drive starts together.
				int module = cell.x / max( ModuleWidth, 1 ) + 4096 * cell.y;
				float wait = Stagger * hash01( uint( module ) * 2654435761u + 0x51F1u );
				//No wait leaves the flutter timer alone, so the flap that just
				//landed does not bounce again when the next target arrives.
				if( wait > 0.0 )
					a = wait;
			}
			t = tn;
		}
	}

	//How much of this frame the flap spends falling.
	float fallDt = 0.0;
	if( p > 0.0 )
		fallDt = Dt;
	else if( t != c )
	{
		if( a > 0.0 )
		{
			a -= Dt;
			if( a <= 0.0 )
			{
				fallDt = -a;
				a = -100.0;
			}
		}
		else
			fallDt = Dt;
	}

	if( p > 0.0 || ( t != c && a <= 0.0 ) )
	{
		float np = p + fallDt / FlipTime;

		//Flaps still to pass, counting the one in the air. A target equal to
		//the flap on show while one is falling means the whole drum round.
		int remaining = ( ( t - c ) % N + N ) % N;
		if( remaining == 0 )
			remaining = N;
		int dir = 1;
		if( AllowBackward == 1 && remaining < N && remaining > N / 2 )
		{
			remaining = N - remaining;
			dir = -1;
		}

		int steps = int( floor( np ) );
		if( steps >= remaining )
		{
			//Arrived. The last flap landed this many flip units ago.
			c = t;
			p = 0.0;
			a = -( np - float( remaining ) ) * FlipTime;
		}
		else if( steps >= 1 )
		{
			c = ( ( c + dir * steps ) % N + N ) % N;
			p = np - float( steps );
			a = -p * FlipTime;
		}
		else
		{
			p = np;
			a = min( a, 0.0 ) - Dt;
		}
	}
	else if( t == c )
	{
		//Idle: the flutter timer runs on.
		a = max( a - Dt, -100.0 );
	}

	fragColor = vec4( float( c ), p, float( t ), a );
}
)";

//---------------------------------------------------------------------------
// Pass 3: board.
//---------------------------------------------------------------------------
const char* const kBoardShader = R"(#version 410 core

uniform sampler2D State;      //Columns x Rows, the motor's output
uniform sampler2D Drum;       //N x 1: what each flap is printed with
uniform sampler2D Curve;      //the flap's angle against flip units
uniform sampler2D FontTexture;//5 x 7 glyphs, one per ASCII code, in a row
uniform sampler2D Picture;    //the clip, for Mix

uniform ivec2 Grid;
uniform int Flaps;
uniform int DrumMode;
uniform float FlipTime;
uniform vec2 BoardOrigin;     //the board's rectangle in picture uv
uniform vec2 BoardSize;
uniform vec2 OutSize;         //the output in pixels
uniform vec2 Gap;             //frame between cells, as a fraction of the cell
uniform float Split;          //the split line, as a fraction of the cell's height
uniform float CellAspect;     //a cell's width over its height, in pixels
uniform vec3 FlapColour;
uniform float LightAngle;     //radians of elevation
uniform float MixAmount;
uniform float Perspective;    //how much wider the free edge draws when it is near
uniform int CurveSamples;
uniform float CurveSpan;

in vec2 uv;
out vec4 fragColor;

const float kPi = 3.14159265358979;
const float kHalfPi = 1.57079632679490;
const float kAmbient = 0.30;

//The same arithmetic as FlapCurve::angleAt: float index, floor, two
//fetches, one mix.
float curveAngle( float u )
{
	float f = clamp( u / CurveSpan, 0.0, 1.0 ) * float( CurveSamples - 1 );
	int i = clamp( int( floor( f ) ), 0, CurveSamples - 1 );
	int j = min( i + 1, CurveSamples - 1 );
	float w = f - float( i );
	return mix( texelFetch( Curve, ivec2( i, 0 ), 0 ).r, texelFetch( Curve, ivec2( j, 0 ), 0 ).r, w );
}

//What flap `flap` shows at plate position `pl` (0..1 across, 0..1 up, the
//hinge at 0.5).
vec3 printedColour( int flap, vec2 pl )
{
	vec4 printed = texelFetch( Drum, ivec2( flap, 0 ), 0 );
	vec3 dark = FlapColour * 0.06;
	if( DrumMode == 0 )
		return mix( dark, FlapColour, printed.a );
	if( DrumMode == 1 )
		return printed.rgb;

	//Text: a 5 x 7 glyph box centred on the plate, 0.64 of its width, or
	//less where the cell is too short for that.
	int code = int( printed.a + 0.5 );
	float gw = min( 0.64, 0.9 * ( 5.0 / 7.0 ) / max( CellAspect, 0.01 ) );
	float gh = gw * ( 7.0 / 5.0 ) * CellAspect;
	vec2 g = ( pl - 0.5 ) / vec2( gw, gh ) + 0.5;
	if( any( lessThan( g, vec2( 0.0 ) ) ) || any( greaterThanEqual( g, vec2( 1.0 ) ) ) )
		return dark;
	int gx = int( floor( g.x * 5.0 ) );
	int gy = int( floor( ( 1.0 - g.y ) * 7.0 ) );
	float bit = texelFetch( FontTexture, ivec2( code * 5 + gx, gy ), 0 ).r;
	return bit > 0.5 ? FlapColour : dark;
}

//Lambert, for a plate whose front normal has turned by `theta` about the
//hinge toward the viewer. The light is in the y-z plane at elevation
//LightAngle. Front face for the top half; back face for the bottom.
float shadeFront( float theta )
{
	return kAmbient + ( 1.0 - kAmbient ) * max( 0.0, cos( theta + LightAngle ) );
}

float shadeBack( float theta )
{
	return kAmbient + ( 1.0 - kAmbient ) * max( 0.0, -cos( theta + LightAngle ) );
}

void main()
{
	vec4 source = texture( Picture, uv );

	vec2 px = gl_FragCoord.xy;
	vec2 boardOrigin = BoardOrigin * OutSize;
	vec2 boardSize = BoardSize * OutSize;
	vec2 q = ( px - boardOrigin ) / boardSize;

	vec4 board = vec4( 0.0 );
	if( all( greaterThanEqual( q, vec2( 0.0 ) ) ) && all( lessThan( q, vec2( 1.0 ) ) ) )
	{
		vec2 g = q * vec2( Grid );
		int col = int( floor( g.x ) );
		int row = Grid.y - 1 - int( floor( g.y ) );
		vec2 local = g - floor( g );

		vec3 frame = vec3( 0.02 );
		vec2 halfGap = 0.5 * Gap;
		if( any( lessThan( local, halfGap ) ) || any( greaterThan( local, 1.0 - halfGap ) ) )
		{
			board = vec4( frame, 1.0 );
		}
		else
		{
			vec2 pl = ( local - halfGap ) / ( 1.0 - Gap );

			vec4 s = texelFetch( State, ivec2( col, row ), 0 );
			int N = max( Flaps, 2 );
			int c = int( s.r + 0.5 ) % N;
			float p = s.g;
			float a = s.a;

			bool falling = p > 0.0;
			int prev = ( c - 1 + N ) % N;
			int next = ( c + 1 ) % N;
			float theta = falling ? curveAngle( p ) : kPi;
			float since = max( -a, 0.0 );
			float thetaLanded = curveAngle( 1.0 + since / FlipTime );

			//The plate in the display plane, lit straight on.
			float plateShade = shadeFront( 0.0 );

			float y = pl.y - 0.5;//above the hinge, in plate units; L = 0.5
			vec3 colour;

			if( y >= 0.0 )
			{
				//Top half. Behind the flap in the air is the next symbol.
				int sym = falling ? next : c;
				colour = printedColour( sym, pl ) * plateShade;
				if( abs( y ) < 0.5 * Split )
					colour = frame;

				if( falling && theta < kHalfPi )
				{
					float h = 0.5 * cos( theta );
					if( y < h )
					{
						float sfrac = y / max( h, 1.0e-6 );
						float widen = 1.0 + Perspective * sin( theta ) * sfrac;
						float xp = 0.5 + ( pl.x - 0.5 ) / widen;
						if( abs( xp - 0.5 ) <= 0.5 )
							colour = printedColour( c, vec2( xp, 0.5 + 0.5 * sfrac ) ) * shadeFront( theta );
					}
				}
			}
			else
			{
				//Bottom half. The landed flap's back shows this symbol's lower
				//half; it may still be fluttering, and beneath it is the last
				//symbol.
				float yd = -y;
				colour = printedColour( prev, pl ) * plateShade;
				if( yd < 0.5 * Split )
					colour = frame;

				float hb = 0.5 * abs( cos( thetaLanded ) );
				if( yd < hb )
				{
					float sfrac = yd / max( hb, 1.0e-6 );
					float widen = 1.0 + Perspective * max( sin( thetaLanded ), 0.0 ) * sfrac;
					float xp = 0.5 + ( pl.x - 0.5 ) / widen;
					if( abs( xp - 0.5 ) <= 0.5 )
						colour = printedColour( c, vec2( xp, 0.5 - 0.5 * sfrac ) ) * shadeBack( thetaLanded );
				}

				//The flap in the air, once past the horizontal, comes down over it.
				if( falling && theta >= kHalfPi )
				{
					float h = 0.5 * -cos( theta );
					if( yd < h )
					{
						float sfrac = yd / max( h, 1.0e-6 );
						float widen = 1.0 + Perspective * sin( theta ) * sfrac;
						float xp = 0.5 + ( pl.x - 0.5 ) / widen;
						if( abs( xp - 0.5 ) <= 0.5 )
							colour = printedColour( next, vec2( xp, 0.5 - 0.5 * sfrac ) ) * shadeBack( theta );
					}
				}
			}
			board = vec4( colour, 1.0 );
		}
	}

	fragColor = mix( source, board, MixAmount );
}
)";

} // namespace splitflap
