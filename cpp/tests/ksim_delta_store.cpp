// The member delta store (docs/formats/RUNTIME_SAVE.md v99, kenshi-k16.6.51): what a world squad's members keep while the squad is a token.
//   - a neutral world squad is built beside the player; one member is wounded and bleeding, one knocked out, one enslaved, one looted
//     and trained; the player leaves (the squad is demoted), the world runs, the player returns (the squad is promoted): each of them
//     is as it was, with no route, target or task, and an untouched member carries no record and is exactly its template build;
//   - a save of the demoted squad with its records loads to the same hash, and the loaded world promotes the same members.
// A save of the previous version (kSaveVersion - 1) loads: the older-version read is exercised.

#include "delta_scene.hpp"

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

constexpr std::size_t kHurt = 3; // the first three members of the subject are hurt, looted or enslaved; the rest and the control are not

// The persistent subset of a Fighter, as the record keeps it.
struct Kept
{
	std::vector<state::FighterPart> parts;
	double blood = 0.0;
	std::vector<double> wounds;
	double koTimer = 0.0;
	bool unconscious = false;
	std::uint8_t slaveState = 0;
	std::string captorSid;
	std::string weaponSid;
	std::vector<std::string> armour;
	double strength = 0.0;
	bool operator==(const Kept&) const = default;
};

Kept keptOf(const state::Fighter& f)
{
	Kept k{f.parts, f.blood, f.wounds, f.koTimer, f.unconscious, f.slaveState, f.captorSid, f.weapon.sid, {}, f.skills.strength};
	for (const state::FighterArmour& a : f.armour)
		k.armour.push_back(a.sid);
	return k;
}

void cleanSlate(const state::Fighter& f, const std::string& what)
{
	test::require(f.route.empty() && f.target == 0 && f.orderTarget == 0 && f.followTarget == 0 && f.drive == state::DriveMode::Stop,
		what + " has no route, target or task after the promotion");
}

// The record's state is exact; what runs in detail is not: the bleeding member goes on bleeding and the knocked out one's timer runs down, in the
// ticks on the way out of detail and since the promotion (a token's members do not bleed or wake: that is k16.6.52).
void sameAsBefore(Kept now, const Kept& was, std::size_t k, const std::string& what)
{
	if (k == 0)
	{
		test::require(now.blood <= was.blood && now.blood > was.blood - 10.0 && !now.wounds.empty(), "the bleeding member goes on bleeding");
		test::require(now.parts.size() == was.parts.size(), "the parts are kept");
		now.blood = was.blood;
		now.parts = was.parts;
		now.wounds = was.wounds;
	}
	if (k == 1)
	{
		test::require(now.unconscious && now.koTimer <= was.koTimer && now.koTimer > was.koTimer - 10.0, "the knocked out member is still out");
		now.koTimer = was.koTimer;
		now.blood = was.blood;
		now.parts = was.parts;
	}
	test::require(now == was, what);
}

void run(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, gNav, 23);
	Scene scene = pick(sim, 3);
	std::cout << "squad of " << scene.members.size() << "\n";

	// The subject: 0 wounded, bleeding, looted and trained; 1 knocked out; 2 enslaved; the others and the control squad are left alone.
	state::Fighter& wounded = fighterOf(sim, scene.members[0]);
	test::require(!wounded.parts.empty(), "a member has body parts");
	wounded.parts[0].health -= 37.0;
	wounded.parts.back().health -= 11.0;
	wounded.blood -= 41.5;
	wounded.wounds = {0.25, 0.5};
	state::Fighter& knocked = fighterOf(sim, scene.members[1]);
	knocked.unconscious = true;
	knocked.koTimer = 40.0;
	state::Fighter& slave = fighterOf(sim, scene.members[2]);
	slave.slaveState = 1;
	slave.captorSid = "delta-store captor";
		state::Fighter& looted = wounded;
	looted.weapon = combat->unarmed;
	looted.armour.clear();
	looted.skills.strength += 6.5;
	looted.skills.katana += 3.0;

	std::vector<Kept> before;
	for (const flecs::entity member : scene.members)
		before.push_back(keptOf(fighterOf(sim, member)));
	std::vector<Kept> controlBefore;
	for (const flecs::entity member : scene.control)
		controlBefore.push_back(keptOf(fighterOf(sim, member)));

	leave(sim, scene);
	for (std::size_t k = 0; k < scene.members.size(); ++k)
		test::require(scene.members[k].has<state::MemberDelta>() == (k < kHurt), "a hurt member stores a record when its squad is demoted, an untouched one nothing");
	for (const flecs::entity member : scene.control)
		test::require(!member.has<state::MemberDelta>(), "an untouched member stores nothing");
	{
		const state::MemberDelta& record = *scene.members[0].get<state::MemberDelta>();
		// The wounded one bled on its way out of detail (the ticks between the wounding and the demotion): its blood is a little lower.
		test::require(record.parts.size() == before[0].parts.size() && record.blood <= before[0].blood && record.blood > before[0].blood - 5.0 && !record.wounds.empty()
			&& record.weapon.sid == before[0].weaponSid && record.skills.strength == before[0].strength, "the record holds the member's state");
	}
	std::cout << "stored: " << scene.members[0].get<state::MemberDelta>()->parts.size() << " parts and mask "
		<< static_cast<int>(scene.members[0].get<state::MemberDelta>()->mask) << " for the wounded and looted one, " << static_cast<int>(scene.members[1].get<state::MemberDelta>()->mask)
		<< " for the knocked out, " << static_cast<int>(scene.members[2].get<state::MemberDelta>()->mask) << " for the enslaved\n";

	// A save of the demoted squad keeps the records.
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_delta_store_test.ksav";
	sim.saveGame(path);
	// The same world as a file of the previous version: it loads with every record kept.
	{
		const std::string old = std::string(KSIM_BINARY_DIR) + "/ksim_delta_store_prev.ksav";
		writeSaveFile(old, sim.world().ecs(), sim.world().loadOrder(), {}, kSaveVersion - 1);
		Simulation previous = Simulation::loadGame(KENSHI_DATA, order, old, false, nullptr, RegionConfig{});
		for (const flecs::entity member : scene.members)
		{
			flecs::entity twin;
			ecsOf(previous).each([&](flecs::entity e, const state::Id& id) { twin = id.value == idOf(member) ? e : twin; });
			test::require(twin.is_valid() && twin.has<state::MemberDelta>() == member.has<state::MemberDelta>(), "a save of the previous version loads with each member's record");
		}
		std::remove(old.c_str());
	}
	sim.run(300);

	ret(sim, scene);
	for (std::size_t k = 0; k < scene.members.size(); ++k)
	{
		const state::Fighter& f = fighterOf(sim, scene.members[k]);
		test::require(!scene.members[k].has<state::MemberDelta>(), "a promoted member's record is spent");
		sameAsBefore(keptOf(f), before[k], k, "member " + std::to_string(k) + " is as it was across the demotion");
		cleanSlate(f, "member " + std::to_string(k));
	}
	for (std::size_t k = 0; k < scene.control.size(); ++k)
	{
		test::require(!scene.control[k].has<state::MemberDelta>() && keptOf(fighterOf(sim, scene.control[k])) == controlBefore[k], "an untouched member is its template build");
		cleanSlate(fighterOf(sim, scene.control[k]), "an untouched member");
	}

	// A second round trip with nothing changed in between writes nothing new: the Fighters are the state, no record is stale.
	leave(sim, scene);
	for (std::size_t k = 0; k < scene.members.size(); ++k)
		test::require(scene.members[k].has<state::MemberDelta>() == (k < kHurt), "the same members store a record again");
	for (const flecs::entity member : scene.control)
		test::require(!member.has<state::MemberDelta>(), "the untouched ones still store nothing");

	// The saved one: loaded, it promotes the same members.
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	CombatSettings settings;
	settings.substeps = 2;
	loaded.enableCombat(combat, settings);
	loaded.enableNavigation(gNav);
	std::vector<flecs::entity> twins;
	for (const flecs::entity member : scene.members)
	{
		flecs::entity twin;
		ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { twin = id.value == idOf(member) ? e : twin; });
		test::require(twin.is_valid() && twin.has<state::MemberDelta>() == (twins.size() < kHurt), "the load keeps each member's record");
		twins.push_back(twin);
	}
	Scene there = scene;
	there.members = twins;
	there.squad = twins.front().target<state::MemberOf>();
	ret(loaded, there);
	for (std::size_t k = 0; k < twins.size(); ++k)
		sameAsBefore(keptOf(fighterOf(loaded, twins[k])), before[k], k, "member " + std::to_string(k) + " is as it was across save, load and promotion");
	std::remove(path.c_str());
	std::cout << "delta store ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::shared_ptr<const CombatData> combat;
		{
			SpawnOptions options;
			options.seed = 23;
			options.wildernessByDensity = false;
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
			combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		run(order, combat);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_delta_store: FAILED: " << e.what() << "\n";
		return 1;
	}
}
