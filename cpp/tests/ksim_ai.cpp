// The squad AI (include/ksim/ai/ai.hpp) on the default world:
//
//  1. the behaviour registry covers the spawned squads (coverage report), with
//     patrol and territory implemented;
//  2. a goal that fails (a navigator that cannot reach one town) is
//     blacklisted for a cooldown and a successor is picked: no squad is ever
//     goal-less, and a squad never travels to a town it is blacklisted for;
//  3. a short soak on the baked world_sample window (usage: ksim_ai BAKEDIR
//     RX0 RZ0 RX1 RZ1): no goal-less squad, arrivals happen, travelling squads
//     stand on the navmesh, none is stuck, the Faction stub
//     are called;
//  4. the result does not depend on the region worker count, for the stress
//     table and for the real one;
//  5. save at N, load, run M == the uninterrupted N + M run (route and goal
//     state are in the save), again for both tables;
//  6. wilderness squads all have a position (placement.hpp), on dry land and
//     deterministic by seed; the real table on the window: patrol and
//     territory squads end on the navmesh and stay within their leash;
//  7. patrol loops and territories everywhere (straight-line navigator, whole
//     world, real table): loops are walked round, territories stay within the
//     leash, nobody is ever goal-less;
//  8. wilderness by density (wilderness.hpp): every slot's quota is reached at
//     the start, a removed squad and a removed nest are replaced after their
//     respawn delay, the population multipliers scale the counts, 1 = 4
//     workers, and save with a respawn pending + load + run == uninterrupted.
//     Sections 1..7 use the one-squad-per-template wilderness
//     (wildernessByDensity = false), as the earlier slices did.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"
#include "ksim/world/squad_alive.hpp"

#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using ksim::state::AiReason;
using ksim::state::GoalKind;

// Straight lines everywhere, except that nothing can reach `blocked`.
class FakeNavigator final : public ksim::Navigator
{
public:
	FakeNavigator(double blockedX, double blockedZ)
		: blockedX_(blockedX)
		, blockedZ_(blockedZ)
	{
	}

	bool covers(double, double) const override { return true; }

	ksim::Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		ksim::Route route;
		if (std::hypot(toX - blockedX_, toZ - blockedZ_) < 1000.0)
		{
			route.failure = AiReason::NoRoute;
			return route;
		}
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}

private:
	double blockedX_;
	double blockedZ_;
};

ksim::RegionConfig regions(int workers)
{
	ksim::RegionConfig config;
	config.workerThreads = workers;
	return config;
}

ksim::Simulation newGame(const kworld::LoadOrder& order, int workers, std::uint64_t seed = 5, ksim::SpawnReport* report = nullptr)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	return ksim::Simulation::newGame(KENSHI_DATA, order, options, report, regions(workers));
}

ksim::Simulation newDensityGame(const kworld::LoadOrder& order, int workers,
	double population = 1.0, double nests = 1.0, std::uint64_t seed = 5)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	options.wildernessPopulation = population;
	options.nestMultiplier = nests;
	return ksim::Simulation::newGame(KENSHI_DATA, order, options, nullptr, regions(workers));
}

struct WildTotals
{
	std::int64_t homelessQuota = 0;
	std::int64_t homeless = 0;
	std::int64_t homelessPending = 0;
	std::int64_t nestQuota = 0;
	std::int64_t nests = 0;
	std::int64_t nestsPending = 0;
	std::size_t slotsFull = 0; // slots whose live counts equal their quotas
	std::size_t slots = 0;
};

// The homeless quota holes of the float arithmetic (bq15_corpse_cleanup.md d): per slot ceil(target - (|S2| + 0.5 |S3|)), at least 0.
std::int64_t quotaHoles(const ksim::Simulation& simulation)
{
	std::int64_t total = 0;
	for (const ksim::WildernessSlotCount& slot : simulation.wilderness()->counts(simulation.world().ecs()))
	{
		const float cur = static_cast<float>(slot.homeless - slot.homelessNonIntact) + 0.5f * static_cast<float>(slot.homelessNonIntact);
		total += std::max<std::int64_t>(0, static_cast<std::int64_t>(std::ceil(static_cast<float>(slot.homelessQuota) - cur)));
	}
	return total;
}

WildTotals totals(const ksim::Simulation& simulation)
{
	WildTotals total;
	for (const ksim::WildernessSlotCount& slot : simulation.wilderness()->counts(
		simulation.world().ecs()))
	{
		total.homelessQuota += slot.homelessQuota;
		total.homeless += slot.homeless;
		total.homelessPending += slot.homelessPending;
		total.nestQuota += slot.nestQuota;
		total.nests += slot.nests;
		total.nestsPending += slot.nestsPending;
		total.slotsFull += slot.homeless == slot.homelessQuota && slot.nests == slot.nestQuota ? 1 : 0;
		++total.slots;
	}
	return total;
}

std::shared_ptr<const ksim::BehaviourRegistry> registry(const ksim::Simulation& simulation,
	const std::string& table)
{
	return std::make_shared<ksim::BehaviourRegistry>(simulation.world(),
		ksim::BehaviourRegistry::readRules(table));
}

void enable(ksim::Simulation& simulation, const std::string& table,
	std::shared_ptr<const ksim::Navigator> navigator)
{
	ksim::AiSystem::Config config;
	config.registry = registry(simulation, table);
	config.navigator = std::move(navigator);
	simulation.enableAi(std::move(config));
}

// Every squad has a goal; nobody heads for a town it is blacklisted for.
// Returns how many cooldown entries exist.
std::size_t checkGoals(const ksim::Simulation& simulation)
{
	const ksim::AiAudit audit = simulation.ai()->audit(simulation.world().ecs());
	test::require(audit.goalless == 0, std::to_string(audit.goalless) + " squads without a goal");
	std::size_t cooldowns = 0;
	simulation.world().ecs().each([&](const ksim::state::GroupAI& ai)
	{
		cooldowns += ai.cooldowns.size();
		for (const ksim::state::GoalCooldown& entry : ai.cooldowns)
		{
			test::require(entry.kind != ai.goal || entry.target.id != ai.target.id,
				"a squad travels to a town it is blacklisted for");
		}
	});
	return cooldowns;
}

// Squads on a patrol or roam goal: how many, how many have walked a whole
// patrol loop, and the worst distance from the anchor in radii of their behaviour.
// The anchor as the AI picks it (ai.cpp GroupContext): home town, overridden by the
// populate's building, then by a gate crew's post; else the den.
struct LocalAudit
{
	double worst = 0.0;
	std::size_t walking = 0;
	std::size_t loops = 0;
};

LocalAudit auditLocal(const ksim::Simulation& simulation)
{
	LocalAudit audit;
	const flecs::world& ecs = simulation.world().ecs();
	ecs.each([&](flecs::entity entity, const ksim::state::SquadInstance& squad, const ksim::state::GroupAI& ai,
		const ksim::state::Position& position)
	{
		if (ai.goal != GoalKind::Patrol && ai.goal != GoalKind::Roam)
			return;
		++audit.walking;
		audit.loops += ai.leg >= ksim::kPatrolPoints ? 1 : 0;
		bool homed = squad.homeTown.id != 0;
		double anchorX = squad.denX;
		double anchorZ = squad.denZ;
		if (homed)
		{
			const ksim::state::TownState& town =
				*flecs::entity(ecs, squad.homeTown.id).get<ksim::state::TownState>();
			anchorX = town.x;
			anchorZ = town.z;
		}
		if (squad.homeBuilding != 0)
		{
			homed = true;
			anchorX = squad.homeX;
			anchorZ = squad.homeZ;
		}
		if (const ksim::state::GateCrew* crew = entity.get<ksim::state::GateCrew>())
		{
			homed = true;
			anchorX = crew->postX;
			anchorZ = crew->postZ;
		}
		const double radius = ksim::localRadius(simulation.ai()->registry().squad(
			squad.templateSid).behaviour, homed);
		test::require(radius > 0.0, "a squad on a local goal has no radius");
		audit.worst = std::max(audit.worst,
			std::hypot(position.x - anchorX, position.z - anchorZ) / radius);
	});
	return audit;
}

// Squads that walk a route stand on the navmesh. Returns how many were checked.
std::size_t checkOnMesh(const ksim::Simulation& simulation, const ksim::KnavNavigator& navigator,
	const std::string& bake, const kworld::Heightmap& terrain)
{
	std::map<std::pair<int, int>, knav::NavMesh> meshes;
	std::size_t checked = 0;
	simulation.world().ecs().each([&](const ksim::state::SquadInstance& squad,
		const ksim::state::GroupAI& ai, const ksim::state::Position& position)
	{
		if (!navigator.covers(position.x, position.z) || ai.route.empty())
			return;
		const kworld::ZoneCell cell = kworld::zoneCellOf(position.x, position.z);
		const std::pair<int, int> key{cell.x / knav::kRegionZones, cell.z / knav::kRegionZones};
		auto found = meshes.find(key);
		if (found == meshes.end())
			found = meshes.emplace(key, knav::NavMesh::loadRegion(bake, key.first, key.second,
				0)).first;
		const knav::NavQuery query(found->second);
		// A squad has no height, so any layer within 400 of the terrain counts. Probed in thin
		// slices: in one tall box Detour prefers a polygon a few units away sideways at about the
		// box's height over one straight over or under it (a ramp up to a gate deck).
		const float ground = static_cast<float>(terrain.height(position.x, position.z));
		bool on = false;
		for (float dy = -400.0f; dy <= 400.0f && !on; dy += 20.0f)
			on = query.onMesh({static_cast<float>(position.x), ground + dy, static_cast<float>(position.z)}, {3.0f, 10.0f, 3.0f});
		test::require(on,
			"a squad stands off the navmesh: " + squad.templateSid + " at "
			+ std::to_string(position.x) + ", " + std::to_string(position.z) + " (route " + std::to_string(ai.route.size()) + " from "
			+ std::to_string(ai.route.front().x) + ", " + std::to_string(ai.route.front().z) + " to " + std::to_string(ai.route.back().x) + ", "
			+ std::to_string(ai.route.back().z) + ", along " + std::to_string(ai.along) + ", goal " + std::to_string(static_cast<int>(ai.goal)) + ", source "
			+ std::to_string(static_cast<int>(squad.source)) + ", home " + std::to_string(squad.homeX) + ", " + std::to_string(squad.homeZ) + ")");
		++checked;
	});
	return checked;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 6, "usage: ksim_ai BAKEDIR RX0 RZ0 RX1 RZ1");
		const std::string bake = argv[1];
		const int window[4] = {std::stoi(argv[2]), std::stoi(argv[3]), std::stoi(argv[4]),
			std::stoi(argv[5])};
		const std::string table = KSIM_AI_TABLE;
		const std::string stress = std::string(KSIM_AI_DATA) + "/ksim_ai_stress.tsv";
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		const kworld::Heightmap terrain = kworld::Heightmap::open(
			std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");

		// 1. Coverage.
		{
			ksim::SpawnReport report;
			ksim::Simulation simulation = newGame(order, 1, 5, &report);
			std::cout << "spawn: squads " << report.squads << ", gate " << report.gateSquads << ", building " << report.buildingSquads << ", dropped "
				<< report.residentsDropped << ", by source";
			for (int s = 0; s < ksim::state::kSpawnSourceCount; ++s)
				std::cout << " " << ksim::state::spawnSourceName(static_cast<ksim::state::SpawnSource>(s)) << " " << report.squadsBySource[s];
			std::cout << "\n";
			const ksim::Coverage coverage = registry(simulation, table)->coverage(
				simulation.world().ecs());
			// The town populate (WORLD_MODEL.md "Who lives where") in the sample's loaded towns: 825 building squads, 75 gate crews, 888 resident
			// entries dropped (every building there has a zone preset); towns outside the sample keep one squad per entry at the town point. Was
			// 1069 (222 / 60 / 372) before stale zone town hands were rebound (Heng and most cities had no populate) and a `unique` character spawned
			// once; 1490 (797 / 82 / 883) while a stale hand went to the nearest town within 2500 u, before every building bound by position
			// (spawn.cpp bindTownHands, townbind_static.md Q1).
			test::require(coverage.squads == 1477, "default world is not 1477 squads: " + std::to_string(coverage.squads));
			test::require(coverage.squadsReal + coverage.squadsPlanned
				+ coverage.squadsUnmapped == coverage.squads, "coverage does not add up");
			test::require(coverage.squadsReal > 600 && coverage.packagesReal > 120,
				"too few packages map to a real behaviour");
			std::size_t travellers = 0;
			std::size_t local = 0;
			for (const auto& [name, count] : coverage.byBehaviour)
			{
				travellers += name == "travel" ? count : 0;
				local += name == "patrol_local" || name == "animal_territory" ? count : 0;
			}
			test::require(travellers > 0, "no squad maps to the travel behaviour");
			test::require(local > 100, "too few squads map to patrol or territory");
			// Patrol and territory are implemented: only raids are still planned.
			test::require(coverage.squadsPlanned < 10, "squads are still planned: "
				+ std::to_string(coverage.squadsPlanned));
			ksim::printCoverage(std::cout, coverage, 8);
		}

		// 6a. Wilderness placement: every squad has a position, on dry land,
		// the same for the same seed and different for another.
		{
			const auto dens = [](const ksim::Simulation& simulation)
			{
				std::vector<std::pair<double, double>> list;
				std::size_t wilderness = 0;
				simulation.world().ecs().each([&](const ksim::state::SquadInstance& squad,
					const ksim::state::Position& position)
				{
					if (squad.homeTown.id == 0)
					{
						test::require(squad.hasDen && squad.denX == position.x
							&& squad.denZ == position.z, "a wilderness squad has no den");
						list.emplace_back(position.x, position.z);
						++wilderness;
					}
				});
				test::require(wilderness > 150, "too few wilderness squads placed");
				return list;
			};
			ksim::Simulation first = newGame(order, 1);
			std::size_t positioned = 0;
			first.world().ecs().each([&](const ksim::state::SquadInstance&,
				const ksim::state::Position&)
			{
				++positioned;
			});
			test::require(positioned == 1477, "a squad has no position: " + std::to_string(positioned));
			const auto firstDens = dens(first);
			std::size_t dry = 0;
			for (const auto& [x, z] : firstDens)
			{
				test::require(x >= kworld::kWorldMin && x < kworld::kWorldMin + kworld::kWorldExtent
					&& z >= kworld::kWorldMin && z < kworld::kWorldMin + kworld::kWorldExtent,
					"a den is outside the world");
				dry += terrain.height(x, z) >= 100.0 ? 1 : 0;
			}
			test::require(dry * 10 >= firstDens.size() * 9, "fewer than 90% of the dens are on dry land");
			test::require(dens(newGame(order, 4)) == firstDens, "placement depends on the worker count");
			test::require(dens(newGame(order, 1, 6)) != firstDens, "another seed placed the same");
			std::cout << "placement: " << firstDens.size() << " wilderness squads, " << dry
				<< " on dry land\n";
		}

		// 2. Failed goals: blacklist, successor, never goal-less.
		{
			ksim::Simulation simulation = newGame(order, 1);
			double blockedX = 0.0;
			double blockedZ = 0.0;
			simulation.world().ecs().each([&](const ksim::state::TownState& town)
			{
				if (blockedX == 0.0)
				{
					blockedX = town.x;
					blockedZ = town.z;
				}
			});
			enable(simulation, stress, std::make_shared<FakeNavigator>(blockedX, blockedZ));
			std::size_t cooldowns = 0;
			for (int hour = 0; hour < 24 * 3; ++hour)
			{
				simulation.run(ksim::kTicksPerGameHour);
				cooldowns = std::max(cooldowns, checkGoals(simulation));
			}
			const ksim::AiStats& stats = simulation.ai()->stats();
			test::require(stats.failures[static_cast<std::size_t>(AiReason::NoRoute)] > 0,
				"no goal ever failed against the blocked town");
			test::require(cooldowns > 0, "a failed goal left no cooldown");
			test::require(stats.arrivals > 0, "no squad arrived anywhere");
			std::cout << "failed goals: no route " << stats.failures[
				static_cast<std::size_t>(AiReason::NoRoute)] << ", arrivals " << stats.arrivals
				<< ", most cooldowns at once " << cooldowns << "\n";
		}

		// 3. A short soak on the baked window.
		const std::shared_ptr<const ksim::KnavNavigator> navigator = ksim::KnavNavigator::open(
			KENSHI_DATA, bake, window[0], window[1], window[2], window[3], 4);
		// 3a. A local walk over a region border is routed on a window mesh (not refused), and a
		// goal picked with nearestReachable is reachable.
		{
			const double seam = kworld::kWorldMin + 4.0 * kworld::kZoneSize * (window[0] + 1);
			std::size_t crossed = 0;
			std::size_t reachable = 0;
			std::size_t tried = 0;
			for (double z = kworld::kWorldMin + 4.0 * kworld::kZoneSize * window[1] + 500.0;
				z < kworld::kWorldMin + 4.0 * kworld::kZoneSize * (window[1] + 1) - 500.0; z += 700.0)
			{
				const auto from = navigator->nearestWalkable(seam - 150.0, z, 200.0);
				if (!from)
					continue;
				const auto to = navigator->nearestReachable(seam + 150.0, z, 300.0, from->x, from->z);
				if (!to)
					continue;
				++tried;
				const ksim::Route route = navigator->localRoute(from->x, from->z, to->x, to->z);
				reachable += route.found ? 1 : 0;
				crossed += route.found && route.points.front().x < seam && route.points.back().x >= seam ? 1 : 0;
			}
			test::require(tried > 3, "too few border probes: " + std::to_string(tried));
			test::require(crossed > 0, "no local walk crossed a region border");
			test::require(reachable * 10 >= tried * 9, "goals picked reachable were not reachable: "
				+ std::to_string(reachable) + " of " + std::to_string(tried));
			test::require(navigator->stats().crossCalls > 0, "no border walk was routed on a window");
			std::cout << "region border: " << reachable << " of " << tried << " reachable, " << crossed
				<< " crossed the seam\n";
		}

		std::uint64_t soakHash = 0;
		{
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, stress, navigator);
			// A player at the Hub makes its region full detail.
			simulation.enqueue(ksim::Command::setPlayerAreas({{-50978.8, 2932.48}}));
			ksim::SoakOptions options;
			options.days = 2.0;
			const ksim::SoakReport report = ksim::runSoak(simulation, options);
			test::require(report.maxGoalless == 0, "a squad had no goal");
			test::require(report.arrivals > 100, "too few arrivals: " + std::to_string(report.arrivals));
			test::require(report.squadsThatMoved > 10, "too few squads moved");
			test::require(report.stuckEpisodes == 0 && report.stuckNow == 0, "a squad got stuck");
			test::require(report.maxOffRoute == 0, "a travelling squad left its route");
			const ksim::AiStats& stats = simulation.ai()->stats();
			test::require(stats.factionCalls > 0, "the faction layer was never called");

			const std::size_t checked = checkOnMesh(simulation, *navigator, bake, terrain);
			test::require(checked > 10, "too few squad positions checked against the mesh");
			soakHash = simulation.stateHash();
			std::cout << "soak: " << report.arrivals << " arrivals, " << report.squadsThatMoved
				<< " squads moved, " << checked << " positions on the mesh, "
				<< stats.factionCalls << " faction calls\n";
		}

		// 4. The same run with four workers, and the real table on both.
		{
			ksim::Simulation simulation = newGame(order, 4);
			enable(simulation, stress, navigator);
			simulation.enqueue(ksim::Command::setPlayerAreas({{-50978.8, 2932.48}}));
			ksim::SoakOptions options;
			options.days = 2.0;
			ksim::runSoak(simulation, options);
			test::require(simulation.stateHash() == soakHash, "4 workers differ from 1");
		}

		// 6b. The real table on the window: local goals on the navmesh, in leash.
		std::uint64_t realHash = 0;
		{
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, table, navigator);
			std::size_t walked = 0;
			std::size_t loops = 0;
			double worst = 0.0;
			ksim::SoakOptions options;
			options.days = 1.0;
			for (int day = 0; day < 3; ++day)
			{
				const ksim::SoakReport report = ksim::runSoak(simulation, options);
				test::require(report.maxGoalless == 0, "a squad had no goal (real table)");
				test::require(report.stuckEpisodes == 0, "a squad got stuck (real table)");
				const LocalAudit audit = auditLocal(simulation);
				walked = std::max(walked, audit.walking);
				loops = std::max(loops, audit.loops);
				worst = std::max(worst, audit.worst);
			}
			// Real routes may detour a little past the start of a step: 0.3 radii.
			test::require(worst <= ksim::kLocalLeash + 0.3, "a local goal left its leash: "
				+ std::to_string(worst));
			const std::size_t checked = checkOnMesh(simulation, *navigator, bake, terrain);
			realHash = simulation.stateHash();
			std::cout << "real table on the window: up to " << walked << " squads on local goals, "
				<< loops << " past a first loop, worst " << worst << " radii, " << checked
				<< " positions on the mesh\n";
		}
		{
			ksim::Simulation simulation = newGame(order, 4);
			enable(simulation, table, navigator);
			ksim::SoakOptions options;
			options.days = 1.0;
			for (int day = 0; day < 3; ++day)
				ksim::runSoak(simulation, options);
			test::require(simulation.stateHash() == realHash,
				"4 workers differ from 1 (real table)");
		}

		// 5. Save at N, load, run M == uninterrupted N + M.
		for (const std::string& tableUnderTest : {stress, table})
		{
			constexpr std::uint64_t kBefore = 5 * ksim::kTicksPerGameHour + 337;
			constexpr std::uint64_t kAfter = 7 * ksim::kTicksPerGameHour + 41;
			ksim::Simulation reference = newGame(order, 1);
			enable(reference, tableUnderTest, navigator);
			reference.run(kBefore);
			const std::uint64_t split = reference.stateHash();
			std::size_t routes = 0;
			reference.world().ecs().each([&](const ksim::state::GroupAI& ai)
			{
				routes += ai.route.empty() ? 0 : 1;
			});
			// The real table has few travellers in the window: 9 routes on the
			// first bake, 5 on the 2026-10-31 default bake (climb 5, slope 40,
			// stride 2: 7 "no route" failures instead of 1, the mesh joins fewer
			// towns). The stress table has ~45.
			test::require(routes > (tableUnderTest == table ? 0u : 5u),
				"no squad is on a route at the save point");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_ai_test.ksav";
			reference.saveGame(path);
			reference.run(kAfter);

			for (const int workers : {4, 1})
			{
				ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path,
					false, nullptr, regions(workers));
				test::require(loaded.stateHash() == split, "the loaded game hashes differently");
				enable(loaded, tableUnderTest, navigator);
				loaded.run(kAfter);
				test::require(loaded.stateHash() == reference.stateHash(),
					"load + run differs from the uninterrupted run (" + std::to_string(workers)
					+ " workers)");
			}
			std::remove(path.c_str());
			std::cout << "continuation (" << (tableUnderTest == table ? "real" : "stress")
				<< " table): " << routes << " squads on routes at the save, " << kBefore
				<< " + " << kAfter << " ticks identical\n";
		}

		// 7. Patrol loops and territories everywhere, on straight lines.
		{
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, table, std::make_shared<FakeNavigator>(1.0e12, 1.0e12));
			std::size_t walked = 0;
			std::size_t loops = 0;
			double worst = 0.0;
			for (int hour = 0; hour < 24 * 3; ++hour)
			{
				simulation.run(ksim::kTicksPerGameHour);
				checkGoals(simulation);
				const LocalAudit audit = auditLocal(simulation);
				walked = std::max(walked, audit.walking);
				loops = std::max(loops, audit.loops);
				worst = std::max(worst, audit.worst);
			}
			test::require(walked > 50, "too few squads walked a local goal: " + std::to_string(walked));
			test::require(loops > 10, "too few squads walked round a patrol loop");
			test::require(worst <= ksim::kLocalLeash + 1e-6, "a local goal left its leash: "
				+ std::to_string(worst));
			std::cout << "local goals everywhere: up to " << walked << " squads walking, "
				<< loops << " past a first loop, worst " << worst << " radii\n";
		}

		// 8. Wilderness by density.
		{
			using ksim::state::SquadInstance;
			ksim::Simulation first = newDensityGame(order, 1);
			const WildTotals start = totals(first);
			test::require(start.slots > 30, "too few wilderness slots: " + std::to_string(start.slots));
			test::require(start.slotsFull == start.slots, "a slot is below its quota at the start");
			test::require(start.homeless == start.homelessQuota && start.homelessQuota > 2500,
				"the homeless quota is not reached: " + std::to_string(start.homeless) + " of "
				+ std::to_string(start.homelessQuota));
			test::require(start.nests == start.nestQuota && start.nestQuota > 200,
				"the nest quota is not reached: " + std::to_string(start.nests) + " of "
				+ std::to_string(start.nestQuota));
			test::require(start.homelessPending == 0 && start.nestsPending == 0,
				"respawns are pending at the start");
			std::size_t squads = 0;
			std::size_t wild = 0;
			first.world().ecs().each([&](const SquadInstance& squad,
				const ksim::state::Position& position)
			{
				++squads;
				if (squad.slot >= 0)
				{
					++wild;
					test::require(squad.hasDen && squad.denX == position.x && squad.denZ == position.z
						&& squad.homeTown.id == 0, "a spawned wilderness squad has no den");
				}
			});
			test::require(wild > 3000 && squads > wild, "implausible squad counts");
			std::cout << "wilderness: " << start.slots << " slots, " << start.homeless
				<< " homeless squads, " << start.nests << " nests, " << squads << " squads\n";

			// The multipliers scale the quotas; population rounds per slot.
			{
				const ksim::Simulation scaled = newDensityGame(order, 1, 0.5, 2.0);
				const WildTotals half = totals(scaled);
				test::require(half.slotsFull == half.slots, "a scaled slot is below its quota");
				test::require(std::abs(half.homelessQuota - start.homelessQuota / 2)
					<= static_cast<std::int64_t>(start.slots), "population 0.5 did not halve the quota");
				test::require(half.nestQuota >= 2 * start.nestQuota - static_cast<std::int64_t>(start.slots)
					&& half.nestQuota <= 2 * start.nestQuota + static_cast<std::int64_t>(start.slots),
					"nests 2.0 did not double the quota");
				test::require(half.homeless == half.homelessQuota && half.nests == half.nestQuota,
					"a scaled quota is not reached");
				std::cout << "multipliers: population 0.5 -> " << half.homeless << " squads, nests x2 -> "
					<< half.nests << "\n";
			}

			// Kill one homeless squad and one whole nest.
			std::uint64_t victim = 0;
			std::uint64_t nestKey = 0;
			std::vector<std::uint64_t> nestSquads;
			std::int32_t victimSlot = -1;
			std::int32_t nestSlot = -1;
			first.world().ecs().each([&](const ksim::state::Id& id, const SquadInstance& squad)
			{
				if (squad.slot >= 0 && squad.nest == 0 && (victim == 0 || id.value < victim))
				{
					victim = id.value;
					victimSlot = squad.slot;
				}
			});
			first.world().ecs().each([&](const ksim::state::Id&, const SquadInstance& squad)
			{
				if (squad.nest != 0 && (nestKey == 0 || squad.nest < nestKey))
				{
					nestKey = squad.nest;
					nestSlot = squad.slot;
				}
			});
			first.world().ecs().each([&](const ksim::state::Id& id, const SquadInstance& squad)
			{
				if (squad.nest == nestKey)
					nestSquads.push_back(id.value);
			});
			test::require(victim != 0 && nestKey != 0 && nestSquads.size() >= 1, "nothing to kill");
			const std::size_t charactersBefore = static_cast<std::size_t>(
				first.world().ecs().count<ksim::state::CharacterInstance>());
			first.enqueue(ksim::Command::removeSquad(ksim::state::Id{victim}));
			for (const std::uint64_t id : nestSquads)
				first.enqueue(ksim::Command::removeSquad(ksim::state::Id{id}));
			first.run(3 * ksim::kTicksPerGameHour);
			const WildTotals hurt = totals(first);
			test::require(hurt.homeless == start.homeless - 1 && hurt.nests == start.nests - 1,
				"a kill did not free its quota");
			// The float quota (bq15_corpse_cleanup.md d): cur = |S2| + 0.5 |S3|, a hole is queued while target > cur. Besides the killed
			// squad's slot, a squad that spawned far below its template's nominal size (a draw of 1 of 7: below 0.2 x nominal, so
			// non-intact from birth, s79 2.3 / c) counts 0.5 and opens a hole too.
			test::require(quotaHoles(first) >= 1 && hurt.homelessPending == quotaHoles(first) && hurt.nestsPending == 1,
				"the holes (killed squad 1, non-intact ones 0.5 each) did not queue exactly their respawns");
			test::require(static_cast<std::size_t>(first.world().ecs().count<
				ksim::state::CharacterInstance>()) < charactersBefore, "the members were not removed");
			(void)victimSlot;
			(void)nestSlot;

			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_wild_test.ksav";
			first.saveGame(path);
			const std::uint64_t splitHash = first.stateHash();
			constexpr std::uint64_t kRefill = ksim::kNestRespawnMaxHours * ksim::kTicksPerGameHour;
			first.run(kRefill);
			const WildTotals healed = totals(first);
			// Every killed squad is replaced; a replacement that itself spawned below 0.2 x nominal (non-intact, 0.5) opens a new hole with a
			// new delay, so a hole or two may still be queued after one delay window: the queue must hold exactly the remaining float-quota
			// holes (bq15_corpse_cleanup.md d), and the squad count may exceed the start by the replacements of non-intact squads.
			test::require(healed.homeless >= start.homeless && healed.nests == start.nests
				&& healed.homelessPending == quotaHoles(first) && healed.nestsPending == 0 && healed.homelessPending <= 2,
				"the quota was not refilled after the respawn delay (holes " + std::to_string(quotaHoles(first)) + ", pending " + std::to_string(healed.homelessPending) + ")");
			const std::uint64_t healedHash = first.stateHash();
			std::cout << "respawn: killed 1 squad and 1 nest (" << nestSquads.size()
				<< " squads), both refilled within " << ksim::kNestRespawnMaxHours << " game-hours\n";

			// 4 workers, and save + load, reach the same state.
			ksim::Simulation four = newDensityGame(order, 4);
			four.enqueue(ksim::Command::removeSquad(ksim::state::Id{victim}));
			for (const std::uint64_t id : nestSquads)
				four.enqueue(ksim::Command::removeSquad(ksim::state::Id{id}));
			four.run(3 * ksim::kTicksPerGameHour);
			test::require(four.stateHash() == splitHash, "4 workers differ from 1 at the kill");
			four.run(kRefill);
			test::require(four.stateHash() == healedHash, "4 workers differ from 1 after the respawn");

			ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path, false,
				nullptr, regions(1));
			test::require(loaded.stateHash() == splitHash, "the loaded game hashes differently");
			test::require(loaded.wilderness() != nullptr, "a loaded game lost its wilderness spawner");
			loaded.run(kRefill);
			test::require(loaded.stateHash() == healedHash,
				"load + run differs from the uninterrupted run (respawn in flight)");
			std::remove(path.c_str());
			std::cout << "wilderness: 1 = 4 workers, save with a respawn pending + load identical\n";

			// A wiped spawner squad (every member dead) leaves the quota and is replaced after the delay; its bodies and their
			// gear stay in the world, through a save and load. A squad with a KO'd survivor stays.
			{
				ksim::Simulation wipe = newDensityGame(order, 1);
				const WildTotals before = totals(wipe);
				flecs::world& ecs = const_cast<flecs::world&>(wipe.world().ecs());
				std::vector<std::pair<std::uint64_t, flecs::entity>> homeless;
				ecs.each([&](flecs::entity squad, const ksim::state::Id& id, const SquadInstance& instance)
				{
					if (instance.slot >= 0 && instance.nest == 0)
						homeless.emplace_back(id.value, squad);
				});
				std::sort(homeless.begin(), homeless.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				test::require(homeless.size() >= 2, "no spawner squads to wipe");
				const auto membersOf = [&](flecs::entity squad)
				{
					std::vector<flecs::entity> members;
					ecs.query_builder<>().with<ksim::state::MemberOf>(squad).build().each([&](flecs::entity m) { members.push_back(m); });
					return members;
				};
				std::vector<std::uint64_t> bodyIds;
				for (const flecs::entity member : membersOf(homeless[0].second))
				{
					ksim::state::Fighter dead;
					dead.dead = true;
					member.set<ksim::state::Fighter>(dead);
					ksim::state::Inventory pack;
					ksim::state::Inventory::Entry item;
					item.itemSid = "wiped-squad-loot";
					item.count = 1;
					pack.items.push_back(item);
					member.set<ksim::state::Inventory>(pack);
					bodyIds.push_back(member.get<ksim::state::Id>()->value);
				}
				test::require(!bodyIds.empty(), "the wiped squad has no members");
				// The second squad: all dead but one, who is KO'd (alive, wakes up).
				const std::uint64_t survivorSquad = homeless[1].first;
				std::size_t at = 0;
				for (const flecs::entity member : membersOf(homeless[1].second))
				{
					ksim::state::Fighter down;
					down.dead = at++ != 0;
					down.unconscious = !down.dead;
					member.set<ksim::state::Fighter>(down);
				}
				const auto bodiesLeft = [&](ksim::Simulation& s)
				{
					std::size_t found = 0;
					for (const std::uint64_t body : bodyIds)
					{
						s.world().ecs().each([&](const ksim::state::Id& id, const ksim::state::Fighter& f, const ksim::state::Inventory& inv)
						{
							found += id.value == body && f.dead && inv.items.size() == 1 && inv.items[0].itemSid == "wiped-squad-loot" ? 1 : 0;
						});
					}
					return found;
				};
				const auto squadAlive = [&](ksim::Simulation& s, std::uint64_t squadId)
				{
					std::size_t found = 0;
					s.world().ecs().each([&](const ksim::state::Id& id, const SquadInstance&) { found += id.value == squadId ? 1 : 0; });
					return found != 0;
				};
				// BQ15 (bq15_corpse_cleanup.md a, s71_static.md 5.3 point 2): a wiped squad is NOT retired at once. Its dead members stay in the
				// platoon array until the corpses are destroyed, so it stays, non-intact, at 0.5 in the quota (cur = |S2| + 0.5 |S3|). With
				// no player area nothing is evaluated, so nothing decays: three game hours change nothing.
				// The quota holes the non-intact squads open: per slot ceil(quota - (homeless - nonIntact) - 0.5 nonIntact), at least 0.
				const auto holes = [&](ksim::Simulation& s) { return quotaHoles(s); };
				test::require(!ksim::squadVitals(ecs, homeless[0].second).intact && ksim::squadVitals(ecs, homeless[0].second).dead
					== ksim::squadVitals(ecs, homeless[0].second).members, "the all-dead squad is intact or has no corpses");
				wipe.run(3 * ksim::kTicksPerGameHour);
				const WildTotals hurt = totals(wipe);
				test::require(squadAlive(wipe, homeless[0].first), "the all-dead squad was retired before its corpses decayed");
				test::require(squadAlive(wipe, survivorSquad), "a squad with a KO'd survivor was retired");
				test::require(hurt.homeless == before.homeless, "no squad leaves the quota while its corpses lie");
				test::require(holes(wipe) >= 1 && hurt.homelessPending == holes(wipe),
					"the non-intact squads (0.5 each) did not open exactly the float-quota holes");
				test::require(bodiesLeft(wipe) == bodyIds.size(), "the bodies or their gear did not outlive three game hours");
				// The holes fill by spawning (target > cur): the squad count grows by the holes, the non-intact ones stay.
				const std::int64_t opened = holes(wipe);
				wipe.run(2 * ksim::kHomelessRespawnMaxHours * ksim::kTicksPerGameHour); // the holes were queued during the 3 h above: due within 84 h of that
				const WildTotals refilled = totals(wipe);
				// (A replacement that spawned below 0.2 x nominal is itself non-intact and may leave a hole: the queue holds exactly those.)
				test::require(refilled.homeless >= before.homeless + opened && refilled.homelessPending == holes(wipe) && holes(wipe) <= 2,
					"the quota holes of the non-intact squads were not filled: opened " + std::to_string(opened) + ", squads " + std::to_string(before.homeless) + " -> "
					+ std::to_string(refilled.homeless) + ", pending " + std::to_string(refilled.homelessPending) + ", holes " + std::to_string(holes(wipe)));
				test::require(squadAlive(wipe, homeless[0].first) && bodiesLeft(wipe) == bodyIds.size(), "the all-dead squad or its bodies went without a player nearby");
				// A player area over the bodies (they stand at 0, 0): they are stamped at the first evaluation and destroyed 12 game hours
				// later; the stamp survives a save and load; the squad is retired when its last corpse is gone.
				wipe.enqueue(ksim::Command::setPlayerAreas({ksim::state::GroundPoint{0.0, 0.0}}));
				wipe.run(6 * ksim::kTicksPerGameHour);
				test::require(bodiesLeft(wipe) == bodyIds.size() && squadAlive(wipe, homeless[0].first), "the bodies decayed after 6 game hours");
				const std::string wipePath = std::string(KSIM_BINARY_DIR) + "/ksim_wipe_test.ksav";
				wipe.saveGame(wipePath);
				wipe.run(7 * ksim::kTicksPerGameHour);
				test::require(bodiesLeft(wipe) == 0, "the bodies were not destroyed 12 game hours after they were first evaluated");
				wipe.run(ksim::kTicksPerGameHour);
				test::require(!squadAlive(wipe, homeless[0].first), "the squad outlived its last corpse");
				test::require(squadAlive(wipe, survivorSquad), "the squad with the KO'd survivor went with its corpses");
				ksim::Simulation again = ksim::Simulation::loadGame(KENSHI_DATA, order, wipePath, false, nullptr, regions(1));
				test::require(bodiesLeft(again) == bodyIds.size() && squadAlive(again, homeless[0].first),
					"the bodies did not survive the save and load");
				again.run(7 * ksim::kTicksPerGameHour + ksim::kTicksPerGameHour);
				test::require(bodiesLeft(again) == 0 && !squadAlive(again, homeless[0].first),
					"the saved stamp did not carry: the loaded bodies did not decay on schedule");
				test::require(again.stateHash() == wipe.stateHash(), "load + run differs from the uninterrupted run after the corpses decayed");
				std::remove(wipePath.c_str());
				std::cout << "wilderness: a wiped squad (" << bodyIds.size() << " bodies) stayed at 0.5 until its corpses decayed (12 game h in a player area), then left; stamp kept through save + load\n";
			}

			// Squad liveness (kinspect s71_static.md 5, ARCHITECTURE.md "Squad liveness"): a squad whose members are all KO'd, or dead
			// and KO'd, is not retired (it is non-intact, the exe erases it only on unload); a squad with no member at all is retired.
			// A KO'd member is "effective" only when its injury severity is under 0.2 (s79_static.md 2.3, 14079acf0), so the KO'd members of
			// these tests carry a vital part at 0 HP (severity 1), as a member knocked out by wounds does.
			{
				const auto wounded = [](ksim::state::Fighter fighter)
				{
					ksim::state::FighterPart head;
					head.vital = true;
					head.health = 0.0;
					fighter.parts.push_back(head);
					return fighter;
				};
				ksim::Simulation live = newDensityGame(order, 1);
				flecs::world& ecs = const_cast<flecs::world&>(live.world().ecs());
				std::vector<std::pair<std::uint64_t, flecs::entity>> homeless;
				ecs.each([&](flecs::entity squad, const ksim::state::Id& id, const SquadInstance& instance)
				{
					if (instance.slot >= 0 && instance.nest == 0)
						homeless.emplace_back(id.value, squad);
				});
				std::sort(homeless.begin(), homeless.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
				test::require(homeless.size() >= 4, "no spawner squads for the liveness test");
				const auto membersOf = [&](flecs::entity squad)
				{
					std::vector<flecs::entity> members;
					ecs.query_builder<>().with<ksim::state::MemberOf>(squad).build().each([&](flecs::entity m) { members.push_back(m); });
					return members;
				};
				const auto setStates = [&](flecs::entity squad, bool deadFirst)
				{
					std::size_t at = 0;
					for (const flecs::entity member : membersOf(squad))
					{
						ksim::state::Fighter down;
						down.dead = deadFirst && at++ == 0;
						down.unconscious = !down.dead;
						member.set<ksim::state::Fighter>(wounded(down));
					}
				};
				const auto exists = [&](std::uint64_t squadId)
				{
					std::size_t found = 0;
					live.world().ecs().each([&](const ksim::state::Id& id, const SquadInstance&) { found += id.value == squadId ? 1 : 0; });
					return found != 0;
				};
				setStates(homeless[0].second, false); // all KO'd
				setStates(homeless[1].second, true); // one dead, the rest KO'd
				for (const flecs::entity member : membersOf(homeless[2].second)) // no member at all
					member.destruct();
				test::require(!ksim::squadVitals(ecs, homeless[0].second).intact && ksim::squadVitals(ecs, homeless[0].second).effective == 0,
					"an all-KO'd squad is intact");
				test::require(ksim::squadVitals(ecs, homeless[1].second).dead == 1 && !ksim::squadVitals(ecs, homeless[1].second).intact,
					"a dead + KO'd squad is intact");
				test::require(ksim::squadVitals(ecs, homeless[2].second).members == 0, "the memberless squad still has members");
				live.run(3 * ksim::kTicksPerGameHour);
				test::require(exists(homeless[0].first), "an all-KO'd squad was retired");
				test::require(exists(homeless[1].first), "a dead + KO'd squad was retired");
				test::require(!exists(homeless[2].first), "a squad with no members was not retired");
				// A KO'd squad that wakes is intact again (every member awake: effective = size).
				for (const flecs::entity member : membersOf(homeless[0].second))
					member.set<ksim::state::Fighter>(ksim::state::Fighter{});
				test::require(ksim::squadVitals(ecs, homeless[0].second).intact, "a woken squad is not intact");
				live.run(ksim::kTicksPerGameHour);
				test::require(exists(homeless[0].first), "a woken squad was retired");
				// Thresholds: a squad of 4 or more is intact at 20% effective (40% with the leader down); small ones follow the leader.
				const auto synthetic = [&](std::size_t size, bool leaderUp, std::size_t othersAwake)
				{
					flecs::entity squad = ecs.entity();
					for (std::size_t i = 0; i < size; ++i)
					{
						ksim::state::Fighter fighter;
						const bool up = i == 0 ? leaderUp : i <= othersAwake;
						fighter.unconscious = !up;
						flecs::entity member = ecs.entity().add<ksim::state::MemberOf>(squad);
						member.set<ksim::state::Fighter>(up ? fighter : wounded(fighter));
						member.set<ksim::state::MemberRole>(ksim::state::MemberRole{i == 0 ? ksim::state::kRoleLeader : ksim::state::kRoleSquad});
					}
					const bool intact = ksim::squadVitals(ecs, squad).intact;
					ecs.delete_with<ksim::state::MemberOf>(squad.id());
					squad.destruct();
					return intact;
				};
				// Effective counts the leader: size 5 needs 1 (20%), or 2 (40%) with the leader down; size 3 follows the leader alone.
				test::require(synthetic(5, true, 0) && !synthetic(5, false, 0), "size 5: the lone awake leader is intact, an all-KO'd squad not");
				test::require(synthetic(5, false, 2) && !synthetic(5, false, 1), "size 5, leader down: intact at 2 effective (40%), not at 1");
				test::require(synthetic(3, true, 0) && !synthetic(3, false, 2), "size 3 follows the leader, not the count");
				// BQ15 follow-ups (bq15_corpse_cleanup.md b, c; s79_static.md 2.2 to 2.4). A downed member counts as effective when its injury
				// severity is strictly under 0.2: max(0.5 (1 - min part), 1 - min vital part), the bleeding term when larger and > 0.2.
				const auto part = [](bool vital, double health)
				{
					ksim::state::FighterPart p;
					p.vital = vital;
					p.maxHealth = 100.0;
					p.health = health;
					return p;
				};
				{
					ksim::state::Fighter f;
					test::require(ksim::injurySeverity(f) == 0.0, "an unhurt fighter has severity 0");
					f.parts = {part(false, 100.0), part(true, 100.0)};
					test::require(ksim::injurySeverity(f) == 0.0, "full parts have severity 0");
					f.parts = {part(false, 60.0), part(true, 100.0)};
					test::require(ksim::injurySeverity(f) == ksim::kEffectiveSeverity, "a part at 60% is exactly 0.2: 0.5 x (1 - 0.6)");
					f.parts = {part(false, 100.0), part(true, 0.0)};
					test::require(ksim::injurySeverity(f) == 1.0, "a vital part at 0 HP is severity 1");
					f.parts = {part(true, 100.0)};
					f.blood = 75.0; // half the blood of a 150 max (strength 100: maxBlood = 150)
					f.minBlood = 75.0;
					f.maxBloodRace = 150.0;
					f.skills.strength = 100.0;
					f.bleeding = 0.5;
					test::require(std::abs(ksim::injurySeverity(f) - 0.5 * 0.5 * 10.0) < 1.0e-9, "the bleeding term (1 - blood / max) x rate x 10 replaces a smaller value when > 0.2");
				}
				const auto downMember = [&](flecs::entity squad, bool leader, ksim::state::Fighter fighter)
				{
					flecs::entity member = ecs.entity().add<ksim::state::MemberOf>(squad);
					member.set<ksim::state::Fighter>(fighter);
					member.set<ksim::state::MemberRole>(ksim::state::MemberRole{leader ? ksim::state::kRoleLeader : ksim::state::kRoleSquad});
					return member;
				};
				{
					// One downed leader alone: effective only under the threshold (strictly: 60% = exactly 0.2 is NOT effective).
					const auto effectiveWith = [&](double partHealth, bool carriedNotKo)
					{
						flecs::entity squad = ecs.entity();
						ksim::state::Fighter fighter;
						fighter.unconscious = !carriedNotKo;
						fighter.carriedBy = carriedNotKo ? 7 : 0;
						fighter.parts = {part(false, partHealth)};
						downMember(squad, true, fighter);
						const std::size_t effective = ksim::squadVitals(ecs, squad).effective;
						ecs.delete_with<ksim::state::MemberOf>(squad.id());
						squad.destruct();
						return effective;
					};
					test::require(effectiveWith(61.0, false) == 1, "a KO'd member at severity 0.195 is effective");
					test::require(effectiveWith(60.0, false) == 0, "a KO'd member at severity exactly 0.2 is not effective (strict <)");
					test::require(effectiveWith(100.0, false) == 1, "an unhurt KO'd member is effective (severity 0)");
					test::require(effectiveWith(10.0, true) == 0, "a carried, badly hurt member is not effective");
					test::require(effectiveWith(100.0, true) == 1, "a carried but unhurt member is effective");
				}
				{
					// Nominal size: the template's, not the live count (platoon+0xa4, 1408fe0f0).
					const kdata::GameData& data = *live.gameData();
					const kdata::Record* big = nullptr;
					for (const kdata::Record* record : data.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
					{
						if (ksim::nominalSquadSize(*record) >= 6 && big == nullptr)
							big = record;
					}
					test::require(big != nullptr, "no squad template of nominal size 6 or more");
					std::int64_t expected = 0;
					if (const kdata::RefCategory* leaders = big->findRefs("leader"))
						expected = leaders->entries.empty() ? 0 : 1;
					for (const char* list : {"squad", "squad2", "animals", "animals2"})
					{
						if (const kdata::RefCategory* refs = big->findRefs(list))
						{
							for (const kdata::RefEntry& entry : refs->entries)
								expected += (entry.v1 == 0 || entry.v1 == 100) ? entry.v0 : (entry.v0 + entry.v1) / 2;
						}
					}
					test::require(static_cast<std::int64_t>(ksim::nominalSquadSize(*big)) == expected, "the nominal size is 1 + the sum of (min + max) / 2");
					test::require(ksim::nominalSquadSize(*big, 1.0) == ksim::nominalSquadSize(*big) && ksim::nominalSquadSize(*big, 2.0) == ksim::nominalSquadSize(*big),
						"a multiplier of 1 or more does not scale");
					test::require(ksim::nominalSquadSize(*big, 0.5) == std::max<std::size_t>(1, ksim::nominalSquadSize(*big) / 2) && ksim::nominalSquadSize(*big, 0.0001) == 1,
						"a multiplier below 1 scales, floor 1");
					// One awake member of a big squad: the live count says intact (1 of 1), the template's size says it is not (1 < 0.2 x N).
					flecs::entity squad = ecs.entity();
					squad.set<SquadInstance>(SquadInstance{big->stringID, {}, {}, ksim::state::SpawnSource::TownResident});
					downMember(squad, false, ksim::state::Fighter{});
					const ksim::SquadVitals live1 = ksim::squadVitals(ecs, squad);
					const ksim::SquadVitals tmpl = ksim::squadVitals(ecs, squad, live.gameData().get());
					test::require(live1.intact && live1.nominal == 1, "without the data the live count is the size");
					test::require(tmpl.nominal == ksim::nominalSquadSize(*big) && tmpl.members == 1 && !tmpl.intact,
						"with the data a lone survivor of a big squad is not intact");
					ecs.delete_with<ksim::state::MemberOf>(squad.id());
					squad.destruct();
				}
				std::cout << "wilderness: severity threshold, nominal size and float quota\n";
				std::cout << "wilderness: all-KO'd and dead + KO'd squads kept (non-intact), memberless squad retired\n";
			}

			// The AI takes up respawned squads: nobody is goal-less.
			enable(first, table, std::make_shared<FakeNavigator>(1.0e12, 1.0e12));
			first.run(2 * ksim::kTicksPerGameHour);
			checkGoals(first);
			std::cout << "wilderness: AI on the full density world, nobody goal-less\n";
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_ai: " << error.what() << "\n";
		return 1;
	}
	std::cout << "ksim_ai ok\n";
	return 0;
}
