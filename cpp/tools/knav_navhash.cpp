// The original's navtile hashes recomputed from our data (kworld/navhash_source.hpp; kinspect findings/navhash.md): per
// interior the stored hash and ours, and per zone the building fold, the vegetation word and the full zone hash. Pairs
// stored interiors with our placements by the saved uid ((handleI + 1) << 16 | handleC, the Info string's hex id).
//   knav_navhash X Y       one zone (e.g. 20 32, the Hub)
//   knav_navhash --world   every shipped tile with a zone file: the interior match rates
//   knav_navhash --stale   every shipped tile: the loader's stale-tile decisions, totalled (the runtime loader's check,
//                          NAVMESH_CACHE.md "The stale-tile check in the loader")
// The gather order of an interior's children is not reproduced (see NAVHASH.md): for child-bearing interiors the tool
// prints the child set match against the live capture, not the interior hash, and --stale counts them unverifiable.

#include "knav/native.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/navhash.hpp"
#include "kworld/navhash_source.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

namespace fs = std::filesystem;

std::string hex(std::uint32_t value)
{
	char text[16];
	std::snprintf(text, sizeof text, "%08x", value);
	return text;
}

bool gStaleZones = false; // --stale --zones: a line per shipped tile

struct Counts
{
	std::size_t interiors = 0;
	std::size_t matched = 0;
	std::size_t unpaired = 0;
	std::size_t bare = 0; // no interior layout named
	std::size_t bareMatched = 0;
	std::size_t bareStoredDestroyed = 0; // bare, and stored = ours with the 0x100 term: the tile was baked destroyed
	std::size_t unjoined = 0; // a layout named but not found in interiors.level
	std::size_t withChildren = 0; // a joined layout with at least one child
	std::size_t withChildrenMatched = 0;
	std::uint32_t zoneFold = 0;
	std::size_t folded = 0;
	std::uint32_t vegetation = 0;
	std::uint32_t zoneHash = 0; // building fold + vegetation
	std::size_t foliage = 0; // generated tree-loader instances in the zone
	std::size_t foliageFolded = 0; // those its FOLIAGE_MESH lets into the fold
	bool forest = false; // the zone has a tree layer: the forest word is folded even when 0
	bool hasStored = false;
	std::uint32_t storedZone = 0;
};

// One zone's report; returns false when the tile has no shipped file.
bool reportZone(kworld::ZoneCell cell, kworld::NavHashSource& source, const kworld::AreaSources& sources, Counts& counts, bool print)
{
	knav::native::StoredHashes stored;
	const std::vector<knav::native::Mesh> meshes = knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), cell.x, cell.z, &stored);
	if (meshes.empty())
		return false;
	const kworld::NavHashSource::ZoneHashes zone = source.zone(cell);
	counts.zoneFold = zone.fold;
	counts.folded = zone.buildings;
	counts.foliage = zone.trees;
	counts.foliageFolded = zone.treesFolded;
	counts.vegetation = zone.vegetation;
	counts.forest = zone.forest;
	counts.zoneHash = zone.hash.value_or(0);
	counts.hasStored = stored.hasZone;
	counts.storedZone = stored.zone;
	if (print)
	{
		std::printf("zone %d,%d: %zu folded (path mode), building fold %s\n", cell.x, cell.z, counts.folded, hex(zone.fold).c_str());
		const std::string zoneHash = stored.hasZone ? hex(stored.zone) : "none";
		std::printf("  stored zone hash: %s\n", zoneHash.c_str());
		if (zone.hash)
			std::printf("  vegetation: %zu instances (%zu fold), word %s; our zone hash %s\n", zone.trees, zone.treesFolded,
				hex(zone.vegetation).c_str(), hex(*zone.hash).c_str());
		else
			std::printf("  vegetation: %zu instances (%zu fold), word %s; our zone hash not computable (%s)\n", zone.trees,
				zone.treesFolded, hex(zone.vegetation).c_str(), zone.reason);
	}
	std::size_t interiorIndex = 0;
	for (const knav::native::Mesh& mesh : meshes)
	{
		if (!mesh.interior())
			continue;
		const std::optional<std::uint32_t>& storedHash = stored.interiors[interiorIndex++];
		if (!storedHash)
			continue;
		++counts.interiors;
		std::uint64_t uid = 0;
		try
		{
			uid = std::stoull(mesh.info, nullptr, 16);
		}
		catch (const std::exception&)
		{
		}
		const kworld::NavPlaced* entry = source.placement(cell, uid);
		if (entry == nullptr)
		{
			++counts.unpaired;
			if (print)
				std::printf("  interior %s: stored %s (no placement with that uid)\n", mesh.info.c_str(), hex(*storedHash).c_str());
			continue;
		}
		const kworld::NavHashSource::InteriorHashes hashes = source.interior(cell, uid);
		if (hashes.basis == kworld::NavHashSource::Basis::Unjoined)
		{
			if (print)
				std::printf("  interior %-10s UNJOINED %s (%s, layout '%s'/'%s')\n", mesh.info.c_str(), entry->instanceID.c_str(),
					entry->buildingID.c_str(), entry->interiorLayout.c_str(), entry->exteriorLayout.c_str());
			++counts.unjoined;
			continue;
		}
		const std::vector<kworld::PlacedInstance> children =
			kworld::navInteriorChildren(*entry, sources.gameData, sources.layouts);
		const std::uint32_t ours = hashes.hash.value_or(0);
		const std::uint32_t parent = kworld::navInteriorBuildingHash(*entry);
		const bool ok = hashes.hash && ours == *storedHash;
		if (children.empty())
		{
			++counts.bare;
			if (!ok && kworld::comb(ours, 0x100) == *storedHash)
				++counts.bareStoredDestroyed;
		} else
			++counts.withChildren;
		if (ok)
		{
			++counts.matched;
			++(children.empty() ? counts.bareMatched : counts.withChildrenMatched);
		}
		if (print)
			std::printf("  interior %-10s %s %s (%s at %.1f,%.1f; mesh origin %.1f,%.1f,%.1f; destroyed %d, gate %d) building hash %s, %zu children: stored %s ours %s\n",
				mesh.info.c_str(), ok ? "OK  " : "FAIL", entry->instanceID.c_str(), entry->buildingID.c_str(), entry->placed.x, entry->placed.z,
				mesh.base[0], mesh.base[1], mesh.base[2], entry->destroyed ? 1 : 0, entry->isGate ? 1 : 0, hex(parent).c_str(), children.size(),
				hex(*storedHash).c_str(), hex(ours).c_str());
	}
	return true;
}

// The loader's stale-tile decisions over every shipped tile (`--stale`): the unit is the zone and each of its interiors.
struct StaleTally
{
	std::size_t tiles = 0;
	std::size_t zonesShipped = 0;
	std::size_t zonesStale = 0;
	std::size_t zonesUnverifiable = 0;
	std::size_t interiorsShipped = 0;
	std::size_t interiorsStale = 0;
	std::size_t interiorsUnverifiable = 0;
	std::map<std::string, std::size_t> reasons; // unverifiable units by reason
	std::size_t staleNoTrees = 0; // stale zones our scatter gives no instance
	std::size_t staleNoTreesForestTerm = 0; // ... whose stored hash is comb(fold, 0): the forest term is folded anyway
	std::size_t shippedNoTrees = 0;
	std::size_t noTrees = 0;
};

void tallyZone(StaleTally& tally, kworld::ZoneCell cell, kworld::NavHashSource& source, const std::vector<knav::native::Mesh>& meshes,
	const knav::native::StoredHashes& stored)
{
	const kworld::NavHashSource::ZoneHashes zone = source.zone(cell);
	const kworld::StaleVerdict verdict =
		kworld::staleVerdict(stored.hasZone ? std::optional(stored.zone) : std::nullopt, zone.hash);
	if (gStaleZones)
		std::printf("  zone %2d,%2d %-5s stored %s ours %s wood %08x trees %zu/%zu grass %zu\n", cell.x, cell.z,
			verdict == kworld::StaleVerdict::Fresh ? "fresh" : verdict == kworld::StaleVerdict::Stale ? "stale" : "??",
			stored.hasZone ? hex(stored.zone).c_str() : "none", zone.hash ? hex(*zone.hash).c_str() : "none",
			zone.vegetation, zone.trees, zone.treesFolded, zone.treesGrass);
	if (zone.trees == 0)
	{
		++tally.noTrees;
		if (stored.hasZone && stored.zone == kworld::comb(zone.fold, 0))
			++tally.staleNoTreesForestTerm; // the forest term folded even with no instance
		if (verdict == kworld::StaleVerdict::Stale)
			++tally.staleNoTrees;
		if (verdict == kworld::StaleVerdict::Fresh)
			++tally.shippedNoTrees;
	}
	if (verdict == kworld::StaleVerdict::Fresh)
		++tally.zonesShipped;
	else if (verdict == kworld::StaleVerdict::Stale)
		++tally.zonesStale;
	else
	{
		++tally.zonesUnverifiable;
		++tally.reasons[stored.hasZone ? zone.reason : "no stored zone hash"];
	}
	if (verdict != kworld::StaleVerdict::Fresh)
		return; // the zone and all its interiors rebake
	std::size_t interiorIndex = 0;
	for (const knav::native::Mesh& mesh : meshes)
	{
		if (!mesh.interior())
			continue;
		const std::optional<std::uint32_t>& storedHash = stored.interiors[interiorIndex++];
		std::uint64_t uid = 0;
		try
		{
			uid = std::stoull(mesh.info, nullptr, 16);
		}
		catch (const std::exception&)
		{
		}
		const kworld::NavHashSource::InteriorHashes hashes = source.interior(cell, uid);
		const kworld::StaleVerdict interior =
			kworld::staleVerdict(storedHash, hashes.hash, hashes.basis);
		if (interior == kworld::StaleVerdict::Fresh)
			++tally.interiorsShipped;
		else if (interior == kworld::StaleVerdict::Stale)
			++tally.interiorsStale;
		else
		{
			++tally.interiorsUnverifiable;
			++tally.reasons[storedHash ? kworld::staleReason(hashes.basis) : "no stored interior hash"];
		}
	}
}

bool tileCell(const std::string& name, kworld::ZoneCell& cell)
{
	return std::sscanf(name.c_str(), "tile%d.%d.hkt", &cell.x, &cell.z) == 2;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!fs::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::fprintf(stderr, "knav_navhash: no shipped navtiles in %s\n", KENSHI_DATA);
			return 77;
		}
		kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		kworld::AreaGatherer gatherer(KENSHI_DATA, terrain);
		const kworld::AreaSources sources = gatherer.sources();
		kworld::NavHashSource source(KENSHI_DATA, sources);
		if (argc == 4 && std::strcmp(argv[1], "--trees") == 0)
		{
			// Every generated tree-loader instance in spawn order, for diffing against a live capture (kinspect
			// findings/navhash/navb_zone*.json.gz): `x y z fold recordID`.
			const kworld::ZoneCell cell{ std::atoi(argv[2]), std::atoi(argv[3]) };
			kworld::FoliageParams params;
			params.unquantisedPositions = true;
			const std::vector<kworld::FoliageInstance> instances = kworld::generateFoliage(cell,
				sources.terrain, sources.biomes, sources.gameData, sources.features, sources.towns, params);
			for (const kworld::FoliageInstance& tree : instances)
				std::printf("%.6f %.6f %.6f %d %s\n", tree.x, tree.y, tree.z, tree.navFold ? 1 : 0,
					tree.recordID.c_str());
			return 0;
		}
		if (argc == 2 && std::strcmp(argv[1], "--check") == 0)
		{
			// The pinned numbers (docs/formats/NAVHASH.md "What reproduces"): the Hub's stored tile and our recomputation.
			int failures = 0;
			const auto check = [&](bool ok, const std::string& what)
			{
				if (!ok)
					std::fprintf(stderr, "knav_navhash: FAIL %s\n", what.c_str());
				failures += ok ? 0 : 1;
			};
			knav::native::StoredHashes hubStored;
			const std::vector<knav::native::Mesh> hubTile = knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), 20, 32, &hubStored);
			check(hubTile.size() == 22, "the Hub tile converts to 22 meshes (the world and 21 interiors)");
			check(hubStored.hasZone && hubStored.zone == 0xc9ebf91d, "the Hub's stored zone hash is c9ebf91d");
			check(hubStored.interiors.size() == 21, "the Hub tile stores 21 interior hashes");
			bool barStored = false, residentialStored = false;
			for (const auto& hash : hubStored.interiors)
			{
				barStored = barStored || (hash && *hash == 0x3550fd65);
				residentialStored = residentialStored || (hash && *hash == 0x506a99ef);
			}
			check(barStored && residentialStored, "the Bar and Residential Small stored hashes are in the Hub tile");
			Counts hub;
			check(reportZone({ 20, 32 }, source, sources, hub, false), "the Hub has a shipped tile");
			check(hub.interiors == 21 && hub.matched == 19, "the Hub: 19 of 21 stored interiors recomputed (Bar and Residential Small are stale)");
			check(hub.bare == 19 && hub.bareMatched == 19, "the Hub: every zero-child interior matches stored");
			check(hub.withChildren == 2 && hub.withChildrenMatched == 0, "the Hub: the two child-bearing interiors are reported, not matched");
			check(hub.folded == 103 && hub.zoneFold == 0x982a5c29, "the Hub: 103 folded placements, building fold 982a5c29");
			// The vegetation fold (kinspect findings/navhash.md section 11): zone 10,10 reproduces the live word from our data.
			knav::native::StoredHashes tenthStored;
			knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), 10, 10, &tenthStored);
			check(tenthStored.hasZone && tenthStored.zone == 0xe93d103c, "zone 10,10's stored zone hash is e93d103c");
			Counts tenth;
			check(reportZone({ 10, 10 }, source, sources, tenth, false), "zone 10,10 has a shipped tile");
			check(tenth.foliageFolded == 48, "zone 10,10 has 48 tree-loader instances in the fold");
			check(tenth.vegetation == 0x4b059683, "zone 10,10's vegetation word is 4b059683 (live)");
			check(tenth.zoneHash == 0xe93d103c, "zone 10,10's zone hash recomputed from our data equals the stored one");
			// Asp Gulch (52,37), the f32 height query: the live capture's folded x sequence and the stored zone hash.
			Counts asp;
			check(reportZone({ 52, 37 }, source, sources, asp, false), "zone 52,37 has a shipped tile");
			check(asp.foliage == 2515 && asp.foliageFolded == 1567, "zone 52,37 has 2515 instances and folds 1567 (live)");
			check(asp.vegetation == 0xcb3d3cdc, "zone 52,37's vegetation word is cb3d3cdc (stored zone hash 4dc1666b)");
			check(asp.zoneHash == 0x4dc1666b, "zone 52,37's zone hash recomputed from our data equals the stored one");
			Counts hubVegetation;
			reportZone({ 20, 32 }, source, sources, hubVegetation, false);
			check(hubVegetation.vegetation == 0x91949d55, "the Hub's vegetation word is 91949d55 (live)");
			check(hubVegetation.foliageFolded == 1090, "the Hub folds 1090 instances");
			// A two-biome zone (kinspect navhash section 14): the list runs the wider biome first -- Red Coast before Cinder
			// Ridge, the map's first-seen order reversed -- and the mesh gate folds 179 of its 655 instances (live).
			Counts eleventh;
			check(reportZone({ 10, 11 }, source, sources, eleventh, false), "zone 10,11 has a shipped tile");
			check(eleventh.vegetation == 0xde107fea, "zone 10,11's vegetation word is de107fea (live)");
			check(eleventh.foliage == 655 && eleventh.foliageFolded == 179, "zone 10,11 has 655 instances and folds 179 (live)");
			check(eleventh.hasStored && eleventh.storedZone == eleventh.zoneHash, "zone 10,11's stored zone hash recomputed from our data");
			std::printf("knav_navhash: vegetation words: 10,10 %08x (stored zone hash %08x), Hub %08x (%zu/%zu fold), 10,11 %08x "
				"(%zu/%zu fold)\n", tenth.vegetation, tenth.zoneHash, hubVegetation.vegetation, hubVegetation.foliageFolded,
				hubVegetation.foliage, eleventh.vegetation, eleventh.foliageFolded, eleventh.foliage);
			// The world floor: the shipped tiles that our data reproduces (docs/formats/NAVHASH.md "Gaps": stale tiles).
			Counts world;
			std::size_t noTrees = 0, noTreesForestTerm = 0;
			for (const fs::directory_entry& entry : fs::directory_iterator(knav::native::navtilesDir(KENSHI_DATA)))
			{
				kworld::ZoneCell cell;
				if (!tileCell(entry.path().filename().string(), cell) || !kworld::navZoneFileExists(KENSHI_DATA, cell))
					continue;
				knav::native::StoredHashes stored;
				if (knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), cell.x, cell.z, &stored).empty() || !stored.hasZone)
					continue;
				Counts zone;
				reportZone(cell, source, sources, zone, false);
				if (zone.foliage == 0)
				{
					++noTrees;
					if (zone.hasStored && zone.storedZone == kworld::comb(zone.zoneFold, 0))
						++noTreesForestTerm;
				}
				world.interiors += zone.interiors;
				world.matched += zone.matched;
				world.unpaired += zone.unpaired;
				world.bare += zone.bare;
				world.bareMatched += zone.bareMatched;
				world.bareStoredDestroyed += zone.bareStoredDestroyed;
				world.unjoined += zone.unjoined;
				world.withChildren += zone.withChildren;
				world.withChildrenMatched += zone.withChildrenMatched;
			}
			check(world.bare >= 480, "the world scan sees the bare interiors");
			check(world.bareMatched >= 250, "at least 250 bare interiors match their stored hash");
			check(world.bareMatched + world.bareStoredDestroyed >= 330, "at least 330 bare interiors match stored or stored with the 0x100 term");
			// The forest term is folded when the zone has a tree layer, whatever the scatter survives: every one of the
			// instance-less zone-file tiles but one equals comb(fold, 0) (NAVHASH.md "What reproduces").
			check(noTreesForestTerm >= 6 && noTreesForestTerm + 1 >= noTrees,
				"the instance-less zones fold the forest term (comb(fold, 0))");
			std::printf("knav_navhash: %s (Hub 19/21 interiors, fold %08x; world bare %zu/%zu, +0x100 %zu; children %zu/%zu)\n",
				failures == 0 ? "ok" : "FAILED", hub.zoneFold, world.bareMatched, world.bare, world.bareStoredDestroyed,
				world.withChildrenMatched, world.withChildren);
			return failures == 0 ? 0 : 1;
		}
		if ((argc == 2 || (argc == 3 && std::strcmp(argv[2], "--zones") == 0)) && std::strcmp(argv[1], "--stale") == 0)
		{
			gStaleZones = argc == 3;
			StaleTally all, filed, empty;
			for (const fs::directory_entry& entry : fs::directory_iterator(knav::native::navtilesDir(KENSHI_DATA)))
			{
				kworld::ZoneCell cell;
				if (!tileCell(entry.path().filename().string(), cell))
					continue;
				knav::native::StoredHashes stored;
				const std::vector<knav::native::Mesh> meshes =
					knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), cell.x, cell.z, &stored);
				if (meshes.empty())
					continue;
				++all.tiles;
				const bool hasZoneFile = kworld::navZoneFileExists(KENSHI_DATA, cell);
				StaleTally& tally = hasZoneFile ? filed : empty;
				tally.tiles = tally.tiles + 1;
				tallyZone(tally, cell, source, meshes, stored);
			}
			const auto total = [&](auto member)
			{
				return (filed.*member) + (empty.*member);
			};
			all.tiles = filed.tiles + empty.tiles;
			all.zonesShipped = total(&StaleTally::zonesShipped);
			all.zonesStale = total(&StaleTally::zonesStale);
			all.zonesUnverifiable = total(&StaleTally::zonesUnverifiable);
			all.interiorsShipped = total(&StaleTally::interiorsShipped);
			all.interiorsStale = total(&StaleTally::interiorsStale);
			all.interiorsUnverifiable = total(&StaleTally::interiorsUnverifiable);
			for (const auto& [name, tally] : { std::pair<const char*, const StaleTally&>{ "all", all },
					 std::pair<const char*, const StaleTally&>{ "zone file", filed },
					 std::pair<const char*, const StaleTally&>{ "no zone file", empty } })
			{
				std::printf("knav_navhash: stale check, %s: %zu shipped tiles\n", name, tally.tiles);
				std::printf("  zones:     %zu shipped, %zu rebaked (stale hash), %zu rebaked (unverifiable)\n",
					tally.zonesShipped, tally.zonesStale, tally.zonesUnverifiable);
				std::printf("  interiors: %zu shipped, %zu rebaked (stale hash), %zu rebaked (unverifiable)\n",
					tally.interiorsShipped, tally.interiorsStale, tally.interiorsUnverifiable);
				std::printf("  no trees:  %zu zones (%zu shipped, %zu stale; %zu equal comb(fold, 0))\n",
					tally.noTrees, tally.shippedNoTrees, tally.staleNoTrees, tally.staleNoTreesForestTerm);
				for (const auto& [reason, count] : tally.reasons)
					std::printf("    unverifiable: %s: %zu\n", reason.c_str(), count);
			}
			return 0;
		}
		if (argc == 3 && std::strcmp(argv[1], "--world") != 0 && std::strcmp(argv[1], "--zones") != 0)
		{
			Counts counts;
			if (!reportZone({ std::atoi(argv[1]), std::atoi(argv[2]) }, source, sources, counts, true))
			{
				std::fprintf(stderr, "knav_navhash: no tile%u.%u.hkt\n", static_cast<unsigned>(std::atoi(argv[1])), static_cast<unsigned>(std::atoi(argv[2])));
				return 1;
			}
			return 0;
		}
		// Every shipped tile: the interior match rates (uid-paired), and the zone folds.
		const bool perZone = argc == 2 && std::strcmp(argv[1], "--zones") == 0;
		Counts counts;
		std::size_t tiles = 0;
		std::size_t withHash = 0;
		for (const fs::directory_entry& entry : fs::directory_iterator(knav::native::navtilesDir(KENSHI_DATA)))
		{
			kworld::ZoneCell cell;
			if (!tileCell(entry.path().filename().string(), cell))
				continue;
			if (!kworld::navZoneFileExists(KENSHI_DATA, cell))
				continue;
			++tiles;
			knav::native::StoredHashes stored;
			if (knav::native::loadTile(knav::native::navtilesDir(KENSHI_DATA), cell.x, cell.z, &stored).empty() || !stored.hasZone)
				continue;
			++withHash;
			Counts zoneCounts;
			reportZone(cell, source, sources, zoneCounts, false);
			if (perZone && zoneCounts.interiors != 0)
				std::printf("zone %d,%d: interiors %zu, matched %zu (bare %zu/%zu, children %zu/%zu, unjoined %zu, unpaired %zu)\n",
					cell.x, cell.z, zoneCounts.interiors, zoneCounts.matched, zoneCounts.bareMatched, zoneCounts.bare,
					zoneCounts.withChildrenMatched, zoneCounts.withChildren, zoneCounts.unjoined, zoneCounts.unpaired);
			counts.interiors += zoneCounts.interiors;
			counts.matched += zoneCounts.matched;
			counts.unpaired += zoneCounts.unpaired;
			counts.bare += zoneCounts.bare;
			counts.bareMatched += zoneCounts.bareMatched;
			counts.bareStoredDestroyed += zoneCounts.bareStoredDestroyed;
			counts.unjoined += zoneCounts.unjoined;
			counts.withChildren += zoneCounts.withChildren;
			counts.withChildrenMatched += zoneCounts.withChildrenMatched;
		}
		std::printf("tiles %zu, with a stored zone hash %zu\n", tiles, withHash);
		std::printf("interiors %zu (paired %zu, unpaired %zu): matched %zu\n", counts.interiors, counts.interiors - counts.unpaired,
			counts.unpaired, counts.matched);
		std::printf("  bare (no children): %zu of %zu (%zu more equal ours with the 0x100 term: baked destroyed)\n",
			counts.bareMatched, counts.bare, counts.bareStoredDestroyed);
		std::printf("  with children:      %zu of %zu (child gather order not reproduced)\n", counts.withChildrenMatched, counts.withChildren);
		std::printf("  unjoined layouts:   %zu\n", counts.unjoined);
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "knav_navhash: %s\n", error.what());
		return 1;
	}
}
