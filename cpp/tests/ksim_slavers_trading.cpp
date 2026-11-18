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
#include "caravan_unhurt.hpp"
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

void play(const std::uint64_t seed)
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
	// The server's default: the runtime mesh over the shipped navtiles (ksim_server without a nav argument).
	sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, hub, {}, context.gameData.get(), NativeCacheOptions{}));
	const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
	// The scenario's ids follow the world's (the client's script named 41968 / 41969 / 41970): the squad, the player, the boss.
	const std::uint64_t firstId = sim.world().ecs().get<state::Ids>()->next;
	const std::uint64_t kPlayer = firstId + 2;
	const std::uint64_t kBoss = firstId + 3;
	scenario::apply(sim, *context.gameData, *data, context, file, seed);
	ServerAiOptions ai;
	ai.registryPath = KSIM_AI_TABLE;
	enableServerAi(sim, ai);
	std::vector<std::string> lines;
	sim.setTaskLog([&](const std::string& line) { lines.push_back("tick " + std::to_string(sim.clockTick()) + " " + line); });
	// The client's script: the player attacks the boss; the caravan downs and cuffs the player.
	Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
	attack.tick = sim.clockTick() + 100;
	sim.enqueue(attack);
	// From the player's first hit (the order is at tick 100) to the first time it is down: the first tick the Boss targets it, and the most
	// members hunting it at once.
	int bossAt = -1;
	int hunters = 0;
	bool down = false;
	for (int i = 0; i < 1500; ++i)
	{
		sim.tick();
		for (std::uint64_t id = kBoss; id < kBoss + 8; ++id)
			keepCaravanUnhurt(sim, id);
		down = down || sim.combat()->find(kPlayer)->unconscious;
		if (i < 100 || down)
			continue;
		int hunting = 0;
		for (std::uint64_t id = kBoss; id < kBoss + 8; ++id)
			hunting += sim.combat()->find(id)->target == kPlayer ? 1 : 0;
		hunters = std::max(hunters, hunting);
		if (bossAt < 0 && sim.combat()->find(kBoss)->target == kPlayer)
		{
			bossAt = i;
			test::require(sim.charStats(kBoss).goal != "Trading", "the Boss has left Trading when it targets the player");
		}
	}
	const std::string tag = " (seed " + std::to_string(seed) + ")";
	// k16.6.254: the caravan walks to the Hub bar, the Boss is already ~55 u off at the first hit and the player must close in (190 ticks seen; 60 standing).
	test::require(bossAt >= 0 && bossAt <= 260, "the Boss targets the player within 160 ticks of the first hit" + tag);
	test::require(hunters >= 4, "at least half of the caravan hunts the player at once" + tag);
	test::require(down, "the player is down");
	const auto count = [&](const std::string& needle)
	{
		return std::count_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(needle) != std::string::npos; });
	};
	test::require(count("fight goal 15 member") >= 1 && count("goal CAPTURE_NEW_SLAVES member") >= 1, "a fight goal, then the capture goal, on the downed player");
	test::require(count("168 cuff + lift") == 1, "the capture chain runs to the cuff once (seed " + std::to_string(seed) + ")");
	// kenshi-k16.6.111: the cuff wipes the player's jobs and adds the one row 187 Obedient Slave (a player cannot add it); its goal is the slave task it runs.
	const state::Fighter& slave = *sim.combat()->find(kPlayer);
	test::require(slave.slaveState == 1, "the cuffed player is a slave" + tag);
	const JobsView view = sim.jobsOf(kPlayer);
	test::require(view.enabled && view.rows.size() == 1 && view.rows[0].type == 187 && view.rows[0].target == sim.slaveCaptor(kPlayer) && view.rows[0].target != 0, "the slave's JOBS is exactly one enabled row 187 aimed at its captor" + tag);
	test::require(sim.checkJobOrder({76, kPlayer, 187, 0, 0, 0, 0}) == "unsupported", "a player's 76 for 187 is unsupported" + tag);
	test::require(sim.checkJobOrder({77, kPlayer, 0, 0, view.rows[0].jobId, 0, 0}).empty(), "the player may remove the 187 row" + tag);
	const std::string goal = sim.charStats(kPlayer).goal;
	test::require(slave.carriedBy != 0 || slave.followTarget != 0, "the slave is carried or follows at the end" + tag);
	// kenshi-k16.6.116 (rwh.99): the label is the committed goal's name (171 while it follows), never the row's "Obedient Slave" nor the carrier's
	// "Carrying slaves"; carried it is Aimless (kenshi-k16.6.204, the user on kenshi-8x6). The row gates PROTECT_ALLIES and the slave shield (Fighter::obedient).
	if (slave.carriedBy != 0)
		test::require(goal.empty(), "a carried slave's goal label is Aimless: '" + goal + "'" + tag);
	else
		test::require(!goal.empty() && goal != "Aimless" && goal != "Obedient Slave" && goal != "Carrying slaves", "the slave's goal label is its follow goal: '" + goal + "'" + tag);
	test::require(slave.obedient, "the slave with its 187 row is obedient" + tag);
	// Order 77 deletes the row and nothing else: the slave is no longer obedient (PROTECT_ALLIES is back) and keeps its slave state.
	JobOrder remove;
	remove.kind = 77;
	remove.character = kPlayer;
	remove.jobId = view.rows[0].jobId;
	sim.enqueue(Command::jobOrder(remove));
	sim.run(2);
	test::require(sim.jobsOf(kPlayer).rows.empty() && !sim.combat()->find(kPlayer)->obedient && sim.combat()->find(kPlayer)->slaveState == 1,
		"order 77 removes the 187 row: not obedient, still a slave" + tag);
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_slavers_trading: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		for (const std::uint64_t seed : {std::uint64_t{1}, std::uint64_t{706960716}, std::uint64_t{1151889715}})
			play(seed);
		std::cout << "ksim_slavers_trading ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_slavers_trading: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
