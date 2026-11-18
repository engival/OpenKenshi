// kenshi-h2v.16.3: scenarios/progress_bars.kscen starts every kind of task a bar can show, all at once at the Heft slave shop; within kSeconds game seconds
// each intended kind of Simulation::taskProgress(true) appears for the character meant to work it (the flags too), and with one task dropped from the file
// that kind does not (the check that fails before the scenario had it). The caged NPC's own lock pick is reported, not required (it is the AI's choice).
// Needs the world navmesh bake (argv[1], exit 77 without the Heft region).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

using namespace ksim;

namespace {

constexpr int kSeconds = 90;

struct Seen
{
	std::map<std::uint8_t, std::set<std::uint64_t>> by; // kind -> the characters that had a row of it
	std::map<std::uint8_t, std::uint8_t> flags; // kind -> OR of the row flags
};

Seen run(Simulation& sim, const kdata::GameData& gd, const CombatData& data, simlink::ServerContext& context, const scenario::File& file, std::map<int, std::uint64_t>& ids)
{
	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, gd, data, context, file, 5);
	ids = placed.idOf;
	Seen seen;
	for (int tick = 0; tick < kSeconds * 20; ++tick)
	{
		sim.tick();
		for (const Simulation::TaskProgressRow& row : sim.taskProgress(true))
		{
			seen.by[row.kind].insert(row.id);
			seen.flags[row.kind] |= row.flags;
		}
	}
	return seen;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/progress_bars.kscen");
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), zone.x, zone.z))
		{
			std::cout << "ksim_progress_scen: SKIPPED: needs the world navmesh bake with the scenario's region (pass its directory)\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		// A fresh simulation per run: lock orders and the rest of the first run's state must not carry into a control run.
		std::shared_ptr<const kdata::GameData> gameData;
		std::shared_ptr<const CombatData> combatData;
		const auto fresh = [&]()
		{
			SpawnOptions spawn;
			spawn.seed = 7;
			Simulation made = Simulation::newGame(KENSHI_DATA, order, spawn);
			made.markPristine();
			gameData = made.gameData();
			combatData = CombatData::build(*gameData, KENSHI_DATA);
			CombatSettings settings;
			settings.substeps = 2;
			made.enableCombat(combatData, settings);
			made.enableNavigation(test::openWorldNav(world, zones));
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(made.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
			ai.factions = FactionBook::build(made.gameData());
			made.enableAi(std::move(ai));
			return made;
		};
		simlink::ServerContext context;
		context.squadLimit = 50;

		// kind, the file's char that works it, the flag bits the row must carry (1 = the original shows it, 2 = a player row)
		struct Want { std::uint8_t kind; int who; std::uint8_t flags; const char* what; };
		const Want wants[] = {
			{1, 2, 3, "lockpick (the Lockpicker in the cage)"},
			{7, 3, 3, "cut (the Sawyer on the Shackled one's shackles)"},
			{10, 5, 2, "produce (the Miner at the Ore Mine)"},
			{11, 6, 2, "build (the Builder at the site)"},
		};
		std::map<int, std::uint64_t> ids;
		Simulation first = fresh();
		context.gameData = gameData;
		scenario::check(*gameData, file);
		const Seen full = run(first, *gameData, *combatData, context, file, ids);
		std::cout << "kinds seen (kind: characters, file ids):";
		for (const auto& [kind, who] : full.by)
		{
			std::cout << " " << int(kind) << ":";
			for (const auto& [fileId, id] : ids)
				std::cout << (who.count(id) != 0 ? std::to_string(fileId) : "");
		}
		std::cout << "\n";
		for (const Want& w : wants)
		{
			const auto it = full.by.find(w.kind);
			test::require(it != full.by.end() && it->second.count(ids.at(w.who)) != 0, std::string("no TASK_PROGRESS kind ") + std::to_string(w.kind) + " for " + w.what + " within " + std::to_string(kSeconds) + " s");
			test::require((full.flags.at(w.kind) & w.flags) == w.flags, "kind " + std::to_string(w.kind) + " lacks flag bits " + std::to_string(w.flags));
		}

		const bool npcLock = full.by.count(1) != 0 && full.by.at(1).count(ids.at(7)) != 0;
		// The control: each task taken out of the file in turn, and its kind is gone.
		for (const Want& w : wants)
		{
			scenario::File without = file;
			for (scenario::Char& c : without.chars)
				if (c.id == w.who)
					c.task.clear();
			std::map<int, std::uint64_t> otherIds;
			Simulation other = fresh();
			const Seen dropped = run(other, *gameData, *combatData, context, without, otherIds);
			const auto it = dropped.by.find(w.kind);
			test::require(it == dropped.by.end() || it->second.count(otherIds.at(w.who)) == 0, std::string("kind ") + std::to_string(w.kind) + " still shows for " + w.what + " with its task dropped");
		}
		std::cout << "the caged NPC picked its own lock: " << (npcLock ? "yes" : "no") << "\n";
		std::cout << "ksim_progress_scen ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
