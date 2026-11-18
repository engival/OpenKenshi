// The unloaded town life soak gate (TOWN_LIFE_SLICE 5.3 LOD and 5.7, phase T3f): with no player anywhere, every squad runs the
// squad-level path only, so what the town would be doing comes from the leader's package and the clock — the unloaded `phase`
// (Post / Work / Relax / Patrol / Sleep / Home). This pins:
//  - the 5.7 soak numbers: no squad without a goal, none stuck, none off its route, no reservation left in the book;
//  - the town-life counters: squads by phase over the run (the night half asleep, guards at their posts, people at work, patrons in
//    a bar), the "unmodelled goal" counter, and that every squad with a package carries a phase;
//  - the coverage view of the same numbers: a world spawned and not stepped still reports a phase for every squad with a package.
// argv[1]: the world bake (`knav_bake --world DIR`, the world_sample fixture). Exit 77 without it.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/navigator.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/soak.hpp"
#include "ksim/spawn.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

// The world_sample window (the AI tests').
constexpr int kWindow[4] = {4, 7, 6, 9};

// The squads the AI phases: every one whose template's package compiles (a Full-detail squad has real Routines instead, but the
// soak has no player area, so none is Full).
std::size_t phasedSquads(const Simulation& simulation, const BehaviourRegistry& registry, const routines::PackageTable& packages)
{
	std::size_t count = 0;
	simulation.world().ecs().each([&](flecs::entity, const state::SquadInstance& squad)
	{
		const Mapping& mapping = registry.squad(squad.templateSid);
		count += !mapping.package.empty() && packages.find(mapping.package) != nullptr ? 1 : 0;
	});
	return count;
}

void townLife(const std::string& bake)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation simulation = Simulation::newGame(KENSHI_DATA, order, options, nullptr, regions);
	const std::shared_ptr<const KnavNavigator> navigator =
		KnavNavigator::open(KENSHI_DATA, bake, kWindow[0], kWindow[1], kWindow[2], kWindow[3], 4);
	const std::shared_ptr<const BehaviourRegistry> registry =
		std::make_shared<const BehaviourRegistry>(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	AiSystem::Config config;
	config.registry = registry;
	config.navigator = navigator;
	simulation.enableAi(std::move(config));
	// No player areas: nobody is Full detail, so every town squad runs the unloaded path this gate is about.

	SoakOptions soak;
	soak.days = 2.0;
	const SoakReport report = runSoak(simulation, soak);

	// The 5.7 numbers: the soak stays at 0 goal-less and 0 stuck.
	test::require(report.maxGoalless == 0, "a squad had no goal at some hour");
	test::require(report.stuckNow == 0 && report.stuckEpisodes == 0, "a squad got stuck");
	test::require(report.maxOffRoute == 0, "a travelling squad left its route");
	test::require(simulation.routineBook().empty(), "a reservation leaked without a loaded member");
	test::require(report.arrivals > 0, "no squad arrived anywhere");

	// Every squad with a compiled package carries a phase, and the audit counts each squad exactly once.
	const routines::PackageTable packages(*simulation.gameData());
	const std::size_t phrased = phasedSquads(simulation, *registry, packages);
	test::require(phrased > 0, "no squad carries an unloaded phase");
	const std::size_t counted = report.last.byPhase[0] + report.last.byPhase[1] + report.last.byPhase[2]
		+ report.last.byPhase[3] + report.last.byPhase[4] + report.last.byPhase[5];
	test::require(counted == phrased, "the phase counters (" + std::to_string(counted)
		+ ") do not cover every phased squad (" + std::to_string(phrased) + ")");

	// The counters 5.7 names, over the two days: someone sleeps at night, guards hold posts, people work, and bar patrons relax.
	test::require(report.maxByPhase[static_cast<std::size_t>(routines::TownPhase::Sleep)] > 0,
		"no squad was in its sleep phase at any hour");
	test::require(report.maxByPhase[static_cast<std::size_t>(routines::TownPhase::Post)] > 0,
		"no squad was at a post at any hour");
	test::require(report.maxByPhase[static_cast<std::size_t>(routines::TownPhase::Work)] > 0,
		"no squad was at work at any hour");
	test::require(report.maxByPhase[static_cast<std::size_t>(routines::TownPhase::Relax)] > 0,
		"no squad relaxed at any hour (the bar hangout never showed)");
	// The unmodelled-goal counter sees the rows the scorer does not run.
	test::require(report.maxUnmodelled > 0, "the unmodelled goal counter stayed at 0");

	std::printf("town soak: %zu squads, %zu phased; peaks", report.last.squads, phrased);
	for (int i = 0; i < routines::kTownPhaseCount; ++i)
		std::printf(" %s=%zu", routines::townPhaseName(static_cast<routines::TownPhase>(i)),
			report.maxByPhase[static_cast<std::size_t>(i)]);
	std::printf("; unmodelled goal rows %zu; %zu arrivals, %.1f s\n", report.maxUnmodelled, report.arrivals, report.seconds);
}

void coverageSnapshot()
{
	// The coverage view (ksim_ai_coverage): a freshly spawned world, never stepped, still answers a phase for every squad with a
	// compiled package — the same pure lookup the AI does each hour.
	SpawnOptions options;
	options.seed = 1;
	const Simulation simulation = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options);
	const routines::PackageTable goals(*simulation.gameData());
	const BehaviourRegistry registry(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	const GameTime when = gameTimeAt(simulation.clockTick());
	const double hour = when.hour + when.minute / 60.0;
	std::size_t withPackage = 0;
	std::size_t unmodelled = 0;
	std::array<std::size_t, routines::kTownPhaseCount> byPhase{};
	simulation.world().ecs().each([&](flecs::entity, const state::SquadInstance& squad)
	{
		const Mapping& mapping = registry.squad(squad.templateSid);
		const routines::CompiledPackage* compiled = mapping.package.empty() ? nullptr : goals.find(mapping.package);
		if (compiled == nullptr)
			return;
		std::size_t skipped = 0;
		byPhase[static_cast<std::size_t>(routines::townPhase(*compiled, hour, &skipped))] += 1;
		unmodelled += skipped;
		++withPackage;
	});
	test::require(withPackage > 0, "no spawned squad resolved to a compiled package");
	test::require(withPackage == byPhase[0] + byPhase[1] + byPhase[2] + byPhase[3] + byPhase[4] + byPhase[5],
		"the coverage snapshot does not classify every squad with a package");
	std::printf("coverage at %d:00: %zu squads with a package, %zu window-valid leader rows unmodelled\n", when.hour, withPackage, unmodelled);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2)
		{
			std::fprintf(stderr, "usage: ksim_town_soak BAKE\n");
			return 2;
		}
		const std::string bake = argv[1];
		if (!std::filesystem::exists(bake))
		{
			std::fprintf(stderr, "ksim_town_soak: no bake at %s (knav_bake --world DIR)\n", bake.c_str());
			return 77;
		}
		townLife(bake);
		coverageSnapshot();
		std::cout << "ksim_town_soak: ok\n";
	} catch (const test::Failure& failure) {
		std::fprintf(stderr, "ksim_town_soak: %s\n", failure.what());
		return 1;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "ksim_town_soak: %s\n", error.what());
		return 1;
	}
	return 0;
}
