// The G1 package Board in the save (task ds/board-save, save v92; kinspect dt19 section 0 save keys replacementAI / runningAwayMode):
// the table is derived from the data and rebuilt on load, but a dialogue's runtime state is not: the active contract and its expiry,
// the `change AI` replacement table (one priority-0 package), the applied package and priority, the reapply flag and running away.
// The squad is dt27_rig's hand-made world squad; the contract and `change AI` are applied the way dialogue_sim does, the game is
// saved and loaded, and the loaded board must equal the straight run's - straight after the load and after the selector's first passes.

#include "dt27_rig.hpp"

#include "ksim/save/save.hpp"

#include <cstdio>
#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

constexpr const char* kContractSid = "59293-rebirth.mod"; // the passing-town-assault package (ksim_raid_goals)

flecs::entity squadById(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& key, const state::SquadInstance&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

// Every way the two boards differ, one line each ("" = equal).
std::string diff(const blackboard::Board* want, const blackboard::Board* got)
{
	if (want == nullptr || got == nullptr)
		return got == want ? "" : (got == nullptr ? "board absent after load; " : "board unexpected; ");
	std::string out;
	const auto check = [&](bool same, const char* what) { if (!same) out += std::string(what) + "; "; };
	check(want->contract.sid == got->contract.sid, "contract");
	check(want->contract.signalFunc == got->contract.signalFunc, "contract signal");
	check(want->contractExpiry == got->contractExpiry, "contract expiry");
	check(want->current == got->current, "applied package");
	check(want->currentPriority == got->currentPriority, "applied priority");
	check(want->reapply == got->reapply, "reapply flag");
	check(want->runningAway == got->runningAway, "running away");
	check(want->startedAt == got->startedAt, "package start time");
	check(want->traderState == got->traderState, "trader state");
	check(want->occupyHeld == got->occupyHeld, "occupy held");
	check(want->maxPriority == got->maxPriority, "max priority");
	check(want->table.size() == got->table.size(), "table priorities");
	for (const auto& [priority, packages] : want->table)
	{
		const auto it = got->table.find(priority);
		check(it != got->table.end() && it->second.size() == packages.size(), "table level size");
		for (std::size_t i = 0; it != got->table.end() && i < packages.size() && i < it->second.size(); ++i)
			check(it->second[i].sid == packages[i].sid, "table package");
	}
	return out;
}

void board()
{
	Rig rig({}, 0);
	blackboard::Board* live = rig.sim.blackboardOf(rig.squad);
	test::require(live != nullptr, "the rig squad has no board");
	const double now = trader::gameHours(rig.sim.clockTick());
	blackboard::Package replacement;
	test::require(blackboard::readPackage(*rig.gd, "14544-gamedata.base", 0, replacement), "no AI package 14544");
	blackboard::changeAI(*live, std::move(replacement)); // dialogue `change AI`: the table is wiped, one priority-0 package
	blackboard::Package contract;
	test::require(blackboard::readPackage(*rig.gd, kContractSid, 0, contract), "no AI package 59293");
	blackboard::pushContract(*live, std::move(contract), 36.0, now); // an `AI contract`; change AI left no mark on it
	live->runningAway = true;
	rig.tick(40); // the ladder runs with the contract on top
	const std::uint64_t squadId = rig.squad.get<state::Id>()->value;
	test::require(live->contract.sid == kContractSid && live->table.size() == 1 && live->maxPriority == 0, "the straight run's board is not staged");

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_board_save.save";
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	std::remove(path.c_str());
	const flecs::entity loadedSquad = squadById(loaded, squadId);
	test::require(loadedSquad.is_valid(), "the squad is not in the loaded world");
	const std::string atLoad = diff(live, loaded.blackboardOf(loadedSquad));
	std::cout << "  at load: " << (atLoad.empty() ? "equal" : "LOST " + atLoad) << "\n";

	// The selector's first passes rebuild a missing board from the data (ensureBlackboard); the straight run is ticked the same.
	loaded.setGameData(rig.gd);
	CombatSettings settings;
	settings.substeps = 2;
	loaded.enableCombat(rig.data, settings);
	rig.tick(80);
	for (int i = 0; i < 80; ++i)
		loaded.tick();
	const std::string later = diff(rig.sim.blackboardOf(rig.squad), loaded.blackboardOf(squadById(loaded, squadId)));
	std::cout << "  after 80 ticks: " << (later.empty() ? "equal" : "LOST " + later) << "\n";
	test::require(atLoad.empty(), "the loaded board differs at load: " + atLoad);
	// The in-place load (Simulation::reload, the server's load): the same board comes back.
	{
		const blackboard::Board want = *live;
		std::stringstream stream;
		rig.sim.save(stream);
		rig.sim.reload(stream);
		const std::string reloaded = diff(&want, rig.sim.blackboardOf(squadById(rig.sim, squadId)));
		std::cout << "  after reload: " << (reloaded.empty() ? "equal" : "LOST " + reloaded) << "\n";
		test::require(reloaded.empty(), "the reloaded board differs: " + reloaded);
	}

	test::require(later.empty(), "the loaded board differs after 80 ticks: " + later);
}

// The first AI_PACKAGE record with this `signal func` (the F classes have no named fixture).
std::string packageWithSignal(const kdata::GameData& data, std::int32_t signal)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("AI_PACKAGE")))
		if (record->intOr("signal func", 0) == signal)
			return record->stringID;
	return {};
}

// Save v94: the F classes' per-package state crosses a save. A patrol 0.01 h from its 12.0 finish must finish and re-stamp at the same tick in the loaded
// world; a trader at state 1 must reach 2 on an arrival in both.
void fState(std::int32_t signal, const char* what)
{
	Rig rig({}, 0);
	blackboard::Board* live = rig.sim.blackboardOf(rig.squad);
	test::require(live != nullptr, "the rig squad has no board");
	const std::string sid = packageWithSignal(*rig.gd, signal);
	test::require(!sid.empty(), std::string("no AI package with signal ") + what);
	blackboard::Package replacement;
	test::require(blackboard::readPackage(*rig.gd, sid, 0, replacement), "no AI package " + sid);
	live->contract = blackboard::Package{}; // the rig's 24 h test contract would override the table
	live->contractExpiry = 0.0;
	blackboard::changeAI(*live, std::move(replacement));
	rig.tick(20);
	const double now = trader::gameHours(rig.sim.clockTick());
	live->startedAt = now - (signal == blackboard::kWanderingTrader ? 1.0 : blackboard::kPatrolHours - 0.01);
	live->traderState = signal == blackboard::kWanderingTrader ? 1 : 0;
	const std::uint64_t squadId = rig.squad.get<state::Id>()->value;

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_board_save_f.save";
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	std::remove(path.c_str());
	loaded.setGameData(rig.gd);
	CombatSettings settings;
	settings.substeps = 2;
	loaded.enableCombat(rig.data, settings);
	const std::string atLoad = diff(live, loaded.blackboardOf(squadById(loaded, squadId)));
	std::cout << "  " << what << " at load: " << (atLoad.empty() ? "equal" : "LOST " + atLoad) << "\n";
	test::require(atLoad.empty(), std::string(what) + ": the loaded board differs at load: " + atLoad);
	// Both boards are driven by hand at the same hour (the loaded world's selector does not tick this hand-made squad): 0.02 h past the save
	// crosses the patrol's 12.0 finish; the trader gets an arrival.
	blackboard::Situation arrived;
	arrived.atTown = signal == blackboard::kWanderingTrader;
	for (blackboard::Board* board : {rig.sim.blackboardOf(rig.squad), loaded.blackboardOf(squadById(loaded, squadId))})
		blackboard::tick(*board, now + 0.02, {}, arrived);
	if (signal == blackboard::kWanderingTrader)
		test::require(rig.sim.blackboardOf(rig.squad)->traderState == 2, "the straight run's trader did not reach state 2");
	else
		test::require(rig.sim.blackboardOf(rig.squad)->startedAt == now + 0.02, "the patrol did not finish and re-stamp in the straight run");
	const std::string later = diff(rig.sim.blackboardOf(rig.squad), loaded.blackboardOf(squadById(loaded, squadId)));
	std::cout << "  " << what << " after the finish: " << (later.empty() ? "equal" : "LOST " + later) << "\n";
	test::require(later.empty(), std::string(what) + ": the loaded board differs after the finish: " + later);
}

} // namespace

int main()
{
	try
	{
		board();
		fState(blackboard::kGoOutOnPatrol, "patrol");
		fState(blackboard::kWanderingTrader, "trader");
		std::cout << "ksim_board_save: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_board_save: FAILED: " << e.what() << "\n";
		return 1;
	}
}
