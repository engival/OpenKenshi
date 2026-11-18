// The cannibals' corpse chain on a world squad (task ds/cannibal-pot; kinspect dt39_eaten_alive section 4, dt24_killprisoner):
// the kill - 135 KILL_A_RANDOM_CAGE_OCCUPANT, the cannibal packages' row; 196 BEAT_CAGE_OCCUPANT is the same class, but no AI_TASK 196 exists in
// this install's data, so no package can list it and the fixture cannot carry it - kills the caged prisoner (Task_KillPrisoner) -> 137 DEAD_GUYS_GO_IN_THE_POT, whose fetch half
// is 138's pick (the dead occupant of a town cage) and whose machine is the function-11 argmax -> carry.cpp's chain lifts the body,
// walks it to the machine and 136 FEED_CORPSE_INTO_MACHINE removes it ("incinerated or cooked"; no item). Bare world: the squad, the
// cage's prisoner and the machine are the data's own placements (a cage building and a disposal machine near each other); no mesh,
// so the walks are the straight legs of stepCorpsePot and the kill's approach is done by standing the killer at the cage.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

void chain()
{
	{
		Rig data({}, 0);
		test::require(dataHasRow(*data.gd, routines::task::KillRandomCageOccupant), "no AI_PACKAGE carries 135");
		test::require(dataHasRow(*data.gd, routines::task::DeadGuysInThePot), "no AI_PACKAGE carries 137");
	}
	Rig rig({{routines::task::KillRandomCageOccupant}, {routines::task::DeadGuysInThePot}}, 0);
	// A cage building with a disposal machine close by (the squad needs both in one place).
	const DisposalSite* pot = nullptr;
	const BuildingSite* cage = nullptr;
	double bestD = 3000.0;
	for (const DisposalSite& site : rig.sim.world().disposalSites())
	{
		const BuildingSite* near = findLayoutBuilding(rig, routines::SiteKind::Cage, true, site.x, site.z);
		if (near != nullptr && std::hypot(near->cx - site.x, near->cz - site.z) < bestD)
		{
			bestD = std::hypot(near->cx - site.x, near->cz - site.z);
			pot = &site;
			cage = near;
		}
	}
	test::require(pot != nullptr && cage != nullptr, "no cage building within 3000 u of a disposal machine in the data");
	std::cout << "  fixture: cage " << cage->key << " and machine " << pot->recordSid << " " << fmt(bestD) << " u apart\n";

	// The squad's faction owns the machine (or the machine has no owner).
	if (!pot->ownerFaction.empty())
	{
		state::Allegiance side;
		side.factionSid = pot->ownerFaction;
		for (const std::uint64_t id : {rig.leader, rig.member})
			rig.entityOf(id).set<state::Allegiance>(side);
	}
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = cage->cx;
		f.z = cage->cz + (id == rig.member ? 2.0 : 6.0);
		f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
		f.homeBuilding = cage->key;
	}
	armMember(rig);
	const std::uint64_t captive = rig.spawn(rig.body, cage->cx, cage->cz);
	state::Fighter& victim = rig.fighterOf(captive);
	victim.y = rig.sim.groundHeight(victim.x, victim.z).value_or(victim.y);
	victim.cageBuilding = cage->key;
	victim.cageSlot = 0;
	for (state::FighterPart& part : victim.parts)
		part.health = std::min(part.health, 1.0); // a blow ends it: the case is the chain, not the damage model

	test::require(rig.until(10.0, [&] { return rig.goalOf(rig.member) == routines::task::KillRandomCageOccupant; }),
		"135: the armed member did not take KILL_A_RANDOM_CAGE_OCCUPANT for the caged prisoner");
	test::require(rig.until(60.0, [&] { return rig.fighterOf(captive).dead; }), "135: the prisoner was never beaten dead");
	test::require(rig.logged("kill 135 member"), "135: no kill line was logged");
	test::require(rig.until(15.0, [&] { return rig.goalOf(rig.member) == routines::task::DeadGuysInThePot; }),
		"137: the member did not take DEAD_GUYS_GO_IN_THE_POT for the corpse");
	test::require(rig.logged("pot " + std::to_string(pot->key)) || rig.logged("pot "), "137: the pick did not log the machine");
	// A bare world has no navigator, so the walks are driven by hand: the member to the body, then (carrying) to the machine.
	const auto standAt = [&](double x, double z)
	{
		state::Fighter& m = rig.fighterOf(rig.member);
		m.x = x;
		m.z = z;
		m.y = rig.sim.groundHeight(x, z).value_or(m.y);
		m.clearRoute();
	};
	standAt(rig.fighterOf(captive).x + 1.0, rig.fighterOf(captive).z);
	test::require(rig.until(30.0, [&] { return rig.fighterOf(rig.member).carrying == captive; }), "138: the corpse was never lifted");
	test::require(rig.fighterOf(captive).cageBuilding == 0, "138: the lifted body still holds its cage slot");
	standAt(pot->x, pot->z + 1.0);
	test::require(rig.until(30.0, [&] { return !rig.exists(captive); }), "136: the corpse never went into the machine");
	test::require(rig.logged("corpse 136 feed member"), "136: no feed line was logged");
	test::require(rig.logged("incinerated or cooked"), "136: the body was not removed as incinerated or cooked");
	test::require(rig.fighterOf(rig.member).carrying == 0, "136: the feeder still holds something");
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == 0 || rig.goalOf(rig.member) == routines::task::KillRandomCageOccupant; }),
		"137: the feeder kept the goal after the feed");
	std::cout << "  135 -> 138 -> 137 -> 136: prisoner beaten dead, carried to the machine and removed\n";
}

} // namespace

int main()
{
	try
	{
		chain();
		std::cout << "ksim_cannibal_pot: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_cannibal_pot: FAILED: " << e.what() << "\n";
		return 1;
	}
}
