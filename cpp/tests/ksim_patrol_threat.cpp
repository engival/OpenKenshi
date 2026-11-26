// kenshi-k16.6.543: why a Holy Nation patrol does nothing about Fogmen in sight. A new game with the player beside the patrol (template
// 1532649, package 2672 "Patrol map (short range)"): its members are package-driven and carry the attack goals 16 and 15 (so the
// join path is live and the squad is not stuck on an unloaded rule), they sight a Fogmen squad placed next to them, and the Fogmen are
// NOT hostile to them in the loaded faction data (The Holy Nation lists no Fogmen entry and defaults to 0), so goal 16 admits no
// candidate. Pins the generic path as it is; the data question (does the original attack?) is the oracle question in the bead.
// Needs the world navmesh bake (argv[1], exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/world/join_rules.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "knav/navmesh.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"
#include <algorithm>
#include <iostream>
#include <set>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_patrol_threat: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		const std::string world = argv[1];
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		options.settleSeconds = 30.0; // the Holy Army patrol's place at the player's teleport was pinned on the 30 s new-game settle (kenshi-rmw6 made the default 45)
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const auto combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		sim.run(3);
		state::Fighter& player = *const_cast<state::Fighter*>(playerOf(sim));
		player.x = -53211.0;
		player.z = -11278.0;
		player.y = sim.groundHeight(player.x, player.z).value_or(player.y);
		sim.run(200); // the patrol squad is built at full detail by proximity
		const flecs::entity fog = sim.placeWildernessSquad("42040-rebirth.mod", -53211.0 + 120.0, -11278.0 + 120.0);
		std::vector<std::uint64_t> fogIds;
		for (const flecs::entity member : membersOf(sim, fog))
			fogIds.push_back(member.get<state::Id>()->value);
		std::size_t attacking = 0; // distinct non-fog fighters that took a fogman as their target within 1200 ticks
		std::set<std::uint64_t> attackers;
		for (int t = 0; t < 1200; ++t)
		{
			sim.tick();
			for (const CombatArena::Member& m : sim.combat()->members())
				if (std::find(fogIds.begin(), fogIds.end(), m.fighter.target) != fogIds.end() && std::find(fogIds.begin(), fogIds.end(), m.id) == fogIds.end())
					attackers.insert(m.id);
		}
		attacking = attackers.size();

		// The patrol: the world squad of template 1532649 nearest the player (ids shift with the data, so no fixed character id).
		const state::Fighter& playerNow = *playerOf(sim); // `player` above is a reference into storage the ticks may have moved: read it afresh
		std::uint64_t paladinId = 0;
		double nearest = 1.0e18;
		ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const Detail& detail)
		{
			if (instance.templateSid.rfind("1532649", 0) != 0 || detail.level != DetailLevel::Full)
				return;
			for (const flecs::entity member : membersOf(sim, squad))
			{
				const state::Fighter* f = fighterOf(sim, member.get<state::Id>()->value);
				const double d = f != nullptr ? std::hypot(f->x - playerNow.x, f->z - playerNow.z) : 1.0e18;
				if (d < nearest)
				{
					nearest = d;
					paladinId = member.get<state::Id>()->value;
				}
			}
		});
		const state::Fighter* paladin = paladinId != 0 ? fighterOf(sim, paladinId) : nullptr;
		test::require(paladin != nullptr && paladin->faction != 0, "the Holy Army patrol's High Paladin exists at full detail");
		bool driven = false;
		for (const CombatArena::Member& m : sim.combat()->members())
			if (m.id == paladinId)
				driven = m.joinDriven && (m.joinGoals & joins::kAttackEnemies) != 0 && (m.joinGoals & joins::kProtectAllies) != 0;
		test::require(driven, "the patrol's members are package-driven with goals 16 and 15 (not stuck without an enemy scan)");

		const sensor::Context& ctx = sim.combat()->sensorContext();
		std::size_t sighted = 0;
		std::size_t hostile = 0;
		for (const flecs::entity member : membersOf(sim, fog))
		{
			const std::uint64_t id = member.get<state::Id>()->value;
			const state::Fighter* fogman = fighterOf(sim, id);
			// The patrol now kills a Fogman inside the window (k16.9.24 / k16.6.495 combat): a dead one is no longer sighted, so only
			// the sighted ones are tested for the relation.
			if (const state::SightRecord* record = perception::find(*paladin, id); record != nullptr && record->seen)
			{
				++sighted;
				hostile += sensor::enemy(ctx, *paladin, *fogman) ? 1 : 0;
			}
		}
		test::require(sighted > 0, "the patrol sights the Fogmen beside it");
		// rwh.227: the relation map is min-seeded per pair, so the Holy Nation (default 0) reads the Fogmen (default -100) at -100.
		test::require(hostile == sighted, "the Holy Nation reads every sighted Fogman as hostile (min-seeded relation, rwh.227)");
		test::require(attacking > 0, "a Holy Army patrol attacks the Fogmen squad in sight");

		// Asymmetry stays: HN authors -100 toward the Raptors, the Raptors author nothing toward HN (min(0,0) = 0); Dust Bandits read HN at
		// min(-10, 0) = -10, not an enemy.
		const auto sidOf = [&](const char* name)
		{
			for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("FACTION")))
				if (record->name == name)
					return record->stringID;
			return std::string();
		};
		const auto entityOf = [&](const std::string& sid)
		{
			flecs::entity found;
			ecsOf(sim).each([&](flecs::entity e, const state::FactionState& f) { if (f.templateSid == sid) found = e; });
			return found;
		};
		const flecs::entity hn = entityOf(sidOf("The Holy Nation"));
		const flecs::entity raptors = entityOf(sidOf("Raptors"));
		const flecs::entity bandits = entityOf(sidOf("Dust Bandits"));
		const kdata::GameData& data = *sim.gameData();
		test::require(factionRelation(data, hn, raptors) == -100.0, "HN -> Raptors is the authored -100");
		test::require(factionRelation(data, raptors, hn) == 0.0, "Raptors -> HN stays min(0,0) = 0: the authored side is not symmetrised");
		test::require(factionRelation(data, bandits, hn) == -10.0, "Dust Bandits -> HN is min(-10,0) = -10");
		test::require(factionRelation(data, bandits, hn) > FactionBook::kHostile, "-10 is not an enemy");
		std::cout << "ksim_patrol_threat: OK (" << sighted << " fogmen sighted, " << hostile << " hostile, " << attacking << " attacking)\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_patrol_threat: FAIL: " << e.what() << "\n";
		return 1;
	}
}
