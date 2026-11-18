// kenshi-k16.6.183: the client's far attack order through the server's own world: far.kscen (the sneaking player 300 u from the Dust boss), the squad AI on
// as ksim_server runs it, one Session; the ORDER frame the client sends (attack, id 2). kFlagCombat stays clear and the walk is one move from the order
// until in range. argv[2]: ticks the scenario runs before the order (0: the boss 300 u away; 177: the client's run, the boss walking at the player).
// Needs the world bake (argv[1]; exit 77 without).
#include "ksim/combat_data.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/simdata.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "ksim/placement.hpp"

#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_FAR_SCEN
#error KSIM_FAR_SCEN must be defined
#endif

using namespace ksim;
namespace ks = ksim::simlink;

namespace {

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::is_directory(argv[1]))
		{
			std::cerr << "ksim_combat_flag_scen: SKIPPED: no navworld bake\n";
			return 77;
		}
		const bool trace = std::getenv("KSIM_TRACE") != nullptr;
		const kworld::ZoneCell centre = {ks::kHubZoneX, ks::kHubZoneZ};
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({centre.x + dx, centre.z + dz});
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], zones);
		const scenario::File file = scenario::parseFile(KSIM_FAR_SCEN);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		sim.markPristine();
		const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		sim.enableWeather(*gd);
		sim.newWorld();
		sim.run(1);
		ks::ServerContext context;
		context.squadLimit = 50;
		context.gameData = gd;
		const scenario::Placed placed = scenario::apply(sim, *gd, *data, context, file, 5);
		ServerAiOptions ai;
		ai.registryPath = simDataAiTable();
		enableServerAi(sim, ai);
		const std::uint64_t player = placed.players.front();
		const std::uint64_t boss = placed.others.front();

		ks::Session session(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session.sendTick(w, true);
		std::uint32_t wireFlags = 0;
		float wireX = 0.0f;
		float wireZ = 0.0f;
		const auto parse = [&]
		{
			for (std::size_t p = 0; p < bytes.size();)
			{
				const std::uint32_t len = at<std::uint32_t>(bytes, p);
				const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
				const std::size_t b = p + 6;
				p += 4 + len;
				if (type == ks::kDynamic && at<std::uint64_t>(bytes, b + 8) == player)
				{
					wireFlags = at<std::uint32_t>(bytes, b + 40);
					wireX = at<float>(bytes, b + 16);
					wireZ = at<float>(bytes, b + 24);
				}
			}
			bytes.clear();
		};
		parse();
		if (trace)
			std::printf("clock %llu player %llu boss %llu others %zu\n", static_cast<unsigned long long>(sim.clockTick()), static_cast<unsigned long long>(player), static_cast<unsigned long long>(boss), placed.others.size());
		for (int i = 0; i < (argc > 2 ? std::atoi(argv[2]) : 0); ++i)
		{
			sim.tick();
			if (trace && i % 50 == 0)
				std::printf("pre %d boss %.1f %.1f state %d\n", i, sim.combat()->find(boss)->x, sim.combat()->find(boss)->z, static_cast<int>(sim.combat()->find(boss)->state));
		}
		// the client's frame 300: the order lands on a settled character

		// The ORDER frame as the client sends it: kind 2 (attack), the selection, the target (SimView::attack).
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter f(frame);
		f.begin(ks::kOrder);
		f.u32("seq", 1);
		f.u16("kind", 2);
		f.u16("count", 1);
		f.u64("id", player);
		f.u64("target", boss);
		f.end();
		ks::BinaryWriter out0(bytes);
		test::require(session.onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, out0), "order refused");
		if (trace)
			{ std::printf("order sent, %zu bytes:", bytes.size()); for (const std::uint8_t c : bytes) std::printf("%c", c >= 32 && c < 127 ? c : 46); std::printf("\n"); }
		parse();

		int combatTicks = 0;
		int snaps = 0;
		double lastX = 0.0;
		double lastZ = 0.0;
		bool inRange = false;
		int approachDrops = 0; // ticks off Approach after it started: the walk dropped and restarted
		bool approached = false;
		for (int tick = 0; tick < 25 * 20 && !inRange; ++tick)
		{
			sim.tick();
			ks::BinaryWriter out(bytes);
			session.sendTick(out, false);
			parse();
			const state::Fighter* a = sim.combat()->find(player);
			const state::Fighter* t = sim.combat()->find(boss);
			const double dist = std::hypot(a->x - t->x, a->z - t->z);
			const bool combat = (wireFlags & ks::kFlagCombat) != 0;
			const double step = std::hypot(a->x - lastX, a->z - lastZ);
			if (trace)
				std::printf("t%d st %d tgt %llu tech %d rt %u/%zu seq %u chase %d ord %llu hold %d sneak %d pos %.1f %.1f d %.1f step %.2f wire %.1f %.1f flags 0x%x\n", tick,
					static_cast<int>(a->state), static_cast<unsigned long long>(a->target), a->technique, a->routeNext, a->route.size(), a->routeSeq, a->routeChase,
					static_cast<unsigned long long>(a->orderTarget), a->hold, a->sneak, a->x, a->z, dist, step, wireX, wireZ, wireFlags);
			if (tick > 0 && step > 3.5) // a jump: more than the fastest run (the chase ramps up to 3.0 u/tick, 3.004 on a long straight leg with no corner to brake for, k16.6.248) allows in one tick
				++snaps;
			lastX = a->x;
			lastZ = a->z;
			if (a->state == state::CombatState::Approach || dist > 150.0)
				combatTicks += combat ? 1 : 0;
			approachDrops += approached && a->state != state::CombatState::Approach && dist > 150.0 ? 1 : 0;
			approached = approached || a->state == state::CombatState::Approach;
			inRange = dist < 60.0;
		}
		std::printf("combat bit on %d ticks before range, %d position jumps\n", combatTicks, snaps);
		test::require(combatTicks == 0, "kFlagCombat is clear from the order until in range");
		test::require(approachDrops == 0, "the move-in is not dropped to the hub and restarted while the target is far");
		test::require(snaps == 0, "the move is not dropped and restarted (no position snap-back)");
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_combat_flag_scen: " << e.what() << "\n";
		return 1;
	}
}
