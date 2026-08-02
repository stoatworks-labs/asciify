#pragma once

#include <string>

/**
    Logging for a plugin that lives inside somebody else's process.

    A small member of the fleet's `diag` family. The rest of the repos get a
    rotating log, a crash report and a diagnostics bundle; an FFGL effect gets
    only the log, for two reasons:

    - **No crash handler.** A plugin loaded into Resolume must not install a
      process-wide signal handler. It would intercept faults that are not ours
      and interfere with the host's own handling. A plugin has no business
      deciding what happens when Resolume dies.
    - **No bundle command.** There is no UI to hang one off -- an effect is a
      list of sliders in someone else's inspector.

    What it covers is the failure that actually happens: `InitGL` returning
    `FF_FAIL` because one of the three shaders would not compile. From the
    operator's side that looks like "the effect does nothing", with no message
    anywhere -- and with three passes it also records *which* one, which is the
    difference between a five-minute fix and an afternoon. The GL vendor and
    version strings go in next to it, because a shader that compiles on one
    machine and not on another is a driver answer, not a source answer.
*/
namespace asciify::diag
{

/// Open the log file and record the plugin build, once per process.
void init();

void info( const std::string& message );
void warn( const std::string& message );
void error( const std::string& message );

/// Full path of the log file, for the README to point at.
std::string logPath();

} // namespace asciify::diag
