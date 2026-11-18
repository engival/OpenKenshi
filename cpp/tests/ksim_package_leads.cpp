// Task leads-audit (kenshi-k16.6.66; kinspect rwh.53 sections 2.4, 3; docs/reference/FACTIONS.md 2.1b). Pins, from the merged data:
//  - `Leads to` is set on exactly two AI_PACKAGE records: `passing town assault then occupy` -> `hostile occupy town` (the live chain) and
//    `Slaver grab all the local prisoners` -> `Slaver OLD` (no template references it);
//  - a NON-raid squad template that carries both packages (its board built as Simulation::createBlackboard does: the template's `AI packages`
//    in data order) keeps the lead through resolveLeads, and the lead fires once the assault finishes (two members engaged);
//  - a squad that carries the assault package without the occupy package loses the lead at resolveLeads (it never occupies);
//  - the board restore drops a lead whose successor is not on the restored board;
//  - D: 140290450's town rule (blackboard::assaultsTown): a player-owned town is assaulted whatever the relation, unless allied.

#include "ksim/blackboard.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

blackboard::Board boardOf(const kdata::GameData& gd, const kdata::Record& squad)
{
	blackboard::Board out;
	if (const kdata::RefCategory* links = squad.findRefs("AI packages"))
		for (const kdata::RefEntry& entry : links->entries)
		{
			blackboard::Package pkg;
			if (blackboard::readPackage(gd, entry.targetID, entry.v0, pkg))
				blackboard::registerPackage(out, std::move(pkg));
		}
	blackboard::resolveLeads(out);
	return out;
}

bool carries(const blackboard::Board& board, std::int32_t signalFunc)
{
	for (const auto& [priority, bucket] : board.table)
		for (const blackboard::Package& pkg : bucket)
			if (pkg.signalFunc == signalFunc)
				return true;
	return false;
}

const blackboard::Package* leading(const blackboard::Board& board, std::int32_t signalFunc)
{
	for (const auto& [priority, bucket] : board.table)
		for (const blackboard::Package& pkg : bucket)
			if (pkg.signalFunc == signalFunc && !pkg.leadsTo.empty())
				return &pkg;
	return nullptr;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA));

		// ---- every package with a `Leads to` ----
		std::vector<std::string> leads;
		for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("AI_PACKAGE")))
		{
			blackboard::Package pkg;
			if (blackboard::readPackage(gd, record->stringID, 0, pkg) && !pkg.leadsTo.empty())
				leads.push_back(record->name + " -> " + gd.find(pkg.leadsTo)->name);
		}
		for (const std::string& lead : leads)
			std::cout << "Leads to: " << lead << "\n";
		test::require(leads.size() == 2, "two AI_PACKAGE records carry a `Leads to`");
		test::require(leads[0].find("passing town assault then occupy -> hostile occupy town") != std::string::npos
			|| leads[1].find("passing town assault then occupy -> hostile occupy town") != std::string::npos, "the assault -> occupy chain is in the data");

		// ---- the squad templates that carry the assault package ----
		int withBoth = 0;
		const kdata::Record* firer = nullptr;
		for (const kdata::Record* squad : gd.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		{
			const blackboard::Board board = boardOf(gd, *squad);
			if (!carries(board, blackboard::kPassingByTownAssault))
				continue;
			if (carries(board, blackboard::kOccupyTown))
			{
				++withBoth;
				test::require(leading(board, blackboard::kPassingByTownAssault) != nullptr, "a squad with both packages keeps the lead: " + squad->name);
				if (firer == nullptr)
					firer = squad;
				std::cout << "assault + occupy: " << squad->name << "\n";
			} else {
				test::require(leading(board, blackboard::kPassingByTownAssault) == nullptr, "a squad without the occupy package drops the lead: " + squad->name);
				std::cout << "assault only: " << squad->name << "\n";
			}
		}
		test::require(withBoth > 0 && firer != nullptr, "some squad template carries the assault -> occupy stack");

		// ---- the lead fires for that squad's real board ----
		{
			blackboard::Board board = boardOf(gd, *firer);
			blackboard::Situation town;
			town.assaultTown = true;
			blackboard::tick(board, 12.0, {}, town);
			const blackboard::Package* running = blackboard::find(board, board.current);
			test::require(running != nullptr && running->signalFunc == blackboard::kPassingByTownAssault, "the assault is applied in a town it may assault");
			town.engagedMembers = blackboard::kAssaultEngaged;
			blackboard::tick(board, 12.5, {}, town); // the engaged count is read by the package's own tick, the walk after sees it finished
			blackboard::tick(board, 13.0, {}, town);
			running = blackboard::find(board, board.current);
			test::require(running != nullptr && running->signalFunc == blackboard::kOccupyTown, "the finished assault leads on to the occupy package");
		}

		// ---- the restore: a lead whose successor is missing from the restored board is dropped ----
		{
			blackboard::Board board;
			state::BoardStates::Entry saved;
			saved.replacement = "passing town assault then occupy";
			for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("AI_PACKAGE")))
				if (record->name == saved.replacement)
					saved.replacement = record->stringID;
			blackboard::restore(board, saved, gd);
			const blackboard::Package* pkg = blackboard::find(board, saved.replacement);
			test::require(pkg != nullptr && pkg->leadsTo.empty(), "restore resolves the replacement's lead against the restored board");
		}

		// ---- D: 140290450's town rule ----
		test::require(blackboard::assaultsTown(false, false, true, false), "a player-owned town is assaulted whatever the relation reads");
		test::require(!blackboard::assaultsTown(true, true, true, true), "an allied town is never assaulted");
		test::require(!blackboard::assaultsTown(false, false, false, false), "a neutral owner's town is not assaulted");
		test::require(blackboard::assaultsTown(false, false, false, true), "an enemy owner's town is assaulted");
		test::require(blackboard::assaultsTown(false, true, false, false), "a squad that attacks neutrals assaults any town");
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_package_leads OK\n";
	return 0;
}
