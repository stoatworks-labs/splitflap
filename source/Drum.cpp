#include "Drum.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace splitflap::drum
{
namespace
{
struct Rgb
{
	float r, g, b;
};

struct Palette
{
	const char* name;
	std::vector< Rgb > stops;
};

/// Authored here, not imported. Sampled at N evenly spaced points, so a
/// 12-flap drum on Airport carries black, white, yellow, orange, red and the
/// blends between.
const std::vector< Palette >& palettes()
{
	static const std::vector< Palette > list = {
		{ "Amber", { { 0.05f, 0.03f, 0.00f }, { 1.00f, 0.62f, 0.05f }, { 1.00f, 0.92f, 0.60f } } },
		{ "Airport", { { 0.02f, 0.02f, 0.02f }, { 0.95f, 0.95f, 0.92f }, { 1.00f, 0.85f, 0.00f }, { 1.00f, 0.45f, 0.00f }, { 0.85f, 0.05f, 0.05f } } },
		{ "Rainbow", { { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } } },
		{ "Ocean", { { 0.02f, 0.05f, 0.15f }, { 0.00f, 0.35f, 0.60f }, { 0.20f, 0.80f, 0.85f }, { 0.90f, 0.98f, 1.00f } } },
		{ "Ember", { { 0.02f, 0.00f, 0.00f }, { 0.50f, 0.02f, 0.00f }, { 1.00f, 0.35f, 0.00f }, { 1.00f, 0.85f, 0.30f } } },
		{ "Candy", { { 0.95f, 0.20f, 0.50f }, { 1.00f, 0.85f, 0.20f }, { 0.30f, 0.90f, 0.60f }, { 0.30f, 0.60f, 1.00f }, { 0.70f, 0.30f, 0.90f } } },
	};
	return list;
}

Rgb sample( const Palette& palette, float s )
{
	const size_t n = palette.stops.size();
	if( n == 1 )
		return palette.stops[ 0 ];
	const float f  = std::min( std::max( s, 0.0f ), 1.0f ) * static_cast< float >( n - 1 );
	const size_t i = std::min( static_cast< size_t >( f ), n - 2 );
	const float w  = f - static_cast< float >( i );
	const Rgb& a   = palette.stops[ i ];
	const Rgb& b   = palette.stops[ i + 1 ];
	return { a.r + ( b.r - a.r ) * w, a.g + ( b.g - a.g ) * w, a.b + ( b.b - a.b ) * w };
}

/// Fisher-Yates over [first, n), with the hash as the generator. Seeded from a
/// constant: a shuffled drum is one drum, not a new one per session.
void shuffle( std::vector< int >& order, int first )
{
	constexpr uint32_t kSeed = 0x5F17F1A9u;
	for( int i = static_cast< int >( order.size() ) - 1; i > first; --i )
	{
		const uint32_t h = Hash( kSeed ^ ( static_cast< uint32_t >( i ) * 0x9E3779B9u ) );
		const int j      = first + static_cast< int >( h % static_cast< uint32_t >( i - first + 1 ) );
		std::swap( order[ static_cast< size_t >( i ) ], order[ static_cast< size_t >( j ) ] );
	}
}

/// The flap order as a permutation of the natural order. `fixedFirst` keeps
/// element 0 (the Text blank) where it is.
std::vector< int > ordering( int n, DrumOrder order, bool fixedFirst )
{
	std::vector< int > out( static_cast< size_t >( n ) );
	for( int i = 0; i < n; ++i )
		out[ static_cast< size_t >( i ) ] = i;
	const int first = fixedFirst ? 1 : 0;
	switch( order )
	{
	case kOrderLightToDark:
		std::reverse( out.begin() + first, out.end() );
		break;
	case kOrderShuffled:
		shuffle( out, first );
		break;
	default:
		break;
	}
	return out;
}
} // namespace

const char* const kAlphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.:!?/'&";

int AlphabetSize()
{
	return static_cast< int >( std::strlen( kAlphabet ) );
}

int PaletteCount()
{
	return static_cast< int >( palettes().size() );
}

const char* PaletteName( int index )
{
	const auto& list = palettes();
	if( index < 0 || index >= static_cast< int >( list.size() ) )
		return "?";
	return list[ static_cast< size_t >( index ) ].name;
}

uint32_t Hash( uint32_t x )
{
	// The PCG output permutation (RXS-M-XS on 32 bits), exact in integers.
	x = x * 747796405u + 2891336453u;
	x = ( ( x >> ( ( x >> 28u ) + 4u ) ) ^ x ) * 277803737u;
	return ( x >> 22u ) ^ x;
}

Print Build( DrumMode mode, int flaps, DrumOrder order, int palette )
{
	Print print;
	if( mode == kDrumText )
	{
		const int n                     = AlphabetSize();
		print.flaps                     = n;
		const std::vector< int > perm   = ordering( n, order, true );
		print.rgba.assign( static_cast< size_t >( n ) * 4, 0.0f );
		for( int k = 0; k < n; ++k )
		{
			const int code                                    = static_cast< unsigned char >( kAlphabet[ perm[ static_cast< size_t >( k ) ] ] );
			print.rgba[ static_cast< size_t >( k ) * 4 + 3 ] = static_cast< float >( code );
		}
		return print;
	}

	const int n = std::min( std::max( flaps, kFlapsMin ), kFlapsMax );
	print.flaps = n;
	print.rgba.assign( static_cast< size_t >( n ) * 4, 0.0f );
	const std::vector< int > perm = ordering( n, order, false );
	const Palette& pal            = palettes()[ static_cast< size_t >( std::min( std::max( palette, 0 ), PaletteCount() - 1 ) ) ];
	for( int k = 0; k < n; ++k )
	{
		const float s = static_cast< float >( perm[ static_cast< size_t >( k ) ] ) / static_cast< float >( n - 1 );
		const Rgb c   = sample( pal, s );
		float* row    = &print.rgba[ static_cast< size_t >( k ) * 4 ];
		row[ 0 ]      = c.r;
		row[ 1 ]      = c.g;
		row[ 2 ]      = c.b;
		row[ 3 ]      = s;
	}
	return print;
}

std::vector< float > Message( const std::string& text, int columns, int rows, const Print& textPrint )
{
	std::vector< float > cells( static_cast< size_t >( columns ) * static_cast< size_t >( rows ), 0.0f );
	for( size_t i = 0; i < text.size() && i < cells.size(); ++i )
	{
		const int code = std::toupper( static_cast< unsigned char >( text[ i ] ) );
		for( int k = 0; k < textPrint.flaps; ++k )
		{
			if( textPrint.code( k ) == code )
			{
				cells[ i ] = static_cast< float >( k );
				break;
			}
		}
	}
	return cells;
}

} // namespace splitflap::drum
