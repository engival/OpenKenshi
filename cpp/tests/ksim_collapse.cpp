// The limb collapse law end to end (kinspect 8de1eab findings/medical, VERIFIED-live; the forced KO from task BL,
// kinspect down_ko_trigger.md / fb0248d): an intact limb is down only when its effective health is below the character's
// KO point. A leg at -50 stands and the character walks; at -52 (below the -51.65 point) he is down and the collapse fires
// the forced KO writer: 3.0 medical timer units, then awake with down still set, so he only crawls (5 u/s, task BF); the
// collapse sends one EVENT 48 cause 0. Raising the health above the point stands him up on the next tick (the flag is
// recomputed, never a timer); a severed limb fires the same writer (3.0 floor) and leaking. Needs the Hub navmesh cache
// (argv[1]; the knav_cache fixture).

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kToughness = 55.5337; // Engival's: the KO point -51.65

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_collapse HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kdata::Record* servant = recordNamed(gd, "CHARACTER", "Escaped Servant");

		CombatSettings settings;
		settings.medicalClock = 1.0; // one step of dt is dt medical units
		Simulation sim = Simulation::load(KENSHI_DATA, 77);
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::uint64_t rng = 12345;
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.team = 0;
		f.playerControlled = true;
		f.skills.toughness = kToughness;
		const auto spot = nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
		test::require(spot.has_value(), "no mesh near the Hub");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		state::Fighter* fighter = const_cast<state::Fighter*>(sim.combat()->find(id));
		test::require(fighter != nullptr, "no fighter");
		test::require(std::fabs(sim.combat()->rules().koPoint(*fighter) + 51.65) < 0.01, "the KO point at toughness 55.5337");
		state::FighterPart* leg = nullptr;
		for (state::FighterPart& p : fighter->parts)
		{
			if (p.collapses && (p.collapseBits & (1 | 16 | 32)) && !p.vital && !p.robotic)
			{
				leg = &p;
				break;
			}
		}
		test::require(leg != nullptr, "no collapsing leg part on the servant");

		// ---- a leg at -50 stands and walks; at -52 the character is down ----
		leg->health = -50.0;
		sim.run(1);
		test::require(!fighter->down && !fighter->unconscious, "a leg at -50 is not down");
		test::require(fighter->bleeding == 0.0, "an intact limb does not bleed");
		const double x0 = fighter->x;
		const double z0 = fighter->z;
		bool moved = false;
		for (std::uint32_t heading = 0; heading < 16 && !moved; ++heading)
		{
			sim.enqueue(Command::fighterDrive(state::Id{id}, 1, heading * 0.392699, 100 + heading));
			for (int t = 0; t < 40 && !moved; ++t)
			{
				sim.tick();
				moved = std::hypot(fighter->x - x0, fighter->z - z0) > 5.0;
			}
			sim.enqueue(Command::fighterDrive(state::Id{id}, 0, 0.0, 500 + heading));
			sim.run(2);
		}
		std::printf("a leg at -50 walked %.2f units\n", std::hypot(fighter->x - x0, fighter->z - z0));
		test::require(moved, "a leg at -50 still walks");

		// ---- a collapse fires the forced KO writer: 3.0 medical timer units, then awake, down stays (task BL, fb0248d) ----
		std::vector<CombatEvent> ragdolls;
		sim.setCombatObserver([&](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Ragdoll)
				ragdolls.push_back(e);
		});
		leg->health = -52.0;
		sim.run(1);
		test::require(fighter->down && fighter->unconscious, "a leg at -52 is down and KO'd (the collapse's forced KO)");
		test::require(std::fabs(fighter->koTimer - 3.0) < 1.0e-9,
			"the collapse's KO timer is 3.0 medical units, not " + std::to_string(fighter->koTimer));
		test::require(fighter->bleeding == 0.0, "the down character is not bleeding");
		test::require(ragdolls.size() == 1 && ragdolls.front().code == 0 && ragdolls.front().who == id,
			"the collapse sends one EVENT 48, cause 0 (a ragdoll start)");
		// 3.0 medical units at medicalClock 1.0 = 60 ticks; the body wakes with down still set (it crawls, posture 2).
		sim.run(65);
		test::require(!fighter->unconscious, "the collapse's KO ended on its 3.0 timer");
		test::require(fighter->down, "the body stays down after the collapse's KO");
		const double x1 = fighter->x;
		const double z1 = fighter->z;
		sim.enqueue(Command::fighterDrive(state::Id{id}, 1, 0.0, 900));
		sim.run(30);
		// A down character crawls on an order (task BF; kinspect bf_crawl_down.md: 5.0 u/s, down still set), and never at a
		// walking pace.
		const double crawled = std::hypot(fighter->x - x1, fighter->z - z1);
		test::require(fighter->down, "the down character's crawl cleared down");
		test::require(crawled > 0.5, "a down character crawls on a drive (moved " + std::to_string(crawled) + " u)");
		test::require(crawled <= 30 * kTickSeconds * CombatArena::kCrawlSpeed + 1.0, "the crawl is faster than kCrawlSpeed");

		// ---- the flag is recomputed: raising the health above the point stands him up at once ----
		leg->health = -40.0;
		sim.run(1);
		test::require(!fighter->down, "raising the health above the KO point clears the down flag");

		// ---- a severed limb is always down, fires the same writer (3.0 floor), and leaks (H + pool) x -0.002 ----
		leg->health = -10.0;
		leg->pool = 0.0;
		leg->maxHealth = state::FighterPart::kSeveredMax; // limb state 1
		sim.run(1);
		test::require(fighter->down && fighter->unconscious, "a severed leg is down and KO'd (the sever's forced KO)");
		test::require(std::fabs(fighter->koTimer - 3.0) < 0.06, "the sever's KO timer is the 3.0 floor");
		test::require(fighter->bleeding > 0.0, "a severed leg leaks");
		std::printf("ok  collapse: -50 walks, -52 is down with the 3.0 KO and crawls after it, a severed limb is down, KO'd and leaks\n");
	}
	catch (const test::Failure& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
