// KCLIENT_CATCHLOG=1 (kenshi-6q7f, dev-only): see catchlog.hpp. Line: CATCH id tick newest(age) step fields; classify offline (findings/kenshi-6q7f/classify.py).
#include "dev/catchlog.hpp"

#include <cstdio>
#include <cstdlib>

namespace catchlog
{
bool gOn = false;
void init() { gOn = std::getenv("KCLIENT_CATCHLOG") != nullptr; }
void noteSlow(const SimView::CatchJump& j)
{
	fprintf(stderr, "CATCH id %llu tick %.2f newest %.0f age %.2f dt %.4f clk %.3f snap %d ease %d jump %.2f off %.2f wv %.1f pwv %.1f base %.1f,%.1f drawn %.1f,%.1f pair %llu..%llu iz %llu prev %llu..%llu route %u seen %u\n",
		(unsigned long long)j.id, j.tick, j.newest, j.newest - j.tick, j.dtSim, j.clockMove, (int)j.clockSnapped, (int)j.easing, j.jump, j.off, j.wv, j.prevWv, j.baseX, j.baseZ, j.drawnX, j.drawnZ,
		(unsigned long long)j.ia, (unsigned long long)j.ib, (unsigned long long)j.iz, (unsigned long long)j.prevIa, (unsigned long long)j.prevIb, j.route, j.routeSeen);
}
}
