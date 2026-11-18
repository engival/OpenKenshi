// Outpost s-IV floors (kenshi-k16.6.277; client lead 2026-11-15: the Shop Guards on s-IV's upper storey, y ~1246, carried wire
// floor 0, 438 of 456 wire floors in the dump were 0).
//
// Cause: s-IV's interior mask has no box shape, so kworld's hullPlanes is empty and no shape is keepInside; BuildingSite::floors
// (the tops of the keepInside shapes) came out empty and floorAt answered 0 at every height. Also the placement origin is 139 u
// under the entrance deck, so the climb to the first slab started far below the deck. Now the floors are read from the upper
// parts' own horizontal faces (BuildingSite::floors, groundY). The wire's CHARACTER `floor` is Fighter::floor as it stands
// (simlink.cpp writes it directly), so the Simulation value is the wire value.
//
//  1. the site: floors found, the deck (y 1203.4) reads 0, the bunk room (y 1246.3) reads >= 1, the roof storey the last floor;
//  2. a fighter put on each of them reads the same after a tick: building = s-IV, floor 0 / >= 1; and on the ramp midway (the rule
//     gives the midpoint between the deck and the slab: the floor turns over halfway up, like the Bar's stairs);
//  3. the open ground outside s-IV reads building 0, floor 0.

#include "knav/native.hpp"
#include "ksim/clock.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/zones.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <filesystem>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDeckX = 67772.7; // the user's capture point (knav_outpost_floor2): the entrance deck, y 1203.4
constexpr double kDeckZ = -71427.9;
constexpr double kBunkX = 67724.0; // the floor-2 bunk room
constexpr double kBunkZ = -71480.0;

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_siv_floors: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, { kworld::zoneCellOf(kDeckX, kDeckZ) });
		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::string sid;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		std::uint64_t rng = 12345;
		state::Fighter f = data->makeFighter(*gd, sid, "", rng);
		f.playerControlled = true;
		f.x = kDeckX;
		f.y = 1203.4;
		f.z = kDeckZ;
		const std::uint64_t player = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		sim.tick();
		const auto put = [&](double x, double y, double z) -> const state::Fighter&
		{
			state::Fighter& p = *const_cast<CombatArena*>(sim.combat())->find(player);
			p.x = x;
			p.y = y;
			p.z = z;
			sim.tick();
			sim.tick();
			return *sim.combat()->find(player);
		};

		const BuildingSite* siv = nullptr;
		for (const BuildingSite& site : sim.world().buildingSites())
			if (std::hypot(site.cx - 67687.9, site.cz + 71526.4) < 10.0 && site.hull)
				siv = &site;
		test::require(siv != nullptr, "no building site at Outpost s-IV");
		test::require(!siv->floors.empty(), "s-IV has no floors table (the old gap)");
		std::cout << "s-IV: key " << siv->key << " origin y " << siv->originY << " ground " << siv->groundY << " floors:";
		for (const double f : siv->floors)
			std::cout << " " << f;
		std::cout << "\n";

		test::require(siv->floorAt(1203.4) == 0, "the entrance deck is not floor 0");
		test::require(siv->floorAt(1246.3) >= 1, "the bunk room is not floor >= 1");
		test::require(siv->floorAt(siv->floors.back() + 0.5) == static_cast<int>(siv->floors.size()), "the top storey is not the last floor");

		const state::Fighter& deck = put(kDeckX, 1203.4, kDeckZ);
		test::require(deck.building == siv->key, "on the deck: building " + std::to_string(deck.building));
		test::require(deck.floor == 0, "on the deck: floor " + std::to_string(deck.floor));
		const state::Fighter& bunk = put(kBunkX, 1246.3, kBunkZ);
		test::require(bunk.building == siv->key, "in the bunk room: building " + std::to_string(bunk.building));
		test::require(bunk.floor >= 1, "in the bunk room: floor " + std::to_string(bunk.floor));
		const int bunkFloor = bunk.floor;
		// The ramp: floor 0 up to the first quarter of the climb, the upper floor from past the midpoint.
		const double slab = siv->floors.front();
		const double low = siv->groundY;
		test::require(put(kDeckX, low + 0.25 * (slab - low), kDeckZ).floor == 0, "a quarter up the ramp is not floor 0");
		test::require(put(kBunkX, low + 0.75 * (slab - low), kBunkZ).floor >= 1, "three quarters up the ramp is not floor >= 1");
		// Outside, on the hill above the deck.
		const state::Fighter& out = put(kDeckX - 120.0, 1240.0, kDeckZ + 120.0);
		test::require(out.building == 0 && out.floor == 0, "open ground outside: building " + std::to_string(out.building) + " floor " + std::to_string(out.floor));
		std::cout << "ksim_siv_floors: deck 0, bunk room " << bunkFloor << ", ramp quarter 0 / three quarters >= 1, outside 0\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_siv_floors: FAILED: " << e.what() << "\n";
		return 1;
	}
}
