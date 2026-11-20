// kenshi-k16.5.20 (rebuilt from rwh.191, findings/perception_cheese_mourn_hold.md): scenarios/perception_cheese.kscen run headless at the user's spot, Mourn's
// Abandoned HQ: a crossbow ranger on the tower ring, 154 u from the Great White Gorillo at his home inside the tower, the wall between them.
// 1. Neutral (relation 0, as the save), the ranged attack held for 30 game minutes: he stays home (never targets, never strays), no bolt lands (his blood
//    never drops), and Perception and Crossbows rise by the aim-XP closed form (rwh.166: dL/dt = ((101 - L) / 101)^2 c per tick the range test passes,
//    c = 0.02 x 0.25 x expGainMultiplier x weight x race rate; the range test pays before the line-of-sight decision, so the wall stops the bolt, not the XP).
// 2. The same with Wildlife <-> the player at war (-100), 10 game minutes: still no target, still home (an attacker he cannot sense is never listed).
// 3. Control, in sight: the ranger without the crossbow, 100 u away in the open, ordered to attack: he engages and closes. (Live he also drops the target
//    within about a second when the ranger is moved behind the wall; ours keeps it for seconds, a known difference, not asserted: k16.5.20 report.)
// Needs the world navmesh bake (argv[1], exit 77 without the Mourn region).
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

constexpr const char* kWildlife = "3943-gamedata.base";
constexpr const char* kPlayerFaction = "204-gamedata.base";

double health(const state::Fighter& f)
{
	double sum = f.blood;
	for (const state::FighterPart& p : f.parts)
		sum += p.health;
	return sum;
}

state::Id factionId(Simulation& sim, const char* sid)
{
	return *flecs::entity(ecsOf(sim).c_ptr(), sim.importFaction(sid).id).get<state::Id>();
}

struct Cheese
{
	scenario::File file;
	scenario_rig::Rig rig;
	Simulation& sim;
	std::uint64_t shooterId = 0;
	std::uint64_t gorilloId = 0;
	double homeX = 0.0;
	double homeZ = 0.0;

	Cheese(const scenario::File& f, const std::string& bake, int relation)
		: file(f), rig(file, bake), sim(rig.sim)
	{
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		shooterId = placed.idOf.at(1);
		gorilloId = placed.idOf.at(2);
		const state::Fighter* gorillo = sim.combat()->find(gorilloId);
		test::require(sim.combat()->find(shooterId) != nullptr && gorillo != nullptr, "the scenario placed no shooter or gorillo");
		homeX = gorillo->x;
		homeZ = gorillo->z;
		const state::Id wild = factionId(sim, kWildlife);
		const state::Id player = factionId(sim, kPlayerFaction);
		sim.enqueue(Command::setFactionRelation(wild, player, relation));
		sim.enqueue(Command::setFactionRelation(player, wild, relation));
	}

	const state::Fighter& shooter() { return *sim.combat()->find(shooterId); }
	const state::Fighter& gorillo() { return *sim.combat()->find(gorilloId); }
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/perception_cheese.kscen");
		if (argc < 2 || !scenario_rig::hasRegion(file, argv[1]))
		{
			std::cout << "ksim_perception_cheese: SKIPPED: needs the world navmesh bake with the Mourn region (pass its directory)\n";
			return 77;
		}
		const std::string bake = argv[1];
		const char* minutesEnv = std::getenv("KSIM_CHEESE_MINUTES");
		const int heldMinutes = minutesEnv != nullptr ? std::atoi(minutesEnv) : 30;

		// ---- 1 and 2: the ranged order held behind the wall ----
		for (const int relation : { 0, -100 })
		{
			Cheese w(file, bake, relation);
			const int ticks = (relation == 0 ? heldMinutes : std::max(1, heldMinutes / 3)) * 60 * 20;
			const RaceCombat* race = w.rig.combatData->race(w.shooter().raceSid);
			test::require(race != nullptr, "the shooter's race is not in the data");
			const double unit = 0.02 * 0.25 * w.rig.combatData->k.expGainMultiplier;
			const double cPerception = unit * 3.0 * race->xpRate(0x18);
			const double cCrossbows = unit * 1.2 * race->xpRate(0x23);
			const double perception0 = w.shooter().skills.perception;
			const double crossbows0 = w.shooter().skills.crossbows;
			const double health0 = health(w.gorillo());
			std::cout << "relation " << relation << ": shooter at " << w.shooter().x << ", " << w.shooter().z << " ("
				<< std::hypot(w.shooter().x - w.homeX, w.shooter().z - w.homeZ) << " u from the gorillo's home), Perception " << perception0 << ", Crossbows " << crossbows0 << "\n";

			// The ranger holds position (the user's HOLD); the scenario's attack order, like any attack order, clears the hold at the first tick, so it is set again.
			w.sim.run(1);
			fighterOf(w.sim, w.shooterId)->hold = true;
			int rangedTicks = 0;
			int firstHit = -1;
			int targetTicks = 0;
			double wander = 0.0;
			for (int tick = 0; tick < ticks; ++tick)
			{
				w.sim.run(1);
				if (w.shooter().target == w.gorilloId && w.shooter().ranged.ammo >= 1)
					++rangedTicks; // the range test pays XP on each of these ticks, a shot held by the wall included
				if (w.gorillo().target != 0)
					++targetTicks;
				wander = std::max(wander, std::hypot(w.gorillo().x - w.homeX, w.gorillo().z - w.homeZ));
				if (firstHit < 0 && health(w.gorillo()) < health0 - 1e-9)
				{
					firstHit = tick;
					std::cout << "  HIT at tick " << tick << ": shooter at " << w.shooter().x << ", " << w.shooter().y << ", " << w.shooter().z << " ammo " << w.shooter().ranged.ammo << "\n";
				}
				if (tick % 1200 == 0)
					std::cout << "  " << tick / 1200 << " min: task " << w.shooter().rangedTask << ", Perception " << w.shooter().skills.perception << ", Crossbows "
						<< w.shooter().skills.crossbows << ", gorillo " << w.gorillo().x << ", " << w.gorillo().z << " target " << w.gorillo().target << " shooter " << w.shooter().x << ", " << w.shooter().z << " ammo " << w.shooter().ranged.ammo << "\n";
			}
			const double t = rangedTicks * 0.05;
			const auto closed = [&](double level, double c) { return 101.0 - 1.0 / (1.0 / (101.0 - level) + c * t / (101.0 * 101.0)); };
			const auto close = [](double gain, double expected) { return std::fabs(gain - expected) <= 0.03 * expected; }; // 3 % of the gain
			const double perception = w.shooter().skills.perception;
			const double crossbows = w.shooter().skills.crossbows;
			std::cout << "  ranged task " << rangedTicks << " of " << ticks << " ticks; gorillo targeted him " << targetTicks << " ticks, strayed at most " << wander
				<< " u, first bolt tick " << firstHit << "; Perception " << perception << " (closed form " << closed(perception0, cPerception) << "), Crossbows " << crossbows
				<< " (closed form " << closed(crossbows0, cCrossbows) << ")\n";
			test::require(firstHit < 0, "a bolt landed on the gorillo");
			test::require(targetTicks == 0, "the gorillo listed the shooter he cannot sense");
			test::require(wander < 10.0, "the gorillo left home");
			test::require(rangedTicks >= ticks * 95 / 100, "the shooter's task did not stay the ranged attack");
			test::require(perception > perception0 && crossbows > crossbows0, "the skills did not rise");
			test::require(close(perception - perception0, closed(perception0, cPerception) - perception0), "Perception is off the closed form");
			test::require(close(crossbows - crossbows0, closed(crossbows0, cCrossbows) - crossbows0), "Crossbows is off the closed form");
		}

		// ---- 3: control, in sight ----
		{
			scenario::File control = file;
			control.chars[0].items.clear();
			control.chars[0].ranged = false;
			control.chars[0].hold = false;
			Cheese w(control, bake, 0);
			// A spot 100 u from his home with a clear line to him (the tower's open floor), found by the sim's own line of sight.
			const perception::Environment& env = w.sim.combat()->environment();
			const state::Fighter& gorillo = w.gorillo();
			bool placed = false;
			for (int i = 0; i < 16 && !placed; ++i)
			{
				const double a = i * 3.14159265358979 / 8.0;
				const double x = w.homeX + 100.0 * std::sin(a);
				const double z = w.homeZ + 100.0 * std::cos(a);
				state::Fighter* shooter = fighterOf(w.sim, w.shooterId);
				shooter->x = x;
				shooter->z = z;
				shooter->y = gorillo.y;
				placed = env.lineOfSight && env.lineOfSight(w.shooterId, *shooter, w.gorilloId, gorillo)
					&& !(env.shotBlocker && env.shotBlocker({x, shooter->y + 18.0, z}, gorillo) >= 0.0);
			}
			test::require(placed, "no spot 100 u from his home with a line to him");
			const double startX = w.shooter().x;
			const double startZ = w.shooter().z;
			w.sim.enqueue(Command::fighterAttack(state::Id{w.shooterId}, state::Id{w.gorilloId}));
			const double gap0 = std::hypot(startX - w.homeX, startZ - w.homeZ);
			bool engaged = false;
			for (int tick = 0; tick < 20 * 20 && !engaged; ++tick)
			{
				w.sim.run(1);
				engaged = w.gorillo().target == w.shooterId;
			}
			for (int tick = 0; tick < 3 * 20; ++tick)
				w.sim.run(1);
			const double closed = std::hypot(w.gorillo().x - w.homeX, w.gorillo().z - w.homeZ);
			std::cout << "control: ranger " << gap0 << " u away in sight; engaged " << engaged << ", the gorillo moved " << closed << " u toward him in 3 s more\n";
			test::require(engaged, "the gorillo did not engage an attacker in sight");
			test::require(closed > 10.0, "the gorillo did not close on an attacker in sight");
		}
		std::cout << "ksim_perception_cheese ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_perception_cheese: " << e.what() << "\n";
		return 1;
	}
}
