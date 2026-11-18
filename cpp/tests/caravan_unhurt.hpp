#pragma once
// The original's caravan took one hit in the whole fight (rwh.89 3): the guards spawn with a first aid kit now (their CHARACTER `inventory`,
// k16.6.287), so a guard the test's fight grazed would go on 58 before 167 (package order: Medic 100, Capture Slaves 24) and the downed
// player could wake first. A test that pins the capture, not the aid, keeps the caravan unhurt until the player is cuffed.
#include "ksim/combat_data.hpp"
#include "ksim/simulation.hpp"
#include <algorithm>

#include <cstdint>

namespace ksim
{

inline void keepCaravanUnhurt(Simulation& sim, const std::uint64_t id)
{
	state::Fighter* guard = const_cast<state::Fighter*>(sim.combat()->find(id));
	if (guard == nullptr || guard->dead)
		return;
	for (state::FighterPart& part : guard->parts)
	{
		part.health = part.maxHealth;
		part.pool = 0.0;
	}
	guard->wounds.clear();
	guard->bleeding = 0.0;
}

// A fixture that pins the walk, the capture's actions or the decision trace, not the aid, has the squad spawn without its first aid kit (their
// CHARACTER `inventory`, k16.6.287): a kitted guard goes on 58 over the lift and the walk's follow, and a fight with kits adds medic picks.
inline void stripMedkits(Simulation& sim, const std::uint64_t first, const std::uint64_t last)
{
	sim.world().ecs().each([&](const state::Id& who, state::Inventory& pack)
	{
		if (who.value >= first && who.value < last)
			std::erase_if(pack.items, [&](const state::Inventory::Entry& entry)
			{
				const ItemInfo* info = sim.combat()->data().item(entry.itemSid);
				return info != nullptr && info->function == kItemFunctionMedkit;
			});
	});
}

} // namespace ksim
