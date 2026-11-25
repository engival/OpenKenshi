#pragma once

// The simulation's view of coarse navigation: where squads may go and the
// route between two ground points (ARCHITECTURE.md "Pathfinding": at Squad
// and Strategic level squads move along the coarse region/portal graph, so a
// position is always on walkable ground).
//
// The navigator is derived data (a bake of the world plus a graph over it),
// never saved: the loader configures the same navigator again, as it does the
// load order. Queries are const and deterministic. It is used from the serial
// squad step only (simulation.hpp), so an implementation need not be thread
// safe.

#include "ksim/combat/body.hpp"
#include "ksim/core/profile.hpp"
#include "ksim/core/state.hpp"

#include "knav/door.hpp"
#include "knav/navmesh.hpp"
#include "knav/route.hpp"
#include "knav/walk.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/coords.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {
class AreaGatherer;
class Heightmap;
class NavHashSource;
}

namespace ksim {

struct Route
{
	bool found = false;
	// OffMesh, NoRoute or OutsideWindow when not found.
	state::AiReason failure = state::AiReason::None;
	// The check that refused it, for the server log (a literal; empty when found or when no check named itself): which of
	// "no poly near the goal", "goal in another island", "search closed", "out of nodes" ... (kenshi-k16.6.260).
	std::string_view check{};
	// A search with a body radius failed and the radius-0 search fails too (the bare mesh does not reach the goal): no retry at 0 helps.
	bool plainFailed = false;
	// Walkable ground, start to end. The first point is the walkable point
	// nearest the requested start, the last the one nearest the requested end.
	std::vector<state::GroundPoint> points{};
	// The surface height of each point, parallel to `points`; empty when the navigator does not
	// know heights (only MeshNavigator fills it).
	std::vector<double> heights{};
	// MeshNavigator::routeFrom: the route ends at an approach point (the clicked spot was off the
	// mesh, or only a partial path leads towards it), not at the clicked spot itself.
	bool approach = false;
	// MeshNavigator::routeFrom: the start was off the mesh or in a connected set with no way to the goal, and `points` begin at the
	// nearest ground of the goal's set instead (PATHFINDING_STATIC 1.11: the search for the nearest ground widens 1000 u, then 10000 u).
	// The caller moves the character onto points[0] first.
	bool reseated = false;
	// combat's routeForBody: this route is the radius-0 retry after the body's own search failed (kenshi-k16.7.62).
	bool radiusFallback = false;
	// MeshNavigator::routeFarFrom: the goal's zone is not loaded like the walker's, so the route ends at the exit of the walker's own zone
	// along the line to the goal (rwh208 section 2), not at the goal: the owner asks again from that end (LOCOMOTION.md D9).
	bool clamped = false;
};

// Who is asking, as far as gates are concerned. Doors have no class, faction or owner test (as in the original): only a
// locked door refuses, and a basher goes through it.
struct Traveller
{
	// A raid that bashes its way through: gates are passable whatever their state (the sets and the coarse
	// graph see every door open). The caller adds the time the bashing takes (doors.hpp, DoorAccess::bashing).
	bool bash = false;
	// The traveller's body: the routing clearance radius (CombatData::pathRadius, NAVMESH_CACHE.md "Clearance").
	// 0 = no body (a squad token at Strategic level); the local route then asks for no clearance, and the coarse graph
	// does not apply one yet either way.
	float footprint = 0.0f;
};

#if KSIM_PROFILE
struct NavStats;
#endif

class Navigator
{
public:
	virtual ~Navigator() = default;
#if KSIM_PROFILE
	// The query counters, when this navigator keeps them (the tick profiler reads their running totals); null otherwise.
	virtual const NavStats* profileStats() const noexcept { return nullptr; }
#endif

	// The queries below for a given traveller. The defaults ignore the traveller, which is right
	// for a navigator without doors (the straight-line doubles of the tests).
	virtual Route routeFor(const Traveller&, double fromX, double fromZ, double toX, double toZ) const
	{
		return route(fromX, fromZ, toX, toZ);
	}
	virtual Route localRouteFor(const Traveller&, double fromX, double fromZ, double toX,
		double toZ) const
	{
		return localRoute(fromX, fromZ, toX, toZ);
	}
	// A far goal (its zone not loaded) is walked one zone at a time: the route ends at the exit of the walker's zone (clamped), the caller asks again
	// from there (MeshNavigator::routeFarFrom, kenshi-k16.6.482). The default is routeFor.
	// The portal corridor of a final route's points (PATH_WALK.md 3a): empty = none known (setRoute then lays a point portal at every point).
	virtual std::vector<state::RoutePortal> corridorFor(const std::vector<state::RoutePoint>&, Body) const { return {}; }
	// The doors a corridor crosses that are not locked now: a walker on this route keeps going through them when they lock (rwh.231 c3a, rwh.244;
	// the lock refuses a plan, not a walk under way). A door locked at the plan is not in it: the walker stops there as before.
	virtual std::vector<std::uint64_t> openDoorsOn(const std::vector<state::RoutePortal>&) const { return {}; }
	virtual Route farRouteFor(const Traveller& who, double fromX, double fromZ, double toX, double toZ) const
	{
		return routeFor(who, fromX, fromZ, toX, toZ);
	}
	virtual std::optional<state::GroundPoint> nearestReachableFor(const Traveller&, double x,
		double z, double reach, double fromX, double fromZ) const
	{
		return nearestReachable(x, z, reach, fromX, fromZ);
	}
	// Makes `book` the state of every door (the gates of doors.hpp): at a tick boundary, before
	// the next query. The queries are const, so is this; the state it changes is the
	// navigator's door view, not anything saved.
	virtual void applyDoors(const knav::DoorBook&) const
	{
	}

	// True when routes may start or end at (x, z): its mesh tiles are loaded.
	virtual bool covers(double x, double z) const = 0;

	virtual Route route(double fromX, double fromZ, double toX, double toZ) const = 0;

	// As route, for a goal near its start (a patrol leg, a territory walk): an
	// implementation may answer from a cheaper bounded search. The default is route.
	virtual Route localRoute(double fromX, double fromZ, double toX, double toZ) const
	{
		return route(fromX, fromZ, toX, toZ);
	}

	// The walkable ground nearest (x, z) within `reach` horizontally: dry land
	// if there is any in reach, otherwise any walkable surface (the sea floor,
	// for swimmers). Empty when there is none, or (x, z) is outside the
	// window. The default is for test doubles: a covered point is walkable.
	// `radius` above 0 keeps a body that wide: only ground where it fits (a point at least `radius` from every wall).
	virtual std::optional<state::GroundPoint> nearestWalkable(double x, double z,
		double reach, double radius = 0.0) const;

	// As nearestWalkable, but only ground connected to the walkable ground at (fromX, fromZ), so a
	// walk from there to the answer cannot fail for want of a connection. Empty when none is in
	// reach. The default is nearestWalkable.
	virtual std::optional<state::GroundPoint> nearestReachable(double x, double z, double reach,
		double fromX, double fromZ) const;
};

// Calls and wall time of the navigator's queries (metrics, not state).
struct NavStats
{
	std::uint64_t routeCalls = 0; // coarse graph
	std::uint64_t routeFound = 0;
	std::uint64_t routeNs = 0;
	// routeFrom's failures by knav::PathResult::Failure (index 0 unused), the polygons its clearance searches closed, and the
	// radius-0 retries combat's routeForBody made after a body route failed (kenshi-k16.7.62).
	std::array<std::uint64_t, 6> routeFailures{};
	std::uint64_t routeClearanceNodes = 0;
	std::uint64_t routeNegativeHits = 0; // routeFrom calls answered from the no-route cache (kenshi-k16.6.627)
	std::uint64_t radiusZeroRetries = 0;
	std::uint64_t localCalls = 0; // bounded Detour search
	std::uint64_t localFound = 0;
	std::uint64_t localNs = 0;
	std::uint64_t crossCalls = 0; // local goals over a region border (window mesh)
	std::uint64_t crossFound = 0;
	std::uint64_t wideRetries = 0; // closed searches retried in a wider window
	std::uint64_t wideFound = 0;
	std::uint64_t crossNs = 0; // ... wall time including building the window
	// Local walks that failed, by knav::PathResult::Failure (index 0 unused): start off mesh, end
	// off mesh, no path, search closed (end in another connected set), node pool exhausted.
	std::array<std::uint64_t, 6> localFailures{};
	std::array<std::uint64_t, 6> crossFailures{}; // ... of which over a region border
	std::uint64_t snapCalls = 0;
	std::uint64_t snapNs = 0;
	std::uint64_t snapSearches = 0; // reachable snaps that needed the full polygon search
	// Doors: applyDoors calls, coarse routes that failed only because a gate refused the
	// traveller (Route::failure DoorRefused), and polygons skipped by a goal pick because a
	// gate would have to be passed.
	std::uint64_t doorApplies = 0;
	std::uint64_t doorRefusedRoutes = 0;
	std::uint64_t routeSetRefusals = 0; // coarse routes answered by the connected sets, no graph search
	std::uint64_t doorSkippedPolys = 0;
	std::uint64_t doorApplyNs = 0;
};

// The native loader's disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache"): the converted shipped regions and our zone
// rebakes, kept between runs in an XDG cache tree. Entries are immutable and content-addressed (a temp file, then a rename); a
// corrupt one is deleted, logged and rebuilt, never a crash and never a silent fallback (MeshNavigator::openNative).
struct NativeCacheOptions
{
	// The cache root. Empty: nativeCacheRoot() (KENSHI_NAV_CACHE, $XDG_CACHE_HOME, %LOCALAPPDATA%, %APPDATA%, then ~/.cache).
	std::string dir;
	// false: no cache at all (ksim_server --no-nav-cache): nothing is read or written.
	bool enabled = true;
	// The load order's content hash (kworld::dataHash), the tree's world key: one tree per mod set, order and game data.
	// 0 = no key, so no tree: the cache stays off (a test or a tool that has no load order).
	std::uint64_t worldKey = 0;
	// The bake sources' hash in the tree name (KNAV_BAKE_SRC_HASH, kenshi-k16.7.161). Empty = the one compiled in; a test injects one.
	std::string bakeSrcHash;
	// Trees kept under the root, the newest by mtime (the tree in use always stays). 0 = the default (3: a few mod sets or recipes).
	// A test's private cache dir sets 1: every recipe or bake-source change otherwise leaves a dead ~3.4 GB tree (kenshi-k16.6.814).
	std::size_t keepTrees = 0;
};
// The default cache root from an environment lookup (getenv-like, null = unset): KENSHI_NAV_CACHE, else
// $XDG_CACHE_HOME/kenshi-sim/navmesh, else %LOCALAPPDATA%/kenshi-sim/navmesh, else %APPDATA%/..., else
// $HOME/.cache/kenshi-sim/navmesh. Empty when none is set. (The Windows variables come before HOME: under Wine/Proton HOME is a
// Unix path.) The lookup is a parameter so a test fakes the environment.
std::string nativeCacheRoot(const std::function<const char*(const char*)>& env);

struct NativeCacheStats
{
	std::size_t regionHits = 0; // a converted region read from the cache
	std::size_t regionMisses = 0; // converted and written
	std::size_t zoneHits = 0; // a zone rebake read from the cache
	std::size_t zoneMisses = 0; // rebaked and written
	std::size_t filesWritten = 0;
	std::size_t bytesWritten = 0;
	std::size_t loadFailures = 0; // corrupt, truncated or another version: deleted, logged and rebuilt
	// Where the time went (the startup stage line): hashing the shipped tiles for the region keys, reading cache entries,
	// and converting / rebaking on a miss (the write included).
	double keySeconds = 0, readSeconds = 0, convertSeconds = 0;
	std::size_t keyFiles = 0, keyBytes = 0, readFiles = 0, readBytes = 0;
};

// One navmesh (a bake of a few zones, or a `knav_bake --out` cache file) and the movement step on
// it: what characters at full detail need (docs/formats/SIMLINK.md section 12). Routes are one
// Detour search over the whole mesh, so the local/long split of KnavNavigator does not exist here:
// route and localRoute are the same. Not thread safe (one query state); the sim uses it serially.
class MeshNavigator final : public Navigator
{
public:
	explicit MeshNavigator(knav::NavMesh mesh);
	~MeshNavigator() override; // out of line: the world-data members are forward-declared
	// Bakes the zone cells min..max inclusive from the game data (the Hub zone takes ~3 s).
	static std::shared_ptr<MeshNavigator> bake(const std::string& dataDir, kworld::ZoneCell min,
		kworld::ZoneCell max, int threads);
	// Reads a `knav_bake --out` cache file.
	static std::shared_ptr<MeshNavigator> load(const std::string& path, const std::string& dataDir = {});

	bool covers(double x, double z) const override;
	Route route(double fromX, double fromZ, double toX, double toZ) const override;
	std::vector<state::RoutePortal> corridorFor(const std::vector<state::RoutePoint>& points, Body body) const override;
	std::vector<std::uint64_t> openDoorsOn(const std::vector<state::RoutePortal>& portals) const override;
	Route farRouteFor(const Traveller& who, double fromX, double fromZ, double toX, double toZ) const override;
	std::optional<state::GroundPoint> nearestWalkable(double x, double z, double reach, double radius = 0.0) const override;
	// The free run beyond (x, z) at height y along the unit direction (dirX, dirZ): how far a body of `radius` can go on before it stops fitting (a wall within
	// `radius` of the point, or no ground), probed every kRoomStep up to `limit`. The room a corner's arc has to overshoot into (walk.hpp speedLimit).
	double roomAhead(double x, double y, double z, double dirX, double dirZ, double radius, double limit) const;
#if KSIM_PROFILE
	const NavStats* profileStats() const noexcept override { return &stats_; }
#endif
	// The route counters, kept in every build (kenshi-k16.7.62).
	const NavStats& routeStats() const noexcept { return stats_; }
	void noteRadiusZeroRetry() const noexcept { ++stats_.radiusZeroRetries; }

	// The route from a character standing at `from` (y included, so a deck and the ground under
	// it are told apart) to the walkable point nearest the click (toX, toY, toZ). Heights filled.
	// With `toY` (the click's surface height, SIMLINK move_to yValid) the snap is the original's, in 3D:
	// the closest walkable point within kClickReach of the click point. Without it the distance is
	// horizontal and any height counts. A click off the mesh snaps ("walk as close as you can": click a
	// rock and you walk up to it); `approach` is set when the route does not end on the clicked spot.
	// The original accepts only a complete path (MOVE_CLICK_LIVE.md): a goal in another connected area,
	// or one the search cannot reach, is not found (`no_route`), never a walk to the closest reachable point.
	// `planned`: the route of a character that plans its own door steps (DOORS.md section 6): locked doors are passable
	// in the search, because the owner unlocks them and others bash them (interact.hpp). A player's move order is not
	// planned: a locked door is a wall to it.
	// `body`: the character's clearance radius (CombatData::pathRadius; NAVMESH_CACHE.md "Clearance"): the route crosses
	// only gaps a body that wide fits through and both ends are pushed off the walls. Every character route passes it;
	// 0 is Detour's raw search, for a route no body walks (a campaign's travel estimate).
	// `straighten`: a short route (under 300 u) keeps to the straight line over big polygons instead of Detour's portal middles
	// (TravelFilter::straighten; NAVMESH_CACHE.md "Node placement"). On by default: the player's click, AI, follow and chase routes alike;
	// a route over 300 u keeps Detour's placement (speed), radius 0 has none.
	// `clickGoal`: the player's click without a height (the client sends one only from its heightmap): the goal is the ground nearest the click in xz at
	// ITS height, not the floor level with the walker; internal callers that know the height leave it off (kenshi-k16.8.10).
	Route routeFrom(const knav::Position& from, double toX, double toZ, std::optional<double> toY = std::nullopt,
		bool planned = false, Body body = {}, bool straighten = true, bool clickGoal = false) const;
	// Far goals (LOCOMOTION.md D9, rwh208 section 2). A zone is loaded when the mesh holds tiles of it (a streaming mesh: it is in the coverage).
	bool zoneLoaded(kworld::ZoneCell zone) const;
	// True when the goal lies in another zone than (fromX, fromZ) and that zone is not loaded: routeFarFrom clamps it.
	bool farGoal(double fromX, double fromZ, double toX, double toZ) const;
	// kZoneMargin: the loaded zone's box is grown by this (the exe's navsys+0x1d4, value not read: ours).
	static constexpr double kZoneMargin = 50.0;
	// routeFrom, except that a far goal is replaced by the exit of the walker's zone along the line walker -> goal (box grown by kZoneMargin, snapped
	// to the mesh like any goal), `clamped` set on the route. The exit that gives no progress (a snap back over the border, no ground) widens the
	// margin and tries sideways offsets along the crossed axis (the exe's ladder). Not found only when no exit has ground.
	Route routeFarFrom(const knav::Position& from, double toX, double toZ, std::optional<double> toY = std::nullopt,
		bool planned = false, Body body = {}, bool straighten = true, bool clickGoal = false) const;
	// The talk / trade approach (docs/FEATURES.md "Talk / trade approach stops on the near side of a counter"): picks the GOAL first.
	// Candidates are navmesh points on rings round the target (tx, ty, tz) within `reach` (xz; the stopped reach) that have a clear
	// line to it (lineClear), each pathed with routeFrom; the one with the shortest path from `from` wins. Not found (found false)
	// when no candidate has a route: the caller falls back to routeFrom to the target itself.
	// `body` as routeFrom: passed to every candidate's path.
	Route routeToStand(const knav::Position& from, double tx, double ty, double tz, double reach, bool planned = false,
		Body body = {}) const;
	// True when a character at (ax, ay, az) can talk across to (tx, ty, tz): the straight line is on the mesh, or leaves it only for
	// a stretch of at most kThinObstacle (a counter, a table: the mesh is inset by the agent radius) AND the walk between the two
	// is a detour of the kind a free-standing obstacle makes (at most kDetourRatio x the straight line + kDetourSlack). A wall
	// between rooms has the same thin gap but its way round is through a door, far longer, so it blocks.
	// UNSURE: no wall / furniture classification exists in the navmesh; this is the stand-in.
	bool lineClear(double ax, double ay, double az, double tx, double ty, double tz) const;
	// True when the straight leg (ax, ay, az) -> (tx, ty, tz) walks on the mesh with no hole in it: the original's melee line check, the
	// navmesh-only straight walk behind cc+0x2c0 (kenshi-rwh.186 / .187). A table's cut (the furniture ring) breaks it, a wall does too; no
	// body clearance, no detour allowance (lineClear's talk rule is the opposite). Not a collision test: a table never blocks sight or a shot.
	bool straightWalk(double ax, double ay, double az, double tx, double ty, double tz) const;
	static constexpr double kThinObstacle = 40.0;
	static constexpr double kDetourRatio = 3.0;
	static constexpr double kDetourSlack = 60.0;

	// How far from the mesh a clicked destination may be and still be routed to its nearest point.
	// MEASURED (docs/reference/MOVE_CLICK_LIVE.md): the original finds a point at 347 u (diagonal) and 330 u
	// (vertical) and refuses 354 / 340: a 335-347 bracket; 340 is the pick.
	static constexpr double kClickReach = knav::kClickReach;
	// A route end this close (xz) to the click counts as the clicked spot, not an approach point.
	static constexpr double kClickExact = knav::kClickExact;
	// How far from the mesh a walker's own start may be and still be snapped on (route()).
	static constexpr double kEndReach = knav::kEndReach;

	// Paints the door polygons of the mesh from `book` (knav::NavMesh::applyDoors): a closed door stays passable at the
	// DoorClosed cost, a locked one is excluded from every search and slide. Changed tiles get a new tileVersion and a patch
	// for the tile stream. Call at a tick boundary (Simulation::syncDoorPaint does).
	void applyDoors(const knav::DoorBook& book) const override
	{
		mesh_.applyDoors(book);
		++doorPaints_;
	}
	// Door books applied so far: a changed door changes routability (a shut gate fails routeFrom), so a failed-route ladder started before it
	// is stale (Simulation::routeRetry, LOCOMOTION.md 9 D8).
	std::uint64_t doorPaints() const noexcept { return doorPaints_; }

	// Swaps navmesh tiles in place (a tile patch); the revision change drops the no-route cache.
	void replaceTiles(const std::vector<knav::TileCoord>& removed, std::vector<knav::BakedTile> added) const { mesh_.replaceTiles(removed, std::move(added)); }
	const NavStats& stats() const noexcept { return stats_; }
	const knav::NavMesh& mesh() const noexcept { return mesh_; }
	const knav::Walker& walker() const noexcept { return walker_; }

	// ---- Streaming coverage (docs/formats/NAVMESH_CACHE.md "Streaming coverage") ----
	// A mesh over a `knav_bake --world DIR` bake that holds only the zones it is told to (setCoverage): the sim follows the
	// players with it (Simulation::syncNavCoverage). Starts with `initial` loaded. Throws when DIR has no region file for them.
	// `dataDir` is unused by the walker now (no terrain snap: the navmesh point is the y, k16.8.31).
	static std::shared_ptr<MeshNavigator> openWorld(const std::string& dir, const std::vector<kworld::ZoneCell>& initial,
		const std::string& dataDir = {});
	const NativeCacheStats& nativeCacheStats() const noexcept { return cacheStats_; }
	// openNative: the resolved cache tree (empty = off).
	const std::string& nativeCacheDir() const noexcept { return cacheTree_; }
	// The one line per open: the hits and misses and the bytes written, so a cold start is visible.
	void logNativeCache() const;
	// The cache root keeps only the newest `keep` recipe trees (direct children named w<digits>_<hex>, by mtime; the tree in use is
	// touched first, so it is always among them). The others are removed whole and logged, one line each; nothing outside `root`
	// is touched. Called by openNative (kNativeCacheKeep); public for the test. Returns the removed names.
	static std::vector<std::string> pruneCacheRoot(const std::string& root, const std::string& inUse, std::size_t keep,
		const std::function<void(const std::string&)>& log = {});

	// The same coverage over the game's SHIPPED navtiles instead of a bake: each region is converted to Detour tiles in memory
	// the first time a zone of it is wanted (the knav_native conversion, NAVMESH_CACHE.md "Runtime mesh"). A converted region is
	// kept while a zone of it is resident (a returning zone is instant) and for the two most recently used regions besides (a step
	// back across a border does not convert again); the rest are dropped, so a long walk does not grow the cache without bound.
	// Starts with `initial` loaded; throws without the navtiles in dataDir.
	// `gameData` is the sim's own merged data (it loaded one anyway): the stale check and the rebakes read that instead
	// of a second copy, which costs the process one whole game data set less. Null: the loader loads its own. Either
	// way it must live as long as the navigator.
	static std::shared_ptr<MeshNavigator> openNative(const std::string& dataDir, const std::vector<kworld::ZoneCell>& initial,
		std::function<void(const std::string&)> log = {}, const kdata::GameData* gameData = nullptr,
		NativeCacheOptions cache = NativeCacheOptions{});

	// ---- The original's stale-tile check (docs/formats/NAVMESH_CACHE.md "The stale-tile check in the loader") ----
	// A shipped tile is a cache: its zone and each interior carry a hash of the world they were baked from. A zone
	// whose recomputed hash differs from the stored one is rebaked with our recipe, its shipped world mesh and all its
	// interiors left out; in a fresh zone a stale interior alone is rebaked, the shipped zone mesh kept. A unit whose
	// hash cannot be verified (no stored hash, an unpaired uid, an interior whose children's gather order we do not
	// reproduce) is rebaked too and counted apart. A rebake is our bake's tiles at the zone's (or the interior's) tile
	// square, clipped to the same 256 u borders (kworld/navhash_source.hpp).
	enum class NativeChoice
	{
		Shipped, // the stored hash matched
		Rebaked, // the stored hash differed
		NotComputable, // no usable hash on either side: rebaked unverified
	};
	struct NativeUnitReport
	{
		NativeChoice choice = NativeChoice::Shipped;
		std::string reason; // empty when shipped; otherwise the hash verdict or why it could not be computed
		std::optional<std::uint32_t> stored;
		std::optional<std::uint32_t> ours;
	};
	struct NativeInteriorReport
	{
		std::uint64_t uid = 0; // the tile Info's hex id
		NativeUnitReport decision;
	};
	struct NativeTileReport
	{
		kworld::ZoneCell zone{};
		NativeUnitReport decision;
		std::vector<NativeInteriorReport> interiors; // as shipped in the tile, in file order
		// The tiles this decision replaced with our bake: the zone's whole square for a rebaked zone, the interior's own
		// square in a fresh one. A polygon in one of them is ours, not the original's (ksim_nav_islands' source column).
		std::vector<knav::TileCoord> rebaked;
		// The tiles of a stale unit whose shipped mesh agrees with our rebake (knav/navgeom.hpp): the shipped tile stays, our bake is
		// dropped (kenshi-k16.7.128). Never in `rebaked`.
		std::vector<knav::TileCoord> agreed;
	};
	struct NativeStaleStats
	{
		std::size_t zonesShipped = 0;
		std::size_t zonesForestOnly = 0; // counted in zonesShipped too: the hash differs by the forest word alone, the shipped tile is kept
		std::size_t zonesRebaked = 0; // a stale zone hash
		std::size_t zonesNotComputable = 0; // rebaked without a verdict
		std::size_t interiorsShipped = 0;
		std::size_t interiorsRebaked = 0; // a stale interior hash, in a fresh zone
		std::size_t interiorsNotComputable = 0;
		std::size_t interiorChildOrder = 0; // ... of the not computable: children whose gather order is unknown
		std::size_t interiorUnpaired = 0; // ... no placement for the uid
		std::size_t interiorUnjoined = 0; // ... a layout not in interiors.level
		std::size_t tilesAgreed = 0; // tiles of stale units that kept the shipped mesh (the rebake agreed with it)
		std::size_t tilesRebaked = 0; // tiles of stale units that took our rebake
		std::size_t zonesScanned = 0; // zones decided so far (lazily: a zone is decided when a coverage wants it)
		double checkMs = 0.0; // the hashes
		double bakeMs = 0.0; // the rebakes
		std::size_t buildingsGenerated = 0; // the surface generator's buildings (zone bakes, a zone counts each time it is baked)
		std::size_t buildingsFailed = 0; // the generator threw: left to Recast
		std::size_t tilesAssembled = 0;
		std::vector<std::string> tilesFellBack; // "zone x,z tile x,z": the assembly failed, the tile is Recast with every shape
	};
	// The decision on a zone from the last conversion that saw it (nullptr when it has not been decided).
	const NativeTileReport* nativeReport(kworld::ZoneCell zone) const;
	const NativeStaleStats& nativeStaleStats() const noexcept { return staleStats_; }

	bool streaming() const noexcept { return !worldDir_.empty() || nativeSource_; }
	// The zones come from the shipped navtiles (openNative), not from a world bake.
	bool native() const noexcept { return nativeSource_; }
	// openNative: the regions (rx, rz) whose converted tiles are held now (the loader's own test/probe view).
	std::vector<std::pair<int, int>> nativeCachedRegions() const;
	// Most zones resident at once: kMaxResidentTiles / (18 x 18 tiles per zone).
	static constexpr std::size_t kMaxZones = 25;
	// What one setCoverage call did.
	struct CoverageChange
	{
		std::size_t added = 0; // zones loaded
		std::size_t removed = 0; // zones dropped
		std::size_t tilesAdded = 0;
		std::size_t tilesRemoved = 0;
		std::size_t overflow = 0; // wanted zones left out for kMaxZones
		double readMs = 0.0; // obtaining the new tiles: reading the region files, or converting the shipped navtiles
		double applyMs = 0.0; // NavMesh::replaceTiles (Detour tiles, links, door index and paint)
		bool changed() const noexcept { return added != 0 || removed != 0; }
	};
	// Makes the resident zones exactly `wanted` (the first kMaxZones of it, in the caller's priority order; zones outside the
	// world or without shipped tiles are skipped): tiles of dropped zones leave, tiles of new zones join and link across the
	// seams. Door paint is kept (NavMesh::replaceTiles re-applies the book). Tile versions of changed tiles go up by one, so the
	// client's tile stream sends the new ones and removes the dropped ones. At a tick boundary only, like applyDoors; const for
	// the same reason (the mesh is the navigator's own view). Not streaming: does nothing.
	CoverageChange setCoverage(const std::vector<kworld::ZoneCell>& wanted) const;
	// The resident zones, ascending by zone index.
	std::vector<kworld::ZoneCell> coveredZones() const;
	// A region file exists for the zone: it can be covered.
	bool canCover(kworld::ZoneCell zone) const;
	// Totals over all setCoverage calls.
	const CoverageChange& coverageTotals() const noexcept { return totals_; }

	// The nav cache's recipe number, and a digest of one zone's fresh bake (the canary test pins the two together:
	// tests/ksim_nav_cache_canary.cpp). Needs openNative.
	static std::uint32_t nativeCacheRecipe();
	std::uint64_t bakedZoneDigest(kworld::ZoneCell zone) const;
	// Our fresh bake of one zone's tiles, uncached (knav_stalegeom's calibration). Needs openNative.
	std::vector<knav::BakedTile> rebakeZone(kworld::ZoneCell zone) const { return bakeNativeZone(zone); }

	// A building the zone files do not carry (a save's player town, a construction): the one path that gives it collision and nav. Its
	// placement joins the gatherer, the zone is baked with our recipe and the tiles the building's shapes cover (clipped to its zone,
	// one tile of margin) replace the mesh's, now when the zone is resident and whenever it comes back. Needs openNative; false
	// when the mesh is not native or the placement has no collision. `tiles` (when given) receives how many tiles were replaced.
	bool standBuilding(const kworld::PlacedInstance& placed, std::size_t* tiles = nullptr) const;
	// Several at once: one zone gather and one bake per zone they fall in. Returns how many had collision; `tiles` the tiles replaced.
	std::size_t standBuildings(const std::vector<kworld::PlacedInstance>& placed, std::size_t* tiles = nullptr) const;
	std::size_t stoodBuildings() const noexcept { return stood_.size(); }

private:
	struct StoodBuilding
	{
		kworld::ZoneCell zone;
		std::vector<knav::TileRect> rects;
		std::vector<knav::BakedTile> tiles;
	};
	void applyStood(const std::vector<kworld::ZoneCell>& zones) const;
	mutable std::vector<StoodBuilding> stood_;

	// The stale check's inputs, loaded once by openNative (the gatherer loads the game data the audit and the rebake
	// both read; declaration order matters: the hash source refers into the gatherer, the gatherer into the terrain).
	void openNativeData();
	std::vector<knav::BakedTile> bakeNativeZone(kworld::ZoneCell zone) const;
	std::vector<knav::BakedTile> bakeNativeZoneFresh(kworld::ZoneCell zone) const;
	std::vector<knav::BakedTile> convertNativeRegion(int rx, int rz) const;
	bool applyStaleCheck(std::vector<knav::BakedTile>& tiles, kworld::ZoneCell zone) const;

	// The disk cache (docs/formats/NAVMESH_CACHE.md "Patch cache"): resolve the tree, key the entries, read and write them.
	void openNativeCache(const NativeCacheOptions& cache);
	std::uint64_t regionCacheKey(int rx, int rz) const;
	std::uint64_t zoneCacheKey(kworld::ZoneCell zone, const std::vector<std::uint64_t>& interiorUids) const;
	std::string cacheEntryPath(const char* kind, int x, int z, std::uint64_t key) const;
	std::optional<std::vector<knav::BakedTile>> readCacheEntry(const std::string& path, const knav::NavConfig& expected,
		const knav::TileRect& keep) const;
	bool writeCacheEntry(const std::string& path, const knav::NavConfig& config, const std::vector<knav::BakedTile>& tiles) const;

	std::string worldDir_; // the world bake (openWorld); empty otherwise
	std::string nativeDir_; // the navtiles directory (openNative); empty otherwise
	std::string dataDir_; // the game data directory (openNative: the stale check and its rebakes)
	const kdata::GameData* sharedGameData_ = nullptr; // openNative: the caller's game data (not owned)
	bool nativeSource_ = false;
	NativeCacheOptions cacheOptions_;
	std::string cacheTree_; // <root>/w<recipe>_<srcHash>_<worldKey> (openNative); empty = no cache
	mutable NativeCacheStats cacheStats_;
	std::unique_ptr<kworld::Heightmap> terrain_;
	std::unique_ptr<kworld::AreaGatherer> gatherer_;
	std::unique_ptr<kworld::NavHashSource> hashSource_;
	mutable std::vector<knav::Point> seedsDef_; // openNative: navtiles/seeds.def, read once
	std::function<void(const std::string&)> log_;
	mutable std::map<int, NativeTileReport> staleReports_; // openNative: by zone index, last conversion
	mutable NativeStaleStats staleStats_;
	mutable std::mutex bakeMemoMutex_;
	mutable std::vector<std::pair<kworld::ZoneCell, std::vector<knav::BakedTile>>> bakeMemo_; // the last bakeNativeZone results
	mutable std::map<int, std::vector<knav::BakedTile>> nativeRegions_; // openNative: the converted tiles of each region held now
	mutable std::vector<int> nativeRecent_; // ... in use order, most recent first (the small LRU beside the resident regions)
	mutable std::set<int> zones_; // resident zone indices (kworld::zoneIndexOf)
	mutable std::map<int, bool> fileOk_; // canCover answers (a stat per zone per tick would be waste)
	mutable CoverageChange totals_;
	// Mutable for applyDoors only: the paint is the navigator's door view, the tiles themselves never change.
	mutable knav::NavMesh mesh_;
	mutable std::uint64_t doorPaints_ = 0;
	mutable NavStats stats_; // routeFrom's calls and time, for the tick profiler; the failure counters run in every build
	Route routeFromUntimed(const knav::Position& from, double toX, double toZ, std::optional<double> toY, bool planned, Body body,
		bool straighten, bool clickGoal) const;
	// The no-route cache (routeFromUntimed): failed NoRoute plans by start poly, goal poly and body, dropped when the mesh revision changes.
	struct NoRouteKey
	{
		int sx, sz; int sp; int gx, gz; int gp; float radius, water; std::uint8_t mode;
		auto operator<=>(const NoRouteKey&) const = default;
	};
	struct NoRouteEntry { Route route; std::size_t pathFailure; };
	static constexpr std::size_t kNoRouteMax = 4096;
	static constexpr std::uint64_t kNoRouteLife = 20000; // route calls before the cache is dropped anyway
	mutable std::map<NoRouteKey, NoRouteEntry> negative_;
	mutable std::uint64_t negRevision_ = 0;
	mutable std::uint64_t negAge_ = 0;
	// The connected sets of the mesh for the planner (rebuilt lazily when the mesh revision changes).
	mutable knav::RouteSets routeSets_;
	knav::Walker walker_;
	knav::NavQuery query_;
};

// THE route installer (PATH_WALK.md 3a, kenshi-e9s9.4): every writer of a fighter's route calls this with the final points. Installs `points` as
// f.route with `next` as routeNext, derives the corridor from them (a point portal at every point when `nav` is null or knows no corridor) and puts
// portalNext in step. Nothing else about the route (version, flags, seq) changes: the caller keeps those. An empty list clears the corridor too.
void setRoute(state::Fighter& f, std::vector<state::RoutePoint> points, const Navigator* nav, Body body = {}, std::uint32_t next = 0);
// The corridor of f.route again (a load, a stale ref), keeping routeNext.
void rebuildCorridor(state::Fighter& f, const Navigator* nav, Body body = {});
// portalNext := the first portal whose route point is routeNext or later (the corridor stays in step with routeNext; cheap: a forward scan).
void syncPortalNext(state::Fighter& f) noexcept;

// knav navmesh regions of a `knav_bake --world DIR` bake plus the coarse graph
// built over them.
class KnavNavigator final : public Navigator
{
public:
	// Loads the bake regions rx0..rx1 x rz0..rz1 (inclusive, regions of 4 x 4
	// zones) from bakeDir and builds the graph on `threads` workers. The
	// terrain height (for the y of the route ends) comes from dataDir.
	static std::unique_ptr<KnavNavigator> open(const std::string& dataDir,
		const std::string& bakeDir, int rx0, int rz0, int rx1, int rz1, int threads);

	// As open, but the coarse graph is read from a `knav_graph` file instead of
	// being built (it must cover the regions: the swimmer or dry-only graph of
	// the world bake). The way to open the whole world.
	static std::unique_ptr<KnavNavigator> openWithGraph(const std::string& dataDir,
		const std::string& bakeDir, int rx0, int rz0, int rx1, int rz1,
		const std::string& graphPath);

	~KnavNavigator() override;

	bool covers(double x, double z) const override;
	Route route(double fromX, double fromZ, double toX, double toZ) const override;
	// One Detour search (bounded node pool) on the mesh of the region both ends are in,
	// or, over a region border, on a window of tiles of both regions (cached). A goal it
	// cannot reach is NoRoute; it is not handed to the coarse graph.
	Route localRoute(double fromX, double fromZ, double toX, double toZ) const override;
	std::optional<state::GroundPoint> nearestWalkable(double x, double z,
		double reach, double radius = 0.0) const override;
	std::optional<state::GroundPoint> nearestReachable(double x, double z, double reach,
		double fromX, double fromZ) const override;

	// Gate-aware versions: the door classes' view of `book` (applyDoors) decides which gates
	// the route may use and which side of a gate a goal can be on. A coarse route that fails only
	// because of a gate says DoorRefused (the ends are one connected set with every door open).
	Route routeFor(const Traveller& who, double fromX, double fromZ, double toX,
		double toZ) const override;
	Route localRouteFor(const Traveller& who, double fromX, double fromZ, double toX,
		double toZ) const override;
	std::optional<state::GroundPoint> nearestReachableFor(const Traveller& who, double x,
		double z, double reach, double fromX, double fromZ) const override;
	// Repaints the door polygons of every region mesh and of the windows built later, and
	// re-derives which connected sets a class can cross between.
	void applyDoors(const knav::DoorBook& book) const override;
	const knav::DoorBook& doors() const noexcept;
	// The connected-set labels the goal picks use: the set of the polygon nearest (x, z), as the
	// representative a traveller class can reach from there through the doors of the book;
	// kNone off the mesh. Two points are mutually reachable on foot (barring the node pool of a
	// long search) when their sets are equal.
	std::uint32_t reachSet(const Traveller& who, double x, double z) const;
	static constexpr std::uint32_t kNoSet = ~0u;

	std::size_t clusterCount() const;
	const NavStats& stats() const noexcept { return stats_; }
#if KSIM_PROFILE
	const NavStats* profileStats() const noexcept override { return &stats_; }
#endif

private:
	struct Impl;
	Route routeUntimed(const Traveller& who, double fromX, double fromZ, double toX, double toZ) const;
	explicit KnavNavigator(std::unique_ptr<Impl> impl);

	std::unique_ptr<Impl> impl_;
	mutable NavStats stats_;
};

} // namespace ksim
