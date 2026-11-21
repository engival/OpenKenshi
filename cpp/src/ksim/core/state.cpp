#include "ksim/core/state.hpp"

#include "ksim/ai/blackboard.hpp"

namespace ksim::state {

std::string_view spawnSourceName(SpawnSource source) noexcept
{
	switch (source)
	{
	case SpawnSource::TownResident:
		return "town resident";
	case SpawnSource::TownBar:
		return "town bar squad";
	case SpawnSource::TownDefaultResident:
		return "town default resident";
	case SpawnSource::TownRoaming:
		return "town roaming squad";
	case SpawnSource::BiomeHomeless:
		return "biome homeless spawn";
	case SpawnSource::BiomeNest:
		return "biome nest town";
	case SpawnSource::FactionRoaming:
		return "faction roaming squad";
	case SpawnSource::Campaign:
		return "campaign raider";
	case SpawnSource::Dismissed:
		return "dismissed character";
	}
	return "unknown";
}

std::string_view aiReasonName(AiReason reason) noexcept
{
	switch (reason)
	{
	case AiReason::None:
		return "none";
	case AiReason::Unmapped:
		return "no mapped package";
	case AiReason::Planned:
		return "behaviour planned";
	case AiReason::NoPosition:
		return "no position";
	case AiReason::NoNavigation:
		return "no navigation";
	case AiReason::OutsideWindow:
		return "outside navigation window";
	case AiReason::NoRoute:
		return "no route";
	case AiReason::OffMesh:
		return "off mesh";
	case AiReason::Stuck:
		return "stuck";
	case AiReason::NoTarget:
		return "no target";
	case AiReason::Expired:
		return "deadline passed";
	case AiReason::DoorRefused:
		return "gate refuses";
	}
	return "unknown";
}

std::string_view intentName(Intent intent) noexcept
{
	switch (intent)
	{
	case Intent::Consolidate:
		return "consolidate";
	case Intent::Defend:
		return "defend";
	case Intent::Raid:
		return "raid";
	case Intent::Mourn:
		return "mourn";
	}
	return "unknown";
}

void registerComponents(flecs::world& ecs)
{
	ecs.component<Id>();
	ecs.component<Ids>();
	ecs.component<Clock>();
	ecs.component<FactionState>();
	ecs.component<TownState>();
	ecs.component<SquadInstance>();
	ecs.component<CharacterInstance>();
	ecs.component<Position>();
	ecs.component<Mover>();
	ecs.component<GroupAI>();
	ecs.component<FactionAI>();
	ecs.component<FactionEvents>();
	ecs.component<Fighter>();
	ecs.component<Door>();
	ecs.component<GroundItem>();
	ecs.component<ItemNode>();
	ecs.component<Inventory>();
	ecs.component<InteractTask>();
	ecs.component<OrderQueue>();
	ecs.component<Allegiance>();
	ecs.component<CrimeRecord>();
	ecs.component<PlayerFaction>();
	ecs.component<PlayerSquad>();
	ecs.component<Jobs>();
	ecs.component<ProspectTask>();
	ecs.component<PlayerBuilding>();
	ecs.component<PlayerTown>();
	ecs.component<TraderStock>();
	ecs.component<TraderTrip>();
	ecs.component<MemberRole>();
	ecs.component<CorpseDecay>();
	ecs.component<Routine>();
	ecs.component<SquadRoutine>();
	ecs.component<DoorStep>();
	ecs.component<Container>();
	ecs.component<Producer>();
	ecs.component<CarryStep>();
	ecs.component<TakeOut>();
	ecs.component<GateCrew>();
	ecs.component<GateDuty>();
	ecs.component<DoorOverrides>();
	ecs.component<ViewFocus>();
	ecs.component<WeatherRegions>();
	ecs.component<GatesGone>();
	ecs.component<BuildingLooks>();
	ecs.component<Conversation>();
	ecs.component<DialogueShown>();
	ecs.component<DialogueMemory>();
	ecs.component<Scheduler>();
	ecs.component<StandInGroups>();
	ecs.component<PlayerAreas>();
	ecs.component<WildernessState>();
	ecs.component<TownStock>();
	ecs.component<EconomyProducer>();
	ecs.component<CaravanOrder>();
	ecs.component<SupplyBoard>();
	ecs.component<CropPlot>();
	ecs.component<TalkHold>();
	ecs.component<MemberDelta>();
	ecs.component<MemberSlot>();
	ecs.component<StragglerOf>();
	ecs.component<ExtraTown>();
	ecs.component<EconomySettings>();
	ecs.component<WorldEventState>();
	ecs.component<WorldOptions>();
	ecs.component<LockStates>();
	ecs.component<BoardStates>();
	ecs.component<ArrestStates>();
	ecs.component<RunStates>();
	ecs.component<MemberOf>().add(flecs::Exclusive);
	ecs.component<Relation>();
	if (ecs.get<Ids>() == nullptr)
		ecs.set<Ids>(Ids{});
	if (ecs.get<Clock>() == nullptr)
		ecs.set<Clock>(Clock{});
}

flecs::entity createInstance(flecs::world& ecs, bool furniture)
{
	Ids* ids = ecs.get_mut<Ids>();
	if (ids == nullptr)
	{
		registerComponents(ecs);
		ids = ecs.get_mut<Ids>();
	}
	flecs::entity entity = ecs.entity();
	entity.set<Id>(Id{furniture ? ids->furniture++ : ids->next++});
	return entity;
}

} // namespace ksim::state
