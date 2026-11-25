// An interact order whose walk is abandoned (routes blocked) ends with a failure event 13, never hangs (kenshi-xu27).
// A bare world with no navmesh: every walk the combat arena runs is abandoned at once (walkOrder's "no navmesh" site, the same
// clearRoute as the blocked and out-of-retries sites), so the order is left holding an InteractTask with nothing to walk.
// The order must end within a bounded number of ticks with a result that is not "done". Door actions: open (1) and lock (2 + lock flag).

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/interact.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kX = -50978.8; // open ground outside the Hub's buildings
constexpr double kZ = 2932.48;
constexpr double kY = 1647.0;

std::string bodySid(const kdata::GameData& gd)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (record->name == "Escaped Servant")
			return record->stringID;
	throw std::runtime_error("no CHARACTER Escaped Servant");
}

// Walks `action` towards a door 40 u away, on a world with no navmesh; returns the first event 13 result, if one came within `limit` ticks.
std::optional<std::uint8_t> run(std::uint8_t action, std::uint8_t flags, int limit, int& ticks)
{
	const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
	const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
	Simulation sim = Simulation::load(KENSHI_DATA, 91);
	sim.setGameData(gd);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(data, settings);
	std::uint64_t rng = 3;
	state::Fighter f = data->makeFighter(*gd, bodySid(*gd), "", rng);
	f.x = kX;
	f.z = kZ;
	f.y = kY;
	f.playerControlled = true;
	const std::uint64_t who = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	state::Door door;
	door.x = kX + 40.0;
	door.y = kY;
	door.z = kZ;
	door.state = 1;
	const std::uint64_t doorId = sim.spawnDoor(door).get<state::Id>()->value;
	std::optional<std::uint8_t> result;
	sim.setInteractObserver([&](const InteractEvent& e)
	{
		if (e.kind == InteractEvent::Kind::Done && e.who == who && e.target == doorId && !result)
			result = e.result;
	});
	// What beginInteract does: the walk to the use point, and the task that owns it.
	state::Fighter& walker = *fighterOf(sim, who);
	walker.route = {{kX, kY, kZ}, {kX + 40.0, kY, kZ}};
	walker.routeNext = 1;
	walker.routeSeq = 1;
	++walker.routeVersion;
	flecs::entity entity;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id) { if (id.value == who) entity = e; });
	entity.set<state::InteractTask>(state::InteractTask{doorId, action, 1, flags});
	for (ticks = 0; ticks < limit && !result; ++ticks)
		sim.tick();
	if (sim.combat()->routesBlocked() == 0)
		throw std::runtime_error("the stage never abandoned the walk");
	return result;
}

} // namespace

int main()
{
	try
	{
		for (const auto& [name, action, flags] : {std::tuple<const char*, std::uint8_t, std::uint8_t>{"open", 1, 0}, {"lock", 2, kInteractFlagLock}})
		{
			int ticks = 0;
			const std::optional<std::uint8_t> result = run(action, flags, 400, ticks);
			test::require(result.has_value(), std::string(name) + ": the interact whose walk was abandoned never ended (no event 13 in 400 ticks)");
			test::require(*result != InteractEvent::kDone, std::string(name) + ": an abandoned walk reported done");
			std::cout << "  " << name << ": abandoned walk -> event 13 result " << static_cast<int>(*result) << " after " << ticks << " ticks\n";
		}
		std::cout << "ksim_interact_abandon: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_interact_abandon: FAILED: " << e.what() << "\n";
		return 1;
	}
}
