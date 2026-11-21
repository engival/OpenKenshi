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
// The buffer is our own static one on purpose: a null buffer makes setvbuf malloc 4 KB at startup, which moves every later heap
// allocation - and 6 of the scene test's rain / weather scenes (rock_arch, greenlands_bridge, swamp_dawn_rain, swamp_noon_rain,
// petal_hills, riveroverlook1) then render a different frame under render.thread=lockstep (kenshi-k16.2.461's findings: the malloc
// version failed lockstep_check's scene half with rmse up to 15.8, this one passes all 49; whatever those scenes read moves with the
// heap, which is worth its own look).
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
