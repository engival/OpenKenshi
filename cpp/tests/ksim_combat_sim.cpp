// The character layer inside the simulation: fighters spawned from CHARACTER templates fight
// through Simulation::tick, and the run is deterministic: one region worker equals four, and a
// save at tick N loaded and run M ticks equals the uninterrupted N + M run (state hash). The
// fighters are flecs instances (state::Fighter, mirrored from the arena's hot array each tick).

#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr std::uint64_t kBefore = 240; // 12 s: the servant has swung, been dodged or hit
constexpr std::uint64_t kAfter = 360;

std::string characterSid(const kdata::GameData& gd, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (r->name == name)
			return r->stringID;
	}
	throw std::runtime_error(std::string("no CHARACTER ") + name);
}

void spawn(Simulation& sim, const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data)
{
	std::uint64_t rng = 12345;
	// Escaped Servant, then the first two other CHARACTER records with a STATS record and a race.
	std::vector<std::string> sids{characterSid(gd, "Escaped Servant")};
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (sids.size() < 3 && r->findRefs("stats") != nullptr && r->findRefs("race") != nullptr
			&& !r->findRefs("race")->entries.empty() && r->stringID != sids[0])
			sids.push_back(r->stringID);
	}
	for (int i = 0; i < 3; ++i)
	{
		state::Fighter f = data->makeFighter(gd, sids[static_cast<std::size_t>(i)], "", rng);
		f.team = i == 1 ? 1 : (i == 0 ? 0 : 1);
		f.x = i * 6.0;
		f.z = i == 0 ? 0.0 : 22.0;
		f.yaw = 0.0;
		sim.spawnFighter(std::move(f));
	}
}

Simulation make(int workers, const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd)
{
	RegionConfig regions;
	regions.workerThreads = workers;
	Simulation sim = Simulation::load(KENSHI_DATA, 77, regions);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(data, settings);
	spawn(sim, gd, data);
	return sim;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);

		Simulation one = make(1, data, gd);
		one.run(kBefore + kAfter);
		const std::uint64_t reference = one.stateHash();

		Simulation four = make(4, data, gd);
		four.run(kBefore + kAfter);
		test::require(four.stateHash() == reference, "1 worker and 4 workers differ");

		// Something happened: somebody was hit, got XP or bled.
		std::size_t damaged = 0;
		for (const auto& member : one.combat()->members())
		{
			for (const state::FighterPart& p : member.fighter.parts)
			{
				if (p.health < p.maxHealth - p.wear || p.stun > 0.0)
					++damaged;
			}
		}
		test::require(damaged > 0, "nobody was hurt in 30 s of fighting");

		// Save at N, load, run M == N + M.
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation split = make(1, data, gd);
		split.run(kBefore);
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_combat_sim.save";
		split.saveGame(path);
		RegionConfig regions;
		regions.workerThreads = 4;
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, regions);
		CombatSettings settings;
		settings.substeps = 2;
		loaded.enableCombat(data, settings);
		loaded.run(kAfter);
		test::require(loaded.stateHash() == reference, "save at N + load + M differs from N + M");

		// The mirror equals the arena after the run.
		std::cout << "fighters " << one.combat()->members().size() << ", hash " << std::hex << reference
			<< std::dec << ", damaged parts " << damaged << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
