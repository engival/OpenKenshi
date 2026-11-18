// The bar hangout (docs/reference/TOWN_LIFE_SLICE.md 5.17, phase T3d):
//   - the package `hang out in a bar` is the behaviour bar_hangout, and its squads are promoted like the other residents;
//   - one claimed table per squad, from a global claim set: tables are distinct, the book's claims are the squads' tables plus the members' sites;
//   - the BAR (designation 3) building of the town is tried first; when it is full the any-public-table rule runs; when that is out too the goal
//     drops out and the squad falls to the fallback (never a stall);
//   - the claim is given back on leave (demotion), and a save / load mid-stay continues exactly like the straight run;
//   - every member always has a goal (SHOPPING 118 is a real, list-gated transaction since task CH group 3: see ksim_shopping; the
//     patron templates here carry no `shopping` refs, so a member with no table falls to STAY_IN_HOME, TQ24's fallback replaced);
//   - a wandering trader staying in a town takes a table too (T2's minimal stay is replaced).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/economy/trader.hpp"
#include "knav/navmesh.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

// The Hub (TOWN 18919): its town point, where the bar squads stand.

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	// The Hub's bar recruit lists roll 0-1 characters each: the seed gives it at least 3 squads with someone to seat (12 until 2026-11-04,
	// when position binding re-rolled it to 2; 5 and 8 also give 2).
	options.seed = 1;
	options.wildernessByDensity = false;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

void teleport(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = const_cast<state::Fighter&>(*sim.combat()->find(id));
	f.x = x;
	f.z = z;
	f.clearRoute();
}

void centreOn(Simulation& sim, double x, double z)
{
	sim.enqueue(Command::setPlayerAreas({{x, z}}));
	sim.run(1);
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

// The bar_hangout squads standing at one town point that have someone to seat (a recruit list can roll no one: 45676, 0-1 random chars), Id order.
std::vector<flecs::entity> barSquads(Simulation& sim, double townX, double townZ)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id != 0 && registry.squad(instance.templateSid).behaviour == Behaviour::BarHangout
			&& std::hypot(at.x - townX, at.z - townZ) < 50.0 && !membersOf(sim, squad).empty())
			found.emplace_back(id.value, squad);
	});
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> squads;
	for (const auto& [id, squad] : found)
		squads.push_back(squad);
	return squads;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

std::uint64_t tableOf(flecs::entity squad)
{
	return squad.has<state::SquadRoutine>() ? squad.get<state::SquadRoutine>()->barTable : 0;
}

// The squads that hold a table and the book's claims: the same set, one table each, and nothing else in the book but the members' own sites.
std::size_t checkClaimSet(Simulation& sim, const std::string& when)
{
	std::size_t held = 0;
	std::set<std::uint64_t> keys;
	ecsOf(sim).each([&](const state::Id& id, const state::SquadRoutine& routine)
	{
		if (routine.barTable == 0)
			return;
		++held;
		keys.insert(routine.barTable);
		const routines::Site site = sim.routineBook().held(id.value);
		test::require(site.kind == routines::SiteKind::Table && site.object == routine.barTable, when + ": the book holds the squad's table for the squad");
	});
	test::require(keys.size() == held, when + ": every squad has a table of its own");
	std::size_t memberClaims = 0;
	ecsOf(sim).each([&](const state::Routine& routine) { memberClaims += routine.target.kind != 0 ? 1 : 0; });
	test::require(sim.routineBook().size() == held + memberClaims,
		when + ": the claim set is the squads' tables plus the members' sites (" + std::to_string(sim.routineBook().size()) + " vs " + std::to_string(held) + " + " + std::to_string(memberClaims) + ")");
	return held;
}

// ---- the Hub bar squads: one table each, release on leave ----

void hubBar(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	const std::vector<flecs::entity> squads = barSquads(sim, kHubX, kHubZ);
	test::require(squads.size() >= 3, "the Hub has bar_hangout squads (" + std::to_string(squads.size()) + ")");
	centreOn(sim, kHubX, kHubZ);
	runUntil(sim, [&] { return std::all_of(squads.begin(), squads.end(), [](flecs::entity s) { return s.has<state::SquadRoutine>(); }); }, 4000,
		"the Hub bar squads were never promoted");
	sim.run(60);
	const std::size_t holders = checkClaimSet(sim, "hub");
	const Simulation::TownScorerStats& stats = sim.townScorerStats();
	std::cout << "hub: " << squads.size() << " bar squads, " << holders << " tables held (bar rule " << stats.tablesBar << ", public rule "
		<< stats.tablesPublic << ", none " << stats.tablesNone << ")\n";
	test::require(stats.tablesBar >= 1, "the first claim is in the BAR building");
	std::size_t relaxing = 0;
	for (const flecs::entity squad : squads)
	{
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine == nullptr)
				continue;
			test::require(routine->goal != 0, "a bar member always has a goal");
			if (routine->goal == routines::task::RelaxInTownPackage)
			{
				++relaxing;
				test::require(tableOf(squad) != 0, "a relaxing squad holds its table");
			}
		}
	}
	test::require(relaxing > 0, "some bar member relaxes at a table");

	// Release on leave: the player goes far away, the squads are demoted, and every table is given back.
	centreOn(sim, kHubX + 40000.0, kHubZ + 40000.0);
	runUntil(sim, [&] { return std::none_of(squads.begin(), squads.end(), [](flecs::entity s) { return s.has<state::SquadRoutine>(); }); }, 4000,
		"the Hub bar squads were never demoted");
	const std::size_t left = checkClaimSet(sim, "after leave");
	for (const flecs::entity squad : squads)
		test::require(sim.routineBook().held(idOf(squad)).kind == routines::SiteKind::None, "a squad that left holds no table");
	std::cout << "release ok (" << holders << " claims before, " << left << " held elsewhere after)\n";
}

// ---- the three rules over towns with several bar squads ----

void fallThrough(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	std::map<std::pair<long, long>, std::vector<flecs::entity>> byTown;
	const BehaviourRegistry& registry = sim.ai()->registry();
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id != 0 && registry.squad(instance.templateSid).behaviour == Behaviour::BarHangout)
			byTown[{std::lround(at.x), std::lround(at.z)}].push_back(squad);
	});
	std::size_t towns = 0;
	for (const auto& [where, squads] : byTown)
	{
		if (squads.size() < 3 || towns >= 6)
			continue;
		++towns;
		centreOn(sim, static_cast<double>(where.first), static_cast<double>(where.second));
		sim.run(400);
		const std::size_t holders = checkClaimSet(sim, "town " + std::to_string(where.first));
		std::size_t members = 0;
		for (const flecs::entity squad : squads)
		{
			for (const flecs::entity member : membersOf(sim, squad))
			{
				const state::Routine* routine = member.get<state::Routine>();
				if (routine == nullptr)
					continue;
				++members;
				test::require(routine->goal != 0, "no member of an over-full bar is goal-less");
			}
		}
		std::cout << "town (" << where.first << ", " << where.second << "): " << squads.size() << " bar squads, " << holders << " tables held, " << members << " members with a goal\n";
	}
	test::require(towns > 0, "towns with several bar squads");
	const Simulation::TownScorerStats& stats = sim.townScorerStats();
	std::cout << "fall-through: bar rule " << stats.tablesBar << ", public rule " << stats.tablesPublic << ", no table " << stats.tablesNone
		<< ", fallbacks " << stats.fallbacks << ", blocked " << stats.blocked << "\n";
	test::require(stats.tablesBar > 0, "the BAR rule claims tables");
	// The fallback fires when no table is reachable: members of squads with no table fall to another goal, never stall. TQ24's fallback
	// was the pretend shopping walk; SHOPPING 118 is a real transaction since task CH group 3 and its list gates it (a CHARACTER's
	// `shopping` refs: Citizen, Drifter, caravans - not the bar patron templates of these towns), so the fallback is STAY_IN_HOME at the
	// home point (stats.fallbacks). The loop above checks every over-full member has a goal.
	test::require(stats.tablesNone == 0 || stats.fallbacks > 0, "with no table left a patron falls to another goal (the 118 list gate, task CH group 3)");
	test::require(stats.tablesPublic > 0 || stats.tablesNone > 0, "when the bar is full the any-public-table rule runs, then the goal drops out");
}

std::string describe(Simulation& sim, const std::vector<flecs::entity>& squads)
{
	std::string out;
	for (const flecs::entity squad : squads)
	{
		out += "squad " + std::to_string(idOf(squad)) + " table " + std::to_string(tableOf(squad)) + "\n";
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::Routine* r = member.get<state::Routine>();
			const state::Fighter* f = member.get<state::Fighter>();
			if (r != nullptr && f != nullptr)
				out += "  m " + std::to_string(idOf(member)) + " goal " + std::to_string(r->goal) + " since " + std::to_string(r->since) + " next " + std::to_string(r->nextScore)
					+ " hold " + std::to_string(r->holdUntil) + " at " + std::to_string(f->x) + "," + std::to_string(f->z) + " route " + std::to_string(f->route.size()) + "\n";
		}
	}
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::Routine& r)
	{
		const state::Fighter* f = e.get<state::Fighter>();
		out += "R " + std::to_string(id.value) + " g" + std::to_string(r.goal) + " s" + std::to_string(r.since) + " n" + std::to_string(r.nextScore) + " h" + std::to_string(r.holdUntil)
			+ " ls" + std::to_string(r.lastSleep) + " t" + std::to_string(r.target.kind) + " b" + std::to_string(r.blocked.size()) + (f ? " " + std::to_string(f->x) + "," + std::to_string(f->z) : "") + "\n";
	});
	ecsOf(sim).each([&](const state::Id& id, const state::TraderTrip& t, const state::GroupAI& g, const state::Position& at)
	{
		out += "T " + std::to_string(id.value) + " st" + std::to_string(t.staying) + " arr" + std::to_string(t.arrivals) + " goal" + std::to_string(int(g.goal)) + " until" + std::to_string(g.until)
			+ " route" + std::to_string(g.route.size()) + " along" + std::to_string(g.along) + " at " + std::to_string(at.x) + "," + std::to_string(at.z) + " since" + std::to_string(g.since) + " ls" + std::to_string(g.lastStep) + "\n";
	});
	ecsOf(sim).each([&](const state::Id& id, const state::SquadRoutine& r)
	{
		out += "S " + std::to_string(id.value) + " l" + std::to_string(r.homeLocked) + " t" + std::to_string(r.barTable) + "\n";
	});
	return out;
}

// ---- shopping, determinism, save / load ----

void shoppingAndSave(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_town_bar.save";
	Simulation straight = newGame(order);
	enable(straight, combat);
	const std::vector<flecs::entity> squads = barSquads(straight, kHubX, kHubZ);
	centreOn(straight, kHubX, kHubZ);
	runUntil(straight, [&] { return squads.front().has<state::SquadRoutine>(); }, 4000, "promoted before the save");
	straight.run(kTicksPerGameHour / 2);
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	std::map<std::uint16_t, std::uint64_t> seen;
	std::vector<std::uint64_t> trail;
	std::vector<std::string> notes;
	for (int hour = 0; hour < 6; ++hour)
	{
		for (std::uint64_t t = 0; t < kTicksPerGameHour; t += 100)
		{
			straight.run(std::min<std::uint64_t>(100, kTicksPerGameHour - t));
			trail.push_back(straight.stateHash());
			notes.push_back(describe(straight, squads));
		}
		for (const flecs::entity squad : squads)
		{
			for (const flecs::entity member : membersOf(straight, squad))
			{
				const state::Routine* routine = member.get<state::Routine>();
				if (routine == nullptr)
					continue;
				test::require(routine->goal != 0, "no goal-less member over six hours");
				++seen[routine->goal];
			}
		}
		checkClaimSet(straight, "hour " + std::to_string(hour));
	}
	std::cout << "goals seen over six hours:";
	for (const auto& [goal, count] : seen)
		std::cout << " " << goal << "x" << count;
	std::cout << "\n";
	// TQ24 (live: 30 h, every patron stayed on 214, 118 never appeared): a squad that holds a table stays on RELAX; shopping is only the fallback.
	test::require(straight.townScorerStats().byGoal.count(routines::task::RelaxInTownPackage) != 0, "the bar squads relax");
	for (const flecs::entity squad : squads)
		if (tableOf(squad) != 0)
			for (const flecs::entity member : membersOf(straight, squad))
				if (const state::Routine* routine = member.get<state::Routine>())
					test::require(routine->goal == routines::task::RelaxInTownPackage, "a patron with a reachable table stays on RELAX (goal " + std::to_string(routine->goal) + ")");

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	checkClaimSet(loaded, "after load");
	std::size_t k = 0;
	for (int hour = 0; hour < 6; ++hour)
	{
		for (std::uint64_t t = 0; t < kTicksPerGameHour; t += 100, ++k)
		{
			loaded.run(std::min<std::uint64_t>(100, kTicksPerGameHour - t));
			if (loaded.stateHash() != trail[k])
				std::cout << "STRAIGHT\n" << notes[k] << "LOADED\n" << describe(loaded, squads);
			test::require(loaded.stateHash() == trail[k], "save at N, load, run M equals the uninterrupted N + M (first difference in chunk " + std::to_string(k) + ")");
		}
	}
	test::require(loaded.stateHash() == straight.stateHash(), "save at N, load, run M equals the uninterrupted N + M");
	std::remove(path.c_str());
	std::cout << "save / load ok\n";
}

// ---- the wandering trader's stay ----

void traderStay(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	flecs::entity trader;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position&)
	{
		if (!trader.is_valid() && sim.ai()->registry().squad(instance.templateSid).behaviour == Behaviour::TradeCircuit && instance.homeTown.id != 0)
			trader = squad;
	});
	if (!trader.is_valid())
	{
		std::cout << "trader stay: no town-based trade-circuit squad in this world, skipped\n";
		return;
	}
	const state::TownState* town = ecsOf(sim).entity(trader.get<state::SquadInstance>()->homeTown.id).get<state::TownState>();
	trader.set<state::Position>(state::Position{town->x, town->z});
	sim.run(3 * kTicksPerGameHour);
	const std::uint64_t key = tableOf(trader);
	std::cout << "trader stay: table claim " << key << ", stays given a table " << sim.townScorerStats().traderTables << "\n";
	test::require(key == 0 || sim.routineBook().held(idOf(trader)).object == key, "a trader's table is in the book");
	checkClaimSet(sim, "trader");
}

// ---- the real Hub, with the world navmesh bake: the bar squads walk to the Bar's tables ----

void realHub(const std::string& world, const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	std::vector<kworld::ZoneCell> hubZones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(world, hubZones);
	SpawnOptions options;
	options.seed = 1;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	sim.enableNavigation(mesh);
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(ai));
	const std::vector<flecs::entity> squads = barSquads(sim, kHubX, kHubZ);
	test::require(squads.size() >= 3, "the Hub's bar squads");
	centreOn(sim, kHubX, kHubZ);
	runUntil(sim, [&] { return std::all_of(squads.begin(), squads.end(), [](flecs::entity s) { return s.has<state::SquadRoutine>(); }); }, 6000,
		"the real Hub's bar squads were never promoted");
	// Hour 13 (the client's live check: hours 12-14): run to it.
	while (std::fmod(trader::gameHours(sim.clockTick()), 24.0) < 13.0 || std::fmod(trader::gameHours(sim.clockTick()), 24.0) > 13.5)
		sim.run(kTicksPerGameHour / 12);
	std::size_t seatedNow = 0;
	std::size_t relaxingNow = 0;
	std::size_t inBar = 0;
	std::size_t tableless = 0;
	std::vector<std::pair<std::uint64_t, std::array<double, 2>>> spots;
	for (const flecs::entity squad : squads)
	{
		// k16.6.32: the bar's seated tables are few (3 here); a squad past them holds none and its members take another goal (no seatless table to stand round).
		if (tableOf(squad) == 0)
		{
			++tableless;
			continue;
		}
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::Routine* routine = member.get<state::Routine>();
			const state::Fighter* f = member.get<state::Fighter>();
			test::require(routine != nullptr && f != nullptr, "a Hub bar member is a Fighter with a routine");
			const double away = std::hypot(f->x - kHubX, f->z - kHubZ);
			std::cout << "  hour 13: squad " << idOf(squad) << " member " << idOf(member) << " goal " << routine->goal << " at (" << f->x << ", " << f->y << ", " << f->z << ") "
				<< away << " u from the town point\n";
			test::require(away > 100.0, "every bar member is off the town point at hour 13");
			test::require(routine->goal == routines::task::RelaxInTownPackage, "a bar member with a table stays on RELAX at hour 13 (TQ24)");
			// The Hub's Bar is the Storm House placement 3384 at (-51181, 2639) (zone GAMESTATE_BUILDING: squad template Bar blackmarket, layout Bar).
			// A squad the bar's seated tables have no room for (k16.6.32: no seatless table to stand round) takes a seated table of another public building.
			inBar += std::hypot(f->x + 51181.0, f->z - 2639.0) < 80.0 ? 1 : 0;
			for (const auto& [other, at] : spots)
				test::require(std::hypot(f->x - at[0], f->z - at[1]) >= 1.0, "each member has its own spot (member " + std::to_string(idOf(member)) + " vs " + std::to_string(other) + ")");
			double faceYaw = 0.0;
			if (sim.seated(idOf(member), &faceYaw))
			{
				++seatedNow;
				// A seated member looks at its table (atan2 from the chair to the table centre), not yaw 0.
				double off = std::fabs(f->yaw - faceYaw);
				off = std::min(off, 2.0 * 3.141592653589793 - off);
				test::require(off < 0.05, "a seated member faces its table (yaw " + std::to_string(f->yaw) + " vs " + std::to_string(faceYaw) + ")");
				// The pose tail: the seat's origin (beside the stand point), a finite facing and a resolved clip.
				const std::optional<Simulation::Pose> pose = sim.pose(idOf(member));
				test::require(pose.has_value() && !pose->clip.empty() && std::isfinite(pose->yaw), "a seated member has a pose with a clip");
				test::require(std::hypot(pose->x - f->x, pose->z - f->z) <= routines::kSeatTolerance, "the seat pose is the seat the member stands at");
				test::require(std::fabs(pose->y - f->y) < 15.0, "the seat pose is on the member's floor");
				if (seatedNow == 1)
					std::cout << "  seat pose: clip '" << pose->clip << "' yaw " << pose->yaw << " at (" << pose->x << ", " << pose->y << ", " << pose->z << ")\n";
			}
			++relaxingNow;
			spots.push_back({idOf(member), {f->x, f->z}});
		}
	}
	// SIMLINK flag 15: a member on RELAX at a layout seat is seated, one that is not at a seat (or walking) is not. A member with no seat in the data
	// (ring fallback) is never seated, so the count is of those whose table has seats.
	std::cout << "  hour 13: " << seatedNow << " of " << relaxingNow << " bar members are seated (at a layout seat)\n";
	std::cout << "  hour 13: " << tableless << " of " << squads.size() << " bar squads hold no table\n";
	test::require(tableless < squads.size() - 2, "the Hub's seated tables are held at hour 13");
	test::require(seatedNow >= 3, "bar members on RELAX at a layout seat are seated (flag 15)");
	test::require(inBar >= 6, "the bar members sit in the Hub's Bar (placement (-51181, 2639)): " + std::to_string(inBar) + " of " + std::to_string(relaxingNow));
	{
		const flecs::entity squad = squads.front();
		const flecs::entity member = membersOf(sim, squad).front();
		const state::Fighter* f = member.get<state::Fighter>();
		if (sim.seated(idOf(member)))
		{
			teleport(sim, idOf(member), f->x + 20.0, f->z);
			test::require(!sim.seated(idOf(member)), "a member 20 u from its seat is not seated");
		}
	}
	// The barman (Bar blackmarket, designation 3) works at the Bar (his home building is the placement that carries his template), not at the town point.
	std::size_t barmen = 0;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.templateSid != "44422-rebirth.mod" || std::hypot(at.x - kHubX, at.z - kHubZ) > 400.0)
			return;
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::Fighter* f = member.get<state::Fighter>();
			if (f == nullptr || f->dead)
				continue;
			++barmen;
			std::cout << "  hour 13: Bar blackmarket member " << idOf(member) << " at (" << f->x << ", " << f->z << ") building " << f->building << " home " << f->homeBuilding << "\n";
			test::require(f->homeBuilding != 0, "a Bar blackmarket member has the Bar as its home building");
			test::require(std::hypot(f->x + 51181.0, f->z - 2639.0) < 120.0, "the barman and his guards work at the Bar, not at the town point");
		}
	});
	test::require(barmen > 0, "the Hub has Bar blackmarket people");
	checkClaimSet(sim, "real Hub");
}

// ---- a busy northern city, with the world navmesh bake: its sleepers lie at their beds' use nodes, on their floors ----

// Heng (TOWN 1074-gamedata.base), the Traders Guild capital: the Hub is famously idle and poor, so the bed check lives here.
constexpr std::string_view kHengSid = "1074-gamedata.base";

void realHeng(const std::string& world, const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	SpawnOptions options;
	options.seed = 1;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	double townX = 0.0;
	double townZ = 0.0;
	bool found = false;
	ecsOf(sim).each([&](const state::TownState& town)
	{
		if (town.rootSid == kHengSid)
		{
			townX = town.x;
			townZ = town.z;
			found = true;
		}
	});
	test::require(found, "Heng is a town of the new game");
	const kworld::ZoneCell cell = kworld::zoneCellOf(townX, townZ);
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({cell.x + dx, cell.z + dz});
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	sim.enableNavigation(test::openWorldNav(world, zones));
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	const int rx = cell.x / knav::kRegionZones;
	const int rz = cell.z / knav::kRegionZones;
	ai.navigator = KnavNavigator::open(KENSHI_DATA, world, rx - 1, rz - 1, rx + 1, rz + 1, 4);
	ai.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(ai));
	std::cout << "real Heng: town point (" << townX << ", " << townZ << "), zone (" << cell.x << ", " << cell.z << ")\n";
	centreOn(sim, townX, townZ);
	// Over a day: everyone asleep on a real (layout) Bed site lies at its bed's use node and on the site's floor, not on the terrain under an
	// upstairs bed, nor parked where the walk to an unreachable bed ended (that bed is blocked and another taken).
	std::set<std::uint64_t> sleepers;
	std::size_t synthetic = 0;
	std::size_t upstairs = 0;
	for (std::uint64_t t = 0; t < 30 * kTicksPerGameHour; t += kTicksPerGameHour / 4)
	{
		sim.run(kTicksPerGameHour / 4);
		ecsOf(sim).each([&](flecs::entity member, const state::Routine& routine, const state::Fighter& f)
		{
			if (!f.asleep || routine.target.kind != static_cast<std::uint8_t>(routines::SiteKind::Bed) || std::hypot(f.x - townX, f.z - townZ) > 3000.0)
				return;
			const routines::SiteEntry* site = sim.routineSite(routine.target);
			test::require(site != nullptr, "a sleeper's bed is an indexed site");
			if (site->synthetic)
			{
				synthetic += sleepers.insert(idOf(member)).second;
				return;
			}
			const double off = std::hypot(f.x - site->x, f.z - site->z);
			const double ground = sim.groundHeight(f.x, f.z).value_or(f.y);
			if (sleepers.insert(idOf(member)).second)
			{
				upstairs += site->hasY && site->y > ground + 5.0;
				std::cout << "  hour " << std::fmod(trader::gameHours(sim.clockTick()), 24.0) << ": member " << idOf(member) << " asleep at (" << f.x << ", " << f.y
					<< ", " << f.z << "), " << off << " u from its bed node (floor " << (site->hasY ? site->y : f.y) << ", terrain " << ground << ")\n";
			}
			// Within the scorer's reach (4 u) and its floor reach (6 u), plus a little for the separation of the crowd.
			test::require(off < 5.0, "a sleeper lies at its bed's use node (" + std::to_string(off) + " u off)");
			test::require(!site->hasY || std::fabs(f.y - site->y) < 7.0, "a sleeper lies on its bed's floor (y " + std::to_string(f.y) + " vs " + std::to_string(site->y) + ")");
		});
	}
	std::cout << "  real Heng: " << sleepers.size() << " sleepers over 30 h, " << synthetic << " on synthetic beds, " << upstairs << " in an upstairs bed\n";
	test::require(sleepers.size() > synthetic, "someone sleeps in a real Heng bed over a day");
	checkClaimSet(sim, "real Heng");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation probe = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		test::require(behaviourName(Behaviour::BarHangout) == "bar_hangout", "behaviour name");
		hubBar(order, combat);
		fallThrough(order, combat);
		shoppingAndSave(order, combat);
		traderStay(order, combat);
		if (argc > 1 && test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
			{ realHub(argv[1], order, combat); realHeng(argv[1], order, combat); }
		else
			std::cout << "real Hub: skipped (pass the world navmesh bake directory)\n";
		std::cout << "ksim_town_bar: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_bar: FAILED: " << e.what() << "\n";
		return 1;
	}
}
