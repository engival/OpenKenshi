// The tick profiler (docs/PROFILING.md): only a KSIM_PROFILE build has one. The ring never grows, the sections of a tick add up to
// no more than the tick, a save lands in the Save section, and the JSON line names every section. An OFF build checks the macros
// compile away (the build itself is the test: Simulation has no profiler()).

#include "ksim/core/simulation.hpp"

#include "ksim/dev/island_probe.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <iostream>
#include <vector>
#include <sstream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	try
	{
		// The island probe (kenshi-k16.6.305): three clusters 1500 u apart in a chain of cells, one lone point, one inactive point.
		{
			ksim::dev::IslandProbe uf;
			const std::vector<double> x = {0, 100, 200, 5000, 5100, 9000, 0};
			const std::vector<double> z = {0, 0, 0, 0, 0, 9000, 100};
			std::vector<char> active(7, 1);
			active[6] = 0;
			uf.reset(7);
			uf.joinByRange(x, z, active, 1500.0);
			const ksim::dev::IslandStats st = uf.stats(active);
			test::require(st.components == 3 && st.largest == 3 && st.second == 2 && st.singletons == 1 && st.nonSingleton == 2, "island probe: 3 + 2 + 1");
			uf.reset(7);
			uf.joinByRange(x, z, active, 6000.0);
			test::require(uf.stats(active).components == 2, "island probe: a larger range joins the two clusters, the far point stays");
		}
#if KSIM_PROFILE
		using namespace ksim;
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		profile::Profiler& profiler = sim.profiler();
		const std::uint64_t before = profiler.totals().ticks;

		sim.run(10);
		test::require(profiler.totals().ticks == before + 10, "every tick is counted");
		const profile::Summary few = profiler.summary();
		test::require(few.ticks == before + 10, "the window holds the ticks run so far");

		// The sections of the tick add up to no more than the tick itself; Simlink and Save are outside it.
		std::uint64_t inside = 0;
		for (std::size_t i = 0; i < static_cast<std::size_t>(profile::Section::Simlink); ++i)
			inside += profiler.totals().ns[i];
		test::require(inside > 0 && inside <= profiler.totals().tickNs, "the tick's sections sum to at most the tick");

		// A save is timed into Save and joins the next tick's sample.
		std::ostringstream out;
		sim.save(out);
		test::require(profiler.totals().ns[static_cast<std::size_t>(profile::Section::Save)] == 0, "a save is pending until the tick that follows");
		sim.run(1);
		test::require(profiler.totals().ns[static_cast<std::size_t>(profile::Section::Save)] > 0, "the save time is in the next tick's sample");

		// The ring is fixed: more ticks than it holds do not grow it.
		sim.run(profile::Profiler::kWindow + 100);
		const profile::Summary full = profiler.summary();
		test::require(full.ticks == profile::Profiler::kWindow, "the window is capped at kWindow ticks");
		test::require(full.tickP50Us <= full.tickP99Us && full.tickP99Us <= full.tickMaxUs && full.tickMeanUs > 0.0, "the tick percentiles are ordered");
		test::require(profiler.totals().ticks == before + 10 + 1 + profile::Profiler::kWindow + 100, "the totals keep counting past the window");

		const std::string json = profiler.json();
		test::require(json.front() == '{' && json.back() == '}' && json.find("\"tick_us\"") != std::string::npos, "the JSON line is one object");
		for (const char* name : profile::kSectionNames)
			test::require(json.find(std::string("\"") + name + "\":") != std::string::npos, std::string("the JSON line names section ") + name);
		test::require(json.find('\n') == std::string::npos, "the JSON line is one line");
		std::cout << "ksim_profile: ON, OK\n";
#else
		std::cout << "ksim_profile: KSIM_PROFILE is off, the tick holds no profile code; OK\n";
#endif
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_profile: FAILED: " << e.what() << "\n";
		return 1;
	}
}
