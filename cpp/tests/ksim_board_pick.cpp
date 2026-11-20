// The registry's squad pick is the G1 board's (task ds/board-pick; kinspect dt19; RUNTIME_SYSTEMS G1): for every SQUAD_TEMPLATE the
// package BehaviourRegistry::squad names is the one a Blackboard built from the template's `AI packages` (and the faction's `AI
// fallback`, registered after them at priority 0) applies, the same functions Simulation::ensureBlackboard runs. The trade-circuit
// and bar-hangout overrides are gone: the rebirth Caravan (wandering at 1 over a bar at 0) is a trade circuit by priority alone.
// argv: none (KENSHI_DATA, KSIM_AI_TABLE).

#include "ksim/ai/behaviours.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kHours = 1.0;

// The applied package of a board built the way ensureBlackboard builds it.
std::string boardPick(const kdata::GameData& data, const kdata::Record& squadTemplate, const std::string& fallback)
{
	blackboard::Board board;
	if (const kdata::RefCategory* links = squadTemplate.findRefs("AI packages"))
		for (const kdata::RefEntry& entry : links->entries)
		{
			blackboard::Package package;
			if (blackboard::readPackage(data, entry.targetID, entry.v0, package))
				blackboard::registerPackage(board, std::move(package));
		}
	blackboard::Package package;
	if (!fallback.empty() && blackboard::readPackage(data, fallback, 0, package))
		blackboard::registerPackage(board, std::move(package));
	blackboard::resolveLeads(board);
	blackboard::tick(board, kHours);
	return board.current;
}

} // namespace

int main()
{
	try
	{
		SpawnOptions options;
		const Simulation simulation = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA), options);
		const kdata::GameData& data = *simulation.gameData();
		const BehaviourRegistry registry(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		std::set<std::string> fallbacks{""};
		for (const kdata::Record* faction : data.byType(kworld::itemTypeByName("FACTION")))
			if (const kdata::RefCategory* refs = faction->findRefs("AI fallback"); refs != nullptr && !refs->entries.empty())
				fallbacks.insert(refs->entries.front().targetID);
		std::size_t templates = 0;
		std::size_t lowest = 0; // the templates whose pick is not the old lowest-priority package
		for (const kdata::Record* record : data.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		{
			++templates;
			for (const std::string& fallback : fallbacks)
			{
				const std::string expected = boardPick(data, *record, fallback);
				const std::string got = registry.squad(record->stringID, fallback).package;
				test::require(got == expected, "template " + record->stringID + " (" + record->name + ") fallback '" + fallback
					+ "': the registry names '" + got + "', the board applies '" + expected + "'");
			}
			const kdata::RefCategory* links = record->findRefs("AI packages");
			if (links == nullptr || links->entries.empty())
				continue;
			const kdata::RefEntry* low = &links->entries.front();
			for (const kdata::RefEntry& entry : links->entries)
				if (entry.v0 < low->v0)
					low = &entry;
			if (registry.squad(record->stringID).package != low->targetID)
				++lowest;
		}
		if (const char* dump = std::getenv("BOARD_PICK_DUMP"))
			if (const kdata::Record* record = data.find(dump); record != nullptr)
				if (const kdata::RefCategory* links = record->findRefs("AI packages"))
				{
					for (const kdata::RefEntry& entry : links->entries)
					{
						const kdata::Record* package = data.find(entry.targetID);
						std::cout << "  link " << entry.targetID << " " << (package != nullptr ? package->name : "?") << " priority " << entry.v0 << " -> "
							<< behaviourName(registry.package(entry.targetID).behaviour) << "\n";
					}
					std::cout << "  pick " << registry.squad(dump).package << " " << behaviourName(registry.squad(dump).behaviour) << "\n";
				}
		test::require(templates > 500, "too few SQUAD_TEMPLATE records");
		const kdata::Record* caravan = data.find("1161-gamedata.base");
		test::require(caravan != nullptr, "no Caravan template");
		test::require(registry.squad(caravan->stringID).behaviour == Behaviour::TradeCircuit, "the Caravan is not a trade circuit");
		std::cout << "ksim_board_pick: ok (" << templates << " templates x " << fallbacks.size() << " fallbacks, " << lowest
			<< " differ from the old lowest-priority pick)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_board_pick: FAILED: " << e.what() << "\n";
		return 1;
	}
}
