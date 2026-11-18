// Task port-standing: the standing goals of the generic path - 24 WANDERER (the nearest town, walk in), 19 GO_HOMEBUILDING (the home building),
// 30 HOLD_POSITION (the order's place, then stand, no finish) and 62 STAND_STILL (stop, no finish). None is a package goal in the data, so the
// test names the goal through Simulation::orderStandingGoal (the finder's own answer). A bare dt27 world: no navmesh, so a "walk" is the test
// moving the member to where the goal sends it.

#include "dt27_rig.hpp"

#include "ksim/routines.hpp"
#include "ksim/towncircle.hpp"

#include "../src/ksim/world_resolvers.hpp"

namespace {

using namespace ksim;
using namespace dt27;

void moveTo(Rig& rig, double x, double z)
{
	state::Fighter& f = rig.fighterOf(rig.member);
	f.x = x;
	f.z = z;
}

// 24 (140998040): the nearest town circle, a marker (radius 0) never; none when there is no town.
void wandererFinder()
{
	const std::vector<towncircle::Circle> circles = {{0.0, 0.0, 0.0}, {1000.0, 0.0, 2500.0}, {300.0, 0.0, 2500.0}, {-5000.0, 0.0, 437.5}};
	test::require(worldres::wanderer24(circles, 100.0, 0.0) == 2, "24: not the nearest town");
	test::require(worldres::wanderer24(circles, -4000.0, 0.0) == 3, "24: not the nearest town to the west");
	test::require(worldres::wanderer24({{0.0, 0.0, 0.0}}, 1.0, 1.0) == -1, "24: a marker is a town");
	test::require(worldres::wanderer24({}, 1.0, 1.0) == -1, "24: a town with no towns");
	std::cout << "  24: the finder takes the nearest town, never a marker\n";
}

void wanderer()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(!rig.sim.orderStandingGoal(rig.member, routines::task::Wanderer), "24: ordered with no town");
	const state::Fighter& at = rig.fighterOf(rig.member);
	ecsOf(rig.sim).entity().set<state::TownState>(state::TownState{"", "", state::EntityRef{}, at.x + 4000.0, at.z}); // a town 4000 u east (radius 2500)
	test::require(rig.sim.orderStandingGoal(rig.member, routines::task::Wanderer), "24: the finder named no town");
	test::require(rig.goalOf(rig.member) == routines::task::Wanderer, "24: no Routine carries the goal");
	rig.tick(40);
	test::require(rig.goalOf(rig.member) == routines::task::Wanderer, "24: the goal ended before the member was in a town");
	// Walk it into the town the finder chose: the nearest circle's centre.
	std::vector<towncircle::Circle> circles;
	ecsOf(rig.sim).each([&](const state::TownState& town) { circles.push_back(towncircle::of(*rig.gd, town)); });
	const state::Fighter& f = rig.fighterOf(rig.member);
	const towncircle::Circle town = circles[static_cast<std::size_t>(worldres::wanderer24(circles, f.x, f.z))];
	moveTo(rig, town.x, town.z);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) == 0; }), "24: the goal did not end in the town");
	test::require(rig.logged("standing 24 member"), "24: no log line");
	std::cout << "  24: ordered, held until the member stood in the nearest town, then ended\n";
}

void homeBuilding()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(!rig.sim.orderStandingGoal(rig.member, routines::task::GoHomeBuilding), "19: ordered with no home building");
	test::require(!rig.sim.world().buildingSites().empty(), "19: the world has no building site");
	const BuildingSite& home = rig.sim.world().buildingSites().front();
	rig.fighterOf(rig.member).homeBuilding = home.key;
	test::require(rig.sim.orderStandingGoal(rig.member, routines::task::GoHomeBuilding), "19: the finder named no home building");
	rig.tick(40);
	test::require(rig.goalOf(rig.member) == routines::task::GoHomeBuilding, "19: the goal ended before the member was home");
	moveTo(rig, home.cx, home.cz);
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) == 0; }), "19: the goal did not end at the home building");
	test::require(rig.logged("standing 19 member"), "19: no log line");
	std::cout << "  19: walked to the home building and ended there\n";
}

void holdPosition()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const state::Fighter& f = rig.fighterOf(rig.member);
	test::require(rig.sim.orderStandingGoal(rig.member, routines::task::HoldPosition, f.x + 300.0, f.z), "30: the order named no place");
	rig.tick(60);
	test::require(rig.goalOf(rig.member) == routines::task::HoldPosition, "30: the goal ended before the member was there");
	moveTo(rig, f.x + 300.0, f.z);
	rig.tick(200);
	test::require(rig.goalOf(rig.member) == routines::task::HoldPosition, "30: Task_Blank has no finish, the hold ended");
	rig.fighterOf(rig.member).dead = true; // a death ends it
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != routines::task::HoldPosition; }), "30: a death did not end the hold");
	std::cout << "  30: walked to the order's place, stood with no finish, ended by a death\n";
}

void standStill()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(rig.sim.orderStandingGoal(rig.member, routines::task::StandStill), "62: the order failed");
	state::Fighter& f = rig.fighterOf(rig.member);
	f.route = {{f.x + 50.0, f.y, f.z}};
	rig.tick(40);
	test::require(rig.fighterOf(rig.member).route.empty(), "62: the route was not stopped");
	test::require(rig.goalOf(rig.member) == routines::task::StandStill, "62: no finish flag, yet the goal ended");
	rig.fighterOf(rig.member).dead = true;
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != routines::task::StandStill; }), "62: a death did not end it");
	std::cout << "  62: stopped the movement, held, ended by a death\n";
}

} // namespace

int main()
{
	try
	{
		wandererFinder();
		wanderer();
		homeBuilding();
		holdPosition();
		standStill();
		std::cout << "ksim_standing: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_standing: FAILED: " << e.what() << "\n";
		return 1;
	}
}
