// The raid's phase packages on the whole force (task raid-phases; kinspect rwh.53 sections 1, 2.1, 4, rwh.44 section 6): a campaign
// squad's Blackboard gets the package of its campaign's state - pt1 (assemble) while the force gathers and walks, pt2 (the loot or assault
// package) on site, retreat after the on-site time - pushed as the 24 h contract of blackboard::consider (dt19 section 2 step 1). The
// leader squad of the force takes the `leader` package, every other squad the `others` one (an empty `others` list was filled with the
// leader's at load, CampaignInfo::phases). The phase is the squad's own order status, so it sits on the order, not on a built squad: a
// squad rebuilt at another detail level reads the same one. When the order is over the campaign hook is empty and the contract, a war class
// with no instance left, finishes at once (blackboard::finished kWarBase / kWarAssault).

#include "ksim/simulation.hpp"

#include "ksim/factions.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace ksim {

namespace {

// The CampaignInstance state an order status stands for, -1 once the order is over (the instance is cleared).
int phaseOf(state::OrderStatus status) noexcept
{
	switch (status)
	{
	case state::OrderStatus::Issued:
	case state::OrderStatus::Outbound:
		return static_cast<int>(kPhaseAssemble);
	case state::OrderStatus::Staying:
		return static_cast<int>(kPhaseAssault);
	case state::OrderStatus::Retreating:
	case state::OrderStatus::Returning:
		return static_cast<int>(kPhaseRetreat);
	default:
		return -1;
	}
}

std::string forceKey(const state::RaidOrder& order)
{
	return order.campaign + '/' + std::to_string(order.target.id);
}

// A force with no campaign (a scenario's squad) is its own force.
std::string holdKey(const state::RaidOrder& order, std::uint64_t squadId)
{
	return order.campaign.empty() ? "#" + std::to_string(squadId) : forceKey(order);
}

// A held force's GroupAI::until and order deadline: the squad stays until the hold ends or it is killed.
constexpr std::uint64_t kRaidHold = ~std::uint64_t(0);
// A held force that has no occupier for this long (its own 4 s rule has run, and the package has not started again) is released; also the grace
// at the start of a hold, for the package to begin.
constexpr std::uint64_t kHoldGraceTicks = static_cast<std::uint64_t>(60.0 / kTickSeconds);

} // namespace

void Simulation::refreshRaidLeaders()
{
	raidLeaders_.clear();
	if (!raidQuery_)
		raidQuery_.emplace(world_.ecs().query<const state::Id, const state::GroupAI>());
	raidQuery_->each([&](const state::Id& id, const state::GroupAI& group)
	{
		if (group.order.campaign.empty() || phaseOf(group.order.status) < 0)
			return;
		const auto [at, fresh] = raidLeaders_.emplace(forceKey(group.order), id.value);
		if (!fresh && id.value < at->second)
			at->second = id.value; // the force's first squad (the faction raises them in order) leads
	});
}

blackboard::Campaign Simulation::raidCampaign(flecs::entity squad, std::uint64_t squadId) const
{
	blackboard::Campaign out;
	const state::GroupAI* group = squad.get<state::GroupAI>();
	if (group == nullptr || group->order.campaign.empty() || ai_ == nullptr || ai_->config().factions == nullptr)
		return out;
	const int phase = phaseOf(group->order.status);
	const CampaignInfo* info = ai_->config().factions->campaign(group->order.campaign);
	if (phase < 0 || info == nullptr)
		return out;
	const auto leader = raidLeaders_.find(forceKey(group->order));
	const bool leads = leader == raidLeaders_.end() || leader->second == squadId;
	// Victory AI (rwh.53 section 4.1, honoured behind the switch): the Retreating status is the force holding its target after the on-site
	// time, i.e. it won the town, so a campaign that names a victory package runs it there; the walk home (Returning) is the retreat's.
	const bool victory = ai_->config().victoryAi && group->order.status == state::OrderStatus::Retreating && !info->victory.leader.empty();
	const CampaignInfo::Phase& packages = victory ? info->victory : info->phases[static_cast<std::size_t>(phase)];
	// No package for the state: the exe logs "<campaign> is missing AI data!" and pushes nothing.
	out.present = blackboard::readPackage(*gameData_, leads ? packages.leader : packages.others, 0, out.package);
	return out;
}

// The win of a force at the end of its stay (victory AI): its leader (the squad's first member in Id order that is alive and not the player's)
// stands in the target town, conscious, and no fighter able to fight that is hostile to the force stands in the town. UNSURE (deferred:
// playtesting): the original never loads the victory AI (rwh.61), so it has no win test; ours reads the built Fighters, a defender not built is
// not seen.
bool Simulation::raidWon(const state::GroupAI& group, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members) const
{
	const state::Fighter* lead = nullptr;
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat_->find(id);
		if (f != nullptr && !f->dead && !f->playerControlled)
		{
			lead = f;
			break;
		}
	}
	if (lead == nullptr || lead->down || lead->unconscious || townEntityAt(lead->x, lead->z) != group.order.target.id)
		return false;
	for (const CombatArena::Member& other : combat_->members())
	{
		const state::Fighter& f = other.fighter;
		if (!f.dead && !f.down && !f.unconscious && combat_->hostileFactions(f.faction, lead->faction) && townEntityAt(f.x, f.z) == group.order.target.id)
			return false;
	}
	return true;
}

// Once a second per built raid squad (the selector's pass), behind AiSystem::Config::victoryAi. A force whose stay is over (Retreating) and
// that won holds its town: the leader squad records the win and the next pass applies it to the force (applyRaidVictory). A held force records
// when a member last occupied, from the package's own situation.
void Simulation::watchRaidVictory(flecs::entity squad, std::uint64_t squadId, const blackboard::Situation& situation,
	const std::vector<std::pair<std::uint64_t, flecs::entity>>& members, std::uint64_t now)
{
	const state::GroupAI* group = squad.get<state::GroupAI>();
	if (group == nullptr || group->goal != state::GoalKind::Raid)
		return;
	const std::string key = holdKey(group->order, squadId);
	if (group->until == kRaidHold)
	{
		RaidHold& hold = raidHolds_.try_emplace(key, RaidHold{now, now, now}).first->second;
		hold.looked = now;
		if (situation.occupying)
			hold.occupied = now;
		return;
	}
	if (group->order.status != state::OrderStatus::Retreating)
		return;
	// A campaign names its victory package (the data of a campaign without one: today's retreat); a force without a campaign has none to name.
	if (!group->order.campaign.empty())
	{
		const CampaignInfo* info = ai_->config().factions != nullptr ? ai_->config().factions->campaign(group->order.campaign) : nullptr;
		if (info == nullptr || info->victory.leader.empty())
			return;
	}
	const auto leader = raidLeaders_.find(forceKey(group->order));
	if (!group->order.campaign.empty() && leader != raidLeaders_.end() && leader->second != squadId)
		return; // the force's leader squad judges it
	if (raidWon(*group, members))
		raidWon_.insert(key);
}

// The pass after: a won force's squads hold (no retreat, no deadline), and a held force whose occupation ended walks home.
void Simulation::applyRaidVictory(std::uint64_t now)
{
	if (ai_ == nullptr || !ai_->config().victoryAi)
	{
		raidHolds_.clear();
		raidWon_.clear();
		return;
	}
	// Released: looked at in the last pass (a force out of detail is not judged) and no occupier for the grace.
	std::set<std::string> released;
	for (auto it = raidHolds_.begin(); it != raidHolds_.end();)
	{
		if (now - it->second.looked > kTicksPerGameHour * 2)
		{
			it = raidHolds_.erase(it); // gone (killed or released elsewhere)
			continue;
		}
		if (now - it->second.looked <= kHoldGraceTicks && now - std::max(it->second.seen, it->second.occupied) >= kHoldGraceTicks)
			released.insert(it->first);
		++it;
	}
	if (raidWon_.empty() && released.empty())
		return;
	ecsMutable().each([&](const state::Id& id, state::GroupAI& group)
	{
		if (group.goal != state::GoalKind::Raid || group.order.status != state::OrderStatus::Retreating)
			return;
		const std::string key = holdKey(group.order, id.value);
		if (group.until != kRaidHold && raidWon_.count(key) != 0)
		{
			group.until = kRaidHold;
			group.order.deadline = kRaidHold;
		} else if (group.until == kRaidHold && released.count(key) != 0) {
			group.until = now; // the way home, as a retreat's end
			group.order.deadline = now + 24 * kTicksPerGameHour;
		}
	});
	for (const std::string& key : released)
		raidHolds_.erase(key);
	raidWon_.clear();
}

} // namespace ksim
