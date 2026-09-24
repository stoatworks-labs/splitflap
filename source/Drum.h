#pragma once

#include "Controls.h"

#include <cstdint>
#include <string>
#include <vector>

/**
    What is printed on the flaps.

    A drum is N flaps in a fixed order, and a cell can only turn forward
    through them. This file decides what each flap carries, once, on the CPU,
    and hands it to the shaders as a small texture: the motor pass compares a
    cell's mean against every flap to pick the nearest, and the board pass
    draws whatever the flap it has reached is printed with.

    Three drums:

    - **Tones**: N greys from dark to light. `Drum Order` prints them
      ascending, descending or shuffled -- the order is the asymmetry: with
      dark -> light, one step brighter is one flip and one step darker is
      N - 1.
    - **Colours**: N swatches sampled along a palette, same three orders.
    - **Text**: a fixed alphabet of `kAlphabet` characters, blank first, so
      a cell's target is either its message character or the blank. `Drum
      Order` keeps the blank at flap 0 and orders the rest.

    Shuffles are seeded from a constant with an integer hash, so a shuffled
    drum is the same drum in every session and in the harness.
*/
namespace splitflap::drum
{

/// The Text drum. Blank first; anything not in it draws as the blank.
extern const char* const kAlphabet;
int AlphabetSize();

int PaletteCount();
const char* PaletteName( int index );

/// One flap per row: rgb is the printed colour (Colours), a is the tone
/// (Tones, 0..1) or the character code (Text). The mode decides which is read.
struct Print
{
	int flaps = 0;
	std::vector< float > rgba;

	float tone( int flap ) const
	{
		return rgba[ static_cast< size_t >( flap ) * 4 + 3 ];
	}
	int code( int flap ) const
	{
		return static_cast< int >( rgba[ static_cast< size_t >( flap ) * 4 + 3 ] + 0.5f );
	}
};

Print Build( DrumMode mode, int flaps, DrumOrder order, int palette );

/// The message laid over the board: the drum index of each cell's character,
/// row-major from the top left, wrapping at `columns`; 0 (the blank) past
/// the end of the text and for any character the drum does not carry.
/// Lower case is folded to upper.
std::vector< float > Message( const std::string& text, int columns, int rows, const Print& textPrint );

/// PCG-style integer mixer: exact, the same everywhere.
uint32_t Hash( uint32_t x );

} // namespace splitflap::drum
