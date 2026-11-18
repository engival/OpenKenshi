// The standing goals of the generic path (task port-standing; kinspect dt24_plantasks sections 1 - 2, ai_enum_map rows 19 / 24 / 30 / 62):
// 24 WANDERER, 19 GO_HOMEBUILDING, 30 HOLD_POSITION and 62 STAND_STILL. None is a package goal in the data ("code-only: player order /
// dialogue / sub-goal"), so orderStandingGoal runs the finder (world_resolvers.hpp) and puts the goal in the member's Routine;
// stepStandingGoals executes it. 28 is world_tasks.cpp's, 14 / 17 are bare Task_Blank plan nodes (goap.cpp), 37 / 47 / 51 need the town node
// database the data does not hold (docs/reference/AI_COVERAGE.md rows 14 - 62).

#include "ksim/simulation.hpp"

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/towncircle.hpp"

#include "world_resolvers.hpp"

#include <cmath>
#include <sstream>

namespace ksim {

namespace {

constexpr double kGiveUpSeconds = 120.0; // OURS: a walk that never arrives drops 24 / 19 / 30 (the exe's plan fails the task on a blocked route)
constexpr double kHoldArrive = 20.0; // 30's goal isAtLocation: within 20 u of the hand's place. UNSURE: the exe's radius (10 / 50 m) is not read for this row

} // namespace

bool Simulation::orderStandingGoal(std::uint64_t id, std::uint16_t goal, double x, double z)
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || gameData_ == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0)
		return false;
	StandingRun run;
	switch (goal)
	{
	case routines::task::Wanderer:
	{
		std::vector<towncircle::Circle> circles;
		world_.ecs().each([&](const state::TownState& town) { circles.push_back(towncircle::of(*gameData_, town)); });
		const int found = worldres::wanderer24(circles, f->x, f->z);
		if (found < 0)
			return false;
		run = {circles[found].x, circles[found].z, circles[found].radius};
		break;
	}
	case routines::task::GoHomeBuilding:
	{
		// 14099f370: the home building's hand (ownerships +0x38). The branches for a player faction (the platoon's home building) and for a member
		// with no home town (the nearest town of my faction within a 4608 u margin, adopted) are not modelled: UNSURE, no such member orders it.
		const BuildingSite* home = nullptr;
		for (const BuildingSite& site : world_.buildingSites())
			if (f->homeBuilding != 0 && site.key == f->homeBuilding)
				home = &site;
		if (home == nullptr)
			return false;
		run = {home->cx, home->cz, std::max(home->halfA, home->halfB)};
		break;
	}
	case routines::task::HoldPosition:
		run = {x, z, kHoldArrive}; // no finder (desc+0x160 null): the plan's hand is the place the order names
		break;
	case routines::task::StandStill:
		run = {f->x, f->z, 0.0}; // Task_StandStill: no place, the character stops where it is
		break;
	default:
		return false;
	}
	standingRun_[id] = run;
	state::Routine routine;
	routine.goal = goal;
	routine.since = clockTick();
	routine.plan = {goal};
	routine.nextScore = clockTick() + 100000; // the scorer does not re-score the member while the goal runs; endGoalRoutine hands it back
	fighterEntity(id).set<state::Routine>(std::move(routine));
	mirrorFighter(id);
	return true;
}

void Simulation::stepStandingGoals(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Routine* current = ecs.entity(combatEntities_[i]).get<state::Routine>();
		if (current == nullptr)
			continue;
		const std::uint16_t goal = current->goal;
		if (goal != routines::task::Wanderer && goal != routines::task::GoHomeBuilding && goal != routines::task::HoldPosition
			&& goal != routines::task::StandStill)
			continue;
		const std::uint64_t id = members[i].id;
		const auto run = standingRun_.find(id);
		state::Fighter& f = *combat_->find(id);
		if (run == standingRun_.end() || f.dead || f.unconscious || f.down || f.carriedBy != 0 || f.target != 0)
		{
			endGoalRoutine(id, goal); // a fight, a fall or a lift ends it: the goal is just not served any more
			continue;
		}
		if (goal == routines::task::StandStill)
		{
			// Task_StandStill's tick (140336ba0) only stops the movement host; no finish flag, the plan's goal ends it.
			if (!f.route.empty())
			{
				f.clearRoute();
				mirrorFighter(id);
			}
			continue;
		}
		const bool arrived = std::hypot(f.x - run->second.x, f.z - run->second.z) <= run->second.radius;
		if (arrived)
		{
			if (goal == routines::task::HoldPosition)
			{
				if (!f.route.empty())
				{
					f.clearRoute();
					mirrorFighter(id);
				}
				continue; // Task_Blank: nothing more to do, and no finish
			}
			std::ostringstream line;
			line << "standing " << goal << " member " << id << " arrived";
			logTask(line.str());
			endGoalRoutine(id, goal);
			continue;
		}
		if (static_cast<double>(ticksSince(now, current->since)) * kTickSeconds > kGiveUpSeconds)
			endGoalRoutine(id, goal);
		else if (now % kSquadIntervalTicks == 0 && f.route.empty())
			walk(id, run->second.x, run->second.z);
	}
}

} // namespace ksim
