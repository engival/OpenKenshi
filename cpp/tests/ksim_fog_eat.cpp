// 139 EAT_A_RANDOM_CAGE_OCCUPANT and 237 FOGMAN_WORSHIP_VICTIM (task fog-eat; kinspect dt29_fog_kidnap.md sections 6 and 8, dt24_237.md).
// The prince (a Leader goal 139) takes the live caged victim and bites it with 220: the victim bleeds and carries the "being fed on"
// mark but stays in the world. A fogman (a Squad goal 237) takes the worship only once that mark exists, and then gathers at the cage;
// with no marked victim it never takes it. A bare world: one squad holds both roles, the cage and the victim are fixtures.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"

#include <cmath>
#include <stdexcept>


namespace {

using namespace ksim;
using namespace dt27;


double totalHealth(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.health;
	return sum;
}

const state::FighterPart& partOf(const state::Fighter& f, const char* name)
{
	for (const state::FighterPart& p : f.parts)
		if (p.name == name)
			return p;
	throw std::runtime_error(std::string("no part ") + name);
}

// The eat application itself (CombatArena::eat; dt39): reverse part order, sever then the next limb, XP and screams only while conscious.
void limbLoop()
{
	Rig rig({}, 0);
	CombatArena& arena = *const_cast<CombatArena*>(rig.sim.combat());
	const std::uint64_t id = rig.spawn(rig.body, 50.0, 50.0);
	state::Fighter& v = rig.fighterOf(id);
	v.skills.toughness = 20.0;
	v.unconscious = true; // knocked out but alive: no per-application XP, no scream
	CombatArena::EatState st;
	const double toughness = v.skills.toughness;
	bool rightLegGone = false;
	std::size_t applications = 0;
	CombatArena::EatOutcome out;
	while (applications < 100000)
	{
		out = arena.eat(id, false, 0.05, st);
		++applications;
		test::require(!out.screamed, "eat: a knocked-out victim screamed");
		if (!rightLegGone && partOf(v, "Right Leg").severed())
		{
			rightLegGone = true;
			test::require(!partOf(v, "Left Leg").severed() && partOf(v, "Left Leg").health < partOf(v, "Left Leg").maxHealth,
				"eat: the left leg is not the next part after the right leg's sever");
			test::require(partOf(v, "Right Arm").health == partOf(v, "Right Arm").maxHealth, "eat: an arm was touched before both legs");
			const auto severs = arena.takeSevers();
			test::require(severs.size() == 1 && severs[0].part == "Right Leg", "eat: the right leg's sever was not reported");
			test::require(v.skills.toughness > toughness, "eat: the sever gave no toughness XP");
		}
		if (out.result != CombatArena::EatResult::Applied)
			break;
	}
	test::require(rightLegGone, "eat: the right leg never severed first");
	test::require(out.result == CombatArena::EatResult::Eaten, "eat: the body was never marked eaten");
	for (const char* limb : {"Right Leg", "Left Leg", "Right Arm", "Left Arm"})
		test::require(partOf(v, limb).severed(), std::string("eat: ") + limb + " not severed at the end");
	std::cout << "  eat: right leg first, sever, then the left leg; removal after the last part (" << applications << " applications)\n";
}

void awakeVictimScreamsAndTrains()
{
	Rig rig({}, 0);
	CombatArena& arena = *const_cast<CombatArena*>(rig.sim.combat());
	const std::uint64_t id = rig.spawn(rig.body, 50.0, 50.0);
	state::Fighter& v = rig.fighterOf(id);
	v.skills.toughness = 20.0;
	CombatArena::EatState st;
	const CombatArena::EatOutcome out = arena.eat(id, false, 0.05, st);
	test::require(out.screamed, "eat: a conscious victim did not scream");
	test::require(v.skills.toughness > 20.0, "eat: a conscious victim got no per-application toughness XP");
	std::cout << "  eat: a conscious victim screams and trains\n";
}

void drainPath()
{
	Rig rig({}, 0);
	CombatArena& arena = *const_cast<CombatArena*>(rig.sim.combat());
	const std::uint64_t id = rig.spawn(rig.body, 50.0, 50.0);
	state::Fighter& v = rig.fighterOf(id);
	v.unconscious = true;
	const double blood = v.blood;
	for (const char* name : {"Spider", "Small Spider", "Skimmer", "Land Bat"})
	{
		const RaceCombat* race = arena.data().race(name);
		test::require(race != nullptr && race->vampiric, std::string("RACE ") + name + " is not vampiric");
	}
	// RACE `water avoidance` -> the routing water cost scale 1 + avoidance (PATHFINDING_STATIC.md item 4): human 7.0, Fishman 0.25.
	test::require(std::fabs(arena.data().race("Greenlander")->waterCost - 7.0) < 1e-6, "Greenlander water cost is not 7.0");
	test::require(std::fabs(arena.data().race("Fishman")->waterCost - 0.25) < 1e-6, "Fishman water cost is not 0.25 (" + std::to_string(arena.data().race("Fishman")->waterCost) + ")");
	test::require(!arena.data().race("Beak Thing")->vampiric, "RACE Beak Thing is vampiric");
	CombatArena::EatState st;
	arena.eat(id, true, 1.0, st);
	test::require(v.blood < blood && st.drain < 0.0, "eat: the drain path did not take blood");
	test::require(totalHealth(v) == totalHealth(rig.fighterOf(id)) && partOf(v, "Right Leg").health == partOf(v, "Right Leg").maxHealth,
		"eat: the drain path damaged a part");
	st.drain = -0.5;
	std::size_t n = 0;
	while (arena.eat(id, true, 0.05, st).result == CombatArena::EatResult::Applied)
		test::require(++n < 20, "eat: the drain path never ended");
	test::require(n == 6, "eat: removal is not the seventh application past the drain limit");
	std::cout << "  eat: blood drain path, removal after six applications\n";
}

// The victim in the cage, everyone beside it (the cage interior is not on the bare rig's walkable mesh, so the fogman starts within reach), the building the squad's home (the ownership test's second arm, as ksim_eat_kill does).
std::uint64_t lockVictim(Rig& rig, std::uint64_t fogman = 0)
{
	const BuildingSite* cage = findCageBuilding(rig);
	test::require(cage != nullptr, "no building with a cage layout exists");
	for (const std::uint64_t id : {rig.leader, rig.member, fogman})
	{
		if (id == 0)
			continue;
		state::Fighter& f = rig.fighterOf(id);
		f.x = cage->cx + (id == fogman ? 10.0 : 0.0);
		f.z = cage->cz + (id == fogman ? 0.0 : id == rig.member ? 8.0 : 6.0);
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
		f.homeBuilding = cage->key;
	}
	const std::uint64_t captive = rig.spawn(rig.body, cage->cx, cage->cz);
	state::Fighter& victim = rig.fighterOf(captive);
	victim.y = rig.sim.groundHeight(victim.x, victim.z).value_or(victim.y);
	victim.cageBuilding = cage->key;
	victim.cageSlot = 0;
	return captive;
}

void princeEatsFogmenWorship()
{
	// rig.member is the prince (Leader list); the fogman is a third member on the Squad list.
	Rig rig({{routines::task::EatRandomCageOccupant, 0, 24, 0, 2}, {routines::task::FogmanWorshipVictim, 0, 24, 0, 0}}, 2);
	test::require(dataHasRow(*rig.gd, routines::task::EatRandomCageOccupant), "no AI_PACKAGE carries 139");
	test::require(dataHasRow(*rig.gd, routines::task::FogmanWorshipVictim), "no AI_PACKAGE carries 237");
	const std::uint64_t fogman = rig.spawn(rig.body, 0.0, 0.0);
	rig.join(fogman, 0);
	const std::uint64_t captive = lockVictim(rig, fogman);
	const double health = totalHealth(rig.fighterOf(captive));

	// Before the first bite there is no marked victim: the fogman never takes 237, whatever the leader does.
	test::require(rig.until(15.0, [&]
		{
			test::require(rig.goalOf(fogman) != routines::task::FogmanWorshipVictim || rig.logged("eat 139"),
				"237: the fogman took the worship before any bite");
			return rig.logged("eat 139");
		}),
		"139: the prince never bit the caged victim [" + [&] { std::string all; for (const std::string& l : rig.log) all += l + " | "; return all; }() + "]");
	test::require(rig.goalOf(rig.member) == routines::task::EatRandomCageOccupant, "139: the prince bites without holding the goal");
	test::require(rig.until(10.0, [&] { return rig.goalOf(fogman) == routines::task::FogmanWorshipVictim; }),
		"237: the fogman did not join the worship after the first bite");
	test::require(rig.until(10.0, [&] { return rig.logged("worship 237 member"); }), "237: the fogman never gathered at the cage");
	const state::Fighter& victim = rig.fighterOf(captive);
	test::require(std::hypot(rig.fighterOf(fogman).x - victim.x, rig.fighterOf(fogman).z - victim.z) <= kInteractReach + 1.0,
		"237: the fogman is not at the cage");
	rig.tick(60);
	test::require(rig.exists(captive) && !rig.fighterOf(captive).dead, "139: the live victim left the world or died");
	test::require(totalHealth(rig.fighterOf(captive)) < health, "139: the victim's limbs were not eaten");
	test::require(rig.sim.dialogueEventCount(0x0b) > 0, "139: the conscious victim did not scream (SCREAMING_TORTURE)");
	test::require(rig.goalOf(fogman) == routines::task::FogmanWorshipVictim, "237: the fogman left the cage while the prince eats");
	std::cout << "  139 / 237: the prince bit the live victim (kept in the world, bleeding); the fogman worshipped only after\n";
}

// 282 EAT_A_RANDOM_CAGE_OCCUPANT_MEASURED_RATE is 139's finder and goal (dt24_killprisoner section 1); "measured rate" has no reading, so it runs
// 139's bite loop unchanged.
void measuredRateIsTheSameEater()
{
	Rig rig({{routines::task::EatRandomCageOccupantMeasured, 0, 24, 0, 2}}, 2);
	test::require(dataHasRow(*rig.gd, routines::task::EatRandomCageOccupantMeasured), "no AI_PACKAGE carries 282");
	const std::uint64_t captive = lockVictim(rig);
	const double health = totalHealth(rig.fighterOf(captive));
	test::require(rig.until(15.0, [&] { return rig.logged("eat 282 member"); }), "282: the eater never bit the caged victim");
	rig.tick(60);
	test::require(rig.exists(captive) && totalHealth(rig.fighterOf(captive)) < health, "282: the live victim was not eaten in the world");
	std::cout << "  282: the measured-rate goal bites like 139\n";
}

void noMarkNoWorship()
{
	Rig rig({{routines::task::FogmanWorshipVictim, 0, 24, 0, 0}}, 0);
	const std::uint64_t captive = lockVictim(rig, rig.member);
	rig.tick(400);
	test::require(rig.exists(captive), "237: the victim vanished");
	test::require(!rig.logged("goal FOGMAN_WORSHIP_VICTIM"), "237: a fogman took the worship of an unmarked victim");
	std::cout << "  237: a fogman with no marked victim never takes the worship\n";
}

} // namespace

int main()
{
	try
	{
		limbLoop();
		awakeVictimScreamsAndTrains();
		drainPath();
		princeEatsFogmenWorship();
		measuredRateIsTheSameEater();
		noMarkNoWorship();
		std::cout << "ksim_fog_eat: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_fog_eat: FAILED: " << e.what() << "\n";
		return 1;
	}
}
