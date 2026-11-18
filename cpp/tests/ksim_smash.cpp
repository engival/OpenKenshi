// 272 SMASH_BUILDING (task ds/arrest; dt40_tasks_289_290): the actor walks to a player building and bashes it every kBashInterval seconds with
// Task_BashDoor's blow, clamp(2 blunt + 0.5 cut, 20, 60), through removeHp until it is down. A bare world.

#include "dt27_rig.hpp"

#include "ksim/buildings.hpp"
#include "ksim/interact.hpp"
#include "ksim/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

std::uint64_t makeSite(Rig& rig, double hp)
{
	state::PlayerBuilding b;
	b.x = kX + 6.0;
	b.z = kZ;
	b.y = rig.fighterOf(rig.member).y;
	b.built = true;
	b.hp = hp;
	b.maxHp = hp;
	b.status = buildings::statusOf(b);
	flecs::entity entity = state::createInstance(ecsOf(rig.sim));
	const std::uint64_t id = entity.get<state::Id>()->value;
	entity.set<state::PlayerBuilding>(std::move(b));
	return id;
}

const state::PlayerBuilding& siteOf(Rig& rig, std::uint64_t id)
{
	const state::PlayerBuilding* found = nullptr;
	ecsOf(rig.sim).each([&](const state::Id& key, const state::PlayerBuilding& b)
	{
		if (key.value == id)
			found = &b;
	});
	test::require(found != nullptr, "no such building");
	return *found;
}

}

int main()
{
	try
	{
		Rig rig({}, 0);
		test::require(!rig.sim.orderSmash(rig.member, 424242ull), "272: a building that does not exist was accepted");
		const std::uint64_t site = makeSite(rig, 2.0);
		test::require(rig.sim.orderSmash(rig.member, site), "272: the order was refused");
		rig.tick(static_cast<int>(kBashInterval / kTickSeconds) + 2);
		const double afterOne = siteOf(rig, site).hp;
		test::require(afterOne < 2.0 && !siteOf(rig, site).built && siteOf(rig, site).broken, "272: the first blow did not land (the building is still built)");
		const double blow = 1.0 - afterOne; // a building at max HP first drops to max - 1 (removeHp)
		const state::Fighter& hand = rig.fighterOf(rig.member);
		const double want = bashDamage(hand.weapon.blunt1, hand.weapon.cut1) / 0.015 / buildings::kRateDivisor; // 20..60 raw, then the rate
		test::require(std::abs(blow - want) < 1.0e-6, "272: a blow is clamp(2 blunt + 0.5 cut, 20, 60) x the rate: got " + std::to_string(blow) + " want " + std::to_string(want));
		test::require(rig.until(120.0, [&] { return rig.sim.smashOrderCount() == 0; }), "272: the order never ended");
		test::require(siteOf(rig, site).ruin && siteOf(rig, site).hp == 0.0, "272: a smashed building is not a ruin");
		std::cout << "272: blows of " << blow << " hp every " << kBashInterval << " s, down to a ruin\n";
		std::cout << "ksim_smash: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_smash: FAILED: " << e.what() << "\n";
		return 1;
	}
}
