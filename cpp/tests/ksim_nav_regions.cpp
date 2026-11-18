// Streaming navmesh coverage (docs/formats/NAVMESH_CACHE.md "Streaming coverage"): a MeshNavigator over the world bake holds the zones of the players'
// full-detail areas.
//   - the mesh API: zones join and leave, a zone that comes back is the same bytes, seams link, the resident limit holds, the tile versions move;
//   - a NEW_GAME at the Hub on such a mesh: the player walks out of the Hub zone (to the iron deposit in the next one) and arrives, and the
//     mesh follows the player's areas;
//   - a hostile world squad outside the Hub's zones is built once the player is near it (it never was with the Hub mesh alone);
//   - a save loads to the same hash and the coverage is rebuilt from the saved areas;
//   - what the mesh does not depend on: the walk is the same with more zones resident at the start.
// Needs the world bake (`knav_bake --world DIR`, 256 region files): argv[1] is its directory; without it the test is skipped (exit 77).

#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

std::string gWorld; // argv[1]

// The iron deposit of the next zone east of the Hub's (the playable-loop check: the player must be able to walk there).
constexpr double kIronX = -49430.0;
constexpr double kIronZ = 3611.0;

std::vector<kworld::ZoneCell> block(int cx, int cz, int radius)
{
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -radius; dz <= radius; ++dz)
	{
		for (int dx = -radius; dx <= radius; ++dx)
			zones.push_back({cx + dx, cz + dz});
	}
	return zones;
}

std::shared_ptr<MeshNavigator> openHub(int radius = 1)
{
	return test::openWorldNav(gWorld, block(simlink::kHubZoneX, simlink::kHubZoneZ, radius));
}

double secondsSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

// ---- the mesh alone ----

// Some route from walkable ground 80 units west of the zone border x = `border` to walkable ground 80 units east of it, along a z line in
// the Hub's row of zones (a seam that is not linked has none).
bool routeAcross(const MeshNavigator& nav, double border)
{
	for (double z = 150.0; z < kworld::kZoneSize; z += 200.0)
	{
		const auto west = nav.nearestWalkable(border - 80.0, z, 20.0);
		const auto east = nav.nearestWalkable(border + 80.0, z, 20.0);
		if (!west || !east || west->x >= border || east->x <= border)
			continue;
		const Route route = nav.route(west->x, west->z, east->x, east->z);
		if (route.found)
			return true;
	}
	return false;
}

void meshApi()
{
	const auto start = std::chrono::steady_clock::now();
	const std::shared_ptr<MeshNavigator> nav = openHub();
	std::cout << "mesh: Hub 3x3 opened in " << secondsSince(start) << " s: " << nav->mesh().tiles().size() << " tiles, " << nav->mesh().polyCount()
		<< " polys (read " << nav->coverageTotals().readMs << " ms, apply " << nav->coverageTotals().applyMs << " ms)\n";
	test::require(nav->streaming() && nav->coveredZones().size() == 9, "nine zones resident");
	test::require(nav->covers(kIronX, kIronZ), "the iron deposit (zone 21) is on the mesh");

	// The first tile of the Hub zone's own bytes and version, to see a zone come back unchanged.
	const knav::NavConfig config = nav->mesh().config();
	const int hubTileX = static_cast<int>(std::lround((kworld::zoneMinX(simlink::kHubZoneX) - config.originX) / config.tileSize)) + 9;
	const int hubTileZ = static_cast<int>(std::lround((kworld::zoneMinZ(simlink::kHubZoneZ) - config.originZ) / config.tileSize)) + 9;
	const knav::BakedTile* before = nav->mesh().tile(hubTileX, hubTileZ);
	test::require(before != nullptr, "a tile in the Hub zone");
	const std::vector<std::uint8_t> bytes = before->data;
	const std::uint32_t version = nav->mesh().tileVersion(hubTileX, hubTileZ);

	// A walk across the seam of two zones (Hub zone to the iron's): seams are linked.
	test::require(routeAcross(*nav, kworld::zoneMinX(simlink::kHubZoneX + 1)), "a route over the seam between zones 20 and 21");

	// The window moves east by one column of zones: three zones leave, three join, no seam is left behind.
	const auto move = std::chrono::steady_clock::now();
	const MeshNavigator::CoverageChange change = nav->setCoverage(block(simlink::kHubZoneX + 1, simlink::kHubZoneZ, 1));
	std::cout << "mesh: window moved one zone east in " << secondsSince(move) << " s (read " << change.readMs << " ms, apply " << change.applyMs
		<< " ms): +" << change.added << " -" << change.removed << " zones, +" << change.tilesAdded << " -" << change.tilesRemoved << " tiles\n";
	test::require(change.added == 3 && change.removed == 3 && change.tilesAdded > 0 && change.tilesRemoved > 0, "three zones left, three joined");
	test::require(nav->mesh().tile(hubTileX, hubTileZ) != nullptr, "the zone in the middle of both windows kept its tiles");
	test::require(nav->mesh().tileVersion(hubTileX, hubTileZ) == version + 0 || nav->mesh().tileVersion(hubTileX, hubTileZ) > version,
		"tile versions only go up");
	test::require(!nav->setCoverage(block(simlink::kHubZoneX + 1, simlink::kHubZoneZ, 1)).changed(), "the same window again changes nothing");
	test::require(routeAcross(*nav, kworld::zoneMinX(simlink::kHubZoneX + 2)), "a route across the seam of zones 21 and 22, which joined later");

	// Far away and back: the Hub zone's tile is the same bytes again, the version has moved on (the tile stream sends it whole).
	// A locked door is painted before: the paint is the mesh's book, and the tile that comes back is painted again.
	const std::vector<knav::DoorKey> doors = nav->mesh().doorKeys();
	test::require(!doors.empty(), "doors in the resident zones");
	knav::DoorBook book;
	book.setState(doors.front(), knav::DoorState::Locked);
	nav->applyDoors(book);
	const knav::PolyLoc doorPoly = nav->mesh().doorPolys(doors.front()).front();
	const std::vector<std::uint8_t> locked = nav->mesh().tileBlob(doorPoly.tileX, doorPoly.tileZ);
	nav->setCoverage(block(40, 10, 1));
	test::require(!nav->covers(kIronX, kIronZ) && nav->mesh().tile(hubTileX, hubTileZ) == nullptr, "a dropped zone has no tiles");
	test::require(nav->coveredZones().size() == 9, "nine zones resident far away");
	nav->setCoverage(block(simlink::kHubZoneX + 1, simlink::kHubZoneZ, 1));
	test::require(nav->mesh().appliedDoors() == book && nav->mesh().tileBlob(doorPoly.tileX, doorPoly.tileZ) == locked,
		"a door tile that was dropped and read again is painted from the mesh's door book");
	const knav::BakedTile* back = nav->mesh().tile(hubTileX, hubTileZ);
	test::require(back != nullptr && back->data == bytes, "a zone that comes back has the same tile bytes");
	test::require(nav->mesh().tileVersion(hubTileX, hubTileZ) > version, "a tile that came back has a higher version");

	// The resident limit: more zones than fit keep the first kMaxZones in the caller's order.
	const MeshNavigator::CoverageChange big = nav->setCoverage(block(simlink::kHubZoneX, simlink::kHubZoneZ, 3));
	test::require(nav->coveredZones().size() == MeshNavigator::kMaxZones && big.overflow == 49 - MeshNavigator::kMaxZones, "the resident limit");
	test::require(nav->mesh().tiles().size() <= static_cast<std::size_t>(knav::kMaxResidentTiles), "no more tiles than the tile id allows");
	std::cout << "mesh: 25 zones resident: " << nav->mesh().tiles().size() << " tiles\n";
	std::cout << "mesh: totals: " << nav->coverageTotals().added << " zones in, " << nav->coverageTotals().removed << " out, read "
		<< nav->coverageTotals().readMs << " ms, apply " << nav->coverageTotals().applyMs << " ms\n";
}

// ---- a game on it ----

Simulation newGame(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav, std::uint64_t seed,
	bool byDensity = false)
{
	SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = byDensity;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	if (byDensity)
	{
		// A hostile near the Hub, placed on purpose (BAR_FIGHT_LOOP "Why Dust Bandits patrol"): the density draw puts one there only by chance.
		sim.placeWildernessSquad("1302-gamedata.base", -48880.0, -823.0, 15);
	}
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	sim.enableNavigation(nav);
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	test::require(!sid.empty(), "the data has the Wanderer start-off");
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(3);
	return sim;
}

std::uint64_t playerOf(const Simulation& sim)
{
	for (const CombatArena::Member& member : sim.combat()->members())
	{
		if (member.fighter.playerControlled)
			return member.id;
	}
	test::require(false, "the new game has a player character");
	return 0;
}

state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

void teleport(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = fighterOf(sim, id);
	f.x = x;
	f.z = z;
	f.y = sim.groundHeight(x, z).value_or(f.y);
	f.clearRoute();
	f.building = 0;
}

// The order the session gives for a click: the route from where the character stands, then the command.
// Returns where the route ends (an approach route ends short of the click: "walk as close as you can").
std::optional<state::GroundPoint> orderWalk(Simulation& sim, std::uint64_t id, double x, double z, std::uint32_t seq)
{
	const state::Fighter& f = *sim.combat()->find(id);
	Route route = sim.navigation()->routeFrom({f.x, f.y, f.z}, x, z);
	if (!route.found)
		return std::nullopt;
	const state::GroundPoint end = route.points.back();
	sim.enqueue(Command::fighterMoveTo(state::Id{id}, std::move(route.points), std::move(route.heights), seq, false, route.approach));
	return end;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

bool zoneCovered(const Simulation& sim, kworld::ZoneCell zone)
{
	const auto zones = sim.navigation()->coveredZones();
	return std::find(zones.begin(), zones.end(), zone) != zones.end();
}

bool standsAt(Simulation& sim, std::uint64_t id, const state::GroundPoint& spot)
{
	return std::hypot(sim.combat()->find(id)->x - spot.x, sim.combat()->find(id)->z - spot.z) < 1.0;
}

// Where the player stands every 50 ticks of the walk to the iron deposit, the tick it arrived and where the route ended.
struct Walk
{
	std::vector<std::pair<double, double>> track;
	std::uint64_t arrived = 0;
	state::GroundPoint end;
};

// The deposit is on a slope: the nearest walkable ground to it is 580 u away, beyond the click reach (340, MOVE_CLICK_LIVE.md), so a click on the
// deposit itself is refused as in the original. The click is on the walkable ground nearest to it; that is a separate piece of the mesh, so the walk
// is an approach route that ends at the closest reachable point (about 200 u short of it, in zone 21).
state::GroundPoint ironSpot(const Simulation& sim)
{
	const auto spot = sim.navigation()->nearestWalkable(kIronX, kIronZ, 3000.0);
	test::require(spot.has_value(), "walkable ground near the iron deposit");
	return *spot;
}

Walk walkToIron(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, int hubRadius)
{
	Simulation sim = newGame(order, combat, openHub(hubRadius), 21);
	const std::uint64_t player = playerOf(sim);
	Walk walk;
	const state::GroundPoint spot = ironSpot(sim);
	const auto end = orderWalk(sim, player, spot.x, spot.z, 1);
	test::require(end.has_value(), "a route from the Hub start towards the iron deposit");
	walk.end = *end;
	test::require(kworld::zoneCellOf(end->x, end->z).x == simlink::kHubZoneX + 1, "the route leaves the Hub zone for the next one");
	for (std::uint64_t tick = 1; tick <= 20000 && walk.arrived == 0; ++tick)
	{
		sim.run(1);
		if (tick % 50 == 0)
			walk.track.push_back({sim.combat()->find(player)->x, sim.combat()->find(player)->z});
		if (standsAt(sim, player, walk.end))
			walk.arrived = tick;
	}
	if (walk.arrived == 0)
	{
		std::cout << "walk: route end " << walk.end.x << ", " << walk.end.z << "; last positions:";
		for (std::size_t i = walk.track.size() > 12 ? walk.track.size() - 12 : 0; i < walk.track.size(); ++i)
			std::cout << " (" << walk.track[i].first << ", " << walk.track[i].second << ")";
		std::cout << "\n";
	}
	test::require(walk.arrived != 0, "the walk out of the Hub zone never arrived");
	return walk;
}

// A world squad that is hostile to the player (no faction, or a faction that thinks little of the player's) and well outside the Hub's zones.
struct Wild
{
	flecs::entity squad;
	double x = 0.0;
	double z = 0.0;
	std::vector<flecs::entity> members;
};

Wild findWild(Simulation& sim, double fromX, double fromZ)
{
	flecs::entity player;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { player = e; });
	Wild best;
	double bestDistance = 1.0e18;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id&, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.source == state::SpawnSource::Dismissed)
			return;
		const double distance = std::hypot(at.x - fromX, at.z - fromZ);
		// two zones from the Hub's window at least (the 3x3 around the Hub), and nearer than any found so far
		if (distance < 3.0 * kworld::kZoneSize || distance >= bestDistance || !sim.navigation()->canCover(kworld::zoneCellOf(at.x, at.z)))
			return;
		const bool hostile = instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id)
			|| factionRelation(*sim.gameData(), ecsOf(sim).entity(instance.faction.id), player) <= FactionBook::kHostile;
		if (!hostile)
			return;
		best.squad = squad;
		best.x = at.x;
		best.z = at.z;
		bestDistance = distance;
	});
	test::require(best.squad.is_valid(), "a hostile world squad outside the Hub zones");
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(best.squad).build().each([&](flecs::entity member, const state::Id&)
	{
		best.members.push_back(member);
	});
	return best;
}

std::size_t built(const Simulation& sim, const Wild& wild)
{
	return static_cast<std::size_t>(std::count_if(wild.members.begin(), wild.members.end(), [&](flecs::entity m)
	{
		const state::Fighter* f = sim.combat()->find(idOf(m));
		return f != nullptr && !f->dead;
	}));
}

void game(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	// ---- the walk out of the Hub zone, and the same walk with more zones resident at the start ----
	const Walk walk = walkToIron(order, combat, 1);
	std::cout << "game: walked out of the Hub zone to (" << walk.end.x << ", " << walk.end.z << ") in " << walk.arrived << " ticks\n";
	const Walk more = walkToIron(order, combat, 2);
	test::require(more.arrived == walk.arrived && more.track == walk.track, "the walk is the same whatever else is resident at the start");

	// ---- the mesh follows the player; a hostile wild squad outside the Hub zones is built when the player comes ----
	Simulation sim = newGame(order, combat, openHub(), 21);
	const std::uint64_t player = playerOf(sim);
	const double homeX = fighterOf(sim, player).x;
	const double homeZ = fighterOf(sim, player).z;
	const state::GroundPoint foot = ironSpot(sim);
	const auto end = orderWalk(sim, player, foot.x, foot.z, 1);
	test::require(end.has_value(), "a route towards the iron deposit");
	runUntil(sim, [&] { return standsAt(sim, player, *end); }, 20000, "the walk towards the iron deposit never arrived");
	sim.run(100);
	const kworld::ZoneCell iron = kworld::zoneCellOf(kIronX, kIronZ);
	test::require(iron.x != simlink::kHubZoneX && zoneCovered(sim, iron) && zoneCovered(sim, {iron.x + 1, iron.z}),
		"the player's area follows into zone 21 and the mesh with it");
	std::cout << "game: " << sim.navigation()->coveredZones().size() << " zones resident with the player in zone " << iron.x << "," << iron.z << "\n";
	teleport(sim, player, homeX, homeZ);
	sim.run(200);

	const Wild wild = findWild(sim, homeX, homeZ);
	const kworld::ZoneCell wildZone = kworld::zoneCellOf(wild.x, wild.z);
	test::require(!zoneCovered(sim, wildZone) && built(sim, wild) == 0, "the wild squad is outside the mesh and is a token");
	std::cout << "game: wild squad of " << wild.members.size() << " in zone " << wildZone.x << "," << wildZone.z << ", " << std::hypot(wild.x - homeX, wild.z - homeZ)
		<< " u from the Hub start\n";
	const auto jump = std::chrono::steady_clock::now();
	const MeshNavigator::CoverageChange before = sim.navigation()->coverageTotals();
	teleport(sim, player, wild.x + 3.0, wild.z);
	runUntil(sim, [&] { return built(sim, wild) > 0; }, 3000, "the wild squad was never built near the player");
	const MeshNavigator::CoverageChange after = sim.navigation()->coverageTotals();
	test::require(zoneCovered(sim, wildZone), "the wild squad's zone is on the mesh");
	std::cout << "game: wild squad built (" << built(sim, wild) << " of " << wild.members.size() << " members) " << secondsSince(jump) << " s after the jump; coverage moved +"
		<< after.added - before.added << " -" << after.removed - before.removed << " zones, read " << after.readMs - before.readMs << " ms, apply "
		<< after.applyMs - before.applyMs << " ms\n";

	// ---- save and load ----
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_nav_regions_test.ksav";
	sim.run(7);
	const std::uint64_t split = sim.stateHash();
	const std::vector<kworld::ZoneCell> zonesAtSave = sim.navigation()->coveredZones();
	sim.saveGame(path);
	sim.run(100);

	// the loaded run starts on the Hub window again: the coverage is derived and comes back from the saved areas
	const std::shared_ptr<MeshNavigator> fresh = openHub();
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	CombatSettings settings;
	settings.substeps = 2;
	loaded.enableCombat(combat, settings);
	loaded.enableNavigation(fresh);
	test::require(loaded.stateHash() == split, "enabling combat and the mesh does not change the hash (doors and containers are in the save)");
	test::require(fresh->coveredZones() == zonesAtSave, "the coverage is rebuilt from the saved areas before anyone routes");
	loaded.run(100);
	test::require(loaded.stateHash() == sim.stateHash(), "load and continue gives the straight run's hash");
	std::remove(path.c_str());
	std::cout << "game: save ok (hash " << std::hex << split << std::dec << ", " << zonesAtSave.size() << " zones rebuilt)\n";
}

// The server's own new game (seed 1, wilderness by density) at the Hub with the streaming mesh: the wild squads inside the Hub's zones are built - at
// least one fighter of another faction - and the player can route to it. Task BV (2026-11-07): hostility is directional (FUN_14079c950, kinspect dt11),
// and the wild factions around the Hub are Dust Bandits, whose own relation to a new player is -10, neutral (residents.cpp worldTeam): they share the
// player's team (0) and are no longer "hostile". The fighter's own faction is what the test reads.
void hostileAtHub(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order, combat, openHub(), 1, true);
	sim.run(100);
	const std::uint64_t player = playerOf(sim);
	const state::Fighter& self = *sim.combat()->find(player);
	const std::string mine = sim.playerFactionSid();
	const auto zones = sim.navigation()->coveredZones();
	std::size_t squads = 0;
	std::size_t foes = 0;
	const state::Fighter* nearest = nullptr;
	for (const CombatArena::Member& member : sim.combat()->members())
	{
		const state::Fighter& f = member.fighter;
		std::string own;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& eid)
		{
			const state::Allegiance* a = e.get<state::Allegiance>();
			if (eid.value == member.id && a != nullptr)
				own = a->factionSid;
		});
		if (f.dead || f.playerControlled || (!mine.empty() && own == mine)
			|| std::find(zones.begin(), zones.end(), kworld::zoneCellOf(f.x, f.z)) == zones.end())
			continue;
		++foes;
		if (nearest == nullptr || std::hypot(f.x - self.x, f.z - self.z) < std::hypot(nearest->x - self.x, nearest->z - self.z))
			nearest = &f;
	}
	ecsOf(sim).each([&](flecs::entity, const state::Id&, const state::SquadInstance& instance, const state::Position& at)
	{
		squads += instance.source == state::SpawnSource::BiomeHomeless && std::find(zones.begin(), zones.end(), kworld::zoneCellOf(at.x, at.z)) != zones.end();
	});
	std::cout << "hostile: " << squads << " wild squads in the " << zones.size() << " covered zones, " << foes << " other-faction fighters built\n";
	test::require(foes > 0, "a new game at the Hub builds another faction's fighters in the covered zones");
	test::require(nearest != nullptr && orderWalk(sim, player, nearest->x, nearest->z, 1).has_value(), "the player can route to the nearest other-faction fighter");
	std::cout << "hostile: nearest at (" << nearest->x << ", " << nearest->z << "), " << std::hypot(nearest->x - self.x, nearest->z - self.z) << " u from the player\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_nav_regions: SKIPPED: needs the world navmesh bake (knav_bake --world DIR; pass DIR, -DKSIM_NAVWORLD=DIR): "
				<< (argc < 2 ? "no directory given" : std::string(argv[1]) + " has no region file for the Hub") << "\n";
			return 77;
		}
		gWorld = argv[1];
		meshApi();
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::shared_ptr<const CombatData> combat;
		{
			SpawnOptions options;
			options.seed = 21;
			options.wildernessByDensity = false;
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
			combat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		game(order, combat);
		hostileAtHub(order, combat);
		std::cout << "ksim_nav_regions: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_nav_regions: FAILED: " << e.what() << "\n";
		return 1;
	}
}
