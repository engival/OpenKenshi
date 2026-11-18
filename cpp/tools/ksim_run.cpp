// ksim_run: starts a new game (default world from the load order: vanilla, or --mods FILE)
// or loads a runtime save, optionally runs ticks, then prints the game date,
// squads per faction and totals.
//
//   ksim_run [--seed N] [--load PATH [--force]] [--ticks N] [--save PATH]
//            [--snapshot PATH] [--mods FILE]
//
// --mods FILE: our mod list (kworld/loadorder.hpp grammar); the install's own data/mods.cfg
// is never read.
// --save writes a runtime save (docs/formats/RUNTIME_SAVE.md), --snapshot the
// squad JSON (ksim/save/snapshot.hpp). --force loads a save made with another
// load order. --seed is a new game's seed; a save keeps its own.

#include "ksim/core/clock.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/save/snapshot.hpp"
#include "ksim/core/state.hpp"

#include "kworld/loadorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct Options
{
	std::uint64_t seed = 1;
	std::uint64_t ticks = 0;
	std::string load;
	std::string save;
	std::string snapshot;
	std::string mods;
	bool force = false;
};

int usage()
{
	std::cerr << "usage: ksim_run [--seed N] [--load PATH [--force]] [--ticks N]"
		" [--save PATH] [--snapshot PATH] [--mods FILE]\n";
	return 2;
}

std::string templateName(const ksim::World& world, std::string_view sid)
{
	const flecs::entity prefab = world.find(sid);
	if (!prefab.is_valid())
		return {};
	const auto* source = prefab.get<ksim::tmpl::Source>();
	return source != nullptr ? source->name : std::string();
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	try
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--force")
				options.force = true;
			else if (arg == "--seed" && hasValue)
				options.seed = std::stoull(argv[++i]);
			else if (arg == "--ticks" && hasValue)
				options.ticks = std::stoull(argv[++i]);
			else if (arg == "--load" && hasValue)
				options.load = argv[++i];
			else if (arg == "--save" && hasValue)
				options.save = argv[++i];
			else if (arg == "--snapshot" && hasValue)
				options.snapshot = argv[++i];
			else if (arg == "--mods" && hasValue)
				options.mods = argv[++i];
			else
				return usage();
		}
	}
	catch (const std::exception&)
	{
		return usage();
	}

	try
	{
		const auto loadStart = std::chrono::steady_clock::now();
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, options.mods);
		for (const std::string& missing : order.missing)
			std::cerr << "ksim_run: --mods lists missing " << missing << "\n";

		ksim::SaveLoadResult loaded;
		ksim::SpawnOptions spawnOptions;
		spawnOptions.seed = options.seed;
		ksim::Simulation simulation = options.load.empty()
			? ksim::Simulation::newGame(KENSHI_DATA, order, spawnOptions)
			: ksim::Simulation::loadGame(KENSHI_DATA, order, options.load,
				options.force, &loaded);
		if (!loaded.loadOrderMatches)
			std::cerr << "ksim_run: warning: save load order differs from the current one\n";
		std::cout << (options.load.empty() ? "new game, seed " + std::to_string(options.seed)
			: "loaded " + options.load) << " ("
			<< std::fixed << std::setprecision(2) << std::chrono::duration<double>(
				std::chrono::steady_clock::now() - loadStart).count() << " s)\n";

		const auto runStart = std::chrono::steady_clock::now();
		simulation.run(options.ticks);
		if (options.ticks != 0)
		{
			std::cout << "ran " << options.ticks << " ticks, avg us "
				<< std::fixed << std::setprecision(3)
				<< std::chrono::duration<double, std::micro>(
					std::chrono::steady_clock::now() - runStart).count()
					/ static_cast<double>(options.ticks) << "\n";
		}

		const flecs::world& ecs = simulation.world().ecs();
		const ksim::GameTime time = ksim::gameTimeAt(simulation.clockTick());
		std::cout << "game time Day " << time.day << " " << std::setfill('0')
			<< std::setw(2) << time.hour << ":" << std::setw(2) << time.minute
			<< std::setfill(' ') << " (tick " << simulation.clockTick() << ")\n";

		std::map<std::string, std::size_t> squadsByFaction;
		std::size_t squads = 0;
		std::size_t positioned = 0;
		ecs.each([&](flecs::entity squad, const ksim::state::SquadInstance& instance)
		{
			std::string faction = "(none)";
			if (instance.faction.id != 0)
			{
				const std::string& sid = flecs::entity(ecs.c_ptr(), instance.faction.id)
					.get<ksim::state::FactionState>()->templateSid;
				faction = templateName(simulation.world(), sid) + " [" + sid + "]";
			}
			++squadsByFaction[faction];
			++squads;
			positioned += squad.get<ksim::state::Position>() != nullptr ? 1 : 0;
		});
		std::vector<std::pair<std::string, std::size_t>> sorted(
			squadsByFaction.begin(), squadsByFaction.end());
		std::stable_sort(sorted.begin(), sorted.end(),
			[](const auto& left, const auto& right)
			{
				return left.second > right.second;
			});
		std::cout << "squads per faction:\n";
		for (const auto& [faction, count] : sorted)
			std::cout << std::setw(6) << count << "  " << faction << "\n";

		std::cout << "factions " << ecs.count<ksim::state::FactionState>()
			<< ", towns " << ecs.count<ksim::state::TownState>()
			<< ", squads " << squads << " (" << positioned << " positioned)"
			<< ", characters " << ecs.count<ksim::state::CharacterInstance>() << "\n";
		std::cout << "state hash " << std::hex << simulation.stateHash() << std::dec << "\n";

		if (!options.save.empty())
		{
			simulation.saveGame(options.save);
			std::cout << "saved " << options.save << "\n";
		}
		if (!options.snapshot.empty())
		{
			std::ofstream out(options.snapshot);
			ksim::writeSquadSnapshotJson(out, ecs, [&](std::string_view sid)
			{
				return templateName(simulation.world(), sid);
			});
			if (!out)
				throw std::runtime_error("cannot write " + options.snapshot);
			std::cout << "snapshot " << options.snapshot << "\n";
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_run: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
