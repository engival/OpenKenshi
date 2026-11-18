#include "resident_data.hpp"
#include "ksim/combat.hpp"
#include "ksim/factions.hpp"
#include "ksim/simulation.hpp"
#include "ksim/state.hpp"
#include "ksim/token_fight.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

namespace ksim {

namespace {

// One living, conscious stored member of a side: its slot in the squad's Id order and its score (the Fighter is built only for one that falls).
struct Living
{
	flecs::entity member;
	std::size_t k = 0;
	double score = 0.0;
};

struct Side
{
	std::uint64_t id = 0;
	flecs::entity squad;
	state::SquadInstance instance;
	std::vector<Living> living;
	double strength = 0.0;
};

struct Token
{
	std::uint64_t id = 0;
	flecs::entity squad;
	double x = 0.0;
	double z = 0.0;
};

} // namespace

bool Simulation::squadsHostile(const state::SquadInstance& a, const state::SquadInstance& b) const
{
	const flecs::world& ecs = world_.ecs();
	const bool aLive = a.faction.id != 0 && ecs.is_alive(a.faction.id);
	const bool bLive = b.faction.id != 0 && ecs.is_alive(b.faction.id);
	if (aLive != bLive)
		return true; // a wild squad is hostile to every faction
	if (!aLive || a.faction.id == b.faction.id)
		return false; // two wild squads share a team (worldTeam), as one faction does
	return factionsHostile(*gameData_, ecs.entity(a.faction.id), ecs.entity(b.faction.id))
		|| factionsHostile(*gameData_, ecs.entity(b.faction.id), ecs.entity(a.faction.id));
}

// Off-screen fights (k16.6.46; COMBAT_OPTIONS.md 9), behind setOffscreenFights, once per group-layer step. A token is a world squad none of whose
// members is an alive Fighter and which is not built (a Full-detail squad never fights here). Two tokens whose factions are hostile (squadsHostile:
// a factionless squad is hostile to every faction; else either relation hostile) within tokenfight::kEncounterRadius fight once, a pair at most once a game hour: strength = the sum over the living,
// conscious members of tokenfight::memberScore, the winner by tokenfight::resolve. The loser's weakest ceil(n/2) members fall and it routs;
// the winner's weakest falls with the loser's chance. A fallen member dies now (leaveCorpse, cause 2) with world.offscreen_medical off, else it
// is KO'd and bleeding in its MemberDelta and the stored medical step decides. OURS; UNSURE / deferred: playtesting: the radius, the score, the
// wound, the rout, a pair clock lost by a load (not saved).
void Simulation::stepTokenFights()
{
	if (!offscreenFights_ || combat_ == nullptr || gameData_ == nullptr || combatData_ == nullptr || residentData_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	for (auto it = fightPairs_.begin(); it != fightPairs_.end();)
		it = now - it->second >= tokenfight::kPairTicks ? fightPairs_.erase(it) : std::next(it);

	std::set<std::uint64_t> built; // squads with an alive Fighter: in detail
	for (const auto& entry : combat_->members())
	{
		if (!entry.fighter.dead && entry.fighter.squad != 0)
			built.insert(entry.fighter.squad);
	}
	std::vector<Token> tokens;
	flecs::entity player;
	ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState&) { player = entity; });
	ecs.each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.source == state::SpawnSource::Dismissed || residentData_->world.count(id.value) != 0 || built.count(id.value) != 0)
			return;
		if (!player.is_valid() || instance.faction.id != player.id())
			tokens.push_back({id.value, squad, at.x, at.z});
	});
	++fightStats_.steps;
	fightStats_.tokensMax = std::max<std::uint64_t>(fightStats_.tokensMax, tokens.size());
	if (tokens.size() < 2)
		return;
	std::sort(tokens.begin(), tokens.end(), [](const Token& a, const Token& b) { return a.id < b.id; });
	// A grid of the encounter radius: a token looks at its own and the eight cells around it.
	const auto cell = [](double v) { return static_cast<std::int64_t>(std::floor(v / tokenfight::kEncounterRadius)); };
	std::map<std::pair<std::int64_t, std::int64_t>, std::vector<std::size_t>> grid;
	for (std::size_t i = 0; i < tokens.size(); ++i)
		grid[{cell(tokens[i].x), cell(tokens[i].z)}].push_back(i);
	std::vector<std::pair<std::size_t, std::size_t>> meetings;
	for (std::size_t i = 0; i < tokens.size(); ++i)
	{
		for (std::int64_t dx = -1; dx <= 1; ++dx)
		{
			for (std::int64_t dz = -1; dz <= 1; ++dz)
			{
				const auto found = grid.find({cell(tokens[i].x) + dx, cell(tokens[i].z) + dz});
				if (found == grid.end())
					continue;
				for (const std::size_t j : found->second)
				{
					if (j > i && std::hypot(tokens[i].x - tokens[j].x, tokens[i].z - tokens[j].z) <= tokenfight::kEncounterRadius
						&& squadsHostile(*tokens[i].squad.get<state::SquadInstance>(), *tokens[j].squad.get<state::SquadInstance>()))
						meetings.emplace_back(i, j);
				}
			}
		}
	}
	fightStats_.meetings += meetings.size();
	if (meetings.empty())
		return;
	std::sort(meetings.begin(), meetings.end(), [&](const auto& a, const auto& b)
	{
		return std::tie(tokens[a.first].id, tokens[a.second].id) < std::tie(tokens[b.first].id, tokens[b.second].id);
	});

	const CombatRules& rules = combat_->rules();
	// The member as promotion would build it: its template plus its record.
	const auto fighterOf = [&](flecs::entity member, const state::SquadInstance& instance, std::size_t k, const state::MemberDelta* record)
	{
		std::uint64_t base = 0;
		std::optional<state::Fighter> built = templateFighter(member, instance, k, base);
		if (built && record != nullptr)
			record->applyTo(*built);
		return built;
	};
	// A side's living members, scored; empty when the squad has none (nothing to fight with).
	const auto gather = [&](const Token& token, Side& side)
	{
		side.id = token.id;
		side.squad = token.squad;
		side.instance = *token.squad.get<state::SquadInstance>();
		std::vector<std::pair<std::uint64_t, flecs::entity>> members;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(token.squad).build().each([&](flecs::entity member, const state::Id& id)
		{
			members.emplace_back(id.value, member);
		});
		std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (std::size_t k = 0; k < members.size(); ++k)
		{
			const flecs::entity member = members[k].second;
			if (member.has<state::Fighter>() || member.get<state::CharacterInstance>() == nullptr)
				continue; // a body, or in the arena
			const state::MemberDelta* record = member.get<state::MemberDelta>();
			if (record != nullptr && (record->mask & state::MemberDelta::kBody) != 0 && (record->dead || record->unconscious))
				continue;
			Living one;
			one.member = member;
			one.k = k;
			const auto known = record == nullptr ? memberScores_.find(members[k].first) : memberScores_.end();
			if (known != memberScores_.end())
			{
				one.score = known->second; // an untouched member is its template: scored once
			} else {
				const std::optional<state::Fighter> now = fighterOf(member, side.instance, k, record);
				if (!now)
					continue;
				one.score = tokenfight::memberScore(*now);
				if (record == nullptr)
					memberScores_[members[k].first] = one.score;
			}
			side.strength += one.score;
			side.living.push_back(one);
		}
	};
	// The `count` weakest living members fall (ties by slot), in the way the setting says.
	const auto fall = [&](Side& side, std::size_t count)
	{
		std::stable_sort(side.living.begin(), side.living.end(), [](const Living& a, const Living& b) { return a.score < b.score; });
		for (std::size_t i = 0; i < count && i < side.living.size(); ++i)
		{
			const Living& one = side.living[i];
			std::uint64_t base = 0;
			std::optional<state::Fighter> built = templateFighter(one.member, side.instance, one.k, base);
			if (!built)
				continue;
			state::Fighter now = *built;
			if (const state::MemberDelta* record = one.member.get<state::MemberDelta>())
				record->applyTo(now);
			memberScores_.erase(one.member.get<state::Id>()->value);
			now.unconscious = true;
			if (offscreenMedical_)
			{
				now.koTimer = rules.knockoutTime(now);
				now.wounds.push_back(combatData_->k.bleedRate * tokenfight::kFallWound);
				if (const std::optional<state::MemberDelta> delta = state::MemberDelta::between(now, *built))
					one.member.set<state::MemberDelta>(*delta);
			} else {
				now.dead = true;
				leaveCorpse(side.squad, one.member, one.k, state::MemberDelta::between(now, *built).value_or(state::MemberDelta{}), state::CorpseDecay::kCauseTokenFight);
			}
		}
	};

	std::set<std::uint64_t> fought;
	for (const auto& [i, j] : meetings)
	{
		const Token& a = tokens[i];
		const Token& b = tokens[j];
		const std::pair<std::uint64_t, std::uint64_t> pair{a.id, b.id};
		if (fought.count(a.id) != 0 || fought.count(b.id) != 0 || fightPairs_.count(pair) != 0)
		{
			++fightStats_.cooling;
			continue;
		}
		Side sideA;
		Side sideB;
		gather(a, sideA);
		gather(b, sideB);
		if (sideA.living.empty() || sideB.living.empty())
		{
			++fightStats_.empty;
			continue;
		}
		fought.insert(a.id);
		fought.insert(b.id);
		fightPairs_[pair] = now;
		const tokenfight::Outcome out = tokenfight::resolve(seed_, a.id, b.id, now, sideA.strength, sideB.strength);
		Side& winner = out.aWins ? sideA : sideB;
		Side& loser = out.aWins ? sideB : sideA;
		const std::size_t loserFalls = tokenfight::loserLosses(loser.living.size());
		const std::size_t winnerFalls = out.winnerFalls ? 1 : 0;
		fall(loser, loserFalls);
		fall(winner, winnerFalls);
		// The loser routs: its group layer picks the way home at once (home urgency at its most, no cooldown on it). A squad with no home stays.
		if (loserFalls < loser.living.size() && loser.instance.homeTown.id != 0)
		{
			if (const state::GroupAI* held = loser.squad.get<state::GroupAI>())
			{
				state::GroupAI ai = *held;
				ai.goal = state::GoalKind::None;
				ai.route.clear();
				ai.length = 0.0;
				ai.along = 0.0;
				ai.homeTick = 0;
				ai.cooldowns.erase(std::remove_if(ai.cooldowns.begin(), ai.cooldowns.end(), [](const state::GoalCooldown& c) { return c.kind == state::GoalKind::GoHome; }),
					ai.cooldowns.end());
				loser.squad.set<state::GroupAI>(std::move(ai));
			}
		}
		++fightStats_.fights;
		std::ostringstream line;
		line.precision(3);
		line << "offscreen fight " << a.id << " vs " << b.id << " at " << std::lround((a.x + b.x) / 2.0) << "," << std::lround((a.z + b.z) / 2.0) << ": "
			<< winner.id << " wins p=" << out.chanceA << " losses A=" << (out.aWins ? winnerFalls : loserFalls) << " B=" << (out.aWins ? loserFalls : winnerFalls);
		logTask(line.str());
	}
}

} // namespace ksim
