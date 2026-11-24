// Decision cadences at game speed above 1 (kenshi-k16.6.633): at speed 1 they are the second's; at 3 and 5 an NPC's scan, scorer period and
// slice run a third as often, a player character's and a fighter's do not.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cstring>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr int kSteps = 600; // 30 game seconds

struct Counts
{
	int npc = 0, player = 0, fighter = 0;
};

// Scans each character makes in kSteps at the given cadence scale (a scan moves the sensor's due-stamp senseAt forward).
Counts scans(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, std::uint32_t scale)
{
	CombatArena arena(data, CombatSettings{}, 5);
	arena.environmentMutable().cadenceScale = scale;
	const kdata::Record* servant = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			servant = r;
	test::require(servant != nullptr, "no Escaped Servant");
	std::uint64_t rng = 11;
	auto spawn = [&](std::uint64_t id, double x, std::uint64_t faction, bool player)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = x;
		f.faction = faction;
		f.team = static_cast<int>(faction);
		f.playerControlled = player;
		arena.add(id, std::move(f));
	};
	spawn(1, 0.0, 10, false);      // the NPC
	spawn(2, 2000.0, 10, true);    // the player character
	spawn(3, 4000.0, 10, false);   // the fighter (its target is set every step: a fight that does not end)
	spawn(4, 4010.0, 20, false);
	Counts out;
	double last[5] = {state::Fighter::kNoStamp, state::Fighter::kNoStamp, state::Fighter::kNoStamp, state::Fighter::kNoStamp, state::Fighter::kNoStamp};
	for (int i = 0; i < kSteps; ++i)
	{
		arena.find(3)->target = 4;
		arena.find(4)->target = 3;
		arena.step(kDt);
		for (std::uint64_t id : {1, 2, 3})
		{
			const double clock = arena.find(id)->senseAt;
			if (clock > last[id] && last[id] > state::Fighter::kNoStamp * 0.5)
				(id == 1 ? out.npc : id == 2 ? out.player : out.fighter) += 1;
			last[id] = clock;
		}
	}
	return out;
}

void testScans()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const auto data = CombatData::build(gd, KENSHI_DATA);
	const Counts one = scans(gd, data, 1);
	const Counts three = scans(gd, data, 3);
	test::require(one.npc >= 20, "speed 1: the NPC scanned only " + std::to_string(one.npc) + " times");
	test::require(three.npc * 5 <= one.npc * 2, "speed 3: the NPC's scans " + std::to_string(three.npc) + " vs " + std::to_string(one.npc));
	test::require(three.npc >= 1, "speed 3: the NPC never scanned");
	test::require(three.player == one.player, "the player character's scan cadence changed");
	test::require(three.fighter == one.fighter, "a fighter's scan cadence changed");
}

void testSimulationCadences()
{
	Simulation sim = Simulation::load(KENSHI_DATA);
	const state::Fighter npc;
	state::Fighter player;
	player.playerControlled = true;
	state::Fighter fighter;
	fighter.target = 9;
	const struct { double speed; std::uint32_t scale; } rows[] = {{1.0, 1}, {1.5, 1}, {2.0, 2}, {3.0, 3}, {5.0, 3}};
	for (const auto& row : rows)
	{
		sim.setGameSpeed(row.speed);
		test::require(sim.cadenceScale() == row.scale, "scale at speed " + std::to_string(row.speed));
		test::require(sim.scoreInterval(npc) == routines::kScoreIntervalTicks * row.scale, "scorer period of an NPC");
		test::require(sim.scoreInterval(player) == routines::kScoreIntervalTicks, "scorer period of a player character");
		test::require(sim.scoreInterval(fighter) == routines::kScoreIntervalTicks, "scorer period of a fighter");
		int npcHits = 0, exemptHits = 0;
		for (std::uint64_t now = 0; now < 360; ++now)
		{
			npcHits += sim.inSliceScaled(now, 7, false) ? 1 : 0;
			exemptHits += sim.inSliceScaled(now, 7, true) ? 1 : 0;
		}
		test::require(exemptHits == 18, "an exempt member's slice is the second's");
		test::require(npcHits == 18 / static_cast<int>(row.scale), "an NPC's slice at speed " + std::to_string(row.speed));
	}
}

// ---- end to end: a promoted town squad at speed 1 and 3 over the same game time ----

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

struct E2e
{
	int scansNpc = 0, scansFighter = 0;
	std::uint64_t passes = 0;
};

E2e town(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, double speed)
{
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
	sim.enableCombat(combat, settings);
	// The first resident squad (Id order) of three or more people.
	flecs::entity squad;
	std::uint64_t best = ~std::uint64_t(0);
	double x = 0.0, z = 0.0;
	const routines::PackageTable packages(*sim.gameData());
	const BehaviourRegistry& registry = sim.ai()->registry();
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id == 0 || instance.source != state::SpawnSource::TownResident || id.value >= best)
			return;
		const Mapping& mapping = registry.squad(instance.templateSid);
		if (mapping.behaviour != Behaviour::ShopKeeper && mapping.behaviour != Behaviour::ResidentLife)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		if (membersOf(sim, e).size() >= 3 && membersOf(sim, e).size() <= 8)
		{
			best = id.value;
			squad = e;
			x = at.x;
			z = at.z;
		}
	});
	test::require(squad.is_valid(), "a resident squad of three");
	sim.enqueue(Command::setPlayerAreas({{x, z}}));
	for (int i = 0; i < 3000 && !squad.has<state::SquadRoutine>(); ++i)
		sim.run(1);
	test::require(squad.has<state::SquadRoutine>(), "the squad was promoted");
	std::vector<std::uint64_t> ids;
	for (const flecs::entity member : membersOf(sim, squad))
		if (fighterOf(sim, member.get<state::Id>()->value) != nullptr)
			ids.push_back(member.get<state::Id>()->value);
	test::require(ids.size() >= 3, "three built members");
	sim.setGameSpeed(speed);
	const std::uint64_t before = sim.townScorerStats().passes;
	E2e out;
	double last[2] = {state::Fighter::kNoStamp, state::Fighter::kNoStamp};
	for (int i = 0; i < kSteps; ++i)
	{
		state::Fighter* fighter = fighterOf(sim, ids[0]); // fights every tick: a target that never resolves
		fighter->target = ids[1];
		sim.run(1);
		for (int k = 0; k < 2; ++k)
		{
			const double clock = fighterOf(sim, ids[k == 0 ? 0 : 2])->senseAt;
			if (last[k] > state::Fighter::kNoStamp * 0.5 && clock > last[k])
				(k == 0 ? out.scansFighter : out.scansNpc) += 1;
			last[k] = clock;
		}
	}
	out.passes = sim.townScorerStats().passes - before;
	return out;
}

void testEndToEnd()
{
	const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const auto combat = CombatData::build(gd, KENSHI_DATA);
	const E2e one = town(order, combat, 1.0);
	const E2e three = town(order, combat, 3.0);
	const E2e five = town(order, combat, 5.0);
	std::cout << "scans npc " << one.scansNpc << "/" << three.scansNpc << "/" << five.scansNpc << " fighter " << one.scansFighter << "/"
		<< three.scansFighter << "/" << five.scansFighter << " scorer passes " << one.passes << "/" << three.passes << "/" << five.passes << "\n";
	test::require(one.scansNpc >= 10 && one.passes >= 10, "speed 1 runs the NPC's scan and scorer");
	for (const E2e* e : {&three, &five})
	{
		test::require(e->scansNpc * 5 <= one.scansNpc * 2, "the NPC's scans fall about 3x");
		test::require(e->passes * 5 <= one.passes * 2 + 5, "the scorer passes fall about 3x");
		test::require(e->scansFighter * 5 >= one.scansFighter * 4, "the fighter's scans do not fall");
	}
}

} // namespace

int main()
{
	try
	{
		testScans();
		testSimulationCadences();
		testEndToEnd();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_cadence_scale OK\n";
	return 0;
}
