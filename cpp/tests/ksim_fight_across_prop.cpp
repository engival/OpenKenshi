// A fighter in reach by the crow but with a prop in the line is not in reach (kenshi-k16.6.441): it keeps walking its route round the prop
// until the line is clear, then fights. The Storm House of zone 21,31 (the bar counter and barrel, as ksim_fight_prop): pairs of mesh spots a
// little apart whose straight line a prop blocks (NavQuery::walkLeg fails) but whose walk is short are found on the floor; two fighters
// of opposite teams placed on either side must both close to melee and both land hits. Before the fix the pair stood within reach by the
// crow and swung into the prop.
// argv[1]: the navworld bake directory (the runtime mesh unless KSIM_NAVWORLD_BAKE is set).
#include "ksim/combat/combat.hpp"
#include "ksim/nav/navigator.hpp"
#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr double kSpotX = -50566.0;
constexpr double kSpotZ = -1168.0;
constexpr double kFloorY = 1077.0;
constexpr double kMinGap = 10.0; // the pair stands this far apart at least, 20 at most
constexpr double kMaxGap = 20.0;
constexpr double kMaxWalk = 45.0; // a short way round
constexpr double kSwingGap = 8.0; // closer than this the endpoints are in the mesh's inset band round the prop, where walkLeg refuses
constexpr int kPairs = 3;
constexpr double kSeconds = 120.0; // equal fighters dodge and block most swings: a fixed-seed pair can go 40 s with one side landing none

struct Pair { knav::Position a, b; };

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_fight_across_prop NAVWORLD");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kworld::ZoneCell zone = kworld::zoneCellOf(kSpotX, kSpotZ);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], zones);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");

		// The pairs: a grid of mesh spots round the counter; a pair is two spots kMinGap..kMaxGap apart, the line blocked, a short walk.
		std::vector<Pair> pairs;
		std::vector<knav::Position> spots;
		for (double dx = -40.0; dx <= 40.0; dx += 4.0)
		{
			for (double dz = -40.0; dz <= 40.0; dz += 4.0)
			{
				if (const auto at = nav->walker().snap({kSpotX + dx, kFloorY, kSpotZ + dz}, 3.0))
					spots.push_back(*at);
			}
		}
		for (std::size_t i = 0; i < spots.size() && pairs.size() < kPairs; ++i)
		{
			for (std::size_t j = i + 1; j < spots.size() && pairs.size() < kPairs; ++j)
			{
				const knav::Position& a = spots[i];
				const knav::Position& b = spots[j];
				const double gap = std::hypot(a.x - b.x, a.z - b.z);
				if (gap < kMinGap || gap > kMaxGap || std::fabs(a.y - b.y) > 4.0 || nav->straightWalk(a.x, a.y, a.z, b.x, b.y, b.z))
					continue;
				const Route walk = nav->routeFrom(a, b.x, b.z, b.y);
				if (!walk.found)
					continue;
				double length = 0.0;
				double px = a.x;
				double pz = a.z;
				for (const auto& p : walk.points)
				{
					length += std::hypot(p.x - px, p.z - pz);
					px = p.x;
					pz = p.z;
				}
				length += std::hypot(b.x - px, b.z - pz);
				if (length <= kMaxWalk)
					pairs.push_back({a, b});
			}
		}
		test::require(!pairs.empty(), "no pair of spots with a prop between them in the Storm House");

		std::size_t bad = 0;
		for (const Pair& pair : pairs)
		{
			std::uint64_t rng = 7;
			CombatArena arena(data, CombatSettings{}, 7);
			arena.setNavigation(nav);
			const knav::Position at[2] = {pair.a, pair.b};
			for (int i = 0; i < 2; ++i)
			{
				state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
				f.ranged = state::FighterRanged{};
				f.x = at[i].x;
				f.y = at[i].y;
				f.z = at[i].z;
				f.team = i;
				f.orderTarget = 2 - i; // each is told to attack the other (a perception check would not see across the prop)
				arena.add(i + 1, std::move(f));
			}
			std::uint64_t blockedSwings = 0; // swings started with a prop between the fighter and its target
			std::uint64_t hits[2] = {0, 0}; // hits landed by fighter 1 / 2
			arena.setObserver([&](const CombatEvent& e)
			{
				if (e.kind == CombatEvent::Kind::TechniqueStart)
				{
					const state::Fighter& a = *arena.find(e.who);
					const state::Fighter& t = *arena.find(e.other);
					if (e.technique >= 0 && !nav->straightWalk(a.x, a.y, a.z, t.x, t.y, t.z) && std::hypot(a.x - t.x, a.z - t.z) > kSwingGap)
						++blockedSwings;
				}
				if (e.kind == CombatEvent::Kind::Hit && (e.who == 1 || e.who == 2))
					++hits[e.who == 1 ? 1 : 0]; // `who` is the victim
			});
			double closest = 1e9;
			for (int step = 0; step < static_cast<int>(kSeconds / kDt); ++step)
			{
				arena.step(kDt);
				const state::Fighter& f1 = *arena.find(1);
				const state::Fighter& f2 = *arena.find(2);
				closest = std::min(closest, std::hypot(f1.x - f2.x, f1.z - f2.z));
			}
			std::printf("pair (%.1f %.1f)-(%.1f %.1f) gap %.1f: hits by 1 %llu, by 2 %llu, closest %.1f, swings started across the prop %llu\n", pair.a.x, pair.a.z, pair.b.x,
				pair.b.z, std::hypot(pair.a.x - pair.b.x, pair.a.z - pair.b.z), static_cast<unsigned long long>(hits[0]),
				static_cast<unsigned long long>(hits[1]), closest, static_cast<unsigned long long>(blockedSwings));
			if (hits[0] == 0 || hits[1] == 0 || blockedSwings != 0)
				++bad;
		}
		test::require(bad == 0, std::to_string(bad) + " pairs where a fighter never landed a hit, or swung across the prop");
		std::printf("ksim_fight_across_prop: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
