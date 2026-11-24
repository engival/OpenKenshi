// DEV ONLY (include/ksim/dev/test_phases.hpp).
#include "ksim/dev/test_phases.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#if KSIM_DEV

namespace ksim::dev {

namespace {

constexpr int kPhases = static_cast<int>(Phase::Count);
const char* const kNames[kPhases] = {"data", "world", "nav", "combat", "tick", "teardown"};

struct Clock
{
	double wall = 0.0;
	double cpu = 0.0;
};

Clock now()
{
	timespec w{}, c{};
	clock_gettime(CLOCK_MONOTONIC, &w);
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c);
	return {w.tv_sec + w.tv_nsec * 1e-9, c.tv_sec + c.tv_nsec * 1e-9};
}

struct State
{
	const char* sink = nullptr;
	bool on = false;
	std::thread::id owner;
	Clock mark;
	Clock begin;
	Clock sum[kPhases];
	std::uint64_t count[kPhases] = {};
	int stack[16] = {};
	int depth = 0;
	Clock teardownStart;
	bool tearing = false;
};

State s;

void report()
{
	const Clock end = now();
	std::FILE* out = std::strcmp(s.sink, "1") == 0 ? stderr : std::fopen(s.sink, "a");
	if (out == nullptr)
		return;
	Clock phased;
	for (int i = 0; i < kPhases; ++i)
	{
		phased.wall += s.sum[i].wall;
		phased.cpu += s.sum[i].cpu;
		std::fprintf(out, "PHASE %s wall=%.3f cpu=%.3f n=%llu\n", kNames[i], s.sum[i].wall, s.sum[i].cpu, (unsigned long long) s.count[i]);
	}
	std::fprintf(out, "PHASE other wall=%.3f cpu=%.3f n=0\n", end.wall - s.begin.wall - phased.wall, end.cpu - phased.cpu);
	std::fprintf(out, "PHASE total wall=%.3f cpu=%.3f n=0\n", end.wall - s.begin.wall, end.cpu);
	if (out != stderr)
		std::fclose(out);
}

bool enabled()
{
	static const bool on = [] {
		s.sink = std::getenv("KSIM_TEST_PHASES");
		if (s.sink == nullptr || *s.sink == '\0')
			return false;
		s.owner = std::this_thread::get_id();
		s.begin = s.mark = now(); // wall of "total" runs from library init (the process start, to within the loader); cpu from exec
		std::atexit(report);
		return true;
	}();
	return on;
}

// Charges the time since the last mark to the phase on top of the stack (exclusive: a nested scope takes its own time out of its parent).
void charge()
{
	const Clock t = now();
	if (s.depth > 0)
	{
		s.sum[s.stack[s.depth - 1]].wall += t.wall - s.mark.wall;
		s.sum[s.stack[s.depth - 1]].cpu += t.cpu - s.mark.cpu;
	}
	s.mark = t;
}

const bool kInit = enabled(); // starts the wall clock at library init

} // namespace

PhaseScope::PhaseScope(Phase phase)
	: active_(enabled() && std::this_thread::get_id() == s.owner && s.depth < 15)
{
	if (!active_)
		return;
	charge();
	s.stack[s.depth++] = static_cast<int>(phase);
	++s.count[static_cast<int>(phase)];
}

PhaseScope::~PhaseScope()
{
	if (!active_)
		return;
	charge();
	--s.depth;
}

void teardownBegin()
{
	if (enabled() && std::this_thread::get_id() == s.owner && s.depth == 0)
	{
		s.teardownStart = now();
		s.tearing = true;
	}
}

TeardownEnd::~TeardownEnd()
{
	if (!s.tearing)
		return;
	s.tearing = false;
	const Clock t = now();
	s.sum[static_cast<int>(Phase::Teardown)].wall += t.wall - s.teardownStart.wall;
	s.sum[static_cast<int>(Phase::Teardown)].cpu += t.cpu - s.teardownStart.cpu;
	++s.count[static_cast<int>(Phase::Teardown)];
}

} // namespace ksim::dev

#endif
