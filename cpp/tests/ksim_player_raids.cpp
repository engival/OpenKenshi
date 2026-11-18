// world.npc_raids (FACTIONS.md 4, kenshi-51x.5; user 2026-11-12: "raids are a player town only type of thing"): with it off (the default) a
// campaign's target candidates are towns a player faction owns, so a world with no player town starts no raid; a town a player faction owns,
// in range of a hostile raider, does get one; with it on, the NPC towns are targeted as before.
// The same new game three times: on (to find a raid), off with no player town, off with the first raid's target town owned by a player faction.

#include "ksim/ai.hpp"
#include "ksim/buildings.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/factions.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <iostream>
#include <memory>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

constexpr int kDays = 8;

// Straight lines everywhere.
class FakeNavigator final : public Navigator
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

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig regions;
	regions.workerThreads = 1;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, regions);
}

// `techGate` > 0: every campaign's `target tech level >=` is raised to it (a fixture: the book is immutable to the sim).
void enable(Simulation& simulation, bool npcRaids, int techGate = 0)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<FakeNavigator>();
	config.factions = FactionBook::build(simulation.gameData());
	if (techGate > 0)
		for (const CampaignInfo& campaign : config.factions->campaigns())
			const_cast<CampaignInfo&>(campaign).targetTechLevel = techGate;
	config.factionTuning.raidFrequency = 40.0;
	config.factionTuning.npcRaids = npcRaids;
	simulation.enableAi(std::move(config));
}

std::uint32_t planned(const Simulation& simulation)
{
	std::uint32_t total = 0;
	simulation.world().ecs().each([&](const state::FactionAI& faction) { total += faction.planned; });
	return total;
}

// Runs hour by hour until a raid has been planned (or the days are over); the first one's target town.
flecs::entity_t firstTarget(Simulation& simulation)
{
	for (int hour = 0; hour < kDays * 24 && planned(simulation) == 0; ++hour)
		simulation.run(kTicksPerGameHour);
	flecs::entity_t target = 0;
	simulation.world().ecs().each([&](const state::FactionAI& faction)
	{
		for (const state::Raid& raid : faction.raids)
			target = target == 0 ? raid.target.id : target;
	});
	return target;
}

// A BUILDING record by what the town level reads: `function` (0: none) with `tech level` `tier` (research), or the first plain shell.
const kdata::Record* pick(const kdata::GameData& data, int function, int tier)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("BUILDING")))
	{
		buildings::TownCounters counters;
		buildings::countTownBuilding(data, *record, counters);
		const bool match = function == buildings::kFunctionResearch ? counters.research == tier
			: function == buildings::kFunctionTurret ? counters.turrets == 1 && counters.shells == 0
			: function == buildings::kFunctionGate ? counters.gates >= 1
			: counters.shells == 1 && counters.research == 0 && counters.turrets == 0 && counters.gates == 0;
		if (match)
			return record;
	}
	return nullptr;
}

void addBuilding(flecs::world& ecs, std::uint64_t owner, const std::string& sid, double x, double z)
{
	state::PlayerBuilding building;
	building.recordSid = sid;
	building.ownerFaction = owner;
	building.x = x;
	building.z = z;
	building.built = true;
	ecs.entity().set<state::PlayerBuilding>(std::move(building));
}

// The level of a hand-built layout (TownCounters), pinned against the rwh.93 section 4 thresholds.
void checkThresholds()
{
	using buildings::townLevelOf;
	const auto level = [](int t, int s, int u, int g) { return townLevelOf({t, s, u, g}); };
	test::require(level(0, 0, 0, 0) == 0 && level(1, 1, 3, 0) == 0, "town level: a few small buildings are level 0");
	test::require(level(2, 0, 0, 0) == 1 && level(0, 2, 0, 0) == 1 && level(0, 0, 4, 0) == 1 && level(0, 0, 0, 1) == 1, "town level: each level-1 trigger");
	test::require(level(4, 5, 7, 0) == 1, "town level: no gate caps it at 1");
	test::require(level(0, 0, 0, 1) == 1 && level(3, 4, 4, 1) == 1, "town level: a gate alone, or short of every level-2 trigger, is 1");
	test::require(level(4, 0, 0, 1) == 2 && level(0, 5, 0, 1) == 2 && level(0, 0, 5, 1) == 2, "town level: a gate and one level-2 trigger");
	test::require(level(4, 5, 6, 1) == 2 && level(3, 5, 7, 1) == 2 && level(4, 4, 7, 1) == 2, "town level: level 3 needs all three");
	test::require(level(4, 5, 7, 1) == 3 && level(6, 9, 12, 2) == 3, "town level: level 3");
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// On: raids happen in this world, on NPC towns; take the first one's target and its owner.
		Simulation on = newGame(order);
		enable(on, true);
		const flecs::entity_t targetEntity = firstTarget(on);
		test::require(targetEntity != 0, "npc_raids on: no raid planned in " + std::to_string(kDays) + " days");
		const std::uint64_t targetId = flecs::entity(on.world().ecs(), targetEntity).get<state::Id>()->value;
		std::cout << "npc_raids on: first raid planned at tick " << on.clockTick() << " against town id " << targetId << "\n";

		// Off, no player town: nothing is planned in the same days.
		Simulation off = newGame(order);
		enable(off, false);
		off.run(static_cast<std::uint64_t>(kDays) * 24 * kTicksPerGameHour);
		test::require(planned(off) == 0, "npc_raids off, no player town: " + std::to_string(planned(off)) + " raids planned");

		// Off, that town owned by a player faction (and a player squad of enough members for the population gate): a raid on a player town.
		Simulation owned = newGame(order);
		enable(owned, false);
		flecs::world& ecs = const_cast<flecs::world&>(owned.world().ecs()); // a test fixture: marks a faction as the player's
		flecs::entity_t playerFaction = 0;
		ecs.each([&](const state::Id& id, const state::TownState& town)
		{
			if (id.value == targetId)
				playerFaction = town.owner.id;
		});
		test::require(playerFaction != 0, "the target town has no owner in the second world");
		flecs::entity(ecs, playerFaction).set<state::PlayerFaction>(state::PlayerFaction{});
		state::PlayerSquad squad;
		for (std::uint64_t i = 1; i <= 200; ++i)
			squad.members.push_back(i);
		ecs.entity().set<state::PlayerSquad>(std::move(squad));
		owned.run(static_cast<std::uint64_t>(kDays) * 24 * kTicksPerGameHour);
		test::require(planned(owned) > 0, "npc_raids off, a player town: no raid planned in " + std::to_string(kDays) + " days");
		std::size_t byPlayer = 0;
		std::size_t others = 0;
		owned.world().ecs().each([&](const state::FactionAI& faction)
		{
			for (const state::Raid& raid : faction.raids)
			{
				const state::TownState* town = flecs::entity(owned.world().ecs(), raid.target.id).get<state::TownState>();
				(town != nullptr && town->owner.id == playerFaction ? byPlayer : others) += 1;
			}
		});
		test::require(others == 0, "npc_raids off: a running raid targets a town that is not the player's");
		// The tech gate: every campaign wants a level 1 town. A player town with one shell (level 0) is never chosen; a second shell makes it a candidate.
		checkThresholds();
		const kdata::GameData& data = *owned.gameData();
		const kdata::Record* shell = pick(data, 0, 0);
		test::require(shell != nullptr, "the data has no one-shell BUILDING");
		test::require(pick(data, buildings::kFunctionResearch, 1) != nullptr, "the data has no research tier 1 bench");
		test::require(pick(data, buildings::kFunctionTurret, 0) != nullptr, "the data has no one-turret BUILDING");
		test::require(pick(data, buildings::kFunctionGate, 0) != nullptr, "the data has no gate BUILDING");
		double townX = 0.0, townZ = 0.0;
		ecs.each([&](const state::Id& id, const state::TownState& town)
		{
			if (id.value == targetId)
			{
				townX = town.x;
				townZ = town.z;
			}
		});
		const std::uint64_t playerId = flecs::entity(ecs, playerFaction).get<state::Id>()->value;
		const auto gated = [&](int shells)
		{
			Simulation world = newGame(order);
			enable(world, false, 1);
			flecs::world& wecs = const_cast<flecs::world&>(world.world().ecs());
			flecs::entity(wecs, playerFaction).set<state::PlayerFaction>(state::PlayerFaction{});
			state::PlayerSquad crowd;
			for (std::uint64_t i = 1; i <= 200; ++i)
				crowd.members.push_back(i);
			wecs.entity().set<state::PlayerSquad>(std::move(crowd));
			for (int i = 0; i < shells; ++i)
				addBuilding(wecs, playerId, shell->stringID, townX + 10.0 * i, townZ);
			world.run(static_cast<std::uint64_t>(kDays) * 24 * kTicksPerGameHour);
			return planned(world);
		};
		test::require(gated(0) == 0 && gated(1) == 0, "tech gate: a level 0 player town was raided");
		test::require(gated(2) > 0, "tech gate: a level 1 player town (two shells) was never raided");
		std::cout << "tech gate: level 0 towns (no buildings, one shell) are not raided; two shells (level 1) are\n";
		std::cout << "npc_raids off: no player town " << planned(off) << " raids; a player town " << planned(owned) << " planned (" << byPlayer << " running on it)\n";
		std::cout << "ksim_player_raids: OK\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_player_raids: FAILED: " << e.what() << "\n";
		return 1;
	}
}
