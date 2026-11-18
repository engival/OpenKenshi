// Task k16.6.116 (rwh.99 section 3, the graft): the slavers_downed caravan cuffs the player; while the player holds its 187 row it scores the master's
// package Slave-role goals as its own, and loses them when order 77 removes the row. (The capture scenario is k16.6.96's, ksim_slavers_trading.)
// Task k16.6.96 (playtest kenshi-h2v.6.1, bug h2v.6.6): the slavers_downed caravan, on the server's world (runtime mesh over the shipped navtiles,
// the squad AI), leaves its Trading stay when the player attacks its Boss: the Boss and its squadmates take fight goals against the player
// within seconds of the first hit (143 / 32 as the victim's squad joins, 16 on sight), and when the player goes down the capture chain runs to
// the cuff (CAPTURE_NEW_SLAVES 167: strip 181, restrain 166, cuff + lift 168). "Trading" is only the squad behaviour's fallback label (TradeCircuit,
// charStats) for a member with no individual task. Run on the playtest's two seeds as well as the default one. Exit 77 without the shipped navtiles.
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "ksim/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

namespace
{

// `rowGone`: order 77 deletes the 187 row the tick the player is cuffed (the control run).
// Returns whether the player took the graft goal (false: it was carried the whole time - a downed slave is carried, and a carried one is no free member).
bool play(const std::uint64_t seed, const bool rowGone)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
	SpawnOptions spawn;
	spawn.seed = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
	simlink::ServerContext context;
	context.gameData = sim.gameData();
	sim.setGameData(context.gameData);
	sim.enableWeather(*context.gameData);
	const std::shared_ptr<const CombatData> data = CombatData::build(*context.gameData, KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(data, settings);
	std::vector<kworld::ZoneCell> hub;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
	sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, hub, {}, context.gameData.get(), NativeCacheOptions{}));
	const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
	const std::uint64_t firstId = sim.world().ecs().get<state::Ids>()->next;
	const std::uint64_t kPlayer = firstId + 2;
	const std::uint64_t kBoss = firstId + 3;
	scenario::apply(sim, *context.gameData, *data, context, file, seed);
	ServerAiOptions ai;
	ai.registryPath = KSIM_AI_TABLE;
	enableServerAi(sim, ai);
	std::vector<std::string> lines;
	sim.setTaskLog([&](const std::string& line) { lines.push_back("tick " + std::to_string(sim.clockTick()) + " " + line); });
	Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
	attack.tick = sim.clockTick() + 100;
	sim.enqueue(attack);
	const std::string tag = " (seed " + std::to_string(seed) + ")";
	const std::string me = "member " + std::to_string(kPlayer);
	for (int i = 0; i < 4000 && sim.combat()->find(kPlayer)->slaveState != 1; ++i)
		sim.tick();
	test::require(sim.combat()->find(kPlayer)->slaveState == 1, "the player is cuffed" + tag);
	test::require(sim.combat()->find(kPlayer)->obedient, "the cuffed player holds its 187 row" + tag);
	// The capture's first aid runs, then the slave is up and follows (a downed slave is carried, and a carried one is no free member: seeds where it is
	// still carried after 8000 ticks show nothing here).
	for (int i = 0; i < 8000; ++i)
	{
		const state::Fighter& slave = *sim.combat()->find(kPlayer);
		if (slave.carriedBy == 0 && !slave.unconscious && !slave.down && slave.followTarget != 0)
			break;
		sim.tick();
	}
	state::Fighter* slave = const_cast<state::Fighter*>(sim.combat()->find(kPlayer));
	if (slave->followTarget == 0)
		return false;
	const std::string label = sim.charStats(kPlayer).goal;
	test::require(label != "Aimless" && label != "Obedient Slave" && label != "Carrying slaves", "the label is the committed goal's name: '" + label + "'" + tag);
	if (rowGone)
	{
		// Order 77: no row, no graft - PROTECT_ALLIES is back and the master's goals are not scored for the slave any more.
		const JobsView view = sim.jobsOf(kPlayer);
		test::require(view.rows.size() == 1 && view.rows[0].type == 187, "the row is there to remove" + tag);
		JobOrder remove;
		remove.kind = 77;
		remove.character = kPlayer;
		remove.jobId = view.rows[0].jobId;
		sim.enqueue(Command::jobOrder(remove));
		sim.run(2);
		test::require(!sim.combat()->find(kPlayer)->obedient && sim.combat()->find(kPlayer)->slaveState == 1, "order 77: not obedient, still a slave" + tag);
		// A player not grafting keeps a Routine that is not the graft's: the player's sleep order (interact.cpp) puts it on a bed; the graft's end pass
		// must not strip it (kenshi-k16.6.116 review). Several squad intervals later it is still there and the player still asleep.
		flecs::entity entity;
		sim.world().ecs().each([&](flecs::entity e, const state::Id& key)
		{
			if (key.value == kPlayer)
				entity = e;
		});
		const_cast<state::Fighter*>(sim.combat()->find(kPlayer))->asleep = true;
		state::Routine bed;
		bed.target = {static_cast<std::uint8_t>(routines::SiteKind::Bed), 1, 0};
		entity.set<state::Routine>(bed);
		sim.run(400);
		test::require(entity.has<state::Routine>() && sim.combat()->find(kPlayer)->asleep, "a sleeping player's bed Routine is not the graft's: it stays" + tag);
		const_cast<state::Fighter*>(sim.combat()->find(kPlayer))->asleep = false;
		entity.remove<state::Routine>();
	}
	// The master's order is dropped from the slave (as a hand-over or a stop would); the next second's graft scores 171 again - only while the row stands.
	CombatArena::endFollow(*slave);
	const std::size_t cut = lines.size();
	sim.run(200);
	const bool again = std::any_of(lines.begin() + static_cast<std::ptrdiff_t>(cut), lines.end(), [&](const std::string& line)
	{
		return line.find("goal 171 follow " + me) != std::string::npos;
	});
	if (!rowGone)
	{
		test::require(again && sim.combat()->find(kPlayer)->followTarget != 0, "the cuffed player takes the master's slave goal 171 through the graft" + tag);
		return true;
	}
	test::require(!again && sim.combat()->find(kPlayer)->followTarget == 0, "order 77: no master goal is scored for the slave" + tag);
	return false;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_slave_obedience: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		int grafted = 0;
		for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{706960716}, std::uint64_t{1151889715}})
		{
			grafted += play(seed, false) ? 1 : 0;
			play(seed, true);
		}
		test::require(grafted >= 1, "at least one seed shows the graft");
		std::cout << "ksim_slave_obedience ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_slave_obedience: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
