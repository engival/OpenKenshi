#pragma once
#include <string>

// Where the client's log goes (kenshi-379.31.9.1). Windows: OpenKenshi.exe is a GUI-subsystem program (no console window opens), so stdout and
// stderr are redirected at startup to <user data>/logs/openkenshi.log (the previous run's kept as openkenshi.log.1). --console or
// KCLIENT_CONSOLE=1 opts in to a console instead: the parent's (cmd / a terminal it was started from) when there is one, else a new window.
// Linux and macOS: nothing changes (platform/posix/console.cpp does nothing).
namespace plat {

// Call first thing in the program (before anything prints or the crash recorder installs). argv are the command-line words.
void setupLog(int argc, char** argv);
// The log file's path, empty when the log is not a file (a console, or a POSIX terminal).
std::string logPath();

}
