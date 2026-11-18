#include "ksim/combat/corpses.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "kworld/coords.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace ksim {

// The exe's dead-character branch (1405ccd90), run on our terms (ARCHITECTURE.md "Corpses", b0's default, tagged deferred):
//   - the first evaluation after death stamps the time (state::CorpseDecay, game hours); later ones destroy the corpse when
//     now - stamp > 12 h; a carried corpse's stamp is clamped to >= now - 6 h, so it never decays while carried;
//   - "evaluated" = our "on screen": an NPC corpse is evaluated only inside a player's full-detail area (or while carried); a player
//     corpse (a player-controlled character, or one of a player faction) always;
//   - "zone unloaded" = no player area covers the corpse and no player is within the original's load range (corpses.hpp): the corpse
//     is destroyed ("corpse unloaded"), whether or not it was ever evaluated. Not applied to a player corpse (UNSURE: the exe would);
//   - nothing is evaluated while there is no player area at all (nobody plays: what exists stays, as the navmesh coverage does);
//   - destroying a corpse drops nothing extra: its Inventory goes with the entity (the exe's destroy releases the inventory too).
// The squad needs nothing: the entity's MemberOf goes with it, and a spawner squad left with no member is retired by retireWipedSquads.
void Simulation::stepCorpses()
{
	const std::uint64_t now = clockTick();
	if (now % kCorpseIntervalTicks != 0 || regions_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const double hours = gameHoursIn(now);
	const bool anyPlayer = !regions_->areas().empty();
	std::set<std::int32_t> detailRegions;
	for (const Area& area : regions_->areas())
		detailRegions.insert(area.regions.begin(), area.regions.end());
	std::vector<state::GroundPoint> centres;
	if (const state::PlayerAreas* areas = ecs.get<state::PlayerAreas>())
		centres = areas->centres;
	const auto nearPlayer = [&](double x, double z)
	{
		const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
		for (const state::GroundPoint& centre : centres)
		{
			const kworld::ZoneCell at = kworld::zoneCellOf(centre.x, centre.z);
			if (std::abs(at.x - cell.x) <= kZoneLoadRadiusCells && std::abs(at.z - cell.z) <= kZoneLoadRadiusCells)
				return true;
			if (std::hypot(centre.x - x, centre.z - z) <= kPlayerLoadRadius)
				return true;
		}
		return false;
	};

	std::set<std::uint64_t> playerFactions; // state::Id of each player faction (Fighter::faction)
	ecs.each([&](const state::Id& id, const state::PlayerFaction&) { playerFactions.insert(id.value); });
	std::vector<flecs::entity> gone;
	std::vector<std::pair<flecs::entity, double>> stamps;
	ecs.each([&](flecs::entity body, const state::Fighter& fighter)
	{
		if (!fighter.dead)
			return;
		const bool player = fighter.playerControlled || playerFactions.count(fighter.faction) != 0;
		const bool carried = fighter.carriedBy != 0;
		const bool inDetail = detailRegions.count(regions_->regionOf(fighter.x, fighter.z)) != 0;
		const state::CorpseDecay* decay = body.get<state::CorpseDecay>();
		const double stamp = decay != nullptr ? decay->stamp : 0.0;
		// Setting world.offscreen_medical (COMBAT_OPTIONS.md 8): a corpse is evaluated everywhere, so it ages 12 h from its death wherever it lies,
		// and none is destroyed for lying outside every player area (the original's "zone unloaded"): it persists.
		if (player || carried || inDetail || offscreenMedical_)
		{
			// Evaluated. The exe's order: stamp, else decay unless carried, else cap the age; then the unloaded test.
			if (stamp == 0.0)
				stamps.emplace_back(body, std::max(hours, 1.0e-6));
			else if (!carried && hours - stamp > kCorpseDecayHours)
			{
				gone.push_back(body);
				return;
			} else if (carried && stamp < hours - kCorpseCarriedAgeCapHours) {
				stamps.emplace_back(body, hours - kCorpseCarriedAgeCapHours);
			}
		}
		if (!player && !offscreenMedical_ && anyPlayer && !inDetail && !nearPlayer(fighter.x, fighter.z))
			gone.push_back(body);
	});
	for (const auto& [body, stamp] : stamps)
		body.set<state::CorpseDecay>(state::CorpseDecay{stamp});
	for (flecs::entity body : gone)
		body.destruct();
}

} // namespace ksim
