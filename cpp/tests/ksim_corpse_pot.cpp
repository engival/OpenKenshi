// The corpse pot, DEAD_GUYS_GO_IN_THE_POT 137 and its plan (138 FIND_A_DEAD_GUY lifts the body, 136 FEED_CORPSE_INTO_MACHINE consumes
// it): task CH town-residents-3; kinspect ~/kinspect_bu/dt24_patrol_town_pot.md section 3; TOWN_LIFE_SLICE 5.32.
//   - the scorer adopts 137 only with a disposal machine (a BUILDING whose functionality function is 11) of the member's faction in its
//     town and a body to feed (a dead occupant of a town cage first, else the nearest sensed corpse);
//   - the member walks to the corpse, lifts it (68 LIFT_PERSON), carries it to the machine and feeds it (136): the body is consumed
//     and the member's hands are empty, 137's goal 100;
//   - the world's own residents run it: the Cannibal Capital's squads carry the "Cannibals at home" package (137 and 138 in the Leader
//     list) and two Corpse Furnaces stand in the town. No NPC is ordered; the corpse is spawned by hand.
// No navmesh is needed (a straight navigator walks the legs).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

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

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

void centreOn(Simulation& sim, double x, double z)
{
	sim.enqueue(Command::setPlayerAreas({{x, z}}));
	sim.run(1);
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

void corpsePot(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	const kdata::GameData& data = *sim.gameData();

	// The Cannibal Capital (world model: towns.tsv), the fixture's town: two Corpse Furnaces stand in its cell.
	constexpr double kTownX = -61996.9;
	constexpr double kTownZ = -110806.7;
	centreOn(sim, kTownX, kTownZ);

	// The town entity and its circle, once the zone is loaded.
	flecs::entity townEntity;
	runUntil(sim, [&]
	{
		ecsOf(sim).each([&](flecs::entity entity, const state::TownState& town)
		{
			if (std::hypot(town.x - kTownX, town.z - kTownZ) < 100.0)
				townEntity = entity;
		});
		return townEntity.is_valid();
	}, 4000, "the Cannibal Capital's town is loaded");
	test::require(townEntity.has<state::TownState>(), "the town keeps its state");
	// The town's Ids by its POSITION: a town has a root and an active (variant) Id, and the squads name either. Collected up front:
	// another query inside an `each` iterates the same world.
	std::set<std::uint64_t> townIds;
	ecsOf(sim).each([&](flecs::entity town, const state::TownState& state)
	{
		if (std::hypot(state.x - kTownX, state.z - kTownZ) < 200.0 && town.get<state::Id>() != nullptr)
			townIds.insert(town.get<state::Id>()->value);
	});

	// The disposal machines of the town: function 11 buildings inside its circle.
	const towncircle::Circle circle = towncircle::of(data, *townEntity.get<state::TownState>());
	std::vector<std::uint64_t> machines;
	for (const DisposalSite& site : sim.world().disposalSites())
		if (std::hypot(site.x - circle.x, site.z - circle.z) <= circle.radius)
			machines.push_back(site.key);
	std::printf("  the town (%.0f, %.0f, r %.0f) holds %zu disposal machine(s)\n", circle.x, circle.z, circle.radius,
		machines.size());
	test::require(!machines.empty(), "the Cannibal Capital holds a disposal machine (a Corpse Furnace)");

	// A NEW_GAME world rewrites the player areas every 20 ticks from its player characters and the camera focus (residents.cpp
	// stepPlayerAreas), so a bare setPlayerAreas alone is overwritten: the camera focus keeps the town at Full detail.
	sim.enqueue(Command::setView(circle.x, circle.z));
	centreOn(sim, circle.x, circle.z);
	sim.run(40);

	// A promoted resident squad of the town whose package carries 137 (the "Cannibals at home" Leader list).
	const routines::PackageTable packages(data);
	const auto promoted = [&]() -> flecs::entity
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance)
		{
			// A squad of THIS town: its people stand in the circle (the homeTown ref is another town entity than the TownState, so
			// the position is the reliable test).
			const state::Position* at = squad.get<state::Position>();
			if (!squad.has<state::SquadRoutine>() || at == nullptr || std::hypot(at->x - circle.x, at->z - circle.z) > circle.radius)
				return;
			const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
			const routines::CompiledPackage* package = packages.find(mapping.package);
			if (package == nullptr || !routines::carriesGoal(*package, routines::task::DeadGuysInThePot))
				return;
			if (!found.is_valid() || squad.get<state::Id>()->value < found.get<state::Id>()->value)
				found = squad;
		});
		return found;
	};
	runUntil(sim, [&] { return promoted().is_valid(); }, 8000,
		"a resident squad of the town carrying DEAD_GUYS_GO_IN_THE_POT was never promoted");
	const flecs::entity squad = promoted();
	test::require(squad.has<state::SquadRoutine>(), "the resident squad is in routine mode");

	std::vector<std::pair<std::uint64_t, flecs::entity>> members;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		members.emplace_back(id.value, member);
	});
	std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	test::require(!members.empty(), "the squad has promoted members");

	// A corpse by hand: a copy of a member, dead, laid on open ground in the town (an outdoors spot: a body inside a building is
	// behind a door the walk cannot open without the door planner).
	state::Fighter body = *sim.combat()->find(members.front().first);
	body.sight.clear();
	body.dead = true;
	body.unconscious = false;
	body.down = false;
	body.target = 0;
	body.carrying = 0;
	body.carriedBy = 0;
	bool placed = false;
	for (double radius = 20.0; radius <= 400.0 && !placed; radius += 20.0)
		for (int i = 0; i < 16 && !placed; ++i)
		{
			const double angle = static_cast<double>(i) * 0.39269908169872414; // pi / 8
			const double x = circle.x + std::sin(angle) * radius;
			const double z = circle.z + std::cos(angle) * radius;
			const std::optional<double> y = sim.groundHeight(x, z);
			if (!y || sim.buildingAt(x, *y, z) != 0)
				continue;
			body.x = x;
			body.y = *y;
			body.z = z;
			placed = true;
		}
	test::require(placed, "an open spot in the town for the corpse");
	body.clearRoute();
	const double corpseX = body.x;
	const double corpseZ = body.z;
	flecs::entity bodyEntity = sim.spawnFighter(std::move(body));
	const std::uint64_t corpse = bodyEntity.get<state::Id>()->value;

	// The walk is the mesh's (combat.cpp walkOrder abandons a route with no navigator) and the baked world covers the Hub, not the
	// capital; the test therefore moves the member by hand: one member is put beside the corpse (the lift), and the carrier is put at
	// the machine once it has the body in its arms (the feed). What the walk would do is the other tests' subject.
	{
		state::Fighter& lifter = const_cast<state::Fighter&>(*sim.combat()->find(members.front().first));
		lifter.x = corpseX + 2.0;
		lifter.z = corpseZ + 2.0;
		lifter.clearRoute();
	}

	// Run: a member adopts 137, lifts the corpse, carries it to a machine, and the body is consumed.
	bool sawGoal = false;
	bool sawOurs = false;
	bool sawLift = false;
	bool sawPotWalk = false;
	bool consumed = false;
	for (std::uint64_t tick = 0; tick < 40000 && !consumed; ++tick)
	{
		sim.run(1);
		ecsOf(sim).each([&](flecs::entity member, const state::Routine& routine)
		{
			if (routine.goal != routines::task::DeadGuysInThePot)
				return;
			sawGoal = true;
			const state::Id* id = member.get<state::Id>();
			sawOurs = sawOurs || routine.chainTarget == corpse;
			if (routine.target.kind != static_cast<std::uint8_t>(routines::SiteKind::Disposal))
				return;
			if (std::count(machines.begin(), machines.end(), routine.target.object) == 0)
				return;
			const state::Fighter* carrier = id != nullptr ? sim.combat()->find(id->value) : nullptr;
			sawPotWalk = sawPotWalk || (carrier != nullptr && carrier->carrying == routine.chainTarget);
			if (carrier != nullptr && carrier->carrying == corpse)
				if (const DisposalSite* machine = sim.world().disposalSite(routine.target.object);
					machine != nullptr && std::hypot(carrier->x - machine->x, carrier->z - machine->z) > 1.0)
				{
					// The feed's walk, by hand (no navigator in this world): the carrier stands at its machine.
					state::Fighter& live = const_cast<state::Fighter&>(*carrier);
					live.x = machine->x;
					live.z = machine->z;
					live.y = machine->y;
					live.clearRoute();
				}
		});
		if (const state::Fighter* f = sim.combat()->find(corpse); f != nullptr)
		{
			sawLift = sawLift || f->carriedBy != 0;
			continue;
		}
		// The body is gone: consumed at a machine.
		test::require(sim.townScorerStats().corpsesDisposed >= 1, "the corpse left the world but nobody fed it to a machine");
		consumed = true;
	}
	test::require(sawGoal, "no resident ever adopted DEAD_GUYS_GO_IN_THE_POT 137");
	test::require(sawOurs, "no resident ever picked the spawned corpse");
	test::require(sawLift, "the corpse was never lifted (138's plan step)");
	test::require(sawPotWalk, "the carrier never walked the corpse to a disposal machine");
	test::require(consumed, "the corpse was never consumed at a machine");
	test::require(sim.townScorerStats().corpsesDisposed >= 1, "the feed was not counted");
	test::require(sim.combat()->find(corpse) == nullptr, "the consumed body is still in the arena");
	std::printf("  corpse %llu disposed, %llu fed in all\n", static_cast<unsigned long long>(corpse),
		static_cast<unsigned long long>(sim.townScorerStats().corpsesDisposed));
	std::cout << "corpse pot ok\n";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation first = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*first.gameData(), KENSHI_DATA);
		corpsePot(order, combat);
		std::cout << "ksim_corpse_pot: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_corpse_pot: FAILED: " << e.what() << "\n";
		return 1;
	}
}
