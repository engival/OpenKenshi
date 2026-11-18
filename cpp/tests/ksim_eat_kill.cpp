// 230 EAT_A_RANDOM_DEAD_BODY and 135 KILL_A_RANDOM_CAGE_OCCUPANT (task dt27; kinspect dt27_eat_hunt_slavetrade.md sections 2-3).
// 230: a member whose applied package carries the row finds the nearest sensed corpse, walks to it (20 u), and bites once per tick; the
// body is removed as "eaten" after the counter's six bites. A robot-race body is refused (the executor's race test) and an armed eater
// sheathes first. 135: a member holding a weapon takes the occupant of a cage it owns and lands one blow per "chop down static" pass
// until the occupant is dead; an unarmed member does not offer the goal. A bare world: the squad, the corpse and the cage are
// fixtures, the live selector and executors do the rest (no orders to NPCs).

#include "dt27_rig.hpp"

#include "ksim/routines.hpp"


namespace {

using namespace ksim;
using namespace dt27;


std::uint64_t spawnCorpse(Rig& rig, double x, double z)
{
	const std::uint64_t id = rig.spawn(rig.body, x, z);
	state::Fighter& f = rig.fighterOf(id);
	f.dead = true;
	for (state::FighterPart& part : f.parts)
		part.health = -part.maxHealth; // every part is already past its eat threshold: the bites only count (the 5x damage is ksim_fog_eat's)
	return id;
}

void eatsCorpse()
{
	{
		Rig data({}, 0); // the data carries the rows: wolves' packages list 230, the Cannibals' list 135
		test::require(dataHasRow(*data.gd, routines::task::EatRandomDeadBody), "no AI_PACKAGE carries 230");
		test::require(dataHasRow(*data.gd, routines::task::KillRandomCageOccupant), "no AI_PACKAGE carries 135");
	}
	Rig rig({{routines::task::EatRandomDeadBody}}, 0);
	const std::uint64_t corpse = spawnCorpse(rig, kX + 12.0, kZ + 4.0);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::EatRandomDeadBody; }),
		"230: the member did not take EAT_A_RANDOM_DEAD_BODY for a corpse in sight");
	test::require(rig.until(10.0, [&] { return !rig.exists(corpse); }), "230: the corpse was never eaten");
	test::require(rig.logged("eat 230 member"), "230: no eat line was logged");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) == 0; }), "230: the eater kept the goal after the last bite");
	std::cout << "  230: the corpse was walked to, bitten and removed\n";
}

// 233 EAT_A_RANDOM_KO_BODY (dt39 section 5): the same executor on a knocked-out living body.
void eatsKoBody()
{
	Rig rig({{routines::task::EatRandomKoBody}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::EatRandomKoBody), "no AI_PACKAGE carries 233");
	const std::uint64_t body = rig.spawn(rig.body, kX + 12.0, kZ + 4.0);
	rig.fighterOf(body).unconscious = true;
	rig.fighterOf(body).koTimer = 600.0; // stays down for the case
	test::require(rig.until(8.0, [&] { return rig.goalOf(rig.member) == routines::task::EatRandomKoBody; }),
		"233: the member did not take EAT_A_RANDOM_KO_BODY for a knocked-out body in sight [" + [&] { std::string all; for (const std::string& l : rig.log) all += l + " | "; return all; }() + "]");
	test::require(rig.until(8.0, [&] { return rig.logged("eat 233 member"); }), "233: the member never bit the knocked-out body");
	std::cout << "  233: a knocked-out living body is walked to and bitten\n";
}

void armedEaterSheathes()
{
	Rig rig({{routines::task::EatRandomDeadBody}}, 0);
	armMember(rig);
	test::require(rig.fighterOf(rig.member).weapon.sid != rig.data->unarmed.sid, "the fixture did not arm the member");
	const std::uint64_t corpse = spawnCorpse(rig, kX + 12.0, kZ + 4.0);
	test::require(rig.until(15.0, [&] { return !rig.exists(corpse); }), "230: the armed eater never finished the corpse");
	test::require(rig.fighterOf(rig.member).weapon.sid == rig.data->unarmed.sid, "230: the eater bit with its weapon in hand (no sheathe)");
	std::cout << "  230: an armed eater sheathed first\n";
}

void robotCorpseRefused()
{
	Rig rig({{routines::task::EatRandomDeadBody}}, 0);
	const std::uint64_t corpse = spawnCorpse(rig, kX + 12.0, kZ + 4.0);
	for (state::FighterPart& part : rig.fighterOf(corpse).parts)
		part.robotic = true;
	rig.tick(400); // 20 s: several selector passes and the 20 s block between them
	test::require(rig.logged("goal EAT_A_RANDOM_DEAD_BODY member"), "230: the finder never offered the robot corpse (the case would prove nothing)");
	test::require(rig.exists(corpse), "230: a robot-race corpse was eaten");
	std::cout << "  230: a robot-race corpse is refused\n";
}

void killsCageOccupant()
{
	Rig rig({{routines::task::KillRandomCageOccupant}}, 2);
	const BuildingSite* cage = findCageBuilding(rig);
	test::require(cage != nullptr, "no building with a cage layout exists");
	// Everyone beside the cage; the building is the member's home (the ownership test's second arm).
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = cage->cx;
		f.z = cage->cz + (id == rig.member ? 2.0 : 6.0);
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
		f.homeBuilding = cage->key;
	}
	const std::uint64_t captive = rig.spawn(rig.body, cage->cx, cage->cz);
	state::Fighter& victim = rig.fighterOf(captive);
	victim.y = rig.sim.groundHeight(victim.x, victim.z).value_or(victim.y);
	victim.cageBuilding = cage->key;
	victim.cageSlot = 0;
	for (state::FighterPart& part : victim.parts)
		part.health = std::min(part.health, 1.0); // a blow ends it: the case is the executor, not the damage model

	// Unarmed: 134's precondition is a weapon in hand - the goal is not offered.
	rig.fighterOf(rig.member).weapon = rig.data->unarmed;
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != routines::task::KillRandomCageOccupant, "135: an unarmed member took the kill goal");
	test::require(!rig.fighterOf(captive).dead, "135: the captive died with nobody armed");

	armMember(rig);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::KillRandomCageOccupant; }),
		"135: the armed member did not take KILL_A_RANDOM_CAGE_OCCUPANT for the cage's occupant");
	test::require(rig.until(60.0, [&] { return rig.fighterOf(captive).dead; }), "135: the occupant was never killed");
	test::require(rig.logged("kill 135 member"), "135: no kill line was logged");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) == 0; }), "135: the killer kept the goal after the kill");
	std::cout << "  135: the armed member took the cage's occupant and killed it\n";
}

} // namespace

int main()
{
	try
	{
		eatsCorpse();
		eatsKoBody();
		armedEaterSheathes();
		robotCorpseRefused();
		killsCageOccupant();
		std::cout << "ksim_eat_kill: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_eat_kill: FAILED: " << e.what() << "\n";
		return 1;
	}
}
