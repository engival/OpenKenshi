// Combat movement never ends inside a furniture prop (user 2026-11-17, kenshi-k16.6.367: "decided the best way to get out of the fight is to clip
// through a barrel and counter"; kenshi-k16.6.368). The Storm House of zone 21,31 (-50566, -1168, the playtest's): fights with their backs to the
// bar counter and the barrel, over several seeds, 60 s each; no fighter sample (every step) is inside a solid placed prop (prop_clip.hpp).
// KSIM_PROP_BODY: see below. argv[1]: the navworld bake directory (the runtime mesh unless KSIM_NAVWORLD_BAKE is set).
#include "ksim/combat/combat.hpp"
#include "ksim/nav/navigator.hpp"
#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "prop_clip.hpp"
#include "test_util.hpp"

#include <algorithm>
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

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_fight_prop NAVWORLD");
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
		prop_clip::PropClip clip(gd);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		// KSIM_PROP_BODY=<u>: every solid grown by that body radius (default knav::kFurnitureBodyClearance: the bake nulls a prop's footprint grown by it, kenshi-k16.7.89).
		const double body = std::getenv("KSIM_PROP_BODY") ? std::atof(std::getenv("KSIM_PROP_BODY")) : static_cast<double>(knav::kFurnitureBodyClearance);
		// The bake's contour simplification (kMaxSimplificationError 1.3 voxels of 2 u) can pull a mesh edge up to 0.6 u into the
		// ring at a corner, so the check tolerates that much: the body minus 0.6 u (user decision, kenshi-k16.7.89 round 5).
		constexpr double kSimplificationSlack = 0.6;
		const double margin = -std::max(0.0, body - kSimplificationSlack);
		std::size_t bad = 0;
		for (std::uint64_t seed = 5; seed < 11; ++seed)
		{
			std::uint64_t rng = seed;
			CombatArena arena(data, CombatSettings{}, seed);
			arena.setNavigation(nav);
			std::vector<std::uint64_t> ids;
			// Four fighters a side scattered over the house floor around the counter and the barrel.
			for (int i = 0; i < 8; ++i)
			{
				state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
				f.ranged = state::FighterRanged{};
				f.x = kSpotX - 4.0 - 3.0 * (i % 4) - (i / 4) * 10.0;
				f.z = kSpotZ - 14.0 + 6.0 * (i % 4) + 3.0 * (i / 4);
				// On the mesh: the bake keeps a body's radius off every prop, so a spot picked on a grid can fall in that band (off the mesh).
				const knav::Position at = nav->walker().snap({f.x, 1077.0, f.z}, 10.0).value_or(knav::Position{f.x, 1077.0, f.z});
				f.x = at.x;
				f.y = at.y;
				f.z = at.z;
				f.team = i / 4;
				arena.add(i + 1, std::move(f));
				ids.push_back(i + 1);
			}
			std::size_t seedBad = 0;
			for (int step = 0; step < 1200; ++step)
			{
				arena.step(kDt);
				for (const std::uint64_t id : ids)
				{
					const state::Fighter& f = *arena.find(id);
					const std::string in = clip.inside(f.x, f.y + 3.0, f.z, margin, true);
					if (!in.empty() && seedBad++ < 3)
						std::printf("seed %llu step %d fighter %llu at (%.1f %.1f %.1f) is inside %s\n", static_cast<unsigned long long>(seed), step, static_cast<unsigned long long>(id), f.x, f.y, f.z, in.substr(0, 120).c_str());
				}
			}
			std::printf("seed %llu: %zu samples inside a prop\n", static_cast<unsigned long long>(seed), seedBad);
			bad += seedBad;
		}
		test::require(bad == 0, std::to_string(bad) + " (fighter, step) samples stood inside a solid prop");
		std::printf("ksim_fight_prop: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
