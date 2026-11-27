// The stealth KO's Simulation side (task ds/stealth-ko S4, dt41_C Task_StealthKO). The task itself runs in the combat arena
// (CombatArena::stealthKoStep, Fighter::stealthKo): the approach, the clip, the roll, the KO and the XP. The assault crime goes through the
// arena's attack notes (stepCrime). What is left here is the order and the dialogue event of a failed roll.

#include "ksim/core/simulation.hpp"
#include "ksim/combat/stealth_ko.hpp"

namespace ksim {

std::string Simulation::orderStealthKo(std::uint64_t actor, std::uint64_t victim)
{
	if (combat_ == nullptr)
		return "bad_target";
	const std::string reason = combat_->orderStealthKo(actor, victim);
	if (!reason.empty())
		return reason;
	// A replacing order, as order 60 is: the running interact order and the queue end, the follow task is dropped.
	cancelOrders(actor);
	if (state::Fighter* f = combat_->find(actor))
	{
		CombatArena::endFollow(*f);
		if (f->asleep)
		{
			f->asleep = false;
			f->leaveBed();
		}
	}
	mirrorFighter(actor);
	return {};
}

std::string Simulation::checkStealthKo(std::uint64_t actor, std::uint64_t victim) const
{
	return combat_ != nullptr ? combat_->checkStealthKo(actor, victim) : "bad_target";
}

void Simulation::stepStealthKo()
{
	if (combat_ == nullptr)
		return;
	for (const CombatArena::StealthKoNote& note : combat_->takeStealthKos())
	{
		// 0x38 EV_ASSASSINATION_FAILED (H): the victim's event, the assassin its subject.
		if (note.result == 1)
			fireDialogue(note.victim, note.assassin, stealthko::kEvAssassinationFailed);
	}
}

} // namespace ksim
