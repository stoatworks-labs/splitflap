#pragma once

#include <FFGLSDK.h>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

/**
    Every parameter, and what its host-side value means.

    ## The two units

    - **`FF_TYPE_STANDARD` is 0..1**, always. `SetParamInfo` clamps the default
      into that range before `SetParamRange` could widen it, so a standard
      parameter that means anything else is mapped here, in a named function,
      and nowhere else. Both the plugin and the harness read the mapping from
      this one file, so there is only ever one answer to what a slider means.
    - **`FF_TYPE_INTEGER` holds a real integer** with a real range: Columns,
      Rows, Flaps and Module Width are counts, and the clamp does not apply.
    - **`FF_TYPE_OPTION` holds the element VALUE**, which for every dropdown
      here is its index. Its SDK range reads back 0..1 whatever the element
      count (the fleet trap), so the harness's `--list` prints the real range
      for the sweep, and `OptionIndex()` accepts either an index or a
      normalised 0..1.
    - **`FF_TYPE_BOOLEAN` is 0 or 1**, read as `> 0.5f`.
    - **`FF_TYPE_BUFFER`** holds the host's 64-bin spectrum. The host writes
      it, not the operator.

    ## Order is load-bearing

    The host draws parameters in declaration order and `SetParamGroup`
    collapses *runs* of the same group name into one fold, so an id moved out
    of its run splits its group in two. The enum below is the inspector, top
    to bottom.

    ## Names

    FFGL truncates a parameter name at 16 characters, silently. `sftest
    --names` lists any that are over, and any duplicate: `--set`, the cue
    script and `tools/sweep.py` all find a parameter by its name.
*/
namespace splitflap
{
enum ParamId : FFUInt32
{
	// -- Board ---------------------------------------------------------------
	PT_COLUMNS,
	PT_ROWS,
	PT_FIT,
	PT_CELL_ASPECT,
	PT_GAP,
	PT_SPLIT,

	// -- Drum ----------------------------------------------------------------
	PT_DRUM,
	PT_FLAPS,
	PT_ORDER,
	PT_PALETTE,
	PT_TEXT,

	// -- Motor ---------------------------------------------------------------
	PT_FLIP_TIME,
	PT_STAGGER,
	PT_MODULE,
	PT_UPDATE,
	PT_INTERVAL,
	PT_UPDATE_NOW,
	PT_AUDIO,

	// -- Look ----------------------------------------------------------------
	PT_FLAP_R,
	PT_FLAP_G,
	PT_FLAP_B,
	PT_LIGHT,
	PT_BOUNCE,
	PT_MIX,

	// -- About ---------------------------------------------------------------
	// One text line and one button per link. Its size is decided by
	// StoatworksAbout.h at compile time, so Splitflap.cpp static_asserts this
	// run against `about::kParamCount`: five entries now the generated header
	// carries the user guide's URL (text, User guide, Project page, Source on
	// GitHub, Support the work).
	PT_ABOUT_TEXT,
	PT_ABOUT_BUTTON_1,
	PT_ABOUT_BUTTON_2,
	PT_ABOUT_BUTTON_3,
	PT_ABOUT_BUTTON_4,

	PT_COUNT_
};

/// Spectrum bins in the Audio buffer parameter.
constexpr int kAudioBins = 64;

// -- Ranges of the integer parameters -----------------------------------------
constexpr int kColumnsMin = 4, kColumnsMax = 96, kColumnsDefault = 32;
constexpr int kRowsMin = 2, kRowsMax = 54, kRowsDefault = 18;
constexpr int kFlapsMin = 2, kFlapsMax = 64, kFlapsDefault = 12;
constexpr int kModuleMin = 1, kModuleMax = 16, kModuleDefault = 1;

// -- Options ----------------------------------------------------------------
enum DrumMode
{
	kDrumTones   = 0,
	kDrumColours = 1,
	kDrumText    = 2,
	kDrumCount   = 3
};

enum DrumOrder
{
	kOrderDarkToLight = 0,
	kOrderLightToDark = 1,
	kOrderShuffled    = 2,
	kOrderCount       = 3
};

enum UpdateMode
{
	kUpdateContinuous = 0,
	kUpdateInterval   = 1,
	kUpdateOnset      = 2,
	kUpdateManual     = 3,
	kUpdateCount      = 4
};

/// An option's stored value is its element index; a host or a script may
/// also hand over a normalised 0..1, which this folds back onto an index.
int OptionIndex( float value, int count );

// -- Standard (0..1) parameters, in engineering units ----------------------------

/// Width over height of one cell, 0.5..1.5, linear. Only read with Fit off.
float CellAspectFromParam( float value );
float CellAspectToParam( float aspect );

/// The frame between cells as a fraction of the cell's width, 0..0.25.
float GapFromParam( float value );

/// The split line's thickness as a fraction of the cell's height, 0..0.12.
float SplitFromParam( float value );

/// Seconds for one flap to fall, 0.03..1.0, geometrically. The motor's rate:
/// a cell advances one flap per Flip Time.
float FlipTimeFromParam( float value );
float FlipTimeToParam( float seconds );

/// Per-cell start delay, 0..1 s, linear. A cell waits up to this long,
/// decided by a hash of its module, before its first flip.
float StaggerFromParam( float value );

/// Seconds between board updates in Interval mode, 0.1..10, geometrically.
float IntervalFromParam( float value );
float IntervalToParam( float seconds );

/// The light's elevation in radians, -60 to +60 degrees, linear; 0.5 is
/// straight on. Positive is from above.
float LightAngleFromParam( float value );

/// Coefficient of restitution at the stop, 0..0.6, linear. 0 lands dead.
float RestitutionFromParam( float value );

} // namespace splitflap
