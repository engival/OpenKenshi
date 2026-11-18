// The talk goals of the generic path (task port-talk; kinspect dt24_plantasks, dt24_stalk_turret): 102 SEEK_AND_TALK_AND_SEND_SIGNAL and 159
// SEND_DIALOGUE_SIGNAL (Task_TalktoPlayer), 164 FOLLOW_WHILE_TALKING (Task_FollowAndTalk), 165 TOWN_STALKER (Task_TownStalker). None has a
// finder (desc+0x160 null: "target from the plan / current hand") and none is a package goal in the data, so an order or a plan names the
// hand; the executor is world_tasks.cpp's (stepWorldTasks). Docs: AI_COVERAGE.md rows 102 / 159 / 164 / 165.

#include "ksim/simulation.hpp"

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"

namespace ksim {

bool Simulation::orderTalkGoal(std::uint64_t id, std::uint16_t goal, std::uint64_t target)
{
	if (goal != routines::task::SeekAndTalk && goal != routines::task::SendDialogue && goal != routines::task::FollowWhileTalking
		&& goal != routines::task::TownStalker)
		return false;
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	const state::Fighter* hand = combat_ != nullptr ? combat_->find(target) : nullptr;
	if (f == nullptr || hand == nullptr || target == id || f->dead || f->unconscious || f->down || f->carriedBy != 0 || hand->dead || hand->carriedBy != 0)
		return false;
	state::Routine routine;
	routine.goal = goal;
	routine.since = clockTick();
	routine.chainTarget = target;
	routine.plan = {goal};
	routine.nextScore = clockTick() + 100000; // the scorer does not re-score the member while the goal runs
	fighterEntity(id).set<state::Routine>(std::move(routine));
	mirrorFighter(id);
	return true;
}

} // namespace ksim
