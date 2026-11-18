// G1 of the native AI runtime, phase E (task BY): the package Blackboard (kinspect dt19). Pure: staged packages and boards, no
// game data and no world. Pins the priority walk, the faction fallback's place, the contract override and its expiry, `change
// AI`, and the `inherits from` row merge (append, dedupe a row the list already holds).

#include "ksim/blackboard.hpp"
#include "ksim/routines.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace ksim;

blackboard::Package package(const char* sid, std::int32_t priority, std::int32_t signalFunc = 0)
{
	blackboard::Package out;
	out.sid = sid;
	out.priority = priority;
	out.signalFunc = signalFunc;
	return out;
}

blackboard::Board board(const std::vector<blackboard::Package>& packages)
{
	blackboard::Board out;
	for (const blackboard::Package& pkg : packages)
		blackboard::registerPackage(out, pkg);
	blackboard::resolveLeads(out);
	return out;
}

std::string applied(const blackboard::Board& b)
{
	return b.current;
}

// ---- the priority walk ----

void testPriorityWalk()
{
	// The highest registered priority wins; the ladder walks max down.
	blackboard::Board b = board({package("low", 0), package("high", 2), package("mid", 1)});
	test::require(b.maxPriority == 2, "max priority is 2");
	blackboard::tick(b, 13.0);
	test::require(applied(b) == "high", "the highest priority's package is applied");
	// The highest package never finishes (the base class), so a later tick keeps it.
	blackboard::tick(b, 14.0);
	test::require(applied(b) == "high", "a base package keeps the platoon");
	// The faction fallback sits at priority 0: alone, it is the whole ladder.
	blackboard::Board fallback = board({package("go home everybody", 0)});
	blackboard::tick(fallback, 13.0);
	test::require(applied(fallback) == "go home everybody", "the fallback at priority 0 is applied");
	// No packages at all: nothing is applied, and no layer is in a terminal state.
	blackboard::Board empty;
	blackboard::tick(empty, 13.0);
	test::require(applied(empty).empty(), "an empty board applies nothing");
	// A lower priority whose current package is finished is entered: the first package there starts.
	blackboard::Board lower = board({package("top", 1), package("bottom", 0)});
	blackboard::tick(lower, 13.0);
	test::require(applied(lower) == "top", "the top package applies first");
}

// ---- the ladder quirks ----

void testLeadsTo()
{
	// A finished package with a registered `Leads to` hands over to its successor (at the same priority).
	blackboard::Board b = board({package("first", 0, blackboard::kContractTimed), package("second", 0)});
	b.table[0][0].leadsTo = "second"; // resolveLeads would keep a registered successor; staged here
	b.current = "first";
	b.contractExpiry = 5.0; // a table package with a timed signal reads the board's one expiry
	blackboard::tick(b, 6.0);
	test::require(applied(b) == "second", "a finished package leads to its registered successor");
	// Without a successor the whole priority level is dropped, not just the finished package.
	blackboard::Board drop = board({package("only", 0, blackboard::kContractTimed), package("other", 0)});
	drop.current = "only";
	drop.contractExpiry = 1.0;
	blackboard::tick(drop, 2.0);
	test::require(applied(drop) == "only", "no `Leads to`: the level falls through, the finished package stays applied");
}

// ---- contracts ----

void testContract()
{
	blackboard::Board b = board({package("template", 3)});
	blackboard::tick(b, 10.0);
	test::require(applied(b) == "template", "the template package first");
	// A contract overrides the table while unfinished, whatever its priority.
	blackboard::pushContract(b, package("contract", 0, blackboard::kContractTimed), 2.0, 10.0);
	test::require(applied(b) == "contract" && std::fabs(b.contractExpiry - 12.0) < 1e-9, "the contract applies with its expiry");
	blackboard::tick(b, 11.0, blackboard::Campaign{});
	test::require(applied(b) == "contract", "an unfinished contract keeps the platoon");
	// At the expiry it ends and the table resumes at the next tick.
	blackboard::tick(b, 12.5);
	test::require(b.contract.sid.empty() && applied(b) == "template", "an expired contract pops and the table resumes");
	// `hours < 1` is the 10 min default.
	blackboard::Board shortContract = board({package("template", 0)});
	blackboard::pushContract(shortContract, package("timed", 0, blackboard::kContractTimed), 0.0, 0.0);
	test::require(std::fabs(shortContract.contractExpiry - blackboard::kShortContractHours) < 1e-12, "0 hours -> 10 minutes");
	// A contract with no `signal func` is Contract_Timed (the contract default).
	blackboard::Board defaulted = board({});
	blackboard::pushContract(defaulted, package("plain", 0), 1.0, 0.0);
	test::require(defaulted.contract.signalFunc == blackboard::kContractTimed, "`signal func` 0 -> Contract_Timed");
	// The base class never finishes: a contract with another signal stays for ever.
	blackboard::Board base = board({});
	blackboard::pushContract(base, package("forever", 0, 0x10), 1.0, 0.0);
	for (double now = 1.0; now < 100.0; now += 10.0)
		blackboard::tick(base, now);
	test::require(!base.contract.sid.empty() && applied(base) == "forever", "a base-class contract never finishes");
	// The same record pushed again does not restart the clock.
	blackboard::Board same = board({});
	blackboard::pushContract(same, package("c", 0, blackboard::kContractTimed), 3.0, 0.0);
	blackboard::pushContract(same, package("c", 0, blackboard::kContractTimed), 9.0, 1.0);
	test::require(std::fabs(same.contractExpiry - 3.0) < 1e-9, "the same contract already running keeps its expiry");
	// The campaign hook pushes its phase package as a 24 h contract ahead of everything.
	blackboard::Board campaign = board({package("template", 4)});
	blackboard::Campaign hook;
	hook.present = true;
	hook.package = package("phase", 0, blackboard::kContractTimed);
	blackboard::tick(campaign, 5.0, hook);
	test::require(applied(campaign) == "phase" && std::fabs(campaign.contractExpiry - 29.0) < 1e-9,
		"the campaign phase is a 24 h contract over the table");
}

// ---- change AI ----

void testChangeAI()
{
	// Wipes the table and installs one package at priority 0.
	blackboard::Board b = board({package("template", 3), package("other", 0)});
	blackboard::tick(b, 10.0);
	blackboard::changeAI(b, package("Escaping slave- run away solo", 7));
	test::require(applied(b) == "Escaping slave- run away solo" && b.table.size() == 1 && b.maxPriority == 0,
		"change AI replaces the whole table with one priority-0 package");
	blackboard::tick(b, 11.0);
	test::require(applied(b) == "Escaping slave- run away solo", "the installed package runs");
	// The original leaves an active contract alone: it keeps overriding the installed package until it expires.
	blackboard::Board withContract = board({package("template", 1)});
	blackboard::pushContract(withContract, package("contract", 0, blackboard::kContractTimed), 5.0, 0.0);
	blackboard::changeAI(withContract, package("installed", 0));
	test::require(withContract.contract.sid == "contract", "change AI does not end the contract");
	blackboard::tick(withContract, 1.0);
	test::require(applied(withContract) == "contract", "the contract keeps overriding the changed AI");
	blackboard::tick(withContract, 6.0);
	test::require(withContract.contract.sid.empty() && applied(withContract) == "installed", "after the contract the installed package runs");
}

// ---- the `inherits from` merge (BX) ----

std::vector<routines::GoalRow> rows(std::initializer_list<std::pair<const char*, std::uint16_t>> list)
{
	std::vector<routines::GoalRow> out;
	for (const auto& [sid, task] : list)
	{
		routines::GoalRow row;
		row.taskSid = sid;
		row.task = task;
		out.push_back(std::move(row));
	}
	return out;
}

void testMergeRows()
{
	// Parents are merged first; a child's row for a task the list already holds is skipped, not substituted; new rows append.
	std::vector<routines::GoalRow> lists = rows({{"taskA", 15}, {"taskB", 58}}); // the parent's rows
	routines::mergeRows(lists, rows({{"taskA", 15}, {"taskC", 177}}));
	test::require(lists.size() == 3, "append: the parent's two rows and the child's new one");
	test::require(lists[0].task == 15 && lists[1].task == 58 && lists[2].task == 177, "parents first, child rows after");
	// The kept row is the first one met (the parent's), window included: the insert dedupes on the task record, not its
	// window.
	std::vector<routines::GoalRow> windows = rows({{"taskA", 15}});
	windows[0].start = 0;
	windows[0].end = 100;
	routines::GoalRow later;
	later.taskSid = "taskA";
	later.task = 15;
	later.start = 20;
	later.end = 8;
	routines::mergeRows(windows, {later});
	test::require(windows.size() == 1 && windows[0].start == 0 && windows[0].end == 100,
		"the parent's row wins over the child's row of the same task");
	// The same task enum from a different record is a different hand: both stay.
	routines::mergeRows(windows, rows({{"taskA-copy", 15}}));
	test::require(windows.size() == 2, "a same-task row from another record is kept (a different hand)");
	// A repeated row inside one list is deduped too.
	std::vector<routines::GoalRow> repeated = rows({{"taskA", 15}});
	routines::mergeRows(repeated, rows({{"taskA", 15}}));
	test::require(repeated.size() == 1, "a row already met in the list is skipped");
}

// ---- the F package classes (dt41_F_packages.md) ----

void testFClasses()
{
	using namespace blackboard;
	// s8: the slaver packages start only on a platoon holding slaves; a plain one skips to the next entry of the bucket.
	Board slaver = board({package("hang", 0, 0x11), package("ship", 1, kSlaverCamps)});
	tick(slaver, 8.0);
	test::require(applied(slaver) == "hang", "s8: a plain platoon cannot start the slaver package");
	Situation mission;
	mission.members = 4;
	mission.owned = 1;
	Board shipping = board({package("hang", 0, 0x11), package("ship", 1, kSlaverCamps)});
	tick(shipping, 8.0, {}, mission);
	test::require(applied(shipping) == "ship", "s8: a platoon holding slaves ships");
	mission.owned = 0;
	tick(shipping, 9.0, {}, mission);
	test::require(applied(shipping) == "hang", "s8: the ownership set emptied finishes the shipping, the bar level takes over");
	// s3: OccupyTown never self-starts, but follows PassingByTownAssault's Leads to once two members are engaged (rwh.53 section 2.4).
	Board raid;
	registerPackage(raid, package("assault", 1, kPassingByTownAssault));
	registerPackage(raid, package("occupy", 1, kOccupyTown));
	registerPackage(raid, package("home", 0));
	raid.table[1][0].leadsTo = "occupy"; // staged, as testLeadsTo does
	Situation town;
	tick(raid, 10.0, {}, town);
	test::require(applied(raid) == "home", "s2: no town to assault and OccupyTown never starts: the bucket yields to priority 0");
	town.assaultTown = true;
	tick(raid, 10.5, {}, town);
	test::require(applied(raid) == "assault", "s2: a town to assault starts it");
	town.engagedMembers = 1;
	tick(raid, 10.6, {}, town);
	test::require(applied(raid) == "assault", "s2: with one member engaged it keeps going");
	town.engagedMembers = 2;
	tick(raid, 10.6 + kAssaultMinHours / 2, {}, town);
	test::require(applied(raid) == "assault", "s2: not before the 0.25 minimum run");
	tick(raid, 10.6 + kAssaultMinHours, {}, town);
	test::require(applied(raid) == "occupy", "s2: two engaged members lead to OccupyTown");
	town.assaultTown = false; // the raid is over: the assault would not start again
	town.occupying = true;
	town.stepHours = kOccupyHours;
	for (int step = 0; step < 4; ++step)
		tick(raid, 11.0 + step, {}, town);
	test::require(applied(raid) == "occupy" && raid.occupyHeld == 0.0, "s3: while a member stands in the town nothing accrues");
	town.occupying = false; // nobody living and standing is in the town: 4 s accumulate (stepHours = 1 s of play here, x4)
	town.stepHours = kOccupyHours / 4.0;
	for (int step = 0; step < 3; ++step)
		tick(raid, 15.0 + step * 0.001, {}, town);
	test::require(applied(raid) == "occupy", "s3: held for 3 of 4 s it is still running");
	tick(raid, 16.0, {}, town);
	tick(raid, 16.1, {}, town);
	test::require(applied(raid) == "home", "s3: held for 4.0 s it finishes and the level yields");
	// s10: GoOutOnPatrol re-finishes at 12.0 and the lone priority-0 package re-applies, stamping again.
	Board patrol = board({package("patrol", 0, kGoOutOnPatrol)});
	tick(patrol, 1.0);
	test::require(applied(patrol) == "patrol" && patrol.startedAt == 1.0, "s10: patrol starts and stamps now");
	tick(patrol, 12.5);
	test::require(patrol.startedAt == 1.0, "s10: not finished at 11.5");
	tick(patrol, 13.5);
	test::require(applied(patrol) == "patrol" && patrol.startedAt == 13.5, "s10/s12: finished past 12.0, re-applied, the same package");
	// s10: RetreatAndRefresh starts only when the platoon is not ready.
	Board retreat = board({package("retreat", 1, kRetreatAndRefresh), package("home", 0)});
	tick(retreat, 1.0);
	test::require(applied(retreat) == "home", "s10: a ready platoon does not retreat");
	Situation hurt;
	hurt.platoonReady = false;
	tick(retreat, 2.0, {}, hurt);
	test::require(applied(retreat) == "retreat", "s10: an unready platoon retreats");
	tick(retreat, 3.0);
	test::require(applied(retreat) == "home", "s10: ready again, it is finished");
	// s9: the trader's state runs 0 -> 1 -> 2 on arrivals and 2 is finished; it restarts only by daylight or a town owner.
	Board trader = board({package("wander", 1, kWanderingTrader), package("bar", 0, 0x11)});
	Situation arrived;
	arrived.atTown = true;
	tick(trader, 10.0, {}, arrived);
	test::require(applied(trader) == "wander" && trader.traderState == 1, "s9: the trader starts and advances to 1");
	tick(trader, 10.1, {}, arrived);
	test::require(trader.traderState == 2, "s9: the next arrival makes 2");
	tick(trader, 22.0, {}, arrived); // night: finished, may not restart
	test::require(applied(trader) == "bar", "s9: state 2 is finished and, at night, it does not restart");
	// s10: WarAssault finishes when the campaign map has no instance of the platoon.
	Board war = board({package("war", 0, kWarAssault)});
	tick(war, 1.0);
	Situation gone;
	gone.warInstance = false;
	test::require(finished(war, *find(war, "war"), 2.0, gone) && !finished(war, *find(war, "war"), 2.0), "s10: WarAssault ends with its campaign instance");
}

} // namespace

int main()
{
	try
	{
		testPriorityWalk();
		testLeadsTo();
		testContract();
		testChangeAI();
		testMergeRows();
		testFClasses();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_blackboard OK\n";
	return 0;
}
