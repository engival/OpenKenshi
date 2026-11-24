// DEV ONLY (fenced by KSIM_DEV, ARCHITECTURE "Dev-only code is fenced"). The test phase timer (kenshi-k16.15.54): with KSIM_TEST_PHASES set,
// the process prints one PHASE line per phase at exit (wall + CPU seconds of the process, exclusive of nested phases): data (game data parse /
// merge), world (world + town build, spawn), nav (navmesh / navigator open), combat (CombatData build), tick (Simulation::tick, with the tick
// count), teardown (World destruction), other (the rest: the test's own setup, checks and process start / exit). KSIM_TEST_PHASES=1 prints to
// stderr; any other value is a file the lines are appended to. Unset: one cached getenv per scope, no output. Main thread only.
#pragma once

#if KSIM_DEV

namespace ksim::dev {

enum class Phase { Data, World, Nav, Combat, Tick, Teardown, Count };

class PhaseScope
{
public:
	explicit PhaseScope(Phase phase);
	~PhaseScope();
	PhaseScope(const PhaseScope&) = delete;
	PhaseScope& operator=(const PhaseScope&) = delete;
private:
	bool active_;
};

// A World's destructor body starts the teardown clock; this member, declared first (destroyed last), stops it.
void teardownBegin();
struct TeardownEnd { ~TeardownEnd(); };

} // namespace ksim::dev

#define KSIM_PHASE(name) ::ksim::dev::PhaseScope ksimPhaseScope_(::ksim::dev::Phase::name)
#define KSIM_TEARDOWN_BEGIN() ::ksim::dev::teardownBegin()
#define KSIM_TEARDOWN_END_MEMBER [[no_unique_address]] ::ksim::dev::TeardownEnd teardownEnd_;

#else

#define KSIM_PHASE(name) ((void) 0)
#define KSIM_TEARDOWN_BEGIN() ((void) 0)
#define KSIM_TEARDOWN_END_MEMBER

#endif
