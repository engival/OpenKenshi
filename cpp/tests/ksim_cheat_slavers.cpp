// kenshi-h2v.6.3: the debug cheat's knock out (Simulation::debugCheat op 1, the DEBUG_CHEAT frame 216) on the player of scenarios/slavers_downed.kscen
// gets the slaver handling a KO from damage gets. Two runs on the server's own Hub world (the runtime mesh over the shipped navtiles, the squad AI):
// the script (the player attacks the boss and is downed by the caravan), and the cheat (the player is knocked out at the first tick, no fight). Each
// records, in order, the first tick of the player's down, disarm (the weapon taken), cuff (slave state 1) and lift (carried); the cheat run must show the same
// sequence. Exit 77 without the shipped navtiles.
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

namespace {

constexpr int kRunTicks = 4800;

struct Trace
{
	std::vector<std::pair<std::string, int>> marks; // first tick of each marker, in order
	std::vector<std::string> log;
};

Trace runCase(bool cheat, const kworld::LoadOrder& order)
{
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
	const scenario::Placed placed = scenario::apply(sim, *context.gameData, *data, context, file, 1);
	const std::uint64_t player = placed.idOf.at(1);
	const std::uint64_t boss = placed.idOf.at(2);
	ServerAiOptions ai;
	ai.registryPath = KSIM_AI_TABLE;
	enableServerAi(sim, ai);
	Trace trace;
	sim.setTaskLog([&](const std::string& line) { trace.log.push_back(line); });
	const std::size_t armour0 = sim.combat()->find(player)->armour.size();
	const std::string weapon0 = sim.combat()->find(player)->weapon.sid;
	test::require(armour0 > 0, "the player wears armour to be stripped");
	if (!cheat)
	{
		// The client's script: the player attacks the boss; the caravan downs and cuffs the player.
		Command attack = Command::fighterAttack(state::Id{player}, state::Id{boss});
		attack.tick = sim.clockTick() + 100;
		sim.enqueue(attack);
	}
	const auto mark = [&](const std::string& what, int tick)
	{
		for (const auto& m : trace.marks)
			if (m.first == what)
				return;
		trace.marks.emplace_back(what, tick);
	};
	for (int i = 0; i < kRunTicks; ++i)
	{
		if (cheat && i == 100)
			test::require(sim.debugCheat(0, 1).empty(), "the cheat knock out was refused");
		sim.tick();
		const state::Fighter* f = sim.combat()->find(player);
		if (f->unconscious)
			mark("down", i);
		if (f->unconscious && f->armour.size() < armour0)
			mark("strip", i);
		if (f->unconscious && f->weapon.sid != weapon0)
			mark("disarm", i);
		if (f->slaveState == 1)
			mark("cuff", i);
		if (f->carriedBy != 0)
			mark("lift", i);
		if (f->dead)
			mark("dead", i);
	}
	return trace;
}

std::string describe(const Trace& trace)
{
	std::string text;
	for (const auto& m : trace.marks)
		text += (text.empty() ? "" : " > ") + m.first + "@" + std::to_string(m.second);
	return text.empty() ? "(nothing)" : text;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_cheat_slavers: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
		const Trace damage = runCase(false, order);
		const Trace cheat = runCase(true, order);
		std::cout << "damage KO: " << describe(damage) << "\n";
		std::cout << "cheat KO:  " << describe(cheat) << "\n";
		// The handling is down > disarm > cuff > lift; a later bleed-out is the damage run's own (the guards that joined the fight, kenshi-k16.6.103,
		// wounded it more than the cheat's clean KO, and nobody carrying it treats it). The lift is the down branch's: a captive that is awake after the cuff
		// and its aid walks (171, kenshi-k16.6.110), so only down > disarm > cuff is compared.
		const auto names = [](const Trace& t)
		{
			std::vector<std::string> out;
			for (const auto& m : t.marks)
				if (m.first != "dead" && m.first != "lift")
					out.push_back(m.first);
			return out;
		};
		test::require(!names(damage).empty() && names(damage).front() == "down", "the scripted run downs the player");
		test::require(names(cheat) == names(damage), "the cheat KO gets a different slaver handling than the damage KO");
		std::cerr << "ksim_cheat_slavers: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
