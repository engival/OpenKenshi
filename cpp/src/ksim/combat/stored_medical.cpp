#include "ksim/ai/routines.hpp"
#include "ksim/combat/corpses.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <optional>
#include <vector>

namespace ksim {

namespace {

// One stored member for the length of a step: its Fighter rebuilt from the template plus its record (what promotion would give it, minus the
// placement), the template build it is compared to afterwards, and its inventory (a kit leaves it).
struct Stored
{
	flecs::entity member;
	std::size_t k = 0;
	state::Fighter built;
	state::Fighter now;
	state::Inventory inventory;
	bool hasInventory = false;
	bool taken = false;
	bool hadRecord = false;
	bool medicRole = false; // its goal list holds 58 JOB_MEDIC
};

double woundSum(const state::Fighter& f)
{
	return std::accumulate(f.wounds.begin(), f.wounds.end(), 0.0);
}

} // namespace

bool Simulation::canTreatAllies(flecs::entity member)
{
	const flecs::entity squad = member.target<state::MemberOf>();
	const routines::CompiledPackage* package = squad.is_valid() ? appliedPackage(squad) : nullptr;
	if (package == nullptr)
		return false;
	for (const routines::GoalRow& row : package->list(liveRole(member)))
	{
		if (row.task == routines::task::FirstAid)
			return true;
	}
	return false;
}

// The off-screen medical step (k16.6.52; ARCHITECTURE.md "Corpses", RUNTIME_SAVE.md v100), behind setOffscreenMedical. Once a game minute, for every
// world squad that has a stored member with a body record (state::MemberDelta kBody): each member is rebuilt as promotion would build it, then
// kStoredMedicalTicks ticks of in-detail medical time run on it with the in-detail functions, never a copy (CombatRules::updateMedical: bleed,
// clot, KO, the blood coma, death; Simulation::treatBodies: first aid). Per tick, each conscious member carrying a kit treats itself, and one in the Medic role (canTreatAllies)
// the squad's worst bleeder (a lone member only treats itself, at the self skill). The result is stored again as the difference from the template:
// a member back at it loses its record. OURS / UNSURE (deferred: playtesting): the treatment has no walk to the patient and no interact time;
// hunger is not stepped (it is not stored: the in-detail clock is the only one); an untouched squadmate is rebuilt only for the aid it can give.
void Simulation::stepStoredMedical()
{
	if (!offscreenMedical_ || combat_ == nullptr || gameData_ == nullptr || clockTick() % kStoredMedicalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	std::map<std::uint64_t, flecs::entity> squads; // by Id: a fixed order
	ecs.each([&](flecs::entity member, const state::MemberDelta& record)
	{
		if ((record.mask & state::MemberDelta::kBody) == 0 || member.has<state::Fighter>() || member.get<state::CharacterInstance>() == nullptr)
			return;
		if (const flecs::entity squad = member.target<state::MemberOf>(); squad.is_valid() && squad.get<state::Id>() != nullptr
			&& squad.get<state::SquadInstance>() != nullptr && squad.get<state::Position>() != nullptr)
			squads.emplace(squad.get<state::Id>()->value, squad);
	});
	const CombatRules& rules = combat_->rules();
	for (const auto& [squadId, squad] : squads)
	{
		const state::SquadInstance instance = *squad.get<state::SquadInstance>();
		std::vector<std::pair<std::uint64_t, flecs::entity>> members;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
		{
			members.emplace_back(id.value, member);
		});
		std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		std::vector<Stored> stored;
		for (std::size_t k = 0; k < members.size(); ++k)
		{
			const flecs::entity member = members[k].second;
			if (member.has<state::Fighter>() || member.get<state::CharacterInstance>() == nullptr)
				continue; // in the arena (a body, the player's own): not stored
			std::uint64_t base = 0;
			std::optional<state::Fighter> built = templateFighter(member, instance, k, base);
			if (!built)
				continue;
			Stored one;
			one.member = member;
			one.k = k;
			one.built = *built;
			one.now = std::move(*built);
			if (const state::MemberDelta* record = member.get<state::MemberDelta>())
			{
				record->applyTo(one.now);
				one.hadRecord = true;
			}
			if (const state::Inventory* held = member.get<state::Inventory>())
			{
				one.inventory = *held;
				one.hasInventory = true;
			}
			stored.push_back(std::move(one));
		}
		// rwh.204 / dt45: only a member whose goal list holds the Medic row (58) treats a downed ally; any carrier of a kit treats itself (269).
		for (Stored& one : stored)
			one.medicRole = canTreatAllies(one.member);
		for (std::uint64_t tick = 0; tick < kStoredMedicalTicks; ++tick)
		{
			for (Stored& medic : stored)
			{
				if (medic.now.dead || medic.now.unconscious)
					continue;
				Stored* patient = nullptr;
				for (Stored& other : stored)
				{
					if (&other != &medic && !medic.medicRole)
						continue;
					if (!other.now.dead && !other.now.wounds.empty() && (patient == nullptr || woundSum(other.now) > woundSum(patient->now)))
						patient = &other;
				}
				if (patient != nullptr)
					treatBodies(medic.now, patient->now, patient == &medic, medic.hasInventory ? &medic.inventory : nullptr, medic.taken, kTickSeconds);
			}
			for (Stored& one : stored)
			{
				const double hunger = one.now.hunger; // not stored, so not stepped
				rules.updateMedical(one.now, kTickSeconds);
				one.now.hunger = hunger;
			}
		}
		for (Stored& one : stored)
		{
			if (one.taken)
				one.member.set<state::Inventory>(std::move(one.inventory));
			std::optional<state::MemberDelta> delta = state::MemberDelta::between(one.now, one.built);
			if (one.now.dead)
				leaveCorpse(squad, one.member, one.k, delta.value_or(state::MemberDelta{}), state::CorpseDecay::kCauseBledOut);
			else if (delta)
				one.member.set<state::MemberDelta>(std::move(*delta));
			else if (one.hadRecord)
				one.member.remove<state::MemberDelta>();
		}
	}
}

} // namespace ksim
