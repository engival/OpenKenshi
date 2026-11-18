// kenshi-k16.5.13: a world squad member with a Routine (a resident that walked in, then its squad is demoted) kept a moved-from Fighter in its
// MemberDelta: demoteWorldSquad read the member's Fighter through a pointer taken before `remove<Routine>` moved the member's components to another
// table, so the delta's weapon / name had empty strings with the old numbers. Promotion applied it: a fighter with a non-5 weaponType and an empty
// weapon name, which killed every simlink snapshot ("no WEAPON record named ").
// Invariant after a demote + promote: no fighter holds a weapon type other than 5 with an empty name, and each member keeps its template's weapon name.

#include "delta_scene.hpp"

#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace delta_scene;

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::shared_ptr<const CombatData> combat;
		{
			SpawnOptions options;
			options.seed = 23;
			options.wildernessByDensity = false;
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
			combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		Simulation sim = newGame(order, combat, nav, 23);
		Scene scene = pick(sim, 3);
		std::vector<std::string> weapon;
		for (const flecs::entity member : scene.members)
		{
			weapon.push_back(fighterOf(sim, member).weapon.name);
			member.set<state::Routine>(state::Routine{}); // the table move that invalidated the pointer
		}
		leave(sim, scene);
		ret(sim, scene);
		for (std::size_t k = 0; k < scene.members.size(); ++k)
		{
			const state::Fighter& f = fighterOf(sim, scene.members[k]);
			test::require(f.weapon.weaponType == 5 || !f.weapon.name.empty(), "no fighter holds a weapon type other than 5 with an empty name");
			test::require(f.weapon.name == weapon[k] && !f.name.empty(), "member " + std::to_string(k) + " keeps its weapon and name across the demotion");
		}
		std::cout << "demote weapon ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_demote_weapon: FAILED: " << e.what() << "\n";
		return 1;
	}
}
