// Task k16.6.288 (user 2026-11-15, k16.6.285): after the slavers_downed capture no member keeps a fight goal or a weapon in hand. The world
// selector's follow (141 / 171, no Routine) used to leave a drawn weapon drawn, and the member read "Attacking enemies" while a swing's technique
// stayed set. The check: kSettle s after the player is shackled, every caravan member is out of the fight (no goal 16 / 5 / 143 / 32, no
// technique, not inFight) and holds no weapon in hand. KSIM_DUMP=1 prints each member. Exit 77 without the shipped navtiles.
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/decision_trace.hpp"
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
#include <cstdio>
#include <cstdlib>
#include <map>
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

constexpr double kSettle = 30.0; // s after the cuff; the original's N is not in docs (ours)

struct Row { std::uint64_t id; bool weapon; bool fight; int goal; bool routine; int technique; };

std::vector<Row> play(const std::uint64_t seed, const double settle)
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
	for (int i = 0; i < 6000; ++i)
	{
		sim.tick();
		if (shackled < 0 && sim.combat()->find(kPlayer)->slaveState == 1)
			shackled = static_cast<std::int64_t>(sim.clockTick());
		if (shackled >= 0 && static_cast<double>(static_cast<std::int64_t>(sim.clockTick()) - shackled) * kTickSeconds >= settle)
			break;
	}
	test::require(shackled >= 0, "the player is captured");
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
			const std::vector<Row> rows = play(seed, kSettle);
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
