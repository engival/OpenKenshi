// A carrier the world selector cannot place never sits with the body for ever (task ds/dead-guy-r; CLAUDE.md "no layer may reach a
// terminal state"): a member holding a person or a corpse whose package has no placing row (106 for a person, 281 for a corpse) puts it
// down and cannot lift it again at once. 138's lift ends holding the corpse and is the same case: with no 281 row the body is put down.
// A bare world: the live selector and executors do the rest.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

constexpr double kDropSeconds = 4.0; // the selector's pass is every 0.4 s; a corpse goes at once
constexpr double kPersonSeconds = 35.0; // a living person waits 30 s (the KO'd-captive carries ride on, ksim_putdown_carried)

std::uint64_t spawnBody(Rig& rig, double x, double z, bool dead)
{
	const std::uint64_t id = rig.spawn(rig.body, x, z);
	state::Fighter& f = rig.fighterOf(id);
	f.team = 0;
	f.faction = 0;
	f.dead = dead;
	f.unconscious = !dead;
	f.koTimer = 1.0e6;
	return id;
}

// 138 lifts a corpse and, with no 281 row, puts it down; the same member does not lift it again.
void liftThenDrop()
{
	Rig rig({{routines::task::FindCorpses, 0, 24, 0, 0}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::FindCorpses), "no AI_PACKAGE carries 138");
	const std::uint64_t lifter = rig.member; // the rig's leader is the follow leader; the member scores
	const std::uint64_t corpse = spawnBody(rig, rig.fighterOf(lifter).x + 2.0, rig.fighterOf(lifter).z, true);
	const bool picked = rig.until(10.0, [&] { return rig.goalOf(lifter) == routines::task::FindCorpses; });
	test::require(picked, "138: the corpse was not picked");
	test::require(rig.until(15.0, [&] { return rig.fighterOf(lifter).carrying == corpse; }), "138: the corpse was not lifted");
	test::require(rig.until(kDropSeconds, [&] { return rig.fighterOf(lifter).carrying == 0; }),
		"138: the lifter kept the corpse with no placing row in the package");
	test::require(rig.fighterOf(corpse).carriedBy == 0, "138: the dropped corpse still reads as carried");
	test::require(rig.logged("(no placing row in the package)"), "138: the drop is not logged");
	int lifts = 0;
	rig.tick(static_cast<int>(10.0 / kTickSeconds));
	for (const std::string& line : rig.log)
		lifts += line.find("lifts body " + std::to_string(corpse)) != std::string::npos ? 1 : 0;
	test::require(lifts == 1, "138: the dropped corpse was lifted again at once (" + std::to_string(lifts) + " lifts)");
	std::cout << "  138: lifts a corpse, puts it down with no 281 row, no re-lift\n";
}

// A person in the arms and a package with no 106: put down too.
void personNoBed()
{
	Rig rig({{routines::task::FindCorpses, 0, 24, 0, 0}}, 0);
	const std::uint64_t carrier = rig.member;
	const std::uint64_t person = spawnBody(rig, rig.fighterOf(carrier).x, rig.fighterOf(carrier).z, false);
	rig.fighterOf(carrier).carrying = person;
	rig.fighterOf(person).carriedBy = carrier;
	test::require(rig.until(kPersonSeconds, [&] { return rig.fighterOf(carrier).carrying == 0; }), "a carrier with no placing row never put the person down");
	test::require(rig.fighterOf(person).carriedBy == 0, "the person still reads as carried");
	std::cout << "  a carrier with no 106 row puts the person down\n";
}

// 138's lift ends holding the corpse and the package has 281: the selector hands the carrier a cage of the town (the 112 chain with a dead
// body allowed), which keeps the body.
void liftThenCage()
{
	Rig rig({{routines::task::FindCorpses, 0, 24, 0, 0}, {routines::task::FindCageAndPutDeadGuyIn, 0, 24, 0, 0}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::FindCageAndPutDeadGuyIn), "no AI_PACKAGE carries 281");
	const BuildingSite* cage = findCageBuilding(rig);
	test::require(cage != nullptr, "no building with a cage layout exists");
	makeTown(rig, 2, cage->cx, cage->cz);
	const std::uint64_t lifter = rig.member;
	for (const std::uint64_t id : {rig.leader, lifter})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = cage->cx - 8.0 + (id == lifter ? 3.0 : 0.0);
		f.z = cage->cz;
		f.homeBuilding = cage->key;
	}
	const std::uint64_t corpse = spawnBody(rig, rig.fighterOf(lifter).x + 2.0, rig.fighterOf(lifter).z, true);
	test::require(rig.until(15.0, [&] { return rig.fighterOf(lifter).carrying == corpse; }), "281: the corpse was not lifted");
	test::require(rig.until(10.0, [&] { return rig.goalOf(lifter) == routines::task::FindCageAndPutDeadGuyIn; }),
		"281: the lifted corpse was not handed to the cage goal");
	rig.tick(40);
	test::require(rig.fighterOf(lifter).carrying == corpse, "281: the carrier let the corpse go");
	std::cout << "  138 -> 281: the lifted corpse is carried on to a cage\n";
}

} // namespace

int main()
{
	try
	{
		liftThenDrop();
		personNoBed();
		liftThenCage();
		std::cout << "ksim_carrier_drop: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_carrier_drop: FAILED: " << e.what() << "\n";
		return 1;
	}
}
