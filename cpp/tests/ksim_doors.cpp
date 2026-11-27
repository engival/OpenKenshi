// Town gates in the simulation (include/ksim/nav/doors.hpp, docs/reference/DOORS.md):
//
//  1. data: every door of the placements gets a key like the navmesh bake's, the gate doors
//     are found (BUILDING `is wall gate` / `is gateway`) and belong to towns, non-public towns
//     (TOWN `is public` false) keep their gates open like any other (no class refusal, as the original);
//  2. no alert closing (retired, GATES_CARRY_LIVE.md a): a faction that loses a town defends, and its gates stay exactly as
//     the data has them;
//  3. overrides: SetDoorState locks and unlocks one door, saved with the game;
//  4. stances: how the owner of a town sees a faction (no door effect);
//  5. (needs a bake, usage: ksim_doors BAKEDIR RX0 RZ0 RX1 RZ1) Squin: with its gates locked (an override, Squin's
//     crews taken off them) every route in is refused, a raid that the locked gates refuse bashes one open for ever, the
//     squads of Squin keep a goal (no terminal state); unlocked, everything is as before;
//  6. 1 worker == 4 workers with a town lost and an override in play, and save + load + run == the
//     uninterrupted run, the book rebuilt after the load.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/nav/doors.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using ksim::DoorClass;
using ksim::state::GoalKind;

// Straight lines everywhere; keeps the last door book it was given.
class FakeNavigator final : public ksim::Navigator
{
public:
	bool covers(double, double) const override { return true; }

	ksim::Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		ksim::Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}

	void applyDoors(const knav::DoorBook& book) const override
	{
		book_ = book;
		++applies_;
	}

	const knav::DoorBook& book() const { return book_; }
	int applies() const { return applies_; }

private:
	mutable knav::DoorBook book_;
	mutable int applies_ = 0;
};

ksim::Simulation newGame(const kworld::LoadOrder& order, int workers, std::uint64_t seed = 5)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	ksim::RegionConfig config;
	config.workerThreads = workers;
	return ksim::Simulation::newGame(KENSHI_DATA, order, options, nullptr, config);
}

void enable(ksim::Simulation& simulation, std::shared_ptr<const ksim::Navigator> navigator)
{
	ksim::AiSystem::Config config;
	config.registry = std::make_shared<ksim::BehaviourRegistry>(simulation.world(),
		ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::move(navigator);
	config.factions = ksim::FactionBook::build(simulation.gameData());
	config.factionTuning.raidFrequency = 0.0;
	simulation.enableAi(std::move(config));
}

flecs::entity entityOf(const ksim::Simulation& simulation, flecs::entity_t id)
{
	return flecs::entity(simulation.world().ecs(), id);
}

std::uint64_t idOf(const ksim::Simulation& simulation, flecs::entity_t id)
{
	return entityOf(simulation, id).get<ksim::state::Id>()->value;
}

// A faction that owns a gate town that is not private, another town of its own, and a faction
// that is hostile to it (so the new owner of the lost town is a hostile one).
struct Scene
{
	flecs::entity_t owner = 0;
	flecs::entity_t gateTown = 0;
	std::vector<std::uint64_t> gateKeys; // the doors of gateTown
	std::vector<std::uint64_t> lockingKeys; // ... that always lock
	std::uint64_t spareTownId = 0; // another town of the owner
	flecs::entity_t hostile = 0;
	flecs::entity_t neutral = 0;
};

Scene findScene(const ksim::Simulation& simulation)
{
	const ksim::DoorSystem& doors = *simulation.ai()->doorSystem();
	const auto data = ksim::DoorData::fromWorld(simulation.world());
	std::map<flecs::entity_t, std::map<flecs::entity_t, std::vector<const ksim::DoorSite*>>> byOwner;
	for (const ksim::DoorSite& site : data->sites)
	{
		const flecs::entity_t town = doors.townOf(site.key);
		if (!site.gate || town == 0)
			continue;
		const ksim::state::TownState* state = entityOf(simulation, town).get<ksim::state::TownState>();
		if (state->owner.id == 0 || data->privateTowns.contains(state->activeSid))
			continue;
		byOwner[state->owner.id][town].push_back(&site);
	}
	std::map<flecs::entity_t, std::vector<std::uint64_t>> townsOf;
	simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::Id& id,
		const ksim::state::TownState& town)
	{
		townsOf[town.owner.id].push_back(id.value);
		(void)entity;
	});
	std::vector<flecs::entity_t> factions;
	simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::FactionState&)
	{
		factions.push_back(entity.id());
	});
	for (const auto& [owner, towns] : byOwner)
	{
		if (townsOf[owner].size() < 3)
			continue;
		Scene scene;
		scene.owner = owner;
		scene.gateTown = towns.begin()->first;
		for (const ksim::DoorSite* site : towns.begin()->second)
		{
			scene.gateKeys.push_back(site->key);
			if (site->alwaysLocks)
				scene.lockingKeys.push_back(site->key);
		}
		const std::uint64_t gateTownId = idOf(simulation, scene.gateTown);
		for (const std::uint64_t id : townsOf[owner])
			if (id != gateTownId && scene.spareTownId == 0)
				scene.spareTownId = id;
		for (const flecs::entity_t faction : factions)
		{
			const DoorClass stance = doors.classOf(faction, owner);
			if (stance == DoorClass::Hostile && scene.hostile == 0)
				scene.hostile = faction;
			if (stance == DoorClass::Neutral && scene.neutral == 0)
				scene.neutral = faction;
		}
		if (scene.hostile != 0 && scene.neutral != 0 && !scene.lockingKeys.empty())
			return scene;
	}
	throw std::runtime_error("no faction with gates, towns and a hostile and a neutral stranger");
}

// The first gate door with a town and no crew (nobody reopens it).
std::uint64_t uncrewedGate(const ksim::Simulation& simulation)
{
	const ksim::DoorSystem& doors = *simulation.ai()->doorSystem();
	std::set<std::uint64_t> crewed;
	simulation.world().ecs().each([&](const ksim::state::GateCrew& crew) { crewed.insert(crew.gate); });
	for (const ksim::DoorSite& site : ksim::DoorData::fromWorld(simulation.world())->sites)
		if (site.gate && doors.townOf(site.key) != 0 && !crewed.contains(site.key))
			return site.key;
	throw std::runtime_error("every gate has a crew");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// 1. Data.
		{
			ksim::Simulation simulation = newGame(order, 1);
			const auto data = ksim::DoorData::fromWorld(simulation.world());
			std::size_t gates = 0;
			std::size_t locking = 0;
			std::set<std::uint64_t> keys;
			for (const ksim::DoorSite& site : data->sites)
			{
				gates += site.gate ? 1 : 0;
				locking += site.gate && site.alwaysLocks ? 1 : 0;
				keys.insert(site.key);
			}
			// Ruins have no doors (7d6ac14, as the original; user-checked on the Hub 2026-11-04): 991 sites, was 1000+.
			test::require(data->sites.size() > 950, "too few door sites: " + std::to_string(data->sites.size()));
			test::require(keys.size() == data->sites.size(), "door keys collide");
			test::require(gates >= 100 && locking >= 1, "too few gate doors: " + std::to_string(gates));
			test::require(data->privateTowns.size() >= 30, "too few non-public towns");

			auto fake = std::make_shared<FakeNavigator>();
			enable(simulation, fake);
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			const ksim::DoorSystem& doors = *simulation.ai()->doorSystem();
			test::require(doors.stats().gates >= 90 && doors.stats().gateTowns >= 60,
				"gates were not bound to towns: " + std::to_string(doors.stats().gates));
			test::require(fake->applies() == 1, "the first book was not applied exactly once: " + std::to_string(fake->applies()));
			// The original has no class or faction door test: a private town's gates start as open as any other.
			test::require(fake->book().settings().empty(), "a gate starts restricted");
			std::cout << "data: " << data->sites.size() << " doors, " << gates << " on gate buildings ("
				<< locking << " always lock), " << doors.stats().gates << " bound to "
				<< doors.stats().gateTowns << " towns, " << data->privateTowns.size()
				<< " non-public TOWN records, no door restricted at the start\n";
		}

		// 2. Posture, 3. overrides, 4. classes.
		{
			ksim::Simulation simulation = newGame(order, 1);
			auto fake = std::make_shared<FakeNavigator>();
			enable(simulation, fake);
			simulation.run(ksim::kTicksPerGameHour + 1);
			const ksim::DoorSystem& doors = *simulation.ai()->doorSystem();
			const Scene scene = findScene(simulation);
			const knav::DoorBook calm = fake->book();
			for (const std::uint64_t key : scene.gateKeys)
				test::require(calm.get(key) == knav::DoorSetting{}, "a public town's gate starts restricted");

			// Classes.
			test::require(doors.classOf(scene.owner, scene.owner) == DoorClass::Resident, "own faction");
			test::require(doors.classOf(scene.hostile, scene.owner) == DoorClass::Hostile, "hostile stance");
			test::require(doors.classOf(scene.neutral, scene.owner) == DoorClass::Neutral, "neutral stance");
			test::require(doors.classOf(0, scene.owner) == DoorClass::Hostile, "a factionless squad is a stranger");
			test::require(doors.classOf(scene.hostile, 0) == DoorClass::Neutral, "an ownerless town treats all alike");
			// The exe's thresholds: hostile at -30 and below, allied at 50 and above.
			{
				const flecs::entity keeper = entityOf(simulation, scene.owner);
				const flecs::entity other = entityOf(simulation, scene.neutral);
				const double original = ksim::factionRelation(*simulation.gameData(), keeper, other);
				const auto classAt = [&](std::int32_t value)
				{
					simulation.enqueue(ksim::Command::setFactionRelation(
						ksim::state::Id{idOf(simulation, scene.owner)},
						ksim::state::Id{idOf(simulation, scene.neutral)}, value));
					simulation.run(1);
					return doors.classOf(scene.neutral, scene.owner);
				};
				test::require(classAt(-30) == DoorClass::Hostile, "-30 is hostile");
				test::require(classAt(-29) == DoorClass::Neutral, "-29 is neutral");
				test::require(classAt(-50) == DoorClass::Hostile, "-50 is hostile");
				test::require(classAt(49) == DoorClass::Neutral, "49 is neutral");
				test::require(classAt(50) == DoorClass::Ally, "50 is an ally");
				classAt(static_cast<std::int32_t>(original));
				test::require(doors.classOf(scene.neutral, scene.owner) == DoorClass::Neutral,
					"the neutral stance did not come back");
			}
			// The owner loses a town and a squad: it defends. Nothing closes a gate for that (retired Alert,
			// GATES_CARRY_LIVE.md a: MAN_THE_GATE never closes, no package but a shopkeeper's plans close / lock).
			const flecs::entity ownerEntity = entityOf(simulation, scene.owner);
			simulation.enqueue(ksim::Command::setTownOwner(ksim::state::Id{scene.spareTownId},
				ksim::state::Id{idOf(simulation, scene.hostile)}));
			simulation.run(ksim::kTicksPerGameHour);
			test::require(ownerEntity.get<ksim::state::FactionAI>()->intent == ksim::state::Intent::Defend,
				"the owner does not defend");
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			test::require(fake->book() == calm, "a gate closed because its owner defends");

			// 3. Overrides: lock one door (one nobody crews, or its crew would reopen it), then clear.
			const std::uint64_t key = uncrewedGate(simulation);
			simulation.enqueue(ksim::Command::setDoorState(key, 2));
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			test::require(fake->book().get(key).state == knav::DoorState::Locked, "the override did not lock");
			simulation.enqueue(ksim::Command::setDoorState(key, -1));
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			test::require(fake->book() == calm, "clearing the override did not restore the calm state");
			std::cout << "gates: the owner defends and " << scene.gateKeys.size() << " of its gates (" << scene.lockingKeys.size()
				<< " always lock) stay open; override lock/clear ok; " << fake->applies() << " books applied\n";
		}

		// 5. Squin on the real navmesh.
		if (argc == 6)
		{
			const std::string bake = argv[1];
			const int rx0 = std::stoi(argv[2]);
			const int rz0 = std::stoi(argv[3]);
			const int rx1 = std::stoi(argv[4]);
			const int rz1 = std::stoi(argv[5]);
			// Squin (2 locking gates, 1085 and 1469 units from the centre, so a raid meets one mid-route): the Hub's gates are ruins
			// with no doors (7d6ac14, user-checked 2026-11-04); Stack's west gate is 257 units from its centre.
			constexpr double kSquinX = -62461.0;
			constexpr double kSquinZ = 16346.8;
			ksim::Simulation simulation = newGame(order, 1);
			std::shared_ptr<ksim::KnavNavigator> navigator = ksim::KnavNavigator::open(KENSHI_DATA, bake,
				rx0, rz0, rx1, rz1, 4);
			enable(simulation, navigator);
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			const ksim::DoorSystem& doors = *simulation.ai()->doorSystem();

			flecs::entity_t squin = 0;
			double best = 800.0;
			simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::TownState& town)
			{
				const double far = std::hypot(town.x - kSquinX, town.z - kSquinZ);
				if (far < best)
				{
					best = far;
					squin = entity.id();
				}
			});
			test::require(squin != 0, "no town at Squin");
			const ksim::state::TownState townState = *entityOf(simulation, squin).get<ksim::state::TownState>();
			const flecs::entity_t owner = townState.owner.id;
			test::require(owner != 0, "Squin has no owner");
			flecs::entity_t hostile = 0;
			simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::FactionState&)
			{
				if (hostile == 0 && doors.classOf(entity.id(), owner) == DoorClass::Hostile)
					hostile = entity.id();
			});
			test::require(hostile != 0, "nobody is hostile to Squin's owner");
			std::vector<std::uint64_t> townGates;
			for (const ksim::DoorSite& site : ksim::DoorData::fromWorld(simulation.world())->sites)
				if (site.gate && doors.townOf(site.key) == squin)
					townGates.push_back(site.key);
			test::require(townGates.size() >= 2, "Squin has fewer than two gate doors");

			// Start points a street away from the walls, all round the town.
			struct Probe
			{
				double x = 0.0;
				double z = 0.0;
				double openLength = 0.0;
			};
			const auto length = [](const ksim::Route& route)
			{
				double total = 0.0;
				for (std::size_t i = 1; i < route.points.size(); ++i)
					total += std::hypot(route.points[i].x - route.points[i - 1].x,
						route.points[i].z - route.points[i - 1].z);
				return total;
			};
			const ksim::Traveller resident;
			const ksim::Traveller stranger; // the same: a door has no class, only its state refuses
			std::vector<Probe> probes;
			for (int k = 0; k < 12; ++k)
			{
				const double angle = k * 3.14159265358979 / 6.0;
				Probe probe{kSquinX + 1300.0 * std::cos(angle), kSquinZ + 1300.0 * std::sin(angle), 0.0};
				const ksim::Route open = navigator->routeFor(stranger, probe.x, probe.z, kSquinX, kSquinZ);
				if (!open.found)
					continue;
				probe.openLength = length(open);
				probes.push_back(probe);
			}
			test::require(probes.size() >= 3, "fewer than three streets lead to the centre of Squin");
			test::require(navigator->doors().settings().empty() || owner == 0
				|| !navigator->doors().settings().contains(townGates.front()), "the gates of Squin start restricted");

			const auto reach = [&](const ksim::Traveller& who, const Probe& probe)
			{
				const auto goal = navigator->nearestReachableFor(who, kSquinX, kSquinZ, 250.0, probe.x, probe.z);
				return goal && std::hypot(goal->x - kSquinX, goal->z - kSquinZ) < 260.0;
			};
			for (const Probe& probe : probes)
				test::require(reach(stranger, probe) && reach(resident, probe),
					"an open Squin is not reachable by goal pick");

			// Take Squin's crews off its gates (a crew reopens a shut gate within seconds, gatecrew.hpp) and lock them all: a
			// locked gate is a wall for everyone.
			{
				std::set<std::uint64_t> townKeys(townGates.begin(), townGates.end());
				std::vector<flecs::entity> crews;
				simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::GateCrew& crew)
				{
					if (townKeys.contains(crew.gate))
						crews.push_back(entity);
				});
				for (flecs::entity crew : crews)
					crew.remove<ksim::state::GateCrew>();
			}
			for (const std::uint64_t key : townGates)
				simulation.enqueue(ksim::Command::setDoorState(key, 2));
			simulation.run(ksim::kTicksPerGameHour + ksim::Simulation::kSquadIntervalTicks + 1);
			test::require(navigator->doors().get(townGates.front()).state == knav::DoorState::Locked, "the gates of Squin did not lock");
			std::size_t refused = 0;
			for (const Probe& probe : probes)
			{
				const ksim::Route inside = navigator->routeFor(resident, probe.x, probe.z, kSquinX, kSquinZ);
				const ksim::Route out = navigator->routeFor(stranger, probe.x, probe.z, kSquinX, kSquinZ);
				test::require(!inside.found || !out.found || length(out) >= probe.openLength * 0.95, "a locked gate changed nothing");
				refused += (!inside.found && !out.found) ? 1 : 0;
			}
			test::require(refused >= 1, "no route into Squin was refused by its locked gates");
			const std::uint64_t refusedRoutes = navigator->stats().doorRefusedRoutes;
			test::require(refusedRoutes >= 1, "refusals are not counted");

			// A raid that a gate refuses bashes it (DOORS.md section 6): it keeps its order, stops at the gate for the time the
			// bashing takes, and the gate stays broken, open to every class. Before this the refused route failed the order.
			{
				flecs::entity raider;
				double raiderX = 0.0;
				double raiderZ = 0.0;
				double raiderFar = 1.0e18;
				std::size_t hostiles = 0, refusedSquads = 0, midRoute = 0; // why no raider: printed with the failure
				simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::SquadInstance&, const ksim::state::Position& at,
					const ksim::state::GroupAI& ai)
				{
					if (ai.order.status != ksim::state::OrderStatus::None)
						return;
					const double far = std::hypot(at.x - kSquinX, at.z - kSquinZ);
					if (far < 1000.0 || far > 40000.0 ||  !navigator->covers(at.x, at.z)
						|| doors.classOf(hostile, owner) != DoorClass::Hostile)
						return;
					++hostiles;
					const ksim::Route out = navigator->routeFor(stranger, at.x, at.z, kSquinX, kSquinZ);
					if (!out.found && out.failure == ksim::state::AiReason::DoorRefused)
					{
						++refusedSquads;
						// The gate to bash must lie mid-route: a gate beside the town centre is met at the route's end, where
						// the faction layer already counts the raid as arrived.
						ksim::Traveller basher = stranger;
						basher.bash = true;
						const ksim::Route through = navigator->routeFor(basher, at.x, at.z, kSquinX, kSquinZ);
						const ksim::GateBash target = doors.bashing(hostile, kSquinX, kSquinZ);
						if (!through.found || target.key == 0)
							return;
						double along = 0.0, total = 0.0, best = 1.0e300, bestAlong = 0.0;
						for (std::size_t i = 0; i < through.points.size(); ++i)
						{
							if (i > 0)
								along += std::hypot(through.points[i].x - through.points[i - 1].x, through.points[i].z - through.points[i - 1].z);
							const double d = std::hypot(through.points[i].x - target.x, through.points[i].z - target.z);
							if (d < best)
							{
								best = d;
								bestAlong = along;
							}
						}
						total = along;
						if (bestAlong > total - 300.0)
							return;
						++midRoute;
						if (far >= raiderFar)
							return;
						raider = entity;
						raiderFar = far;
						raiderX = at.x;
						raiderZ = at.z;
					}
				});
				test::require(raider.is_valid(), "no squad of the window is refused by Squin's gates mid-route: "
					+ std::to_string(hostiles) + " hostile squads, " + std::to_string(refusedSquads) + " refused");
				ksim::state::SquadInstance squad = *raider.get<ksim::state::SquadInstance>();
				squad.faction = ksim::state::EntityRef{hostile};
				raider.set<ksim::state::SquadInstance>(squad);
				ksim::state::GroupAI ai = *raider.get<ksim::state::GroupAI>();
				ai.goal = GoalKind::None;
				ai.order.status = ksim::state::OrderStatus::Issued;
				ai.order.target = ksim::state::EntityRef{squin};
				ai.order.originX = raiderX;
				ai.order.originZ = raiderZ;
				ai.order.stay = 100;
				ai.order.deadline = simulation.clockTick() + 40 * ksim::kTicksPerGameHour;
				ai.order.issued = simulation.clockTick();
				raider.set<ksim::state::GroupAI>(ai);
				simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
				const ksim::state::GroupAI& started = *raider.get<ksim::state::GroupAI>();
				test::require(started.goal == GoalKind::Raid && started.order.status == ksim::state::OrderStatus::Outbound
					&& started.bashGate != 0, "a raid refused by the gates did not set out to bash one: goal "
					+ std::to_string(static_cast<int>(started.goal)) + " status " + std::to_string(static_cast<int>(started.order.status)));
				const std::uint64_t gate = started.bashGate;
				const double seconds = started.bashSeconds;
				test::require(doors.book().get(gate).state != knav::DoorState::Open, "the gate to bash is not shut");
				std::uint64_t hours = 0;
				while (doors.stats().gatesBroken == 0 && hours++ < 8 * 24)
				{
					simulation.run(ksim::kTicksPerGameHour / 4);
				}
				test::require(doors.stats().gatesBroken == 1, "the raiders never broke the gate: " + std::to_string(doors.stats().gatesBroken));
				simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
				const knav::DoorSetting broken = doors.book().get(gate);
				test::require(broken.state == knav::DoorState::Open, "a broken gate is not open to everyone");
				test::require(navigator->routeFor(stranger, raiderX, raiderZ, kSquinX, kSquinZ).found, "the broken gate still refuses strangers");
				const ksim::state::GroupAI& after = *raider.get<ksim::state::GroupAI>();
				test::require(after.order.status != ksim::state::OrderStatus::Failed && after.goal != GoalKind::None, "the raid lost its goal after the bash");
				std::cout << "raid bash: a hostile raid refused by Squin's gates stopped " << seconds << " s at gate " << gate
					<< " and broke it open for ever (" << doors.stats().gatesBroken << " broken)\n";
				// Put the gates back for the locked test below.
				simulation.enqueue(ksim::Command::setDoorState(gate, -1));
				simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			}

			// Locked for everyone: Squin's own squads keep a goal and none is stuck.
			for (const std::uint64_t key : townGates)
				simulation.enqueue(ksim::Command::setDoorState(key, 2));
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			std::size_t lockedOut = 0;
			for (const Probe& probe : probes)
				lockedOut += navigator->routeFor(resident, probe.x, probe.z, kSquinX, kSquinZ).found ? 0 : 1;
			simulation.run(2 * ksim::kTicksPerGameHour);
			const ksim::AiAudit audit = simulation.ai()->audit(simulation.world().ecs());
			test::require(audit.goalless == 0, "a squad has no goal behind locked gates");
			std::size_t townSquads = 0;
			simulation.world().ecs().each([&](const ksim::state::SquadInstance& squad, const ksim::state::GroupAI& ai)
			{
				if (squad.homeTown.id == squin)
				{
					++townSquads;
					test::require(ai.goal != GoalKind::None, "a Squin squad has no goal");
				}
			});
			// Unlock: everything as on alert again, then as before.
			for (const std::uint64_t key : townGates)
				simulation.enqueue(ksim::Command::setDoorState(key, -1));
			simulation.run(ksim::Simulation::kSquadIntervalTicks + 1);
			for (const Probe& probe : probes)
				test::require(navigator->routeFor(resident, probe.x, probe.z, kSquinX, kSquinZ).found,
					"Squin did not reopen to its residents after the unlock");
			std::cout << "squin: " << probes.size() << " streets, locked gates refused " << refused << " routes in; "
				<< lockedOut << " resident routes refused, " << townSquads << " Squin squads kept a goal; "
				<< navigator->stats().doorApplies << " books applied, " << navigator->stats().doorSkippedPolys
				<< " polygons skipped\n";
		}

		// 6. Workers and continuation, with an alert and an override in play.
		{
			constexpr std::uint64_t kHours = 24 * 2;
			std::uint64_t hashOne = 0;
			for (const int workers : {1, 4})
			{
				ksim::Simulation simulation = newGame(order, workers);
				enable(simulation, std::make_shared<FakeNavigator>());
				simulation.run(ksim::kTicksPerGameHour);
				const Scene scene = findScene(simulation);
				simulation.enqueue(ksim::Command::setTownOwner(ksim::state::Id{scene.spareTownId},
					ksim::state::Id{idOf(simulation, scene.hostile)}));
				simulation.enqueue(ksim::Command::setDoorState(uncrewedGate(simulation), 2));
				simulation.run(kHours * ksim::kTicksPerGameHour);
				if (workers == 1)
					hashOne = simulation.stateHash();
				else
					test::require(simulation.stateHash() == hashOne, "1 worker and 4 workers differ");
			}

			auto fake = std::make_shared<FakeNavigator>();
			ksim::Simulation reference = newGame(order, 1);
			enable(reference, fake);
			reference.run(ksim::kTicksPerGameHour);
			const Scene scene = findScene(reference);
			reference.enqueue(ksim::Command::setTownOwner(ksim::state::Id{scene.spareTownId},
				ksim::state::Id{idOf(reference, scene.hostile)}));
			const std::uint64_t locked = uncrewedGate(reference);
			reference.enqueue(ksim::Command::setDoorState(locked, 2));
			reference.run(ksim::kTicksPerGameHour + 37);
			const std::uint64_t split = reference.stateHash();
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_doors_test.ksav";
			reference.saveGame(path);
			const std::uint64_t after = 12 * ksim::kTicksPerGameHour + 11;
			reference.run(after);
			for (const int workers : {4, 1})
			{
				ksim::RegionConfig config;
				config.workerThreads = workers;
				ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr,
					config);
				test::require(loaded.stateHash() == split, "the loaded game hashes differently");
				auto loadedFake = std::make_shared<FakeNavigator>();
				enable(loaded, loadedFake);
				loaded.run(ksim::Simulation::kSquadIntervalTicks);
				// The book is rebuilt from the saved override at the first squad step.
				test::require(loadedFake->applies() == 1 && loadedFake->book().get(locked).state == knav::DoorState::Locked,
					"the loaded game did not rebuild its door book");
				loaded.run(after - ksim::Simulation::kSquadIntervalTicks);
				test::require(loaded.stateHash() == reference.stateHash(),
					"load + run differs from the uninterrupted run (" + std::to_string(workers) + " workers)");
				test::require(loadedFake->book() == fake->book(), "the loaded game's door book differs");
			}
			std::remove(path.c_str());
			std::cout << "determinism: 1 = 4 workers over " << kHours
				<< " hours with a lost town and a lock; save/load mid-lock continues identically\n";
		}
		std::cout << "ksim_doors: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_doors: " << error.what() << "\n";
		return 1;
	}
}
