// The player speed modes (docs/formats/SIMLINK.md order 10 `which` 5; docs/reference/SQUAD_FOLLOW.md s3 "Group speed"):
// Walk, Jog and Run top speeds on flat ground, the Group cap on the slowest live member and its release when that member
// dies, the which-2 alias, a v65 save round trip, and a v64 save whose run bool loads as Walk. Player orders only.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat.hpp"
#include "ksim/navigator.hpp"
#include "ksim/save.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTickSeconds = 0.05; // the tests' 20 ticks per second

struct Rig
{
	Simulation sim;
	std::shared_ptr<const CombatData> data;
	const kdata::GameData& gd;
	std::shared_ptr<MeshNavigator> nav;
	std::string sid;
	std::uint64_t rng = 12345;

	Rig(const std::shared_ptr<const CombatData>& d, const kdata::GameData& g, const std::shared_ptr<MeshNavigator>& n)
		: sim(Simulation::load(KENSHI_DATA, 77))
		, data(d)
		, gd(g)
		, nav(n)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute)); // 10:00, full daylight
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
	}

	std::uint64_t spawn(double x, double z, int team = 0, bool player = true)
	{
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
		f.team = team;
		f.playerControlled = player;
		const auto spot = nav->walker().snap({x, 1535.0, z}, 200.0);
		test::require(spot.has_value() && std::hypot(spot->x - x, spot->z - z) < 6.0, "no mesh at the spawn spot");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	bool spotAt(double cx, double cz, double distance, double& x, double& z, int first = 0)
	{
		for (int k = 0; k < 64; ++k)
		{
			const double a = (first + k) * 0.0981748;
			x = cx + std::sin(a) * distance;
			z = cz + std::cos(a) * distance;
			const auto spot = nav->walker().snap({x, 1535.0, z}, 20.0);
			if (spot && std::hypot(spot->x - x, spot->z - z) < 1.0)
				return true;
		}
		return false;
	}

	const state::Fighter& at(std::uint64_t id) const { return *sim.combat()->find(id); }
	state::Fighter& mut(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }
	void run(int ticks) { sim.run(static_cast<std::uint64_t>(ticks)); }

	// Puts a character back at (x, z) on the mesh (the fixtures' teleport).
	void place(std::uint64_t id, double x, double z)
	{
		const auto spot = nav->walker().snap({x, 1535.0, z}, 50.0);
		test::require(spot.has_value(), "no mesh where the character is placed back");
		state::Fighter& f = mut(id);
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		f.clearRoute();
	}
};

// A heading along which a driven character runs at least `length` units from (x, z) without being blocked, found by
// probing the eight main directions (the Rig starts from rest each time).
double openHeading(Rig& rig, std::uint64_t id, double x, double z, double length)
{
	for (int k = 0; k < 16; ++k)
	{
		const double a = k * kPi / 8.0;
		rig.place(id, x, z);
		rig.sim.enqueue(Command::fighterDrive(state::Id{id}, 1, a, 1));
		rig.run(7 * 20);
		const double moved = std::hypot(rig.at(id).x - x, rig.at(id).z - z);
		rig.sim.enqueue(Command::fighterDrive(state::Id{id}, 0, a, 2));
		rig.run(10);
		if (moved >= length)
			return a;
	}
	throw test::Failure("no open heading near the Hub");
}

// The top speed (u/s) of a forward drive from rest along `heading`, the largest per-tick displacement over `seconds`.
double driveTop(Rig& rig, std::uint64_t id, double heading, double seconds)
{
	rig.sim.enqueue(Command::fighterDrive(state::Id{id}, 1, heading, 1));
	double top = 0.0;
	double lastX = rig.at(id).x;
	double lastZ = rig.at(id).z;
	for (int i = 0; i < static_cast<int>(seconds / kTickSeconds); ++i)
	{
		rig.run(1);
		top = std::max(top, std::hypot(rig.at(id).x - lastX, rig.at(id).z - lastZ) / kTickSeconds);
		lastX = rig.at(id).x;
		lastZ = rig.at(id).z;
	}
	rig.sim.enqueue(Command::fighterDrive(state::Id{id}, 0, heading, 2));
	rig.run(10);
	return top;
}

void mode(Rig& rig, std::uint64_t id, std::uint8_t value, std::uint64_t group = 0)
{
	rig.sim.enqueue(Command::fighterSpeedMode(state::Id{id}, value, group));
	rig.run(1);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_speed_modes HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// ---- Walk, Jog and Run: the measured top speed of a drive on flat ground ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t player = rig.spawn(kHubX, kHubZ);
			const double startX = rig.at(player).x;
			const double startZ = rig.at(player).z;
			const double heading = openHeading(rig, player, startX, startZ, 250.0);
			const double physics = rig.sim.combat()->runSpeed(rig.at(player));
			const double walk = data->race(rig.at(player).raceSid)->walkSpeed;
			test::require(physics > 55.0, "the rig's run speed does not separate Run from Jog");
			rig.place(player, startX, startZ);
			const double runTop = driveTop(rig, player, heading, 8.0);
			std::printf("speed modes: Run %.3f u/s (physics %.3f)\n", runTop, physics);
			test::require(std::fabs(runTop - physics) < 1.0, "Run is not the uncapped run speed");
			mode(rig, player, 1);
			test::require(rig.at(player).moveMode == 1, "the Jog command did not apply");
			rig.place(player, startX, startZ);
			const double jogTop = driveTop(rig, player, heading, 8.0);
			std::printf("speed modes: Jog %.3f u/s (cap 55)\n", jogTop);
			test::require(std::fabs(jogTop - 55.0) < 1.0, "Jog is not capped at 55");
			mode(rig, player, 0);
			rig.place(player, startX, startZ);
			const double walkTop = driveTop(rig, player, heading, 8.0);
			std::printf("speed modes: Walk %.3f u/s (race walk speed %.3f)\n", walkTop, walk);
			test::require(std::fabs(walkTop - walk) < 1.0, "Walk is not the race walk speed");
			// The which-2 alias: 0 = Walk, 1 = Run.
			rig.sim.enqueue(Command::fighterRun(state::Id{player}, true));
			rig.run(1);
			test::require(rig.at(player).moveMode == 2 && rig.at(player).speedGroup == 0, "the run alias is not Run");
			rig.place(player, startX, startZ);
			const double aliasTop = driveTop(rig, player, heading, 8.0);
			std::printf("speed modes: alias 1 %.3f u/s\n", aliasTop);
			test::require(std::fabs(aliasTop - physics) < 1.0, "the run alias is not the run speed");
			rig.sim.enqueue(Command::fighterRun(state::Id{player}, false));
			rig.run(1);
			test::require(rig.at(player).moveMode == 0, "the walk alias is not Walk");
			rig.place(player, startX, startZ);
			test::require(std::fabs(driveTop(rig, player, heading, 8.0) - walk) < 1.0, "the walk alias is not the walk speed");

			// The modes cap move orders and drives; a chase always runs (SIMLINK.md order 10 `which` 5).
			const auto enemySpot = nav->walker().snap(
				{startX + std::sin(heading) * 200.0, 1535.0, startZ + std::cos(heading) * 200.0}, 30.0);
			test::require(enemySpot.has_value(), "no spot for the chased enemy");
			const std::uint64_t enemy = rig.spawn(enemySpot->x, enemySpot->z, 1, false);
			rig.place(player, startX, startZ);
			rig.sim.enqueue(Command::fighterAttack(state::Id{player}, state::Id{enemy}));
			double chaseTop = 0.0;
			double lastX = rig.at(player).x;
			double lastZ = rig.at(player).z;
			for (int i = 0; i < 4 * 20; ++i)
			{
				rig.run(1);
				chaseTop = std::max(chaseTop, std::hypot(rig.at(player).x - lastX, rig.at(player).z - lastZ) / ksim::kTickSeconds);
				lastX = rig.at(player).x;
				lastZ = rig.at(player).z;
			}
			std::printf("speed modes: Walk-mode chase %.3f u/s (walk %.3f)\n", chaseTop, walkTop);
			test::require(chaseTop > walkTop + 5.0, "a Walk-mode character's chase is capped at the walk speed");
		}

		// ---- Group: every member capped at the slowest live, conscious member's own run speed ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t fast = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z), "no spot for the slow member");
			const std::uint64_t slow = rig.spawn(x, z);
			rig.mut(slow).encumbrance = 0.5; // a slow member: half encumbered
			const double fastPhysics = rig.sim.combat()->runSpeed(rig.at(fast));
			const double slowPhysics = rig.sim.combat()->runSpeed(rig.at(slow));
			test::require(slowPhysics < fastPhysics - 5.0, "the slow member is not slower");
			const std::uint64_t group = std::min(fast, slow);
			mode(rig, fast, 3, group);
			mode(rig, slow, 3, group);
			test::require(rig.at(fast).speedGroup == group && rig.at(slow).speedGroup == group, "the group was not formed");
			const double startX = rig.at(fast).x;
			const double startZ = rig.at(fast).z;
			const double heading = openHeading(rig, fast, startX, startZ, 250.0);
			rig.place(fast, startX, startZ);
			const double grouped = driveTop(rig, fast, heading, 8.0);
			std::printf("speed modes: Group %.3f u/s (slow member's run %.3f, fast %.3f)\n", grouped, slowPhysics, fastPhysics);
			test::require(std::fabs(grouped - slowPhysics) < 1.0, "Group is not capped at the slowest member's run speed");

			// A save round trip keeps the mode and the group.
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_speed_modes_test.ksav";
			const std::uint64_t hash = rig.sim.stateHash();
			rig.sim.saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
			test::require(loaded.stateHash() == hash, "the v65 save does not load to the saved hash");
			loaded.enableCombat(data, CombatSettings{});
			test::require(loaded.combat()->find(fast)->moveMode == 3 && loaded.combat()->find(fast)->speedGroup == group,
				"the group did not survive the save");
			test::require(loaded.combat()->find(slow)->moveMode == 3 && loaded.combat()->find(slow)->speedGroup == group,
				"the slow member's group did not survive the save");
			std::remove(path.c_str());

			// A v64 save keeps only the run bool: Walk writes run = 0 and loads as Walk, Run writes 1 and loads as Run.
			mode(rig, fast, 0);
			mode(rig, slow, 2);
			const std::string old = std::string(KSIM_BINARY_DIR) + "/ksim_speed_modes_v64.ksav";
			writeSaveFile(old, rig.sim.world().ecs(), rig.sim.world().loadOrder(), {}, kSaveVersion - 1);
			Simulation legacy = Simulation::loadGame(KENSHI_DATA, order, old);
			legacy.enableCombat(data, CombatSettings{});
			test::require(legacy.combat()->find(fast)->moveMode == 0, "a v64 run = 0 did not load as Walk");
			test::require(legacy.combat()->find(slow)->moveMode == 2, "a v64 run = 1 did not load as Run");
			std::remove(old.c_str());

			// The slow member dies: it stops counting and the cap rises to the survivor's own run speed.
			mode(rig, fast, 3, group);
			mode(rig, slow, 3, group);
			rig.mut(slow).dead = true;
			rig.place(fast, startX, startZ);
			const double freed = driveTop(rig, fast, heading, 8.0);
			std::printf("speed modes: Group after the slow member died %.3f u/s (fast %.3f)\n", freed, fastPhysics);
			test::require(std::fabs(freed - fastPhysics) < 1.0, "the dead member still caps the group");
		}

		std::cout << "ksim_speed_modes: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_speed_modes: FAILED: " << e.what() << "\n";
		return 1;
	}
}
