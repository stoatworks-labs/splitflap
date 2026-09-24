/**
 * Splitflap — browser demo.
 *
 * The picture on a split-flap departures board. The one idea, from
 * `source/Splitflap.h`: a drum can only turn forward. Every cell of the board
 * holds a drum of N flaps and can only pass through them one at a time, at the
 * motor's rate, so a change ripples, a step brighter is one flip and a step
 * darker is N − 1, a moving picture is never finished, and every flap in the
 * air is a hinged plate falling under gravity with its shading following its
 * angle.
 *
 * Like atrac and teletext, this plugin is **not only a shader**, and the two
 * halves of the page are not equally faithful:
 *
 *   The shaders are the plugin's. The four GLSL programs below — vertex, copy,
 *   motor and board — are `source/Shaders.cpp`'s string constants, copied
 *   across unedited by `demo/tools/splice_shaders.py` (two backticks in a
 *   comment escaped). `demo/tools/check_shaders.py` compares them character
 *   for character, and the Text drum's 5 × 7 font against `source/Font.cpp`,
 *   and `tools/verify.sh` runs it. The motor shader IS the plugin: one
 *   fragment per cell holds the state (flap, phase, target, timer) in an
 *   RGBA32F texel, latches a target when the update fires, and advances by
 *   the frame's Dt. Here it runs in WebGL2 on a float render target, ping-
 *   ponged between two buffers sized to the grid, exactly as in the plugin.
 *
 *   The CPU half is a PORT — of `Flap.cpp` (the rigid plate solved with RK4
 *   in double, the impacts interpolated inside the step, sampled to a table in
 *   flip units), `Drum.cpp` (the tones, the six palettes, the 45-character
 *   Text alphabet, the three orders, the message layout), `Font.cpp`'s
 *   texture, `Onset.cpp`, `Controls.cpp` and the frame sequence in
 *   `SplitflapPlugin::ProcessOpenGL` (the clock's 0.25 s delta clamp, the
 *   update decision with Interval ticking from the mode's start, the board's
 *   geometry, the state ping-pong and the regrid) — function for function, in
 *   JavaScript numbers, which are IEEE doubles as the C++'s are. Nothing
 *   checks a port but a reader. `sftest --flips`, `--asymmetry`, `--settle`,
 *   `--fall`, `--resize` and `--prime` check the C++ originals and have no
 *   idea this page exists.
 *
 * ------------------------------------------------------------- the clock
 *
 * The plugin's `Clock` measures the host's unit (seconds or milliseconds)
 * over several frames before it believes a delta. The page's clock is the
 * kit's `time` — seconds since the page started, paused by Pause and stepped
 * by Step — which the port reads with the unit DECLARED as seconds, the way
 * `sftest` declares its own. The unit vote never runs here. Restart sends the
 * clock backwards; the plugin's clamp reads that as a delta of zero, and the
 * board holds still for one frame.
 *
 * ------------------------------------------------------------- what is missing
 *
 * **Nothing audio.** The Audio FFT buffer is absent rather than present and
 * dead — it is a host-written buffer, not a control an operator draws — and
 * the ported onset detector is handed silence every frame, which it reads
 * exactly as the plugin reads an unrouted input. Onset mode therefore never
 * fires here past the first frame's latch. **Update Now is an event** with
 * no counterpart in the kit, so it is a button the page presses for one
 * frame. **Columns, Rows, Flaps and Module Width are FF_TYPE_INTEGER**, which
 * the kit has no control for, so they are dropdowns of every value in the
 * plugin's range. **The About block is absent**, as on every page in this
 * suite.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp, written here by
// demo/tools/splice_shaders.py. Do not edit between the markers. The two
// backticks in a GLSL comment are escaped, and check_shaders.py unescapes them.
//---------------------------------------------------------------------------

// @@shaders-begin -- written by demo/tools/splice_shaders.py, do not edit

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const COPY = `#version 410 core

uniform sampler2D InputTexture;
uniform vec2 MaxUV;

in vec2 uv;
out vec4 fragColor;

void main()
{
	fragColor = texture( InputTexture, uv * MaxUV );
}
`;

const MOTOR = `#version 410 core

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
		//is not.
		if( luma < 0.5 )
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
`;

const BOARD = `#version 410 core

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

//What flap \`flap\` shows at plate position \`pl\` (0..1 across, 0..1 up, the
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

//Lambert, for a plate whose front normal has turned by \`theta\` about the
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
`;

// @@shaders-end

//===========================================================================
// Controls.cpp, ported. Every conversion from the host's 0..1 to the board's
// unit lives there and nowhere else, so it lives here and nowhere else too.
//===========================================================================

const clamp01 = (v) => Math.min(Math.max(v, 0), 1);
const lerp = (from, to, t) => from + (to - from) * clamp01(t);
/// Geometric interpolation: equal slider movements are equal *ratios*.
const geometric = (from, to, t) => from * Math.pow(to / from, clamp01(t));
const geometricInverse = (from, to, x) => clamp01(Math.log(x / from) / Math.log(to / from));

// -- Ranges of the integer parameters (Controls.h) -------------------------------
const K_COLUMNS_MIN = 4, K_COLUMNS_MAX = 96, K_COLUMNS_DEFAULT = 32;
const K_ROWS_MIN = 2, K_ROWS_MAX = 54, K_ROWS_DEFAULT = 18;
const K_FLAPS_MIN = 2, K_FLAPS_MAX = 64, K_FLAPS_DEFAULT = 12;
const K_MODULE_MIN = 1, K_MODULE_MAX = 16, K_MODULE_DEFAULT = 1;

// -- Options (Controls.h) ---------------------------------------------------------
const K_DRUM_TONES = 0, K_DRUM_COLOURS = 1, K_DRUM_TEXT = 2, K_DRUM_COUNT = 3;
const K_ORDER_DARK_TO_LIGHT = 0, K_ORDER_LIGHT_TO_DARK = 1, K_ORDER_SHUFFLED = 2, K_ORDER_COUNT = 3;
const K_UPDATE_CONTINUOUS = 0, K_UPDATE_INTERVAL = 1, K_UPDATE_ONSET = 2, K_UPDATE_MANUAL = 3, K_UPDATE_COUNT = 4;

/// An option's stored value is its element index; a host or a script may
/// also hand over a normalised 0..1, which this folds back onto an index.
function optionIndex(value, count) {
  if (count <= 1) return 0;
  let v = value;
  if (v > 0 && v <= 1 && Math.abs(v - Math.round(v)) > 1e-4) v = v * (count - 1);
  const index = Math.round(v);
  return Math.min(Math.max(index, 0), count - 1);
}

/// Splitflap.cpp's intParam: lround, then clamp to the declared range.
const intParam = (value, lo, hi) => Math.min(Math.max(Math.round(value), lo), hi);

const cellAspectFromParam = (v) => lerp(0.5, 1.5, v);
const cellAspectToParam = (aspect) => clamp01((aspect - 0.5) / 1.0);
const gapFromParam = (v) => lerp(0.0, 0.25, v);
const splitFromParam = (v) => lerp(0.0, 0.12, v);
const flipTimeFromParam = (v) => geometric(0.03, 1.0, v);
const flipTimeToParam = (seconds) => geometricInverse(0.03, 1.0, seconds);
const staggerFromParam = (v) => lerp(0.0, 1.0, v);
const intervalFromParam = (v) => geometric(0.1, 10.0, v);
const intervalToParam = (seconds) => geometricInverse(0.1, 10.0, seconds);
const K_SIXTY_DEGREES = 1.0471975512;
const lightAngleFromParam = (v) => lerp(-K_SIXTY_DEGREES, K_SIXTY_DEGREES, v);
const restitutionFromParam = (v) => lerp(0.0, 0.6, v);

//===========================================================================
// Flap.cpp, ported: the hinged plate solved once, in double.
//
// theta'' = (3g / 2L) sin theta, an inverted pendulum let go three degrees
// past vertical, integrated with RK4 at a fine step; the impacts at pi found
// by interpolating the crossing inside the step; the velocity reversed and
// shrunk by the restitution until a rebound would rise less than half a
// degree, after which the table is exactly pi. Time is in flip units: u = 1
// is the landing. The negative controls (constant acceleration, detuned
// gravity) are harness-only and always nominal in the plugin, so they are not
// carried here.
//===========================================================================

const K_PI = 3.14159265358979323846;
const K_STEP = 1.0e-4;
const K_RELEASE_ANGLE = 0.05235987755982988;
const CURVE_SAMPLES = 2048;
const CURVE_SPAN = 4.0;

const accel = (theta) => Math.sin(theta);

/// RK4 step, dimensionless. Returns [theta, omega].
function rk4(theta, omega, h) {
  const k1t = omega, k1w = accel(theta);
  const k2t = omega + 0.5 * h * k1w, k2w = accel(theta + 0.5 * h * k1t);
  const k3t = omega + 0.5 * h * k2w, k3w = accel(theta + 0.5 * h * k2t);
  const k4t = omega + h * k3w, k4w = accel(theta + h * k3t);
  return [
    theta + h / 6.0 * (k1t + 2.0 * k2t + 2.0 * k3t + k4t),
    omega + h / 6.0 * (k1w + 2.0 * k2w + 2.0 * k3w + k4w),
  ];
}

/// The dimensionless time from release to the first arrival at pi, with no
/// bounce: the reference every table is scaled by.
function fallTime() {
  let theta = K_RELEASE_ANGLE, omega = 0.0, t = 0.0;
  for (let i = 0; i < 20000000; i += 1) {
    const prevTheta = theta;
    [theta, omega] = rk4(theta, omega, K_STEP);
    t += K_STEP;
    if (theta >= K_PI) {
      const f = (K_PI - prevTheta) / (theta - prevTheta);
      return t - K_STEP + f * K_STEP;
    }
  }
  return t;
}

/// SolveFlap: { table: Float32Array(2048), bounceEnd, fallTime }.
function solveFlap(restitution) {
  const reference = fallTime();
  let theta = K_RELEASE_ANGLE, omega = 0.0, t = 0.0;
  const tEnd = CURVE_SPAN * reference;
  const stopVelocity = 0.5 * K_PI / 180.0;

  const fine = [theta];
  let pinned = false;
  let pinnedAt = 0.0;
  while (t < tEnd) {
    if (pinned) {
      fine.push(K_PI);
      t += K_STEP;
      continue;
    }
    const prevTheta = theta, prevOmega = omega;
    [theta, omega] = rk4(theta, omega, K_STEP);
    t += K_STEP;
    if (theta >= K_PI) {
      const f = (K_PI - prevTheta) / (theta - prevTheta);
      const tHit = f * K_STEP;
      let wHit = prevOmega + f * (omega - prevOmega);
      wHit = -restitution * wHit;
      if (-wHit < stopVelocity) {
        pinned = true;
        pinnedAt = t - K_STEP + tHit;
        theta = K_PI;
        omega = 0.0;
      } else {
        theta = K_PI;
        omega = wHit;
        [theta, omega] = rk4(theta, omega, K_STEP - tHit);
        if (theta > K_PI) theta = K_PI;
      }
    }
    fine.push(theta);
  }

  const table = new Float32Array(CURVE_SAMPLES);
  const bounceEnd = pinned ? pinnedAt / reference : CURVE_SPAN;
  for (let i = 0; i < CURVE_SAMPLES; i += 1) {
    const u = CURVE_SPAN * i / (CURVE_SAMPLES - 1);
    const tt = u * reference;
    const fi = tt / K_STEP;
    const k = Math.floor(fi);
    let value;
    if (k + 1 >= fine.length) value = fine[fine.length - 1];
    else {
      const w = fi - k;
      value = fine[k] + (fine[k + 1] - fine[k]) * w;
    }
    table[i] = Math.min(value, K_PI);
  }
  // The stop is exactly pi from the last bounce on, so a settled board is
  // bit-identical frame to frame rather than creeping by an ulp.
  if (pinned) {
    const from = Math.ceil(bounceEnd / CURVE_SPAN * (CURVE_SAMPLES - 1));
    for (let i = Math.max(from, 0); i < CURVE_SAMPLES; i += 1) table[i] = K_PI;
  }
  return { table, bounceEnd, fallTime: reference };
}

//===========================================================================
// Drum.cpp, ported: what is printed on the flaps.
//===========================================================================

/// Authored in the plugin, not imported. Sampled at N evenly spaced points.
const PALETTES = [
  { name: 'Amber', stops: [[0.05, 0.03, 0.00], [1.00, 0.62, 0.05], [1.00, 0.92, 0.60]] },
  { name: 'Airport', stops: [[0.02, 0.02, 0.02], [0.95, 0.95, 0.92], [1.00, 0.85, 0.00], [1.00, 0.45, 0.00], [0.85, 0.05, 0.05]] },
  { name: 'Rainbow', stops: [[1.0, 0.0, 0.0], [1.0, 1.0, 0.0], [0.0, 1.0, 0.0], [0.0, 1.0, 1.0], [0.0, 0.0, 1.0], [1.0, 0.0, 1.0], [1.0, 0.0, 0.0]] },
  { name: 'Ocean', stops: [[0.02, 0.05, 0.15], [0.00, 0.35, 0.60], [0.20, 0.80, 0.85], [0.90, 0.98, 1.00]] },
  { name: 'Ember', stops: [[0.02, 0.00, 0.00], [0.50, 0.02, 0.00], [1.00, 0.35, 0.00], [1.00, 0.85, 0.30]] },
  { name: 'Candy', stops: [[0.95, 0.20, 0.50], [1.00, 0.85, 0.20], [0.30, 0.90, 0.60], [0.30, 0.60, 1.00], [0.70, 0.30, 0.90]] },
];

/// The Text drum. Blank first; anything not in it draws as the blank.
const K_ALPHABET = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:!?/'&";
const ALPHABET_SIZE = K_ALPHABET.length;

/// The PCG output permutation (RXS-M-XS on 32 bits), exact in integers. The
/// C++ does this in uint32_t; here every product is Math.imul and every
/// result is forced back to an unsigned 32-bit value.
function hash(x) {
  x = (Math.imul(x, 747796405) + 2891336453) >>> 0;
  x = Math.imul(((x >>> ((x >>> 28) + 4)) ^ x) >>> 0, 277803737) >>> 0;
  return ((x >>> 22) ^ x) >>> 0;
}

function samplePalette(palette, s) {
  const n = palette.stops.length;
  if (n === 1) return palette.stops[0];
  const f = clamp01(s) * (n - 1);
  const i = Math.min(Math.floor(f), n - 2);
  const w = f - i;
  const a = palette.stops[i];
  const b = palette.stops[i + 1];
  return [a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w];
}

/// Fisher-Yates over [first, n), with the hash as the generator. Seeded from a
/// constant: a shuffled drum is one drum, not a new one per session.
function shuffle(order, first) {
  const K_SEED = 0x5F17F1A9;
  for (let i = order.length - 1; i > first; i -= 1) {
    const h = hash((K_SEED ^ Math.imul(i, 0x9E3779B9)) >>> 0);
    const j = first + (h % (i - first + 1));
    const tmp = order[i];
    order[i] = order[j];
    order[j] = tmp;
  }
}

/// The flap order as a permutation of the natural order. `fixedFirst` keeps
/// element 0 (the Text blank) where it is.
function ordering(n, order, fixedFirst) {
  const out = [];
  for (let i = 0; i < n; i += 1) out.push(i);
  const first = fixedFirst ? 1 : 0;
  if (order === K_ORDER_LIGHT_TO_DARK) {
    const tail = out.slice(first).reverse();
    for (let i = first; i < n; i += 1) out[i] = tail[i - first];
  } else if (order === K_ORDER_SHUFFLED) {
    shuffle(out, first);
  }
  return out;
}

/// drum::Build. One flap per texel: rgb is the printed colour (Colours), a is
/// the tone (Tones, 0..1) or the character code (Text).
function buildDrum(mode, flaps, order, palette) {
  if (mode === K_DRUM_TEXT) {
    const n = ALPHABET_SIZE;
    const perm = ordering(n, order, true);
    const rgba = new Float32Array(n * 4);
    for (let k = 0; k < n; k += 1) rgba[k * 4 + 3] = K_ALPHABET.charCodeAt(perm[k]);
    return { flaps: n, rgba };
  }
  const n = Math.min(Math.max(flaps, K_FLAPS_MIN), K_FLAPS_MAX);
  const rgba = new Float32Array(n * 4);
  const perm = ordering(n, order, false);
  const pal = PALETTES[Math.min(Math.max(palette, 0), PALETTES.length - 1)];
  for (let k = 0; k < n; k += 1) {
    const s = perm[k] / (n - 1);
    const c = samplePalette(pal, s);
    rgba[k * 4 + 0] = c[0];
    rgba[k * 4 + 1] = c[1];
    rgba[k * 4 + 2] = c[2];
    rgba[k * 4 + 3] = s;
  }
  return { flaps: n, rgba };
}

/// drum::Message: the drum index of each cell's character, row-major from the
/// top left, wrapping at `columns`; 0 (the blank) past the end of the text
/// and for any character the drum does not carry. Lower case is folded to
/// upper. The C++ walks the std::string's BYTES, so the text is encoded to
/// UTF-8 first and a multi-byte character is as many blanks as it has bytes.
function buildMessage(text, columns, rows, textPrint) {
  const cells = new Float32Array(columns * rows);
  const bytes = new TextEncoder().encode(text);
  for (let i = 0; i < bytes.length && i < cells.length; i += 1) {
    let code = bytes[i];
    if (code >= 97 && code <= 122) code -= 32;
    for (let k = 0; k < textPrint.flaps; k += 1) {
      if (Math.round(textPrint.rgba[k * 4 + 3]) === code) {
        cells[i] = k;
        break;
      }
    }
  }
  return cells;
}

//===========================================================================
// Font.cpp, ported: the 5 x 7 bitmap font the Text drum is printed with. The
// glyph pictures are copied from Font.cpp by splice_shaders.py and held to
// it by check_shaders.py; font::Texture() is the loop below.
//===========================================================================

const FONT_FIRST = 32;
const FONT_WIDTH = 5;
const FONT_HEIGHT = 7;
const FONT_TEXTURE_WIDTH = FONT_WIDTH * 128;
const FONT_TEXTURE_HEIGHT = FONT_HEIGHT;

// @@font-begin -- written by demo/tools/splice_shaders.py from source/Font.cpp, do not edit

const GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'],
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'],
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'],
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'],
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'],
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'],
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'],
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'],
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'],
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'],
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'],
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'],
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'],
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'],
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'],
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'],
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'],
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'],
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'],
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'],
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'],
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'],
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'],
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'],
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'],
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'],
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'],
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'],
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'],
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'],
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'],
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'],
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'],
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'],
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'],
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'],
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'],
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'],
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'],
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'],
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'],
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'],
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'],
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'],
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'],
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'],
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'],
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'],
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'],
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'],
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'],
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'],
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'],
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'],
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'],
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'],
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'],
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'],
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'],
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'],
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'],
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'],
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'],
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'],
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'],
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'],
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'],
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'],
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'],
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'],
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'],
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'],
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'],
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'],
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'],
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'],
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'],
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'],
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'],
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'],
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'],
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'],
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'],
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'],
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'],
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'],
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'],
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'],
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'],
];

// @@font-end

function fontBit(code, x, y) {
  if (code < FONT_FIRST || code >= FONT_FIRST + GLYPHS.length) return false;
  return GLYPHS[code - FONT_FIRST][y][x] === '#';
}

/// The whole table as an 8-bit single-channel image, indexed by ASCII code
/// directly: glyph for code c starts at column c * 5.
function fontTexture() {
  const out = new Uint8Array(FONT_TEXTURE_WIDTH * FONT_TEXTURE_HEIGHT);
  for (let code = 0; code < 128; code += 1) {
    for (let y = 0; y < FONT_HEIGHT; y += 1) {
      for (let x = 0; x < FONT_WIDTH; x += 1) {
        if (fontBit(code, x, y)) out[y * FONT_TEXTURE_WIDTH + code * FONT_WIDTH + x] = 255;
      }
    }
  }
  return out;
}

//===========================================================================
// Onset.cpp, ported: spectral flux over the 64 bins, primed on frame one. On
// this page it is handed silence every frame -- no spectrum reaches a
// browser -- which it reads exactly as the plugin reads an unrouted input.
//===========================================================================

const K_BINS = 64;
const K_FLOOR_SECONDS = 1.0;
const K_RATIO = 2.5;
const K_MIN_FLUX = 0.02;
const K_REFRACTORY = 0.10;
const K_PRIME_FRACTION = 0.125;

class Onset {
  constructor() {
    this.reset();
  }

  reset() {
    this.prev = new Float64Array(K_BINS);
    this.floor = 0.0;
    this.flux = 0.0;
    this.fluxPrev = 0.0;
    this.lastFire = -1.0e9;
    this.lastSeconds = 0.0;
    this.primed = false;
  }

  /// One frame of the host's spectrum (`bins` null: nothing written).
  frame(seconds, bins, count) {
    const m = new Float64Array(K_BINS);
    let level = 0.0;
    for (let i = 0; i < K_BINS; i += 1) {
      const v = bins !== null && i < count ? bins[i] : 0.0;
      const clean = v > 0.0 ? v : 0.0;
      m[i] = Math.sqrt(clean);
      level += m[i];
    }

    if (!this.primed) {
      this.prev = m;
      this.floor = level * K_PRIME_FRACTION;
      this.fluxPrev = 0.0;
      this.lastSeconds = seconds;
      this.lastFire = seconds - K_REFRACTORY;
      this.primed = true;
      return false;
    }

    const dt = Math.max(seconds - this.lastSeconds, 0.0);
    this.lastSeconds = seconds;

    let flux = 0.0;
    for (let i = 0; i < K_BINS; i += 1) flux += Math.max(m[i] - this.prev[i], 0.0);
    this.prev = m;

    let fired = false;
    if (flux > Math.max(K_RATIO * this.floor, K_MIN_FLUX) && flux > this.fluxPrev && seconds - this.lastFire >= K_REFRACTORY) {
      fired = true;
      this.lastFire = seconds;
    }

    if (dt > 0.0) {
      const a = 1.0 - Math.exp(-dt / K_FLOOR_SECONDS);
      this.floor += (flux - this.floor) * a;
    }
    this.fluxPrev = flux;
    this.flux = flux;
    return fired;
  }
}

//===========================================================================
// The renderer: SplitflapPlugin::ProcessOpenGL, in its order.
//
//   0. clock, audio, the update decision     CPU
//   1. copy      W x H, RGBA8, mip chain     the clip, into a texture of ours
//   2. motor     Columns x Rows, RGBA32F     one fragment per cell, ping-ponged
//   3. board     onto the canvas
//===========================================================================

/// Seconds of host time a single frame may advance the board by.
const K_MAX_FRAME_DELTA = 0.25;
/// How much wider the flap's free edge draws when it is nearest the viewer.
const K_PERSPECTIVE = 0.10;

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = { columns: 0, rows: 0, flaps: 0, mode: 0, fires: 0, flipTime: 0, dt: 0, nextTick: -1, now: 0, onsets: 0, bounceEnd: 1 };

function createSmallTexture(gl) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);
  return texture;
}

function createRenderer(gl, quad) {
  const shaders = {
    copy: new Program(gl, VERTEX, COPY, 'copy'),
    motor: new Program(gl, VERTEX, MOTOR, 'motor'),
    board: new Program(gl, VERTEX, BOARD, 'board'),
  };

  // The clip, ours, with a mip chain the motor's sixteen taps read at a whole
  // level. The plugin's PassBuffer::Sampling::Mipmapped is trilinear too.
  const copy = new PassBuffer(gl, { filter: 'linear', mip: true });
  // Ping-pong: one float texel per cell, sized to the grid, never to the
  // picture. Nearest, as in the plugin.
  const state = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
  let stateCurrent = 0;

  const drumTexture = createSmallTexture(gl);
  const messageTexture = createSmallTexture(gl);
  const curveTexture = createSmallTexture(gl);
  const fontTex = createSmallTexture(gl);
  // On the first frame the plugin binds the buffer it is about to write as
  // PrevState too (OldGrid is 0, so the shader never reads it). WebGL refuses
  // a draw whose sampler is its own render target, so the first frame binds
  // this 1 x 1 texel instead. The shader's arithmetic is the same either way.
  const noState = createSmallTexture(gl);
  gl.bindTexture(gl.TEXTURE_2D, noState);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, 1, 0, gl.RGBA, gl.FLOAT, new Float32Array([0, 0, 0, -100]));
  gl.bindTexture(gl.TEXTURE_2D, null);

  // uploadFont, once.
  gl.bindTexture(gl.TEXTURE_2D, fontTex);
  gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, FONT_TEXTURE_WIDTH, FONT_TEXTURE_HEIGHT, 0, gl.RED, gl.UNSIGNED_BYTE, fontTexture());
  gl.bindTexture(gl.TEXTURE_2D, null);

  // The state SplitflapPlugin carries across host frames.
  let print = { flaps: 0, rgba: null };
  let drumKey = '';
  let messageKey = '';
  let curveKey = '';
  let curve = null;
  let lastSeconds = -1.0;
  let firstFrame = true;
  const onset = new Onset();
  let onsetFired = false;
  let onsetCount = 0;
  let updatePending = false;
  let nextTick = -1.0;
  let lastUpdateMode = -1;
  let fireCount = 0;

  function uploadCurve(restitution) {
    const key = String(restitution);
    if (key === curveKey) return;
    curveKey = key;
    curve = solveFlap(restitution);
    gl.bindTexture(gl.TEXTURE_2D, curveTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, CURVE_SAMPLES, 1, 0, gl.RED, gl.FLOAT, curve.table);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  function uploadDrum(mode, flaps, order, palette) {
    const key = `${mode}/${flaps}/${order}/${palette}`;
    if (key === drumKey) return;
    drumKey = key;
    print = buildDrum(mode, flaps, order, palette);
    gl.bindTexture(gl.TEXTURE_2D, drumTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, print.flaps, 1, 0, gl.RGBA, gl.FLOAT, print.rgba);
    gl.bindTexture(gl.TEXTURE_2D, null);
    // As in the plugin: a new drum marks the message dirty.
    messageKey = '';
  }

  function uploadMessage(text, columns, rows, order) {
    const key = `${text}\u0000${columns}/${rows}/${order}`;
    if (key === messageKey) return;
    messageKey = key;
    // The message is laid out against the Text drum's order whatever drum is
    // on show, so switching to Text later finds it ready.
    const textPrint = buildDrum(K_DRUM_TEXT, 0, order, 0);
    const m = buildMessage(text, columns, rows, textPrint);
    gl.bindTexture(gl.TEXTURE_2D, messageTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, columns, rows, 0, gl.RED, gl.FLOAT, m);
    gl.bindTexture(gl.TEXTURE_2D, null);
  }

  /// SplitflapPlugin::decideUpdate, verbatim in its logic.
  function decideUpdate(now, mode, interval) {
    let fire = firstFrame || updatePending;
    updatePending = false;

    if (mode !== lastUpdateMode) {
      lastUpdateMode = mode;
      nextTick = -1.0;
    }

    switch (mode) {
      case K_UPDATE_CONTINUOUS:
        fire = true;
        break;
      case K_UPDATE_INTERVAL:
        if (nextTick < 0.0) nextTick = now + interval;
        else if (now >= nextTick) {
          fire = true;
          nextTick += interval;
          if (now >= nextTick) nextTick = now + interval; // a stall longer than the interval: one update, not a burst
        }
        break;
      case K_UPDATE_ONSET:
        if (onsetFired) fire = true;
        break;
      default:
        break;
    }
    return fire;
  }

  const setIVec2 = (program, name, x, y) => {
    const loc = program.location(name);
    if (loc !== null) gl.uniform2i(loc, x, y);
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const p = (id) => params.get(id);
      const picture = input;
      const pictureWidth = picture.width;
      const pictureHeight = picture.height;

      //------------------------------------------------------------------
      // Time, in seconds. The unit is declared, not measured (see the head
      // of this file). The frame delta is what the motor advances by; the
      // shader never sees an absolute time.
      //------------------------------------------------------------------
      const now = time;
      let dt = 0.0;
      if (lastSeconds >= 0.0) dt = Math.min(Math.max(now - lastSeconds, 0.0), K_MAX_FRAME_DELTA);
      lastSeconds = now;

      //------------------------------------------------------------------
      // Audio (none reaches this page: silence, as an unrouted input), and
      // whether the board refreshes its targets this frame. Update Now is
      // an event in the plugin: the button is read as one press and released.
      //------------------------------------------------------------------
      onsetFired = onset.frame(now, null, 0);
      if (onsetFired) onsetCount += 1;
      if (p('updateNow') > 0.5) {
        updatePending = true;
        params.set('updateNow', 0, { silent: true });
      }
      const updateMode = optionIndex(p('update'), K_UPDATE_COUNT);
      const interval = intervalFromParam(p('interval'));
      const fireThisFrame = decideUpdate(now, updateMode, interval);
      if (fireThisFrame) fireCount += 1;

      //------------------------------------------------------------------
      // Geometry: where the board sits in the picture.
      //------------------------------------------------------------------
      const columns = intParam(integerValue('columns', p('columns')), K_COLUMNS_MIN, K_COLUMNS_MAX);
      const rows = intParam(integerValue('rows', p('rows')), K_ROWS_MIN, K_ROWS_MAX);

      let boardOriginX = 0.0, boardOriginY = 0.0, boardSizeX = 1.0, boardSizeY = 1.0;
      if (p('fit') <= 0.5) {
        const aspect = cellAspectFromParam(p('cellAspect'));
        const boardAspect = columns * aspect / rows;
        const frameAspect = pictureWidth / pictureHeight;
        if (boardAspect > frameAspect) boardSizeY = frameAspect / boardAspect;
        else boardSizeX = boardAspect / frameAspect;
        boardOriginX = 0.5 * (1.0 - boardSizeX);
        boardOriginY = 0.5 * (1.0 - boardSizeY);
      }
      const cellWidthPx = boardSizeX * pictureWidth / columns;
      const cellHeightPx = boardSizeY * pictureHeight / rows;
      const pictureLod = Math.floor(Math.log2(Math.max(1.0, Math.min(cellWidthPx, cellHeightPx) / 8.0)));

      //------------------------------------------------------------------
      // Tables and buffers, before anything binds a texture.
      //------------------------------------------------------------------
      const drumMode = optionIndex(p('drum'), K_DRUM_COUNT);
      const order = optionIndex(p('order'), K_ORDER_COUNT);
      const palette = optionIndex(p('palette'), PALETTES.length);
      const flaps = intParam(integerValue('flaps', p('flaps')), K_FLAPS_MIN, K_FLAPS_MAX);
      const moduleWidth = intParam(integerValue('module', p('module')), K_MODULE_MIN, K_MODULE_MAX);
      const flipTime = flipTimeFromParam(p('flipTime'));

      uploadCurve(restitutionFromParam(p('bounce')));
      uploadDrum(drumMode, flaps, order, palette);
      uploadMessage(String(p('text')), columns, rows, order);

      copy.ensure(pictureWidth, pictureHeight, gl.RGBA8);

      // The state that holds the board is whatever size the last frame's grid
      // was; only the buffer about to be written is sized to this frame's
      // grid. The motor pass maps one onto the other, so a Columns change
      // keeps the picture and a picture resize never touches it at all.
      const current = stateCurrent;
      const target = 1 - stateCurrent;
      state[target].ensure(columns, rows, gl.RGBA32F);
      let oldColumns = 0, oldRows = 0;
      const haveState = !firstFrame && state[current].texture !== null;
      if (haveState) {
        oldColumns = state[current].width;
        oldRows = state[current].height;
      }
      gl.disable(gl.BLEND);

      //------------------------------------------------------------------
      // 1. The clip, into a texture of ours, with a mip chain. A browser
      // texture is unpadded, so MaxUV is (1, 1).
      //------------------------------------------------------------------
      copy.bind();
      shaders.copy.use();
      bindTexture(gl, 0, picture.texture);
      shaders.copy.setSampler('InputTexture', 0);
      shaders.copy.set('MaxUV', 1.0, 1.0);
      quad.draw();
      copy.generateMipmap();

      //------------------------------------------------------------------
      // 2. The motor: one fragment per cell.
      //------------------------------------------------------------------
      state[target].bind();
      shaders.motor.use();
      bindTexture(gl, 0, haveState ? state[current].texture : noState);
      bindTexture(gl, 1, copy.texture);
      bindTexture(gl, 2, drumTexture);
      bindTexture(gl, 3, messageTexture);
      shaders.motor.setSampler('PrevState', 0);
      shaders.motor.setSampler('Picture', 1);
      shaders.motor.setSampler('Drum', 2);
      shaders.motor.setSampler('Message', 3);
      setIVec2(shaders.motor, 'Grid', columns, rows);
      setIVec2(shaders.motor, 'OldGrid', oldColumns, oldRows);
      shaders.motor.set('Fire', fireThisFrame ? 1.0 : 0.0);
      shaders.motor.setInt('DrumMode', drumMode);
      shaders.motor.setInt('Flaps', print.flaps);
      shaders.motor.set('Dt', dt);
      shaders.motor.set('FlipTime', flipTime);
      shaders.motor.set('Stagger', staggerFromParam(p('stagger')));
      shaders.motor.setInt('ModuleWidth', moduleWidth);
      shaders.motor.set('BoardOrigin', boardOriginX, boardOriginY);
      shaders.motor.set('BoardSize', boardSizeX, boardSizeY);
      shaders.motor.set('PictureLod', pictureLod);
      shaders.motor.setInt('AllowBackward', 0);
      quad.draw();
      stateCurrent = target;
      firstFrame = false;

      //------------------------------------------------------------------
      // 3. The board, straight to the canvas. The host's viewport is the
      // whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      shaders.board.use();
      bindTexture(gl, 0, state[target].texture);
      bindTexture(gl, 1, drumTexture);
      bindTexture(gl, 2, curveTexture);
      bindTexture(gl, 3, fontTex);
      bindTexture(gl, 4, copy.texture);
      shaders.board.setSampler('State', 0);
      shaders.board.setSampler('Drum', 1);
      shaders.board.setSampler('Curve', 2);
      shaders.board.setSampler('FontTexture', 3);
      shaders.board.setSampler('Picture', 4);
      setIVec2(shaders.board, 'Grid', columns, rows);
      shaders.board.setInt('Flaps', print.flaps);
      shaders.board.setInt('DrumMode', drumMode);
      shaders.board.set('FlipTime', flipTime);
      shaders.board.set('BoardOrigin', boardOriginX, boardOriginY);
      shaders.board.set('BoardSize', boardSizeX, boardSizeY);
      shaders.board.set('OutSize', vpW, vpH);
      const gap = gapFromParam(p('gap'));
      shaders.board.set('Gap', gap, gap * cellWidthPx / Math.max(cellHeightPx, 1.0));
      shaders.board.set('Split', splitFromParam(p('split')));
      shaders.board.set('CellAspect', cellWidthPx / Math.max(cellHeightPx, 1.0));
      shaders.board.set('FlapColour', p('flapR'), p('flapG'), p('flapB'));
      shaders.board.set('LightAngle', lightAngleFromParam(p('light')));
      shaders.board.set('MixAmount', p('mix'));
      shaders.board.set('Perspective', K_PERSPECTIVE);
      shaders.board.setInt('CurveSamples', CURVE_SAMPLES);
      shaders.board.set('CurveSpan', CURVE_SPAN);
      quad.draw();

      for (let unit = 4; unit >= 1; unit -= 1) bindTexture(gl, unit, null);
      gl.activeTexture(gl.TEXTURE0);

      telemetry.columns = columns;
      telemetry.rows = rows;
      telemetry.flaps = print.flaps;
      telemetry.mode = updateMode;
      telemetry.fires = fireCount;
      telemetry.flipTime = flipTime;
      telemetry.dt = dt;
      telemetry.nextTick = nextTick;
      telemetry.now = now;
      telemetry.onsets = onsetCount;
      telemetry.bounceEnd = curve ? curve.bounceEnd : 1;
    },
  };
}

//===========================================================================
// The controls, read out of SplitflapPlugin::SplitflapPlugin(). Same names,
// same groups, same order, same defaults, same dropdown elements. Absent: the
// Audio FFT buffer (a host-written buffer, not a control an operator draws,
// and no spectrum exists here) and the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores these
/// as the integer itself. The kit has no integer control, so -- as copperlist
/// and teletext did -- they are dropdowns of every value in the plugin's
/// range; `integerValue` turns the dropdown's index back into it.
const INTEGER_RANGES = {
  columns: [K_COLUMNS_MIN, K_COLUMNS_MAX],
  rows: [K_ROWS_MIN, K_ROWS_MAX],
  flaps: [K_FLAPS_MIN, K_FLAPS_MAX],
  module: [K_MODULE_MIN, K_MODULE_MAX],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: value - INTEGER_RANGES[id][0], group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const text = (id, name, def, group, hint) => ({ id, name, type: 'text', default: def, group, hint });
const colour = (id, name, def, group, hint) => ({ id, name, type: 'colour', default: def, group, hint });

const seconds = (s) => (s < 1 ? `${(s * 1000).toPrecision(3)} ms` : `${s.toPrecision(3)} s`);

const demo = mountDemo({
  name: 'Splitflap',
  pluginId: 'SF01',
  kind: 'effect',
  tagline:
    'The picture on a split-flap departures board. Every cell is a drum of flaps that can only turn forward, one flap at a time, at the motor’s rate: a change ripples across the board, a step brighter is one flip and a step darker is the whole drum round, a moving picture is never finished, and every flap in the air is a hinged plate falling under gravity — an inverted pendulum let go, which hesitates, slaps on to the stop and flutters — with its shading following its angle. The shaders here are the plugin’s own; the fall table, the drum print and the update decision are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/splitflap',
  page: 'https://stoatworks-labs.com/software/splitflap/',

  blurb:
    'It is Splitflap’s own GLSL, ported from the repository to WebGL2 — the motor that holds every cell’s state in a float texel included — with the fall table it solves on the CPU, its drum print, its onset detector and its update decision ported to JavaScript by hand; nothing checks that port but a reader. It runs on generated clips in this page, with the plugin’s own parameters and no install. No audio reaches it, so Onset mode never fires.',

  // The state is one RGBA32F texel per cell, ping-ponged between two float
  // render targets, exactly as in the plugin. Without EXT_color_buffer_float
  // the page says so rather than quantising the flap phase to 8 bits.
  needFloat: true,
  // With Fit off the surround is transparent, as it is in the plugin.
  showBackdrop: true,

  params: [
    integer('columns', 'Columns', K_COLUMNS_DEFAULT, 'Board',
      'Cells across the board, 4 to 96. FF_TYPE_INTEGER in the plugin; the kit has no integer control, so this is a dropdown of every value. Changing it mid-run re-maps the old state onto the new grid: each new cell takes the old cell at the same place on the board, mid-flip included.'),
    integer('rows', 'Rows', K_ROWS_DEFAULT, 'Board',
      'Cells down the board, 2 to 54. FF_TYPE_INTEGER in the plugin, a dropdown here. Regrids like Columns.'),
    bool('fit', 'Fit', 1, 'Board',
      'On: the cells stretch to fill the frame. Off: Cell Aspect is honoured and the board is centred with a transparent surround, which composites over the layers below.'),
    std('cellAspect', 'Cell Aspect', cellAspectToParam(0.75), 'Board', {
      display: (v) => `${cellAspectFromParam(v).toFixed(2)} wide over high`,
      hint: 'Width over height of one cell, 0.5 to 1.5. Only read with Fit off.',
    }),
    std('gap', 'Gap', 0.24, 'Board', {
      display: (v) => `${(gapFromParam(v) * 100).toFixed(1)} % of the cell`,
      hint: 'The frame between cells as a fraction of the cell’s width, 0 to 25 %.',
    }),
    std('split', 'Split Line', 0.25, 'Board', {
      display: (v) => `${(splitFromParam(v) * 100).toFixed(1)} % of the cell’s height`,
      hint: 'The hinge line across the middle of every cell, 0 to 12 % of the cell’s height.',
    }),

    opt('drum', 'Drum', ['Tones', 'Colours', 'Text'], K_DRUM_TONES, 'Drum',
      'What the flaps are printed with. Tones: N greys, the cell chasing its mean luma. Colours: N swatches along the Palette, the cell chasing its mean colour. Text: a fixed 45-character alphabet, blank first; a cell shows its character of the message where the clip is bright and the blank where it is not, so every letter is a run of flips from the blank.'),
    integer('flaps', 'Flaps', K_FLAPS_DEFAULT, 'Drum',
      'N, the flaps on each drum, 2 to 64, for Tones and Colours (the Text drum is its fixed alphabet). FF_TYPE_INTEGER in the plugin, a dropdown here. More flaps: finer tones, longer trips round the drum.'),
    opt('order', 'Drum Order', ['Dark to Light', 'Light to Dark', 'Shuffled'], K_ORDER_DARK_TO_LIGHT, 'Drum',
      'The order the flaps are printed in — the order IS the asymmetry. Dark to Light: one step brighter is one flip, one step darker is N − 1. Light to Dark: the reverse. Shuffled: a fixed shuffle seeded from a constant, so every step is some way round the drum. The Text blank stays at flap 0 in every order.'),
    opt('palette', 'Palette', PALETTES.map((pal) => pal.name), 0, 'Drum',
      'Six palettes authored in the plugin, sampled at N evenly spaced points for the Colours drum.'),
    text('text', 'Text', 'DEPARTURES', 'Drum',
      'The message for the Text drum, laid over the board row-major from the top left, wrapping at Columns; lower case is folded to upper, and a character the alphabet lacks is the blank.'),

    std('flipTime', 'Flip Time', flipTimeToParam(0.08), 'Motor', {
      display: (v) => `${seconds(flipTimeFromParam(v))} a flap`,
      hint: 'Seconds for one flap to fall, 30 ms to 1 s, geometric. The motor’s rate: a cell advances one flap per Flip Time, and the fall table is scaled so the plate lands in exactly that time.',
    }),
    std('stagger', 'Stagger', 0.15, 'Motor', {
      display: (v) => seconds(staggerFromParam(v)),
      hint: 'Per-cell start delay, up to a second, decided by a hash of the cell’s module — a start delay, not a phase offset. Zero leaves the flutter timer alone.',
    }),
    integer('module', 'Module Width', K_MODULE_DEFAULT, 'Motor',
      'Cells in a row that share one drive and so take the same start delay, 1 to 16. FF_TYPE_INTEGER in the plugin, a dropdown here.'),
    opt('update', 'Update', ['Continuous', 'Interval', 'Onset', 'Manual'], K_UPDATE_CONTINUOUS, 'Motor',
      'When the board refreshes its targets from the clip. Continuous: every frame. Interval: every Interval seconds, ticking from the moment the mode was chosen. Onset: when the audio detector fires — never, on this page, because no audio reaches it. Manual: only on Update Now. Every mode latches the first frame.'),
    std('interval', 'Interval', intervalToParam(2.0), 'Motor', {
      display: (v) => seconds(intervalFromParam(v)),
      hint: 'Seconds between board updates in Interval mode, 0.1 to 10, geometric. A stall longer than the interval gives one update, not a burst.',
    }),
    bool('updateNow', 'Update Now', 0, 'Motor',
      'FF_TYPE_EVENT in the plugin: one press, one refresh of every cell’s target, in any Update mode. The kit has no event control, so this is a button the page presses for one frame.'),

    colour('flapR', 'Flap Colour', 1.00, 'Look',
      'The lightest tone on the Tones drum; the plates and the blank are 6 % of it. Three FF_TYPE_RED/GREEN/BLUE parameters in the plugin, one swatch here as in a host.'),
    colour('flapG', 'FlapColour_Green', 0.93, 'Look'),
    colour('flapB', 'FlapColour_Blue', 0.80, 'Look'),
    std('light', 'Light Angle', 0.60, 'Look', {
      display: (v) => `${(lightAngleFromParam(v) * 180 / Math.PI).toFixed(1)}° ${lightAngleFromParam(v) >= 0 ? 'from above' : 'from below'}`,
      hint: 'The light’s elevation, −60° to +60°; 0.5 is straight on. Lambert on every flap: the front of a falling plate by cos(θ + angle), the back by −cos(θ + angle), 30 % ambient. The plate at rest is lit by cos(angle), which is even, so the slider’s two ends light a settled board the same.',
    }),
    std('bounce', 'Bounce', 0.40, 'Look', {
      display: (v) => `e = ${restitutionFromParam(v).toFixed(2)}`,
      hint: 'Coefficient of restitution at the stop, 0 to 0.6. 0 lands dead; at 0.6 the first rebound reaches 72°. Moving it re-solves the fall table (RK4 in double, a few milliseconds).',
    }),
    std('mix', 'Mix', 1.0, 'Look'),
  ],

  // A moving picture with a full contrast range is what a board chases; the
  // cards show the quantisation to tones and the regrid.
  sources: ['scene', 'bars', 'grid', 'ramp', 'spot', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Airport colours': { drum: 1, palette: 1 },
    'Departures, in text': { drum: 2, columns: 16 - K_COLUMNS_MIN, rows: 9 - K_ROWS_MIN, text: 'GATE 9  BOARDING' },
    'Slow board, long flips': { flipTime: flipTimeToParam(0.4), stagger: 0.6, update: 1, interval: intervalToParam(4.0) },
    'Manual: press Update Now': { update: 3, flipTime: flipTimeToParam(0.15) },
    'Big cells, full bounce': { columns: 8 - K_COLUMNS_MIN, rows: 4 - K_ROWS_MIN, flaps: 6 - K_FLAPS_MIN, flipTime: flipTimeToParam(0.5), bounce: 1.0, stagger: 0 },
    'Light to Dark (the asymmetry reversed)': { order: 1 },
    'Shuffled drum': { order: 2, flipTime: flipTimeToParam(0.05) },
    'Board on the layer below (Fit off)': { fit: 0, columns: 24 - K_COLUMNS_MIN, rows: 8 - K_ROWS_MIN, cellAspect: cellAspectToParam(1.2) },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Splitflap solves the flap’s fall once in double on the CPU (Flap.cpp: RK4 at a 1e-4 step, the impacts interpolated inside the step, the flutter ending at half a degree, sampled to a 2048-entry table in flip units), prints the drum (Drum.cpp: the tones, the six palettes, the 45-character alphabet, the three orders, the message layout), builds the font texture (Font.cpp), runs an onset detector (Onset.cpp), converts every control (Controls.cpp), and decides each frame’s update, geometry and state buffers (ProcessOpenGL). All of that is ported here function for function in JavaScript doubles. Nothing checks a port but a reader; the repository’s sftest checks the C++ and has never heard of this page.',
    'The GPU half is not a port. The four programs — vertex, copy, motor and board — are the plugin’s own GLSL, copied by demo/tools/splice_shaders.py, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of them drifts. The font’s glyph pictures are held to Font.cpp the same way.',
    'The state lives where the plugin keeps it: one RGBA32F texel per cell (flap on show, fall phase, latched target, timer), written by the motor shader into one of two float render targets sized to the grid and read back from the other next frame. A Columns or Rows change writes a buffer of the new size while reading the old one, as in the plugin, so a regrid keeps every flap. This page needs EXT_color_buffer_float for that, and says so rather than falling back to 8 bits.',
    'No audio reaches a browser page. The plugin’s Audio FFT buffer (64 bins from Resolume) is absent rather than present and dead; the ported onset detector is handed silence every frame, which it reads exactly as the plugin reads an unrouted input. Onset mode therefore latches the first frame and never fires again here.',
    'Update Now is FF_TYPE_EVENT in the plugin. The kit has no event control, so it is a button the page reads as one press and releases after one frame, which is what the plugin counts.',
    'Columns, Rows, Flaps and Module Width are FF_TYPE_INTEGER in the plugin. The kit has no integer control, so they are dropdowns of every value in the plugin’s range.',
    'The plugin’s Clock measures whether the host sends seconds or milliseconds over several frames before it believes a delta. This page’s clock is the kit’s time with the unit declared as seconds, as the repository’s harness declares it; the vote never runs here. The 0.25 s clamp on a frame delta is ported, so a stall, a paused tab or Restart advances the board by a quarter second at most, not the whole drum.',
    'On the very first frame the plugin binds the buffer it is about to write as the previous state too, knowing the shader never reads it (OldGrid is zero). WebGL refuses a draw that samples its own render target, so that one frame binds a 1 × 1 texel instead. The shader’s arithmetic is unchanged.',
    'The plugin reads the clip through the host’s MaxUV, because Resolume pads a 720-wide texture to 768; a browser texture is unpadded, so MaxUV is (1, 1) here. The cell mean is sixteen bilinear taps at a whole mip level of that copy, as in the plugin, but the browser’s driver generates the mip chain and its bilinear weights are its own, so a cell’s mean on a real picture can sit a tone away from the plugin’s.',
    'The plugin’s numerical proof — (t − c) mod N flips per cell, the up/down asymmetry, the settle bound, the flap’s projected height against an independent rigid-body solution, the state across a resize and a regrid, the primed detector — is an offline harness in the repository. Nothing on this page measures anything; the line under the canvas reports what the ported update decision did.',
    'The About block is absent here, as on every page in this suite.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// After mount: Update Now is an event, so its row gets a button rather than
// the kit's On/Off toggle. The toggle would work — the renderer releases the
// value after one frame — but a toggle reading "Off" for a control that
// cannot be on is the wrong picture of the plugin's inspector, where a host
// draws an event as a momentary button.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  for (const row of document.querySelectorAll('.prow--boolean')) {
    const label = row.querySelector('.prow__name');
    if (!label || label.textContent !== 'Update Now') continue;
    const toggle = row.querySelector('.prow__toggle');
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'btn';
    button.textContent = 'Update Now';
    button.title = 'One press: every cell takes a new target from the clip, in any Update mode.';
    button.addEventListener('click', () => {
      demo.params.set('updateNow', 1);
      demo.redraw();
    });
    if (toggle) toggle.replaceWith(button);
    else row.append(button);
  }

  //-------------------------------------------------------------------------
  // The line under the canvas. It reports the ported update decision's own
  // numbers — which mode, how many refreshes, when the next Interval tick is
  // due — and the board's size, because a board in Manual mode that is not
  // moving is easy to read as nothing happening.
  //-------------------------------------------------------------------------
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    const MODES = ['Continuous', 'Interval', 'Onset', 'Manual'];
    setInterval(() => {
      const t = telemetry;
      if (!t.columns) return;
      let when = '';
      if (t.mode === K_UPDATE_INTERVAL && t.nextTick >= 0) when = `, next tick in ${Math.max(0, t.nextTick - t.now).toFixed(1)} s`;
      if (t.mode === K_UPDATE_ONSET) when = `, ${t.onsets} onset${t.onsets === 1 ? '' : 's'} (no audio reaches this page)`;
      line.textContent =
        `${t.columns} × ${t.rows} cells, ${t.flaps} flaps a drum, ${seconds(t.flipTime)} a flap; `
        + `the flutter ends ${t.bounceEnd.toFixed(2)} flip units after release. `
        + `Update: ${MODES[t.mode]}${when}; ${t.fires.toLocaleString('en-GB')} refresh${t.fires === 1 ? '' : 'es'} so far. `
        + `Last frame delta ${(t.dt * 1000).toFixed(1)} ms (clamped at 250).`;
    }, 250);
  }
}
