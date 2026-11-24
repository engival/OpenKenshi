#include "ksim/ai/gatecrew.hpp"

#include "ksim/combat/combat.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/economy/trader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

namespace ksim {

// A crew squad at full detail stands as the exe's crew does (item31, GRADES_GATESQUADS_KO_LIVE.md b, rwh.207): its first living member
// (Id order, the sergeant) is the squad's leader: the squad layer's goal 180 (package row) holds the squad at its post, and this walks the
// sergeant itself onto the outside point (the squad stood clumped about 9 u off it). The next four are
// STAND_AT_NODE guards on the slots (gateStandPoint), one member per spot, each a Routine on task 51 (routines::task::GatePost: busy for the follower,
// never scored). Members beyond the fifth are off duty (rwh.207): a Routine with no goal, so the town scorer runs the package's 38 / 157 for them
// (stepRoutines scores a GateCrew squad without a SquadRoutine) and they rotate. A member that dies shifts the rest up.
// The post holders of a crew (rwh.207 s3: the members rotate between goal 38, a post on the gate's own nodes, and 157, a town node,
// so who holds a post changes). `members` is the living crew by Id, the sergeant first; returns per member its post slot (1..4) or 0
// (leader slot 0 for the sergeant). The first rotation epoch is the spawn layout (the first four guards by Id). In later epochs each
// guard draws 38 with the package's 38 / (38 + 157) weight (75 / 25 in "Gate guard -no turrets") and the first four 38-draws (by draw)
// hold the posts. A holder that stays keeps its slot; a new one takes a free slot. UNSURE: the rotation period (rwh.207: "every few
// game minutes", min-duration 0.1 + rand x 0.5 h for 38); ours is a fixed kGateRotationTicks.
std::vector<int> Simulation::postSlots(flecs::entity squad, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	std::vector<int> slot(members.size(), 0);
	if (members.empty())
		return slot;
	double w38 = 75.0;
	double w157 = 25.0;
	if (const routines::CompiledPackage* package = appliedPackage(squad))
	{
		double a = 0.0;
		double b = 0.0;
		for (const routines::Candidate& c : routines::candidatesAt(*package, routines::Role::Squad, std::fmod(trader::gameHours(clockTick()), 24.0)))
		{
			if (c.task == routines::task::StaticGuardBuilding)
				a += c.weight;
			else if (c.task == routines::task::StaticGuardTown)
				b += c.weight;
		}
		if (a + b > 0.0)
		{
			w38 = a;
			w157 = b;
		}
	}
	const double p38 = w38 / (w38 + w157);
	const std::uint64_t epoch = clockTick() / kGateRotationTicks;
	std::vector<std::pair<double, std::size_t>> holders; // (draw, member index)
	for (std::size_t index = 1; index < members.size(); ++index)
	{
		if (epoch == 0)
		{
			holders.emplace_back(static_cast<double>(index), index);
			continue;
		}
		RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kGateRotationStream), static_cast<std::size_t>(members[index].first)), static_cast<std::size_t>(epoch)));
		const double draw = dice.uniform01();
		if (draw < p38)
			holders.emplace_back(draw, index);
	}
	std::sort(holders.begin(), holders.end());
	if (holders.size() > static_cast<std::size_t>(kGateMemberSlots))
		holders.resize(kGateMemberSlots);
	std::array<bool, kGateMemberSlots + 1> taken{};
	slot[0] = 0;
	for (const auto& [draw, index] : holders)
		if (const state::Routine* r = members[index].second.get<state::Routine>();
			r != nullptr && r->goal == routines::task::GatePost && r->target.slot >= 1 && r->target.slot <= kGateMemberSlots && !taken[r->target.slot])
		{
			slot[index] = r->target.slot;
			taken[r->target.slot] = true;
		}
	int next = 1;
	for (const auto& [draw, index] : holders)
	{
		if (slot[index] != 0)
			continue;
		while (taken[next])
			++next;
		slot[index] = next;
		taken[next] = true;
	}
	return slot;
}

void Simulation::stepGateCrewPosts()
{
	if (combat_ == nullptr || combat_->empty() || navigation_ == nullptr || clockTick() % kGateCrewIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::GateCrew>() == 0)
		return;
	std::vector<std::pair<flecs::entity, std::uint64_t>> crews;
	ecs.each([&](flecs::entity squad, const state::GateCrew& crew) { crews.emplace_back(squad, crew.gate); });
	for (const auto& [squad, gate] : crews)
	{
		const GateSite* site = nullptr;
		for (const GateSite& candidate : world().gateSites())
			if (candidate.key == gate)
				site = &candidate;
		if (site == nullptr)
			continue;
		std::vector<std::pair<std::uint64_t, flecs::entity>> members;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
		{
			const state::Fighter* f = combat_->find(id.value);
			if (f != nullptr && !f->dead && !f->playerControlled)
				members.emplace_back(id.value, member);
		});
		std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		const std::vector<int> slotOf = postSlots(squad, members);
		for (std::size_t index = 0; index < members.size(); ++index)
		{
			const auto& [id, member] = members[index];
			const state::Routine* routine = member.get<state::Routine>();
			if (index == 0)
			{
				// The sergeant leads (residents.cpp leaderOf: the first member with no Routine): a former guard gives its post up.
				if (routine != nullptr && routine->goal == routines::task::GatePost)
				{
					routineBook_.release(id);
					member.remove<state::Routine>();
				}
			}
			const std::optional<GateStand> stand = index != 0 && slotOf[index] == 0 ? std::nullopt : gateStandPoint(*site, slotOf[index]);
			if (!stand)
			{
				if (index == 0) // not reached: index 0 always has a stand point
					continue;
				if (routine == nullptr)
				{
					state::Routine off;
					off.since = clockTick();
					off.nextScore = clockTick();
					member.set<state::Routine>(std::move(off));
				} else if (routine->goal == routines::task::GatePost) {
					state::Routine off = *routine; // a post holder handed its post over: the scorer takes it from here
					off.goal = 0;
					off.target = {};
					off.nextScore = clockTick();
					member.set<state::Routine>(std::move(off));
				}
				continue;
			}
			if (index != 0 && (routine == nullptr || routine->goal != routines::task::GatePost || routine->target.slot != slotOf[index]))
			{
				const bool arriving = routine == nullptr || routine->goal != routines::task::GatePost;
				routineBook_.release(id);
				state::Routine post;
				post.since = clockTick();
				post.goal = routines::task::GatePost;
				post.target.slot = static_cast<std::uint16_t>(slotOf[index]);
				post.nextScore = ~std::uint64_t(0);
				member.set<state::Routine>(std::move(post));
				if (arriving) // taking a post over from the scorer's walk: drop that route, the post's walk replaces it
					if (state::Fighter* walker = combat_->find(id); walker != nullptr && walker->walking() && walker->routeSeq == 0)
						walker->clearRoute();
			}
			state::Fighter& f = *combat_->find(id);
			if (f.unconscious || f.down || f.target != 0 || f.carriedBy != 0 || f.carrying != 0 || f.walking()
				|| member.has<state::InteractTask>() || member.has<state::DoorStep>() || member.has<state::CarryStep>())
				continue;
			// The spot's own height is the sergeant's: a guard coming back from town stands on another level than the landing.
			const state::Fighter* sergeant = combat_->find(members[0].first);
			const auto spot = navigation_->walker().snap({stand->x, sergeant != nullptr ? sergeant->y : f.y, stand->z}, 40.0);
			if (!spot || std::hypot(f.x - spot->x, f.z - spot->z) <= kGatePostReach)
				continue;
			const Route route = routeRetry(id, f, spot->x, spot->z, spot->y, true);
			if (!route.found || route.points.empty())
				continue;
			f.clearRoute();
			f.followTarget = 0; // the squad's follow keeps a guard by the leader: the post is its task now
			std::vector<state::RoutePoint> points;
			for (std::size_t p = 0; p < route.points.size(); ++p)
				points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
			setRoute(f, std::move(points), navigation_.get(), body(f));
			++f.routeVersion;
			f.routeSeq = 0;
			f.routeChase = false;
			f.routeApproach = route.approach;
			f.routeTries = 0;
			mirrorFighter(id);
		}
	}
}

void Simulation::removeGate(std::uint64_t doorKey)
{
	flecs::world& ecs = ecsMutable();
	state::GatesGone gone;
	if (const state::GatesGone* current = ecs.get<state::GatesGone>())
		gone = *current;
	const auto at = std::lower_bound(gone.keys.begin(), gone.keys.end(), doorKey);
	if (at != gone.keys.end() && *at == doorKey)
		return;
	gone.keys.insert(at, doorKey);
	ecs.set<state::GatesGone>(std::move(gone));
}

// Once a second. A crew whose gate exists and is not held by a lower-Id crew keeps it. The others rebind in Id order through
// resolveGate (gatebind.hpp): the home building's gate, then the gateway of the crew's town nearest the squad that no other crew
// holds, else GateCrew is removed and the squad is an ordinary resident (goal ManGate ends, the AI selects again). A rebound crew
// takes the new gate's outside point as its post and re-selects, so it walks there.
void Simulation::stepGateBinding()
{
	if (clockTick() % kGateCrewIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::GateCrew>() == 0) // the uncached ecs.each() below walks every table; most worlds have no crews
		return;
	const state::GatesGone* goneList = ecs.get<state::GatesGone>();
	const std::vector<GateSite>& sites = world().gateSites(); // placement order
	const auto siteOf = [&](std::uint64_t key) -> const GateSite*
	{
		for (const GateSite& site : sites)
			if (site.key == key)
				return &site;
		return nullptr;
	};
	const auto exists = [&](std::uint64_t key)
	{
		return siteOf(key) != nullptr && (goneList == nullptr || !std::binary_search(goneList->keys.begin(), goneList->keys.end(), key));
	};
	std::vector<std::pair<std::uint64_t, flecs::entity>> crews; // (squad Id, entity)
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::GateCrew&) { crews.emplace_back(id.value, entity); });
	if (crews.empty())
		return;
	std::sort(crews.begin(), crews.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::set<std::uint64_t> held;
	std::vector<flecs::entity> orphans;
	for (const auto& [id, entity] : crews)
	{
		const std::uint64_t gate = entity.get<state::GateCrew>()->gate;
		if (exists(gate) && held.insert(gate).second)
			continue;
		orphans.push_back(entity);
	}
	if (orphans.empty())
		return;
	// The towns by Id, for the gate -> town rule (nearest centre, first on a tie: the same as spawn and DoorSystem::bind).
	std::vector<std::tuple<std::uint64_t, flecs::entity_t, double, double>> towns;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::TownState& town)
	{
		towns.emplace_back(id.value, entity.id(), town.x, town.z);
	});
	std::sort(towns.begin(), towns.end());
	const auto townOfGate = [&](const GateSite& site) -> flecs::entity_t
	{
		const std::size_t best = nearestTownIndex(site.x, site.z, towns.size(), [&](std::size_t i)
		{
			return std::pair<double, double>(std::get<2>(towns[i]), std::get<3>(towns[i]));
		});
		return best == towns.size() ? 0 : std::get<1>(towns[best]);
	};
	for (flecs::entity entity : orphans)
	{
		const state::GateCrew crew = *entity.get<state::GateCrew>();
		const flecs::entity_t town = entity.get<state::SquadInstance>()->homeTown.id;
		const state::Position* at = entity.get<state::Position>();
		const double fromX = at != nullptr ? at->x : crew.postX;
		const double fromZ = at != nullptr ? at->z : crew.postZ;
		const GateChoice choice = resolveGate(crew.gate, crew.home,
			[&](std::uint64_t key) { return exists(key) && held.count(key) == 0; },
			[&]() -> std::uint64_t
			{
				const GateSite* best = nullptr;
				double bestDistance = std::numeric_limits<double>::infinity();
				for (const GateSite& site : sites)
				{
					if (!exists(site.key) || held.count(site.key) != 0 || townOfGate(site) != town)
						continue;
					const double far = std::hypot(site.outX - fromX, site.outZ - fromZ);
					if (far < bestDistance)
					{
						bestDistance = far;
						best = &site;
					}
				}
				return best != nullptr ? best->key : 0;
			});
		if (state::GroupAI* ai = entity.get_mut<state::GroupAI>())
		{
			ai->goal = state::GoalKind::None;
			ai->route.clear();
		}
		if (choice.source == GateSource::None)
		{
			entity.remove<state::GateCrew>();
			continue;
		}
		const GateSite& site = *siteOf(choice.key);
		const double length = std::hypot(site.outX - site.inX, site.outZ - site.inZ);
		state::GateCrew rebound = crew;
		rebound.gate = site.key;
		rebound.postX = site.outX;
		rebound.postZ = site.outZ;
		rebound.faceX = (site.outX - site.inX) / length;
		rebound.faceZ = (site.outZ - site.inZ) / length;
		entity.set<state::GateCrew>(rebound);
		held.insert(site.key);
	}
}

} // namespace ksim
