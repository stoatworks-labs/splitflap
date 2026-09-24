#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family, carried from orrery. The rest of
    the repos get a rotating log, a crash report and a diagnostics bundle; an
    FFGL plugin gets only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- a plugin is a
      list of sliders in someone else's inspector.

    ## Why this exists here, specifically

    A board that never flips fails in ways an operator cannot tell apart from
    the front: the layer shows the clip, or black, and nothing clatters. That
    one symptom covers several faults, and only a log distinguishes them:

    - **A shader would not compile.** Which of the three it was, next to the
      GL vendor, renderer and version strings, because a shader that builds on
      one machine and not another is a driver answer, not a source answer.
    - **No audio reached the plugin** in Onset mode. Resolume fills an
      `FF_USAGE_FFT` buffer parameter; if nothing is routed to the layer every
      bin is zero, no onset ever fires, and the board latches its first picture
      for ever -- which looks exactly like a broken one. Logged as a
      *transition*, so a session that never saw a signal says so in one line.
    - **The host clock unit.** Resolume sends milliseconds and an offline
      harness sends seconds; the fleet has paid for that confusion twice. What
      the clock settled on is stated outright.

    ## Rate

    `ProcessOpenGL` runs fifty times a second. Nothing here is called from a
    per-frame path except through `stateChanged`, which logs a *transition* --
    so a board that sits idle for an hour writes one line, not 180,000.
*/
namespace splitflap::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Log `message` only when it differs from the last message logged under `key`.
/// For the per-frame paths, where the interesting event is the change and the
/// steady state is noise.
void stateChanged( const std::string& key, const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace splitflap::diag
