#pragma once

// The loaded world model: one flecs prefab per game-data record, the
// relationships between them, and one entity per placed BUILDING instance.
//
// Loading reads kdata (merged records) and kworld (terrain, zones, towns)
// once. After load the world is read-only through this API; simulation state
// changes only via Simulation's command queue (ARCHITECTURE.md "Layering").

#include "ksim/detail.hpp"
#include "ksim/state.hpp"
#include "ksim/templates.hpp"
#include "kworld/zones.hpp"

#include <flecs.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}

namespace ksim {

class Simulation;

struct FactionRelationPair
{
	flecs::entity_t from = 0;
	flecs::entity_t to = 0;
};

struct WorldCounts
{
	std::size_t prefabs = 0;
	std::size_t factions = 0;
	std::size_t towns = 0;
	std::size_t squadTemplates = 0;
	std::size_t characters = 0;
	std::size_t humanCharacters = 0;
	std::size_t animals = 0;
	std::size_t buildings = 0;
	std::size_t races = 0;
	std::size_t aiPackages = 0;

	std::size_t placedBuildings = 0;
	std::size_t placedWithPrefab = 0;

	// Relationship pair counts (see templates.hpp for the relation types).
	std::size_t factionRelations = 0;
	std::size_t membership = 0;
	std::size_t townOwnership = 0;
	std::size_t townResidents = 0;
	std::size_t townBarSquads = 0;
	std::size_t townRoamingSquads = 0;
	std::size_t townDefaultResident = 0;
	std::size_t factionResidents = 0;
	std::size_t factionDefaultResident = 0;
	std::size_t factionBarSquads = 0;
	std::size_t factionSquads = 0;
	std::size_t factionSquadDefault = 0;
	std::size_t factionRoamingSquads = 0;
	std::size_t squadMembers = 0;
	std::size_t squadMembers2 = 0;
	std::size_t squadLeaders = 0;
	std::size_t squadChooseFrom = 0;
	std::size_t squadAnimals = 0;
	std::size_t squadAnimals2 = 0;
	std::size_t raceOptions = 0;
	std::size_t aiPackageLinks = 0;
	std::size_t aiInherits = 0;
	std::size_t aiLeadsTo = 0;
	std::size_t overridesTown = 0;
};

// One door of a placed building (a child BUILDING of its `doors` list), keyed like the navmesh bake
// (kworld::doorKey of "cellX,cellZ/instanceID#n"). x, z is the parent's placement (the door slab is
// modelled in the parent's frame). Derived from the game data, not saved.
struct DoorSite
{
	std::uint64_t key = 0;
	double x = 0.0;
	double z = 0.0;
	// The parent is a `is wall gate` / `is gateway` building: a town gate.
	bool gate = false;
	// The door's `always locks`: its closed state is locked for those without the gate code.
	bool alwaysLocks = false;
	// The door's BUILDING record (its string id); empty if the reference is dangling.
	std::string recordSid;
	// The door's `door move speed` (the DoorStuff ctor's default 0.5 when the record has none): openness per game-second.
	double moveSpeed = 0.5;
	// The building the door belongs to: kworld::doorKey(kworld::placementId) of the parent placement (shared by all its doors).
	std::uint64_t building = 0;
	// The parent placement's `owner faction ID` (its local GAMESTATE_BUILDING): the building's faction, "" = none (then the
	// town's). The lock level and isObjectMine read it (GATES_CARRY_LIVE.md b).
	std::string ownerFaction;
};

// A town gate: a placed `is wall gate` building, at its first door (GRADES_GATESQUADS_KO_LIVE.md b, DOORS.md section 8).
// The door's inside and outside points are 15 units either side of its centre along the slab's horizontal normal n
// (FUN_14029cc90); a gate squad spawns on the outside point and its leader mans it; the members stand at gateStandPoint (gatecrew.hpp). Derived from the
// game data (the door slab's collision), not saved. xz only: the sim's squads are on the plane; the walker snaps to ground.
struct GateSite
{
	std::uint64_t key = 0; // the first door (DoorSite::key)
	std::uint64_t building = 0; // the gate placement (DoorSite::building)
	std::string buildingSid; // the gate's BUILDING record (a squad template's `building` list names these)
	std::string ownerFaction; // the placement's `owner faction ID`, "" = none
	std::string squadTemplate; // the resident template the zone data gave the gate (BuildingSite::squadTemplate); "" = none
	std::uint64_t townHand = 0; // the town whose populate owns it (BuildingSite::townHand)
	double x = 0.0; // the door slab's centre (bounding box)
	double z = 0.0;
	double nx = 0.0; // unit horizontal normal pointing outside
	double nz = 0.0;
	double outX = 0.0; // door centre + 15 n
	double outZ = 0.0;
	double inX = 0.0; // door centre - 15 n
	double inZ = 0.0;
};

// The clearance under a ceiling: a character whose feet are this far below the placement's top (maxY) has a roof over it. The
// roof slabs sit within it (the Hub Bar's deck 1649.2 under its top 1662.2, 13 u), so a top deck reads as open to the sky
// (indoors_state.md section 1: the exe's per-tick up ray).
inline constexpr double kRoofClearance = 30.0;

// The footprint of a placed building that has doors (not a town gate): an oriented box on the plane, centre (cx, cz), unit axis A
// (axisX, axisZ) and B = (-axisZ, axisX), half extents along each, and the height range of its collision. `key` is
// DoorSite::building. Position-based building membership (Simulation::placeInBuildings) tests against it. Derived, not saved.
struct BuildingSite
{
	std::uint64_t key = 0;
	std::string buildingSid; // the placement's BUILDING record (a squad template's `building` list names these; the T3d bar finder)
	std::string squadTemplate; // the placement's own squad template, `interior layout name` and label (zone GAMESTATE_BUILDING; T3d follow-up): "" = none
	std::string interiorLayout;
	std::string label;
	// The placement's `owner faction ID` (its local GAMESTATE_BUILDING): the building's faction, "" = none (then the town's). The
	// 153 / 239 bed ladder's allied test (TOWN_LIFE_SLICE 5.4) reads it.
	std::string ownerFaction;
	// The town whose populate owns this building: the zone state's town hand (kworld::ZoneInstance::townHand, = TownPlacement::hand), 0 = none.
	std::uint64_t townHand = 0;
	// The BUILDING record has an `interior` (the original's b+0x1f0: only such a building, or a gate, takes a resident template).
	bool hasInterior = false;
	// Spawned as a ruin (`kworld::placementRuined`): its interior is not built, so its layout furniture is not there either.
	bool ruined = false;
	// The placement is itself a cage (its own functionality is function 8, a Peeler Machine or a prisoner post: interior furniture a zone
	// places on its own, no doors, no collision shapes): the site is one point, its Cage entry the furniture's use node (kenshi-cym.28).
	bool furnitureCage = false;
	// The BUILDING record's flags the indoors state and the weapon `indoors mod` read (indoors_state.md section 1 / 2): `cavernous`
	// (a lab, a cave: the mod is skipped) and `enforce ceiling` (every floor but the ground is under a ceiling, ray or no ray).
	bool cavernous = false;
	bool enforceCeiling = false;
	// The BUILDING record's `is shelter`: its roof stops acid rain for someone under it (weather_acid_rain.md section 5).
	bool shelter = false;
	double originX = 0.0; // the placement's x, z: building-local layout coordinates are relative to it
	double originZ = 0.0;
	double originY = 0.0; // the placement's y: layout heights are over it
	double cx = 0.0;
	double cz = 0.0;
	double axisX = 1.0; // the placement's local x axis on the plane (a yaw projection of the quaternion below)
	double axisZ = 0.0;
	// The placement's full rotation (w, x, y, z): a building on a slope is tilted, so its floor is too (the Hub bar's runs 1646.7 to
	// 1648.0 over three beds). Layout pieces are placed by it, not by a yaw alone (layoutToWorld below).
	double qw = 1.0;
	double qx = 0.0;
	double qy = 0.0;
	double qz = 0.0;
	double halfA = 0.0;
	double halfB = 0.0;
	double minY = 0.0;
	double maxY = 0.0;
	// The indoors trigger hull (the building's `interior mask` collision, INTERIORS.md): the same kind of oriented box about the
	// placement's own axis, only when the building has a mask. Membership (Simulation::buildingAt) tests this when present, the
	// footprint above otherwise.
	bool hull = false;
	double hullCx = 0.0;
	double hullCz = 0.0;
	double hullHalfA = 0.0;
	double hullHalfB = 0.0;
	double hullMinY = 0.0;
	double hullMaxY = 0.0;
	// The upper floors' walking surfaces, ascending (CHAR_DYNAMIC `floor`, SIMLINK.md section 6): the tops of the building's
	// interior-mask-clipped walkable shapes (the upper floors and their stairs; kworld::WorldShape::keepInside), one per storey.
	// The ground floor is not a shape: a height below the first entry is floor 0. A building whose mask clips nothing (no box in
	// it: Outpost s-IV's) takes them from its upper parts' horizontal faces instead (world.cpp, kenshi-k16.6.277). Derived, not saved.
	std::vector<double> floors;
	// Where the climb to floors[0] starts: originY, or the foot of the first flight when that lies well above the placement
	// (Outpost s-IV: the origin is 139 u under its entrance deck, so the midpoint to the first slab would put the deck on floor 1).
	double groundY = 0.0;

	// The floor index of a height in this building: 0 on the ground, 1 on the first upper floor, and so on. A climber is on
	// floor k from halfway up the stairs: y at or above the midpoint between surface k and the one below it (the placement's
	// y for floor 1), so the client's per-floor roof hiding turns over mid-flight, not at the top step (client playtest
	// 2026-11-05, item 6; UNSURE: the exe's own threshold). Never later than the surface less 6 u (a surface's height and
	// the shape's top differ by a few units).
	int floorAt(double y) const noexcept
	{
		static constexpr double kFloorTolerance = 6.0;
		int floor = 0;
		double below = groundY;
		for (const double surface : floors)
		{
			if (std::min(0.5 * (below + surface), surface - kFloorTolerance) <= y)
				++floor;
			below = surface;
		}
		return floor;
	}

	// Is the character under a ceiling at height `y` on `floor`? This is the exe's per-tick up ray (char+0xd5, indoors_state.md
	// section 1) without the physics query: a storey's walking surface above (floors), or the placement's own roof over the feet
	// (maxY within kRoofClearance), which covers a single-storey building and a storey the bake lost. A top walkable deck has
	// nothing above it but the sky, so it is outdoors. An `enforce ceiling` building is indoors on every floor but the ground,
	// whatever is overhead (the exe forces +0xd5 = 0 there). Call with floor == floorAt(y).
	bool indoorsAt(double y, int floor) const noexcept
	{
		if (enforceCeiling && floor != 0)
			return true;
		if (floor >= 0 && static_cast<std::size_t>(floor) < floors.size())
			return true;
		return maxY - y >= kRoofClearance;
	}
};

// A layout's building-local point in the world: the placement plus its full rotation of (x, y, hand * z), the transform the bake
// places furniture with (kworld::placeFurniture). `hand` (+1 / -1) is the mirror the layout handedness leaves open (routine_scorer
// furnishes with the one that keeps more pieces inside the footprint). The y is the piece's own: on a tilted placement an upstairs
// bed's height depends on where in the building it stands (the Hub bar's three: 1646.67 / 1647.34 / 1648.01).
std::array<double, 3> layoutToWorld(const BuildingSite& building, double hand, double x, double y, double z) noexcept;
// The same, xz only: origin + x * A + hand * z * B on the plane (the footprint tests).
std::array<double, 2> layoutToWorld(const BuildingSite& building, double hand, double x, double z) noexcept;
// (x, z) inside the building's footprint box.
bool insideFootprint(const BuildingSite& building, const std::array<double, 2>& at) noexcept;
// The hand that puts more of `local` (building-local x, z) inside the footprint; +1 on a tie.
double layoutHand(const BuildingSite& building, const std::vector<std::array<double, 2>>& local) noexcept;

// A placed building with its own inventory (BUILDING `has inventory`: crates, barrels, shelves, safes). `key` names the placement
// (the hash of its id, like a door's); x, y, z is the placement. Derived from the game data, not saved.
struct ContainerSite
{
	std::uint64_t key = 0;
	std::string recordSid;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	// The placement's `owner faction ID` (its local GAMESTATE_BUILDING): the box's owner, and the lock level's faction (as for
	// DoorSite::ownerFaction); "" = none (then the nearest town's).
	std::string ownerFaction;
	// The BUILDING record's `has lock`: the box starts locked. A container has no open / close, so its `always locks` adds nothing.
	bool locked = false;
};

// A placed ore deposit: a BUILDING with `building category` MINING and `is foliage` whose functionality has `world resource mining` 1
// (Iron Resource) or 3 (Copper Resource; prospect.hpp). x, z is the placement. Derived from the game data, not saved.
struct DepositSite
{
	std::uint8_t resource = 0; // 1 iron, 3 copper (the exe's resource ids)
	double x = 0.0;
	double z = 0.0;
};

// A BUILDING that produces with no inputs (producers.hpp): a zone placement (World::producerSites()) or a foliage instance whose
// FOLIAGE_MESH has a `building type` (the wild deposits; Simulation::stepProducers generates them per zone near the players). `id` is the
// producer entity's Id, stable for a site (producers.hpp). Derived from the game data, not saved.
struct ProducerSite
{
	std::uint64_t id = 0;
	std::string recordSid;
	std::string ownerFaction; // the placement's `owner faction ID`, "" = wild
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	// The xz bounds of its collision (a foliage deposit's rock); all 0 = none known (a zone placement: UNSURE, not read yet).
	// An operator also counts as arrived inside them x 1.2 (kinspect item59 MQ9).
	double boxMinX = 0.0;
	double boxMaxX = 0.0;
	double boxMinZ = 0.0;
	double boxMaxZ = 0.0;
};

// A placed production BUILDING of any class (economy.hpp, TOWN_LIFE_SLICE 7.3): a farm, a mine, a stone camp, a well, the NPC
// converter set. Every placement whose BUILDING has a functionality that `produces`, with or without `consumes`, but not a natural
// deposit (`is foliage`, the MINING rocks: ProducerSite's territory). Derived from the game data, not saved.
struct ProductionSite
{
	std::string recordSid;
	std::string ownerFaction; // the placement's `owner faction ID`, "" = none
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

// A placed disposal machine: a BUILDING whose functionality function is 11 ("Cooking pot" 2678 / "Corpse disposal" 4004; the
// buildings are Corpse Furnace 3962 and Cooking Pit 2673). 137's pot (TOWN_LIFE_SLICE 5.32). Most are doorless open structures, so
// they are not BuildingSites (a BuildingSite needs a non-door collision shape) and a separate list carries them. Derived from the game
// data, not saved.
struct DisposalSite
{
	std::uint64_t key = 0; // the placement's building key (the BuildingSite key scheme: kworld::doorKey(placementId)); the Routine's
						   // SiteKind::Disposal object
	std::string recordSid;
	std::string ownerFaction; // the placement's `owner faction ID`, "" = none
	std::string squadTemplate; // its `public day` / `public night` (the law / military derating reads it)
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

class World
{
public:
	World(World&&) noexcept = default;
	World& operator=(World&&) noexcept = default;
	World(const World&) = delete;
	World& operator=(const World&) = delete;
	~World() { queries_.clear(); } // the cached queries (each) go before the world

	// Reads the four game-data files in documented order, builds every
	// prefab and relationship, then places all zone building instances.
	// Throws kworld::Error / kdata::ParseError on bad input.
	static World load(std::string_view dataDir);

	// Same, from already merged game data; loadOrder names its files (saves
	// record it, docs/formats/RUNTIME_SAVE.md).
	static World build(const kdata::GameData& gameData, std::string_view dataDir,
		std::vector<std::string> loadOrder);

	// No templates, an empty load order: for tests of template-free state.
	static World empty();

	const flecs::world& ecs() const noexcept { return ecs_; }

	// The game-data file names the templates came from, in order.
	const std::vector<std::string>& loadOrder() const noexcept { return loadOrder_; }

	// Prefab by source stringID; a null entity when absent.
	flecs::entity find(std::string_view stringID) const;

	// The stringID -> prefab map (insertion order is not meaningful).
	const std::unordered_map<std::string, flecs::entity>& prefabs() const noexcept
	{
		return prefabs_;
	}

	// Every stored (from, to) faction relation, in load order.
	const std::vector<FactionRelationPair>& factionRelationPairs() const noexcept
	{
		return factionRelations_;
	}

	// Stored starting relation; nullopt means "not listed" (callers fall back
	// to Faction::defaultRelation, GAMEDATA_SCHEMA.md "FACTION").
	std::optional<std::int32_t> storedRelation(flecs::entity_t from,
		flecs::entity_t to) const;

	const WorldCounts& counts() const noexcept { return counts_; }

	// Every door of every placement, in placement order (doors.hpp turns them into town gates).
	const std::vector<DoorSite>& doorSites() const noexcept { return doorSites_; }
	const std::vector<ContainerSite>& containerSites() const noexcept { return containerSites_; }
	// Every placed iron and copper deposit, in placement order.
	const std::vector<DepositSite>& depositSites() const noexcept { return depositSites_; }
	// Every placed producer BUILDING (deposits, mines, ...), in placement order.
	const std::vector<ProducerSite>& producerSites() const noexcept { return producerSites_; }
	// Every placed BUILDING that produces anything (farm, mine, converter), in placement order: the economy's camps (7.3).
	const std::vector<ProductionSite>& productionSites() const noexcept { return productionSites_; }
	// Every placed disposal machine (functionality function 11: 137's pot), in placement order.
	const std::vector<DisposalSite>& disposalSites() const noexcept { return disposalSites_; }
	// The disposal machine with this key (DisposalSite::key), or null. A linear scan: the list is a few dozen.
	const DisposalSite* disposalSite(std::uint64_t key) const noexcept;
	// Every `is wall gate` placement with a door slab, in placement order.
	const std::vector<GateSite>& gateSites() const noexcept { return gateSites_; }
	// Every non-gate placement with doors and collision, in placement order.
	const std::vector<BuildingSite>& buildingSites() const noexcept { return buildingSites_; }
	// The placed cage buildings (function 8: a Peeler Machine, a prisoner post) that are furniture, not a building: one-point sites, kept apart from
	// buildingSites() so no building consumer (town populate, indoors, beds) sees them; buildingSite(key) and the cage finders do (kenshi-cym.28).
	const std::vector<BuildingSite>& furnitureCageSites() const noexcept { return furnitureCages_; }
	// Every surviving building placement, in zone order (the LOS hulls read the collision of a region's buildings from it).
	const std::vector<kworld::PlacedInstance>& placements() const noexcept { return placements_; }
	// Adds a placement the zone files of the loaded data do not carry (a save's runtime-spawned furniture, a test's): when its BUILDING is a cage
	// itself (function 8: a Peeler Machine, a prisoner post) it becomes a one-point site, like a zone's own. False for any other building.
	bool addFurnitureCage(const kdata::GameData& gameData, const kworld::PlacedInstance& instance);
	// The site with this key (BuildingSite::key), or null: the placement key Simulation::buildingAt and Fighter::building use.
	// A map over buildingSites_, built on first use.
	const BuildingSite* buildingSite(std::uint64_t buildingKey) const;

	// The exe's vf+0x290 for a placed building: "open to public", i.e. the owning SQUAD_TEMPLATE's `public day`
	// (the exe never reads `public night`; kinspect findings/dialogue_pacing). The exe forces it false for a
	// building of type 1 / 0x22 while the squad's "house locked" flag is set, so a caller that already tests
	// houseLocked need not repeat that. False for a key the world does not place, and for a placement with no
	// squad template.
	// `untemplated`: the answer for a building with no squad template (`public day` defaults true in the data; the dialogue's SHOO reads it so, a guard
	// post's intruder hunt keeps the exe's false for it).
	bool openToPublic(std::uint64_t buildingKey, bool untemplated = false) const;

	double loadSeconds() const noexcept { return loadSeconds_; }
	const std::string& dataDir() const noexcept { return dataDir_; }

	// `each` over a CACHED query (ARCHITECTURE "Language and stack", per-tick queries): the same call shape as `ecs().each(fn)`, the
	// query built from the lambda's own parameters on first use and kept for the life of the world (a load changes entities, never
	// the world, so the query stays valid). An `ecs.each` builds and walks an uncached query every call. Do not make the FIRST call
	// of a shape from inside another iteration (flecs refuses to create a query while the world is read-only).
	template <class Fn>
	void each(Fn&& fn) const
	{
		cachedQuery<typename EachQuery<std::decay_t<Fn>>::type>().each(std::forward<Fn>(fn));
	}

	// The members of `squad` (the characters whose MemberOf target it is): the tables of the (MemberOf, squad) pair, looked up by id
	// (a query with the squad as a variable would scan every squad's table; a query built per call costs more than the walk).
	template <class Fn>
	void eachMember(flecs::entity squad, Fn&& fn) const
	{
		ecs_.each(ecs_pair(ecs_.id<state::MemberOf>(), squad.id()), [&](flecs::entity member)
		{
			if (const state::Id* id = member.get<state::Id>())
				fn(member, *id);
		});
	}

private:
	World() = default;

	template <class... A> struct EachArgs { using type = flecs::query<std::remove_reference_t<A>...>; };
	template <class... A> struct EachArgs<flecs::entity, A...> { using type = flecs::query<std::remove_reference_t<A>...>; };
	template <class F> struct EachQuery : EachQuery<decltype(&F::operator())> {};
	template <class R, class C, class... A> struct EachQuery<R (C::*)(A...) const> : EachArgs<A...> {};

	template <class Q>
	Q& cachedQuery() const
	{
		const std::type_index key(typeid(Q));
		auto found = queries_.find(key);
		if (found == queries_.end())
			found = queries_.emplace(key, std::make_shared<Q>(buildQuery<Q>())).first;
		return *static_cast<Q*>(found->second.get());
	}
	template <class... T>
	static flecs::query<T...> buildQuery(const flecs::world& ecs, flecs::query<T...>*)
	{
		return const_cast<flecs::world&>(ecs).query<T...>(); // building a query registers it; no entity changes
	}
	template <class Q>
	Q buildQuery() const { return buildQuery(ecs_, static_cast<Q*>(nullptr)); }

	flecs::world& ecsMutable() noexcept { return ecs_; }
	friend class Simulation;

	void buildPrefabs(const kdata::GameData& gameData);
	void buildRelationships(const kdata::GameData& gameData);
	void placeBuildings(const kdata::GameData& gameData, std::string_view dataDir);
	void finishCounts();

	// The cached queries of `each`. Declared BEFORE ecs_ so a move-assignment frees the old queries while the old world is alive; the
	// destructor clears them first for the same reason.
	mutable std::unordered_map<std::type_index, std::shared_ptr<void>> queries_;
	flecs::world ecs_;
	std::unordered_map<std::string, flecs::entity> prefabs_;
	std::vector<FactionRelationPair> factionRelations_;
	WorldCounts counts_{};
	std::vector<DoorSite> doorSites_;
	std::vector<ContainerSite> containerSites_;
	std::vector<DepositSite> depositSites_;
	std::vector<ProducerSite> producerSites_;
	std::vector<ProductionSite> productionSites_;
	std::vector<DisposalSite> disposalSites_;
	std::vector<GateSite> gateSites_;
	std::vector<BuildingSite> buildingSites_;
	std::vector<BuildingSite> furnitureCages_;
	std::vector<kworld::PlacedInstance> placements_;
	mutable std::unordered_map<std::uint64_t, std::uint32_t> buildingSiteByKey_; // built on first use (buildingSite)
	mutable bool buildingSiteByKeyValid_ = false;
	std::string dataDir_;
	std::vector<std::string> loadOrder_;
	double loadSeconds_ = 0.0;
};

} // namespace ksim
