// The shared plan steps 7 UNEQUIP_WEAPON and 6 EQUIP_WEAPON (task ds/sheathe; GATES_CARRY_STATIC "peaceful tasks"):
//   - a resident that takes a bed sheathes first (USE_BED needs hasWeaponEquipped = 0), so the town sleeps with empty hands;
//   - the 275 SIT_AROUND sitter sheathes through the same step;
//   - an attacked sleeper draws again as it wakes (the plan's 6 before the fight).
// The weapon recheck of a running bed (a sleeper handed a weapon wakes) is in ksim_get_out_of_bed.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/goap.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "dt27_rig.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
	options.seed = 12;
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

flecs::entity squadIn(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::SquadInstance&) { found = i.value == id ? e : found; });
	test::require(found.is_valid(), "the squad exists in this sim");
	return found;
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
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

void centreOn(Simulation& sim, const std::vector<std::array<double, 2>>& at)
{
	std::vector<state::GroundPoint> areas;
	for (const auto& p : at)
		areas.push_back({p[0], p[1]});
	sim.enqueue(Command::setPlayerAreas(areas));
	sim.run(1);
}

state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

// A home-bound resident squad whose people can all be built.
struct Pick
{
	flecs::entity squad;
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	std::set<std::uint16_t> tasks;
	std::size_t members = 0;
	bool leader = false; // a member spawned from the template's `leader` list (it runs the Leader list: the shop, the secure bed)
	std::string behaviour;
	std::string package;
};

std::vector<Pick> residentSquads(Simulation& sim, const routines::PackageTable& packages, const CombatData& combat)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	const kdata::GameData& data = *sim.gameData();
	std::vector<Pick> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id == 0 || instance.source != state::SpawnSource::TownResident)
			return;
		const Mapping& mapping = registry.squad(instance.templateSid);
		if (mapping.behaviour != Behaviour::ShopKeeper && mapping.behaviour != Behaviour::ResidentLife)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		Pick pick;
		pick.squad = squad;
		pick.id = id.value;
		pick.x = at.x;
		pick.z = at.z;
		pick.behaviour = behaviourName(mapping.behaviour);
		pick.package = mapping.package;
		for (const auto& list : package->lists)
			for (const routines::GoalRow& row : list)
				pick.tasks.insert(row.task);
		found.push_back(std::move(pick));
	});
	std::sort(found.begin(), found.end(), [](const Pick& a, const Pick& b) { return a.id < b.id; });
	std::vector<Pick> usable;
	for (Pick& candidate : found)
	{
		const std::vector<flecs::entity> members = membersOf(sim, candidate.squad);
		if (members.empty() || members.size() > 6)
			continue;
		bool buildable = true;
		for (const flecs::entity member : members)
		{
			const state::MemberRole* role = member.get<state::MemberRole>();
			candidate.leader = candidate.leader || (role != nullptr && role->list == state::kRoleLeader);
			std::uint64_t dice = 1;
			try
			{
				combat.makeFighter(data, member.get<state::CharacterInstance>()->templateSid, "", dice, candidate.squad.get<state::SquadInstance>()->templateSid);
			} catch (const std::exception&) {
				buildable = false;
			}
		}
		if (buildable)
		{
			candidate.members = members.size();
			usable.push_back(std::move(candidate));
		}
	}
	return usable;
}

bool isBed(std::uint16_t goal)
{
	return goal == routines::task::GoHomeToBed || goal == routines::task::GoHomeToBedSecure;
}

// A WEAPON record sheathed on the back (Inventory::kSheathed, k16.6.124) or carried in the pack, not in the hands.
bool carriesWeapon(Simulation& sim, flecs::entity who)
{
	const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
	const state::Inventory* pack = who.get<state::Inventory>();
	if (pack == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : pack->items)
		if (entry.slot == state::Inventory::kCarried || entry.slot == state::Inventory::kSheathed)
			if (const kdata::Record* record = sim.gameData()->find(entry.itemSid); record != nullptr && record->type == weaponType)
				return true;
	return false;
}

// 1 and 3: the town sleeps with empty hands, and an attacked sleeper draws again.
void sleepers(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(2, 1, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	std::vector<const Pick*> chosen;
	for (const Pick& pick : picks)
		if ((pick.tasks.count(153) != 0 || pick.tasks.count(239) != 0) && chosen.size() < 10)
			chosen.push_back(&pick);
	test::require(chosen.size() >= 4, "resident squads with a bed goal (" + std::to_string(chosen.size()) + ")");
	std::vector<std::array<double, 2>> at;
	for (const Pick* pick : chosen)
		at.push_back({pick->x, pick->z});
	centreOn(sim, at);
	runUntil(sim, [&] { return std::all_of(chosen.begin(), chosen.end(), [&](const Pick* p) { return squadIn(sim, p->id).has<state::SquadRoutine>(); }); },
		4000, "the squads were never promoted");
	runUntil(sim, [&] {
		for (const Pick* pick : chosen)
			for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
			{
				const state::Routine* r = member.get<state::Routine>();
				if (r != nullptr && fighterOf(sim, idOf(member)).asleep && isBed(r->goal))
					return true;
			}
		return false;
	}, 4000, "no resident was ever asleep in a bed");

	// ---- 1: every bed sleeper's hands are empty; the ones that were armed carry the weapon in the pack ----
	flecs::entity sheathed;
	std::size_t sleepers = 0;
	for (const Pick* pick : chosen)
		for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
		{
			const state::Routine* r = member.get<state::Routine>();
			if (r == nullptr || !fighterOf(sim, idOf(member)).asleep || !isBed(r->goal))
				continue;
			++sleepers;
			test::require(!sim.weaponInHands(idOf(member)), "sleeper " + std::to_string(idOf(member)) + " lies in bed with a weapon in the hands");
			if (!sheathed.is_valid() && carriesWeapon(sim, member))
				sheathed = member;
		}
	test::require(sleepers > 0, "some residents sleep");
	test::require(sheathed.is_valid(), "a sleeper carries the weapon it sheathed for the bed");
	std::cout << "  " << sleepers << " sleepers, none with a weapon in the hands; member " << idOf(sheathed) << " carries its sheathed weapon\n";

	// ---- 3: an attack on it wakes it with the weapon back in the hands ----
	const std::uint64_t id = idOf(sheathed);
	std::uint64_t attacker = 0;
	for (const CombatArena::Member& m : sim.combat()->members())
		if (m.id != id && !m.fighter.dead && !m.fighter.asleep && !m.fighter.playerControlled)
		{
			attacker = m.id;
			break;
		}
	test::require(attacker != 0, "someone awake to attack the sleeper");
	state::Fighter& a = fighterOf(sim, attacker);
	const state::Fighter& s = fighterOf(sim, id);
	a.x = s.x + 1.0;
	a.z = s.z;
	a.y = s.y;
	a.clearRoute();
	sim.enqueue(Command::fighterAttack(state::Id{attacker}, state::Id{id}));
	runUntil(sim, [&] { return !fighterOf(sim, id).asleep; }, 400, "the attacked sleeper never woke");
	runUntil(sim, [&] { return sim.weaponInHands(id); }, 100, "the woken sleeper never drew its weapon");
	std::cout << "  attacked sleeper " << id << " woke and drew its weapon\n";
}

// 2: a world squad's sitter at 275 sheathes through the shared step.
void sitter()
{
	dt27::Rig rig({{routines::task::SitAround, 0, 24, 0, 0}}, 0);
	const BuildingSite* seat = nullptr;
	for (const BuildingSite& site : rig.sim.world().buildingSites())
		if (site.hasInterior && !site.ruined && (seat == nullptr
			|| std::hypot(site.cx - dt27::kX, site.cz - dt27::kZ) < std::hypot(seat->cx - dt27::kX, seat->cz - dt27::kZ)))
			seat = &site;
	test::require(seat != nullptr, "the world has an interior building");
	const dt27::Town town = dt27::makeTown(rig, 2, dt27::kX, dt27::kZ);
	test::require(std::hypot(seat->cx - town.circle.x, seat->cz - town.circle.z) <= town.circle.radius,
		"the nearest interior building is outside the town circle (" + dt27::fmt(std::hypot(seat->cx - dt27::kX, seat->cz - dt27::kZ)) + " u)");
	test::require(rig.sim.weaponInHands(rig.member), "the member starts with a weapon in the hands");
	test::require(rig.until(30.0, [&] { return rig.goalOf(rig.member) == routines::task::SitAround; }), "the member never took 275");
	test::require(rig.until(10.0, [&] { return !rig.sim.weaponInHands(rig.member); }), "the sitter kept its weapon in the hands");
	test::require(carriesWeapon(rig.sim, rig.entityOf(rig.member)), "the sheathed weapon is in the pack");
	std::cout << "  275: the sitter sheathed through the shared step\n";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		sleepers(order, combat);
		sitter();
		std::cout << "ksim_sheathe: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_sheathe: FAILED: " << e.what() << "\n";
		return 1;
	}
}
