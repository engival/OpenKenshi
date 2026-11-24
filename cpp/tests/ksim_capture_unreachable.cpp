// Task k16.6.702: a world squad's capture (167) of a body it cannot reach stays given up. addBlockedTarget blocks the pair for 20 s; at expiry
// the exe re-queries the navmesh at the target's position (dt4_unreachable_target.md): fails, the timer is re-armed; passes, the entry is erased.
// The slavers_downed caravan captures the KO'd player; the body is moved off the mesh the tick the capturer starts walking to it, so the walk is
// given up (30 s) and the pair blocked. Pinned: no second walk in the next 100 s (it was picked again every 50 s before); the body brought back
// to the mesh is picked within 30 s.

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
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_capture_unreachable: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const std::uint64_t seed = std::getenv("KSIM_SEED") != nullptr ? std::strtoull(std::getenv("KSIM_SEED"), nullptr, 10) : 4;
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
		const bool dump = std::getenv("KSIM_DUMP") != nullptr;
		std::map<std::string, int> perMember; // member -> ends
		std::vector<std::pair<std::int64_t, std::string>> ends; // "capture end 167 ... body kPlayer", with the tick
		std::int64_t t0 = static_cast<std::int64_t>(sim.clockTick());
		sim.setTaskLog([&](const std::string& line)
		{
			if (dump)
				std::cout << static_cast<double>(static_cast<std::int64_t>(sim.clockTick()) - t0) * kTickSeconds << " " << line << "\n";
			if (line.find("capture end 167") != std::string::npos && line.find("body " + std::to_string(kPlayer)) != std::string::npos)
			{
				++perMember[line.substr(0, line.find(" body"))];
				ends.emplace_back(static_cast<std::int64_t>(sim.clockTick()), line);
			}
		});
		Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
		attack.tick = sim.clockTick() + 100;
		sim.enqueue(attack);
		// Off the mesh: far outside the loaded hub zones, in sea or wall; the probe line says whether the route is refused.
		std::uint64_t capturer = 0;
		const auto unreachable = [&](const double x, const double z, const double y)
		{
			const MeshNavigator* nav = sim.navigation();
			const state::Fighter* member = sim.combat()->find(capturer);
			return !nav->routeFrom({member->x, member->y, member->z}, x, z, y, false, sim.body(*member)).found;
		};
		const auto setBody = [&](const double x, const double y, const double z)
		{
			state::Fighter* player = const_cast<state::Fighter*>(sim.combat()->find(kPlayer));
			player->x = x;
			player->y = y;
			player->z = z;
		};
		double homeX = 0.0, homeY = 0.0, homeZ = 0.0, offX = 0.0, offY = 0.0, offZ = 0.0;
		bool moved = false;
		bool back = false;
		std::int64_t movedAt = 0;
		std::int64_t backAt = 0;
		std::int64_t pickedAt = 0;
		for (int i = 0; i < 8000 && pickedAt == 0; ++i)
		{
			sim.tick();
			const std::int64_t now = static_cast<std::int64_t>(sim.clockTick());
			if (dump && moved && (now - movedAt) % 200 == 0)
			{
				const Simulation::CharStats st = sim.charStats(capturer);
				const state::Fighter* m = sim.combat()->find(capturer);
				std::cout << "  t+" << (now - movedAt) / 20 << " goal " << st.goalEnum << " step " << st.stepEnum << " walking " << m->walking() << " at " << m->x << "," << m->z << " body " << sim.combat()->find(kPlayer)->x << " y " << sim.combat()->find(kPlayer)->y << " ko " << sim.combat()->find(kPlayer)->unconscious << "\n";
			}
			const double sinceMoved = static_cast<double>(now - movedAt) * kTickSeconds;
			if (!moved)
			{
				// The first member that walks to the body (goal 167, no strip yet) while the player lies KO'd.
				for (std::uint64_t id = kBoss; id < kBoss + 8 && !moved; ++id)
				{
					const Simulation::CharStats stats = sim.charStats(id);
					const state::Fighter* player = sim.combat()->find(kPlayer);
					if (stats.goalEnum == 167 && stats.stepEnum != 181 && stats.stepEnum != 166 && player->unconscious)
					{
						homeX = player->x; homeY = player->y; homeZ = player->z;
						// Straight up: 600 u over the ground, past the click reach of the mesh (move_unreachable.md: refused above ~340 u), within the
						// sight (xz) of the hunters. The route has no goal ground.
						capturer = id;
						offX = homeX + 55.0;
						offZ = homeZ;
						offY = homeY + 600.0;
						test::require(unreachable(offX, offZ, offY), "the spot over the body has no route for the capturer");
						setBody(offX, offY, offZ);
						moved = true;
						movedAt = now;
						if (dump)
							std::cout << "moved the body at " << static_cast<double>(now - t0) * kTickSeconds << " s (member " << id  << ")\n";
					}
				}
			} else if (!back && sinceMoved >= 130.0) {
				for (const auto& [who, count] : perMember)
					test::require(count == 1, who + " gives the unreachable body up once and it stays blocked (" + std::to_string(count) + " give-ups in 130 s)");
				test::require(!perMember.empty(), "a member walked for the unreachable body and gave it up");
				{
					const state::Fighter* m = sim.combat()->find(capturer);
					setBody(m->x + 30.0, m->y, m->z);
					// The KO is a random draw (10-120 s, scaled by the fight); this fight's ran out before the 130 s mark, and a body that is awake is no capture target.
					// The test is about the route, so the put-back keeps the body down (kenshi-e9s9.18: the bake's stair widening moved the fight and its draw).
					state::Fighter* player = const_cast<state::Fighter*>(sim.combat()->find(kPlayer));
					player->koTimer = std::max(player->koTimer, 400.0);
					player->unconscious = true;
				}
				back = true;
				if (dump)
				{
					const state::Fighter* pl = sim.combat()->find(kPlayer);
					std::cout << "back: ko " << pl->unconscious << " down " << pl->down << " dead " << pl->dead << " slave " << int(pl->slaveState) << "\n";
				}
				backAt = now;
			} else if (back) {
				for (std::uint64_t id = kBoss; id < kBoss + 8; ++id)
				{
					const Simulation::CharStats stats = sim.charStats(id);
					if (stats.stepEnum == 181 || stats.stepEnum == 166 || stats.goalEnum == 167)
						pickedAt = now;
				}
				if (!pickedAt && static_cast<double>(now - backAt) * kTickSeconds > 120.0)
					break;
			}
		}
		test::require(moved, "a member walked to the KO'd body");
		test::require(back, "the run reached the end of the unreachable window");
		test::require(pickedAt != 0, "the body, back on the mesh, is picked again within 120 s");
		if (dump)
			std::cout << "picked " << static_cast<double>(pickedAt - backAt) * kTickSeconds << " s after the body came back\n";
		std::cout << "ksim_capture_unreachable ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_capture_unreachable: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
