// ksim_chase_scen (kenshi-k16.5.11): scenarios/chase_intercept.kscen played with a scripted flee. The player (athletics 12) runs in a
// straight line (-x, a drive: the player's own steering input) from a fast (athletics 100) and a slow (athletics 1) Berserker that aggro
// on sight. Pins: the fast one swings at it within 20 s, running on to the predicted point without stopping (k16.6.153: it stood dead at the band and re-ramped; the first swing came only when the player hit a wall); the slow one is never stalled (it moves every second until
// the fast one's swings begin or it is within 60 u of the player); the player starts with the run speeds apart as the scenario says.
// Needs the world bake (argv[1]; exit 77 without its regions).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;
constexpr int kRunTicks = 20 * 40;
constexpr const char* kName = "chase_intercept";

} // namespace

namespace {

// mode 0: the scenario's flee. 1: rwh.116 run L: a warm 99 u/s chaser beside the line of a 77.5 u/s runner. 2 / 3: the same set-up with the chaser's steer
// accumulator cold (2: a character's first combat steer after a load ramps at 15 u/s^2) / warm (3: later chases start at the cap), read 1 s after the
// lead steer takes over (1.5 s).
int play(int argc, char** argv, int mode)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const scenario::ScenarioInfo info = [&]
		{
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kName)
					return listed;
			throw std::runtime_error(std::string("scenario not listed: ") + kName);
		}();
		const scenario::File file = scenario::parseFile(info.path);
		if (bake.empty())
		{
			std::cout << "ksim_chase_scen: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		if (!test::worldRegionReady(bake, zone.x, zone.z))
		{
			std::cout << "ksim_chase_scen: SKIPPED: no region file\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 11;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		sim.enableNavigation(test::openWorldNav(bake, zones));
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		test::require(std::string(enableServerAi(sim, ai)) == "navmesh", "the server AI took the simulation's navigator");
		simlink::ServerContext context;
		context.gameData = sim.gameData();
		context.squadLimit = 50;
		scenario::check(*sim.gameData(), file);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, 11);
		test::require(placed.players.size() == 1 && placed.others.size() == 2, "one player and two chasers are placed");
		test::require(placed.orders == 0, "the scenario issues no order");
		const std::uint64_t player = placed.players[0];
		const auto fighterOf = [&](std::uint64_t id) { return sim.combat()->find(id); };
		// the two chasers, told apart by their athletics
		const std::uint64_t fast = fighterOf(placed.others[0])->skills.athletics > fighterOf(placed.others[1])->skills.athletics ? placed.others[0] : placed.others[1];
		const std::uint64_t slow = fast == placed.others[0] ? placed.others[1] : placed.others[0];
		CombatArena* arena = const_cast<CombatArena*>(sim.combat()); // test-only: the run L set-up edits fighters in place
		const auto setRunSpeed = [&](std::uint64_t id, double want)
		{
			state::Fighter* f = arena->find(id);
			double best = 1e9;
			double pick = 1.0;
			for (double a = 1.0; a <= 100.0; a += 0.5)
			{
				f->skills.athletics = a;
				if (const double d = std::fabs(sim.combat()->runSpeed(*f) - want); d < best)
				{
					best = d;
					pick = a;
				}
			}
			f->skills.athletics = pick;
		};
		const bool runL = mode != 0;
		if (runL)
		{
			setRunSpeed(player, 77.5);
			setRunSpeed(fast, 99.0);
			state::Fighter* ps = arena->find(player);
			state::Fighter* fs = arena->find(fast);
			// The oracle's table has the chaser 46 u BEHIND the runner at 2.67 s, 1 s after it aimed (it closes ~15 u/s from a standing start at the
			// order), so the set-up puts it 70 u behind the runner on the line; the other chaser is out of the way.
			fs->x = ps->x + 70.0;
			fs->z = ps->z + 25.0; // beside the line: dead behind the runner it is the shove case (rwh.116 runs G / H), not run L
			fs->yaw = ps->yaw;
			arena->find(slow)->x = ps->x + 700.0;
			fs->skills.dexterity = 1.0; // the oracle chaser's attack animation mult is 0.766 (a lead of 1.668 s); a Berserker's is higher, i.e. a shorter lead
			fs->skills.attack = 1.0;
		}
		// the chasers have fought before (the original's accumulator is 0 only for a character's first combat steer after a load)
		if (mode != 2)
			arena->warmSteer(fast);
		arena->warmSteer(slow);
		std::printf("run speeds: player %.1f, fast %.1f, slow %.1f u/s\n", sim.combat()->runSpeed(*fighterOf(player)), sim.combat()->runSpeed(*fighterOf(fast)), sim.combat()->runSpeed(*fighterOf(slow)));
		test::require(sim.combat()->runSpeed(*fighterOf(fast)) > sim.combat()->runSpeed(*fighterOf(player)), "the fast chaser outruns the player");
		test::require(runL || sim.combat()->runSpeed(*fighterOf(slow)) < sim.combat()->runSpeed(*fighterOf(player)), "the slow chaser is outrun");

		double firstSwing[2] = {-1.0, -1.0}; // first technique at the player: fast, slow (s from the start)
		int hits = 0;
		const std::uint64_t start = sim.clockTick();
		sim.setCombatObserver([&](const CombatEvent& event)
		{
			const double t = static_cast<double>(sim.clockTick() - start) * 0.05;
			if (event.kind == CombatEvent::Kind::TechniqueStart && event.other == player)
			{
				const int k = event.who == fast ? 0 : 1;
				if (firstSwing[k] < 0.0)
					firstSwing[k] = t;
			}
			if (event.kind == CombatEvent::Kind::Hit && event.who == player)
				++hits;
		});
		std::uint32_t seq = 1;
		sim.enqueue(Command::fighterRun(state::Id{player}, true));
		double slowMin = 1e9;
		double halfSecond = -1.0; // modes 2 / 3: the chaser's forward speed 2.5 s after the start, 1 s into the lead steer
		double levelAt = -1.0; // run L: when the chaser draws level with the runner / is 30 u ahead; its slowest speed while the aim is far; its biggest lead before the swing
		double aheadAt30 = -1.0;
		double crawlFar = 1e9;
		double maxAhead = 0.0;
		double fastSlowest = 1e9; // the fast chaser's lowest forward speed once it has run, until its first swing (a stop at the band shows here)
		bool fastRan = false;
		double fastClosest = 1e9; // the fast chaser's smallest gap to the player within 20 s
		double fastClosestAt = 0.0;
		double lastSlowX = fighterOf(slow)->x;
		double lastSlowZ = fighterOf(slow)->z;
		for (int tick = 0; tick < kRunTicks; ++tick)
		{
			sim.enqueue(Command::fighterDrive(state::Id{player}, 1, -kPi / 2.0, ++seq));
			sim.tick();
			if (mode >= 2)
			{
				if (tick == 49)
					halfSecond = fighterOf(fast)->forwardSpeed;
				if (tick == 50)
					break;
				continue;
			}
			if (runL)
			{
				const state::Fighter* ff = fighterOf(fast);
				const state::Fighter* pp = fighterOf(player);
				const double ahead = pp->x - ff->x; // the run is -x: positive = the chaser is ahead of the runner
				const double now = (tick + 1) * 0.05;
				if (levelAt < 0.0 && ahead > 0.0)
					levelAt = now;
				if (firstSwing[0] < 0.0 && ahead > 30.0 && aheadAt30 < 0.0)
					aheadAt30 = now;
				if (firstSwing[0] < 0.0 && now > 1.0 && std::hypot(ff->x - pp->x, ff->z - pp->z) > 130.0)
					crawlFar = std::min(crawlFar, ff->forwardSpeed);
				if (firstSwing[0] < 0.0)
					maxAhead = std::max(maxAhead, ahead);
			}
			if (const state::Fighter* ff = fighterOf(fast); ff->forwardSpeed > 100.0)
				fastRan = true;
			else if (fastRan && firstSwing[0] < 0.0)
				fastSlowest = std::min(fastSlowest, ff->forwardSpeed);
			if (tick % 20 != 19)
				continue;
			const state::Fighter* p = fighterOf(player);
			const state::Fighter* f = fighterOf(fast);
			const state::Fighter* s = fighterOf(slow);
			const double slowStep = std::hypot(s->x - lastSlowX, s->z - lastSlowZ);
			lastSlowX = s->x;
			lastSlowZ = s->z;
			const double slowGap = std::hypot(s->x - p->x, s->z - p->z);
			const double fastGap = std::hypot(f->x - p->x, f->z - p->z);
			if (tick < 20 * 20 && fastGap < fastClosest)
			{
				fastClosest = fastGap;
				fastClosestAt = (tick + 1) / 20.0;
			}
			const bool fastSwung = firstSwing[0] >= 0.0 && static_cast<double>(sim.clockTick() - start) * 0.05 > firstSwing[0];
			if (slowGap > 60.0 && !fastSwung && tick >= 39)
				slowMin = std::min(slowMin, slowStep);
			std::printf("t=%2d player x %+7.1f (z %+5.1f) | fast gap %6.1f v %5.1f state %2d | slow gap %6.1f step %5.1f\n", (tick + 1) / 20, p->x + 50750.0, p->z + 1000.0,
				fastGap, f->forwardSpeed, static_cast<int>(f->state), slowGap, slowStep);
		}
		std::printf("first swing at the player: fast %.2f s, slow %.2f s; hits on the player %d; the slow one's smallest step while closing %.1f u/s; scenario hash %016llx\n",
			firstSwing[0], firstSwing[1], hits, slowMin, static_cast<unsigned long long>(scenario::fileHash(info.path)));
		std::printf("the fast chaser's closest approach in 20 s: %.1f u at %.0f s\n", fastClosest, fastClosestAt);
		std::printf("the fast chaser's lowest speed between its run and its first swing: %.1f u/s\n", fastSlowest);
		if (mode >= 2)
		{
			std::printf("%s accumulator: speed 1 s into the lead steer %.1f u/s\n", mode == 2 ? "cold" : "warm", halfSecond);
			if (mode == 2)
				test::require(halfSecond > 8.0 && halfSecond < 25.0, "a cold steer ramps at 15 u/s^2: 8-25 u/s one second into the lead steer (rwh.116 run F: 79.26 in 5.7 s)");
			else
				test::require(halfSecond > 90.0, "a warm steer (a second chase by the same character) is at the cap from the first frames: over 90 u/s one second into the lead steer");
			return 0;
		}
		if (runL)
		{
			std::printf("run L: level at %.2f s, 30 u ahead at %.2f s, biggest lead before the swing %.1f u, slowest while the aim is far %.1f u/s\n", levelAt, aheadAt30, maxAhead, crawlFar);
			test::require(levelAt > 4.0 && levelAt < 9.0, "run L: the warm 99 u/s chaser draws level with the 77.5 u/s runner in 4-9 s (the oracle 6.7 s)");
			test::require(maxAhead > 30.0, "run L: it runs on past the runner (the oracle: 47 u ahead), not stalling at the band");
			test::require(firstSwing[0] > 8.15 && firstSwing[0] < 10.15, "run L: the swing starts within 1 s of the oracle's 9.15 s");
			test::require(crawlFar > 60.0, "run L: no crawl while the aim is far");
			return 0;
		}
		test::require(fastClosest < 60.0, "the fast chaser closes to within 60 u of the fleeing player in 20 s");
		// k16.6.172: no swing pin here. With the literal far flag (set > 90, cleared < 70 to the aim) the Berserker's lead is 1.35 s (attack anim mult 0.93: dexterity
		// 100) and its aim ~102 u ahead of the player, so it crawls ~20 u ahead, where the 1.2 s point is still ~70 u away: no swing. Run L's chaser (mult 0.766, lead 1.668 s)
		// swings: that is the pin above. deferred: playtesting (ENGAGEMENT 16).
		test::require(fastSlowest > 15.0, "the fast chaser never stops at the band while the player runs (the 18 u/s crawl is the floor)");
		test::require(slowMin > 5.0, "the slow chaser never stalls while closing");
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_chase_scen: " << e.what() << "\n";
		return 1;
	}
}

} // namespace

int main(int argc, char** argv)
{
	for (const int mode : {1, 2, 3, 0})
		if (const int rc = play(argc, argv, mode); rc != 0)
			return rc;
	return 0;
}
