// ksim_ai_coverage: how much of the default world's squad behaviour maps to a
// behaviour we implement, by AI package (include/ksim/ai/behaviours.hpp).
//
//   ksim_ai_coverage [--table TSV] [--seed N] [--top N | --all] [--aggressive]
//
// Spawns the default world (spawnDefaultWorld), resolves every squad's default
// package through the rule table and prints squads and packages as real
// (implemented behaviour), planned (named by a rule, not implemented: runs
// the generic fallback) or unmapped (no rule: generic fallback).

#include "ksim/ai/behaviours.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/loadorder.hpp"

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	std::string table = KSIM_AI_TABLE;
	std::uint64_t seed = 1;
	std::size_t top = 40;
	bool listAggressive = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == "--table" && hasValue)
			table = argv[++i];
		else if (arg == "--seed" && hasValue)
			seed = std::stoull(argv[++i]);
		else if (arg == "--top" && hasValue)
			top = std::stoull(argv[++i]);
		else if (arg == "--aggressive")
			listAggressive = true;
		else if (arg == "--all")
			top = SIZE_MAX;
		else
		{
			std::cerr << "usage: ksim_ai_coverage [--table TSV] [--seed N] [--top N | --all] [--aggressive]\n";
			return 2;
		}
	}
	try
	{
		ksim::SpawnOptions options;
		options.seed = seed;
		const ksim::Simulation simulation = ksim::Simulation::newGame(KENSHI_DATA,
			kworld::resolveLoadOrder(KENSHI_DATA), options);
		const ksim::BehaviourRegistry registry(simulation.world(),
			ksim::BehaviourRegistry::readRules(table));
		ksim::Coverage coverage = registry.coverage(simulation.world().ecs());
		const ksim::routines::PackageTable goals(*simulation.gameData());
		std::size_t modelled = 0;
		std::size_t collapsed = 0;
		std::size_t aggressivePackages = 0;
		std::size_t aggressiveTemplates = 0;
		std::size_t aggressiveSquads = 0;
		std::vector<std::string> aggressiveNames;
		for (ksim::CoverageRow& row : coverage.packages)
		{
			const ksim::routines::CompiledPackage* compiled = goals.find(row.package);
			row.routine = compiled == nullptr ? "none" : ksim::routines::routineStatus(*compiled);
			(row.routine == "modelled" ? modelled : collapsed) += row.squads;
			if (compiled != nullptr && ksim::routines::carriesGoal(*compiled, ksim::routines::task::AttackEnemiesAndNeutrals))
			{
				++aggressivePackages;
				aggressiveTemplates += row.squadTemplates;
				aggressiveSquads += row.squads;
				aggressiveNames.push_back(row.name + " (" + row.package + "): " + std::to_string(row.squads) + " squads, "
					+ std::to_string(row.squadTemplates) + " templates");
			}
		}
		std::cout << "loaded routine (T3a, goals the town scorer will run): squads on modelled packages " << modelled
			<< ", on packages that collapse to the STAY_IN_HOME fallback or have no goals " << collapsed << "\n";
		// Town life when no player is near (T3f): each squad's unloaded phase at this clock, from its default package's leader list,
		// with every finder assumed to succeed; plus the window-valid leader rows the scorer does not run (the "unmodelled goal"
		// counter of 5.7). The same counters the soak reports over time.
		{
			const ksim::GameTime when = ksim::gameTimeAt(simulation.clockTick());
			const double hour = when.hour + when.minute / 60.0;
			std::array<std::size_t, ksim::routines::kTownPhaseCount> byPhase{};
			std::size_t unmodelled = 0;
			std::size_t withPackage = 0;
			simulation.world().ecs().each([&](flecs::entity, const ksim::state::SquadInstance& squad)
			{
				const ksim::Mapping& mapping = registry.squad(squad.templateSid);
				const ksim::routines::CompiledPackage* compiled =
					mapping.package.empty() ? nullptr : goals.find(mapping.package);
				if (compiled == nullptr)
					return;
				std::size_t skipped = 0;
				byPhase[static_cast<std::size_t>(ksim::routines::townPhase(*compiled, hour, &skipped))] += 1;
				unmodelled += skipped;
				++withPackage;
			});
			std::cout << "town life at " << when.hour << ":" << (when.minute < 10 ? "0" : "") << when.minute << " (unloaded, "
				<< withPackage << " squads with a package):";
			for (int i = 0; i < ksim::routines::kTownPhaseCount; ++i)
				std::cout << " " << ksim::routines::townPhaseName(static_cast<ksim::routines::TownPhase>(i)) << "="
					<< byPhase[static_cast<std::size_t>(i)];
			std::cout << "; window-valid leader rows the scorer does not run: " << unmodelled << "\n";
		}
		std::cout << "goal 21 ATTACK_ENEMIES_AND_NEUTRALS (MQ14 path 2): " << aggressivePackages << " packages carry it (squad lists, after inherit); "
			<< aggressiveTemplates << " squad templates and " << aggressiveSquads << " spawned squads run one as their default package\n";
		for (const std::string& name : aggressiveNames)
			std::cout << "  " << name << "\n";
		if (listAggressive)
		{
			// The aggressive squads within 4000 u of The Hub (primary townlist placement), nearest first.
			double hubX = 0.0;
			double hubZ = 0.0;
			simulation.world().ecs().each([&](const ksim::state::TownState& town)
			{
				const kdata::Record* record = simulation.gameData()->find(town.rootSid);
				if (record != nullptr && record->name == "The Hub")
				{
					hubX = town.x;
					hubZ = town.z;
				}
			});
			struct Near
			{
				double distance;
				std::string line;
			};
			std::vector<Near> near;
			simulation.world().ecs().each([&](flecs::entity, const ksim::state::SquadInstance& squad, const ksim::state::Position& at)
			{
				const ksim::routines::CompiledPackage* compiled = goals.find(registry.squad(squad.templateSid).package);
				if (compiled == nullptr || !ksim::routines::carriesGoal(*compiled, ksim::routines::task::AttackEnemiesAndNeutrals))
					return;
				const double distance = std::hypot(at.x - hubX, at.z - hubZ);
				const kdata::Record* record = simulation.gameData()->find(squad.templateSid);
				std::string faction = "(none)";
				if (squad.faction.id != 0)
				{
					if (const ksim::state::FactionState* state = simulation.world().ecs().entity(squad.faction.id).get<ksim::state::FactionState>())
					{
						const kdata::Record* factionRecord = simulation.gameData()->find(state->templateSid);
						faction = factionRecord != nullptr ? factionRecord->name : state->templateSid;
					}
				}
				near.push_back({distance, "  " + std::to_string(static_cast<long>(distance)) + " u  " + (record != nullptr ? record->name : squad.templateSid)
					+ " [" + faction + "] package " + compiled->name});
			});
			std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
			std::cout << "aggressive squads nearest The Hub (" << hubX << ", " << hubZ << "):\n";
			for (std::size_t i = 0; i < near.size() && i < 25; ++i)
				std::cout << near[i].line << "\n";
		}
		ksim::printCoverage(std::cout, coverage, top);
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_ai_coverage: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
