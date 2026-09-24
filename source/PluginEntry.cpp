/**
	The FF_EFFECT registration, and nothing else.

	**This file is listed directly in the MODULE target, not in the shared
	object library.** `CFFGLPluginInfo` registers itself from a file-scope
	constructor and nothing ever references it by name, so in a static archive
	the linker is entitled to drop the whole translation unit -- giving a bundle
	that loads, exports `plugMain`, and reports that it contains no plugins.

	    nm -gU Splitflap.bundle/Contents/MacOS/Splitflap | grep plugMain

	`SF01`: four characters, unique across the fleet.
*/
#include "Splitflap.h"

static CFFGLPluginInfo PluginInfo(
	PluginFactory< splitflap::SplitflapPlugin >,             // Create method
	"SF01",                                                  // Plugin unique ID of maximum length 4
	"SW Splitflap",                                          // Plugin name
	2,                                                       // API major version number
	1,                                                       // API minor version number
	0,                                                       // Plugin major version number
	1,                                                       // Plugin minor version number
	FF_EFFECT,                                               // Plugin type
	"The picture on a split-flap departures board.\n\n"
	"Every cell is a drum of flaps printed with greys, colours or characters, and a drum can only turn forward, "
	"one flap at a time, at the motor's rate. Point the board at the clip and the board chases it: a change "
	"ripples across the cells as a clatter, brightening by a step is one flip and darkening is the whole drum, "
	"and every flap in the air is a hinged plate falling under gravity, shaded by its angle, caught with a "
	"bounce.\n\n"
	"Refresh the targets continuously, on an interval, on an audio onset, or by hand with Update Now. "
	"The Text drum shows a typed message where the clip is bright.",// Plugin description
	"Splitflap FFGL effect"                                  // About
);

extern "C" const char* SplitflapBuildStamp()
{
	return "splitflap " SPLITFLAP_VERSION " built " __DATE__ " " __TIME__;
}
