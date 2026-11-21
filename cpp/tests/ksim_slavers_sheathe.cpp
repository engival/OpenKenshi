// Task k16.6.288 (user 2026-11-15, k16.6.285): after the slavers_downed capture no member keeps a fight goal or a weapon in hand. The world
// selector's follow (141 / 171, no Routine) used to leave a drawn weapon drawn, and the member read "Attacking enemies" while a swing's technique
// stayed set. The check: kSettle s after the player is shackled, every caravan member is out of the fight (no goal 16 / 5 / 143 / 32, no
// technique, not inFight) and holds no weapon in hand. KSIM_DUMP=1 prints each member. Exit 77 without the shipped navtiles.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/decision_trace.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace
{

constexpr double kChainBound = 300.0; // s after the cuff: the original's sequence (strip, aid, lift) is well inside it
constexpr double kStandDown = 10.0; // s after the chain ended (ours: the original's sheathe delay is not in the docs)
constexpr double kSettle = 30.0; // s after the cuff; the original's N is not in docs (ours)

struct Row { std::uint64_t id; bool weapon; bool fight; int goal; bool routine; int technique; };

// reloadAtCapture: on the tick the player is shackled the run is saved and loaded back (the stand-down comes after it).
std::vector<Row> play(const std::uint64_t seed, const double settle, const bool reloadAtCapture)
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
	Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
	attack.tick = sim.clockTick() + 100;
	sim.enqueue(attack);
	std::int64_t shackled = -1;
	bool ended = false;
	double endedAt = -1.0;
	for (int i = 0; i < 20 * 330 + 2000; ++i)
	{
		sim.tick();
		if (shackled < 0 && sim.combat()->find(kPlayer)->slaveState == 1)
		{
			shackled = static_cast<std::int64_t>(sim.clockTick());
			if (reloadAtCapture)
			{
				std::stringstream blob;
				sim.save(blob);
				sim.reload(blob);
			}
		}
		if (shackled < 0)
			continue;
		// The rule is "after the capture chain ends": kSettle s after the cuff AND the leader has reached the delivery (192, then 53; it holds 192 while it
		// walks, so 192 is the chain's end, not a step in it) AND no member is on 58 (a medic on the slave). Bounded: a chain that never ends is a failure, not a pass.
		const double since = static_cast<double>(static_cast<std::int64_t>(sim.clockTick()) - shackled) * kTickSeconds;
		const int leaderGoal = sim.charStats(kBoss).goalEnum;
		bool chainOver = since >= settle && (leaderGoal == 192 || leaderGoal == 53);
		for (std::uint64_t id = kBoss; chainOver && id < kBoss + 8; ++id)
			chainOver = sim.charStats(id).goalEnum != 58;
		if (chainOver && endedAt < 0.0)
			endedAt = since;
		if (endedAt >= 0.0 && since - endedAt >= kStandDown) // the stand-down follows the last medic's 58 by a moment: check kStandDown s after the chain ended
		{
			ended = true;
			break;
		}
		if (since >= kChainBound)
			break;
	}
	test::require(shackled >= 0, "the player is captured");
	test::require(ended, "the capture chain ended (the leader on the delivery, no medic on 58) within " + std::to_string(kChainBound) + " s of the cuff");
	if (std::getenv("KSIM_DUMP") != nullptr)
		for (std::uint64_t id = kPlayer; id < kBoss + 8; ++id)
		{
			const state::Fighter* f = sim.combat()->find(id);
			std::cout << "  m" << id << " target " << f->target << " state " << static_cast<int>(f->state) << " ko " << f->unconscious << " slave " << int(f->slaveState) << " engaged " << f->engaged.size() << " combatGoal " << f->combatGoal << " order " << f->orderTarget << " engagedBy " << (f->engaged.empty() ? 0 : f->engaged[0]) << "\n";
		}
	std::vector<Row> rows;
	for (std::uint64_t id = kBoss; id < kBoss + 8; ++id)
	{
		const state::Fighter* f = sim.combat()->find(id);
		const Simulation::CharStats stats = sim.charStats(id);
		bool routine = false;
		sim.world().ecs().each([&](flecs::entity e, const state::Id& who, const state::Fighter&) { if (who.value == id) routine = e.has<state::Routine>(); });
		rows.push_back({id, sim.weaponInHands(id), Simulation::inFightNow(*f), stats.goalEnum, routine, f->techniqueFinished ? -1 : f->technique});
	}
	return rows;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_slavers_sheathe: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		for (const std::uint64_t seed : {std::uint64_t{706960716}, std::uint64_t{3}})
		{
			const std::vector<Row> rows = play(seed, kSettle, false);
			const std::vector<Row> loaded = play(seed, kSettle, true);
			test::require(loaded.size() == rows.size(), "the loaded run has the same members");
			for (std::size_t i = 0; i < rows.size(); ++i)
				test::require(loaded[i].weapon == rows[i].weapon && loaded[i].fight == rows[i].fight, // the goal is not compared: a reload does not keep the selector's pick (goal 53 / 43 at the capture)
					"seed " + std::to_string(seed) + " member " + std::to_string(rows[i].id) + ": a run saved and loaded at the capture stands down as the straight run (weapon "
					+ std::to_string(loaded[i].weapon) + " / " + std::to_string(rows[i].weapon) + ", fight " + std::to_string(loaded[i].fight) + " / " + std::to_string(rows[i].fight) + ")");
			if (std::getenv("KSIM_DUMP") != nullptr)
				for (const Row& r : rows)
					std::cout << "seed " << seed << " m" << r.id << " weapon " << r.weapon << " fight " << r.fight << " goal " << r.goal << " routine " << r.routine << " tech " << r.technique << "\n";
			for (const Row& r : rows)
			{
				const std::string tag = "seed " + std::to_string(seed) + " member " + std::to_string(r.id) + ": ";
				test::require(!r.fight && r.technique < 0, tag + "out of the fight");
				test::require(r.goal != 16 && r.goal != 5 && r.goal != 143 && r.goal != 32, tag + "no attack goal (" + std::to_string(r.goal) + ")");
				test::require(!r.weapon, tag + "weapon sheathed");
			}
		}
		std::cout << "ksim_slavers_sheathe ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_slavers_sheathe: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
