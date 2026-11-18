// The get-out-of-bed class (task ds/follow-bed; kinspect dt24_208 / s63): 116 / 208 / 257 are one Task_GetOutOfBed.
//   - the registry: all three rows are canGetUp = 1 -> isInBed = 0, so a goal that needs the member up can plan the wake;
//   - 208 GET_OUT_OF_BED_IF_ITS_EMERGENCY is the Urgent selector goal: a bed sleeper whose package carries the row gets up when
//     it is attacked (utility 2.0, s63 AI::vf28 1405a0b50), and one whose package has no such row lies on - the wake is a goal;
//   - the shared action (140345620) leaves no bed claim and no bed goal behind; being knocked out is no emergency (canGetUp is 0).
// The town scorer runs live over resident squads at night; no NPC is ordered.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

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
	// These tests pin the seeded placement pass (5.3), which a fresh start's residents skip (they walk in: ksim_residents_arrive).
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false);
	return sim;
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

double hourOf(Simulation& sim)
{
	return std::fmod(trader::gameHours(sim.clockTick()), 24.0);
}

std::string clockText(Simulation& sim)
{
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "%05.2f", hourOf(sim));
	return buffer;
}


void emergencyWake(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	// ---- the registry rows ----
	for (const std::uint16_t task : {routines::task::GetOutOfBed, routines::task::GetOutOfBedEmergency, routines::task::GetOutOfBedHealed})
	{
		const goap::TaskRow* row = goap::rowOf(task);
		test::require(row != nullptr && !row->hasGoalState, "get-out-of-bed rows exist and have no goal state");
		test::require(row->preCount == 1 && row->pre[0].sym == goap::Sym::CanGetUp && row->pre[0].value, "pre canGetUp = 1");
		test::require(row->effCount == 1 && row->eff[0].sym == goap::Sym::IsInBed && !row->eff[0].value, "effect isInBed = 0");
	}

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

	const auto hasRow = [&](flecs::entity member, std::uint16_t task)
	{
		const flecs::entity squad = member.target<state::MemberOf>();
		const routines::CompiledPackage* package = packages.find(sim.ai()->registry().squad(squad.get<state::SquadInstance>()->templateSid).package);
		if (package == nullptr)
			return false;
		const state::MemberRole* role = member.get<state::MemberRole>();
		const routines::Role listRole = role != nullptr ? static_cast<routines::Role>(std::min<std::uint8_t>(role->list, 3)) : routines::Role::Squad;
		for (const routines::Candidate& c : routines::candidatesAt(*package, listRole, hourOf(sim)))
			if (!c.fallback && c.task == task)
				return true;
		return false;
	};
	flecs::entity with208;
	for (const Pick* pick : chosen)
		for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
		{
			const state::Routine* r = member.get<state::Routine>();
			if (r == nullptr || !fighterOf(sim, idOf(member)).asleep || !isBed(r->goal))
				continue;
			if (hasRow(member, routines::task::GetOutOfBedEmergency) && !with208.is_valid())
				with208 = member;
		}
	test::require(with208.is_valid(), "a sleeper whose package carries 208");
	std::cout << "  sleeper with 208: " << idOf(with208) << "\n";

	// ---- canGetUp: a knocked-out sleeper has no plan, a conscious one has [208] ----
	{
		const std::uint64_t id = idOf(with208);
		test::require(sim.planHomebase(id, routines::task::GetOutOfBedEmergency, 0.0, 0.0).found(), "an asleep, conscious member plans the wake");
		fighterOf(sim, id).unconscious = true;
		test::require(!sim.planHomebase(id, routines::task::GetOutOfBedEmergency, 0.0, 0.0).found(), "canGetUp is 0 while knocked out");
		fighterOf(sim, id).unconscious = false;
	}

	// ---- the emergency: attacked sleeper with the row gets up; the bed goal and claim go with it ----
	const auto attackFor = [&](flecs::entity sleeper, std::uint64_t ticks) -> bool
	{
		const std::uint64_t id = idOf(sleeper);
		std::uint64_t attacker = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			if (m.id != id && !m.fighter.dead && !m.fighter.asleep && !m.fighter.playerControlled)
			{
				attacker = m.id;
				break;
			}
		test::require(attacker != 0, "someone awake to attack the sleeper");
		// A real attack: the attacker stands next to the sleeper and is ordered onto it (the order goes to the attacker, not to the
		// NPC under test).
		state::Fighter& a = fighterOf(sim, attacker);
		const state::Fighter& s = fighterOf(sim, id);
		a.x = s.x + 1.0;
		a.z = s.z;
		a.y = s.y;
		a.clearRoute();
		sim.enqueue(Command::fighterAttack(state::Id{attacker}, state::Id{id}));
		for (std::uint64_t i = 0; i < ticks; ++i)
		{
			sim.run(1);
			if (!fighterOf(sim, id).asleep)
				return true;
		}
		return false;
	};
	{
		const std::uint64_t id = idOf(with208);
		test::require(attackFor(with208, 400), "an attacked sleeper whose package carries 208 gets out of bed");
		const state::Routine* r = with208.get<state::Routine>();
		test::require(r != nullptr && !isBed(r->goal), "its bed goal is gone with the wake (" + std::to_string(r != nullptr ? r->goal : 0) + ")");
		std::cout << "  208 woke sleeper " << id << " at " << clockText(sim) << ", goal now " << (r != nullptr ? r->goal : 0) << "\n";
	}
	// The control: a sleeper whose list has NO 208 row still leaves the bed when an alarm goes off - the running bed routine's own
	// preconditions fail (USE_BED: no alarm in the vicinity, s63 lines 263-270), no package row needed. A second sleeper is moved to the
	// package's Slave list (no bed and no 208 there) while it lies in its bed.
	flecs::entity control;
	for (const Pick* pick : chosen)
		for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
		{
			const state::Routine* r = member.get<state::Routine>();
			if (!control.is_valid() && member != with208 && r != nullptr && fighterOf(sim, idOf(member)).asleep && isBed(r->goal))
				control = member;
		}
	test::require(control.is_valid(), "a second sleeper for the control");
	{
		state::MemberRole role = control.has<state::MemberRole>() ? *control.get<state::MemberRole>() : state::MemberRole{};
		role.list = 3;
		control.set<state::MemberRole>(role);
	}
	test::require(!hasRow(control, routines::task::GetOutOfBedEmergency), "the control's list carries no 208 row");
	test::require(attackFor(control, 400), "an attacked sleeper with no 208 row still gets out of bed (the bed's preconditions fail)");
	test::require(!fighterOf(sim, idOf(control)).asleep && !isBed(control.get<state::Routine>()->goal), "and its bed goal is gone");
	// The weapon recheck (task ds/sheathe): USE_BED needs hasWeaponEquipped = 0, so a sleeper handed a weapon in the hands fails the
	// running routine's preconditions and wakes. The residents sheathe as they take the bed; a sleeper that carries a weapon draws it.
	{
		flecs::entity armed;
		const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
		for (const Pick* pick : chosen)
			for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
			{
				const state::Routine* r = member.get<state::Routine>();
				const state::Inventory* pack = member.get<state::Inventory>();
				if (armed.is_valid() || r == nullptr || pack == nullptr || !fighterOf(sim, idOf(member)).asleep || !isBed(r->goal))
					continue;
				for (const state::Inventory::Entry& entry : pack->items)
					if (entry.slot == state::Inventory::kCarried || entry.slot == state::Inventory::kSheathed)
						if (const kdata::Record* record = sim.gameData()->find(entry.itemSid); record != nullptr && record->type == weaponType)
							armed = member;
			}
		test::require(armed.is_valid(), "a sleeper that carries a (sheathed) weapon");
		const std::uint64_t id = idOf(armed);
		test::require(!sim.weaponInHands(id), "it sleeps with empty hands");
		test::require(!sim.drawWeapon(id).empty() && sim.weaponInHands(id), "the weapon is put in its hands");
		runUntil(sim, [&] { return !fighterOf(sim, id).asleep; }, 400, "a sleeper holding a weapon never woke (the recheck)");
		test::require(!isBed(armed.get<state::Routine>()->goal), "its bed goal is gone with the wake");
		std::cout << "  armed sleeper " << id << " woke (hasWeaponEquipped recheck)\n";
	}
	std::cout << "emergency wake ok\n";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		emergencyWake(order, combat);
		std::cout << "ksim_get_out_of_bed: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_get_out_of_bed: FAILED: " << e.what() << "\n";
		return 1;
	}
}
