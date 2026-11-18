// A member's build slot (kenshi-k16.6.55; RUNTIME_SAVE.md v101): the template build is keyed on the slot the member was spawned with, not its place in the
// squad's list. A world squad of one template is built beside the player; member 3 is wounded; the player leaves (the squad is a token); member 1 is destroyed
// (a corpse that decayed, a capture); the player returns: members 3 and 4 come back with their own stats and gear, 3 with its wound.

#include "delta_scene.hpp"

#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace delta_scene;

std::shared_ptr<MeshNavigator> gNav;

struct Person
{
	state::FighterSkills skills;
	std::string weapon;
	std::vector<std::string> armour;
	bool operator==(const Person&) const = default;
};

Person personOf(const state::Fighter& f)
{
	Person p{f.skills, f.weapon.sid, {}};
	for (const state::FighterArmour& a : f.armour)
		p.armour.push_back(a.sid);
	return p;
}

void run(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, gNav, 23);
	Scene scene = pick(sim, 5);
	std::cout << "squad of " << scene.members.size() << "\n";
	const std::vector<flecs::entity> all = scene.members;
	std::vector<Person> before;
	for (const flecs::entity member : all)
		before.push_back(personOf(fighterOf(sim, member)));
	test::require(before[3] != before[2] && before[4] != before[2] && before[3] != before[1], "the members differ, so a shifted slot shows");

	state::Fighter& wounded = fighterOf(sim, all[3]);
	wounded.wounds = {0.25};
	wounded.blood -= 30.0;
	const double blood = wounded.blood;
	for (const flecs::entity member : all)
		test::require(member.has<state::MemberSlot>(), "a spawned member has its slot");

	leave(sim, scene);
	test::require(all[3].has<state::MemberDelta>(), "the wounded member stored a record");
	all[1].destruct();
	const state::Position at = *scene.squad.get<state::Position>();
	teleport(sim, scene.player, at.x + 3.0, at.z);
	runUntil(sim, [&] { return inArena(sim, {all[0], all[2], all[3], all[4]}) == 4; }, 3000, "the squad was never built again");

	test::require(personOf(fighterOf(sim, all[3])) == before[3], "member 3 has its own stats and gear after a squadmate left");
	test::require(personOf(fighterOf(sim, all[4])) == before[4], "member 4 has its own stats and gear after a squadmate left");
	test::require(personOf(fighterOf(sim, all[2])) == before[2], "member 2 too");
	const state::Fighter& back = fighterOf(sim, all[3]);
	test::require(!back.wounds.empty() && back.blood <= blood && back.blood > blood - 10.0, "member 3 comes back with its wound");
	std::cout << "member slot ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		SpawnOptions options;
		options.seed = 23;
		options.wildernessByDensity = false;
		Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const std::shared_ptr<const CombatData> combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		run(order, combat);
	} catch (const std::exception& e) {
		std::cerr << "ksim_member_slot: FAILED: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
