#include "ksim/squad_alive.hpp"

#include "ksim/state.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <string_view>

namespace ksim {

double injurySeverity(const state::Fighter& fighter) noexcept
{
	double minAll = 1.0;
	double minVital = 1.0;
	double leak = 0.0;
	bool natural = false;
	for (const state::FighterPart& part : fighter.parts)
	{
		if (part.severed() && part.health + part.pool < 0.0)
			leak += (part.health + part.pool) * -0.002;
		if (part.robotic)
			continue;
		natural = true;
		double health = part.maxHealth > 0.0 ? part.health / part.maxHealth : 1.0;
		if (health > 0.999)
			health = 1.0;
		minAll = std::min(minAll, health);
		if (part.vital)
			minVital = std::min(minVital, health);
	}
	if (!natural)
		return 0.0;
	double severity = std::max(0.5 * (1.0 - minAll), 1.0 - minVital);
	const double maxBlood = (fighter.maxBloodRace - fighter.minBlood) * fighter.skills.strength * 0.01 + fighter.minBlood;
	const double bleed = maxBlood > 0.0 ? (1.0 - fighter.blood / maxBlood) * std::max(0.0, fighter.bleeding - 1.5 * leak) * 10.0 : 0.0;
	if (severity < bleed && bleed > 0.2)
		severity = bleed;
	return severity;
}

bool isDown(const state::Fighter& fighter) noexcept
{
	return fighter.dead || fighter.carriedBy != 0 || fighter.down || fighter.unconscious;
}

std::size_t nominalSquadSize(const kdata::Record& squadTemplate, double sizeMultiplier)
{
	std::int64_t size = 0;
	const kdata::RefCategory* leaders = squadTemplate.findRefs("leader");
	if (leaders != nullptr && !leaders->entries.empty())
		size = 1;
	for (const std::string_view list : {"squad", "squad2", "animals", "animals2"})
	{
		const kdata::RefCategory* refs = squadTemplate.findRefs(list);
		if (refs == nullptr)
			continue;
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (!entry.opaqueEntry)
				size += entry.v1 == 0 || entry.v1 == 100 ? entry.v0 : (entry.v0 + entry.v1) / 2;
		}
	}
	if (sizeMultiplier < 1.0)
		size = static_cast<std::int64_t>(static_cast<double>(size) * sizeMultiplier);
	return static_cast<std::size_t>(std::max<std::int64_t>(size, 1));
}

namespace {

// One member into the count; returns whether it is effective. 14079acf0: effective = not dead and (not down, or severity strictly
// under 0.2).
bool countMember(SquadVitals& vitals, const state::Fighter* fighter)
{
	const bool dead = fighter != nullptr && fighter->dead;
	const bool effective = !dead && (fighter == nullptr || !isDown(*fighter) || injurySeverity(*fighter) < kEffectiveSeverity);
	++vitals.members;
	vitals.dead += dead ? 1 : 0;
	vitals.effective += effective ? 1 : 0;
	return effective;
}

// A world squad's member (MemberOf the squad): counted, and the leader by its role.
void countWorldMember(SquadVitals& vitals, flecs::entity member)
{
	const bool effective = countMember(vitals, member.get<state::Fighter>());
	const state::MemberRole* role = member.get<state::MemberRole>();
	if (role != nullptr && role->list == state::kRoleLeader)
	{
		vitals.hasLeader = true;
		vitals.leaderDown = !effective;
	}
}

// The thresholds over the counted members.
SquadVitals finish(SquadVitals vitals, flecs::entity squad, const kdata::GameData* data)
{
	if (vitals.members == 0)
		return vitals; // no instantiated members: the exe's test returns true (intact = true)
	vitals.nominal = vitals.members;
	if (data != nullptr)
	{
		if (const state::SquadInstance* instance = squad.get<state::SquadInstance>())
		{
			if (const kdata::Record* record = data->find(instance->templateSid))
				vitals.nominal = nominalSquadSize(*record);
		}
	}
	if (vitals.nominal < kIntactSmallSquad)
		vitals.intact = vitals.hasLeader ? !vitals.leaderDown : vitals.effective > 0;
	else
		vitals.intact = static_cast<float>(vitals.effective)
			>= static_cast<float>(vitals.nominal) * (vitals.leaderDown ? kIntactFractionLeaderDown : kIntactFraction);
	return vitals;
}

} // namespace

SquadVitals squadVitals(const flecs::world& ecs, flecs::entity squad, const kdata::GameData* data)
{
	SquadVitals vitals;
	if (const state::PlayerSquad* player = squad.get<state::PlayerSquad>())
	{
		// A player squad (player_squads.hpp) has no MemberOf: its members are the list, leader first.
		const std::uint64_t leader = player->members.empty() ? 0 : player->members.front();
		vitals.hasLeader = leader != 0;
		ecs.each([&](flecs::entity, const state::Id& id, const state::Fighter& fighter)
		{
			if (std::find(player->members.begin(), player->members.end(), id.value) == player->members.end())
				return;
			const bool effective = countMember(vitals, &fighter);
			if (id.value == leader)
				vitals.leaderDown = !effective;
		});
	} else {
		ecs.query_builder<>().with<state::MemberOf>(squad).build().each([&](flecs::entity member) { countWorldMember(vitals, member); });
	}
	return finish(vitals, squad, data);
}

SquadVitals squadVitals(flecs::entity squad, const std::vector<flecs::entity>& members, const kdata::GameData* data)
{
	SquadVitals vitals;
	for (const flecs::entity member : members)
		countWorldMember(vitals, member);
	return finish(vitals, squad, data);
}

} // namespace ksim
