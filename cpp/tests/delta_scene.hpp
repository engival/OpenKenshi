// Shared by ksim_delta_store and ksim_delta_untouched (kenshi-k16.6.51): a new game at the Hub, a neutral world squad built beside the
// player, and the walk away (the squad demoted to a token) and back (promoted again).
#pragma once

#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "sim_access.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace delta_scene {

using namespace ksim;

inline std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

inline void startGame(Simulation& sim, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav)
{
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	sim.enableNavigation(nav);
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	test::require(!sid.empty(), "the data has the Wanderer start-off");
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(3);
}

inline Simulation newGame(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav,
	std::uint64_t seed)
{
	SpawnOptions options;
	options.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
	options.seed = seed;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	startGame(sim, combat, nav);
	return sim;
}

// A game that is kept where it was made (a Simulation does not survive a move once combat is on): for a test that holds several.
inline std::unique_ptr<Simulation> newGameBoxed(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat,
	const std::shared_ptr<MeshNavigator>& nav, std::uint64_t seed)
{
	SpawnOptions options;
	options.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
	options.seed = seed;
	options.wildernessByDensity = false;
	std::unique_ptr<Simulation> sim(new Simulation(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}))); // built in place: no move
	startGame(*sim, combat, nav);
	return sim;
}

inline std::uint64_t playerId(const Simulation& sim)
{
	for (const CombatArena::Member& member : sim.combat()->members())
	{
		if (member.fighter.playerControlled)
			return member.id;
	}
	test::require(false, "the new game has a player character");
	return 0;
}

inline void teleport(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = const_cast<state::Fighter&>(*sim.combat()->find(id));
	f.x = x;
	f.z = z;
	f.y = sim.groundHeight(x, z).value_or(f.y);
	f.clearRoute();
	f.building = 0;
}

inline state::Fighter& fighterOf(Simulation& sim, flecs::entity member)
{
	state::Fighter* f = const_cast<state::Fighter*>(sim.combat()->find(idOf(member)));
	test::require(f != nullptr, "the member is in the arena");
	return *f;
}

inline void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks && !done(); ++i)
		sim.run(1);
	test::require(done(), what);
}

inline std::size_t inArena(const Simulation& sim, const std::vector<flecs::entity>& members)
{
	return static_cast<std::size_t>(std::count_if(members.begin(), members.end(), [&](flecs::entity m) { return sim.combat()->find(idOf(m)) != nullptr; }));
}

struct Scene
{
	std::uint64_t player = 0;
	double x = 0.0; // where the player starts (the squads are built around it)
	double z = 0.0;
	double farX = 0.0; // where the player goes to leave
	double farZ = 0.0;
	flecs::entity squad; // the subject
	std::vector<flecs::entity> members;
	std::vector<flecs::entity> control; // the members of another built world squad, nearest the subject, which nothing is done to
};

// The subject: the largest world squad built now (every member an alive Fighter, none a town resident) of at least `least` members whose
// faction is not at war with the player (one at war would fight at once); a faction-less squad is taken, its members are strangers to all.
// The control is the nearest other such squad. Two worlds of the same seed give the same pick.
inline Scene pick(Simulation& sim, std::size_t least)
{
	Scene scene;
	scene.player = playerId(sim);
	const state::Fighter& start = *sim.combat()->find(scene.player);
	scene.x = start.x;
	scene.z = start.z;
	flecs::entity player;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { player = e; });
	// The town populate leaves the Hub with residents and singles only, so the scene sets two wilderness squads down beside the start (inside
	// the build radius): the nearest homeless ones of a faction that is not at war with the player, no merchants (they keep trade circuits),
	// the subject of at least `least` members, the control of any size.
	struct Homeless
	{
		double d;
		flecs::entity squad;
		std::size_t size;
	};
	std::vector<Homeless> homeless;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& pos)
	{
		if (instance.source != state::SpawnSource::BiomeHomeless || instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id)
			|| factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) < 0 || trader::isMerchant(*sim.gameData()->find(instance.templateSid)))
			return;
		homeless.push_back({std::hypot(pos.x - scene.x, pos.z - scene.z), squad, membersOf(sim, squad).size()});
	});
	std::sort(homeless.begin(), homeless.end(), [](const Homeless& a, const Homeless& b) { return a.d != b.d ? a.d < b.d : idOf(a.squad) < idOf(b.squad); });
	const auto subjectAt = std::find_if(homeless.begin(), homeless.end(), [&](const Homeless& h) { return h.size >= least && h.size <= 8; });
	test::require(subjectAt != homeless.end(), "a wilderness squad of " + std::to_string(least) + " or more to set down at the Hub");
	const auto controlAt = std::find_if(homeless.begin(), homeless.end(), [&](const Homeless& h) { return h.squad != subjectAt->squad && h.size >= 2 && h.size <= 8; });
	test::require(controlAt != homeless.end(), "a second wilderness squad to set down at the Hub");
	sim.moveSquad(subjectAt->squad, scene.x + 250.0, scene.z);
	sim.moveSquad(controlAt->squad, scene.x + 250.0, scene.z + 40.0);
	sim.run(200);
	struct Built
	{
		flecs::entity squad;
		std::vector<flecs::entity> members;
	};
	std::vector<Built> built;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		if (instance.source == state::SpawnSource::Dismissed)
			return;
		if (instance.faction.id != 0 && ecsOf(sim).is_alive(instance.faction.id)
			&& factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) <= FactionBook::kHostile)
			return;
		std::vector<flecs::entity> members = membersOf(sim, squad);
		if (members.empty() || inArena(sim, members) != members.size()
			|| std::any_of(members.begin(), members.end(), [](flecs::entity m) { return m.has<state::Routine>(); }))
			return;
		built.push_back({squad, std::move(members)});
	});
	const auto at = [](const Built& b) { return *b.squad.get<state::Position>(); };
	std::sort(built.begin(), built.end(), [](const Built& a, const Built& b) { return idOf(a.squad) < idOf(b.squad); });
	const Built* subject = nullptr;
	for (const Built& b : built)
	{
		if (b.members.size() >= least && (subject == nullptr || b.members.size() > subject->members.size()))
			subject = &b;
	}
	test::require(subject != nullptr, "a built world squad of " + std::to_string(least) + " or more beside the player");
	const Built* control = nullptr;
	for (const Built& b : built)
	{
		if (&b != subject && (control == nullptr || std::hypot(at(b).x - at(*subject).x, at(b).z - at(*subject).z)
			< std::hypot(at(*control).x - at(*subject).x, at(*control).z - at(*subject).z)))
			control = &b;
	}
	test::require(control != nullptr, "a second built world squad to leave alone");
	scene.squad = subject->squad;
	scene.members = subject->members;
	scene.control = control->members;
	double best = 1.0e18;
	ecsOf(sim).each([&](const state::SquadInstance&, const state::Position& pos)
	{
		const double d = std::hypot(pos.x - scene.x, pos.z - scene.z);
		if (d >= 14000.0 && d < best)
		{
			best = d;
			scene.farX = pos.x;
			scene.farZ = pos.z;
		}
	});
	test::require(best < 1.0e17, "a far point to leave to");
	return scene;
}

// The player leaves: the squads are demoted to tokens (no member has a Fighter).
inline void leave(Simulation& sim, const Scene& scene)
{
	teleport(sim, scene.player, scene.farX, scene.farZ);
	runUntil(sim, [&] { return inArena(sim, scene.members) + inArena(sim, scene.control) == 0; }, 3000, "the squads were never demoted");
}

// The player returns to the subject's token: every member is built again.
inline void ret(Simulation& sim, const Scene& scene)
{
	const state::Position at = *scene.squad.get<state::Position>();
	teleport(sim, scene.player, at.x + 3.0, at.z);
	runUntil(sim, [&] { return inArena(sim, scene.members) == scene.members.size() && inArena(sim, scene.control) == scene.control.size(); }, 3000, "the squads were never built again");
}

} // namespace delta_scene
