// A world squad in a fight that ends up where the streaming navmesh is not (TOWN_LIFE_SLICE 5.14 "off the mesh"). The fight used to hold the squad built
// for ever (a fight holds a demotion), its members off the mesh could not be corrected (position_corrected distance -1 every tick) and could not move:
// a terminal state. Now an off-mesh squad is demoted whatever it is doing.
// Setup as the server's with --bake: Hub start, streaming navmesh, squad AI. Needs the world bake (argv[1]; exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

std::string factionName(Simulation& sim, const state::SquadInstance& instance)
{
	if (instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id))
		return "";
	const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
	const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
	return record != nullptr ? record->name : "";
}

// The lowest-Id built (Fighter, no Routine) member of the first squad of that faction that has one.
std::uint64_t builtMemberOf(Simulation& sim, const std::string& faction, std::uint64_t& squadId)
{
	std::uint64_t found = 0;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
	{
		if (found != 0 || factionName(sim, instance) != faction)
			return;
		ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& mid)
		{
			if (sim.combat()->find(mid.value) != nullptr && !member.has<state::Routine>() && (found == 0 || mid.value < found))
			{
				found = mid.value;
				squadId = id.value;
			}
		});
	});
	return found;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_world_offmesh: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		sim.run(static_cast<std::uint64_t>(kTicksPerGameMinute * 1.5));

		// The neutral side: the nearest homeless squad of a faction not hostile to the player (no merchant or trade circuit: they keep a squad routine),
		// moved 250 u east of the Hub, where it is built. The Hub's Mercenary Guild bar squad was it until it became a resident group (1fe6ad1).
		{
			flecs::entity player;
			ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { player = e; });
			flecs::entity pick;
			double best = 1e18;
			ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
			{
				const double d = std::hypot(at.x - (-50979.0), at.z - 2932.0);
				if (instance.source != state::SpawnSource::BiomeHomeless || instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id) || d >= best
					|| trader::isMerchant(*sim.gameData()->find(instance.templateSid))
					|| sim.ai()->registry().squad(instance.templateSid).behaviour == Behaviour::TradeCircuit
					|| factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) < 0)
					return;
				best = d;
				pick = squad;
			});
			test::require(pick.is_valid(), "a neutral homeless squad to set down at the Hub");
			sim.moveSquad(pick, -50979.0 + 250.0, 2932.0);
			sim.run(200);
		}
		// A bandit (team 13) and that neutral squad (team 0), built at the Hub, are put 2 zones east of the covered 3 x 3 (x = -38000 is zone 23; the
		// Hub's are 19..21) and aimed at each other: a fight no player is part of.
		std::uint64_t banditSquad = 0;
		const std::uint64_t bandit = builtMemberOf(sim, "Dust Bandits", banditSquad);
		std::uint64_t merc = 0;
		ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
		{
			if (merc != 0 || id.value == banditSquad || sim.ai()->registry().squad(instance.templateSid).behaviour == Behaviour::TradeCircuit)
				return;
			ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& mid)
			{
				const state::Fighter* f = sim.combat()->find(mid.value);
				if (merc == 0 && f != nullptr && f->team == 0 && !f->playerControlled && !member.has<state::Routine>())
					merc = mid.value;
			});
		});
		test::require(bandit != 0 && merc != 0, "the new game has a built Dust Bandit and a built neutral world squad member");
		CombatArena* arena = const_cast<CombatArena*>(sim.combat());
		arena->find(bandit)->x = -38000.0;
		arena->find(bandit)->z = 2000.0;
		arena->find(merc)->x = -37900.0;
		arena->find(merc)->z = 2000.0;
		arena->find(bandit)->target = merc;
		arena->find(merc)->target = bandit;
		const std::uint64_t demotedBefore = sim.residentStats().worldDemotedSquads;
		const std::uint64_t stuckBefore = sim.combat()->uncorrectable();

		sim.run(static_cast<std::uint64_t>(kTicksPerGameMinute * 3.0));
		const std::uint64_t stuckMid = sim.combat()->uncorrectable();
		sim.run(static_cast<std::uint64_t>(kTicksPerGameMinute * 1.0));
		const std::uint64_t stuckEnd = sim.combat()->uncorrectable();
		std::cout << "ksim_world_offmesh: demoted " << sim.residentStats().worldDemotedSquads - demotedBefore << " squads; off-mesh corrections: " << stuckMid - stuckBefore
			<< " in the first 3 game minutes, " << stuckEnd - stuckMid << " in the next\n";
		test::require(sim.residentStats().worldDemotedSquads > demotedBefore, "an off-mesh squad is demoted although it is fighting");
		test::require(sim.combat()->find(bandit) == nullptr, "the off-mesh bandit is no Fighter any more");
		test::require(stuckEnd == stuckMid, "nobody is left off the mesh once the squads are demoted (no position_corrected every tick)");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_world_offmesh: FAILED: " << e.what() << "\n";
		return 1;
	}
}
