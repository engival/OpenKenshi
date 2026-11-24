#pragma once

// Recomputes the original's navtile hashes (kworld/navhash.hpp) from our data: a zone's building fold and forest word,
// and an interior's building + children. One implementation for the tools (knav_navhash) and the runtime loader's stale
// check (MeshNavigator::openNative, docs/formats/NAVMESH_CACHE.md "The stale-tile check in the loader"). The recipe and
// what it reproduces:
// docs/formats/NAVHASH.md.

#include "kworld/zones.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

class BiomeMap;
class Features;
class Heightmap;
class InteriorLayouts;
class Towns;
struct AreaSources;

// One placed building with the game state the hash reads (the tool and the loader both need it).
struct NavPlaced
{
	std::string instanceID;
	std::string buildingID;
	PlacedInstance placed;
	bool destroyed = false;
	bool isGate = false; // BUILDING `is wall gate`
	bool hasPath = true; // BUILDING `path mode` != 0 (the loader's object filter, UNSURE mapping)
	std::uint64_t uid = 0; // the saved uid: (handleI + 1) << 16 | handleC, the tile's Info hex id

	std::string interiorLayout;
	std::string exteriorLayout;
};

// The zone's placements across the four data layers: later layers override a same-id instance in place (kworld's
// resolveLayers order), which is the order the game creates the zone's objects in. Throws on an unreadable zone file.
std::vector<NavPlaced> loadNavZone(const std::string& dataDir, ZoneCell cell, const kdata::GameData& gameData);

// Whether any data layer has the zone's file (a zone with none has no placements).
bool navZoneFileExists(const std::string& dataDir, ZoneCell cell);

// The objects a zone's hash folds (the exe's five filters, NAVHASH.md "Mapping the exe filters").
std::vector<const NavPlaced*> navZoneObjects(const std::vector<NavPlaced>& placed);

// A building's term in a zone fold (the destroyed term needs the gate flag too).
std::uint32_t navZoneBuildingHash(const NavPlaced& entry);

// A building's own hash as an interior's parent (no destroyed term: the interior's is comb(s, 0x100)).
std::uint32_t navInteriorBuildingHash(const NavPlaced& entry);

// An interior's children: the layout items that are real buildings (a BUILDING record that is not a node).
std::vector<PlacedInstance> navInteriorChildren(const NavPlaced& entry, const kdata::GameData& gameData,
	const InteriorLayouts& layouts);

// The hashes of one zone and its interiors, from the game data an AreaGatherer already holds (AreaSources).
class NavHashSource
{
public:
	// Why an interior's hash is missing, or a guess.
	enum class Basis
	{
		Exact, // no children: the hash is exact
		ChildOrder, // it has children; our layout order is not the game's gather order (NAVHASH.md "Gaps")
		Unpaired, // no placement has the tile's uid
		Unjoined, // a layout is named but is not in interiors.level
	};

	struct ZoneHashes
	{
		std::uint32_t fold = 0; // the building fold
		std::size_t buildings = 0; // placements folded
		std::uint32_t vegetation = 0; // the forest word
		bool forest = false; // the zone has a tree layer, so the forest word is folded even when 0
		std::size_t trees = 0; // generated instances in the zone
		std::size_t treesFolded = 0; // instances the mesh gate keeps
		std::size_t treesGrass = 0; // instances of `limit to grass areas` meshes (the grass mask's reach)
		std::optional<std::uint32_t> hash; // the zone hash; nullopt when the zone data could not be read
		const char* reason = nullptr; // why, when hash is nullopt
	};

	struct InteriorHashes
	{
		std::optional<std::uint32_t> hash; // computed with our child order; exact when basis is Exact
		Basis basis = Basis::Exact;
	};

	// The placement an interior's uid pairs with (nullptr when none), by the same rules as interior().
	const NavPlaced* placement(ZoneCell cell, std::uint64_t uid);

	NavHashSource(std::string dataDir, const AreaSources& sources);

	// One zone's hash (cached per zone).
	ZoneHashes zone(ZoneCell cell);
	// One interior's hash, by the saved uid the tile's Info string carries (the low 24 bits are retried, NAVHASH.md).
	InteriorHashes interior(ZoneCell cell, std::uint64_t uid);

private:
	struct ZoneData
	{
		std::vector<NavPlaced> placed;
		std::map<std::uint64_t, std::size_t> byUid;
		std::optional<ZoneHashes> hashes;
		bool loaded = false; // the placements have been read (even when there are none)
		const char* failure = nullptr; // the placements could not be read
	};
	ZoneData& data(ZoneCell cell);

	std::string dataDir_;
	const kdata::GameData& gameData_;
	const InteriorLayouts& layouts_;
	const Heightmap& terrain_;
	const BiomeMap& biomes_;
	const Features& features_;
	const Towns& towns_;
	std::map<std::int64_t, ZoneData> zones_; // by zoneIndexOf
};

// What the original's stale-tile check says of one unit (zone or interior), given the hash stored in the shipped tile
// and ours (docs/formats/NAVMESH_CACHE.md "The stale-tile check in the loader").
enum class StaleVerdict
{
	Fresh, // the shipped mesh is valid for our world
	Stale, // the hashes differ and ours is exact: rebake
	Unverifiable, // no stored hash, or a child-order guess that differs: cannot tell, so rebake
};

// `basis` decides whether a difference proves staleness: a child-order interior hash is a guess (NAVHASH.md "Gaps"),
// so a difference may be our order, not the world. Pass Exact for zones.
StaleVerdict staleVerdict(std::optional<std::uint32_t> stored, std::optional<std::uint32_t> ours,
	NavHashSource::Basis basis = NavHashSource::Basis::Exact);

// A short name for the reason a unit could not be verified ("no stored hash", "child gather order", ...).
const char* staleReason(NavHashSource::Basis basis);

// A zone whose hash differs from the shipped tile's but that folds no building: only the forest word can differ, and our
// tree placement is the approximated part (NAVHASH.md), while the shipped tile is the original's own mesh. The loader keeps
// that tile instead of a rebake that redoes the trees only (kenshi-k16.7.74; the rebakes scored 86.7% covered / 15.1% surplus
// against the tile they replaced, "Shipped-tile gate"). The shipped-tile gate uses the same predicate.
bool forestOnlyStale(std::optional<std::uint32_t> stored, const NavHashSource::ZoneHashes& ours);

} // namespace kworld
