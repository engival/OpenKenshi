#include "platform/console.hpp"

#include <cstdio>

// Linux and macOS keep stdout / stderr on the terminal that started the client (platform/console.hpp). Both get their buffering here,
// as the Windows log does (platform/windows/console.cpp): the C library block-buffers stdout when it is not a terminal, so a run whose
// stdout goes to a file or a pipe (the playtest / capture runs redirect it to their run log) held its last 4 KB in memory and a run
// that ends on a signal (SIGTERM from the wrapper, a window close, a crash) never wrote it - the attack click line of the run log
// kenshi-4bps.1 lost, making an order look "never sent" (kenshi-k16.2.461). Line buffered writes each complete line out at once, so
// every line a run printed is in the file whatever ends it; a partial line is all that can be missing.
// setupLog is the first thing sokol_main does (main.cpp), before anything prints, which is when setvbuf may still change the mode.
// The cost is one write per line instead of one per 4 KB: the playtest's stdout is a line every few frames at most, and the pace log
// (dev/pacelog.cpp, one line a second) and the per-frame dev logs go to stderr, which was already unbuffered (the C library makes it
// so; nothing to do here).
// The buffer is our own static one (kept: it costs nothing and avoids a 4 KB malloc at startup). It is NOT needed for scene
// determinism: kenshi-k16.2.461 blamed a startup malloc moving the heap for 6 rain scenes changing under render.thread=lockstep,
// but kenshi-k16.2.465 showed the cause was a run without client/data (foliage_plants.txt missing, so the material rule alone
// decided the trees); a missing client/data is now a fatal startup error (kenshi-k16.2.467).
namespace plat {

namespace {
char gOutBuf[4096];
}

void setupLog(int, char**)
{
	setvbuf(stdout, gOutBuf, _IOLBF, sizeof gOutBuf);
}

std::string logPath() { return std::string(); }

}
