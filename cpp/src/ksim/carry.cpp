#include "ksim/carry.hpp"

#include "resident_data.hpp"
#include "world_resolvers.hpp"

#include "knav/walk.hpp"
#include "ksim/combat.hpp"
#include "ksim/crime.hpp"
#include "ksim/factions.hpp"
#include "ksim/door_plan.hpp"
#include "ksim/interact.hpp"
#include "ksim/navigator.hpp"
#include "ksim/perception.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/towncircle.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace ksim {

namespace
{

// A slack so that 23.4 ticks of 0.05 s do not miss 1.17 s by a rounding error.
constexpr double kStepEpsilon = 1e-9;
// The building lookup's grid cell (units).
constexpr double kBuildingCell = 128.0;
// A building's height range is widened by this when a position is tested against it (feet on a floor at the range's edge).
constexpr double kBuildingHeightSlack = 3.0;
// The put-down spot (task AW, OURS; the live body ends 2.5-6 u from its release point when the pile is full,
// kinspect bar_pile_throwout.md 2): a body goes down clear of the bodies already lying there, at the nearest navmesh point
// within kDropSpotRadius of the carrier (8 directions per ring, kDropSpotStep apart; the snap reach is the same step), on the
// carrier's floor and in its building. None free: the carrier's own point, and the body pass spreads the pile.
constexpr double kDropSpotRadius = 6.0;
constexpr double kDropSpotStep = 3.0;
constexpr double kDropAngleStep = 0.7853981633974483; // pi / 4
constexpr double kDropSpotVertical = 20.0; // the push's vertical reach (combat.cpp kAvoidVertical)

} // namespace

std::optional<Simulation::CaptureAction> Simulation::captureAction(std::uint64_t id) const
{
	const flecs::entity actor = fighterEntity(id);
	if (!actor.is_valid())
		return std::nullopt;
	const std::uint64_t now = clockTick();
	// The tick a step that has run `seconds` began (the clip's own start, as a technique's).
	const auto began = [now](double seconds)
	{
		const auto ticks = static_cast<std::uint64_t>(std::llround(seconds / kTickSeconds));
		return now > ticks ? now - ticks : 0;
	};
	if (const state::CarryStep* lift = actor.get<state::CarryStep>(); lift != nullptr && lift->phase == 0)
		return CaptureAction{"shoulder lift left", began(lift->elapsed), false};
	const state::Routine* r = actor.get<state::Routine>();
	if (r != nullptr && r->goal == routines::task::CaptureSlaves && r->chainElapsed >= 0.0 && r->chainElapsed < kRestrainSeconds
		&& r->step < r->plan.size() && r->plan[r->step] == goap::kChainTarget)
		return CaptureAction{"medic", began(r->chainElapsed), true};
	return std::nullopt;
}

void Simulation::mirrorFighter(std::uint64_t id)
{
	if (const state::Fighter* fighter = combat_->find(id))
		fighterEntity(id).set<state::Fighter>(*fighter);
}

double Simulation::packWeight(std::uint64_t id) const
{
	if (gameData_ == nullptr)
		return 0.0;
	const state::Inventory* held = fighterEntity(id).get<state::Inventory>();
	if (held == nullptr)
		return 0.0;
	double weight = 0.0;
	// A container item's contents weigh with it (INVENTORY_GRID_STATIC (d): UNSURE, the exe also scales a worn bag by `encumbrance effect`).
	const std::function<void(const std::vector<state::Inventory::Entry>&)> add = [&](const std::vector<state::Inventory::Entry>& list)
	{
		for (const state::Inventory::Entry& entry : list)
		{
			// UNSURE: a weapon's `weight kg` times its material's `weight mult` is the exe's weight; the record's own is used.
			if (const kdata::Record* record = gameData_->find(entry.itemSid))
				weight += static_cast<double>(record->floatOr("weight kg", 0.0f)) * entry.count;
			add(entry.contents);
		}
	};
	add(held->items);
	return weight;
}

bool Simulation::alliedTo(std::uint64_t a, std::uint64_t b) const
{
	const state::Fighter* first = combat_->find(a);
	const state::Fighter* second = combat_->find(b);
	if (first == nullptr || second == nullptr)
		return false;
	// The observer's view: the same faction, else my faction's relation toward the target's >= +50 (sensor::ally, task BU/BV). Read
	// through the Allegiance SIDs (the faction state entities), as the arena's bonded set does. NOT the `team` partition: team 0 holds
	// every faction neutral to the player, so a Bar guard and a Dust Bandit (-10 to the player) shared it and were allies of each
	// other - the guard could not pick a knocked-out bandit off its floor (PickupIntruders 223 never scored), and any two factions
	// hostile to the player were "allied" whichever they were.
	const state::Allegiance* factionA = fighterEntity(a).get<state::Allegiance>();
	const state::Allegiance* factionB = fighterEntity(b).get<state::Allegiance>();
	if (factionA == nullptr || factionB == nullptr)
		return false;
	if (factionA->factionSid == factionB->factionSid)
		return true;
	if (gameData_ == nullptr)
		return false;
	flecs::entity from;
	flecs::entity to;
	world_.ecs().each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid == factionA->factionSid)
			from = entity;
		if (faction.templateSid == factionB->factionSid)
			to = entity;
	});
	return from.is_valid() && to.is_valid() && factionRelation(*gameData_, from, to) >= kAlliedRelation;
}

void Simulation::captiveNoticesCarrier(std::uint64_t captiveId, std::uint64_t carrierId)
{
	state::Fighter* captive = combat_->find(captiveId);
	if (captive != nullptr && captive->captive && !captive->dead && !captive->unconscious && !alliedTo(captiveId, carrierId))
		perception::notice(*captive, carrierId);
}

void Simulation::putInBed(std::uint64_t carrierId, std::uint64_t bodyId, const routines::SiteEntry& bed)
{
	const state::Fighter* carrier = combat_->find(carrierId);
	const double carrierY = carrier != nullptr ? carrier->y : 0.0;
	const double carrierYaw = carrier != nullptr ? carrier->yaw : 0.0;
	dropCarried(carrierId);
	if (state::Fighter* put = combat_->find(bodyId))
	{
		put->x = bed.x;
		put->y = bed.hasY ? bed.y : carrierY;
		put->z = bed.z;
		put->yaw = carrierYaw;
		put->velX = 0.0;
		put->velZ = 0.0;
		put->building = bed.site.object;
		put->clearRoute();
		put->forwardSpeed = 0.0;
		put->lateralSpeed = 0.0;
		put->asleep = true;
		mirrorFighter(bodyId);
	}
	routineBook_.release(bodyId);
	routineBook_.release(carrierId);
	state::Routine lying;
	lying.target = {static_cast<std::uint8_t>(bed.site.kind), bed.site.object, bed.site.slot};
	fighterEntity(bodyId).set<state::Routine>(lying);
	routineBook_.take(bed.site, bodyId, clockTick());
}

void Simulation::dropCarried(std::uint64_t carrierId)
{
	state::Fighter* carrier = combat_->find(carrierId);
	if (carrier == nullptr)
		return;
	if (state::Fighter* carried = combat_->find(carrier->carrying))
	{
		captiveNoticesCarrier(carrier->carrying, carrierId);
		carried->carriedBy = 0;
		carried->captive = false;
		// The carrier's own point when nothing lies there, else the nearest free one (task AW): a body dropped on the pile would
		// only stack it higher.
		double dropX = carrier->x;
		double dropY = carrier->y;
		double dropZ = carrier->z;
		const auto clear = [this, dropped = carrier->carrying](double x, double y, double z)
		{
			for (const CombatArena::Member& m : combat_->members())
			{
				const state::Fighter& b = m.fighter;
				if (m.id == dropped || b.carriedBy != 0 || (!b.dead && !b.unconscious && !b.down))
					continue;
				if (std::fabs(b.y - y) >= kDropSpotVertical)
					continue;
				if (std::hypot(x - b.x, z - b.z) < CombatArena::kBodySpacing)
					return false;
			}
			return true;
		};
		// A walking carrier lets go along its motion (task body-rest, PHYSICS.md 3.2, OURS/UNSURE: the live drop lands 13-20 u
		// off): the body flies a seeded 13-20 u down the carrier's heading, as far as the navmesh and the carrier's building
		// allow, and the free-spot search runs from there. A still carrier (a put-down at a bed, a cage, a KO'd carrier) keeps
		// the point it stands on.
		double fromX = carrier->x;
		double fromZ = carrier->z;
		if (navigation_ != nullptr && carrier->walking())
		{
			const double reach = combat_->rollDropDistance(*carried, carrier->carrying);
			const knav::StepResult r = navigation_->walker().step({carrier->x, carrier->y, carrier->z},
				std::sin(carrier->yaw) * reach, std::cos(carrier->yaw) * reach, carrier->building == 0);
			if (r.onMesh && std::fabs(r.at.y - carrier->y) < kDropSpotVertical
				&& buildingAt(r.at.x, r.at.y, r.at.z) == carrier->building)
			{
				fromX = dropX = r.at.x;
				dropY = r.at.y;
				fromZ = dropZ = r.at.z;
			}
		}
		if (navigation_ != nullptr && !clear(dropX, dropY, dropZ))
		{
			bool found = false;
			for (double radius = kDropSpotStep; radius <= kDropSpotRadius + 1.0e-9 && !found; radius += kDropSpotStep)
			{
				for (int i = 0; i < 8 && !found; ++i)
				{
					const double angle = static_cast<double>(i) * kDropAngleStep;
					const auto spot = navigation_->walker().snap(
						{fromX + std::sin(angle) * radius, dropY, fromZ + std::cos(angle) * radius}, kDropSpotStep);
					if (!spot || std::fabs(spot->y - carrier->y) >= kDropSpotVertical)
						continue;
					if (buildingAt(spot->x, spot->y, spot->z) != carrier->building)
						continue;
					if (!clear(spot->x, spot->y, spot->z))
						continue;
					dropX = spot->x;
					dropY = spot->y;
					dropZ = spot->z;
					found = true;
				}
			}
		}
		carried->x = dropX;
		carried->y = dropY;
		carried->z = dropZ;
		carried->building = carrier->building;
		mirrorFighter(carrier->carrying);
		// EVENT 48 cause 2 (task BG): the released body starts its ragdoll where it lands. A body that woke up and walked
		// off is not lying down, so it sends none.
		if (carried->dead || carried->unconscious || carried->down)
			emitRagdoll(carrier->carrying, 2);
	}
	carrier->carrying = 0;
	carrier->encumbrance = 1.0;
	fighterEntity(carrierId).remove<state::CarryStep>();
	mirrorFighter(carrierId);
}

bool Simulation::cageDoorLocked(std::uint64_t cageBuilding) const
{
	// A lock picked, cut or forced open stays open until the next prisoner goes in (the put-in locks it again: item28 b1) - lock_tasks.cpp.
	if (const LockRecord* lock = lockFind(false, cageBuilding); lock != nullptr && lock->open)
		return false;
	// A cage with a prisoner is locked unless opened (OURS: who locks a cage in the exe is unread). Opened is: every door of the cage's
	// building destroyed (state 3, open for ever); or the warden's squad promoted and its lock task never having run (cagesLocked 0 -
	// town residents who never lock lose prisoners). A warden squad that is not promoted keeps no SquadRoutine, so a camp, a nest or a
	// jail away from the player stays locked.
	std::size_t doors = 0;
	std::size_t broken = 0;
	world_.ecs().each([&](const state::Door& door)
	{
		if (door.building != cageBuilding)
			return;
		++doors;
		broken += door.state == 3 ? 1 : 0;
	});
	if (doors != 0 && broken == doors)
		return false;
	if (scenarioLockedCages_.count(cageBuilding) != 0)
		return true;
	bool promotedWarden = false;
	bool locked = false;
	world_.ecs().each([&](const state::SquadInstance& instance, const state::SquadRoutine& routine)
	{
		if (instance.homeBuilding != cageBuilding)
			return;
		promotedWarden = true;
		locked = locked || routine.cagesLocked != 0;
	});
	return promotedWarden ? locked : true;
}

std::uint64_t Simulation::buildingAt(double x, double y, double z)
{
	const std::vector<BuildingSite>& sites = world_.buildingSites();
	if (!enterableValid_)
	{
		// The buildings with a door that has a way in (the footprint rule for a building with no interior mask), and the
		// spatial grid of the sites (the sites never change after the world is built).
		enterable_.clear();
		world_.ecs().each([&](const state::Door& door)
		{
			if (door.building != 0 && (door.inX != 0.0 || door.inZ != 0.0))
				enterable_.push_back(door.building);
		});
		std::sort(enterable_.begin(), enterable_.end());
		enterable_.erase(std::unique(enterable_.begin(), enterable_.end()), enterable_.end());
		enterableValid_ = true;
	}
	if (!buildingGridValid_)
	{
		buildingGrid_.clear();
		for (std::size_t i = 0; i < sites.size(); ++i)
		{
			const BuildingSite& site = sites[i];
			const double reach = std::hypot(site.halfA, site.halfB);
			const std::int64_t x0 = static_cast<std::int64_t>(std::floor((site.cx - reach) / kBuildingCell));
			const std::int64_t x1 = static_cast<std::int64_t>(std::floor((site.cx + reach) / kBuildingCell));
			const std::int64_t z0 = static_cast<std::int64_t>(std::floor((site.cz - reach) / kBuildingCell));
			const std::int64_t z1 = static_cast<std::int64_t>(std::floor((site.cz + reach) / kBuildingCell));
			for (std::int64_t cx = x0; cx <= x1; ++cx)
			{
				for (std::int64_t cz = z0; cz <= z1; ++cz)
					buildingGrid_[cx * 1000003 + cz].push_back(static_cast<std::uint32_t>(i));
			}
		}
		buildingGridValid_ = true;
	}
	const auto cell = buildingGrid_.find(static_cast<std::int64_t>(std::floor(x / kBuildingCell)) * 1000003
		+ static_cast<std::int64_t>(std::floor(z / kBuildingCell)));
	if (cell == buildingGrid_.end())
		return 0;
	std::uint64_t found = 0;
	double smallest = 1.0e30;
	for (const std::uint32_t index : cell->second)
	{
		const BuildingSite& site = sites[index];
		// The interior mask hull when the building has one (the exe's indoors trigger); else the whole footprint, for a building
		// that has a way in only.
		const double cx = site.hull ? site.hullCx : site.cx;
		const double cz = site.hull ? site.hullCz : site.cz;
		const double halfA = site.hull ? site.hullHalfA : site.halfA;
		const double halfB = site.hull ? site.hullHalfB : site.halfB;
		const double minY = site.hull ? site.hullMinY : site.minY;
		const double maxY = site.hull ? site.hullMaxY : site.maxY;
		const double area = halfA * halfB;
		if (area >= smallest)
			continue;
		const double dx = x - cx;
		const double dz = z - cz;
		if (std::fabs(dx * site.axisX + dz * site.axisZ) > halfA || std::fabs(-dx * site.axisZ + dz * site.axisX) > halfB)
			continue;
		if (y < minY - kBuildingHeightSlack || y > maxY + kBuildingHeightSlack)
			continue;
		if (!site.hull && !std::binary_search(enterable_.begin(), enterable_.end(), site.key))
			continue;
		smallest = area;
		found = site.key;
	}
	return found;
}

int Simulation::buildingFloor(std::uint64_t building, double y)
{
	const BuildingSite* site = building != 0 ? world_.buildingSite(building) : nullptr;
	return site != nullptr ? site->floorAt(y) : 0;
}

void Simulation::setLocation(state::Fighter& fighter, std::uint64_t building, double y)
{
	const BuildingSite* site = building != 0 ? world_.buildingSite(building) : nullptr;
	fighter.building = building;
	fighter.floor = static_cast<std::int16_t>(site != nullptr ? site->floorAt(y) : 0);
	fighter.indoors = site != nullptr && site->indoorsAt(y, fighter.floor);
	fighter.cavernous = site != nullptr && site->cavernous;
	fighter.sheltered = shelteredAt(fighter.x, fighter.y, fighter.z);
}

bool Simulation::shelteredAt(double x, double y, double z) const
{
	const BuildingSite* top = nullptr;
	for (const BuildingSite& site : world_.buildingSites())
	{
		if (site.maxY < y + kRoofClearance || !insideFootprint(site, {x, z}))
			continue;
		if (top == nullptr || site.maxY > top->maxY)
			top = &site;
	}
	return top != nullptr && top->shelter;
}

Body Simulation::body(const state::Fighter& f) const
{
	return combat_ != nullptr ? combat_->data().body(f.raceSid) : Body{};
}

double Simulation::pathRadius(const state::Fighter& f) const
{
	return combat_ != nullptr ? combat_->data().pathRadius(f.raceSid) : 0.0;
}

void Simulation::placeInBuildings()
{
	if (combat_ == nullptr)
		return;
	for (const CombatArena::Member& member : combat_->members())
	{
		const state::Fighter& f = member.fighter;
		if (f.building != 0 || f.carriedBy != 0 || f.dead)
			continue;
		if (const std::uint64_t building = buildingAt(f.x, f.y, f.z))
		{
			state::Fighter* placed = combat_->find(member.id);
			setLocation(*placed, building, f.y);
			if (placed->homeBuilding == 0 && !placed->playerControlled)
			{
				placed->homeBuilding = building; // as in spawnFighter
				touchHomes();
			}
			mirrorFighter(member.id);
		}
	}
}

void Simulation::removeFighter(std::uint64_t id)
{
	pathRetries_.reset(id);
	if (combat_ == nullptr)
		return;
	state::Fighter* f = combat_->find(id);
	if (f == nullptr)
		return;
	// Its person lies where it last stood (dropCarried), the one carrying it is freed.
	if (f->carrying != 0)
		dropCarried(id);
	if (f->carriedBy != 0)
	{
		if (state::Fighter* carrier = combat_->find(f->carriedBy))
		{
			const std::uint64_t carrierId = f->carriedBy;
			carrier->carrying = 0;
			carrier->encumbrance = 1.0;
			fighterEntity(carrierId).remove<state::CarryStep>();
			mirrorFighter(carrierId);
		}
		f->carriedBy = 0;
		f->captive = false;
	}
	// Lifts and take-outs aimed at it: stepCarry / stepIntruders end them when the person is gone (finishing the order with
	// event 13 "gone"), so they need no change here.
	const auto& members = combat_->members();
	const auto at = std::find_if(members.begin(), members.end(), [&](const CombatArena::Member& m) { return m.id == id; });
	const std::size_t index = static_cast<std::size_t>(at - members.begin());
	flecs::entity entity(world_.ecs().c_ptr(), combatEntities_[index]);
	combatEntities_.erase(combatEntities_.begin() + static_cast<std::ptrdiff_t>(index));
	combat_->remove(id);
	entity.remove<state::Fighter>();
	if (const state::BuildingLooks* saved = world_.ecs().get<state::BuildingLooks>())
	{
		state::BuildingLooks looks = *saved;
		const auto look = std::lower_bound(looks.looks.begin(), looks.looks.end(), id,
			[](const state::BuildingLook& entry, std::uint64_t key) { return entry.id < key; });
		if (look != looks.looks.end() && look->id == id)
		{
			looks.looks.erase(look);
			ecsMutable().set<state::BuildingLooks>(std::move(looks));
		}
	}
}

void Simulation::stepBuildings()
{
	if (combat_ == nullptr || combat_->empty())
		return;
	flecs::world& ecs = ecsMutable();
	const state::BuildingLooks* saved = ecs.get<state::BuildingLooks>();
	std::vector<state::BuildingLook> looks = saved != nullptr ? saved->looks : std::vector<state::BuildingLook>{};
	bool changed = false;
	for (const CombatArena::Member& member : combat_->members())
	{
		const state::Fighter& f = member.fighter;
		if (f.dead || f.carriedBy != 0)
			continue;
		// Only a fighter that has moved since its last look is tested: a standing one keeps what it has (a value set by hand, a
		// scenario's, a load's), and a fighter seen for the first time is only noted (it was placed at spawn, spawnFighter).
		const auto look = std::lower_bound(looks.begin(), looks.end(), member.id,
			[](const state::BuildingLook& entry, std::uint64_t id) { return entry.id < id; });
		if (look == looks.end() || look->id != member.id)
		{
			looks.insert(look, state::BuildingLook{member.id, f.x, f.y, f.z});
			changed = true;
			// Seen for the first time (a spawn, a load): its floor and indoors state are rebuilt from where it stands (a save
			// from before v85 has no indoors, and `cavernous` is never saved).
			setLocation(*combat_->find(member.id), f.building, f.y);
			mirrorFighter(member.id);
			continue;
		}
		if (look->x == f.x && look->y == f.y && look->z == f.z)
			continue;
		*look = state::BuildingLook{member.id, f.x, f.y, f.z};
		changed = true;
		const std::uint64_t building = buildingAt(f.x, f.y, f.z);
		const int floor = buildingFloor(building, f.y);
		const BuildingSite* site = building != 0 ? world_.buildingSite(building) : nullptr;
		const bool indoors = site != nullptr && site->indoorsAt(f.y, floor);
		if (building != f.building || floor != f.floor || indoors != f.indoors)
		{
			setLocation(*combat_->find(member.id), building, f.y);
			mirrorFighter(member.id);
		}
	}
	if (changed)
		ecs.set<state::BuildingLooks>({std::move(looks)});
}

void Simulation::stepCarry()
{
	if (combat_ == nullptr || combat_->empty())
		return;
	flecs::world& ecs = ecsMutable();
	const CombatConstants& k = combat_->data().k;
	const auto& members = combat_->members();
	// A carried person whose carrier is gone (or no longer holds it) would stay unusable for ever: it lies where it was last
	// carried to. removeFighter frees it at once; this catches any other way the pair came apart.
	for (const CombatArena::Member& member : members)
	{
		if (member.fighter.carriedBy == 0)
			continue;
		const state::Fighter* carrier = combat_->find(member.fighter.carriedBy);
		if (carrier == nullptr || carrier->carrying != member.id)
		{
			combat_->find(member.id)->carriedBy = 0;
			combat_->find(member.id)->captive = false;
			mirrorFighter(member.id);
		}
	}
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::CarryStep* found = actor.get<state::CarryStep>();
		if (found == nullptr)
			continue;
		const std::uint64_t id = members[i].id;
		state::CarryStep step = *found;
		state::Fighter& f = *combat_->find(id);
		state::Fighter* carried = combat_->find(step.target);
		// A copy: dropCarried and remove<CarryStep> move the entity to another table, which dangles a component pointer.
		const std::optional<state::InteractTask> task = actor.has<state::InteractTask>()
			? std::optional<state::InteractTask>(*actor.get<state::InteractTask>()) : std::nullopt;
		// Ends the interact order that started this step (lift: action 8; put down: action 13) with event 13.
		const auto finishTask = [&](std::uint8_t action, std::uint8_t result)
		{
			if (task && task->action == action)
			{
				reportInteract(id, task->target, action, result);
				actor.remove<state::InteractTask>();
			}
		};
		// A carrier that is KO'd, dead or down drops its person at once; one whose person vanished has nothing to carry.
		if (carried == nullptr || f.dead || f.unconscious || f.down)
		{
			dropCarried(id);
			if (carried == nullptr)
				finishTask(step.phase == 2 ? static_cast<std::uint8_t>(InteractAction::PutDown) : static_cast<std::uint8_t>(InteractAction::Carry),
					InteractEvent::kGone);
			continue;
		}
		// The 240 arrest lift takes any body isTargetDown (rwh.124 4.1), conscious or not, and a carried body is down whatever it does (4.1: carried).
		const auto arrestOrder = arrestOrders_.find(id);
		const bool arrestLift = arrestOrder != arrestOrders_.end() && arrestOrder->second.lift == goap::kLiftPersonSnatchingInTown && arrestOrder->second.victim == step.target;
		if (step.phase == 0)
		{
			// The body must still be there to be lifted.
			// A shackled slave may be lifted conscious while it is down (a crawler a squad shot down, kenshi-k16.6.28).
			const bool shackled = carried->slaveState == 1 && !carried->captorSid.empty();
			if (!step.attached && (carried->carriedBy != 0 || !(carried->unconscious || carried->dead || (shackled && carried->down) || (arrestLift && isTargetDown(*carried)))))
			{
				actor.remove<state::CarryStep>();
				finishTask(static_cast<std::uint8_t>(InteractAction::Carry), InteractEvent::kRefused);
				continue;
			}
			step.elapsed += kTickSeconds;
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			if (!step.attached && step.elapsed >= kLiftAttachSeconds - kStepEpsilon)
			{
				step.attached = true;
				carried->carriedBy = id;
				carried->building = 0;
				carried->clearRoute();
				carried->forwardSpeed = 0.0;
				carried->lateralSpeed = 0.0;
				carried->target = 0;
				// CARRY_KIDNAP_LIVE.md: the exe's Task_Kidnapping alone sets the captive flag the carry inspection reads, so a plain carry
				// is never a kidnapping there. Ours sets it on the lift of any non-slave (the user's fix, ARCHITECTURE "Guards notice what
				// you carry"); a slave is judged by its slave state instead.
				carried->captive = carried->slaveState != 1 && carried->slaveState != 2;
				f.carrying = step.target;
			}
			if (step.elapsed >= kLiftSeconds - kStepEpsilon)
			{
				// 14034fbe0 L46 / L52 (CRIME_NOTICE C54 / C55 / C56): lifting a person out of a cage tries SLAVE_FREEING on the lifter (victim: the
				// cage's owner) and ESCAPE_PRISON on the carried. The second is refused as the carried is a cage occupant (C29) and the first stays a
				// 3 s record (setCrime's default timer). The escaped flag the exe sets here is not modelled (UNSURE).
				if (carried->cageBuilding != 0)
				{
					const routines::BuildingSites* cageOf = residentData_ != nullptr ? residentData_->sites.find(carried->cageBuilding) : nullptr;
					const std::string cageOwner = cageOf != nullptr && !cageOf->ownerFaction.empty() ? cageOf->ownerFaction : carried->captorSid;
					reportCrime(id, crime::kSlaveFreeing, cageOwner, step.target);
					reportCrime(step.target, crime::kEscapingPrison, cageOwner, 0);
				}
				step.phase = 1;
				step.elapsed = 0.0;
				finishTask(static_cast<std::uint8_t>(InteractAction::Carry), InteractEvent::kDone);
			}
		} else if (step.phase == 1) {
			// A carried character that wakes up is put down. A chained slave (task BO, kinspect dt3: awake captives walk, goal 171
			// FOLLOW_SLAVEMASTER) then follows its carrier; any other is left where it stands (UNSURE: the original's behaviour for
			// those is not measured).
			const bool slave = carried->slaveState == 1 && !carried->captorSid.empty();
			if (!carried->unconscious && !carried->dead && !(slave && carried->down) && !(arrestLift && !slave))
			{
				dropCarried(id);
				if (slave && navigation_ != nullptr)
				{
					apply(Command::fighterFollow(state::Id{step.target}, state::Id{id}));
					// Task BR2: the captive's follow (171) in the server's log.
					std::ostringstream line;
					line << "capture 171 follow slave " << step.target << " follows its carrier " << id;
					logTask(line.str());
				}
				continue;
			}
		} else {
			step.elapsed += kTickSeconds;
			f.clearRoute();
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			if (step.elapsed >= kPutDownSeconds - kStepEpsilon)
			{
				// Released: it lies where the carrier stands. A non-ally starts a forced KO timer, never under 3 s.
				const bool ally = alliedTo(id, step.target);
				carried->carriedBy = 0;
				// 1405ce1e0 (CRIME_STATIC 15, M): setting a captive (status +0xE9) down writes crime 15 on the carrier, 10 s. The victim is the
				// captive's faction (UNSURE: the exe's victim argument is not read), and setCrime refuses the carrier's own faction.
				if (carried->captive)
				{
					captiveNoticesCarrier(step.target, id); // before the forced KO below, which makes it unconscious
					carried->captive = false;
					const state::Allegiance* side = fighterEntity(step.target).get<state::Allegiance>();
					if (side != nullptr)
						reportCrime(id, crime::kKidnapping, side->factionSid, step.target, crime::kTimerPutDownKidnap);
				}
				carried->x = f.x;
				carried->y = f.y;
				carried->z = f.z;
				carried->yaw = f.yaw;
				carried->building = f.building;
				if (!ally && !carried->dead)
				{
					const double missing = std::max(combat_->rules().maxBlood(*carried) - carried->blood, 0.0);
					const double t = forcedKoSeconds(k.knockoutTimeBase, k.knockoutMult1, k.knockoutMult99, missing,
						carried->skills.toughness, f.skills.strength * CombatRules::effectiveStat(f, CombatRules::kStatStrength));
					carried->koTimer = std::max(carried->koTimer, t);
					carried->unconscious = true;
				}
				f.carrying = 0;
				f.encumbrance = 1.0;
				mirrorFighter(step.target);
				// EVENT 48 cause 2 (task BG): the put-down releases the body into its ragdoll where the carrier stood. An ally
				// put down while awake is not lying down, so it sends none.
				if (carried->dead || carried->unconscious || carried->down)
					emitRagdoll(step.target, 2);
				finishTask(static_cast<std::uint8_t>(InteractAction::PutDown), InteractEvent::kDone);
				actor.remove<state::CarryStep>();
				mirrorFighter(id);
				continue;
			}
		}
		// The carried rides along; the carrier's encumbrance is its pack plus a fixed person weight.
		if (f.carrying != 0)
		{
			carried->x = f.x;
			carried->y = f.y;
			carried->z = f.z;
			carried->yaw = f.yaw;
			carried->velX = f.velX;
			carried->velZ = f.velZ;
			const double strength = f.skills.strength * CombatRules::effectiveStat(f, CombatRules::kStatStrength);
			f.encumbrance = encumbranceRatio(carryCapacity(strength, k.carryWeightMult, k.encumbranceBase),
				packWeight(id) + k.carryPersonWeight);
			mirrorFighter(step.target);
		}
		actor.set<state::CarryStep>(std::move(step));
		mirrorFighter(id);
	}
}

// A world squad's member (the occupiers) adopts 273 through the world selector, which never re-scores it: the take-out's end hands
// the member back by dropping the Routine. A resident's is the town scorer's (re-scored every second) and is left alone.
void Simulation::releaseWorldIntruderGoal(flecs::entity actor)
{
	const state::Routine* routine = actor.get<state::Routine>();
	if (routine == nullptr || routine->goal != routines::task::PickupIntrudersTown)
		return;
	const flecs::entity squad = actor.target<state::MemberOf>();
	if (squad.is_valid() && !squad.has<state::SquadRoutine>())
		actor.remove<state::Routine>();
}

void Simulation::stepIntruders()
{
	if (combat_ == nullptr || combat_->empty() || navigation_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	// The scorer's kReach: the member stands at the body (the same 4 u the slave chain's planner uses). The lift starts at the interact
	// order's own reach: a body inside a building whose inside the walk cannot enter (the Hub Bar's) stops 7 u short, and the lift still
	// happens from there - the exe's isAtLocation is the plan's, the interact's reach is the lift's.
	constexpr double kBodyReach = 4.0;
	constexpr double kLiftReach = kInteractReach;

	// The running take-outs: the goal's G4 chain drives them (task CH; dt24_223). 67 Running is the walk, 68 LIFT_PERSON the lift
	// (beginInteract / stepCarry), the 223 goal node pops, and 224 - planned once the body is in the arms - is the moveTo outside,
	// whose arrival hook drops it (dt24_223 section 3).
	const auto installRoute = [&](std::uint64_t id, state::Fighter& f, double tx, double ty, double tz, std::uint8_t mode = 0) -> bool
	{
		f.clearRoute();
		if (navigation_ == nullptr)
			return false;
		const Route route = routeRetry(id, f, tx, tz, ty, true);
		if (!route.found || route.points.empty())
			return false;
		for (std::size_t p = 0; p < route.points.size(); ++p)
			f.route.push_back({route.points[p].x, p < route.heights.size() ? route.heights[p] : f.y, route.points[p].z});
		f.routeNext = 0;
		++f.routeVersion;
		f.routeSeq = 0;
		f.routeChase = false;
		f.routeApproach = route.approach;
		f.routeTries = 0;
		f.moveMode = mode; // NPC carriers walk (mode 0, bumped to mode 1 by the gait cap); 274 forces RUN (mode 2)
		mirrorFighter(id);
		return true;
	};
	// 274's destination (dt24_273 section 3 / CRIME_STATIC "Town::vf38"): the first gate of my town, then kTownDropOut further out along
	// the town centre's ray through it (Town::vf82). The town is the walled region I stand in; when the walls are already behind me, the
	// region whose centre is the circle I stand in. No town, no gates or no mesh: no point (the take-out drops on the spot).
	const auto townDropPoint = [&](const state::Fighter& f) -> std::optional<knav::Position>
	{
		if (navigation_ == nullptr || combat_ == nullptr)
			return std::nullopt;
		const perception::Environment& env = combat_->environment();
		int region = env.walledRegion(f.x, f.z);
		if (region < 0)
		{
			const int town = env.townOf(f.x, f.z);
			if (town < 0)
				return std::nullopt;
			const towncircle::Circle& circle = env.towns[static_cast<std::size_t>(town)];
			for (std::size_t i = 0; i < env.walled.size() && region < 0; ++i)
				if (env.walled[i].x == circle.x && env.walled[i].z == circle.z)
					region = static_cast<int>(i);
			if (region < 0)
				return std::nullopt;
		}
		const perception::Environment::WalledRegion& walls = env.walled[static_cast<std::size_t>(region)];
		if (walls.gates.empty())
			return std::nullopt;
		const perception::Environment::WalledRegion::Gate& gate = walls.gates.front();
		const double dx = gate.x - walls.x;
		const double dz = gate.z - walls.z;
		const double length = std::hypot(dx, dz);
		if (length <= 1.0e-9)
			return std::nullopt;
		return navigation_->walker().snap({gate.x + dx / length * kTownDropOut, f.y, gate.z + dz / length * kTownDropOut}, 40.0);
	};
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::TakeOut* found = actor.get<state::TakeOut>();
		if (found == nullptr)
			continue;
		const std::uint64_t id = members[i].id;
		state::TakeOut take = *found;
		state::Fighter& f = *combat_->find(id);
		// Read only before dropCarried: that removes the CarryStep and moves the entity, which dangles this pointer.
		const state::CarryStep* carry = actor.get<state::CarryStep>();
		const state::InteractTask* interact = actor.get<state::InteractTask>();
		const auto end = [&]
		{
			actor.remove<state::TakeOut>();
			releaseWorldIntruderGoal(actor);
			f.moveMode = 2; // the carry ends: back to the default speed mode
			mirrorFighter(id);
		};
		take.elapsed += kTickSeconds;
		if (f.dead || f.unconscious || f.down)
		{
			end(); // the carrier is out (stepCarry already dropped the person)
			continue;
		}
		if (take.dropSet && f.carrying != take.target)
		{
			end(); // lost it (woke, was taken)
			continue;
		}
		if (take.plan.empty())
		{
			// A v82 save (whose take-out had a phase, not a plan) or a hand-built one: rebuild the plan from the live hands - in the
			// arms the way out is computed below (the drained branch); otherwise the lift (68 LIFT_PERSON) is next.
			if (f.carrying == take.target)
				take.dropSet = false; // the drained branch plans 224 outside the building
			else
			{
				take.plan = {goap::kLiftPerson, routines::task::PickupIntruders};
				take.step = 0;
			}
		}
		// The 223/273 plan is drained and the body is in the arms: 224's tick computes the outside point for my home building, 274's the
		// point beyond my town's gates; then its plan is the walk out (67) and the moveTo. No door, no point on the mesh or no way: put
		// the person down where we stand (the exe's stuck handler).
		if (take.step >= take.plan.size())
		{
			if (!take.dropSet)
			{
				if (f.carrying != take.target)
				{
					end(); // the lift never happened
					continue;
				}
				// The variant is the drained plan's goal node: 273 PICKUP_INTRUDERS_TOWN runs the town take-out 274, 223 the home's 224.
				const bool townScoped = !take.plan.empty() && take.plan.back() == routines::task::PickupIntrudersTown;
				bool routed = false;
				if (townScoped)
				{
					// 274's destination (dt24_273 section 3, Town::vf38 -> vf82): the first gate of my town, then kTownDropOut further
					// out along the town centre's ray through it. 274 forces RUN (mode 2, cap 999) for every carrier, not only NPCs.
					const std::optional<knav::Position> spot = townDropPoint(f);
					if (spot)
					{
						take.dropX = spot->x;
						take.dropY = spot->y;
						take.dropZ = spot->z;
						routed = installRoute(id, f, take.dropX, take.dropY, take.dropZ, 2);
					}
				} else {
					const state::Door* door = nullptr;
					double nearest = 1.0e18;
					ecs.each([&](const state::Door& candidate)
					{
						const double d = std::hypot(candidate.x - f.x, candidate.z - f.z);
						if (candidate.building == take.building && d < nearest)
						{
							nearest = d;
							door = &candidate;
						}
					});
					if (door != nullptr)
					{
						// outside + 1.1 (outside - inside), the points kDoorSideDistance either side of the door's centre.
						const double outX = -door->inX;
						const double outZ = -door->inZ;
						// The standard drop point lies kDropBeyondDoor out. Where the building's footprint (the interior-mask hull) reaches
						// further than that (the Hub Bar's does, ~50 units past its door), the person would still be "in the building",
						// so go further out in steps until the snapped point is outside it (live: 60-90 units from the door, BQ7 Q2).
						std::optional<knav::Position> spot;
						for (double along = kDropBeyondDoor; along <= kDropBeyondDoor + kDropSearchExtra + 1.0e-9; along += kDropSearchStep)
						{
							spot = navigation_->walker().snap({door->x + outX * along, door->y, door->z + outZ * along}, 40.0);
							if (!spot || buildingAt(spot->x, spot->y, spot->z) != take.building)
								break;
						}
						if (spot)
						{
							take.dropX = spot->x;
							take.dropY = spot->y;
							take.dropZ = spot->z;
							routed = installRoute(id, f, take.dropX, take.dropY, take.dropZ);
						}
					}
				}
				if (!routed)
				{
					dropCarried(id);
					end();
					continue;
				}
				// G4: 224 / 274 plan the way out from where the lift left us (67 Running in front of the take-out step). A plan that
				// fails here would leave the body in the arms for ever, so the stuck handler above is the fallback.
				const goap::Plan out = townScoped ? planIntruder274(id, take.target, take.dropX, take.dropZ)
												  : planIntruder224(id, take.target, take.dropX, take.dropZ);
				if (!out.found())
				{
					dropCarried(id);
					end();
					continue;
				}
				take.plan.clear();
				for (const goap::Step& step : out.steps)
					take.plan.push_back(step.task);
				take.step = 0;
				take.dropSet = true;
				take.outside = -1.0;
				take.elapsed = 0.0;
			}
		}
		// Run the plan: a step that completes at once hands over to the next one in the same tick (the exe's startNextPlanStep runs
		// from checkCurrentTask, so two instant steps take one update; ours takes the tick).
		bool stuck = false;
		while (take.step < take.plan.size() && !stuck)
		{
			const std::uint16_t task = take.plan[take.step];
			if (const goap::TaskRow* row = goap::rowOf(task); row != nullptr && row->hasGoalState)
			{
				++take.step; // a resolver-only entry (model M): the 223 goal node, popped, no task runs
				continue;
			}
			state::Fighter* body = combat_->find(take.target);
			if (body == nullptr)
			{
				stuck = true; // gone
				break;
			}
			if (task == goap::kRunning)
			{
				// 67 Running: the walk to the step's position - the body before the lift, the outside point after it.
				const double tx = take.dropSet ? take.dropX : body->x;
				const double tz = take.dropSet ? take.dropZ : body->z;
				if (!f.walking() && std::hypot(f.x - tx, f.z - tz) <= (take.dropSet ? kBodyReach : kLiftReach))
				{
					++take.step;
					continue;
				}
				// 274's way out runs (mode 2); the walk to the body and 224's way out walk (mode 0).
				const std::uint8_t mode = !take.plan.empty() && take.plan.back() == routines::task::TakeIntruderOutsideTown ? 2 : 0;
				if (!f.walking() && !installRoute(id, f, tx, take.dropSet ? take.dropY : body->y, tz, mode))
					stuck = true; // no way there: the caps end it (never a terminal state)
				break;
			}
			if (goap::isLiftTask(task))
			{
				// 68 LIFT_PERSON (and 170 / 213 / 225 / 240 / 246, the same Task_LiftSomething): at the body, the carry order lifts it (stepCarry); the step ends when the lift does.
				if (carry != nullptr && carry->target == take.target && carry->phase == 1)
				{
					++take.step;
					continue;
				}
				if (carry != nullptr || interact != nullptr)
				{
					take.liftOrdered = true; // under way (or the order is)
					break;
				}
				if (take.liftOrdered)
				{
					stuck = true; // the order ended without a lift: refused, unreachable, the body woke (the old executor's rule)
					break;
				}
				if (f.walking() || std::hypot(f.x - body->x, f.z - body->z) > kLiftReach)
					break; // still on the way (the 67 step's route)
				beginInteract(id, take.target, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
				take.liftOrdered = true;
				break;
			}
			if (task == routines::task::TakeIntruderOutside || task == routines::task::TakeIntruderOutsideTown)
				break; // 224's / 274's moveTo is issued (the walk above it); the arrival test below drops the body
			++take.step; // a step this executor does not run
		}
		// The caps: a walk that cannot finish, a fight, a lost body. No terminal state - the resident is freed and may re-score.
		const bool townTakeOut = take.step < take.plan.size() && take.plan[take.step] == routines::task::TakeIntruderOutsideTown;
		const bool atTakeOut = townTakeOut || (take.step < take.plan.size() && take.plan[take.step] == routines::task::TakeIntruderOutside);
		if (atTakeOut)
		{
			// "Out": 224 reads the carrier out of the building, 274 out of the town's gates (the effect's meaning, 164).
			const bool outside = townTakeOut ? !combat_->environment().inWalls(f.x, f.z) : f.building == 0;
			if (outside)
				take.outside = take.outside < 0.0 ? 0.0 : take.outside + kTickSeconds;
			else
				take.outside = -1.0;
			const bool idle = !f.walking() && !actor.has<state::DoorStep>();
			// Drop once outside and idle, or after 5 game-minutes outside; a walk that ended inside, a fight, or the no-terminal-
			// state cap drop it on the spot (no animation and no forced KO timer: the exe's drop is not the put-down task).
			if ((outside && idle) || take.outside >= kTakeOutOutsideCap || (idle && !outside) || f.target != 0
				|| take.elapsed >= kTakeOutCap)
			{
				dropCarried(id);
				end();
				continue;
			}
		} else if (stuck || take.elapsed >= kTakeOutCap) {
			// The lift or the walk to the body ended (refused, unreachable, the victim woke): nothing more this resident can do now.
			if (f.carrying == take.target)
				dropCarried(id);
			end();
			continue;
		}
		actor.set<state::TakeOut>(std::move(take));
	}

	// New take-outs: once a second, a resident finds a KO'd intruder in its home with the exe's own resolver (14099c110, dt24_223
	// section 2) and plans the 223 chain (G4). The old hand-written nearest scan is retired (retired/intruder_scan.txt).
	if (now % kSquadIntervalTicks != 0)
		return;
	const sensor::Context ctx = sensorContext();
	const auto lookup = [&](std::uint64_t target) -> const state::Fighter* { return combat_->find(target); };
	std::set<std::uint64_t> taken; // intruders another resident already goes for
	ecs.each([&](const state::TakeOut& take) { taken.insert(take.target); });
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		state::Fighter& resident = *combat_->find(members[i].id);
		if (resident.playerControlled || resident.dead || resident.unconscious || resident.down || resident.asleep
			|| resident.carrying != 0 || resident.carriedBy != 0 || resident.target != 0)
			continue; // a sleeper (in a bed) starts no take-out
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		if (actor.has<state::TakeOut>() || actor.has<state::CarryStep>() || actor.has<state::InteractTask>()
			|| actor.has<state::DoorStep>() || resident.taskRunning())
			continue;
		// Since T3e1 the town scorer decides when: a promoted member (one with a Routine) goes for an intruder only on its
		// PICKUP_INTRUDERS goal 223 - or its town scope 273 (task CH group 3) - which its package's list carries and the scorer adopts
		// when its home / its town holds a KO'd non-ally. A fighter with no Routine (a scenario's resident, a hand-built guard) still
		// takes one out of its home (ksim_carry).
		const state::Routine* routine = actor.get<state::Routine>();
		const bool townGoal = routine != nullptr && routine->goal == routines::task::PickupIntrudersTown;
		if (routine != nullptr && !townGoal && routine->goal != routines::task::PickupIntruders)
			continue;
		if (resident.homeBuilding == 0 && !townGoal)
			continue; // 223 needs my building; 273 needs my town instead (checked with the resolver's own test)
		std::uint64_t victim = 0;
		if (townGoal)
		{
			const std::optional<std::uint64_t> found = townIntruder273(resident, ctx);
			if (!found)
				continue; // 273's town and ownership tests (14099c250)
			victim = *found;
		} else {
			victim = worldres::pickupIntruders223(resident, ctx, lookup);
		}
		if (victim == 0 || taken.count(victim) != 0)
		{
			if (victim == 0)
				releaseWorldIntruderGoal(actor); // the body is gone before the lift began
			continue;
		}
		const goap::Plan chain = townGoal ? planIntruder273(members[i].id, victim) : planIntruder223(members[i].id, victim);
		if (!chain.found())
			continue; // no plan = the goal scores 0 this second (dt18 section 6); never a terminal state
		// 67 Running is the way to the body: installed here, the plan's first step. No route = not taken out at all.
		std::size_t step = 0;
		if (step < chain.steps.size() && chain.steps[step].task == goap::kRunning)
		{
			const state::Fighter* body = combat_->find(victim);
			if (body == nullptr || !installRoute(members[i].id, resident, body->x, body->y, body->z))
				continue; // no way to the body: left for a later second
			++step;
		}
		state::TakeOut take;
		take.target = victim;
		// My home building (the exe's ownerships+0x38): 224's outside point is its door's. The town take-out (273 -> 274) has no
		// building: its point is the town's gate (the drained branch reads the plan's goal node).
		take.building = townGoal ? 0 : resident.homeBuilding;
		for (const goap::Step& planStep : chain.steps)
			take.plan.push_back(planStep.task);
		take.step = static_cast<std::uint16_t>(step);
		actor.set<state::TakeOut>(std::move(take));
		taken.insert(victim);
		mirrorFighter(members[i].id);
	}
}

bool Simulation::reportEnslaving(std::uint64_t cuffer, std::uint64_t victim)
{
	if (combat_ == nullptr)
		return false;
	const state::Fighter* body = combat_->find(victim);
	// Free (slave state 0) and the player's, with no bounty on it. UNSURE: dt5's "free" may still admit an escaping or ex-slave
	// (state 2 / 3); the read pins only that a slave (1) is not free, so the narrower test is kept.
	if (body == nullptr || body->slaveState != 0 || !body->playerControlled)
		return false;
	const flecs::entity entity = fighterEntity(victim);
	const state::CrimeRecord* record = entity.get<state::CrimeRecord>();
	if (record != nullptr && !record->bounties.empty())
		return false;
	const state::Allegiance* own = entity.get<state::Allegiance>();
	if (own == nullptr)
		return false;
	return reportCrime(cuffer, crime::kEnslaving, own->factionSid, victim);
}

void Simulation::stepSlaveTrade()
{
	if (combat_ == nullptr || combat_->empty())
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	constexpr double kChainReach = 4.0; // the scorer's kReach: the member is at the body
	// A conscious body (a crawler the squad shot down) keeps its collision footprint: its carrier stops about 6.5 u from it (kenshi-k16.6.28).
	constexpr double kAwakeBodyReach = 8.0;
	// The chain's own timings (5.8; OURS where marked): the "Restraining" bar is 12 s (kRestrainSeconds, rwh.89); a step
	// that has not finished this much longer ends the chain rather than leaving a member waiting for ever (no terminal state).
	constexpr double kStepGiveUpSeconds = 600.0;

	// 67 "Running": a route to the plan's position, installed only when the member is not already walking (the scorer's own walk is
	// the same route). No navmesh or no route: it stands and the step waits, bounded by the give-up.
	const auto runTo = [&](std::uint64_t id, double tx, double tz)
	{
		state::Fighter& f = *combat_->find(id);
		if (f.walking() || navigation_ == nullptr)
			return;
		f.clearRoute();
		const Route route = routeRetry(id, f, tx, tz, std::nullopt, true);
		if (!route.found || route.points.empty())
			return;
		for (std::size_t p = 0; p < route.points.size(); ++p)
			f.route.push_back({route.points[p].x, p < route.heights.size() ? route.heights[p] : f.y, route.points[p].z});
		f.routeNext = 0;
		++f.routeVersion;
		f.routeSeq = 0; // the AI's own route
		f.routeChase = false;
		f.routeApproach = route.approach;
		f.routeTries = 0;
		mirrorFighter(id);
	};

	// 64 MAKE_TARGET_STAND_STILL (rwh.89 6): while the cuff runs the whole squad leaves the body alone - a captive that wakes during the bar is held,
	// not fought again. The bodies under a bar this tick:
	std::set<std::uint64_t> cuffed;
	medicsOn_.clear();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity member = ecs.entity(combatEntities_[i]);
		const state::Routine* held = member.get<state::Routine>();
		// A medic on its patient: the goal's Routine, or the treatment interact it started (that one outlives the Routine's plan step).
		if (const state::InteractTask* treating = member.get<state::InteractTask>();
			treating != nullptr && treating->action == static_cast<std::uint8_t>(InteractAction::FirstAid) && treating->target != members[i].id)
			++medicsOn_[treating->target];
		else if (held != nullptr && held->goal == routines::task::FirstAid)
			++medicsOn_[held->chainTarget];
		if (held != nullptr && held->goal == routines::task::CaptureSlaves && held->chainElapsed >= 0.0 && held->step < held->plan.size()
			&& held->plan[held->step] == goap::kChainTarget)
			cuffed.insert(held->chainTarget);
	}
	if (!cuffed.empty())
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			state::Fighter& mate = *combat_->find(members[i].id);
			if (cuffed.count(mate.target) != 0)
				mate.target = 0;
		}

	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::Routine* found = actor.get<state::Routine>();
		if (found == nullptr)
			continue;
		// Only the chain's three goals run a plan (167 the cuff, 168 the lift, 112 the cage). A world task's loot (LOOT_THE_DEAD 177,
		// task BO) keeps its body in Routine::chainTarget too; its own step is world_tasks.cpp.
		const std::uint64_t id = members[i].id;
		state::Routine r = *found;
		if (r.goal != routines::task::CaptureSlaves && r.goal != routines::task::CarryWoundedSlaves
			&& r.goal != routines::task::FindCageAndPutIn && r.goal != routines::task::FindBedAndPutIn
			&& r.goal != routines::task::FindCageAndPutDeadGuyIn)
			continue;
		if (r.chainTarget == 0)
			continue;

		state::Fighter& f = *combat_->find(id);
		const std::uint64_t bodyId = r.chainTarget;
		const routines::Site site{static_cast<routines::SiteKind>(r.target.kind), r.target.object, r.target.slot};
		// The put-in site: 112's cage, or 106's bed (the same chain, the bed's own function for the reach).
		const bool bedGoal = r.goal == routines::task::FindBedAndPutIn;
		const routines::SiteKind putKind = bedGoal ? routines::SiteKind::Bed : routines::SiteKind::Cage;
		const std::int32_t putFunction = bedGoal ? routines::kFunctionBed : routines::kFunctionCage;
		const routines::SiteEntry* cage = (r.goal == routines::task::FindCageAndPutIn || r.goal == routines::task::FindCageAndPutDeadGuyIn || bedGoal)
				&& r.target.kind == static_cast<std::uint8_t>(putKind) && residentData_ != nullptr
			? residentData_->sites.siteAt(site) : nullptr;

		// G4 (task CB, kinspect dt18): the plan drives the chain. The selector planned it (a goal whose plan fails scores 0 and is
		// re-planned next update); a Routine that arrives without one (a v78 save load) is planned once here.
		if (r.plan.empty())
		{
			const goap::Plan chain = planSlaveChain(id, r.goal, bodyId, cage != nullptr ? &site : nullptr);
			if (!chain.found())
			{
				if (f.carrying == bodyId)
					dropCarried(id);
				r.chainTarget = 0;
				r.chainElapsed = -1.0;
				actor.set<state::Routine>(std::move(r));
				mirrorFighter(id);
				continue;
			}
			// Model M (kinspect dt24_task_blank sections 1-2): a goal enum is never queued and never ticked - it is the goal record
			// (a resolver-only entry, TaskData+0x134 = 0), condition-tested and popped. The queue holds real task classes only.
			for (const goap::Step& step : chain.steps)
			{
				const goap::TaskRow* row = goap::rowOf(step.task);
				if (row != nullptr && row->hasGoalState)
					continue;
				r.plan.push_back(step.task);
			}
			r.step = 0;
			r.chainElapsed = -1.0;
		}

		state::Fighter* body = combat_->find(bodyId);
		// The chain ends without a trace when the body is gone or dead, the catcher is put down, another member carries it, it wakes
		// before the cuff, or a step takes too long. (167's end log, task BR2.)
		const bool lifted = f.carrying == bodyId;
		if (body != nullptr && body->dead && lifted && r.goal == routines::task::FindCageAndPutIn)
		{
			// The carried man died in the arms: 112's row (isCarriedDudeDead = 0) no longer plans, and the planner's other provider of
			// the same effect, 281 FIND_CAGE_AND_PUT_DEADGUY_IN (dt26 section 0), takes the chain over at the same cage.
			{
				std::ostringstream line;
				line << "capture 281 member " << id << " body " << bodyId << " died in the arms (replanned as 281)";
				logTask(line.str());
			}
			r.goal = routines::task::FindCageAndPutDeadGuyIn;
			r.plan.clear();
			r.step = 0;
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
			continue;
		}
		const bool barRunning = r.step < r.plan.size() && r.plan[r.step] == goap::kChainTarget;
		const char* ended = nullptr;
		if (body == nullptr)
			ended = "the body is gone";
		else if (body->dead && r.goal != routines::task::FindCageAndPutDeadGuyIn) // 281 carries the dead (dt26 section 0)
			ended = "the body died";
		else if (f.dead || f.unconscious || f.down)
			ended = "the catcher is down";
		else if (!lifted && body->carriedBy != 0)
			ended = "another member carries it";
		else if (!lifted && body->slaveState != 1 && !(body->unconscious || body->down) && !(barRunning && r.chainElapsed >= 0.0))
			ended = "the body woke before the cuff"; // once the bar runs the captive is held: it comes round during the cuff and the cuff goes on (rwh.89 run 1)
		else if (r.chainElapsed > kRestrainSeconds + kStepGiveUpSeconds)
			ended = "too long";
		if (ended != nullptr)
		{
			if (lifted)
				dropCarried(id);
			{
				std::ostringstream line;
				line << "capture end " << r.goal << " member " << id << " body " << bodyId << " (" << ended << ")";
				logTask(line.str());
			}
			r.chainTarget = 0;
			r.chainElapsed = -1.0;
			r.plan.clear();
			r.step = 0;
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
			continue;
		}

		// Run the plan: a step that completes at once hands over to the next one in the same tick (the exe's startNextPlanStep runs
		// from checkCurrentTask, so two instant steps take one update; ours takes the tick).
		bool finished = false;
		bool changed = false;
		while (!finished && r.step < r.plan.size())
		{
			const std::uint16_t task = r.plan[r.step];
			if (const goap::TaskRow* row = goap::rowOf(task); row != nullptr && row->hasGoalState)
			{
				++r.step; // a resolver-only entry (model M; a plan stored by a v82 save still carries it): popped, no task runs
				changed = true;
				continue;
			}
			if (task == goap::kRunning)
			{
				// The walk goes where the next step works: to the cage only once the body is in the member's arms (the plan runs
				// 67 at the body -> 168 lift -> 67 at the cage -> 108), otherwise to the body.
				const bool toCage = cage != nullptr && f.carrying == bodyId;
				const double tx = toCage ? cage->x : body->x;
				const double tz = toCage ? cage->z : body->z;
				const double reach = toCage && gameData_ != nullptr
					? functionalityReach(*gameData_, cage->record, putFunction, true) : (body->unconscious ? kChainReach : kAwakeBodyReach);
				if (!f.walking() && std::hypot(f.x - tx, f.z - tz) <= reach)
				{
					++r.step;
					changed = true;
					continue;
				}
				// The walk counts against the give-up like the other steps (a walker that can never arrive is let go, not left for ever).
				r.chainElapsed = r.chainElapsed < 0.0 ? 0.0 : r.chainElapsed + kTickSeconds;
				changed = true;
				runTo(id, tx, tz);
				break; // walking, or standing until the next tick
			}
			if (task == goap::kStripWeapons)
			{
				// STRIP_TARGETS_WEAPONS 181 (kinspect dt3: 8 frames, "Stripping weapons"): the worn weapons go to the ground before the
				// restrain bar starts.
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
				f.yaw = std::atan2(body->x - f.x, body->z - f.z);
				stripTargetWeapons(bodyId);
				// Task BR2: the phases the client's log names (181 stripped, 166 restraining).
				{
					std::ostringstream line;
					line << "capture 181 strip member " << id << " body " << bodyId << ", 166 restrain starts (" << kRestrainSeconds << " s)";
					logTask(line.str());
				}
				++r.step;
				r.chainElapsed = -1.0;
				changed = true;
				continue;
			}
			if (task == goap::kChainTarget)
			{
				if (r.chainElapsed < 0.0)
				{
					if (f.walking() || actor.has<state::InteractTask>() || actor.has<state::DoorStep>()
						|| actor.has<state::CarryStep>() || actor.has<state::TakeOut>())
						break; // not free to restrain yet
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					r.chainElapsed = 0.0;
					changed = true;
					break; // the bar starts next tick
				}
				if (r.chainElapsed < kRestrainSeconds)
				{
					// RESTRAINING: both stand still (a captive that comes round during the bar is held: no target, no walk).
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					if (!body->unconscious)
					{
						body->target = 0;
						body->clearRoute();
						body->forwardSpeed = 0.0;
						body->lateralSpeed = 0.0;
					}
					r.chainElapsed += kTickSeconds;
					changed = true;
					if (r.chainElapsed < kRestrainSeconds - 1.0e-9)
						break;
					if (body->slaveState == 1)
					{
						// An earlier member chained it (task BO sends up to five to one body): this chain has nothing left to cuff,
						// and no second pair of shackles is put on.
						r.chainTarget = 0;
						r.chainElapsed = -1.0;
						r.plan.clear();
						r.step = 0;
						finished = true;
						break;
					}
					// The cuff (TQ22's default): the shackles go on here, then the slave state 1 and the captor faction, written only
					// when empty. Task slaver-crime (dt5): the cuff of a free, unbountied, player-owned person is crime 1 ENSLAVING on
					// the cuffer, before the slave state is written (the test reads that state as "free").
					reportEnslaving(id, bodyId);
					shackleTarget(bodyId);
					setSlaveState(bodyId, 1, id);
					if (body->captorSid.empty())
					{
						if (const state::Allegiance* mine = actor.get<state::Allegiance>())
							body->captorSid = mine->factionSid;
					}
					noteNpcImprisoned(bodyId, body->captorSid);
					noteOwnedSlave(id, bodyId);
					combat_->forgiveHostile(id, bodyId); // a shackled slave is not re-engaged by the squad that chained it,
					body->orderTarget = 0; // and a woken one has dropped its own attack order
					body->target = 0;
					aidGrace_[bodyId] = clockTick() + kSquadIntervalTicks + 1;
					body->engaged.clear(); // the squad's blows ended with the bar (its members' targets were dropped above): the slave is out of the fight for 58's gate (rwh.111)
					mirrorFighter(bodyId);
					++townStats_.slavesCaught;
					{
						std::ostringstream line;
						line << "capture 166 restrain done, 168 cuff + lift member " << id << " body " << bodyId << " (slave of '"
							<< body->captorSid << "')";
						logTask(line.str());
					}
					++r.step;
					continue;
				}
				++r.step; // the bar had already run (a save load mid-restrain)
				changed = true;
				continue;
			}
			if (task == routines::task::CarryWoundedSlaves && r.goal == routines::task::CaptureSlaves)
			{
				// The aid first: the cuffer waits by the body while members are on 58 on it (rwh.89 4, rwh.111); a captive that is up by then walks (65, 171)
				// and is not lifted.
				const auto grace = aidGrace_.find(bodyId);
				if (grace != aidGrace_.end() && grace->second <= clockTick())
					aidGrace_.erase(grace);
				if (aided(bodyId) || aidGrace_.count(bodyId) != 0)
				{
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					changed = true;
					break;
				}
				if (!(body->unconscious || body->down) && actor.target<state::MemberOf>().has<state::SquadRoutine>())
				{
					// A town squad's captive (a warden's arrest: the 112 chain lifts it to the cage whatever its state) is not walked, only a trade squad's.
					r.step = static_cast<std::uint16_t>(r.plan.size());
					finished = true;
					break;
				}
			}
			if (task == routines::task::CarryWoundedSlaves && f.carrying != bodyId
				&& (aided(bodyId) || (aidGrace_.count(bodyId) != 0 && aidGrace_[bodyId] > clockTick())))
			{
				// 170 LIFT_OBJECT_BUT_HEAL_FIRST: the lift waits while members are on 58 on the body, or the cuff's grace for them to start runs (rwh.111;
				// the aid's own give-up bounds it). A carrier that is not the cuffer (goal 168, not 167) has no other gate.
				f.clearRoute();
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
				changed = true;
				break;
			}
			if (task == routines::task::CarryWoundedSlaves)
			{
				// The lift: the carry's arm gate and timer are stepCarry's (the exe's 168 / 68 Lifting).
				if (f.carrying == bodyId)
				{
					++r.step;
					changed = true;
					continue;
				}
				f.clearRoute();
				beginInteract(id, bodyId, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
				changed = true;
				break;
			}
			if (task == goap::kPutInBed)
			{
				// THE BED PUT-IN (99 PUT_SOMEONE_IN_BED, effect isCarriedDudeInBed): release the carry and lay the person on the bed's own
				// spot, asleep, the way InteractAction::Sleep lies a sleeper down (OURS); the bed is claimed for the body while it lies there.
				if (cage == nullptr || f.carrying != bodyId)
					break;
				if (f.walking() || gameData_ == nullptr || std::hypot(f.x - cage->x, f.z - cage->z)
						> functionalityReach(*gameData_, cage->record, putFunction, !f.walking()))
					break;
				putInBed(id, bodyId, *cage);
				{
					std::ostringstream line;
					line << "capture 99 put-in-bed member " << id << " body " << bodyId << " bed " << cage->site.object << ":"
						<< cage->site.slot << " at " << std::hypot(f.x - cage->x, f.z - cage->z) << " u";
					logTask(line.str());
				}
				r.chainTarget = 0;
				r.chainElapsed = -1.0;
				r.plan.clear();
				r.step = 0;
				finished = true;
				break;
			}
			if (task == goap::kPutInCage)
			{
				// THE PUT-IN (108 "Put in prison", effect CARRIED_DUDE_NOW_IN_CAGE): release the carry and set the person in at the
				// cage's own spot, xz of the carrier's floor (OURS, 5.19).
				if (cage == nullptr || f.carrying != bodyId)
					break;
				if (f.walking() || gameData_ == nullptr || std::hypot(f.x - cage->x, f.z - cage->z)
						> functionalityReach(*gameData_, cage->record, putFunction, !f.walking()))
					break;
				imprison(id, bodyId, *cage);
				{
					std::ostringstream line;
					line << "capture 108 put-in member " << id << " body " << bodyId << " cage " << cage->site.object << ":"
						<< cage->site.slot << " at " << std::hypot(f.x - cage->x, f.z - cage->z) << " u";
					logTask(line.str());
				}
				r.chainTarget = 0;
				r.chainElapsed = -1.0;
				r.plan.clear();
				r.step = 0;
				++townStats_.slavesCaged;
				finished = true;
				break;
			}
			// An entry with no branch: popped. The chain ends when the queue drains (model M), not on a task "completing".
			++r.step;
			finished = true;
			changed = true;
		}
		// A plan whose last step is the goal task, and that goal has its own branch above (168's lift): the step was the last, so the
		// chain is done (dt18: the plan's goal state holds).
		if (!finished && r.step >= r.plan.size())
			finished = true;

		if (finished && r.goal == routines::task::CaptureSlaves)
		{
			// The capture goal is the cuff (167's goal state): the lift and the cage belong to the caging goal (112 plans 168 in front of
			// itself), so with nothing in the member's arms the 167 routine is done. Only a chain that arrives carrying (a save from
			// before this slice, mid-lift) keeps it.
			if (f.carrying != bodyId)
			{
				const flecs::entity squad = actor.target<state::MemberOf>();
				if (!squad.is_valid() || !squad.has<state::SquadRoutine>())
				{
					// A world squad member: the routine goes and the selector hands out the lift (its 168 row) or the cage.
					actor.remove<state::Routine>();
					mirrorFighter(id);
					continue;
				}
				// A resident keeps its routine (its scorer needs one): idle it, keeping the chained body, and the next pass plans
				// the caging goal with the lift in front of it.
				r.goal = 0;
				r.target = {};
				r.chainElapsed = -1.0;
				r.plan.clear();
				r.step = 0;
				r.nextScore = 0;
				changed = true;
				actor.set<state::Routine>(std::move(r));
				mirrorFighter(id);
				continue;
			}
			r.chainElapsed = -1.0;
			r.plan.clear();
			r.step = 0;
			changed = true;
		}
		if (finished && r.goal == routines::task::CarryWoundedSlaves)
		{
			// The lift is in (168's own goal, task CB): the routine is done - the scorer hands out the caging goal (112) next.
			const flecs::entity squad = actor.target<state::MemberOf>();
			if (!squad.is_valid() || !squad.has<state::SquadRoutine>())
			{
				actor.remove<state::Routine>();
				mirrorFighter(id);
				continue;
			}
			r.goal = 0;
			r.target = {};
			r.chainElapsed = -1.0;
			r.plan.clear();
			r.step = 0;
			r.nextScore = 0;
			changed = true;
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
			continue;
		}
		if (changed)
		{
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
		}
	}

	// A caged person stays in its cage: it has no Routine to score, but a squad's order or a stray route could still give it one.
	// The saved x / y / z are the cage's spot; the route is dropped where it stands (OURS, 5.19).
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& f = members[i].fighter;
		if (f.cageBuilding == 0 || (!f.walking() && !f.routeChase && f.drive == state::DriveMode::Stop))
			continue;
		state::Fighter& live = *combat_->find(members[i].id);
		live.clearRoute();
		live.drive = state::DriveMode::Stop;
		live.driveStrafe = 0;
		live.forwardSpeed = 0.0;
		live.lateralSpeed = 0.0;
		mirrorFighter(members[i].id);
	}
}

void Simulation::stepCorpsePot()
{
	if (combat_ == nullptr || combat_->empty())
		return;
	// The scorer's kReach: the member stands at the body. The feed's reach is the machine's own (its functionality `use range` x 2 at
	// rest, interact.cpp functionalityReach), the same gate the put-in uses.
	constexpr double kBodyReach = 4.0;
	// A chain that has not finished this long after its walk started is let go rather than leaving a member holding a corpse for
	// ever (stepSlaveTrade's give-up; OURS).
	constexpr double kGiveUpSeconds = 600.0;

	// 67 "Running": a route to the step's position, installed only when the member is not already walking (its own scorer walk is the
	// same route). No navmesh: the straight legs the scorer's own walk uses (a test world; the server has a navigator). No route: it
	// stands and the give-up bounds it.
	const auto runTo = [&](std::uint64_t id, double tx, double tz)
	{
		state::Fighter& f = *combat_->find(id);
		if (f.walking())
			return;
		f.clearRoute();
		// A world squad's walk is a plan (world_tasks' walkTo: the arena's mover follows it); a resident's is the AI's own.
		const flecs::entity squad = fighterEntity(id).target<state::MemberOf>();
		const state::Fighter::PlanSource source = squad.is_valid() && !squad.has<state::SquadRoutine>()
			? state::Fighter::PlanSource::Plan : f.routeSource;
		if (navigation_ == nullptr)
		{
			f.route = {{f.x, f.y, f.z}, {tx, f.y, tz}};
			f.routeNext = 0;
			++f.routeVersion;
			f.routeSeq = 0;
			f.routeChase = false;
			f.routeApproach = false;
			f.routeSource = source;
			f.routeTries = 0;
			mirrorFighter(id);
			return;
		}
		const Route route = routeRetry(id, f, tx, tz, std::nullopt, true);
		if (!route.found || route.points.empty())
			return;
		for (std::size_t p = 0; p < route.points.size(); ++p)
			f.route.push_back({route.points[p].x, p < route.heights.size() ? route.heights[p] : f.y, route.points[p].z});
		f.routeNext = 0;
		++f.routeVersion;
		f.routeSeq = 0; // the AI's own route
		f.routeChase = false;
		f.routeApproach = route.approach;
		f.routeSource = source;
		f.routeTries = 0;
		mirrorFighter(id);
	};
	// A world squad's 137 (the selector's, no SquadRoutine) ends by removing the Routine, like every selector goal; a resident keeps
	// it with goal 0 for the town scorer's next pass.
	const auto releaseWorldPot = [&](flecs::entity actor)
	{
		const flecs::entity squad = actor.target<state::MemberOf>();
		if (squad.is_valid() && !squad.has<state::SquadRoutine>())
			actor.remove<state::Routine>();
	};
	// The members' ids first: a feed removes the consumed body from the arena, which moves the members vector.
	std::vector<std::uint64_t> ids;
	ids.reserve(combat_->members().size());
	for (const CombatArena::Member& member : combat_->members())
		ids.push_back(member.id);
	for (const std::uint64_t id : ids)
	{
		if (combat_->find(id) == nullptr)
			continue;
		flecs::entity actor = fighterEntity(id);
		const state::Routine* found = actor.get<state::Routine>();
		if (found == nullptr || found->goal != routines::task::DeadGuysInThePot || found->chainTarget == 0)
			continue;
		state::Routine r = *found;
		const std::uint64_t bodyId = r.chainTarget;
		// The pot: the scorer's SiteKind::Disposal claim (the placement's key). The machine gone is an end like any other.
		const DisposalSite* pot = r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Disposal)
			? world_.disposalSite(r.target.object) : nullptr;
		const double potReach = pot != nullptr && gameData_ != nullptr && !pot->recordSid.empty()
			? functionalityReach(*gameData_, pot->recordSid, routines::kFunctionDisposal, true) : kBodyReach;
		const auto end = [&](const char* why)
		{
			state::Fighter* live = combat_->find(id);
			if (live != nullptr && live->carrying == bodyId)
				dropCarried(id);
			std::ostringstream line;
			line << "corpse end 137 member " << id << " body " << bodyId << " (" << why << ")";
			logTask(line.str());
			state::Routine& out = *actor.get_mut<state::Routine>();
			out.chainTarget = 0;
			out.chainElapsed = -1.0;
			out.plan.clear();
			out.step = 0;
			out.goal = 0;
			releaseWorldPot(actor);
			mirrorFighter(id);
		};
		const state::Fighter& self = *combat_->find(id);
		state::Fighter* body = combat_->find(bodyId);
		const char* ended = nullptr;
		if (pot == nullptr)
			ended = "the machine is gone";
		else if (body == nullptr)
			ended = "the body is gone";
		else if (!body->dead)
			ended = "the body is not dead";
		else if (self.dead || self.unconscious || self.down)
			ended = "the carrier is down";
		else if (self.carrying != bodyId && body->carriedBy != 0)
			ended = "another member carries it";
		if (ended != nullptr)
		{
			end(ended);
			continue;
		}
		// G4: the plan. The selector planned the fetch half (empty-handed) or the feed half (carrying); a Routine that arrives
		// without one (a v82 save load, or the fetch half drained with the body now in the arms) plans it here. No plan: the goal
		// scores 0 next pass and the body goes down.
		if (r.plan.empty())
		{
			const goap::Plan chain = planCorpsePot(id, bodyId, pot->x, pot->z);
			if (!chain.found())
			{
				end("no plan");
				continue;
			}
			for (const goap::Step& step : chain.steps)
				r.plan.push_back(step.task);
			r.step = 0;
			r.chainElapsed = -1.0;
		}
		bool finished = false;
		bool changed = false;
		while (!finished && r.step < r.plan.size())
		{
			const std::uint16_t task = r.plan[r.step];
			if (const goap::TaskRow* row = goap::rowOf(task); row != nullptr && row->hasGoalState)
			{
				++r.step; // the 137 / 138 markers (Task_Blank goal records): popped, no task runs
				changed = true;
				continue;
			}
			state::Fighter& f = *combat_->find(id);
			if (task == goap::kRunning)
			{
				// The walk goes where the next step works: the body while the arms are empty, the machine once it is carried.
				const bool toPot = f.carrying == bodyId;
				const double tx = toPot ? pot->x : body->x;
				const double tz = toPot ? pot->z : body->z;
				const double reach = toPot ? potReach : kBodyReach;
				if (!f.walking() && std::hypot(f.x - tx, f.z - tz) <= reach)
				{
					++r.step;
					changed = true;
					continue;
				}
				r.chainElapsed = r.chainElapsed < 0.0 ? 0.0 : r.chainElapsed + kTickSeconds;
				changed = true;
				runTo(id, tx, tz);
				break; // walking, or standing until the next tick
			}
			if (goap::isLiftTask(task))
			{
				// 68 LIFT_PERSON (and its Task_LiftSomething twins) at the body: the carry order (stepCarry's arm gate and timer); the step ends when the lift does. The
				// cage fields clear on the lift: a body taken out of a cage frees its slot (the gather goal 173's rule, world_tasks).
				if (f.carrying == bodyId)
				{
					if (body->cageBuilding != 0)
					{
						body->cageBuilding = 0;
						body->cageSlot = 0;
						mirrorFighter(bodyId);
					}
					++r.step;
					changed = true;
					continue;
				}
				const state::CarryStep* carry = actor.get<state::CarryStep>();
				if (carry != nullptr && carry->target == bodyId && carry->phase == 1)
				{
					++r.step;
					changed = true;
					continue;
				}
				r.chainElapsed = r.chainElapsed < 0.0 ? 0.0 : r.chainElapsed + kTickSeconds;
				changed = true;
				if (carry != nullptr || actor.has<state::InteractTask>())
					break; // under way (or the order is)
				if (f.walking() || std::hypot(f.x - body->x, f.z - body->z) > kBodyReach)
					break; // still on the way (the walk above)
				beginInteract(id, bodyId, static_cast<std::uint8_t>(InteractAction::Carry), 0, {}, true);
				break;
			}
			if (task == routines::task::FeedCorpseIntoMachine)
			{
				// THE FEED (136's tick and completion, dt24_patrol_town_pot section 3): at the machine with the body in the arms, drop
				// it and consume it - 140799af0(body, 1, "incinerated or cooked"). The exe waits out 0.2 of the shoulder-lift clip
				// first; that is animation timing we do not model (UNSURE, cosmetic). The empty hands are 137's goal state (100).
				if (f.carrying != bodyId)
				{
					++r.step; // nothing to feed: the step means nothing without the body in the arms
					changed = true;
					continue;
				}
				if (f.walking() || std::hypot(f.x - pot->x, f.z - pot->z) > potReach)
				{
					r.chainElapsed = r.chainElapsed < 0.0 ? 0.0 : r.chainElapsed + kTickSeconds;
					changed = true;
					break; // the walk above (or no route: the give-up)
				}
				const double distance = std::hypot(f.x - pot->x, f.z - pot->z);
				dropCarried(id);
				{
					std::ostringstream line;
					line << "corpse 136 feed member " << id << " body " << bodyId << " into building " << pot->key << " at "
						<< distance << " u (incinerated or cooked)";
					logTask(line.str());
				}
				++townStats_.corpsesDisposed;
				// The body is consumed: out of the arena and the world (the exe's 140799af0). Nothing else touches `f` after this.
				removeFighter(bodyId);
				r.chainTarget = 0;
				r.chainElapsed = -1.0;
				r.plan.clear();
				r.step = 0;
				r.goal = 0;
				r.nextScore = 0; // re-score at once: the next corpse can be fetched
				actor.set<state::Routine>(std::move(r));
				releaseWorldPot(actor);
				mirrorFighter(id);
				finished = true;
				break;
			}
			// A step this executor does not run: popped (the queue ends when it drains, model M).
			++r.step;
			changed = true;
		}
		if (!finished && r.chainElapsed > kGiveUpSeconds)
		{
			end("too long");
			continue;
		}
		if (!finished && r.step >= r.plan.size())
		{
			// The queue drained with the body still in hand (the fetch half done, the feed outstanding): re-plan next tick rather
			// than standing with a corpse for ever. Cleared here, the plan is rebuilt from the live hands by the branch above.
			if (const state::Fighter* live = combat_->find(id); live != nullptr && live->carrying == bodyId)
			{
				r.plan.clear();
				r.step = 0;
				r.chainElapsed = -1.0;
				changed = true;
			} else {
				end("the fetch did not end with the body in the arms");
				continue;
			}
		}
		if (changed)
		{
			actor.set<state::Routine>(std::move(r));
			mirrorFighter(id);
		}
	}
}

// ---- the dialogue effects on the slave state (DIALOGUE_ACTIONS_STATIC 24, 53, 58; DIALOGUE.md 5) ----

bool Simulation::removeSlaveStatus(std::uint64_t character)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(character) : nullptr;
	if (f == nullptr)
		return false;
	// DA_REMOVE_SLAVE_STATUS: not a slave, no captor, out of the cage. The cage slot's occupancy is the captive's own saved fields
	// (routine_scorer's "a cage holding someone is not free"), so clearing them frees the slot. UNSURE: the exe's shackle unlock, its
	// "Free!" text and its sound are not modelled.
	setSlaveState(character, 0);
	f->captorSid.clear();
	f->cageBuilding = 0;
	f->cageSlot = 0;
	f->goal = state::IndividualGoal{};
	cancelOrders(character);
	CombatArena::endFollow(*f);
	f->clearRoute();
	f->drive = state::DriveMode::Stop;
	f->driveStrafe = 0;
	f->orderTarget = 0;
	f->orderStop = true;
	f->hold = false;
	mirrorFighter(character);
	return true;
}

bool Simulation::giveSlavesTo(const std::string& ownerSid, std::uint64_t target)
{
	const state::Fighter* t = combat_ != nullptr ? combat_->find(target) : nullptr;
	if (t == nullptr || ownerSid.empty())
		return false;
	// The new owner is T's side. A player-controlled T without an Allegiance takes the player faction's sid.
	std::string targetSide;
	if (const state::Allegiance* side = fighterEntity(target).get<state::Allegiance>())
		targetSide = side->factionSid;
	if (targetSide.empty() && t->playerControlled)
	{
		flecs::world& ecs = ecsMutable();
		ecs.each([&](flecs::entity entity, const state::PlayerFaction&)
		{
			if (const state::FactionState* faction = entity.get<state::FactionState>())
				targetSide = faction->templateSid;
		});
	}
	if (targetSide.empty())
		return false;
	// S's ownership list is the captorSid set in our model (there is no ownership container to clear); a carried slave is dropped, as
	// the exe does. OURS: "the whole inventory of slaves transfers" is nothing more than the characters themselves.
	for (const CombatArena::Member& member : combat_->members())
	{
		state::Fighter* f = combat_->find(member.id);
		if (f == nullptr || f->captorSid != ownerSid || f->carriedBy != 0)
			continue;
		f->captorSid = targetSide;
		mirrorFighter(member.id);
	}
	return true;
}

bool Simulation::knockOut(std::uint64_t character, double seconds)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(character) : nullptr;
	if (f == nullptr || f->dead)
		return false;
	// DA_KNOCKOUT: the exe writes the knockout timer field (MedicalSystem+0xA0) directly, forcing or extending the KO. Seconds here;
	// UNSURE: the exe's field units are unverified (its one data use is operand 60).
	f->koTimer = std::max(f->koTimer, seconds);
	f->unconscious = true;
	mirrorFighter(character);
	return true;
}

} // namespace ksim
