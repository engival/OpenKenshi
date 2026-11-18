// rwh.108 (kenshi-k16.6.135): the melee chase aims at the target's predicted point, pos + facing x speed x lead, once the target has run freely for 1.5 s
// (gate FUN_1402ae0e0), holds its band round that point, and swings on the 1.2 s point. Flat ground, no navmesh: a player-faction runner on a move order
// and one NPC ordered to attack it, a slower and a faster chaser. The slower half is compared with rwh108 section 4 (live: heading error 1.7 mean / 0.6
// median against the predicted point, 22.7 / 12.4 against the current position); the faster half is CODE-DERIVED in the original (rwh108 section 8,
// not captured), so the numbers here are our pin.
#include "ksim/combat/combat.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/coords.hpp"
#include "nav_source.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;
constexpr double kOriginX = -52500.0;
constexpr double kOriginZ = -3350.0;

struct Rig
{
	Simulation sim;
	std::uint64_t runner = 0;
	std::uint64_t chaser = 0;
	const CombatData* data = nullptr;

	// `bake`: a navworld bake. The ground is the spectate_fight line of rwh108 (open desert, x = -50750, z -1150..-650: 1.6 u of relief over 200 u).
	Rig(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& combat, const std::string& bake, double runnerAthletics, double chaserAthletics, double chaserX, double chaserZ)
		: sim(Simulation::load(KENSHI_DATA, 5, RegionConfig{}))
		, data(combat.get())
	{
		sim.enableCombat(combat, CombatSettings{});
		const kworld::ZoneCell zone = kworld::zoneCellOf(kOriginX, kOriginZ);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		sim.enableNavigation(test::openWorldNav(bake, zones));
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: full daylight
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
		std::uint64_t rng = 7;
		const auto spawn = [&](double x, double z, int team, bool player, double athletics)
		{
			state::Fighter f = combat->makeFighter(gd, servant->stringID, "", rng);
			f.x = kOriginX + x;
			f.z = kOriginZ + z;
			const std::optional<knav::Position> ground = sim.navigation()->walker().snap({f.x, 0.0, f.z}, 2000.0);
			test::require(ground.has_value(), "the spawn point is off the mesh");
			f.y = ground->y;
			f.yaw = 0.0; // facing +z
			f.team = team;
			f.playerControlled = player;
			f.skills.athletics = athletics;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		sim.enqueue(Command::setPlayerAreas({{kOriginX, kOriginZ}}));
		sim.run(2);
		runner = spawn(0.0, 0.0, 0, true, runnerAthletics);
		chaser = spawn(chaserX, chaserZ, 1, false, chaserAthletics);
	}

	state::Fighter& me(std::uint64_t id) { return *const_cast<state::Fighter*>(sim.combat()->find(id)); }
	// The runner runs north (+z) along x = 0 on a drive (the player's own steering input); the chaser is ordered to attack it.
	void start()
	{
		sim.enqueue(Command::fighterRun(state::Id{runner}, true));
		drive(0.0);
		sim.enqueue(Command::fighterAttack(state::Id{chaser}, state::Id{runner}));
	}
	// The player's steering input, repeated every tick (a drive lapses without it): heading in radians, 0 = +z (the wire's drive heading).
	void drive(double heading) { sim.enqueue(Command::fighterDrive(state::Id{runner}, 1, heading, ++seq)); }
	std::uint32_t seq = 1;
	double lead() const
	{
		return 1.2 + (1.0 - sim.combat()->rules().attackClipSpeed(*sim.combat()->find(chaser))) * 2.0;
	}
};

double angleBetween(double ax, double az, double bx, double bz)
{
	const double da = std::atan2(ax, az) - std::atan2(bx, bz);
	return std::fabs(std::remainder(da, 2.0 * kPi)) * 180.0 / kPi;
}

double median(std::vector<double> v)
{
	std::sort(v.begin(), v.end());
	return v.empty() ? -1.0 : v[v.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2 || !std::filesystem::is_directory(argv[1]))
	{
		std::cerr << "ksim_chase_intercept: SKIPPED: no navworld bake\n";
		return 77;
	}
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);

		// ---- a slower chaser, starting 70 u to the side (rwh108 chase7) ----
		{
			Rig rig(gd, data, argv[1], 90.0, 30.0, -70.0, 0.0);
			rig.start();
			std::vector<double> errPredicted;
			std::vector<double> errCurrent;
			double lead = 0.0;
			for (int i = 0; i < 20 * 7; ++i)
			{
				rig.drive(0.0);
				rig.me(rig.runner).target = 0; // a player character on a drive picks no fight of its own
				rig.sim.run(1);
				const state::Fighter& r = rig.me(rig.runner);
				const state::Fighter& c = rig.me(rig.chaser);
				lead = rig.lead();
				// From the gate (1.5 s) on, while the chaser runs: its velocity against the two bearings.
				if (i < 20 * 2 || std::hypot(c.velX, c.velZ) < 10.0)
					continue;
				const double speed = std::hypot(r.velX, r.velZ);
				errCurrent.push_back(angleBetween(c.velX, c.velZ, r.x - c.x, r.z - c.z));
				errPredicted.push_back(angleBetween(c.velX, c.velZ, r.x + std::sin(r.yaw) * speed * lead - c.x, r.z + std::cos(r.yaw) * speed * lead - c.z));
			}
			const double meanPredicted = std::accumulate(errPredicted.begin(), errPredicted.end(), 0.0) / static_cast<double>(std::max<std::size_t>(1, errPredicted.size()));
			std::printf("slower chaser: lead %.3f s, %zu samples; heading error vs the predicted point median %.2f mean %.2f deg, vs the current position median %.2f deg\n"
				"  (rwh108 live: 0.6 / 1.7 against the predicted point, 12.4 / 22.7 against the current position)\n",
				lead, errPredicted.size(), median(errPredicted), meanPredicted, median(errCurrent));
			test::require(errPredicted.size() >= 60, "the slower chaser ran for the samples");
			test::require(median(errPredicted) < 3.0, "slower chaser: heading error vs the predicted point has a median under 3 deg");
			test::require(median(errPredicted) < median(errCurrent), "slower chaser: it heads for the predicted point, not the body");
		}

		// ---- a faster chaser (CODE-DERIVED in the original, rwh108 section 8: not captured live; this is our pin) ----
		{
			Rig rig(gd, data, argv[1], 30.0, 90.0, -8.0, 250.0);
			rig.start();
			const std::uint64_t t0 = rig.sim.clockTick();
			double swingAt = -1.0;
			double swingAhead = 0.0; // the chaser's z minus the runner's at the swing start (positive: the chaser is ahead of the runner)
			const_cast<CombatArena*>(rig.sim.combat())->setObserver([&](const CombatEvent& e)
			{
				if (e.kind == CombatEvent::Kind::TechniqueStart && e.who == rig.chaser && swingAt < 0.0)
				{
					swingAt = static_cast<double>(rig.sim.clockTick() - t0) / 20.0;
					swingAhead = rig.me(rig.chaser).z - rig.me(rig.runner).z;
				}
			});
			for (int i = 0; i < 20 * 8 && swingAt < 0.0; ++i)
			{
				rig.drive(0.0);
				rig.me(rig.runner).target = 0;
				rig.sim.run(1);
			}
			std::printf("faster chaser: first swing at %.2f s with the chaser %.1f u ahead of the runner (the runner still coming)\n", swingAt, swingAhead);
			test::require(swingAt >= 0.0, "the faster chaser swings");
			// It stands in the path (here already ahead of the runner, on its line) and starts the swing while the runner is still coming: the 1.2 s point,
			// 85 u/s x 1.2 = 102 u down the runner's line, is in reach long before the runner is. Pinned (ours): 20..150 u ahead of it at the swing start.
			test::require(swingAhead > 20.0 && swingAhead < 150.0, "the faster chaser swings as the runner approaches, not on contact");
		}

		// ---- the lead gate: a target that stops is pursued at the body ----
		{
			Rig rig(gd, data, argv[1], 90.0, 30.0, -70.0, 0.0);
			rig.start();
			const auto hold = [&]
			{
				for (const CombatArena::Member& member : rig.sim.combat()->members())
					if (member.id == rig.chaser)
						return member.leadHold;
				return -1.0;
			};
			for (int i = 0; i < 20 * 3; ++i)
			{
				rig.drive(0.0);
				rig.me(rig.runner).target = 0;
				rig.sim.run(1);
			}
			test::require(hold() >= 1.5, "the lead gate opened after 1.5 s of a free run");
			// The runner stops: the gate closes the same step and the chaser's heading is the body's.
			rig.sim.enqueue(Command::fighterDrive(state::Id{rig.runner}, 0, 0.0, ++rig.seq));
			rig.me(rig.runner).target = 0;
			for (int i = 0; i < 20; ++i)
			{
				rig.me(rig.runner).target = 0;
				rig.sim.run(1);
			}
			std::printf("lead gate: after the runner stopped, hold %.2f s\n", hold());
			test::require(hold() < 1.5, "a target that stops closes the lead gate");
		}
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_chase_intercept: " << e.what() << "\n";
		return 1;
	}
}
