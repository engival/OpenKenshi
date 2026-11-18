// The world-squad log (server option --log-world-squads N, docs/RUNBOOK.md): one line per BUILT world squad every N game minutes, and one line
// at each promote / demote. Off (no sink) it costs one empty std::function test per resident pass and per promote / demote.
#include "ksim/simulation.hpp"

#include "resident_data.hpp"

#include "ksim/clock.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <vector>

namespace ksim {

namespace
{

const char* goalName(state::GoalKind goal)
{
	static const char* const names[] = {"None", "Idle", "GoHome", "TravelTo", "Patrol", "Roam", "Raid", "ManGate"};
	const std::size_t index = static_cast<std::size_t>(goal);
	return index < std::size(names) ? names[index] : "?";
}

} // namespace

void Simulation::setWorldSquadLog(std::uint64_t everyGameMinutes, std::function<void(const std::string&)> sink)
{
	worldSquadLog_ = std::move(sink);
	worldSquadLogTicks_ = static_cast<std::uint64_t>(static_cast<double>(everyGameMinutes) * kTicksPerGameMinute + 0.5);
	worldSquadLogNext_ = 0;
}

std::string Simulation::worldSquadLine(const char* what, flecs::entity squad) const
{
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	const state::Position* at = squad.get<state::Position>();
	if (instance == nullptr)
		return {};
	std::vector<std::pair<std::uint64_t, flecs::entity>> members;
	squad.world().query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		members.emplace_back(id.value, member);
	});
	std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	// The leader is the carry step's: the first living, conscious, non-player member without a Routine, in Id order.
	std::size_t alive = 0;
	const state::Fighter* leader = nullptr;
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
		if (f == nullptr || f->dead || f->playerControlled || member.has<state::Routine>())
			continue;
		++alive;
		if (leader == nullptr && !f->unconscious)
			leader = f;
	}
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(instance->templateSid) : nullptr;
	std::string faction = "-";
	if (instance->faction.id != 0 && world_.ecs().is_alive(instance->faction.id))
	{
		const state::FactionState* state = world_.ecs().entity(instance->faction.id).get<state::FactionState>();
		const kdata::Record* factionRecord = state != nullptr && gameData_ != nullptr ? gameData_->find(state->templateSid) : nullptr;
		faction = factionRecord != nullptr ? factionRecord->name : "?";
	}
	const state::GroupAI* group = squad.get<state::GroupAI>();
	std::ostringstream line;
	line << "world squad " << what << " " << squad.get<state::Id>()->value << " '" << (record != nullptr ? record->name : instance->templateSid)
		<< "' faction '" << faction << "'";
	if (leader != nullptr)
		line << " team " << leader->team << " hostile " << (leader->team != 0 ? 1 : 0);
	line << " members " << alive << "/" << members.size();
	if (leader != nullptr)
		line << " leader (" << std::lround(leader->x) << ", " << std::lround(leader->z) << ")" << (leader->target != 0 ? " fighting" : "");
	if (at != nullptr)
		line << " token (" << std::lround(at->x) << ", " << std::lround(at->z) << ")";
	if (group != nullptr)
		line << " goal " << goalName(group->goal) << " to (" << std::lround(group->goalX) << ", " << std::lround(group->goalZ) << ") along "
			<< std::lround(group->along) << "/" << std::lround(group->length);
	return line.str();
}

void Simulation::logWorldSquadEvent(const char* what, flecs::entity squad) const
{
	if (worldSquadLog_)
		worldSquadLog_(worldSquadLine(what, squad));
}

void Simulation::logWorldSquads()
{
	if (!worldSquadLog_ || residentData_ == nullptr || worldSquadLogTicks_ == 0 || clockTick() < worldSquadLogNext_)
		return;
	worldSquadLogNext_ = clockTick() + worldSquadLogTicks_;
	const GameTime time = gameTimeAt(clockTick());
	std::vector<std::pair<std::uint64_t, flecs::entity>> built;
	ecsMutable().each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance&)
	{
		if (residentData_->world.count(id.value) != 0)
			built.emplace_back(id.value, squad);
	});
	std::sort(built.begin(), built.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::ostringstream head;
	head << "world squads at day " << time.day << " " << time.hour << ":" << (time.minute < 10 ? "0" : "") << time.minute << ": " << built.size() << " built";
	worldSquadLog_(head.str());
	for (const auto& [id, squad] : built)
		worldSquadLog_(worldSquadLine("built", squad));
}

} // namespace ksim
