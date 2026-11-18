#include "ksim/simulation.hpp"

#include "capture_target.hpp"
#include "resident_data.hpp"
#include "world_resolvers.hpp"

#include "ksim/behaviours.hpp"
#include "ksim/blackboard.hpp"
#include "ksim/clock.hpp"
#include "ksim/goalsel.hpp"
#include "ksim/crime.hpp"
#include "ksim/factions.hpp"
#include "ksim/interact.hpp"
#include "ksim/rng.hpp"
#include "ksim/routines.hpp"
#include "ksim/shopping.hpp"
#include "ksim/templates.hpp"
#include "ksim/towncircle.hpp"

#include "kworld/collision.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace ksim {

// The town goal scorer (TOWN_LIFE_SLICE 5.3, T3b2): for every promoted resident, hour windows, then strict tier, then
// utility x weight (crowd decay is 1.0 for town tasks: the finders' claims do the crowding). A failed finder blocks its (goal, target)
// pair for 20 s. STAY_IN_HOME is the last candidate of everybody, so no member is ever without something to do (no terminal state, D6).
// Goals run here: STAY_IN_HOME 43, Shopkeeper 20, STAND_AT_GUARD_NODE_HOMEBUILDING 38, post 157, PATROL_TOWN 36, pretend labour
// 189 / 190 / 221 (T3b2); beds 153 / 239, the door routine 155 / 197 / 86 and shooing 158 / 283 (T3c, TOWN_LIFE_SLICE 5.16). The other
// modelled goals (bar, cages, slaves) belong to later slices and are skipped.

namespace {

// How close to a turret's post the member stands to mount it (task ds/turret S3; the walk's arrival is within a few units, UNSURE).
constexpr double kMountReach = 12.0;
// How often stepRoutines looks at the due members. A member's own cadence is Routine::nextScore (kScoreIntervalTicks).
constexpr std::uint64_t kLookTicks = 5;
// A member this close to its spot (xz, units) has arrived.
constexpr double kReach = 4.0;
// CAPTURE_NEW_SLAVES 167 (T3e2) sees a downed body within this of the member: "no radius beyond the sensor list" (5.8), ours being the
// intruder sight (OURS: the sensor itself has no decoded range).
constexpr double kCatchSight = 60.0;
// (216's route constants are world_resolvers.hpp kSearchPointsMin ... kSearchSnapReach: the same route runs for a world squad.)
// Pretend labour works at a producer BUILDING within this of the home point (UNSURE: the data has no work-station marker of a town).
constexpr double kWorkReach = 250.0;
// Residents standing at the home point spread over a disc of this radius, per member.
constexpr double kHomeJitter = 3.0;
// STAY_IN_HOME is "ordinary life at home": a member already this close to its home point is at home and does not shuffle to a spot (OURS;
// it also leaves a packageless member, a merchant, where promotion put it).
constexpr double kHomeSettle = 25.0;
// The door side of a building: the first door moved this far toward the building centre (inside the door).
constexpr double kDoorInset = 4.0;
// Synthetic guard nodes: a placed building (UNSURE: a constant, so the index does not depend on who asked first), a squad's yard, a town.
constexpr int kBuildingGuardNodes = 8;
constexpr int kYardGuardNodes = 6;
constexpr int kYardBeds = 24; // a yard's synthetic beds, shared by every squad homed on its point (OURS)
constexpr int kTownNodes = 12;
constexpr double kTownNodeRadius = 30.0;
// T3c (5.16). A resident locks or opens a door instantly from within this of it (xz; the live Paste case: the lock took effect 50 u away,
// ours is tighter, UNSURE). Walking to the door side first is the ordinary walk.
constexpr double kDoorReach = 12.0;
// A sleeper arrives at its bed within this (xz) or, when the walk ended short of an unreachable bed (a storm house has no inside), after kBedSettleTicks.
constexpr double kBedArrive = 12.0;
constexpr std::uint64_t kBedSettleTicks = 600;
// A real (layout) bed is reachable only when the route ends within kReach of its use node and within this of its floor (y): one whose walk
// ends short, or on another storey, is blocked like a site with no route (OURS: the parked-in-a-doorway sleeper).
constexpr double kFloorReach = 6.0;
// Shooing a stranger that does not leave: after this long (game ticks) the pair is rested for kShooRestTicks so the resident gets on with its
// day (the trespass / crime path owns the real answer, 5.6); no layer waits for a stranger for ever.
constexpr std::uint64_t kShooPatienceTicks = kTicksPerGameHour / 2;
constexpr std::uint64_t kShooRestTicks = 2 * kTicksPerGameHour;
// Bed utility: 0 for this many game hours after the last sleep (the 180 min after waking, 5.3); otherwise 1.
constexpr double kWakeCooldownHours = routines::kBedWakeCooldownMinutes / 60.0;

bool implemented(std::uint16_t task) noexcept
{
	switch (task)
	{
	case routines::task::Shopkeeper:
	case routines::task::Patrol:
	case routines::task::PatrolTown:
	case routines::task::RunAroundTown:
	case routines::task::StaticGuardBuilding:
	case routines::task::StaticGuardBuildingIndoors: // 38's twin (task CH group 3)
	case routines::task::StayInHome:
	case routines::task::SitOnThrone: // 255 (dt24_43 section 2c): 43's resolver on the home building's throne slot
	case routines::task::StaticGuardTown:
	case routines::task::ManTurrets: // task CH turrets: the finder, the walk stands at the turret (no mounting yet)
	case routines::task::TurretOnBuilding: // 222, the 234 player-job resolver body
	case routines::task::PretendLabour:
	case routines::task::PretendLabourPretend:
	case routines::task::PretendToOperate:
	case routines::task::GoHomeToBed:
	case routines::task::GoHomeToBedSecure:
	case routines::task::GetOutOfBedEmergency: // 208 (dt24_208 / s63): the Urgent wake, an instant act
	case routines::task::LockAllMyDoors:
	case routines::task::LockDoorsStayOutside:
	case routines::task::OpenUpShopDoors:
	case routines::task::ShooStrangers:
	case routines::task::ShooStrangersPrivate:
	case routines::task::RelaxInTownPackage:
	case routines::task::Shopping:
	case routines::task::PickupIntruders: // T3e1: adopted, stepIntruders executes it (carry.cpp)
	case routines::task::TakeIntruderOutside: // the executor's phase, never planned here
	case routines::task::PickupIntrudersTown: // the town scope of 223 (task CH group 3)
	case routines::task::TakeIntruderOutsideTown: // the executor's phase of 274
	case routines::task::HuntBounties: // crime-rows: the finders are Simulation's (crime_goals.cpp), stepCrimeGoals executes
	case routines::task::InvestigateAlarms:
	case routines::task::InvestigateAlarmsAlliesOnly:
	case routines::task::PoliceFreePrisoners:
	case routines::task::LootStolenGoods:
	case routines::task::CaptureSlaves: // T3e2: the scorer picks the target, stepSlaveTrade chains and lifts it (carry.cpp)
	case routines::task::FindCageAndPutIn: // T3e2: the cage walk is walked, the put-in is stepSlaveTrade's
	case routines::task::SlaveWatching: // T3e2: stand at a cage of the home that holds a slave
	case routines::task::LockAllMyCages: // T3e2: instant, from inside the home
	case routines::task::CarryWoundedSlaves: // T3e2: the chain's own lift (stepSlaveTrade); never planned here
	case routines::task::StripSlaves: // T3e2: no package carries it (merged data); no stripping is modelled (5.19)
	case routines::task::DeadGuysInThePot: // the corpse pot: the scorer finds the machine and the body, stepCorpsePot lifts and feeds
		return true;
	default:
		return false;
	}
}

// Utilities (ORIGINAL_AI / item 57): node 38 / 268 and Shopkeeper 0.5; post 157 and STAY_IN_HOME rand x 0.4 + 0.01; PATROL_TOWN 0.25;
// labour 1.0. UNSURE: PRETEND_TO_OPERATE 221 takes the labour value.
double utilityOf(std::uint16_t task, double roll) noexcept
{
	switch (task)
	{
	case routines::task::Shopkeeper:
	case routines::task::StaticGuardBuilding:
	case routines::task::StaticGuardBuildingIndoors:
		return routines::kUtilityNode;
	case routines::task::StaticGuardTown:
	case routines::task::StayInHome:
	case routines::task::SitOnThrone:
		return roll * routines::kUtilityRandMax + routines::kUtilityRandBase;
	case routines::task::Patrol:
	case routines::task::PatrolTown:
		return routines::kUtilityPatrol;
	case routines::task::RunAroundTown:
		return 1.0; // UNSURE: the registry's utility for 216 is not decoded (dt24_36 section 1)
	default:
		return routines::kUtilityDefault;
	}
}

bool bedTask(std::uint16_t task) noexcept
{
	return task == routines::task::GoHomeToBed || task == routines::task::GoHomeToBedSecure;
}

bool exclusive(routines::SiteKind kind) noexcept
{
	return kind == routines::SiteKind::GuardNode || kind == routines::SiteKind::TownNode || kind == routines::SiteKind::Bed
		|| kind == routines::SiteKind::Table || kind == routines::SiteKind::Counter || kind == routines::SiteKind::Work
		|| kind == routines::SiteKind::Cage || kind == routines::SiteKind::Turret || kind == routines::SiteKind::Disposal
		|| kind == routines::SiteKind::Throne || kind == routines::SiteKind::Seat || kind == routines::SiteKind::BarSeat
		|| kind == routines::SiteKind::ShopkeeperNode;
}

routines::Site fromState(const state::RoutineSite& site)
{
	return {static_cast<routines::SiteKind>(site.kind), site.object, site.slot};
}

state::RoutineSite toState(const routines::Site& site)
{
	return {static_cast<std::uint8_t>(site.kind), site.object, site.slot};
}

auto blockKey(const state::RoutineBlocked& b)
{
	return std::make_tuple(b.goal, b.target.kind, b.target.object, b.target.slot);
}

bool isBlocked(const state::Routine& r, std::uint16_t goal, const state::RoutineSite& site, std::uint64_t now)
{
	for (const state::RoutineBlocked& b : r.blocked)
		if (b.goal == goal && b.target == site && b.until > now)
			return true;
	return false;
}

void addBlock(state::Routine& r, std::uint16_t goal, const state::RoutineSite& site, std::uint64_t until)
{
	for (state::RoutineBlocked& b : r.blocked)
	{
		if (b.goal == goal && b.target == site)
		{
			b.until = until;
			return;
		}
	}
	r.blocked.push_back({goal, site, until});
	std::sort(r.blocked.begin(), r.blocked.end(), [](const auto& a, const auto& b) { return blockKey(a) < blockKey(b); });
}

// ---- the site index (derived) ----

void knowBuildingSites(ResidentData& data, const World& world)
{
	if (data.buildingSitesKnown)
		return;
	data.buildingSitesKnown = true;
	const std::vector<BuildingSite>& sites = world.buildingSites();
	for (std::size_t i = 0; i < sites.size(); ++i)
		data.buildingSiteByKey.emplace(sites[i].key, i);
	const std::vector<ProducerSite>& producers = world.producerSites();
	for (std::size_t i = 0; i < producers.size(); ++i)
		data.producerById.emplace(producers[i].id, i);
}

// Every turret BUILDING of the loaded world, key order (task CH turrets; dt24_turret_finder section 2: the class is chosen by the
// functionality function 12). The exe's finder queries its building registry; ours is the world's placements (task ds/turret: a turret has
// no door and no hull of its own, so it is never a BuildingSite - the finder read the sites and found none in the vanilla world; 695 placements
// are turrets). The key is the placement's, the one a BuildingSite would carry.
void knowTurrets(ResidentData& data, const World& world, const kdata::GameData& gameData, bool manning)
{
	if (data.turretsKnown)
		return;
	data.turretsKnown = true;
	if (!manning) // Simulation::setTurretManning: no turret is offered (the state before ds/turret, when the sites list held none)
		return;
	for (const kworld::PlacedInstance& placed : world.placements())
	{
		const kdata::Record* record = gameData.find(placed.buildingID);
		if (record == nullptr || !routines::isTurretBuilding(gameData, *record))
			continue;
		// The manning spot: the turret's own `n-use node` child instance (every turret BUILDING has one - it is the operator position the
		// exe's OperateMachine start walks to), placed by the placement's quaternion. OURS: no use node -> the placement's origin.
		const std::array<double, 4> q{placed.qw, placed.qx, placed.qy, placed.qz};
		double x = placed.x;
		double z = placed.z;
		double y = placed.y;
		const std::vector<routines::UseNode> nodes = routines::useNodePoses(gameData, *record);
		if (!nodes.empty())
		{
			const std::array<double, 3> local = routines::rotate(q, nodes.front().at);
			x += local[0];
			y += local[1];
			z += local[2];
		}
		const std::array<double, 3> ahead = routines::rotate(q, {0.0, 0.0, 1.0});
		const kdata::RefCategory* gun = record->findRefs("gun turret");
		data.turretSites.push_back({kworld::doorKey(kworld::placementId(placed)), x, z, y, std::atan2(ahead[0], ahead[2]), -2,
			gun != nullptr && !gun->entries.empty() ? gun->entries.front().targetID : std::string()});
	}
	std::sort(data.turretSites.begin(), data.turretSites.end(),
		[](const ResidentData::TurretInfo& a, const ResidentData::TurretInfo& b) { return a.key < b.key; });
}

// The first (lowest key) door of every building and the Door entities of each, rebuilt when the number of Door entities changes (zones
// furnish doors as they load).
void knowDoors(ResidentData& data, flecs::world& ecs)
{
	const std::size_t count = static_cast<std::size_t>(ecs.count<state::Door>());
	if (count == data.doorsSeen)
		return;
	data.doorsSeen = count;
	data.doorOfBuilding.clear();
	data.doorsOfBuilding.clear();
	std::unordered_map<std::uint64_t, std::uint64_t> lowest;
	std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint64_t, flecs::entity_t>>> all;
	ecs.each([&](flecs::entity entity, const state::Door& door)
	{
		if (door.building == 0)
			return;
		all[door.building].emplace_back(door.key, entity.id());
		const auto at = lowest.find(door.building);
		if (at == lowest.end() || door.key < at->second)
		{
			lowest[door.building] = door.key;
			data.doorOfBuilding[door.building] = {door.x, door.z};
		}
	});
	for (auto& [building, doors] : all)
	{
		std::sort(doors.begin(), doors.end());
		for (const auto& [key, id] : doors)
			data.doorsOfBuilding[building].push_back(id);
	}
}

// The spec of a placed building, all synthetic (TOWN_LIFE_SLICE 5.15). Null when the world has no such building.
const BuildingSite* buildingSpec(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key,
	routines::BuildingSpec& spec)
{
	knowBuildingSites(data, world);
	knowDoors(data, ecs);
	const auto found = data.buildingSiteByKey.find(key);
	const BuildingSite* placed = found != data.buildingSiteByKey.end() ? &world.buildingSites()[found->second] : world.buildingSite(key);
	if (placed == nullptr)
		return nullptr;
	const BuildingSite& building = *placed; // a furniture cage (BuildingSite::furnitureCage) is world.buildingSite's alone
	// The building's designation and public flags: its squad template (the zone state's `squad template`, BuildingSite::squadTemplate).
	// The 153 / 239 bed ladder and the bar finder read them (TOWN_LIFE_SLICE 5.4).
	spec.key = key;
	spec.ownerFaction = building.ownerFaction;
	if (!building.squadTemplate.empty())
		if (const kdata::Record* squad = gameData.find(building.squadTemplate))
		{
			spec.designation = squad->intOr("building designation", 0);
			spec.publicBuilding = squad->boolOr("public day", true) || squad->boolOr("public night", false);
			spec.publicBeds = squad->boolOr("public beds", false);
		}
	spec.cx = building.cx;
	spec.cz = building.cz;
	spec.doorX = spec.cx;
	spec.doorZ = spec.cz;
	if (const auto door = data.doorOfBuilding.find(key); door != data.doorOfBuilding.end())
	{
		const double dx = spec.cx - door->second[0];
		const double dz = spec.cz - door->second[1];
		const double length = std::hypot(dx, dz);
		spec.doorX = door->second[0] + (length > kDoorInset ? dx / length * kDoorInset : dx);
		spec.doorZ = door->second[1] + (length > kDoorInset ? dz / length * kDoorInset : dz);
	}
	spec.yawCos = building.axisX;
	spec.yawSin = building.axisZ;
	spec.guardCount = kBuildingGuardNodes;
	spec.memberCount = kBuildingGuardNodes;
	return &building;
}

// The yard of a squad whose members have no home building: the squad's position is the home point, everything round it. Keyed by that
// position, so the squads that share a point (a town's homeless squads all stand on its town point) share one bed grid and its claims;
// keyed per squad, every squad laid the same grid and its sleepers stacked on another's (client live Heng check, 2026-11-04).
std::uint64_t ensureYard(ResidentData& data, double x, double z)
{
	const std::uint64_t key = (streamSeed(std::bit_cast<std::uint64_t>(x), static_cast<std::size_t>(std::bit_cast<std::uint64_t>(z)))
		& ~(ResidentData::kYardBit | ResidentData::kTownBit)) | ResidentData::kYardBit;
	if (data.yards.count(key) != 0)
		return key;
	routines::BuildingSpec spec;
	spec.key = key;
	spec.cx = spec.doorX = x;
	spec.cz = spec.doorZ = z;
	spec.guardCount = kYardGuardNodes;
	spec.memberCount = kYardBeds;
	data.sites.add(spec);
	data.yards[key] = {x, z};
	return key;
}

// A town's outside nodes (post 157): a ring round the town's position. The data holds no outside-node marker (UNSURE), so they are synthetic.
std::uint64_t ensureTown(ResidentData& data, std::uint64_t townId, double x, double z)
{
	const std::uint64_t key = townId | ResidentData::kTownBit;
	if (!data.towns.insert(key).second)
		return key;
	routines::BuildingSpec spec;
	spec.key = key;
	spec.cx = spec.doorX = x;
	spec.cz = spec.doorZ = z;
	for (int i = 0; i < kTownNodes; ++i)
	{
		const double angle = 2.0 * std::numbers::pi * i / kTownNodes;
		spec.pieces.push_back({routines::SiteKind::TownNode, kTownNodeRadius * std::cos(angle), kTownNodeRadius * std::sin(angle)});
	}
	data.sites.add(spec);
	return key;
}

// ---- the bar hangout (T3d, TOWN_LIFE_SLICE 5.17) ----

double unitOf(std::uint64_t hash) noexcept
{
	return static_cast<double>(hash >> 11) * 0x1.0p-53;
}

// Every town draw of the bar routine is hash(world seed, id, goal, hour index): a pure function, so nothing is saved.
std::uint64_t drawHash(std::uint64_t seed, std::uint64_t id, std::uint16_t goal, std::uint64_t hourIndex) noexcept
{
	std::uint64_t h = splitMix64(seed ^ 0x6ba2d0000000ULL);
	h = splitMix64(h ^ id);
	h = splitMix64(h ^ goal);
	return splitMix64(h ^ hourIndex);
}

// A table's Site.object: the claim names the table, slot 0 (what SquadRoutine::barTable and rebuildRoutineBook assume).
std::uint64_t tableKey(std::uint64_t building, std::uint16_t slot) noexcept
{
	return splitMix64(building ^ splitMix64(0x7ab1e000ULL + slot)) | 1;
}

// A layout piece that is a chair or stool (function 17, TQ23): not a site, but where a squad member sits at a table.
bool isChairPiece(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return false;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		const kdata::Record* record = data.find(entry.targetID);
		if (record != nullptr && record->intOr("function", 0) == routines::kFunctionChair)
			return true;
	}
	return false;
}

// The `flags` of a chair piece's chair functionality (bed_seat_pose.md 3: 1 `chair` / `sit on floor`, 2 `chair-fixed 180`, 0 fixed).
std::int32_t chairFlags(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	for (std::size_t i = 0; functionality != nullptr && i < functionality->entries.size(); ++i)
	{
		const kdata::Record* record = data.find(functionality->entries[i].targetID);
		if (record != nullptr && record->intOr("function", 0) == routines::kFunctionChair)
			return record->intOr("flags", 0);
	}
	return 0;
}

// A placed furniture building that is a cage itself (BuildingSite::furnitureCage): one Cage entry, its use node nearest the placement
// (the same walkable spot beside the cage as a layout cage's), in the spec's frame.
void addFurnitureCage(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key)
{
	routines::BuildingSpec spec;
	const BuildingSite* building = buildingSpec(data, world, ecs, gameData, key, spec);
	const kdata::Record* record = building != nullptr ? gameData.find(building->buildingSid) : nullptr;
	if (record == nullptr)
		return;
	double x = 0.0;
	double z = 0.0;
	double best = 1.0e30;
	for (const std::array<double, 3>& node : routines::useNodes(gameData, *record))
	{
		const std::array<double, 2> at = routines::pieceToLayout(routines::LayoutPiece{}, node);
		if (const double d = std::hypot(at[0], at[1]); d < best)
		{
			best = d;
			x = at[0];
			z = at[1];
		}
	}
	const std::array<double, 3> at = layoutToWorld(*building, 1.0, x, 0.0, z);
	const double dx = at[0] - building->cx;
	const double dz = at[2] - building->cz;
	spec.pieces.push_back({routines::SiteKind::Cage, spec.yawCos * dx - spec.yawSin * dz, spec.yawSin * dx + spec.yawCos * dz, at[1]});
	spec.hasHeight = true;
	spec.originY = 0.0;
	data.sites.add(spec);
}

// Adds a placement's layout furniture to the site index: the placement's `interior layout name` (zone GAMESTATE_BUILDING; the template's `layout
// interior` when the placement has none) of this building's BUILDING record; its Table pieces (layout node function 16, TQ23), Counter pieces
// (function 9) and Bed pieces (function 6, at a use node: routines::bedPieces), placed from the placement origin. The layout handedness is
// still UNSURE, so both mirror images are tried and the one with more pieces inside the footprint wins (ties: B = (-axisZ, axisX), what the placement quaternion gives); pieces outside it are dropped. No table
// inside: the building keeps its synthetic centre table (and the same for the counter). Each table also gets its seats: the chairs and stools of
// the layout within kSeatReach of it, nearest first (ResidentData::seatsOfTable).
void addLayoutFurniture(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key,
	const std::string& layoutName)
{
	if (!data.tabled.insert(key).second)
		return;
	routines::BuildingSpec spec;
	const BuildingSite* building = buildingSpec(data, world, ecs, gameData, key, spec);
	if (building == nullptr)
		return;
	const routines::Layout* layout = data.layouts != nullptr && !layoutName.empty() ? data.layouts->find(building->buildingSid + "-" + layoutName) : nullptr;
	if (layout == nullptr)
		return;
	struct Placed
	{
		routines::SiteKind kind;
		double wx;
		double wz;
	};
	std::vector<routines::Piece> pieces; // Table, Counter, GuardNode, Bed, Cage
	struct Chair
	{
		double x; // building-local
		double z;
		double y;
		std::array<double, 3> face; // its +Z, building-local
		std::int32_t flags; // the chair functionality's `flags`
		std::string clip;
	};
	std::vector<Chair> chairs;
	for (const routines::LayoutPiece& piece : layout->pieces)
	{
		const kdata::Record* record = gameData.find(piece.target);
		if (record == nullptr)
			continue;
		const routines::SiteKind kind = routines::isShopkeeperNode(*record) ? routines::SiteKind::ShopkeeperNode : routines::classifyPiece(gameData, *record);
		if (kind == routines::SiteKind::Table || kind == routines::SiteKind::Counter || kind == routines::SiteKind::GuardNode
			|| kind == routines::SiteKind::Throne || kind == routines::SiteKind::ShopkeeperNode)
		{
			pieces.push_back({kind, piece.x, piece.z, piece.y});
		} else if (kind == routines::SiteKind::Cage) {
			// A cage itself is a hole in the mesh (like a bed): its site is the use node nearest the piece, a walkable spot beside it
			// (OURS: the original's cage node is not decoded; the occupant is put there, 5.19).
			double x = piece.x;
			double z = piece.z;
			double best = 1.0e30;
			for (const std::array<double, 3>& node : routines::useNodes(gameData, *record))
			{
				const std::array<double, 2> at = routines::pieceToLayout(piece, node);
				const double d = std::hypot(at[0] - piece.x, at[1] - piece.z);
				if (d < best)
				{
					best = d;
					x = at[0];
					z = at[1];
				}
			}
			pieces.push_back({kind, x, z, piece.y});
		} else if (isChairPiece(gameData, *record)) {
			chairs.push_back({piece.x, piece.z, piece.y, routines::rotate({piece.qw, piece.qx, piece.qy, piece.qz}, {0.0, 0.0, 1.0}),
				chairFlags(gameData, *record), routines::functionClip(gameData, *record, routines::kClipSittingChair)});
		}
	}
	for (const routines::Piece& bed : routines::bedPieces(gameData, *layout))
		pieces.push_back(bed);
	std::vector<std::array<double, 2>> local;
	for (const routines::Piece& piece : pieces)
		local.push_back({piece.x, piece.z});
	const double bestHand = layoutHand(*building, local);
	// The placement's full rotation, not a yaw: a building on a slope is tilted and its floor with it, so a piece's height depends
	// on where in the building it stands (layoutToWorld).
	const auto world3 = [&](double hand, double x, double y, double z) { return layoutToWorld(*building, hand, x, y, z); };
	const auto inside = [&](const std::array<double, 3>& at) { return insideFootprint(*building, {at[0], at[2]}); };
	std::vector<std::array<double, 2>> tables; // world positions of the kept tables, slot order
	for (const routines::SiteKind kind : {routines::SiteKind::Table, routines::SiteKind::Counter, routines::SiteKind::GuardNode,
		routines::SiteKind::Bed, routines::SiteKind::Cage, routines::SiteKind::Throne, routines::SiteKind::ShopkeeperNode})
	{
		for (const routines::Piece& piece : pieces)
		{
			if (piece.kind != kind)
				continue;
			const std::array<double, 3> at = world3(bestHand, piece.x, piece.y, piece.z);
			if (!inside(at))
				continue;
			const double dx = at[0] - building->cx;
			const double dz = at[2] - building->cz;
			// back to the spec's frame: SiteIndex::add computes centre + yawCos * x + yawSin * z, centre - yawSin * x + yawCos * z
			const std::array<double, 3> ahead = world3(bestHand, piece.x + piece.faceX, piece.y, piece.z + piece.faceZ);
			const double fx = ahead[0] - at[0];
			const double fz = ahead[2] - at[2];
			// A bed's use node is a few units above its floor (upstairs, on a tilt): the world height of the node's own layout y.
			const double nodeY = kind == routines::SiteKind::Bed ? world3(bestHand, piece.x, piece.nodeY, piece.z)[1] : 0.0;
			spec.pieces.push_back({kind, spec.yawCos * dx - spec.yawSin * dz, spec.yawSin * dx + spec.yawCos * dz, at[1],
				spec.yawCos * fx - spec.yawSin * fz, spec.yawSin * fx + spec.yawCos * fz, piece.clip, piece.record, nodeY});
			if (kind == routines::SiteKind::Table)
				tables.push_back({at[0], at[2]});
		}
	}
	// Each chair belongs to the nearest table within kSeatReach (tables stand about 12 u apart and a chair 8-13 u from the one it serves).
	std::vector<std::vector<std::tuple<double, std::array<double, 2>, ResidentData::SeatPose>>> near(tables.size());
	std::vector<std::tuple<double, std::array<double, 2>, ResidentData::SeatPose>> loose; // the chairs that serve no table (bar stools)
	for (const Chair& chair : chairs)
	{
		const std::array<double, 3> at = world3(bestHand, chair.x, chair.y, chair.z);
		if (!inside(at))
			continue;
		std::size_t nearest = tables.size();
		double best = routines::kSeatReach;
		for (std::size_t slot = 0; slot < tables.size(); ++slot)
		{
			const double d = std::hypot(at[0] - tables[slot][0], at[2] - tables[slot][1]);
			if (d <= best)
			{
				best = d;
				nearest = slot;
			}
		}
		const std::array<double, 3> ahead = world3(bestHand, chair.x + chair.face[0], chair.y, chair.z + chair.face[2]);
		const double fx = ahead[0] - at[0];
		const double fz = ahead[2] - at[2];
		if (nearest == tables.size())
		{
			// A chair that serves no table: a seat of the building for a STAY_IN_HOME resident (k16.6.30), in the spec's frame like the pieces above.
			const double dx = at[0] - building->cx;
			const double dz = at[2] - building->cz;
			spec.pieces.push_back({routines::SiteKind::Seat, spec.yawCos * dx - spec.yawSin * dz, spec.yawSin * dx + spec.yawCos * dz, at[1],
				spec.yawCos * fx - spec.yawSin * fz, spec.yawSin * fx + spec.yawCos * fz, chair.clip, "", 0.0});
			const double facing = std::hypot(fx, fz);
			loose.emplace_back(0.0, std::array<double, 2>{at[0], at[2]}, ResidentData::SeatPose{at[1], facing > 0.0 ? fx / facing : 0.0,
				facing > 0.0 ? fz / facing : 1.0, false, chair.clip});
			continue;
		}
		const double length = std::hypot(fx, fz);
		near[nearest].emplace_back(best, std::array<double, 2>{at[0], at[2]}, ResidentData::SeatPose{at[1], length > 0.0 ? fx / length : 0.0,
			length > 0.0 ? fz / length : 1.0, (chair.flags & 1) != 0, chair.clip});
	}
	spec.hasHeight = true;
	spec.originY = 0.0; // the pieces carry world heights (the placement's tilt applied), not layout heights over the placement's y
	data.sites.add(spec);
	for (std::size_t slot = 0; slot < tables.size(); ++slot)
	{
		std::stable_sort(near[slot].begin(), near[slot].end(), [](const auto& a, const auto& b)
		{
			return std::tie(std::get<0>(a), std::get<1>(a)) < std::tie(std::get<0>(b), std::get<1>(b));
		});
		std::vector<std::array<double, 2>> seats;
		std::vector<ResidentData::SeatPose> poses;
		for (std::size_t i = 0; i < near[slot].size() && i < 8; ++i)
		{
			seats.push_back(std::get<1>(near[slot][i]));
			poses.push_back(std::get<2>(near[slot][i]));
		}
		data.seatsOfTable[tableKey(key, static_cast<std::uint16_t>(slot))] = std::move(seats);
		data.seatPoses[tableKey(key, static_cast<std::uint16_t>(slot))] = std::move(poses);
	}
	// The loose chairs form one more seat group of the building (the bar's stools: vanilla seats a fourth patron squad on them, 5.39), listed
	// after the real tables so a squad takes one only when every table is held. Its position is the chairs' centre.
	if (!loose.empty())
	{
		std::stable_sort(loose.begin(), loose.end(), [](const auto& a, const auto& b) { return std::get<1>(a) < std::get<1>(b); });
		ResidentData::TableInfo group{key, routines::kLooseSlot, 0.0, 0.0, true, 0.0};
		std::vector<std::array<double, 2>> seats;
		std::vector<ResidentData::SeatPose> poses;
		for (std::size_t i = 0; i < loose.size() && i < 8; ++i)
		{
			seats.push_back(std::get<1>(loose[i]));
			poses.push_back(std::get<2>(loose[i]));
			group.x += seats.back()[0];
			group.z += seats.back()[1];
			group.y += poses.back().y;
		}
		const double n = static_cast<double>(seats.size());
		group.x /= n;
		group.z /= n;
		group.y /= n;
		const std::uint64_t groupKey = tableKey(key, routines::kLooseSlot);
		data.looseOfBuilding[key] = group;
		data.tableByKey[groupKey] = group;
		data.seatsOfTable[groupKey] = std::move(seats);
		data.seatPoses[groupKey] = std::move(poses);
	}
}

void knowLayouts(ResidentData& data, const World& world)
{
	if (data.layoutsTried)
		return;
	data.layoutsTried = true;
	try
	{
		data.layouts = std::make_shared<const routines::LayoutLibrary>(routines::LayoutLibrary::load(world.dataDir()));
	} catch (const std::exception&) {
		// No interiors.level: every site is synthetic.
	}
}

// The nearest placed building (not in `taken`) whose own squad template is `templateSid`, within `reach` of (x, z).
const BuildingSite* placementOf(const World& world, const std::string& templateSid, double x, double z, double reach = routines::kPlacementReach)
{
	const BuildingSite* best = nullptr;
	double bestDistance = reach;
	for (const BuildingSite& site : world.buildingSites())
	{
		if (site.squadTemplate != templateSid)
			continue;
		const double d = std::hypot(site.cx - x, site.cz - z);
		if (d <= bestDistance)
		{
			best = &site;
			bestDistance = d;
		}
	}
	return best;
}

// The building a template takes in a town when no placement names it (the original's selectBuildingForEntry, GRADES_GATESQUADS_KO_STATIC_b): the
// best-scoring free building, x10 when its BUILDING record is in the template's `building` list; ours adds the layout compatibility test (`layout
// interior` must exist for the record) and the distance: within kBuildingReach of the town point, ties to the nearest. UNSURE: only a fallback now,
// for the templates with no placement that carries them.
const BuildingSite* pickBuilding(const ResidentData& data, const World& world, const std::set<std::uint64_t>& taken, const kdata::Record& record,
	double x, double z)
{
	std::set<std::string> listed;
	if (const kdata::RefCategory* refs = record.findRefs("building"))
		for (const kdata::RefEntry& entry : refs->entries)
			listed.insert(entry.targetID);
	const std::string layoutName = record.stringOr("layout interior", "");
	const BuildingSite* best = nullptr;
	int bestScore = -1;
	double bestDistance = 0.0;
	for (const BuildingSite& site : world.buildingSites())
	{
		const double d = std::hypot(site.cx - x, site.cz - z);
		if (d > routines::kBuildingReach || taken.count(site.key) != 0 || !site.squadTemplate.empty())
			continue; // a building that a placement gave to another template is not free
		const int score = (listed.count(site.buildingSid) != 0 ? 2 : 0)
			+ (!layoutName.empty() && data.layouts != nullptr && data.layouts->find(site.buildingSid + "-" + layoutName) != nullptr ? 1 : 0);
		if (best == nullptr || score > bestScore || (score == bestScore && d < bestDistance))
		{
			best = &site;
			bestScore = score;
			bestDistance = d;
		}
	}
	return best;
}

// The town's buildings as the bar finder sees them. A building that the zone data gave a squad template (the placement's GAMESTATE_BUILDING
// `squad template`, with its `interior layout name`: the Hub's Bar is the Storm House placement whose template is Bar blackmarket) is that
// template's building, within kPlacementReach of the town point. A designated template with no placement takes a free building near the town point
// (pickBuilding, UNSURE). A function of the data and the town point only (not of where squads stand now, which a save and a load would see
// differently). BAR = designation 3, SHOP = 1; public = the template's `public day`. 
ResidentData::TownBars& knowTownBars(ResidentData& data, const World& world, const kdata::GameData& gameData, std::uint64_t townId,
	const state::TownState& town)
{
	const auto known = data.townBars.find(townId);
	if (known != data.townBars.end() && known->second.activeSid == town.activeSid)
		return known->second;
	knowLayouts(data, world);
	knowBuildingSites(data, world);
	std::set<std::uint64_t> taken;
	std::set<std::uint64_t> all;
	std::set<std::uint64_t> barSet;
	std::set<std::uint64_t> shopSet;
	ResidentData::TownBars bars;
	bars.activeSid = town.activeSid;
	// A placement belongs to the town whose circle holds it (the exe's town radius, 2500 for a TOWN), not only within 800 u of the town point:
	// Heft's UC bar stands 1609 u from its point (k16.6.254; the live caravan went to it, kinspect rwh145).
	const double reach = std::max(routines::kPlacementReach, towncircle::of(gameData, town).radius);
	if (const kdata::Record* record = gameData.find(town.activeSid))
	{
		// The lists as the original loads them (spawn.cpp townList): the owner faction's first unless the town has `residents override`.
		std::vector<const kdata::Record*> sources;
		if (!record->boolOr("residents override", false))
		{
			const kdata::RefCategory* factions = record->findRefs("faction");
			if (factions != nullptr && !factions->entries.empty() && !factions->entries.front().opaqueEntry)
				if (const kdata::Record* faction = gameData.find(factions->entries.front().targetID))
					sources.push_back(faction);
		}
		sources.push_back(record);
		for (const kdata::Record* source : sources)
		for (const char* list : {"residents", "default resident", "bar squads"})
		{
			const kdata::RefCategory* refs = source->findRefs(list);
			if (refs == nullptr)
				continue;
			for (const kdata::RefEntry& entry : refs->entries)
			{
				const kdata::Record* squad = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
				if (squad == nullptr)
					continue;
				const BuildingSite* placed = placementOf(world, entry.targetID, town.x, town.z, reach);
				if (placed != nullptr)
				{
					bars.homes.emplace(entry.targetID, placed->key);
					taken.insert(placed->key);
				}
				const std::int32_t designation = squad->intOr("building designation", 0);
				if (designation == 0 || !squad->boolOr("public day", true))
					continue;
				const BuildingSite* site = placed != nullptr ? placed : pickBuilding(data, world, taken, *squad, town.x, town.z);
				if (site == nullptr)
					continue;
				taken.insert(site->key);
				all.insert(site->key);
				if (designation == 3)
				{
					barSet.insert(site->key);
					shopSet.insert(site->key);
				} else if (designation == 1) {
					shopSet.insert(site->key);
				}
			}
		}
	}
	bars.bars.assign(barSet.begin(), barSet.end());
	bars.publicBuildings.assign(all.begin(), all.end());
	bars.shops.assign(shopSet.begin(), shopSet.end());
	return data.townBars.insert_or_assign(townId, std::move(bars)).first->second;
}

// A placed building's sites, a function of the data alone (a save and a load index them the same, whatever asks first): the building's own
// layout (the placement's `interior layout name`, else its squad template's `layout interior`); synthetic sites when it has none (TOWN_LIFE_SLICE
// 5.15 / 5.17). UNSURE: a bar the finder picked without a placement keeps its synthetic sites (the original furnishes it from the bar template's
// layout; ours would depend on when the town was first looked at, and a raid changes the pick). False when the world has no such building.
bool indexBuilding(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key)
{
	if (data.sites.find(key) != nullptr)
		return true;
	knowLayouts(data, world);
	knowBuildingSites(data, world);
	const auto found = data.buildingSiteByKey.find(key);
	const BuildingSite* placed = found != data.buildingSiteByKey.end() ? &world.buildingSites()[found->second] : world.buildingSite(key);
	if (placed == nullptr)
		return false;
	const BuildingSite& site = *placed;
	if (site.furnitureCage)
	{
		addFurnitureCage(data, world, ecs, gameData, key);
		return data.sites.find(key) != nullptr;
	}
	std::string layoutName = site.interiorLayout;
	if (layoutName.empty() && !site.squadTemplate.empty())
		if (const kdata::Record* squad = gameData.find(site.squadTemplate))
			layoutName = squad->stringOr("layout interior", "");
	addLayoutFurniture(data, world, ecs, gameData, key, layoutName);
	if (data.sites.find(key) != nullptr)
		return true;
	routines::BuildingSpec spec;
	if (buildingSpec(data, world, ecs, gameData, key, spec) == nullptr)
		return false;
	data.sites.add(spec);
	return true;
}

struct Spot
{
	bool ok = false;
	double x = 0.0;
	double z = 0.0;
};

} // namespace

// The town scorer's building indexer for other systems (task BW: a world member's cage finders). False when the world has no such building.
bool ensureBuilding(ResidentData& data, const World& world, flecs::world& ecs, const kdata::GameData& gameData, std::uint64_t key)
{
	return indexBuilding(data, world, ecs, gameData, key);
}

// One scorer pass. Reads: the member's Routine / Fighter / MemberRole, its squad's package and position, the site index, the book.
// Writes: the Routine, the book, the Fighter's route (or its position in placement mode).
void Simulation::scoreMember(flecs::entity member, flecs::entity squad, bool placement)
{
	if (combat_ == nullptr || residentData_ == nullptr || gameData_ == nullptr)
		return;
	const state::Routine* current = member.get<state::Routine>();
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	const std::uint64_t id = member.get<state::Id>()->value;
	state::Fighter* fighter = combat_->find(id);
	if (current == nullptr || instance == nullptr || fighter == nullptr)
		return;
	state::Fighter& f = *fighter;
	state::Routine r = *current;
	ResidentData& data = *residentData_;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	++townStats_.passes;
	r.blocked.erase(std::remove_if(r.blocked.begin(), r.blocked.end(), [&](const state::RoutineBlocked& b) { return b.until <= now; }), r.blocked.end());
	// A worker handed over to the world-task pull executor (dt24_emptymachine_source: the planner's 92 at a full machine) is the
	// pipeline's until the pull ends it; it comes back with goal 0 and the next pass re-scores it.
	if (r.goal == routines::task::EmptyMachine)
		return;

	// The 12 h safety release (OURS; the original has none): a claim the member never reached is given up and the pair blocked.
	if (r.target.kind != 0 && ticksSince(now, r.since) >= static_cast<std::uint64_t>(routines::kReservationMaxHours * static_cast<double>(kTicksPerGameHour)))
	{
		double sx = 1.0e30;
		double sz = 1.0e30;
		if (const routines::SiteEntry* entry = data.sites.siteAt(fromState(r.target)))
		{
			sx = entry->x;
			sz = entry->z;
		} else if (r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Work)) {
			knowBuildingSites(data, world_);
			if (const auto producer = data.producerById.find(r.target.object); producer != data.producerById.end())
			{
				sx = world_.producerSites()[producer->second].x;
				sz = world_.producerSites()[producer->second].z;
			}
		} else if (r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Turret)) {
			// A turret is not in the SiteIndex (task CH turrets): its spot is the turret list's.
			knowTurrets(data, world_, *gameData_, turretManning_);
			for (const ResidentData::TurretInfo& turret : data.turretSites)
				if (turret.key == r.target.object)
				{
					sx = turret.x;
					sz = turret.z;
					break;
				}
		} else if (r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Disposal)) {
			// A disposal machine is a placement, not in the SiteIndex (137's pot): its spot is the machine list's.
			if (const DisposalSite* machine = world_.disposalSite(r.target.object); machine != nullptr)
			{
				sx = machine->x;
				sz = machine->z;
			}
		}
		if (std::hypot(f.x - sx, f.z - sz) > kReach)
		{
			addBlock(r, r.goal, r.target, now + routines::kBlockTicks);
			routineBook_.release(id);
			r.target = {};
			if (r.goal == routines::task::RunAroundTown)
			{
				r.searchBuilding = 0;
				r.searchPoint = 0;
				r.searchLookUntil = 0;
			}
			r.goal = 0;
			++townStats_.unreached;
		}
	}

	// ---- where is home ----
	const state::Position* squadAt = squad.get<state::Position>();
	const double homeX = squadAt != nullptr ? squadAt->x : f.x;
	const double homeZ = squadAt != nullptr ? squadAt->z : f.z;
	const std::uint64_t squadId = squad.get<state::Id>()->value;
	std::uint64_t homeKey = 0;
	std::uint64_t townKey = 0;
	std::uint64_t townId = 0; // the town's state::Id, 0 = none
	double townX = 0.0;
	double townZ = 0.0;
	flecs::entity townEntity;
	if (instance->homeTown.id != 0 && ecs.is_alive(instance->homeTown.id))
	{
		const flecs::entity town = ecs.entity(instance->homeTown.id);
		const state::TownState* townState = town.get<state::TownState>();
		const state::Id* townIdent = town.get<state::Id>();
		if (townState != nullptr && townIdent != nullptr)
		{
			townKey = ensureTown(data, townIdent->value, townState->x, townState->z);
			townId = townIdent->value;
			townX = townState->x;
			townZ = townState->z;
			townEntity = town;
		}
	}
	// A real building (doors, strangers) or the squad's yard. Not told by the key's bits: a building key is a plain hash.
	// A resident's home is the building the town populate gave its squad (spawn.cpp `populate`; *was*: bar / shop templates looked up their
	// zone placement here, everyone else got a yard at the town point).
	if (f.homeBuilding == 0 && !f.playerControlled && instance->homeBuilding != 0)
	{
		f.homeBuilding = instance->homeBuilding;
		touchHomes();
		mirrorFighter(id);
	}
	const bool realHome = f.homeBuilding != 0 && indexBuilding(data, world_, ecs, *gameData_, f.homeBuilding);
	if (realHome)
		homeKey = f.homeBuilding;
	else
		homeKey = ensureYard(data, homeX, homeZ);
	const routines::SiteEntry* homeEntry = nullptr;
	{
		const std::vector<const routines::SiteEntry*> homes = data.sites.sitesOf(homeKey, routines::SiteKind::HomePoint);
		homeEntry = homes.empty() ? nullptr : homes.front();
	}
	const double anchorX = homeEntry != nullptr ? homeEntry->x : homeX;
	const double anchorZ = homeEntry != nullptr ? homeEntry->z : homeZ;

	// ---- the dice: keyed by (seed, member, tick), so a pass is a function of the saved state ----
	RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(id)), static_cast<std::size_t>(now + 1)));
	const double utilityRoll = dice.uniform01();
	const double pickRoll = dice.uniform01();
	const double holdRoll = dice.uniform01();
	const double patrolAngle = dice.uniform01();
	const double patrolRadius = dice.uniform01();

	// A member carrying a chained captive (the 167 chain's lift, no cage to put it in) walks on with its squad (k16.6.3).
	const state::Fighter* heldSlave = f.carrying != 0 ? combat_->find(f.carrying) : nullptr;
	const bool escortsSlave = heldSlave != nullptr && heldSlave->slaveState == 1 && !heldSlave->dead;

	// ---- candidates ----
	// The squad's applied package is the platoon Blackboard's (G1: template `AI packages` by priority, the faction fallback, a pushed
	// contract - dt19 / AITaskSytem::selectGoal 140510ef0), the same board the world squads' selector reads. The old lookup took the
	// registry's lowest-priority pick (retired/scorer_registry_package.txt).
	const routines::CompiledPackage* package = appliedPackage(squad);
	const routines::Role listRole = liveRole(member);
	const double hour = std::fmod(trader::gameHours(now), 24.0);
	std::vector<routines::Candidate> candidates;
	if (package != nullptr)
		candidates = routines::candidatesAt(*package, listRole, hour);

	struct Eval
	{
		routines::Candidate candidate;
		routines::Site site; // None for PATROL_TOWN
		double x = 0.0;
		double z = 0.0;
		std::optional<double> y; // the site's floor (a layout piece); none: the terrain under (x, z)
		double score = 0.0;
		goalsel::Offer offer; // the scored offer (G2)
		bool doorStep = false; // the goal is a walk to the door side and an instant lock / open there; no site
		bool lockAlso = false; // placement: the winner is a secure bed whose door is locked as part of the plan
		bool face = false; // the member faces (faceX, faceZ) once it stands at its spot (a seat looks at its table; yaw 0 = +z toward +x)
		double faceX = 0.0;
		double faceZ = 0.0;
		bool spot = false; // a walk to (x, z) with no claim of the member's own (RELAX at the squad's table, SHOPPING at a counter): re-walked when it drifts, blocked when unreachable
		double factor = 1.0; // the finder's own resolverScore (SHOPPING 118: 1.0 counter, 0.4 stall)
		std::uint64_t shop = 0; // SHOPPING 118: the shop building the goal is at (Routine::shoppingShop)
		bool getOut = false; // 208 GET_OUT_OF_BED_IF_ITS_EMERGENCY: an instant act on the sleeper itself, no site and no walk
		bool cageStep = false; // T3e2: LOCK_ALL_THE_CAGES 195, an instant act from inside the home (no walk)
		std::uint64_t chainTarget = 0; // T3e2: the character CAPTURE_NEW_SLAVES 167 picked (Routine::chainTarget)
		std::vector<std::uint16_t> plan; // G4: the GOAP chain of a goal that has one (167, 112); Routine::plan
		std::uint64_t searchBuilding = 0; // task patrol: the building RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216 searches
		std::uint8_t searchPoint = 0; // ... and the point of its route it walks to
	};
	std::vector<Eval> evals;
	const state::RoutineSite none;

	// A free site (or my own) of one kind in one building, chosen from the keyed roll. `nearestTo` takes the free site nearest that
	// point instead (the guard-node finders, task CH group 3: the exe's node-database query answers nearest); `realOnly` drops the
	// synthetic sites (268's indoors-only node query: a real layout node stands inside, a synthetic one round the door, outside).
	// Marks the pair blocked when there is none.
	const auto findSite = [&](std::uint16_t goal, std::uint64_t key, routines::SiteKind kind, Eval& out,
		std::optional<std::array<double, 2>> nearestTo = std::nullopt, bool realOnly = false) -> bool
	{
		const std::optional<SiteChoice> pick = pickSite(r, id, goal, key, kind, pickRoll, now, anchorX, anchorZ, nearestTo, realOnly);
		if (!pick)
			return false;
		out.site = pick->site;
		out.x = pick->x;
		out.z = pick->z;
		out.y = pick->y;
		return true;
	};

	// ---- the 153 / 239 bed resolver (dt29: 1408cfe00; TOWN_LIFE_SLICE 5.4, TQ9) ----
	// worldres::bedLadder153 orders it: the home's beds, else the nearest allied designation-2 building, then designation 3, then any
	// allied building with a bed, in the member's home town and the town it stands in only (a player faction's member scores them
	// instead); only the building it names is tried - a full one ends the pick. "Allied" is the building owner's relation to the
	// member's faction; a building's town is our town circle at its centre (OURS: the exe holds town handles). A building with no layout
	// has synthetic sites only, so it is not a ladder home unless the index primed it. Inside a building the first bed no squadmate
	// holds wins (findSite: slot order, the claim test; D17 keeps the book hard).
	const auto bedFind = [&](std::uint16_t goal, Eval& out) -> bool
	{
		const int homeBeds = homeKey != 0 ? static_cast<int>(data.sites.sitesOf(homeKey, routines::SiteKind::Bed).size()) : 0;
		const bool playerFaction = f.faction != 0 && f.faction == sensorContext().playerFaction;
		if (!playerFaction && homeBeds > 0)
			return findSite(goal, homeKey, routines::SiteKind::Bed, out);
		if (gameData_ == nullptr)
			return homeBeds > 0 && findSite(goal, homeKey, routines::SiteKind::Bed, out);
		const int homeTown = townId != 0 ? combat_->environment().townOf(townX, townZ) : -1;
		const int hereTown = combat_->environment().townOf(f.x, f.z);
		if (homeTown < 0 && hereTown < 0)
			return false;
		const flecs::entity mine = f.faction != 0 ? ecs.entity(f.faction) : flecs::entity();
		if (!mine.is_valid())
			return false;
		primeSiteIndex();
		const state::FactionState* mineState = mine.get<state::FactionState>();
		const std::string mineSid = mineState != nullptr ? mineState->templateSid : std::string();
		// Whether an owner faction is the member's own, and whether it is allied to it.
		std::unordered_map<std::string, std::pair<bool, bool>> factions;
		const auto buildingFaction = [&](const std::string& sid) -> std::pair<bool, bool>
		{
			const auto known = factions.find(sid);
			if (known != factions.end())
				return known->second;
			std::pair<bool, bool> facts{false, false};
			if (!sid.empty())
			{
				facts.first = sid == mineSid;
				flecs::entity owner;
				ecs.each([&](flecs::entity entity, const state::FactionState& faction)
				{
					if (faction.templateSid == sid)
						owner = entity;
				});
				if (owner.is_valid())
					facts.second = facts.first || factionRelation(*gameData_, owner, mine) >= FactionBook::kAlly;
			}
			return factions.emplace(sid, facts).first->second;
		};
		std::vector<worldres::BedBuilding> candidates;
		for (const std::uint64_t key : data.sites.keys())
		{
			const routines::BuildingSites* building = data.sites.find(key);
			if (building == nullptr || building->buildingSid.empty())
				continue; // a yard or a town point: no building to be allied to
			const int at = combat_->environment().townOf(building->cx, building->cz);
			if (at < 0 || (at != homeTown && at != hereTown))
				continue;
			const std::pair<bool, bool> faction = buildingFaction(building->ownerFaction);
			if (!faction.second)
				continue;
			candidates.push_back({key, building->designation, faction.first,
				static_cast<std::int32_t>(data.sites.sitesOf(key, routines::SiteKind::Bed).size()), building->cx, building->cz});
		}
		for (const std::uint64_t key : worldres::bedLadder153(homeKey, homeBeds, f.npcClass, playerFaction, candidates, f.x, f.z))
			if (findSite(goal, key, routines::SiteKind::Bed, out))
				return true;
		return false;
	};

	// The home point of this member: the building's (or yard's) HomePoint, spread over a small disc by a per-member roll.
	Spot homeSpot;
	{
		RngStream jitter(streamSeed(streamSeed(seed_, kRoutineStream + 1), static_cast<std::size_t>(id)));
		const double angle = 2.0 * std::numbers::pi * jitter.uniform01();
		const double radius = kHomeJitter * std::sqrt(jitter.uniform01());
		homeSpot = {true, anchorX + radius * std::cos(angle), anchorZ + radius * std::sin(angle)};
	}

	// ---- the guard-node finders (task CH group 3, dt24_38) ----
	// 38's intruder hunt runs only in a guard post: the home BUILDING's function id is one of {1, 3, 8} and the building is open to
	// the public (the exe's building+0xc4 and +0x290; UNSURE: the data names 1 the production function and 3 research, and the
	// resolver's character vf+0x58 test is not modelled), and the member is no player character.
	const auto guardPostHome = [&]() -> bool
	{
		if (f.homeBuilding == 0 || f.playerControlled || f.faction == sensorContext().playerFaction)
			return false;
		const auto site = data.buildingSiteByKey.find(f.homeBuilding);
		if (site == data.buildingSiteByKey.end() || !world_.openToPublic(f.homeBuilding))
			return false;
		const kdata::Record* record = gameData_->find(world_.buildingSites()[site->second].buildingSid);
		if (record == nullptr)
			return false;
		for (const std::int32_t function : routines::buildingFunctions(*gameData_, *record))
			if (function == 1 || function == 3 || function == 8)
				return true;
		return false;
	};
	// The intruder pick's dice: a hash of (seed, member, tick) under its own salt, so the pass's own dice sequence - and every other
	// stream - stays as it was (the exe picks one of the list at random).
	const auto guardPickRoll = [&]() -> double
	{
		return unitOf(splitMix64(splitMix64(splitMix64(seed_ ^ 0x3857a7e00000ULL) ^ id) ^ now));
	};

	// ---- the turret finders (task CH turrets, dt24_turret_finder) ----
	// A turret is a placed BUILDING whose functionality function is 12 (routines::isTurretBuilding; knowTurrets): the exe's turret
	// database is its building registry, filtered the same way. "My town" is our town circle at the member's home town's centre (OURS:
	// the exe holds the town and the building's town handles; 1400f6c20). A turret another member already mans is a claim, and a
	// turret the member failed to reach is a blocked pair, which is our reading of the resolver's "not already in use" and
	// "blocked by someone else" tests.
	// hasOneWorkingArm, the precondition of 149 and 234 (not 222): an arm that is not severed; a race with no arm part at all can man a
	// turret (the goap_sim rule for the slaver's lift).
	const auto workingArm = [&]() -> bool
	{
		bool anyArm = false;
		bool wholeArm = false;
		for (const state::FighterPart& part : f.parts)
		{
			if (part.type != 2)
				continue;
			anyArm = true;
			wholeArm = wholeArm || !part.severed();
		}
		return !anyArm || wholeArm;
	};
	struct TurretPick
	{
		bool found = false; // the finder's own result
		routines::Site site;
		double x = 0.0;
		double z = 0.0;
		double y = 0.0;
		double d2 = 0.0; // the squared distance of the finder's result
		bool kept = false; // the member's current hand is still a turret of its town
		routines::Site keptSite;
		double keptX = 0.0;
		double keptZ = 0.0;
		double keptY = 0.0;
	};
	const auto turretPick = [&](std::uint16_t goal) -> TurretPick
	{
		TurretPick pick;
		if (townId == 0)
			return pick;
		const int myTown = combat_->environment().townOf(townX, townZ);
		if (myTown < 0)
			return pick;
		knowTurrets(data, world_, *gameData_, turretManning_);
		const routines::Site mine = fromState(r.target);
		for (ResidentData::TurretInfo& turret : data.turretSites)
		{
			if (turret.town == -2)
				turret.town = combat_->environment().townOf(turret.x, turret.z); // fixed by the world: asked once, 695 turrets x every guard's pass is too many
			if (turret.town != myTown)
				continue;
			const routines::Site site{routines::SiteKind::Turret, turret.key, 0};
			if (isBlocked(r, goal, toState(site), now))
				continue;
			const routines::Claim* held = routineBook_.holder(site);
			if (held != nullptr && held->owner != id)
				continue;
			if (mine.kind == routines::SiteKind::Turret && mine.object == turret.key)
			{
				pick.kept = true;
				pick.keptSite = site;
				pick.keptX = turret.x;
				pick.keptZ = turret.z;
				pick.keptY = turret.y;
			}
			const double d2 = (f.x - turret.x) * (f.x - turret.x) + (f.z - turret.z) * (f.z - turret.z);
			if (!pick.found || d2 < pick.d2)
			{
				pick.found = true;
				pick.site = site;
				pick.x = turret.x;
				pick.z = turret.z;
				pick.y = turret.y;
				pick.d2 = d2;
			}
		}
		return pick;
	};

	// ---- RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216 (task patrol) ----
	// OURS by the user's ruling (FEATURES "Town search"): the exe's resolver hands every member the FIRST building with an interior in
	// the town, so a party streams through the town in single file. Ours: each member takes its own building (the resolver,
	// world_resolvers.hpp), walks 2-4 points on the ground floor, looks around at each, then moves on. The dice are the member's
	// stable key (Fighter::diceSeed, task CC), and every draw is a pure function of (seed, key, building, index), so a load resumes
	// the same route and nothing but the index is saved.
	const std::uint64_t searchDiceKey = f.diceSeed != 0 ? f.diceSeed : id;
	// The buildings with an interior in the member's current town (the exe's getTown; the home town when it stands outside every
	// circle), once per pass.
	std::optional<std::vector<worldres::TownBuilding>> searchBuildingsCache;
	const auto searchBuildings = [&]() -> const std::vector<worldres::TownBuilding>&
	{
		if (searchBuildingsCache)
			return *searchBuildingsCache;
		std::vector<worldres::TownBuilding> out;
		std::vector<towncircle::Circle> circles;
		world_.ecs().each([&](const state::TownState& town) { circles.push_back(towncircle::of(*gameData_, town)); });
		int at = towncircle::at(circles, f.x, f.z);
		if (at < 0 && townEntity.is_valid() && townEntity.get<state::TownState>() != nullptr)
		{
			circles.clear();
			circles.push_back(towncircle::of(*gameData_, *townEntity.get<state::TownState>()));
			at = towncircle::at(circles, f.x, f.z);
		}
		if (at >= 0 && circles[static_cast<std::size_t>(at)].radius > 0.0)
		{
			const towncircle::Circle& circle = circles[static_cast<std::size_t>(at)];
			for (const BuildingSite& site : world_.buildingSites())
			{
				if (!site.hasInterior || site.ruined)
					continue;
				if (std::hypot(site.cx - circle.x, site.cz - circle.z) > circle.radius)
					continue;
				out.push_back({site.key});
			}
		}
		searchBuildingsCache = std::move(out);
		return *searchBuildingsCache;
	};
	// One building's route (world_resolvers.hpp searchRoute216; the mesh puts each sample on its ground floor). Empty for a key
	// the world does not place.
	const auto searchRoute = [&](std::uint64_t building) -> std::vector<worldres::SearchSpot>
	{
		routines::BuildingSpec spec;
		const BuildingSite* site = buildingSpec(data, world_, ecs, *gameData_, building, spec);
		if (site == nullptr)
			return {};
		worldres::SearchBuilding b;
		b.key = building;
		b.cx = site->cx;
		b.cz = site->cz;
		b.axisX = site->axisX;
		b.axisZ = site->axisZ;
		b.halfA = site->halfA;
		b.halfB = site->halfB;
		b.groundY = site->originY;
		b.doorX = spec.doorX;
		b.doorZ = spec.doorZ;
		if (!site->floors.empty())
		{
			b.upperFloors = true;
			b.firstFloor = site->floors.front();
		}
		return worldres::searchRoute216(b, seed_, searchDiceKey, [&](double x, double z, worldres::SearchSpot& out)
		{
			if (navigation_ == nullptr)
			{
				out = {x, z, false, 0.0};
				return true;
			}
			const std::optional<knav::Position> snapped = navigation_->walker().snap({x, b.groundY, z}, worldres::kSearchSnapReach);
			if (!snapped || !insideFootprint(*site, {snapped->x, snapped->z}) || buildingAt(snapped->x, snapped->y, snapped->z) == 0)
				return false; // the walking surface must stand inside the building, as the sim's own membership reads it
			out = {snapped->x, snapped->z, true, snapped->y};
			return true;
		});
	};
	// Point `index` of that route (none when the route or the index is out).
	const auto searchSpot = [&](std::uint64_t building, int index) -> std::optional<worldres::SearchSpot>
	{
		const std::vector<worldres::SearchSpot> route = searchRoute(building);
		if (index < 0 || static_cast<std::size_t>(index) >= route.size())
			return std::nullopt;
		return route[static_cast<std::size_t>(index)];
	};

	// ---- the home's doors, strangers and the squad's lock flag (T3c, 5.16) ----
	const HomeDoors hd = realHome ? homeDoorsOf(homeKey, f.x, f.z) : HomeDoors{};
	const std::vector<flecs::entity>& homeDoors = hd.doors;
	state::SquadRoutine routineSquad;
	const bool haveSquadRoutine = squad.has<state::SquadRoutine>();
	if (haveSquadRoutine)
		routineSquad = *squad.get<state::SquadRoutine>();
	const auto setLocked = [&](std::uint8_t locked)
	{
		if (routineSquad.homeLocked == locked)
			return;
		routineSquad.homeLocked = locked;
		if (haveSquadRoutine)
			squad.set<state::SquadRoutine>(routineSquad);
	};
	// Someone unlocked a home door (a returning resident does, as the owner): the squad is not locked in any more.
	if (routineSquad.homeLocked != 0 && hd.any && !hd.anyLocked)
		setLocked(0);
	f.houseLocked = routineSquad.homeLocked != 0;
	const bool publicHome = ai_ != nullptr && ai_->registry().squad(instance->templateSid).behaviour == Behaviour::ShopKeeper;
	// A stranger: a conscious player character inside the home building (UNSURE: 5.6 says every perceived, non-slave, non-ally character;
	// ours is the player's, the only one that matters to a lock, and the one the trespass slice already tracks).
	int strangerKnown = -1;
	const auto stranger = [&]() -> bool
	{
		if (strangerKnown < 0)
		{
			strangerKnown = realHome && strangerIn(homeKey) ? 1 : 0;
		}
		return strangerKnown == 1;
	};
	// Bed utility (5.3): 1 in the bed; 0 for 180 min after the last sleep (the wake cooldown); else 1. UNSURE: the 0.3 band of "under 20 min
	// since the last sleep" lies inside the cooldown and is never reached.
	const double gameHoursNow = trader::gameHours(now);
	const double bedUtility = f.asleep && bedTask(r.goal)
		? 1.0
		: gameHoursNow - r.lastSleep < kWakeCooldownHours ? 0.0 : 1.0;
	// A door effect (instant): lock every lockable home door, or open every closed one; the flag follows.
	const auto applyDoors = [&](bool lock)
	{
		applyHomeDoors(homeDoors, lock);
		setLocked(lock ? 1 : 0);
		f.houseLocked = lock;
	};

	// ---- 216's step: arrived -> look around -> the next point, then the next building (task patrol) ----
	if (!placement && r.goal == routines::task::RunAroundTown && r.searchBuilding != 0)
	{
		if (r.searchLookUntil != 0 && now >= r.searchLookUntil)
		{
			r.searchLookUntil = 0;
			++r.searchPoint;
		} else if (r.searchLookUntil == 0) {
			const std::optional<worldres::SearchSpot> spot = searchSpot(r.searchBuilding, r.searchPoint);
			if (spot && !f.walking() && std::hypot(f.x - spot->x, f.z - spot->z) <= kReach)
			{
				// Look around for a few seconds: a turn away from the approach, then the next point.
				RngStream look(streamSeed(streamSeed(streamSeed(seed_, kSearchStream), static_cast<std::size_t>(searchDiceKey)),
					static_cast<std::size_t>(r.searchBuilding ^ (0x9E3779B97F4A7C15ull * static_cast<std::uint64_t>(r.searchPoint + 1)))));
				r.searchLookUntil = now + worldres::kSearchLookTicks;
				f.yaw = 2.0 * std::numbers::pi * look.uniform01();
				mirrorFighter(id);
			}
		}
		if (r.searchLookUntil == 0 && r.searchPoint >= static_cast<std::uint8_t>(worldres::searchCount216(seed_, searchDiceKey, r.searchBuilding)))
		{
			// The route is done: remember the building (the squad's shared memory) and pick another next pass.
			r.searched.push_back({r.searchBuilding, now});
			if (r.searched.size() > worldres::kSearchMarks)
				r.searched.erase(r.searched.begin());
			r.searchBuilding = 0;
			r.searchPoint = 0;
		}
	}

	std::size_t notYet = 0;
	// The running goal (dt14 section 4): the same-goal bonus and hold for its own offer, the 1.25 that keeps it competitive against
	// the others scoring under 1.0 (goalsel::scoreOf - the exe's rule, now the residents' too).
	const goalsel::Current running{r.goal, routines::siteId(fromState(r.target)), now < r.holdUntil};
	for (const routines::Candidate& candidate : candidates)
	{
		if (candidate.fallback)
			continue;
		if (!implemented(candidate.task))
		{
			++notYet;
			continue;
		}
		if (isBlocked(r, candidate.task, none, now))
			continue;
		Eval e;
		e.candidate = candidate;
		bool found = true;
		bool skip = false;
		double bedScore = -1.0;
		double cageUtility = 1.0; // 112's goal-condition ladder (dt24_112 section 3); 1.0 for every other row
		switch (candidate.task)
		{
		case routines::task::GoHomeToBed:
		case routines::task::GoHomeToBedSecure:
		{
			// A stranger in a private home keeps the resident from bed (a public home, a shop or bar, ignores them); 239 never goes to bed
			// unlocked (the lock is planned as its first step, even outside the 155 window).
			const bool secure = candidate.task == routines::task::GoHomeToBedSecure;
			if (bedUtility <= 0.0 || (stranger() && (secure || !publicHome)))
			{
				skip = true;
				break;
			}
			bedScore = bedUtility;
			const bool lockFirst = secure && hd.canLock && routineSquad.homeLocked == 0;
			if (lockFirst && !placement)
			{
				e.doorStep = true;
				e.x = anchorX;
				e.z = anchorZ;
			} else {
				found = bedFind(candidate.task, e);
				e.lockAlso = lockFirst;
			}
			break;
		}
		case routines::task::GetOutOfBedEmergency:
		{
			// 208 (s63 AI::vf28 1405a0b50): 2.0 while an alarm or enemy count is non-zero (ours: I have a target, or someone targets me),
			// 1.0 while a hostile is in my home (ours: the player's character inside it, as a lock's stranger is - UNSURE), else the
			// vf+0x70 term, which is unread (0). The goal's test is isInBed = 0, so it exists only while I lie in a bed; the registry's
			// canGetUp is planned by planHomebase (the three rows share it).
			const bool inBed = f.asleep && bedTask(r.goal);
			double emergency = 0.0;
			if (inBed)
			{
				bool attacked = f.target != 0;
				for (const CombatArena::Member& m : combat_->members())
					attacked = attacked || (m.fighter.target == id && !m.fighter.dead);
				emergency = attacked ? 2.0 : (stranger() ? 1.0 : 0.0);
			}
			skip = emergency <= 0.0 || !planHomebase(id, candidate.task, f.x, f.z).found();
			bedScore = emergency;
			e.getOut = true;
			break;
		}
		case routines::task::LockAllMyDoors:
		case routines::task::LockDoorsStayOutside:
			// 155 / 197 (dt24_155): the door-list resolver, then the registry row's plan - no strangers in the house, else the plan
			// grows the 283 shoo in front and the lock waits (ours: the candidate is skipped, the shoo goal being its own row).
			skip = !resolveDoorRow(id, candidate.task, hd, routineSquad.homeLocked != 0, stranger());
			e.doorStep = true;
			e.x = anchorX;
			e.z = anchorZ;
			break;
		case routines::task::OpenUpShopDoors:
			skip = !resolveDoorRow(id, candidate.task, hd, routineSquad.homeLocked != 0, stranger());
			e.doorStep = true;
			e.x = anchorX;
			e.z = anchorZ;
			break;
		case routines::task::ShooStrangers:
		case routines::task::ShooStrangersPrivate:
		{
			// Task CH: 158 and 283 share the exe's resolver 1408ced70 (world_resolvers.hpp shooStrangers283, dt24_131 / dt24_283) -
			// a NEUTRAL, conscious character inside my home building, the goal keeping its current hand while it stays inside. The
			// private requirement is the data's (283's record name) and the exe's vf+0x290 (publicHome); the plan is
			// [161 SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING, the goal node], whose executor is the dialogue producer (dialogue_sim.cpp's
			// shoo, event 9).
			const std::uint64_t strangerId = f.homeBuilding != 0
				? worldres::shooStrangers283(f, sensorContext(), [&](std::uint64_t target) { return combat_->find(target); },
					r.goal == candidate.task ? r.chainTarget : 0)
				: 0;
			skip = strangerId == 0 || (candidate.task == routines::task::ShooStrangersPrivate && publicHome);
			if (!skip)
			{
				if (r.goal == candidate.task && ticksSince(now, r.since) >= kShooPatienceTicks)
				{
					addBlock(r, candidate.task, none, now + kShooRestTicks);
					skip = true;
				}
				if (!skip)
				{
					const goap::Plan chain = planShoo(id, candidate.task);
					skip = !chain.found();
					if (!skip)
					{
						for (const goap::Step& step : chain.steps)
							e.plan.push_back(step.task);
						e.chainTarget = strangerId;
					}
				}
			}
			e.x = homeSpot.x;
			e.z = homeSpot.z;
			break;
		}
		case routines::task::Shopkeeper:
			found = findSite(candidate.task, homeKey, routines::SiteKind::Counter, e) && planTownGoal(id, goap::kStayAtShopkeeperNode, false).found();
			break;
		case routines::task::SitOnThrone:
			// 255 (dt24_43 section 2c): 43's resolver with the throne function; a home building with no throne piece has no slot, so the
			// finder fails and the pair is blocked for 20 s (the exe's 0). Real pieces only: no synthetic throne.
			found = findSite(candidate.task, homeKey, routines::SiteKind::Throne, e, std::nullopt, true) && planTownGoal(id, candidate.task, false).found();
			break;
		case routines::task::StaticGuardBuilding:
		case routines::task::StaticGuardBuildingIndoors:
		{
			// Task CH group 3 (dt24_38 sections 3-4): the exe's resolver 14099edd0. A guard whose home is a guard post hunts first:
			// one of the sensed neutrals inside ANOTHER building (guardNodeIntruders38), at random, and the free guard node NEAREST THE
			// INTRUDER at the resolver's 1.0 (decoded: the node DB is asked for a node near that position); with nobody to hunt, a free
			// node of my building at 0.8, picked by the keyed uniform roll - the node DB's own selector for that query is not decoded
			// (UNSURE), and the nearest member pick moved a Bar guard into line of sight of a fight outside (ksim_bar_fight 3) without a
			// decode behind it. 268 is the same body with the node query's indoors flag: the real layout nodes only, a synthetic node
			// standing round the door, outside.
			std::uint64_t intruder = 0;
			if (guardPostHome())
			{
				const std::vector<std::uint64_t> hunt =
					worldres::guardNodeIntruders38(f, sensorContext(), [&](std::uint64_t target) { return combat_->find(target); });
				if (!hunt.empty())
					intruder = hunt[std::min(hunt.size() - 1, static_cast<std::size_t>(guardPickRoll() * static_cast<double>(hunt.size())))];
			}
			if (intruder != 0)
			{
				const state::Fighter* body = combat_->find(intruder);
				found = body != nullptr
					&& findSite(candidate.task, homeKey, routines::SiteKind::GuardNode, e, std::array<double, 2>{body->x, body->z});
				e.factor = 1.0;
				// An intruder with no free node gives no goal this pass (the exe's resolver returns 0 here, not the 0.8 fallback).
			} else {
				found = findSite(candidate.task, homeKey, routines::SiteKind::GuardNode, e, std::nullopt,
					candidate.task == routines::task::StaticGuardBuildingIndoors);
				e.factor = 0.8;
			}
			break;
		}
		case routines::task::StaticGuardTown:
			// Task CH group 3 (dt24_38 section 2): the current node is kept while it stays mine (findSite's claim test), else the
			// town's outside node nearest the member (the exe's node-database mode 2 query; the ring is synthetic, OURS/UNSURE).
			found = townKey != 0 && findSite(candidate.task, townKey, routines::SiteKind::TownNode, e, std::array<double, 2>{f.x, f.z});
			break;
		case routines::task::ManTurrets:
		{
			// Task CH turrets (dt24_turret_finder section 1, resolver 1408ccc70): no town, no goal. The turret I already stand at is kept
			// at 1.0 while it is still one of my town's and free; else the free turret of my town nearest the member, graded by its
			// distance and whether I am engaged (0.8 within 600 u / 0.05 beyond, 1.0 within 1500 u / 0.3 beyond; worldres::turretBand149).
			if (!workingArm())
				break;
			const TurretPick pick = turretPick(candidate.task);
			found = pick.kept || pick.found;
			if (found && pick.kept)
			{
				e.site = pick.keptSite;
				e.x = pick.keptX;
				e.z = pick.keptZ;
				e.y = pick.keptY;
				e.factor = 1.0;
			} else if (found) {
				e.site = pick.site;
				e.x = pick.x;
				e.z = pick.z;
				e.y = pick.y;
				e.factor = worldres::turretBand149(pick.d2, perception::inCombat(f));
			}
			break;
		}
		case routines::task::TurretOnBuilding:
		{
			// 222 MAN_A_TURRET_ON_BUILDING runs the player job's resolver (dt24_turret_finder section 1, 1408ccfa0, shared with 234):
			// the finder's turret scores 1.0 and overrides the hand; with the finder empty a turret still in hand scores 0.25
			// (DAT_141696c80); nothing else. OURS: the finder's "my neighbourhood" is my town (149's scope), and the kept hand is only
			// counted when it is a turret of that town (the exe keeps any turret hand, which our walk could not reach without a position).
			const TurretPick pick = turretPick(candidate.task);
			found = pick.found || pick.kept;
			if (pick.found)
			{
				e.site = pick.site;
				e.x = pick.x;
				e.z = pick.z;
				e.y = pick.y;
				e.factor = 1.0;
			} else if (pick.kept) {
				e.site = pick.keptSite;
				e.x = pick.keptX;
				e.z = pick.keptZ;
				e.y = pick.keptY;
				e.factor = 0.25;
			}
			break;
		}
		case routines::task::PretendLabour:
		case routines::task::PretendLabourPretend:
		case routines::task::PretendToOperate:
			found = findSite(candidate.task, homeKey, routines::SiteKind::Work, e);
			break;
		case routines::task::RelaxInTownPackage:
		{
			// 214's goal state is IS_WORKING (dt24_emptymachine_source section 3): where the member's home stands by a producer whose
			// chain plans (92 in front of 87 when the output is full, else 87), the package goal works it - the handover below hands
			// the pull to the world-task executor, or orders the Use that takes the operator slot. The plan is asked first, so a home
			// with no machine (the bar squads') goes on to the T3d relax at the squad's table, which is what the live TQ24 capture kept
			// the Hub's patrons on: their packages list 214 and no work goal, and 214 is the goal they sit under.
			// The registry's precondition is atATown (dt24_214 section 1): outside a town circle the row has no plan at all.
			if (!inTownCircle(f))
				break;
			Eval work;
			if (findSite(candidate.task, homeKey, routines::SiteKind::Work, work)
				&& !planWorkChain(id, candidate.task, work.site.object).steps.empty())
			{
				e.site = work.site;
				e.x = work.x;
				e.z = work.z;
				e.y = work.y;
				found = true;
				break;
			}
			// The squad's table (5.17): one per squad, shared by every member, claimed from the global book.
			const BarTable table = haveSquadRoutine && townId != 0 ? claimBarTable(squad, townEntity, routineSquad.barTable) : BarTable{};
			found = table.ok;
			if (table.ok)
			{
				if (routineSquad.barTable != table.key)
				{
					routineSquad.barTable = table.key;
					squad.set<state::SquadRoutine>(routineSquad);
				}
				// Each member its own seat (kenshi-k16.6.32): the member keeps the seat it holds, else takes the first free seat of its squad's table, then
				// of the other tables of the same building, nearest first (the book holds one member per seat). A member with no free seat gets no
				// spot: the goal has no plan for it and it goes on to its next goal rather than standing round the table.
				if (!table.seats.empty())
				{
					const std::uint64_t building = data.tableByKey.at(table.key).building;
					std::vector<std::pair<double, std::uint64_t>> tables;
					for (const auto& [key, known] : data.tableByKey)
					{
						if (known.building == building && data.seatsOfTable.count(key) != 0)
							tables.emplace_back(key == table.key ? -1.0 : std::hypot(known.x - table.x, known.z - table.z), key);
					}
					std::sort(tables.begin(), tables.end());
					const routines::Site held = routineBook_.held(id);
					found = false;
					for (int pass = 0; pass < 2 && !found; ++pass)
					{
						for (const auto& [distance, key] : tables)
						{
							const std::vector<std::array<double, 2>>& seats = data.seatsOfTable.at(key);
							for (std::size_t k = 0; k < seats.size() && !found; ++k)
							{
								const routines::Site seat{routines::SiteKind::BarSeat, key, static_cast<std::uint16_t>(k)};
								if (pass == 0 ? held == seat : routineBook_.free(seat))
								{
									e.site = seat;
									e.x = seats[k][0];
									e.z = seats[k][1];
									e.faceX = data.tableByKey.at(key).x;
									e.faceZ = data.tableByKey.at(key).z;
									found = true;
								}
							}
						}
					}
				} else {
					// Each member its own spot: the table's seat of its rank in the squad (members by Id); the ranks past the seats (a six-man bar
					// squad at a three-seat table stacked two to a chair) get spaced points round the table (UNSURE).
					std::vector<std::uint64_t> ids;
					ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](const state::Id& other) { ids.push_back(other.value); });
					std::sort(ids.begin(), ids.end());
					const std::size_t rank = static_cast<std::size_t>(std::find(ids.begin(), ids.end(), id) - ids.begin());
					if (rank < table.seats.size())
					{
						e.x = table.seats[rank][0];
						e.z = table.seats[rank][1];
					} else {
						const std::size_t standing = rank - table.seats.size();
						const auto plainRing = [&]()
						{
							const double angle = 2.0 * std::numbers::pi * static_cast<double>(standing)
								/ static_cast<double>(std::max<std::size_t>(ids.size() - std::min(ids.size(), table.seats.size()), 1));
							e.x = table.x + routines::kTableRing * std::cos(angle);
							e.z = table.z + routines::kTableRing * std::sin(angle);
						};
						// A ring spot must stand on the table's floor (never the ground outside, a storey down through a wall): each sample that
						// snaps to the mesh within kRingSlack of it and kRingFloor of its height is a spot. The samples go to the standing members
						// in angle order (distinct while there are enough); when none works, the nearest walkable point to the table itself.
						if (navigation_ != nullptr && table.hasY)
						{
							std::vector<knav::Position> spots;
							for (int i = 0; i < routines::kRingSamples; ++i)
							{
								const double angle = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(routines::kRingSamples);
								const double cx = table.x + routines::kTableRing * std::cos(angle);
								const double cz = table.z + routines::kTableRing * std::sin(angle);
								const std::optional<knav::Position> spot = navigation_->walker().snap({cx, table.y, cz}, routines::kRingReach);
								if (spot && std::hypot(spot->x - cx, spot->z - cz) <= routines::kRingSlack && std::fabs(spot->y - table.y) <= routines::kRingFloor)
									spots.push_back(*spot);
							}
							if (!spots.empty())
							{
								const knav::Position& spot = spots[standing % spots.size()];
								e.x = spot.x;
								e.y = spot.y;
								e.z = spot.z;
							} else if (const std::optional<knav::Position> spot = navigation_->walker().snap({table.x, table.y, table.z}, routines::kRingNearestReach)) {
								e.x = spot->x;
								e.y = spot->y;
								e.z = spot->z;
							} else {
								plainRing();
							}
						} else {
							plainRing();
						}
					}
				}
				e.spot = true;
				e.face = true;
				if (table.seats.empty())
				{
					e.faceX = table.x;
					e.faceZ = table.z;
				}
			}
			break;
		}
		case routines::task::Shopping:
		{
			// SHOPPING 118 (task CH group 3; kinspect dt24_118, dt24_shop, TQ24). The member's day list gates the goal: a template
			// with no `shopping` refs never shops, and a satisfied list plans nothing (the exe's ?local_2e60 goal state). The resolver
			// picks a shop of the town that stocks an item still wanted (uniform among them, seeded by the day), else the town's
			// counter fallback; the counter scores 1.0, a shop object without one kShoppingNoCounterScore (0.4). The plan is
			// [67 Running to the counter, 119 BUY_SHIT, 118]; stepShopping (shopping_sim.cpp) runs the transaction pulses once the
			// member stands there. The plain score (0.5) loses to RELAX (1.25 running), so it runs when RELAX cannot plan (TQ24).
			const state::CharacterInstance* character = member.get<state::CharacterInstance>();
			const auto prefab = character != nullptr ? world_.prefabs().find(character->templateSid) : world_.prefabs().end();
			const tmpl::Character* record = prefab != world_.prefabs().end() ? prefab->second.get<tmpl::Character>() : nullptr;
			if (record == nullptr || record->shopping.empty())
			{
				skip = true;
				break;
			}
			refreshShoppingList(r, *character, f.diceSeed != 0 ? f.diceSeed : id);
			const ResidentData::TownBars* bars = townId != 0 ? &knowTownBars(data, world_, *gameData_, townId, *townEntity.get<state::TownState>()) : nullptr;
			if (!shopping::unmet(r.shopping) || bars == nullptr || bars->shops.empty())
			{
				skip = true;
				break;
			}
			std::vector<worldres::Shop> shops;
			shops.reserve(bars->shops.size());
			for (const std::uint64_t key : bars->shops)
			{
				worldres::Shop shop;
				shop.building = key;
				shop.counter = indexBuilding(data, world_, ecs, *gameData_, key)
					&& !data.sites.sitesOf(key, routines::SiteKind::Counter).empty();
				const flecs::entity merchant = shopSquad(key);
				const state::Inventory* store = merchant.is_valid() ? merchant.get<state::Inventory>() : nullptr;
				if (store != nullptr && combatData_ != nullptr)
				{
					for (const state::InventoryEntry& entry : store->items)
					{
						const ItemInfo* info = combatData_->item(entry.itemSid);
						if (info == nullptr)
							continue;
						const std::uint16_t function = static_cast<std::uint16_t>(std::max(0, info->function));
						const bool wanted = std::any_of(r.shopping.begin(), r.shopping.end(),
							[&](const state::ShoppingWant& want) { return want.function == function && want.taken < want.want; });
						shop.stocksWanted = shop.stocksWanted || wanted;
					}
				}
				shops.push_back(shop);
			}
			// The shop pick: a pure function of (seed, the member's dice key, the game day); the hour is only the daily reset.
			const double day = std::floor(trader::gameHours(now) / 24.0);
			RngStream pick(streamSeed(streamSeed(streamSeed(seed_, kShoppingStream), static_cast<std::size_t>(f.diceSeed != 0 ? f.diceSeed : id)),
				static_cast<std::size_t>(static_cast<std::uint64_t>(day) ^ 0x51ED270B1ull)));
			double factor = 1.0;
			const std::size_t chosen = worldres::shopping118(shops, pick.uniform01(), &factor);
			if (chosen >= shops.size())
			{
				skip = true;
				break;
			}
			const std::uint64_t shop = shops[chosen].building;
			const std::vector<const routines::SiteEntry*> counters = data.sites.sitesOf(shop, routines::SiteKind::Counter);
			found = !counters.empty(); // a shop object with no counter is the 0.4 stall; ours never walks to one
			if (found)
			{
				e.x = counters.front()->x;
				e.z = counters.front()->z;
				e.spot = true;
				e.factor = factor;
				e.shop = shop;
				const goap::Plan chain = planShopping(id, false, e.x, e.z);
				found = chain.found();
				for (const goap::Step& step : chain.steps)
					e.plan.push_back(step.task);
			}
			break;
		}
		case routines::task::PickupIntruders:
		{
			// An adopted goal (5.5): the executor is stepIntruders (carry.cpp), which plans 223 -> 68 and lifts the body, then runs 224
			// out through the home's door; the scorer only decides when. The precondition is the executor's own, the exe's resolver
			// 14099c110 (world_resolvers.hpp pickupIntruders223, task CH): an incapacitated body in MY HOME building, not carried, not
			// in a bed/cage, non-ally. 223 is in the leader's list (5.8; a package whose Squad lists fall back to the leader gives it to
			// every member, and the executor's `taken` set then picks one). The goal walks the member to the body and holds it there while
			// the executor works (the scorer yields to CarryStep / TakeOut / InteractTask).
			const std::uint64_t victim = f.homeBuilding != 0
				? worldres::pickupIntruders223(f, sensorContext(), [&](std::uint64_t target) { return combat_->find(target); })
				: 0;
			skip = victim == 0;
			if (!skip)
			{
				const state::Fighter* body = combat_->find(victim);
				e.x = body->x;
				e.z = body->z;
				e.spot = true;
			}
			break;
		}
		case routines::task::TakeIntruderOutside:
			// TAKE_INTRUDER_OUTSIDE is the take-out's own second goal, planned by the executor when the body is in the arms (task CH:
			// 224's tick is carry.cpp's moveTo): the scorer never plans it separately.
			skip = true;
			break;
		case routines::task::PickupIntrudersTown:
		{
			// The town scope of 223 (task CH group 3; dt24_273 section 2). The adopted goal is the executor's (stepIntruders plans
			// 273 -> 68 and then 274); the scorer only decides when. The precondition is the exe's 14099c250: my current town must be
			// mine ("the town is ours", 1406211e0 - OURS: TownState::owner == my faction), and then an incapacitated non-ally anywhere
			// inside that town's gates (world_resolvers.hpp pickupIntrudersTown273).
			const std::uint64_t victim = townIntruder273(f, sensorContext()).value_or(0);
			skip = victim == 0;
			if (!skip)
			{
				const state::Fighter* body = combat_->find(victim);
				e.x = body->x;
				e.z = body->z;
				e.spot = true;
			}
			break;
		}
		case routines::task::InvestigateAlarms:
		case routines::task::InvestigateAlarmsAlliesOnly:
		{
			// Task_InvestigateAlarms (dt41_E): the nearest alarm of my town (14092a810). The walk point is 30 u off the alarm on my side
			// of it (140354260); stepCrimeGoals runs the wait and deletes the alarm.
			const alarm::Alarm* hit = pickAlarm(id, candidate.task == routines::task::InvestigateAlarmsAlliesOnly);
			skip = hit == nullptr;
			if (skip)
				break;
			const double dx = f.x - hit->x;
			const double dz = f.z - hit->z;
			const double d = std::hypot(dx, dz);
			e.x = d > alarm::kStandOff ? hit->x + dx / d * alarm::kStandOff : f.x;
			e.z = d > alarm::kStandOff ? hit->z + dz / d * alarm::kStandOff : f.z;
			e.spot = true;
			e.chainTarget = hit->id;
			break;
		}
		case routines::task::LootStolenGoods:
		case routines::task::PoliceFreePrisoners:
		case routines::task::HuntBounties:
		{
			// 212 / 211 / 204: the finder names the character (the looted / the served prisoner / the bounty); the walk is to it.
			const std::uint64_t who = candidate.task == routines::task::LootStolenGoods ? lootedPrisoner212(id)
				: candidate.task == routines::task::PoliceFreePrisoners ? prisonerToFree211(id) : bountyTarget204(id);
			const state::Fighter* body = who != 0 ? combat_->find(who) : nullptr;
			skip = body == nullptr;
			if (skip)
				break;
			e.x = body->x;
			e.z = body->z;
			e.spot = true;
			e.chainTarget = who;
			break;
		}
		case routines::task::TakeIntruderOutsideTown:
			// 274 is the town take-out's own second goal, planned by the executor once the body is in the arms (274's tick is the
			// moveTo to the gate point): the scorer never plans it separately.
			skip = true;
			break;
		case routines::task::CaptureSlaves:
		{
			// The slave chain (5.8, TQ12 answered): the candidate is capture::nearest (capture_target.hpp, shared with the world squads),
			// reading the member's sensor list (task BU), with no radius beyond its own sight (ours: kCatchSight, the intruder sight).
			// The scorer only picks and walks: stepSlaveTrade (carry.cpp) restrains it and chains it. OURS/UNSURE (5.19): a free cage of
			// the home is required, else the catch has nowhere to go and the carrier would hold it for ever.
			if (r.chainTarget != 0)
			{
				// Mid-chain: stay on the body this member picked (only the executor ends or drops a chain). The cuff ends the capture
				// (167's goal state, s58); the caging goal 112 takes over, and its plan lifts a body that is not yet carried.
				const state::Fighter* body = combat_->find(r.chainTarget);
				skip = body == nullptr || body->dead || body->slaveState == 1 || f.carrying == r.chainTarget;
				if (!skip)
				{
					e.x = body->x;
					e.z = body->z;
					e.spot = true;
				}
				break;
			}
			bool freeCage = false;
			for (const routines::SiteEntry* cage : data.sites.sitesOf(homeKey, routines::SiteKind::Cage))
			{
				const routines::Claim* held = routineBook_.holder(cage->site);
				if ((held == nullptr || held->owner == id) && !cageTaken(cage->site)
					&& !isBlocked(r, candidate.task, toState(cage->site), now))
				{
					freeCage = true;
					break;
				}
			}
			std::uint64_t victim = 0;
			if (realHome && freeCage)
			{
				std::set<std::uint64_t> taken; // the bodies squad-mates already picked
				ecs.query_builder<const state::Routine>().with<state::MemberOf>(squad).build().each(
					[&](const state::Routine& mine) { taken.insert(mine.chainTarget); });
				victim = capture::nearest(id, f, sensorContext(), kCatchSight,
					[&](std::uint64_t body) { return combat_->find(body); },
					[&](std::uint64_t body) { return taken.count(body) != 0; });
			}
			skip = victim == 0;
			if (!skip)
			{
				// G4 (task CB, dt18): the goal's chain, planned against the live world. No plan scores 0 this pass (the next pass
				// re-plans it); the executor never runs a hand-written fallback.
				const goap::Plan chain = planSlaveChain(id, candidate.task, victim, nullptr);
				skip = !chain.found();
				if (!skip)
					for (const goap::Step& step : chain.steps)
						e.plan.push_back(step.task);
			}
			if (!skip)
			{
				const state::Fighter* body = combat_->find(victim);
				e.x = body->x;
				e.z = body->z;
				e.spot = true;
				e.chainTarget = victim;
			}
			break;
		}
		case routines::task::FindCageAndPutIn:
		{
			// The cage walk (5.8): the person this member's chain picked - in its arms, or chained on the ground, since the caging plan
			// lifts it (168 in front of itself) when it is not carried. The best free cage of the home is claimed and walked to, and the
			// executor puts the person in on arrival. A carrier of anything else (a 223 take-out) never plans it.
			// Only once the capture goal's own state holds - the person in the arms, or chained on the ground (the cuff done, s58);
			// mid-capture the 167 row keeps the member and 112 never pre-empts it.
			const state::Fighter* person = combat_->find(r.chainTarget);
			skip = r.chainTarget == 0 || (f.carrying != r.chainTarget && (person == nullptr || person->dead || person->slaveState != 1));
			// 112's utility (task CN, dt24_112 section 3): the goal condition's score for THIS payload, separate from the cage pick.
			// An ally (or the 0x248-vetoed animal) scores 0 and never plans, so a body already in a member's arms is put down by the
			// executor instead; the law/military 1.4 and the 2.0 raise it above the other rows. A resident's town is its home.
			if (!skip)
			{
				cageUtility = worldres::putInCage112(*person, homeKey != 0, f.npcClass == crime::kClassLaw || f.npcClass == crime::kClassMilitary,
					sensorContext().relation(person->faction, f.faction) >= 1.0, alliedTo(id, r.chainTarget));
				if (cageUtility <= 0.0)
					skip = true;
			}
			if (!skip)
			{
				found = findSite(candidate.task, homeKey, routines::SiteKind::Cage, e);
				// The cage's layout height is not the floor the walk can stand on: the two-storey Slave Shop's upstairs cages stand a
				// storey (66 u) above the only floor the navmesh knows, and the placement tilt does not close that (measured after T3e2:
				// the site at 1240.31, the member walking at 1174-1180). Walk to its x / z on the ground the member is on (OURS, 5.19),
				// and the executor puts the person at the carrier's own floor.
				e.y.reset();
				e.spot = found; // re-walked while it drifts, as the lift ends mid-route (the site itself is still claimed)
				// G4 (task CB, dt18): the caging chain against the live world. No plan (a dead body, no free cage) scores 0.
				if (found)
				{
					const goap::Plan chain = planSlaveChain(id, candidate.task, r.chainTarget, &e.site);
					skip = !chain.found();
					if (!skip)
					{
						for (const goap::Step& step : chain.steps)
							e.plan.push_back(step.task);
						// Not carrying: the plan's first step is the walk to the body, so that is where the scorer walks too.
						if (f.carrying == 0)
						{
							const state::Fighter* body = combat_->find(r.chainTarget);
							e.x = body->x;
							e.z = body->z;
						}
					}
				}
			}
			break;
		}
		case routines::task::SlaveWatching:
		{
			// Stand at a cage of the home that holds a slave (5.8). No claim: several wardens may watch one cage, and a spot is re-walked
			// when it drifts. OURS: the first occupied cage in slot order.
			const routines::SiteEntry* watched = nullptr;
			for (const routines::SiteEntry* cage : data.sites.sitesOf(homeKey, routines::SiteKind::Cage))
			{
				if (cageTaken(cage->site))
				{
					watched = cage;
					break;
				}
			}
			skip = !realHome || watched == nullptr;
			if (!skip)
			{
				e.x = watched->x;
				e.z = watched->z;
				e.spot = true;
			}
			break;
		}
		case routines::task::LockAllMyCages:
		{
			// The evening cage lock (5.8): only from inside the building, and only while a cage of the home is unlocked. The cage's own
			// lock (open, broken, level < 1) is not modelled: the cages the data places carry no level, and one key covers them all
			// (OURS, 5.19). `SquadRoutine::cagesLocked` follows like `homeLocked` does.
			// dt38 Q2: 195 takes the first cage whose own lock is unlocked / broken; a cage a prisoner picked, cut or forced open (the Lock
			// record's open flag, lock_tasks.cpp) is one, so the evening lock runs again for it and 77 re-locks it (below).
			const LockRecord* opened = lockFind(false, homeKey);
			skip = !realHome || f.building != homeKey || (routineSquad.cagesLocked != 0 && (opened == nullptr || !opened->open))
				|| data.sites.sitesOf(homeKey, routines::SiteKind::Cage).empty();
			e.cageStep = true;
			e.x = anchorX;
			e.z = anchorZ;
			break;
		}
		case routines::task::CarryWoundedSlaves:
		{
			// CARRY_WOUNDED_SLAVES 168 as a task of its own (task CB: the capture chain ends at the cuff, 167's goal state, so the
			// lift is no longer inside it; the package lists 168). My faction's chained slave, down and not carried - the world
			// squads' resolver (dt15 section 3), the same reading.
			skip = f.carrying != 0;
			if (!skip)
			{
				std::set<std::uint64_t> taken; // the bodies squad-mates already picked
				ecs.query_builder<const state::Id, const state::Routine>().with<state::MemberOf>(squad).build().each(
					[&](const state::Id& mate, const state::Routine& mine)
					{
						// Not my own: the cuffer idles holding the chained body in chainTarget, and it is the one that lifts it.
						if (mate.value != id)
							taken.insert(mine.chainTarget);
					});
				const std::uint64_t slave = worldres::carryWounded168(f, sensorContext(),
					[&](std::uint64_t body) { return combat_->find(body); },
					[&](std::uint64_t body) { return taken.count(body) != 0; });
				skip = slave == 0;
				if (!skip)
				{
					const goap::Plan chain = planSlaveChain(id, candidate.task, slave, nullptr);
					skip = !chain.found();
					if (!skip)
					{
						for (const goap::Step& step : chain.steps)
							e.plan.push_back(step.task);
						const state::Fighter* body = combat_->find(slave);
						e.x = body->x;
						e.z = body->z;
						e.spot = true;
						e.chainTarget = slave;
					}
				}
			}
			break;
		}
		case routines::task::StripSlaves:
			// STRIP_SLAVES is carried by no package on the merged data: never planned (OURS, 5.19).
			skip = true;
			break;
		case routines::task::DeadGuysInThePot:
		{
			// 137 "Corpse disposal or Dead guys go in the pot" (dt24_patrol_town_pot section 3): the finder picks the DISPOSAL
			// MACHINE (function 11: corpse furnace / cooking pit) and the plan fetches the body - 138 FIND_A_DEAD_GUY picks it (a dead
			// occupant of a function-8 cage of my town, else the nearest sensed corpse) and the scorer walks the member there;
			// stepCorpsePot lifts it, walks it to the machine and feeds it (136). No machine or no body: the goal cannot plan (0).
			// While the body is in the member's arms the walk is the machine's and the executor feeds it (the scorer only keeps the goal).
			const perception::Environment& env = combat_->environment();
			const int hereTown = env.townOf(f.x, f.z);
			const int homeTown = townId != 0 ? env.townOf(townX, townZ) : -1;
			const state::Allegiance* allegiance = member.get<state::Allegiance>();
			const std::string mySid = allegiance != nullptr ? allegiance->factionSid : std::string();
			// My home town's faction: the fallback for a machine the placement gives no owner (BuildingSite::ownerFaction is
			// "the town's" then, world.hpp).
			std::string homeSid;
			if (townEntity.is_valid())
				if (const state::TownState* town = townEntity.get<state::TownState>(); town != nullptr && town->owner.id != 0)
					if (const flecs::entity owner = ecs.entity(town->owner.id); owner.is_valid())
						if (const state::FactionState* faction = owner.get<state::FactionState>())
							homeSid = faction->templateSid;
			// 140599600: law, or the military of the town I stand in when it is my home town (the cage finder's reading, dt24_112).
			bool lawMilitary = f.npcClass == crime::kClassLaw;
			if (!lawMilitary && f.npcClass == crime::kClassMilitary && hereTown >= 0 && hereTown == homeTown)
				lawMilitary = true;
			std::vector<worldres::Pot> pots;
			if (gameData_ != nullptr && !mySid.empty())
				for (const DisposalSite& site : world_.disposalSites())
				{
					const int at = env.townOf(site.x, site.z);
					if (at < 0 || (at != hereTown && at != homeTown))
						continue; // the machines of my town (or the town I stand in)
					const std::string& owner = !site.ownerFaction.empty() ? site.ownerFaction : homeSid;
					// The exe scores my faction's machines and skips a foreign one (140621230's ownership list is not modelled, OURS).
					if (owner != mySid)
						continue;
					const routines::Site held{routines::SiteKind::Disposal, site.key, 0};
					const routines::Claim* claim = routineBook_.holder(held);
					if ((claim != nullptr && claim->owner != id) || isBlocked(r, candidate.task, toState(held), now))
						continue; // another member's machine, or this pair failed 20 s ago
					worldres::Pot pot;
					pot.building = site.key;
					pot.x = site.x;
					pot.z = site.z;
					pot.myFaction = true;
					pot.myHome = f.homeBuilding != 0 && f.homeBuilding == site.key;
					pot.publicBuilding = [&]
					{
						// The placement's squad template `public day` / `public night` (the exe's vf+0x290; a machine is not a
						// BuildingSite, so World::openToPublic cannot be asked).
						const kdata::Record* squad = site.squadTemplate.empty() ? nullptr : gameData_->find(site.squadTemplate);
						return squad == nullptr || squad->boolOr("public day", true) || squad->boolOr("public night", false);
					}();
					pots.push_back(pot);
				}
			std::uint64_t pot = 0;
			if (!worldres::findPot137(f, lawMilitary, pots, pot))
			{
				skip = true;
				break;
			}
			double potX = 0.0;
			double potZ = 0.0;
			for (const worldres::Pot& candidate : pots)
				if (candidate.building == pot)
				{
					potX = candidate.x;
					potZ = candidate.z;
					break;
				}
			std::uint64_t corpse = 0;
			if (f.carrying != 0)
			{
				// Mid-chain: the body is in the member's arms (the scorer is here because chainTarget == carrying), so the walk is the
				// machine's and the executor's feed is next.
				corpse = f.carrying;
			} else {
				// 138 FIND_A_DEAD_GUY: a dead occupant of a function-8 cage of my town first, then the nearest unclaimed sensed corpse.
				std::vector<worldres::DeadGuyCage> caged;
				for (const CombatArena::Member& other : combat_->members())
				{
					const state::Fighter& body = other.fighter;
					if (!body.dead || body.cageBuilding == 0)
						continue;
					const BuildingSite* cage = world_.buildingSite(body.cageBuilding);
					if (cage == nullptr || cage->buildingSid.empty())
						continue;
					const int at = env.townOf(body.x, body.z);
					if (at < 0 || (at != hereTown && at != homeTown))
						continue;
					const kdata::Record* record = gameData_ != nullptr ? gameData_->find(cage->buildingSid) : nullptr;
					if (record == nullptr)
						continue;
					const std::vector<std::int32_t> functions = routines::buildingFunctions(*gameData_, *record);
					if (std::find(functions.begin(), functions.end(), routines::kFunctionCage) != functions.end())
						caged.push_back({other.id, true}); // a world site is always built (a player cage is not a BuildingSite: UNSURE)
				}
				std::set<std::uint64_t> taken; // the bodies squad-mates already go for
				ecs.query_builder<const state::Routine>().with<state::MemberOf>(squad).build().each(
					[&](const state::Routine& other)
					{
						if (other.goal == routines::task::DeadGuysInThePot || other.goal == routines::task::FindCorpses)
							taken.insert(other.chainTarget);
					});
				corpse = worldres::findCorpse138(f, sensorContext(), caged,
					[&](std::uint64_t target) { return combat_->find(target); },
					[&](std::uint64_t target) { return taken.count(target) != 0; },
					[](const state::Fighter& body) { return !worldres::inedibleRace(body); });
			}
			skip = corpse == 0;
			if (skip)
				break;
			const goap::Plan chain = planCorpsePot(id, corpse, potX, potZ);
			skip = !chain.found();
			if (skip)
				break;
			for (const goap::Step& step : chain.steps)
				e.plan.push_back(step.task);
			e.chainTarget = corpse;
			e.site = {routines::SiteKind::Disposal, pot, 0};
			if (f.carrying == corpse)
			{
				e.x = potX;
				e.z = potZ;
			} else {
				const state::Fighter* body = combat_->find(corpse);
				e.x = body->x;
				e.z = body->z;
			}
			e.spot = true;
			break;
		}
		case routines::task::PatrolTown:
		{
			// PATROL_TOWN 36 (dt24_patrol_town_pot sections 1-2): the goal walks at the platoon's town, captured for the package's
			// life, so atDestinationTown - my package's town is the town I stand in - gates it (a member with no home town, or
			// outside it, is not offered the goal); each leg is a uniform point in a square of half-side townRadius/4 around the
			// town centre (worldres::patrolSpot36; the move order's 500 radius floor), re-drawn on the first idle pass, no claim.
			towncircle::Circle circle;
			int homeTown = -1;
			int hereTown = -1;
			if (townId != 0 && townEntity.is_valid())
				if (const state::TownState* town = townEntity.get<state::TownState>())
				{
					circle = towncircle::of(*gameData_, *town);
					homeTown = combat_->environment().townOf(townX, townZ);
					hereTown = combat_->environment().townOf(f.x, f.z);
				}
			skip = homeTown < 0 || hereTown != homeTown;
			const worldres::PatrolSpot spot = worldres::patrolSpot36(f.x, f.z, homeTown >= 0, circle.x, circle.z, circle.radius,
				patrolAngle, patrolRadius);
			e.x = spot.x;
			e.z = spot.z;
			break;
		}
		case routines::task::Patrol:
		{
			// PATROL 22 (dt24_patrol_town_pot section 2): the move-options block is the platoon's (a Zone pointer, modes, the "patrol
			// approaches towns" flag), not a route; the zone-sector walk it runs is not modelled (UNSURE). OURS: a point within
			// kPatrolRadius of where the member stands, re-picked on arrival - it patrols around itself.
			const double off = routines::kPatrolMin + patrolRadius * (routines::kPatrolRadius - routines::kPatrolMin);
			e.x = f.x + off * std::cos(2.0 * std::numbers::pi * patrolAngle);
			e.z = f.z + off * std::sin(2.0 * std::numbers::pi * patrolAngle);
			e.spot = true;
			break;
		}
		case routines::task::RunAroundTown:
		{
			// RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216 (dt24_36 section 3). The goal state is I_AM_INSIDE_TARGET_BUILDING; the
			// resolver picks a building with an interior in the current town (OURS per character, the user's ruling), and the
			// member walks its search route (2-4 ground-floor points, a look-around at each) before it picks another.
			const bool running = r.goal == candidate.task && r.searchBuilding != 0;
			std::uint64_t building = running ? r.searchBuilding : 0;
			if (building == 0)
			{
				std::vector<state::RoutineSearch> marks; // the squad's memory: every member's marks
				ecs.query_builder<const state::Routine>().with<state::MemberOf>(squad).build().each(
					[&](const state::Routine& mine) { marks.insert(marks.end(), mine.searched.begin(), mine.searched.end()); });
				building = worldres::runAroundTown216(searchBuildings(), marks, seed_, searchDiceKey, now, worldres::kSearchMemoryTicks);
			}
			skip = building == 0; // the town has no building with an interior
			if (!skip)
			{
				const int point = running ? r.searchPoint : 0;
				const std::optional<worldres::SearchSpot> spot = searchSpot(building, point);
				found = spot.has_value();
				if (found)
				{
					e.x = spot->x;
					e.z = spot->z;
					if (spot->hasY)
						e.y = spot->y;
					e.spot = true;
					e.searchBuilding = building;
					e.searchPoint = static_cast<std::uint8_t>(point);
				}
			}
			break;
		}
		default: // StayInHome
			// 43 (dt24_43): the registry row's plan - sheathe, not crouched, at home - then the home slot.
			found = planTownGoal(id, candidate.task, false).found();
			e.site = {routines::SiteKind::HomePoint, homeKey, 0};
			e.x = homeSpot.x;
			e.z = homeSpot.z;
			// k16.6.30: a free chair of the home layout first (reserved like the bar's seats), else the member sits on the floor where it stands.
			if (found && !escortsSlave)
				findSite(candidate.task, homeKey, routines::SiteKind::Seat, e, std::nullopt, true);
			break;
		}
		if (skip)
			continue;
		if (!found)
		{
			// A failed finder blocks the pair for 20 s (the target is empty: nothing was found to name).
			addBlock(r, candidate.task, none, now + routines::kBlockTicks);
			++townStats_.blocked;
			continue;
		}
		if (e.site.kind == routines::SiteKind::HomePoint)
			e.site = {};
		// G2: one goalsel offer per candidate - the task's utility (bed rows: their own score) x the finder's factor x the cage ladder,
		// the row's weight, the running goal's 1.25 bonus and its hold floor / cap (goalsel::scoreOf; the data's window was applied by
		// candidatesAt).
		const goalsel::Offer offer = routines::makeOffer(candidate.task, candidate.tier, candidate.weight,
			(bedScore >= 0.0 ? bedScore : utilityOf(candidate.task, utilityRoll)) * e.factor * cageUtility, routines::siteId(e.site));
		e.score = goalsel::scoreOf(offer, running, hour);
		e.offer = offer;
		evals.push_back(std::move(e));
	}

	// Strict tiers, then the highest score; a tie goes to the later candidate (goalsel::select, the world squads' own pass).
	const Eval* best = nullptr;
	{
		std::vector<goalsel::Offer> offers;
		offers.reserve(evals.size());
		for (const Eval& e : evals)
			offers.push_back(e.offer);
		std::vector<goalsel::Verdict> verdicts;
		const bool traced = tracing(id);
		const goalsel::Choice choice = goalsel::select(offers, running, hour, traced ? &verdicts : nullptr);
		if (choice.found)
			for (const Eval& e : evals)
				if (e.offer.task == choice.task && e.offer.target == choice.target && e.score == choice.score)
					best = &e;
		if (traced)
		{
			trace::Pick pick = trace::makePick(now, id, trace::Source::TownScorer, running, offers, verdicts);
			pick.result = best != nullptr ? trace::Result::Chosen : trace::Result::Fallback;
			pick.task = best != nullptr ? choice.task : routines::task::StayInHome;
			pick.target = choice.target;
			pick.score = best != nullptr ? choice.score : routines::kFallbackUtility;
			pick.tier = choice.tier;
			decisionTrace_.record(pick);
		}
	}
	Eval fallback;
	if (best == nullptr)
	{
		// Nothing plannable: STAY_IN_HOME at the home point. It cannot fail.
		fallback.candidate.task = routines::task::StayInHome;
		fallback.candidate.fallback = true;
		fallback.x = homeSpot.x;
		fallback.z = homeSpot.z;
		fallback.score = routines::kFallbackUtility;
		best = &fallback;
		++townStats_.fallbacks;
	}
	(void)notYet;
	// k16.6.3 (OURS, AIC special case; SLAVERY 4): a carrier of a chained captive that has nothing else to do (no cage to put it in) stays
	// with its squad. The yard the home point sits on is fixed where the squad first stood, so 43 follows the squad's live position and
	// is re-walked while it drifts, as 112's cage walk is.
	Eval escort;
	if (escortsSlave && best->candidate.task == routines::task::StayInHome)
	{
		escort = *best;
		escort.site = {};
		escort.x = homeX;
		escort.z = homeZ;
		escort.spot = true;
		best = &escort;
	}

	// ---- adopt ----
	const std::uint16_t goal = best->candidate.task;
	const bool same = goal == r.goal && toState(best->site) == r.target;
	bool needsWalk = true;
	if (!same)
	{
		if (exclusive(best->site.kind))
		{
			if (!routineBook_.take(best->site, id, now))
			{
				addBlock(r, goal, toState(best->site), now + routines::kBlockTicks);
				++townStats_.blocked;
				r.nextScore = now + kLookTicks;
				member.set<state::Routine>(std::move(r));
				return;
			}
		} else {
			routineBook_.release(id);
		}
		r.goal = goal;
		r.since = now;
		// A peaceful goal whose row needs hasWeaponEquipped = 0 gets the planner's 7 in front (task ds/sheathe): the weapon goes into the
		// pack as the goal is taken, and a fight or the wake of 208 draws it again (stepRearm).
		if (!same)
			runWeaponPrefix(id, goal);
		const routines::Hold hold = routines::holdOf(goal);
		r.holdUntil = hold.base > 0.0 || hold.span > 0.0
			? now + static_cast<std::uint64_t>((hold.base + holdRoll * hold.span) * static_cast<double>(kTicksPerGameHour))
			: 0;
		r.target = toState(best->site);
		// The slave chain (T3e2): 167 names the body, 168 lifts it (task CB: a goal of its own now), 112 keeps it while it is carried
		// to its cage. Any other goal abandons whatever chain was running (the executor sees the empty chainTarget and puts the
		// person down if it held one).
		if (goal == routines::task::CaptureSlaves && best->chainTarget != 0)
		{
			r.chainTarget = best->chainTarget;
			r.chainElapsed = -1.0;
			r.plan = best->plan;
			r.step = 0;
		} else if (goal == routines::task::CarryWoundedSlaves && best->chainTarget != 0) {
			r.chainTarget = best->chainTarget;
			r.chainElapsed = -1.0;
			r.plan = best->plan;
			r.step = 0;
		} else if (goal == routines::task::FindCageAndPutIn) {
			r.plan = best->plan; // the caging chain (G4); the routine keeps the carried body it names
			r.step = 0;
		} else if ((goal == routines::task::InvestigateAlarms || goal == routines::task::InvestigateAlarmsAlliesOnly
			|| goal == routines::task::LootStolenGoods || goal == routines::task::PoliceFreePrisoners
			|| goal == routines::task::HuntBounties) && best->chainTarget != 0) {
			r.chainTarget = best->chainTarget; // the alarm / the prisoner / the target: stepCrimeGoals' hand
			r.chainElapsed = -1.0;
			r.plan.clear();
			r.step = 0;
		} else if (goal == routines::task::ShooStrangersPrivate && best->chainTarget != 0) {
			r.chainTarget = best->chainTarget; // the stranger (283's hand)
			r.plan = best->plan;
			r.step = 0;
		} else if (goal == routines::task::Shopping) {
			// 118's hand is the shop's counter (the plan [67, 119, 118]); the executor's pulses and its latch start afresh.
			r.chainTarget = 0;
			r.chainElapsed = -1.0;
			r.plan = best->plan;
			r.step = 0;
			r.shoppingShop = best->shop;
			r.shoppingX = best->x;
			r.shoppingZ = best->z;
			r.shoppingTimer = 0.0;
			r.shoppingLatch = false;
		} else if (goal == routines::task::DeadGuysInThePot && best->chainTarget != 0) {
			// The corpse pot: the body 138 picked (or the one already in the arms) and the chain (stepCorpsePot runs it).
			r.chainTarget = best->chainTarget;
			r.chainElapsed = -1.0;
			r.plan = best->plan;
			r.step = 0;
		} else if (f.carrying == 0) {
			// A catch already in hand keeps its chainTarget: stepSlaveTrade puts it down when the chain is abandoned or has waited
			// too long (no member holds a person for ever).
			r.chainTarget = 0;
			r.chainElapsed = -1.0;
			r.plan.clear();
			r.step = 0;
		}
		// A CORPSE in the arms belongs to the corpse pot 137: any other goal puts it down here (the slave chain's own keep is the
		// Alive chain's; a carried dead body has no other consumer, and nothing may leave a member holding one for ever).
		if (goal != routines::task::DeadGuysInThePot && f.carrying != 0 && r.chainTarget == f.carrying)
			if (const state::Fighter* carried = combat_->find(f.carrying); carried != nullptr && carried->dead)
			{
				dropCarried(id);
				r.chainTarget = 0;
				r.chainElapsed = -1.0;
				r.plan.clear();
				r.step = 0;
			}
		// 216's search (task patrol): a new goal takes the building the scorer picked; any other goal drops the search (the marks
		// of the buildings already searched stay, the squad's memory).
		r.searchBuilding = goal == routines::task::RunAroundTown ? best->searchBuilding : 0;
		r.searchPoint = goal == routines::task::RunAroundTown ? best->searchPoint : 0;
		r.searchLookUntil = 0;
		++townStats_.adopted;
		++townStats_.byGoal[goal];
		townStats_.shoppingTrips += goal == routines::task::Shopping ? 1 : 0;
	} else {
		++townStats_.held;
		// The book may have lost the claim (a load rebuilt it with a conflict): take it again, or re-score next time.
		if (exclusive(best->site.kind) && !routineBook_.take(best->site, id, r.since))
		{
			r.goal = 0;
			r.target = {};
			r.nextScore = now + kLookTicks;
			member.set<state::Routine>(std::move(r));
			return;
		}
		if (goal == routines::task::RunAroundTown)
		{
			// The route's running state (the building picked when the search moved on, the point reached).
			r.searchBuilding = best->searchBuilding;
			r.searchPoint = best->searchPoint;
		}
		// A patrol that has arrived picks a new point; a goal that stands at its spot stays.
		needsWalk = (goal == routines::task::Patrol || goal == routines::task::PatrolTown || best->spot) && !f.walking();
	}

	// ---- sleeping, doors (T3c) ----
	const bool bedGoal = bedTask(goal);
	if ((!bedGoal || best->doorStep) && f.asleep)
	{
		f.asleep = false;
		mirrorFighter(id);
	}
	if (goal == routines::task::ShooStrangers || goal == routines::task::ShooStrangersPrivate)
		townStats_.shoos += same ? 0 : 1;
	// A door goal acts when the member is within reach of a door (a placed member acts at once); the next look is soon, to move on.
	if (best->doorStep && (placement || (hd.any && hd.nearest <= kDoorReach)))
	{
		applyDoors(goal != routines::task::OpenUpShopDoors);
		if (!placement)
		{
			r.nextScore = now + kLookTicks;
			member.set<state::Routine>(std::move(r));
			return;
		}
	}
	if (best->lockAlso)
		applyDoors(true);
	// The warden's cage lock (T3e2): one key, every cage of the home locked at once; the squad flag follows like homeLocked. The goal acts
	// from inside the home (its precondition), so there is no walk to make; the next look is soon, to move on.
	// The emergency wake (208): the shared get-out action, then the next look is at once - the bed claim is gone, so the sleeper scores
	// its day again (the bed's own 180-minute cooldown keeps it from lying straight back down).
	if (best->getOut)
	{
		drawWeapon(id); // 208: the bed is under attack, so the plan's 6 before the fight
		getOutOfBed(member, id);
		return;
	}
	if (best->cageStep)
	{
		lockErase(false, f.building); // 77 LOCK_DOOR on the cage: the open flag of a picked / cut / forced lock is gone (dt38 Q2)
		if (routineSquad.cagesLocked == 0)
		{
			routineSquad.cagesLocked = 1;
			if (haveSquadRoutine)
				squad.set<state::SquadRoutine>(routineSquad);
			++townStats_.cagesLocked;
		}
		if (!placement)
		{
			r.nextScore = now + kLookTicks;
			member.set<state::Routine>(std::move(r));
			return;
		}
	}

	// ---- move ----
	const double toX = best->x;
	const double toZ = best->z;
	const bool atHome = goal == routines::task::StayInHome && best->site.kind != routines::SiteKind::Seat && std::hypot(f.x - (escortsSlave ? homeX : anchorX), f.z - (escortsSlave ? homeZ : anchorZ)) <= kHomeSettle;
	if (placement)
	{
		double x = toX;
		double y = best->y ? *best->y : groundHeight(x, toZ).value_or(f.y);
		double z = toZ;
		bool ok = true;
		if (navigation_ != nullptr)
		{
			const std::optional<knav::Position> spot = navigation_->walker().snap({x, y, z}, 100.0);
			if (spot)
			{
				x = spot->x;
				y = spot->y;
				z = spot->z;
			} else {
				ok = false; // off the mesh: it stays where promotion put it
			}
			// A real bed off the mesh (a slave cage) is blocked as the walk would block it, not slept in from the nearest polygon.
			if (best->site.kind == routines::SiteKind::Bed && best->y
				&& (!ok || std::hypot(x - toX, z - toZ) > kReach || std::fabs(y - *best->y) > kFloorReach))
			{
				++townStats_.blocked;
				addBlock(r, goal, r.target, now + routines::kBlockTicks);
				routineBook_.release(id);
				r.goal = 0;
				r.target = {};
				r.nextScore = now + kLookTicks;
				member.set<state::Routine>(std::move(r));
				return;
			}
		}
		if (ok && !atHome)
		{
			f.x = x;
			f.y = y;
			f.z = z;
			f.clearRoute();
			if (f.carriedBy == 0)
				setLocation(f, buildingAt(f.x, f.y, f.z), f.y);
			mirrorFighter(id);
		}
		if (bedGoal && best->site.kind == routines::SiteKind::Bed)
		{
			runWeaponPrefix(id, goal); // lying down needs hasWeaponEquipped = 0 (a placed sleeper arrives past the adoption's sheathe)
			f.asleep = true;
			r.lastSleep = gameHoursNow;
			mirrorFighter(id);
		}
		if (best->face && ok)
		{
			f.yaw = std::atan2(best->faceX - f.x, best->faceZ - f.z);
			mirrorFighter(id);
		}
		++townStats_.placed;
		// The stagger: members spread over the interval.
		r.nextScore = now + routines::kScoreIntervalTicks + id % routines::kScoreIntervalTicks;
		member.set<state::Routine>(std::move(r));
		return;
	}
	// In the bed: arrived (or the walk ended short of a bed nobody can reach, after kBedSettleTicks) and not walking. On a navmesh a real
	// (layout) bed is reached at its use node and floor or not at all: one the walk stalls short of is given up by the 12 h release.
	// lastSleep follows the sleeper, so the moment it wakes is the last write.
	if (bedGoal && best->site.kind == routines::SiteKind::Bed && !f.walking()
		&& (best->y && navigation_ != nullptr ? std::hypot(f.x - toX, f.z - toZ) <= kReach && std::fabs(f.y - *best->y) <= kFloorReach
			: std::hypot(f.x - toX, f.z - toZ) <= kBedArrive || ticksSince(now, r.since) >= kBedSettleTicks))
	{
		if (!f.asleep)
		{
			runWeaponPrefix(id, goal);
			f.asleep = true;
			++townStats_.slept;
			f.clearRoute();
			mirrorFighter(id);
		}
		r.lastSleep = gameHoursNow;
	}
	if (needsWalk && !atHome && !f.asleep && std::hypot(f.x - toX, f.z - toZ) > kReach && !f.walking())
	{
		bool started = false;
		bool here = false;
		std::vector<state::RoutePoint> points;
		bool approach = false;
		if (navigation_ != nullptr)
		{
			const double y = best->y ? *best->y : groundHeight(toX, toZ).value_or(f.y);
			const Route route = routeRetry(id, f, toX, toZ, y, true);
			if (route.found && !route.points.empty())
			{
				const state::GroundPoint& end = route.points.back();
				const bool shortOfBed = best->site.kind == routines::SiteKind::Bed && best->y && route.approach
					&& (std::hypot(end.x - toX, end.z - toZ) > kReach || std::fabs(route.heights.back() - *best->y) > kFloorReach);
				if (shortOfBed)
				{
					// a real bed it cannot reach: blocked below, not slept beside
				} else if (std::hypot(end.x - f.x, end.z - f.z) <= kReach) {
					here = true; // the nearest reachable point is where it stands: arrived
				} else {
					for (std::size_t p = 0; p < route.points.size(); ++p)
						points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
					approach = route.approach;
					started = true;
				}
			}
		} else {
			points = {{f.x, f.y, f.z}, {toX, f.y, toZ}};
			started = true;
		}
		if (started)
		{
			f.clearRoute();
			f.route = std::move(points);
			f.routeNext = 0;
			++f.routeVersion;
			f.routeSeq = 0;
			f.routeChase = false;
			f.routeApproach = approach;
			f.routeTries = 0;
			mirrorFighter(id);
		} else if (!here && (best->site.kind != routines::SiteKind::None || best->doorStep || best->spot) && !best->candidate.fallback)
		{
			// A table nobody can reach is given back, so the squad claims another next time.
			if (goal == routines::task::RelaxInTownPackage && haveSquadRoutine && routineSquad.barTable != 0)
			{
				routineBook_.release(squadId);
				routineSquad.barTable = 0;
				squad.set<state::SquadRoutine>(routineSquad);
			}
			// No way to the site: block the pair, drop the claim, and look again at once. (The fallback and the home point never block:
			// a member that cannot reach them simply stands where it is.)
			++townStats_.noRoute;
			++townStats_.blocked;
			addBlock(r, goal, r.target, now + routines::kBlockTicks);
			routineBook_.release(id);
			if (goal == routines::task::RunAroundTown)
			{
				// A building no route reaches is given up: the next look picks another (the pair stays blocked 20 s).
				r.searchBuilding = 0;
				r.searchPoint = 0;
				r.searchLookUntil = 0;
			}
			r.goal = 0;
			r.target = {};
			r.nextScore = now + kLookTicks;
			member.set<state::Routine>(std::move(r));
			return;
		}
	}
	// Standing at a table spot: face the table (a seat looks at the table it serves; a chair with no table would keep its yaw). A seat is
	// reached as near as the navmesh allows (kSeatTolerance).
	if (best->face && !f.walking() && !f.asleep && std::hypot(f.x - toX, f.z - toZ) <= std::max(kReach, routines::kSeatTolerance))
	{
		const double yaw = std::atan2(best->faceX - f.x, best->faceZ - f.z);
		if (std::fabs(yaw - f.yaw) > 0.01)
		{
			f.yaw = yaw;
			mirrorFighter(id);
		}
	}
	// ---- working the site's machine (dt24_emptymachine_source section 3) ----
	// The work phase stands a member at a producer. The plan decides what the machine needs: a full output plans [92, 87, the
	// goal], and the member is handed to the world-task pull executor (which takes it back with goal 0); a machine that can run
	// and is not working gets the Use order that makes the member an operator (87 OPERATE_MACHINERY - stepProducers keeps only a
	// holder of it). A site with no materialised machine stays the pretend stand (221), as does the automatic producer, which
	// takes no operator. The hold lasts until the machine fills or the pull ends; UNSURE: the exe re-plans on its own update.
	// 214 RELAX_IN_TOWN_PACKAGE carries the same goal state (IS_WORKING), so its routine works the site the same way; the relax at
	// the table is what its case picked instead when no chain planned.
	if (!placement && (goal == routines::task::PretendLabour || goal == routines::task::RelaxInTownPackage) && best->site.kind == routines::SiteKind::Work && !f.walking() && !f.asleep
		&& !f.dead && !f.unconscious && !f.down && std::hypot(f.x - toX, f.z - toZ) <= kReach)
	{
		const goap::Plan plan = planWorkChain(id, goal, best->site.object);
		bool operates = false;
		for (const goap::Step& step : plan.steps)
			operates = operates || step.task == routines::task::OperateMachinery;
		if (!plan.steps.empty() && plan.steps.front().task == routines::task::EmptyMachine)
		{
			r.goal = routines::task::EmptyMachine;
			r.chainTarget = best->site.object;
			r.plan.clear();
			for (const goap::Step& step : plan.steps)
				r.plan.push_back(step.task);
			r.since = now;
			member.set<state::Routine>(std::move(r));
			return;
		}
		if (operates)
		{
			const state::InteractTask* task = member.get<state::InteractTask>();
			if (task == nullptr || task->target != best->site.object || task->action != static_cast<std::uint8_t>(InteractAction::Use))
				beginInteract(id, best->site.object, static_cast<std::uint8_t>(InteractAction::Use), 0, {}, false);
		}
	}
	r.nextScore = now + routines::kScoreIntervalTicks;
	member.set<state::Routine>(std::move(r));
}

const routines::LayoutLibrary* Simulation::interiorLayouts() const
{
	if (gameData_ == nullptr)
		return nullptr;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	knowLayouts(*residentData_, world_);
	return residentData_->layouts.get();
}

Simulation::BarTable Simulation::claimBarTable(flecs::entity squad, flecs::entity town, std::uint64_t current)
{
	BarTable out;
	if (gameData_ == nullptr || !squad.is_valid() || !town.is_valid() || town.get<state::Id>() == nullptr || town.get<state::TownState>() == nullptr || squad.get<state::Id>() == nullptr)
		return out;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	ResidentData& data = *residentData_;
	flecs::world& ecs = ecsMutable();
	ResidentData::TownBars& bars = knowTownBars(data, world_, *gameData_, town.get<state::Id>()->value, *town.get<state::TownState>());
	// The tables of every public building, indexed (a bar with a layout has its real ones) and reachable by key.
	struct Tab
	{
		std::uint64_t key;
		double x;
		double z;
		bool hasY;
		double y;
	};
	const auto tablesOf = [&](std::uint64_t building)
	{
		std::vector<Tab> tables;
		if (!indexBuilding(data, world_, ecs, *gameData_, building))
			return tables;
		for (const routines::SiteEntry* entry : data.sites.sitesOf(building, routines::SiteKind::Table))
		{
			const std::uint64_t key = tableKey(building, entry->site.slot);
			data.tableByKey[key] = {building, entry->site.slot, entry->x, entry->z, entry->hasY, entry->y};
			tables.push_back({key, entry->x, entry->z, entry->hasY, entry->y});
		}
		// The building's loose chairs (its stools), last: a squad sits there only when every table is held.
		if (const auto loose = data.looseOfBuilding.find(building); loose != data.looseOfBuilding.end())
			tables.push_back({tableKey(building, routines::kLooseSlot), loose->second.x, loose->second.z, true, loose->second.y});
		return tables;
	};
	const std::set<std::uint64_t> barSet(bars.bars.begin(), bars.bars.end());
	const auto seatsFor = [&](std::uint64_t key)
	{
		const auto seats = data.seatsOfTable.find(key);
		return seats != data.seatsOfTable.end() ? seats->second : std::vector<std::array<double, 2>>{};
	};
	bool townHasSeats = false;
	for (const std::uint64_t building : bars.publicBuildings)
	{
		for (const Tab& tab : tablesOf(building))
			townHasSeats = townHasSeats || !seatsFor(tab.key).empty();
	}
	const std::uint64_t squadId = squad.get<state::Id>()->value;
	const std::uint64_t now = clockTick();
	if (current != 0)
	{
		const auto known = data.tableByKey.find(current);
		const routines::Site site{routines::SiteKind::Table, current, 0};
		if (known != data.tableByKey.end() && (routineBook_.free(site) || routineBook_.holder(site)->owner == squadId))
		{
			routineBook_.take(site, squadId, now);
			return {true, current, known->second.x, known->second.z, barSet.count(known->second.building) != 0, seatsFor(current), known->second.hasY, known->second.y};
		}
		routineBook_.release(squadId);
	}
	const std::uint64_t hour = static_cast<std::uint64_t>(trader::gameHours(now));
	const double roll = unitOf(drawHash(seed_, squadId, routines::task::RelaxInTownPackage, hour));
	// The BAR buildings from a drawn start index, then every public table of the town, then nothing.
	for (const bool barRule : {true, false})
	{
		const std::vector<std::uint64_t>& list = barRule ? bars.bars : bars.publicBuildings;
		for (std::size_t i = 0; i < list.size(); ++i)
		{
			const std::uint64_t building = list[(std::min(list.size() - 1, static_cast<std::size_t>(roll * static_cast<double>(list.size()))) + i) % list.size()];
			// A table with seats (a squad sits at a layout seat); one with none only in a town that has no seats at all (the synthetic bars of the test worlds).
			const auto tables = tablesOf(building);
			for (const bool seated : {true, false})
			{
				if (!seated && townHasSeats)
					continue; // a town with seats seats its patrons (k16.6.32): a seatless table is never claimed (nobody stands round it)
				for (const Tab& tab : tables)
				{
					const routines::Site site{routines::SiteKind::Table, tab.key, 0};
					if (!routineBook_.free(site) || seatsFor(tab.key).empty() == seated)
						continue;
					routineBook_.take(site, squadId, now);
					++(barRule ? townStats_.tablesBar : townStats_.tablesPublic);
					return {true, tab.key, tab.x, tab.z, barRule, seatsFor(tab.key), tab.hasY, tab.y};
				}
			}
		}
	}
	++townStats_.tablesNone;
	return out;
}

bool Simulation::seated(std::uint64_t memberId, double* faceYaw) const
{
	if (combat_ == nullptr || residentData_ == nullptr)
		return false;
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		if (members[i].id != memberId)
			continue;
		return seatedAt(i, faceYaw);
	}
	return false;
}

std::optional<Simulation::Pose> Simulation::pose(std::uint64_t memberId) const
{
	if (combat_ == nullptr)
		return std::nullopt;
	const auto& members = combat_->members();
	std::size_t i = 0;
	while (i < members.size() && members[i].id != memberId)
		++i;
	if (i == members.size())
		return std::nullopt;
	const state::Fighter& f = members[i].fighter;
	const flecs::entity member = flecs::entity(world_.ecs().c_ptr(), combatEntities_[i]);
	if (f.asleep)
	{
		primeSiteIndex(); // a loaded sleeper's bed is in the index before the first scorer pass too (writeDynamic runs before bedViews)
		const state::Routine* routine = member.get<state::Routine>();
		const routines::SiteEntry* bed = routine != nullptr ? routineSite(routine->target) : nullptr;
		if (bed != nullptr && bed->site.kind == routines::SiteKind::Bed && !bed->synthetic && bed->hasY)
		{
			// sleepinbed lays the body along the model's Z, sleeponfloor along its X (client clip probe, 2026-11-04): a 90 deg turn more puts
			// the floor sleeper's head where a bed's is. UNSURE: which end of a bedroll is the pillow (-90 deg assumes the bed's).
			const std::string clip = bed->clip.empty() ? std::string(routines::kClipSleepInBed) : bed->clip;
			constexpr double kQuarter = 1.57079632679489662;
			// The body lies on the use node itself, a few units above the floor (kinspect bed_seat_pose.md 2), not on the floor (the
			// client drew it under the mattress).
			const double poseY = bed->hasNodeY ? bed->nodeY : bed->y;
			return Pose{bed->x, poseY, bed->z, std::atan2(bed->faceX, bed->faceZ) - (clip == routines::kClipSleepOnFloor ? kQuarter : 0.0), clip};
		}
		return Pose{f.x, f.y, f.z, f.yaw, std::string(routines::kClipSleepOnFloor)};
	}
	double faceYaw = 0.0;
	if (!seatedAt(i, &faceYaw))
		return std::nullopt;
	if (const state::Routine* routine = member.get<state::Routine>(); routine != nullptr && routine->goal == routines::task::StayInHome)
	{
		// A home chair: its own spot, facing and clip. No chair (a ruin, a full home): the synthetic bed's convention, the member's own spot and yaw
		// on the floor-sit clip ("sitting idle", on the male and female skeletons).
		const routines::SiteEntry* chair = nullptr;
		homeSitAt(i, &chair);
		if (chair != nullptr)
			return Pose{chair->x, chair->hasY ? chair->y : f.y, chair->z, faceYaw, chair->clip.empty() ? std::string(routines::kClipSittingChair) : chair->clip};
		return Pose{f.x, f.y, f.z, f.yaw, std::string(routines::kClipSittingIdle)};
	}
	// seatedAt held a BarSeat, so the seat is the member's own.
	const routines::Site own = routineBook_.held(combat_->members()[i].id);
	const std::uint64_t table = own.object;
	const std::vector<std::array<double, 2>>& seats = residentData_->seatsOfTable.at(table);
	const auto poses = residentData_->seatPoses.find(table);
	const std::size_t nearest = own.slot;
	if (poses == residentData_->seatPoses.end() || nearest >= poses->second.size())
		return Pose{seats[nearest][0], f.y, seats[nearest][1], faceYaw, std::string(routines::kClipSittingChair)};
	const ResidentData::SeatPose& seat = poses->second[nearest];
	const auto tableAt = residentData_->tableByKey.find(table);
	const double yaw = seat.faceTable && tableAt != residentData_->tableByKey.end()
		? std::atan2(tableAt->second.x - seats[nearest][0], tableAt->second.z - seats[nearest][1]) : std::atan2(seat.faceX, seat.faceZ);
	return Pose{seats[nearest][0], seat.y, seats[nearest][1], yaw, seat.clip};
}

const routines::SiteEntry* Simulation::bedOf(std::uint64_t id) const
{
	if ((id & routines::kSiteIdFlag) == 0)
		return nullptr;
	if (residentData_ == nullptr)
		primeSiteIndex(); // an order after a load resolves its bed before the first scorer pass
	if (residentData_ == nullptr)
		return nullptr;
	for (const std::uint64_t key : residentData_->sites.keys())
		for (const routines::SiteEntry* bed : residentData_->sites.sitesOf(key, routines::SiteKind::Bed))
			if (routines::siteId(bed->site) == id)
				return bed;
	return nullptr;
}

std::string Simulation::sleepRefusal(std::uint64_t actor, std::uint64_t id) const
{
	if (combat_ == nullptr)
		return "unsupported";
	const state::Fighter* who = combat_->find(actor);
	if (who == nullptr)
		return "bad_target";
	// Down, out, carried: no bed. In a fight (a target): no bed either (the exe's Jobs rule for the same class). A carrier is
	// allowed (k16.6.217: it puts the carried one in, the 106/99 put-in) unless the carried is a corpse (UNSURE: exe rule not decoded).
	if (who->dead || who->unconscious || who->down || who->carriedBy != 0 || who->target != 0)
		return "not_allowed";
	if (who->carrying != 0)
	{
		const state::Fighter* carried = combat_->find(who->carrying);
		if (carried == nullptr || carried->dead)
			return "not_allowed";
	}
	const routines::SiteEntry* bed = bedOf(id);
	if (bed == nullptr)
		return "bad_target";
	// Occupied: another character's routine targets the bed, asleep in it or walking to it (the resident's claim is its
	// Routine::target, which the scorer writes when it picks the bed).
	for (const CombatArena::Member& member : combat_->members())
	{
		if (member.id == actor || member.id == who->carrying)
			continue;
		const state::Routine* routine = fighterEntity(member.id).get<state::Routine>();
		if (routine != nullptr && routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed)
			&& routine->target.object == bed->site.object && routine->target.slot == bed->site.slot)
			return "occupied";
	}
	return std::string();
}

void Simulation::primeSiteIndex() const
{
	if (gameData_ == nullptr)
		return;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	if (residentData_->allBuildingsIndexed)
		return;
	knowLayouts(*residentData_, world_);
	flecs::world& ecs = const_cast<flecs::world&>(world_.ecs()); // the build reads the doors, writes only `residentData_`
	for (const BuildingSite& site : world_.buildingSites())
	{
		if (site.ruined)
		{
			residentData_->ruinedBuildings.insert(site.key);
			continue; // no interior, so no layout furniture either
		}
		if (residentData_->sites.find(site.key) != nullptr)
			continue;
		// The building's own layout: the placement's `interior layout name`, else its squad template's `layout interior` (ensureBuilding).
		std::string layoutName = site.interiorLayout;
		if (layoutName.empty() && !site.squadTemplate.empty())
			if (const kdata::Record* squad = gameData_->find(site.squadTemplate))
				layoutName = squad->stringOr("layout interior", "");
		if (layoutName.empty() || residentData_->layouts == nullptr || residentData_->layouts->find(site.buildingSid + "-" + layoutName) == nullptr)
			continue; // no layout: synthetic sites only, so no real bed
		indexBuilding(*residentData_, world_, ecs, *gameData_, site.key);
	}
	residentData_->allBuildingsIndexed = true;
}

std::vector<Simulation::BedView> Simulation::bedViews() const
{
	return bedViewsRef();
}

const std::vector<Simulation::BedView>& Simulation::bedViewsRef() const
{
	// A loaded save has not run a scorer pass yet: build the site index here, every placed building with a layout, a function of the
	// data alone. Indexed on demand, the index kept an old home's beds after its squad moved out: 744 beds before a save, 712 after it.
	primeSiteIndex();
	if (residentData_ == nullptr)
	{
		bedCache_.clear();
		bedCacheSites_.clear();
		bedCacheRevision_ = 0;
		return bedCache_;
	}
	if (bedCacheRevision_ != residentData_->sites.revision() || bedCacheRuins_ != residentData_->ruinedBuildings.size())
	{
		bedCache_.clear();
		bedCacheSites_.clear();
		bedCacheRevision_ = residentData_->sites.revision();
		bedCacheRuins_ = residentData_->ruinedBuildings.size();
		constexpr double kQuarter = 1.57079632679489662;
		for (const std::uint64_t key : residentData_->sites.keys())
		{
			const routines::BuildingSites* building = residentData_->sites.find(key);
			for (const routines::SiteEntry* bed : residentData_->sites.sitesOf(key, routines::SiteKind::Bed))
			{
				if (bed->synthetic || residentData_->ruinedBuildings.count(key) != 0)
					continue; // a yard bed is not furniture: no id, nothing to order; a ruin has none either
				BedView view;
				view.id = routines::siteId(bed->site);
				view.building = building != nullptr ? building->key : key;
				view.record = bed->record;
				view.x = bed->x;
				view.y = bed->y; // the floor point (where the walk ends)
				view.useY = bed->hasNodeY ? bed->nodeY : bed->y; // the body's spot: the use node a few units above it
				view.z = bed->z;
				const std::string clip = bed->clip.empty() ? std::string(routines::kClipSleepInBed) : bed->clip;
				view.clip = clip;
				view.yaw = std::atan2(bed->faceX, bed->faceZ) - (clip == routines::kClipSleepOnFloor ? kQuarter : 0.0);
				bedCache_.push_back(std::move(view));
				bedCacheSites_.emplace_back(bed->site.object, bed->site.slot);
			}
		}
	}
	// Whose routine targets a bed: one pass over the members, the same claim the `occupied` refusal reads.
	std::map<std::pair<std::uint64_t, std::uint16_t>, std::uint64_t> occupants;
	if (combat_ != nullptr)
	{
		for (const CombatArena::Member& member : combat_->members())
			if (const state::Routine* routine = fighterEntity(member.id).get<state::Routine>())
				if (routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed))
					occupants.emplace(std::make_pair(routine->target.object, routine->target.slot), member.id);
	}
	for (std::size_t i = 0; i < bedCache_.size(); ++i)
	{
		const auto occupant = occupants.find(bedCacheSites_[i]);
		bedCache_[i].occupant = occupant != occupants.end() ? occupant->second : 0;
	}
	return bedCache_;
}

const routines::SiteEntry* Simulation::routineSite(const state::RoutineSite& site) const
{
	if (residentData_ == nullptr)
		return nullptr;
	return residentData_->sites.siteAt({static_cast<routines::SiteKind>(site.kind), site.object, site.slot});
}

bool Simulation::homeSitAt(std::size_t i, const routines::SiteEntry** seat) const
{
	const state::Fighter& f = combat_->members()[i].fighter;
	const flecs::entity member = flecs::entity(world_.ecs().c_ptr(), combatEntities_[i]);
	const state::Routine* routine = member.get<state::Routine>();
	if (routine == nullptr || routine->goal != routines::task::StayInHome || f.dead || f.unconscious || f.down || f.asleep || f.walking()
		|| f.carriedBy != 0 || f.carrying != 0 || f.target != 0 || !f.engaged.empty() || f.drive != state::DriveMode::Stop)
		return false;
	if (routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Seat))
	{
		// A reserved chair: seated once at it (the walk ends within kReach of it); not yet there, the member stands.
		const routines::SiteEntry* chair = routineSite(routine->target);
		if (chair == nullptr || std::hypot(f.x - chair->x, f.z - chair->z) > routines::kSeatTolerance)
			return false;
		if (seat != nullptr)
			*seat = chair;
		return true;
	}
	// A floor-sit only at the home anchor (as scoreMember's: the building's HomePoint, else the squad's yard point; kHomeSettle as its atHome):
	// a walk that stalls on the way stands where it is.
	const std::vector<const routines::SiteEntry*> homes = f.homeBuilding != 0 ? residentData_->sites.sitesOf(f.homeBuilding, routines::SiteKind::HomePoint)
		: std::vector<const routines::SiteEntry*>{};
	const state::Position* yard = homes.empty() ? member.target<state::MemberOf>().get<state::Position>() : nullptr;
	if (homes.empty() && yard == nullptr)
		return false;
	return std::hypot(f.x - (homes.empty() ? yard->x : homes.front()->x), f.z - (homes.empty() ? yard->z : homes.front()->z)) <= kHomeSettle;
}

bool Simulation::seatedAt(std::size_t i, double* faceYaw) const
{
	if (combat_ == nullptr || residentData_ == nullptr)
		return false;
	const state::Fighter& f = combat_->members()[i].fighter;
	const flecs::entity member = flecs::entity(world_.ecs().c_ptr(), combatEntities_[i]);
	const state::Routine* routine = member.get<state::Routine>();
	// k16.6.30: a STAY_IN_HOME resident at its home chair, or on the floor where it stands (a synthetic seat).
	if (routine != nullptr && routine->goal == routines::task::StayInHome)
	{
		const routines::SiteEntry* chair = nullptr;
		if (!homeSitAt(i, &chair))
			return false;
		if (faceYaw != nullptr)
			*faceYaw = chair != nullptr ? std::atan2(chair->faceX, chair->faceZ) : f.yaw;
		return true;
	}
	if (residentData_->seatsOfTable.empty() || routine == nullptr || routine->goal != routines::task::RelaxInTownPackage || f.dead || f.walking())
		return false;
	// The member's own seat (kenshi-k16.6.32): the one it holds in the book, a BarSeat of a table of its bar; seated once within kSeatTolerance of it.
	const routines::Site held = routineBook_.held(combat_->members()[i].id);
	if (held.kind != routines::SiteKind::BarSeat)
		return false;
	const auto seats = residentData_->seatsOfTable.find(held.object);
	if (seats == residentData_->seatsOfTable.end() || held.slot >= seats->second.size())
		return false;
	const std::array<double, 2>& seat = seats->second[held.slot];
	if (std::hypot(f.x - seat[0], f.z - seat[1]) > routines::kSeatTolerance)
		return false;
	if (faceYaw != nullptr)
	{
		const auto table = residentData_->tableByKey.find(held.object);
		*faceYaw = table != residentData_->tableByKey.end() ? std::atan2(table->second.x - f.x, table->second.z - f.z) : f.yaw;
	}
	return true;
}

void Simulation::releaseBarTable(flecs::entity squad)
{
	if (!squad.is_valid() || squad.get<state::Id>() == nullptr)
		return;
	routineBook_.release(squad.get<state::Id>()->value);
	if (!squad.has<state::SquadRoutine>())
		return;
	// In place: this runs inside the squad AI step, which holds references into the squad's components (no structural change here).
	if (squad.get<state::SquadRoutine>()->barTable != 0)
		squad.get_mut<state::SquadRoutine>()->barTable = 0;
}

void Simulation::knowBarSeats()
{
	if (gameData_ == nullptr || (residentData_ != nullptr && residentData_->barsPrimed))
		return;
	flecs::world& ecs = ecsMutable();
	bool missing = false;
	ecs.each([&](const state::SquadRoutine& held)
	{
		if (held.barTable != 0 && (residentData_ == nullptr || residentData_->seatsOfTable.count(held.barTable) == 0))
			missing = true;
	});
	if (!missing)
		return;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	std::vector<flecs::entity> towns;
	ecs.each([&](flecs::entity town, const state::Id&, const state::TownState&) { towns.push_back(town); });
	// The seats come with the buildings' layouts (addLayoutFurniture), so every public building is indexed as claimBarTable indexes it: a town's
	// bar list alone leaves a table held in a building no one has looked at since the load without its seats.
	for (const flecs::entity town : towns)
	{
		const ResidentData::TownBars& bars = knowTownBars(*residentData_, world_, *gameData_, town.get<state::Id>()->value, *town.get<state::TownState>());
		for (const std::uint64_t building : bars.publicBuildings)
		{
			if (!indexBuilding(*residentData_, world_, ecs, *gameData_, building))
				continue;
			for (const routines::SiteEntry* entry : residentData_->sites.sitesOf(building, routines::SiteKind::Table))
				residentData_->tableByKey[tableKey(building, entry->site.slot)] = {building, entry->site.slot, entry->x, entry->z, entry->hasY, entry->y};
			if (const auto loose = residentData_->looseOfBuilding.find(building); loose != residentData_->looseOfBuilding.end())
				residentData_->tableByKey[tableKey(building, routines::kLooseSlot)] = loose->second;
		}
	}
	residentData_->barsPrimed = true;
}

void Simulation::knowHomeSeats()
{
	if (homeSeatsKnown_ || gameData_ == nullptr)
		return;
	homeSeatsKnown_ = true;
	flecs::world& ecs = ecsMutable();
	std::set<std::uint64_t> buildings;
	ecs.each([&](const state::Routine& routine)
	{
		if (routine.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Seat) && routine.target.object != 0)
			buildings.insert(routine.target.object);
	});
	if (buildings.empty())
		return;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	for (const std::uint64_t building : buildings)
		indexBuilding(*residentData_, world_, ecs, *gameData_, building);
}

bool Simulation::imprisonScenario(std::uint64_t bodyId, const std::string& which, std::int32_t bounty, bool peeler)
{
	const state::Fighter* body = combat_ != nullptr ? combat_->find(bodyId) : nullptr;
	if (body == nullptr || gameData_ == nullptr)
		return false;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	knowBuildingSites(*residentData_, world_);
	std::vector<std::uint64_t> keys;
	if (which == "near")
	{
		for (const BuildingSite& site : world_.buildingSites())
		{
			if (std::hypot(site.cx - body->x, site.cz - body->z) <= kScenarioCageReach + 60.0)
				keys.push_back(site.key);
		}
	} else {
		char* end = nullptr;
		const std::uint64_t key = std::strtoull(which.c_str(), &end, 10);
		if (end == which.c_str() || *end != '\0')
			return false;
		keys.push_back(key);
	}
	flecs::world& ecs = ecsMutable();
	const routines::SiteEntry* best = nullptr;
	double bestDistance = 0.0;
	for (const std::uint64_t key : keys)
	{
		if (!indexBuilding(*residentData_, world_, ecs, *gameData_, key))
			continue;
		for (const routines::SiteEntry* entry : residentData_->sites.sitesOf(key, routines::SiteKind::Cage))
		{
			bool taken = false;
			for (const CombatArena::Member& m : combat_->members())
				taken = taken || (m.fighter.cageBuilding == entry->site.object && m.fighter.cageSlot == entry->site.slot);
			const double d = std::hypot(entry->x - body->x, entry->z - body->z);
			if (!taken && (which != "near" || d <= kScenarioCageReach) && (best == nullptr || d < bestDistance))
			{
				best = entry;
				bestDistance = d;
			}
		}
	}
	if (best == nullptr)
		return false;
	if (bounty > 0)
	{
		// The cage owner's legal faction holds it, as an arrest's would: imprison starts the sentence from it (a never-sentenced prisoner counts as
		// served at once and the law frees it).
		const routines::BuildingSites* building = residentData_->sites.find(best->site.object);
		std::string owner = building != nullptr ? building->ownerFaction : std::string();
		if (owner.empty())
			if (const state::Allegiance* mine = fighterEntity(bodyId).get<state::Allegiance>())
				owner = mine->factionSid;
		const std::string legal = crime::legalFaction(gameData_.get(), owner);
		if (!legal.empty())
			addBounty(bodyId, legal, bounty);
	}
	imprison(bodyId, bodyId, *best);
	setScenarioLocked(best->site.object); // the Locked a cage_prisoner file means (CHAR_STATS 5), not the Unlocked a warden that has not locked yet reads as (6): kenshi-k16.6.98
	if (peeler)
		setPeeler(best->site.object, true);
	if (best->hasY)
		combat_->find(bodyId)->y = best->y; // the cage's floor, not the y the file gave the character
	return true;
}

void Simulation::attachBarTables()
{
	if (ai_ == nullptr)
		return;
	if (!barBookKnown_)
	{
		barBookKnown_ = true;
		rebuildRoutineBook();
	}
	// The wandering traders carry SquadRoutine for their table: added here, between steps, because the AI step holds references into the entity.
	std::vector<flecs::entity> bare;
	ecsMutable().each([&](flecs::entity squad, const state::TraderTrip&)
	{
		if (!squad.has<state::SquadRoutine>())
			bare.push_back(squad);
	});
	for (const flecs::entity squad : bare)
		squad.set<state::SquadRoutine>(state::SquadRoutine{});
	// A table held by a squad that no longer exists (wiped in a raid, despawned) is given back before anyone claims: the book a load rebuilds
	// has only the living squads' tables, so the running game must not keep the dead one's (stepResidents' sweep runs only with combat).
	if (!routineBook_.empty())
	{
		std::set<std::uint64_t> squads;
		ecsMutable().each([&](const state::Id& id, const state::SquadRoutine&) { squads.insert(id.value); });
		routineBook_.releaseDead([&](std::uint64_t owner)
		{
			return routineBook_.held(owner).kind != routines::SiteKind::Table || squads.count(owner) != 0;
		});
	}
	BarTables tables;
	tables.claim = [this](flecs::entity squad, flecs::entity_t townId) -> std::optional<std::array<double, 2>>
	{
		if (gameData_ == nullptr || townId == 0 || !ecsMutable().is_alive(townId))
			return std::nullopt;
		// A trader gets SquadRoutine one step before it can hold a table (attachBarTables below): adding it here would move the entity under the AI.
		if (!squad.has<state::SquadRoutine>())
			return std::nullopt;
		const std::uint64_t current = squad.get<state::SquadRoutine>()->barTable;
		const BarTable table = claimBarTable(squad, ecsMutable().entity(townId), current);
		if (!table.ok)
			return std::nullopt;
		if (table.key != current)
		{
			squad.get_mut<state::SquadRoutine>()->barTable = table.key;
			++townStats_.traderTables;
		}
		return std::array<double, 2>{table.x, table.z};
	};
	tables.release = [this](flecs::entity squad) { releaseBarTable(squad); };
	ai_->setBarTables(std::move(tables));
}

std::optional<std::uint64_t> Simulation::townIntruder273(const state::Fighter& self, const sensor::Context& ctx) const
{
	const perception::Environment& env = combat_->environment();
	const int myTown = env.townOf(self.x, self.z);
	if (myTown < 0 || !townOursAt(self.x, self.z, self.faction))
		return std::nullopt;
	return worldres::pickupIntrudersTown273(self, myTown, ctx, env, [&](std::uint64_t target) { return combat_->find(target); });
}

// The exe's "the town is ours" (1406211e0; UNSURE - the same family as 140621390, never decoded). Goal 273's resolver runs it before
// its body scan; ours reads the current town's owner faction (TownState::owner, towncircle.hpp's getTown geometry). The same test
// crime.cpp's FactionView::inOwnTown makes by faction sid for the law paths; here the caller has the faction's state::Id.
bool Simulation::townOursAt(double x, double z, std::uint64_t faction) const
{
	if (gameData_ == nullptr || faction == 0)
		return false;
	std::vector<towncircle::Circle> circles;
	std::vector<std::uint64_t> owners;
	world_.ecs().each([&](const state::TownState& town)
	{
		circles.push_back(towncircle::of(*gameData_, town));
		owners.push_back(town.owner.id);
	});
	const int at = towncircle::at(circles, x, z);
	if (at >= 0 && owners[static_cast<std::size_t>(at)] == faction)
		return true;
	// An occupying faction passes the test through the town's occupancy map, not through an owner change (rwh.53 section 4.3).
	const auto held = occupiers_.find(townEntityAt(x, z));
	return held != occupiers_.end() && std::find(held->second.begin(), held->second.end(), faction) != held->second.end();
}

void Simulation::getOutOfBed(flecs::entity member, std::uint64_t id)
{
	state::Fighter* f = combat_->find(id);
	if (f == nullptr)
		return;
	f->asleep = false;
	routineBook_.release(id); // the bed's occupancy (140345620: the building's release of my slot)
	if (const state::Routine* routine = member.get<state::Routine>())
	{
		state::Routine r = *routine;
		r.goal = 0;
		r.target = {};
		r.nextScore = clockTick();
		member.set<state::Routine>(std::move(r));
	}
	mirrorFighter(id);
}

Simulation::HomeDoors Simulation::homeDoorsOf(std::uint64_t homeBuilding, double fromX, double fromZ)
{
	HomeDoors out;
	if (residentData_ == nullptr || homeBuilding == 0)
		return out;
	flecs::world& ecs = ecsMutable();
	knowDoors(*residentData_, ecs);
	const auto doors = residentData_->doorsOfBuilding.find(homeBuilding);
	if (doors == residentData_->doorsOfBuilding.end())
		return out;
	for (const flecs::entity_t doorId : doors->second)
	{
		if (!ecs.is_alive(doorId))
			continue;
		const flecs::entity door = ecs.entity(doorId);
		const state::Door* d = door.get<state::Door>();
		if (d == nullptr || d->gate || d->state == 3)
			continue;
		out.doors.push_back(door);
		out.any = true;
		out.states.push_back(d->state);
		out.anyLocked = out.anyLocked || d->state == 2;
		out.nearest = std::min(out.nearest, std::hypot(fromX - d->x, fromZ - d->z));
	}
	out.canLock = worldres::firstDoorToLock155(out.states) >= 0;
	out.needOpen = worldres::firstDoorToOpen86(out.states) >= 0;
	return out;
}

bool Simulation::strangerIn(std::uint64_t building) const
{
	for (const CombatArena::Member& m : combat_->members())
		if (m.fighter.building == building && m.fighter.playerControlled && !m.fighter.dead && !m.fighter.unconscious && !m.fighter.down)
			return true;
	return false;
}

void Simulation::applyHomeDoors(const std::vector<flecs::entity>& doors, bool lock)
{
	for (const flecs::entity door : doors)
	{
		state::Door* d = door.get_mut<state::Door>();
		if (d == nullptr || d->state == 3)
			continue;
		const std::uint8_t want = lock ? 2 : 0;
		if (lock ? d->state == 2 : d->state == 0)
			continue;
		d->state = want;
		++d->version; // the door stream (SIMLINK interactables) sends a record whenever the version changes
		++(lock ? townStats_.doorsLocked : townStats_.doorsOpened);
	}
}

bool Simulation::resolveDoorRow(std::uint64_t id, std::uint16_t task, const HomeDoors& doors, bool homeLocked, bool stranger) const
{
	// 155 / 197 (dt24_155): the door-list resolver, then the registry row's plan - no strangers in the house, else the plan grows the
	// 283 shoo in front and the lock waits (ours: the candidate is skipped, the shoo goal being its own row).
	if (task == routines::task::LockAllMyDoors || task == routines::task::LockDoorsStayOutside)
	{
		if (!doors.canLock || homeLocked || stranger)
			return false;
		const LockFacts facts{!doors.canLock, doors.nearest <= kDoorReach}; // the plan: [163 / 199 walk,] 162 / 198 lock, the goal node
		return planTownGoal(id, task, false, &facts).found();
	}
	// 86: a closed or locked door, and the row's plan.
	return doors.needOpen && planTownGoal(id, task, false).found();
}

bool Simulation::cageTaken(const routines::Site& site) const
{
	// A cage holding someone is not free (T3e2). The occupancy is the captives' own saved cage fields (Fighter::cageBuilding /
	// cageSlot, the original's location type 2): no separate book to rebuild, and a load keeps it.
	for (const CombatArena::Member& other : combat_->members())
		if (other.fighter.cageBuilding == site.object && other.fighter.cageSlot == site.slot)
			return true;
	return false;
}

std::optional<Simulation::SiteChoice> Simulation::pickSite(const state::Routine& r, std::uint64_t id, std::uint16_t goal, std::uint64_t key,
	routines::SiteKind kind, double pickRoll, std::uint64_t now, double anchorX, double anchorZ, std::optional<std::array<double, 2>> nearestTo,
	bool realOnly) const
{
	std::vector<routines::Site> free;
	std::vector<std::array<double, 2>> where;
	std::vector<std::optional<double>> heights;
	const routines::Site mine = fromState(r.target);
	bool mineFree = false;
	std::size_t mineAt = 0;
	const auto offer = [&](const routines::Site& site, double x, double z, std::optional<double> y = std::nullopt, bool synthetic = false)
	{
		if (realOnly && synthetic)
			return;
		if (isBlocked(r, goal, toState(site), now))
			return;
		if (cageTaken(site))
			return;
		const routines::Claim* held = routineBook_.holder(site);
		if (held != nullptr && held->owner != id)
			return;
		// A sleeper keeps its bed when the plain and the secure bed goal swap (OURS: a re-roll moved a sleeper to another bedroll unwoken).
		if (site == mine && (r.goal == goal || (bedTask(r.goal) && bedTask(goal))))
		{
			mineFree = true;
			mineAt = free.size();
		}
		free.push_back(site);
		where.push_back({x, z});
		heights.push_back(y);
	};
	if (kind == routines::SiteKind::Work)
	{
		for (const ProducerSite& producer : world_.producerSites())
		{
			if (std::fabs(producer.x - anchorX) > kWorkReach || std::fabs(producer.z - anchorZ) > kWorkReach
				|| std::hypot(producer.x - anchorX, producer.z - anchorZ) > kWorkReach)
				continue;
			offer({routines::SiteKind::Work, producer.id, 0}, producer.x, producer.z);
		}
	} else {
		// The shopkeeper stands at the layout's Node Shopkeeper marker, behind the counter; the counter piece itself is the customers' side
		// (kenshi-k16.6.106). A building with no marker keeps the counter slots.
		if (kind == routines::SiteKind::Counter && !residentData_->sites.sitesOf(key, routines::SiteKind::ShopkeeperNode).empty())
			kind = routines::SiteKind::ShopkeeperNode;
		for (const routines::SiteEntry* entry : residentData_->sites.sitesOf(key, kind))
			offer(entry->site, entry->x, entry->z, entry->hasY ? std::optional<double>(entry->y) : std::nullopt, entry->synthetic);
	}
	if (free.empty())
		return std::nullopt;
	std::size_t pick = std::min(free.size() - 1, static_cast<std::size_t>(pickRoll * static_cast<double>(free.size())));
	if (kind == routines::SiteKind::Bed)
		pick = 0; // dt29 section 3: the first acceptable bed in slot order
	if (mineFree)
		pick = mineAt;
	else if (nearestTo)
	{
		// Nearest to the point; a tie keeps the first (the sites come in slot order).
		double best = 1.0e30;
		for (std::size_t i = 0; i < where.size(); ++i)
		{
			const double d = std::hypot(where[i][0] - (*nearestTo)[0], where[i][1] - (*nearestTo)[1]);
			if (d < best)
			{
				best = d;
				pick = i;
			}
		}
	}
	return SiteChoice{free[pick], where[pick][0], where[pick][1], heights[pick]};
}

void Simulation::stepRoutines()
{
	if (!townScorer_ || combat_ == nullptr || gameData_ == nullptr || clockTick() % kLookTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::Routine>() == 0)
		return;
	// After a load stepResidents has not run yet (it looks once a second): the scorer must not wait for it, or a loaded run would skip the
	// passes that fall between, and differ from the uninterrupted one.
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	const std::uint64_t now = clockTick();
	const auto& members = combat_->members();
	std::set<std::uint64_t> targeted;
	bool targetedBuilt = false;
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity entity = ecs.entity(combatEntities_[i]);
		const state::TalkHold* talkHold = entity.get<state::TalkHold>();
		if (talkHold != nullptr && talkHold->until <= now)
		{
			entity.remove<state::TalkHold>(); // the hold ran out
			talkHold = nullptr;
		}
		// After the remove above: it moves the entity to another table, and a pointer read before it is a moved-from Routine (k16.5.14).
		const state::Routine* routine = entity.get<state::Routine>();
		// A turret gunner (task ds/turret S3, dt41_B): the member whose running goal is 149 / 222 on a turret, standing at its post, mounts it
		// (Task_UseTurret's start: the operator slot is the routine book's claim, the gun the building's GUN_DATA); a mounted member whose goal
		// has moved on, or that is down, carried or taken over, lets go (the task's stop). The combat step does the shooting.
		{
			const state::Fighter& gunner = members[i].fighter;
			const ResidentData::TurretInfo* post = nullptr;
			if (routine != nullptr && (routine->goal == routines::task::ManTurrets || routine->goal == routines::task::TurretOnBuilding)
				&& routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Turret) && !gunner.dead && !gunner.down
				&& !gunner.unconscious && !gunner.playerControlled && gunner.carriedBy == 0)
			{
				knowTurrets(*residentData_, world_, *gameData_, turretManning_);
				for (const ResidentData::TurretInfo& turret : residentData_->turretSites)
					if (turret.key == routine->target.object)
					{
						post = &turret;
						break;
					}
			}
			if (gunner.mount.mounted() && (post == nullptr || post->key != gunner.mount.building))
				combat_->dismountTurret(members[i].id);
			else if (post != nullptr && !gunner.mount.mounted() && !gunner.walking() && std::hypot(gunner.x - post->x, gunner.z - post->z) <= kMountReach)
				if (const kdata::Record* gun = gameData_->find(post->gunSid); gun != nullptr)
					combat_->mountTurret(members[i].id, post->key, post->x, post->z, post->yaw, combat_->data().makeTurretGun(*gameData_, *gun));
		}
		// The running routine's preconditions are rechecked every look (kinspect s63: a goal's task ends when its preconditions fail), not
		// when the routine's own timer comes round - a bed holds that timer for hours. The bed is the one goal with a recheck so far:
		// USE_BED's preconditions are no alarm in the vicinity, no weapon equipped, nothing carried, and the bed slot still the member's
		// (Task_UseBed registry, s63 lines 263-270). Failing one is the same get-out-of-bed action 208 runs, with no package row needed.
		if (routine != nullptr && members[i].fighter.asleep && !members[i].fighter.playerControlled && bedTask(routine->goal))
		{
			const state::Fighter& sleeper = members[i].fighter;
			if (!targetedBuilt)
			{
				targetedBuilt = true;
				for (const CombatArena::Member& m : members)
					if (m.fighter.target != 0 && !m.fighter.dead)
						targeted.insert(m.fighter.target);
			}
			// ALARMS_IN_THE_VICINITY: the alarm / enemy count of AI+0xbc + AI+0x80 (ours: a fight of its own, or someone targeting it).
			bool fails = sleeper.target != 0 || targeted.count(members[i].id) != 0;
			const bool attacked = fails;
			fails = fails || pickAlarm(members[i].id, false) != nullptr; // the town alarm manager (alarm.hpp)
			// Carrying something, or a weapon in the hands: the exe plans UNEQUIP_WEAPON ahead of the bed (ours: runWeaponPrefix as the
			// bed is taken, so a sleeper's hands are empty), and a sleeper handed a weapon fails the precondition and wakes.
			fails = fails || sleeper.carrying != 0;
			if (weaponInHands(members[i].id))
			{
				// A resident placed in its bed at promotion gets its gear the same tick, after the placement: the plan's 7 runs now. Later
				// (a weapon handed to a sleeper) the precondition fails and it wakes.
				if (ticksSince(clockTick(), routine->since) <= kLookTicks)
					sheatheWeapon(members[i].id);
				else
					fails = true;
			}
			// The bed slot is no longer free for it: another member holds the claim.
			if (const routines::Claim* claim = routineBook_.holder(fromState(routine->target)))
				fails = fails || claim->owner != members[i].id;
			if (fails)
			{
				if (attacked)
					drawWeapon(members[i].id); // woken by an attack: the plan's 6 (stepRearm covers the target it then picks)
				getOutOfBed(entity, members[i].id);
				continue;
			}
		}
		if (routine == nullptr || routine->nextScore > now)
			continue;
		const flecs::entity squad = entity.target<state::MemberOf>();
		// The town schedules are a promoted resident squad's (promoteResidents sets its SquadRoutine). A world squad has none: its
		// members' Routines are their own package tasks (task BO's capture 167 / loot 177, world_tasks.cpp), which the scorer must not
		// score away.
		if (!squad.is_valid() || squad.get<state::SquadInstance>() == nullptr || squad.get<state::SquadRoutine>() == nullptr)
			continue;
		const state::Fighter& f = members[i].fighter;
		// The scorer yields to fighting, being down or carried, a door or interaction in progress, the player (5.1), an individual
		// goal (DIALOGUE.md 5, step 6b) and a client move order in progress (routeSeq != 0: a routine's own walks set none, task CC:
		// a scored goal used to land over a client's order for one tick, ksim_bw_orders). Exception (T3e2): a member carrying the body
		// its slave chain picked keeps being scored — the cage goal 112 walks it to the cage (stepSlaveTrade puts the person in).
		const bool chainedCarry = f.carrying != 0 && routine->chainTarget == f.carrying;
		if (f.dead || f.unconscious || f.down || inFight(f) || f.carriedBy != 0 || (f.carrying != 0 && !chainedCarry) || f.playerControlled
			|| (f.walking() && f.routeSeq != 0)
			|| f.goal.kind != state::IndividualKind::None
			|| entity.has<state::InteractTask>() || entity.has<state::DoorStep>() || (entity.has<state::CarryStep>() && !chainedCarry)
			|| entity.has<state::TakeOut>() || entity.has<state::GateDuty>() || arrestOrders_.count(members[i].id) != 0
			|| talkHold != nullptr) // the player is talking to it: it stands still (state::TalkHold)
		{
			state::Routine r = *routine;
			r.nextScore = now + routines::kScoreIntervalTicks;
			entity.set<state::Routine>(std::move(r));
			// Fighting, being downed or carried, or any order wakes a sleeper (the bed's preconditions above catch the fight and the
			// carry first; this is the rest).
			if (f.asleep && !f.playerControlled)
				getOutOfBed(entity, members[i].id);
			continue;
		}
		scoreMember(entity, squad, false);
	}
}

} // namespace ksim
