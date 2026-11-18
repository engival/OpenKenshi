#include "ksim/ai/jobs.hpp"

#include "ksim/combat/combat.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"
#include "world/world_resolvers.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace ksim
{

namespace
{

// jobs17 Appendix A, the types with `kind` != 0: {type, name, kind, targeted, replacement, companion, executor}. A replacement of
// 153 (Going to bed) is a kind 0 type: Sleeping, Sleeping on ground and Hiding are plain orders even with Shift.
constexpr JobType kJobTypes[] =
{
	{2, "Building", 1, false, 125, 0, false},
	{4, "Attacking", 1, false, 16, 0, true},
	{5, "Attacking target", 1, false, 16, 0, true},
	{16, "Attacking enemies", 1, false, 0, 0, true},
	{25, "First aid", 1, false, 58, 0, true},
	{26, "Looting", 1, false, 238, 0, false},
	{31, "Staying close", 1, true, 0, 0, true},
	{45, "Bodyguard", 1, true, 0, 31, true},
	{57, "Robotics", 1, false, 0, 0, true},
	{58, "Medic", 1, false, 0, 0, true},
	{60, "Repairing (robot)", 1, false, 57, 0, false},
	{61, "Attacking target unprovoked", 1, false, 16, 0, true},
	{87, "Operating machine", 1, true, 0, 0, false},
	{95, "Repairing", 1, false, 125, 0, false},
	{97, "Training", 1, true, 0, 0, false},
	{98, "Sleeping", 1, false, 153, 0, false},
	{106, "Find and put in bed", 1, false, 0, 0, false},
	{124, "Hauling to", 1, true, 0, 0, false},
	{125, "Engineer", 1, false, 0, 0, true},
	{136, "Disposing of corpse", 1, true, 137, 138, false},
	{137, "Corpse disposal", 1, true, 0, 138, false},
	{138, "Find corpses", 1, false, 0, 0, false},
	{146, "Turret gunner", 2, false, 234, 0, false},
	{148, "Find and rescue", 1, false, 0, 106, false},
	{152, "Operating automatic machine", 1, true, 0, 124, false},
	{175, "Sleeping on ground", 1, false, 153, 0, false},
	{187, "Obedient Slave", 1, false, 0, 0, false},
	{221, "Operating machine", 1, true, 0, 0, false},
	{223, "Remove Home Intruder", 1, false, 0, 224, false},
	{234, "Turret guard", 2, true, 0, 0, false},
	{238, "Foraging animals", 1, false, 0, 0, false},
	{249, "Splinting", 1, false, 250, 0, false},
	{250, "Splint Rigging", 1, false, 0, 0, true},
	{261, "Hiding", 1, false, 153, 0, false},
	{273, "Remove Intruder", 1, false, 0, 274, false},
	{289, "Staying close", 1, true, 0, 0, true},
};

// The type a row of `info` is stored as.
std::uint16_t storedType(const JobType& info) noexcept
{
	return info.replacement != 0 ? info.replacement : info.type;
}

// The companion row's type: the order's own, else the stored type's (the dispatcher reads the first, addTask the second).
std::uint16_t companionType(const JobType& info) noexcept
{
	if (info.companion != 0)
		return info.companion;
	const JobType* stored = jobType(storedType(info));
	return stored != nullptr ? stored->companion : std::uint16_t{0};
}

bool hasRow(const state::Jobs& jobs, const JobType& type, std::uint64_t target)
{
	return std::any_of(jobs.rows.begin(), jobs.rows.end(), [&](const state::JobRow& row)
	{
		return row.type == type.type && (!type.targeted || row.target == target);
	});
}

double distance(const state::Fighter& a, const state::Fighter& b)
{
	return std::hypot(a.x - b.x, a.z - b.z);
}

bool usable(const state::Fighter& f)
{
	return !f.dead && !f.unconscious && f.carriedBy == 0;
}

// The 187 Obedient Slave row of a slave (state 1) switches the master's deference rules on (Fighter::obedient).
bool obedientOf(const state::Fighter& f, const state::Jobs& jobs)
{
	return f.slaveState == 1 && std::any_of(jobs.rows.begin(), jobs.rows.end(), [](const state::JobRow& row) { return row.type == 187; });
}

} // namespace

const JobType* jobType(std::uint16_t type) noexcept
{
	const auto at = std::lower_bound(std::begin(kJobTypes), std::end(kJobTypes), type,
		[](const JobType& entry, std::uint16_t key) { return entry.type < key; });
	return at != std::end(kJobTypes) && at->type == type ? &*at : nullptr;
}

bool jobRunsWhenOff(std::uint16_t type) noexcept
{
	return type == 16 || type == 31 || type == 289 || type == 45 || type == 57 || type == 58 || type == 250;
}

int addJob(state::Jobs& jobs, std::uint16_t type, std::uint64_t target)
{
	const JobType* order = jobType(type);
	const JobType* stored = order != nullptr ? jobType(storedType(*order)) : nullptr;
	if (stored == nullptr)
		return 0;
	if (!stored->targeted)
		target = 0;
	int added = 0;
	std::size_t at = jobs.rows.size();
	if (!hasRow(jobs, *stored, target))
	{
		jobs.rows.push_back({jobs.nextJobId++, stored->type, target});
		at = jobs.rows.size();
		++added;
	}
	if (const JobType* companion = jobType(companionType(*order)))
	{
		if (!hasRow(jobs, *companion, target))
		{
			jobs.rows.insert(jobs.rows.begin() + static_cast<std::ptrdiff_t>(at),
				state::JobRow{jobs.nextJobId++, companion->type, companion->targeted ? target : 0});
			++added;
		}
	}
	if (added != 0)
		jobs.enabled = true;
	return added;
}

bool moveJob(state::Jobs& jobs, std::uint32_t jobId, std::uint16_t index)
{
	const auto from = std::find_if(jobs.rows.begin(), jobs.rows.end(), [&](const state::JobRow& row) { return row.jobId == jobId; });
	if (from == jobs.rows.end())
		return false;
	const state::JobRow row = *from;
	jobs.rows.erase(from);
	const std::size_t at = std::min<std::size_t>(index, jobs.rows.size());
	jobs.rows.insert(jobs.rows.begin() + static_cast<std::ptrdiff_t>(at), row);
	return true;
}

bool removeJob(state::Jobs& jobs, std::uint32_t jobId)
{
	const auto at = std::find_if(jobs.rows.begin(), jobs.rows.end(), [&](const state::JobRow& row) { return row.jobId == jobId; });
	if (at == jobs.rows.end())
		return false;
	jobs.rows.erase(at);
	return true;
}

std::string Simulation::checkJobOrder(const JobOrder& order) const
{
	const state::Fighter* fighter = combat_ != nullptr ? combat_->find(order.character) : nullptr;
	if (fighter == nullptr)
		return "unknown_id";
	if (!fighter->playerControlled)
		return "not_player_controlled";
	if (fighter->dead)
		return "dead";
	switch (order.kind)
	{
	case 76:
	{
		if (order.type == 0 || order.type > kJobTypeMax)
			return "bad_value";
		// A type with `kind` 0 (or whose stored type has it) is a plain order; one with no executor yet would sit in the list doing nothing.
		const JobType* info = jobType(order.type);
		const JobType* stored = info != nullptr ? jobType(storedType(*info)) : nullptr;
		if (stored == nullptr || !stored->executor)
			return "unsupported";
		if (const std::uint16_t companion = companionType(*info); companion != 0)
		{
			const JobType* extra = jobType(companion);
			if (extra == nullptr || !extra->executor)
				return "unsupported";
		}
		if (stored->targeted)
		{
			const state::Fighter* target = order.target != 0 ? combat_->find(order.target) : nullptr;
			if (target == nullptr || target->dead || order.target == order.character)
				return "bad_target";
		}
		return "";
	}
	case 77:
	case 78:
	{
		const JobsView view = jobsOf(order.character);
		const bool found = std::any_of(view.rows.begin(), view.rows.end(), [&](const state::JobRow& row) { return row.jobId == order.jobId; });
		return found ? "" : "bad_target";
	}
	case 10:
		return order.enabled > 1 ? "bad_value" : "";
	default:
		return "bad_value";
	}
}

JobsView Simulation::jobsOf(std::uint64_t id) const
{
	JobsView view;
	if (combat_ == nullptr || combat_->find(id) == nullptr)
		return view;
	if (const state::Jobs* jobs = fighterEntity(id).get<state::Jobs>())
	{
		view.enabled = jobs->enabled;
		view.rows = jobs->rows;
	}
	return view;
}

std::string Simulation::runJobOrder(const JobOrder& order, bool commit)
{
	std::string reason = checkJobOrder(order);
	if (!reason.empty() || !commit)
		return reason;
	flecs::entity entity = fighterEntity(order.character);
	const state::Jobs* held = entity.get<state::Jobs>();
	state::Jobs jobs = held != nullptr ? *held : state::Jobs{};
	switch (order.kind)
	{
	case 76:
		addJob(jobs, order.type, order.target);
		break;
	case 77:
		removeJob(jobs, order.jobId);
		break;
	case 78:
		moveJob(jobs, order.jobId, order.index);
		break;
	case 10:
		jobs.enabled = order.enabled != 0;
		break;
	}
	if (state::Fighter* f = combat_->find(order.character))
		f->obedient = obedientOf(*f, jobs);
	if (held != nullptr ? !(jobs == *held) : !(jobs == state::Jobs{}))
		entity.set<state::Jobs>(std::move(jobs));
	return {};
}

void Simulation::enslaveJobs(std::uint64_t slave)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(slave) : nullptr;
	if (f == nullptr)
		return;
	// Both enslave handlers (FUN_14035aa20, FUN_14032e590; rwh.99 section 7) wipe the orders and add 187 for every character, not only the player's.
	state::Jobs jobs;
	jobs.rows.push_back({jobs.nextJobId++, 187, 0});
	f->obedient = obedientOf(*f, jobs);
	fighterEntity(slave).set<state::Jobs>(std::move(jobs));
}

void Simulation::stepJobs()
{
	if (combat_ == nullptr || clockTick() % kJobIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::Jobs* held = actor.get<state::Jobs>();
		if (held == nullptr)
			continue;
		const std::uint64_t id = members[i].id;
		state::Fighter& f = *combat_->find(id);
		const state::Jobs before = *held; // the pointer dies when a goal start changes the entity's archetype
		f.obedient = obedientOf(f, before);
		state::Jobs jobs = before;
		bool touched = false; // a fighter field changed (mirrored at the end)

		const auto clearRun = [&]
		{
			jobs.running = 0;
			jobs.runType = 0;
			jobs.runTarget = 0;
		};
		// The goal the sim started is still the character's: no player order replaced it, it has not ended.
		const auto goalLive = [&]
		{
			if (jobs.running == 0)
				return false;
			switch (jobs.runType)
			{
			case 125:
			{
				const auto order = buildOrders_.find(id);
				return order != buildOrders_.end() && order->second.building == jobs.runTarget;
			}
			case 57:
			case 58:
			case 250:
			{
				const state::InteractTask* task = actor.get<state::InteractTask>();
				return task != nullptr && task->action == static_cast<std::uint8_t>(InteractAction::FirstAid) && task->target == jobs.runTarget;
			}
			case 16:
				return f.orderTarget == jobs.runTarget;
			case 31:
				return f.followTarget == jobs.runTarget;
			}
			return false;
		};
		const auto dropGoal = [&]
		{
			switch (jobs.runType)
			{
			case 125:
				buildOrders_.erase(id);
				break;
			case 57:
			case 58:
			case 250:
				cancelOrders(id);
				break;
			case 16:
				f.orderTarget = 0;
				f.orderStop = true;
				// Task dt25 (45 BODYGUARD): a dropped bodyguard goal gives up its protected hand (the original clears
				// AITaskSystem+0x10 when a goal outside its keep set wins).
				if (f.protectTarget == jobs.runTarget)
					f.protectTarget = 0;
				break;
			case 31:
				CombatArena::endFollow(f);
				f.clearRoute();
				break;
			}
			clearRun();
			touched = true;
		};
		const auto rowIndex = [&](std::uint32_t jobId) -> std::size_t
		{
			for (std::size_t r = 0; r < jobs.rows.size(); ++r)
			{
				if (jobs.rows[r].jobId == jobId)
					return r;
			}
			return jobs.rows.size();
		};
		const auto allowed = [&](const state::JobRow& row) { return jobs.enabled || jobRunsWhenOff(row.type); };

		[&]
		{
			if (f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.carrying != 0 || f.asleep)
			{
				clearRun();
				return;
			}
			bool owned = goalLive();
			if (!owned)
				clearRun();
			std::size_t runningRow = jobs.rows.size();
			if (owned)
			{
				runningRow = rowIndex(jobs.running);
				// The row was removed, or JOBS went off under it: the goal is dropped at this replan.
				if (runningRow == jobs.rows.size() || !allowed(jobs.rows[runningRow]))
				{
					dropGoal();
					owned = false;
				}
			}
			// A direct order of the player outranks the jobs while it runs (a walk, drive, follow or attack order, a hold, an
			// interact, a queued order, a door or carry step).
			if (!owned && (actor.has<state::InteractTask>() || actor.has<state::OrderQueue>() || actor.has<state::DoorStep>()
				|| actor.has<state::ProspectTask>() || actor.has<state::CarryStep>() || buildOrders_.contains(id) || f.taskRunning() || f.orderTarget != 0 || f.hold))
				return;
			// Below URGENT: the character's own fight comes first.
			if (f.target != 0)
			{
				const state::Fighter* foe = combat_->find(f.target);
				if (foe != nullptr && usable(*foe) && foe->team != f.team)
					return;
			}
			struct Plan
			{
				std::uint16_t kind = 0; // 57 / 58 / 125 / 250 / 16 / 31, state::Jobs::runType
				std::uint64_t target = 0;
				std::vector<state::RoutePoint> points;
				bool approach = false;
				std::uint16_t task = 0; // 125: the construction task (71 / 2 / 95)
			};
			const auto planFollow = [&](std::uint64_t target) -> Plan
			{
				const state::Fighter* leader = combat_->find(target);
				if (leader == nullptr || leader->dead || target == id || combat_->followLoops(id, target) || f.followTarget == target)
					return {};
				return {31, target, {}, false};
			};
			// The nearest hostile the character is aware of, optionally only one that attacks `protectee`.
			const auto planAttack = [&](std::uint64_t protectee) -> Plan
			{
				std::uint64_t best = 0;
				double bestD = 0.0;
				for (const CombatArena::Member& other : members)
				{
					if (other.id == id || !usable(other.fighter) || other.fighter.team == f.team || !perception::aware(f, other.id)
						|| (protectee != 0 && other.fighter.target != protectee))
						continue;
					const double d = distance(f, other.fighter);
					if (best == 0 || d < bestD)
					{
						best = other.id;
						bestD = d;
					}
				}
				return best != 0 ? Plan{16, best, {}, false} : Plan{};
			};
			// `robotOnly` is 57 JOB_REPAIR_ROBOT (dt24_101 section 4): the Medic body with a robot repair kit (item function 12) and a patient
			// whose robotic parts are hurt; treat() then takes the robotic kit by itself (needsRobotKit). OURS: the exe's flag = 1 only lifts
			// a gate the dt16 notes call "robot-ready" (UNSURE), so the split into kit and category is the reading that fits the name.
			const auto planMedic = [&](bool robotOnly) -> Plan
			{
				// A kit in hand or in the pack (treat() takes it).
				bool kit = robotOnly ? carriesKit(id, kItemFunctionRobotKit) : f.kitCharges > 0.0;
				if (!kit && !robotOnly)
				{
					if (const state::Inventory* pack = actor.get<state::Inventory>())
					{
						for (const state::Inventory::Entry& entry : pack->items)
						{
							const ItemInfo* info = entry.slot == state::Inventory::kCarried ? combat_->data().item(entry.itemSid) : nullptr;
							kit = kit || (info != nullptr && info->charges > 0.0
								&& (info->function == kItemFunctionMedkit || info->function == kItemFunctionRobotKit));
						}
					}
				}
				if (!kit)
					return {};
				std::vector<std::pair<double, std::uint64_t>> patients;
				for (const CombatArena::Member& other : members)
				{
					if (other.id == id || other.fighter.team != f.team || other.fighter.dead || other.fighter.carriedBy != 0
						|| distance(f, other.fighter) > kJobMedicRange
						|| (robotOnly && worldres::injuryOf(other.fighter, true) <= worldres::kInjuryEpsilon))
						continue;
					const InteractTarget target = findInteractTarget(ecs, combat_.get(), other.id);
					if (checkInteract(ecs, *combat_, id, target, static_cast<std::uint8_t>(InteractAction::FirstAid)).empty())
						patients.emplace_back(distance(f, other.fighter), other.id);
				}
				std::sort(patients.begin(), patients.end());
				for (std::size_t k = 0; k < patients.size() && k < 3; ++k)
				{
					const InteractTarget target = findInteractTarget(ecs, combat_.get(), patients[k].second);
					Plan plan{robotOnly ? std::uint16_t{57} : std::uint16_t{58}, patients[k].second, {}, false};
					if (target.distanceTo(f.x, f.z) <= kInteractReach && std::fabs(f.y - target.y) <= kInteractReachY)
						return plan;
					if (navigation_ == nullptr)
						continue; // bare worlds (no navigator): only a patient within reach can be planned
					const Route route = navigation_->routeFrom({f.x, f.y, f.z}, target.x, target.z, target.y, false, body(f));
					if (!route.found || route.points.empty())
						continue;
					for (std::size_t p = 0; p < route.points.size(); ++p)
						plan.points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
					plan.approach = route.approach;
					return plan;
				}
				return {};
			};
			const auto planRow = [&](const state::JobRow& row) -> Plan
			{
				switch (row.type)
				{
				case 57:
					return planMedic(true);
				case 58:
					return planMedic(false);
				case 125:
				{
					const auto [task, building] = planEngineer(id);
					Plan work{task != 0 ? std::uint16_t{125} : std::uint16_t{0}, building, {}, false};
					work.task = task;
					return work;
				}
				case 250:
				{
					// Splint Rigging (Task_FirstAidRig, dt24_smalltasks section 6): the same patient walk and kit as the medic; the splint's own
					// effect on a limb is undecoded (UNSURE), so the first aid interact is all it does.
					Plan splint = planMedic(false);
					if (splint.kind != 0)
						splint.kind = 250;
					return splint;
				}
				case 16:
					return planAttack(0);
				case 31:
				case 289:
					return planFollow(row.target);
				case 45:
				{
					Plan defend = planAttack(row.target);
					return defend.kind != 0 ? defend : planFollow(row.target);
				}
				}
				return {};
			};
			// Row order is priority. An unplannable row falls through; a goal already running ends the search: only the rows above it
			// may take over, and its own row only with another kind of goal (a Bodyguard that was following sees its target attacked).
			const std::size_t last = owned ? runningRow + 1 : jobs.rows.size();
			for (std::size_t r = 0; r < last; ++r)
			{
				if (!allowed(jobs.rows[r]))
					continue;
				Plan plan = planRow(jobs.rows[r]);
				if (plan.kind == 0 || (owned && r == runningRow && plan.kind == jobs.runType))
					continue;
				if (owned)
					dropGoal();
				switch (plan.kind)
				{
				case 125:
					orderConstructionTask(id, plan.task, plan.target);
					break;
				case 57:
				case 58:
				case 250:
					beginInteract(id, plan.target, static_cast<std::uint8_t>(InteractAction::FirstAid), 0, std::move(plan.points), plan.approach);
					break;
				case 16:
					apply(Command::fighterAttack(state::Id{id}, state::Id{plan.target}));
					break;
				case 31:
					apply(Command::fighterFollow(state::Id{id}, state::Id{plan.target}));
					break;
				}
				// Task dt25 (45 BODYGUARD, kinspect dt25 section 1.2 / 1.5): the order's charge is the protected hand while the
				// defense runs (the original's AITaskSystem+0x10, kept by the goal select); the calm half is the companion 31's
				// follow, which holds no hand (the original clears it when the follow wins the calm moment).
				if (jobs.rows[r].type == 45)
				{
					const std::uint64_t charge = plan.kind == 16 ? plan.target : 0;
					if (f.protectTarget != charge)
					{
						f.protectTarget = charge;
						touched = true;
					}
				}
				jobs.running = jobs.rows[r].jobId;
				jobs.runType = plan.kind;
				jobs.runTarget = plan.target;
				touched = true;
				return;
			}
		}();

		if (!(jobs == before))
			actor.set<state::Jobs>(std::move(jobs));
		if (touched)
			mirrorFighter(id);
	}
}

} // namespace ksim
