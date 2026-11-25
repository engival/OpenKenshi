// kenshi-k16.6.758: the special-function finder's owner permission reads the member's Allegiance faction (a resident's Fighter::faction
// entity carries no FactionState). A resident whose home is a building its own faction owns, with a chair or machine part, takes goal 43
// at that part. Driven through the scorer's pick; nothing is staged but the player's position.

#include "ksim/dev/build_paths.hpp"
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace
{

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

bool hasPart(const Simulation& sim, std::uint64_t key)
{
	for (std::uint16_t slot = 0; slot < 16; ++slot)
		for (const routines::SiteKind kind : {routines::SiteKind::Seat, routines::SiteKind::Machine})
			if (sim.routineSite({static_cast<std::uint8_t>(kind), key, slot}) != nullptr)
				return true;
	return false;
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		SpawnOptions options;
		options.seed = 12;
		options.wildernessByDensity = false;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		sim.setResidentsArrive(false);
		AiSystem::Config config;
		config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		config.navigator = std::make_shared<StraightNavigator>();
		sim.enableAi(std::move(config));
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);

		// The player stands at each owned building with a part in turn; the first resident of its own owning faction (Allegiance) to hold 43
		// at a chair or machine of that building ends the search. A refused permission never lets one through (the owner is not "").
		std::string tookAt;
		for (const BuildingSite& b : sim.world().buildingSites())
		{
			if (b.ownerFaction.empty() || b.ruined || !tookAt.empty())
				continue;
			sim.enqueue(Command::setPlayerAreas({{b.cx, b.cz}}));
			sim.run(30);
			if (!hasPart(sim, b.key))
				continue;
			sim.run(120);
			ecsOf(sim).each([&](const state::Fighter& f, const state::Allegiance& a, const state::Routine& r)
			{
				if (f.homeBuilding == b.key && a.factionSid == b.ownerFaction && r.goal == routines::task::StayInHome && r.target.object == b.key
					&& (r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Seat) || r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Machine)))
					tookAt = b.buildingSid;
			});
		}
		test::require(!tookAt.empty(), "no resident of an owning faction takes goal 43 at a part of its own faction's home (the finder's owner permission)");
		std::printf("    goal 43 taken at a part of %s\n", tookAt.c_str());
		std::printf("ksim_owner_part: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_owner_part: FAILED: " << e.what() << "\n";
		return 1;
	}
}
