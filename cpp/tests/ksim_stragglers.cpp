// Stragglers (kenshi-k16.6.53; RUNTIME_SAVE.md v102, TOWN_LIFE_SLICE 5.14): a member left apart from its squad when the squad is demoted becomes a token of its
// own (state::StragglerOf), keeps its slot and its record, and
//   - when conscious walks to the parent token and merges within routines::kStragglerMerge, its wound kept; the squad promoted again has all its members;
//   - when knocked out and bleeding (Simulation::setOffscreenMedical on) dies where it lies and leaves a body there, the squad's own members untouched;
//   - survives a save and a load with its link (a v101 file, which has no link, is outside the loader's window since v103).

#include "delta_scene.hpp"

#include "ksim/combat/corpses.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/save/save.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdio>
#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace delta_scene;

std::shared_ptr<MeshNavigator> gNav;
const kworld::LoadOrder* gOrder = nullptr;
std::shared_ptr<const CombatData> gCombat;

constexpr std::uint64_t kMinute = 37; // a little over a game minute of ticks
constexpr double kApart = 100.0; // well past kStragglerRadius (60), and a step of the walk back (99 units) brings it within the merge range (30)

struct Case
{
	std::unique_ptr<Simulation> box;
	Simulation& sim;
	Scene scene;
	flecs::entity straggler; // scene.members[1]
};

// The squad is built beside the player, its second member is set down kApart from the squad, and the player leaves: the squad is a token.
Case prepare(bool medical, bool down = false)
{
	std::unique_ptr<Simulation> box = newGameBoxed(*gOrder, gCombat, gNav, 23);
	Simulation& sim = *box;
	sim.setOffscreenMedical(medical);
	Scene scene = pick(sim, 3);
	const flecs::entity member = scene.members[1];
	const state::Position at = *scene.squad.get<state::Position>();
	teleport(sim, idOf(member), at.x + kApart, at.z);
	if (down)
	{
		state::Fighter& f = fighterOf(sim, member);
		f.unconscious = true;
		f.koTimer = 1.0e6; // it does not wake before the squad is demoted
	}
	leave(sim, scene);
	return Case{std::move(box), sim, std::move(scene), member};
}

flecs::entity tokenOf(flecs::entity member)
{
	return member.target<state::MemberOf>();
}

std::size_t squadSize(Simulation& sim, flecs::entity squad)
{
	return membersOf(sim, squad).size();
}

void check(const Case& c)
{
	const flecs::entity token = tokenOf(c.straggler);
	test::require(token.is_valid() && token != c.scene.squad, "the straggler is in a token of its own");
	const state::StragglerOf* link = token.get<state::StragglerOf>();
	test::require(link != nullptr && link->parent.id == c.scene.squad.id(), "the token is linked to its parent");
	test::require(token.get<state::SquadInstance>()->templateSid == c.scene.squad.get<state::SquadInstance>()->templateSid, "the token has the parent's template");
	test::require(c.straggler.has<state::MemberSlot>() && c.straggler.get<state::MemberSlot>()->slot == 1, "the straggler keeps its slot");
	test::require(squadSize(const_cast<Simulation&>(c.sim), c.scene.squad) == c.scene.members.size() - 1, "the parent has one member fewer");
	const state::Position away = *token.get<state::Position>();
	const state::Position home = *c.scene.squad.get<state::Position>();
	test::require(std::hypot(away.x - home.x, away.z - home.z) > routines::kStragglerRadius, "the token stands where the member did");
}

void run()
{
	// A conscious straggler walks back and merges, its wound kept; the squad built again has everyone, the member with its stats and wound.
	{
		Case c = prepare(false);
		check(c);
		const flecs::entity token = tokenOf(c.straggler);
		state::MemberDelta wound;
		wound.mask = state::MemberDelta::kBody;
		wound.wounds = {0.05};
		wound.blood = 80.0;
		c.straggler.set<state::MemberDelta>(wound);
		// The player stays away: the merge is the token's walk (36 ticks a step).
		runUntil(c.sim, [&] { return tokenOf(c.straggler) == c.scene.squad; }, 20 * kMinute, "the straggler never merged back");
		test::require(!token.is_alive(), "the token is gone after the merge");
		test::require(squadSize(c.sim, c.scene.squad) == c.scene.members.size(), "the squad has everyone again");
		test::require(c.straggler.has<state::MemberDelta>(), "the record is kept through the merge");
		ret(c.sim, c.scene);
		const state::Fighter& back = fighterOf(c.sim, c.straggler);
		test::require(!back.wounds.empty() && back.blood <= 80.0, "the member is built again with its wound");
		std::cout << "  conscious straggler merges back, wound kept\n";
	}

	// A knocked-out bleeder lies where it is (it does not walk), bleeds out there and leaves a body at its own token; the squad is untouched.
	{
		Case c = prepare(true);
		check(c);
		const flecs::entity token = tokenOf(c.straggler);
		const state::Position lay = *token.get<state::Position>();
		const state::Fighter whole = state::Fighter(); // a record for a KO'd, bleeding member: set by hand on the stored member
		(void)whole;
		state::MemberDelta down;
		down.mask = state::MemberDelta::kBody;
		down.wounds = {40.0, 40.0};
		down.blood = 20.0;
		down.unconscious = true;
		c.straggler.set<state::MemberDelta>(down);
		runUntil(c.sim, [&] { return c.straggler.has<state::Fighter>() && c.straggler.get<state::Fighter>()->dead; }, 12 * kMinute, "the bleeding straggler never died");
		const flecs::entity now = tokenOf(c.straggler);
		test::require(now == token, "the dead straggler is still in its own token");
		const state::Position at = *token.get<state::Position>();
		test::require(std::hypot(at.x - lay.x, at.z - lay.z) < 1.0, "the token never walked");
		const state::Fighter& body = *c.straggler.get<state::Fighter>();
		test::require(std::hypot(body.x - lay.x, body.z - lay.z) < 1.0, "the body lies where the member was left");
		const state::CorpseDecay* decay = c.straggler.get<state::CorpseDecay>();
		test::require(decay != nullptr && decay->cause == state::CorpseDecay::kCauseBledOut, "the body is a corpse of a bleed-out");
		test::require(squadSize(c.sim, c.scene.squad) == c.scene.members.size() - 1, "the parent squad kept its other members");
		std::cout << "  knocked-out bleeder dies in place and leaves a corpse\n";
	}

	// Save and load keep the link.
	{
		Case c = prepare(false);
		check(c);
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_stragglers.ksav";
		c.sim.saveGame(path);
		{
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, *gOrder, path, false, nullptr, RegionConfig{});
			flecs::entity member;
			flecs::entity parent;
			ecsOf(loaded).each([&](flecs::entity e, const state::Id& id)
			{
				member = id.value == idOf(c.straggler) ? e : member;
				parent = id.value == idOf(c.scene.squad) ? e : parent;
			});
			test::require(member.is_valid() && parent.is_valid(), "the load has the member and its parent");
			const flecs::entity token = tokenOf(member);
			const state::StragglerOf* link = token.is_valid() ? token.get<state::StragglerOf>() : nullptr;
			test::require(link != nullptr && link->parent.id == parent.id() && token != parent, "the load keeps the link");
			test::require(member.has<state::MemberSlot>() && member.get<state::MemberSlot>()->slot == 1, "the load keeps the slot");
		}
		std::remove(path.c_str());
		std::cout << "  save / load keep the link\n";
	}

	// A member that is down, far away, with the medical step off: nothing would wake it, so it stays in the squad with its KO stored.
	{
		Case c = prepare(false, true);
		test::require(tokenOf(c.straggler) == c.scene.squad, "a KO'd member is not split off while the medical step is off");
		const state::MemberDelta* record = c.straggler.get<state::MemberDelta>();
		test::require(record != nullptr && record->unconscious, "its KO is kept in its record");
		test::require(squadSize(c.sim, c.scene.squad) == c.scene.members.size(), "the squad keeps everyone");
		std::cout << "  a KO'd member stays with the squad when the medical step is off\n";
	}
	// ... and with it on, the same member is split off (it lies where it is and the step wakes or kills it).
	{
		Case c = prepare(true, true);
		check(c);
		std::cout << "  a KO'd member splits when the medical step is on\n";
	}
	std::cout << "stragglers ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gOrder = &order;
		{
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, SpawnOptions{}, nullptr, RegionConfig{});
			gCombat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		run();
	} catch (const std::exception& e) {
		std::cerr << "ksim_stragglers: FAILED: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
