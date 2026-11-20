// No melee across a table (kenshi-k16.6.416, k16.6.415; oracle rwh.186 static / rwh.187 live): the original reads a navmesh-only straight walk
// between attacker and target (cc+0x2c0); a table is a hole in that mesh (the furniture ring), so the walk is not clear, the attacker paths round
// to the target's side and only swings there. The damage box stays prop-blind. The Storm House of zone 21,31 (user 2026-11-18, bar_lure_hostile):
// a Blood raider at (-50566, -1213) and a Wanderer at (-50543, -1212) with the long table between them, 23 u apart (reach 34.5).
// Reports the gates (straight, walk, reach) and fails if any swing STARTS with the attacker-target segment crossing a furniture ring.
// argv[1]: the navworld bake directory.
#include "ksim/combat/combat.hpp"
#include "ksim/nav/navigator.hpp"
#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "prop_clip.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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
constexpr double kAx = -50566.0, kAz = -1213.2; // the raider (capture, tick 83249)
constexpr double kBx = -50543.2, kBz = -1212.2; // the Wanderer

const kdata::Record* named(const kdata::GameData& gd, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no character ") + name);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_melee_table NAVWORLD");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kworld::ZoneCell zone = kworld::zoneCellOf(kAx, kAz);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav(argv[1], zones);
		prop_clip::PropClip clip(gd);
		const auto crossesRing = [&](const state::Fighter& a, const state::Fighter& b)
		{
			const double length = std::hypot(b.x - a.x, b.z - a.z);
			// A fighter stands at the mesh edge, inside the ring's margin: the segment counts only where it runs through the footprint itself (d = 0).
			for (double s = 0.0; s <= length; s += 0.5)
			{
				bool in = false;
				double d = 0.0;
				const double u = s / length;
				const std::string what = clip.ring(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.z + (b.z - a.z) * u, in, d);
				if (in && d <= 0.0)
				{
					if (std::getenv("KSIM_TABLE_ALL"))
						std::printf("  ring at s %.1f of %.1f: footprint distance %.2f: %s\n", s, length, d, what.c_str());
					return true;
				}
			}
			return false;
		};

		CombatArena arena(data, CombatSettings{}, 7);
		arena.setNavigation(nav);
		std::size_t swingsAcross = 0;
		std::size_t swings = 0;
		std::size_t engagesAcross = 0;
		arena.setObserver([&](const CombatEvent& e)
		{
			if (std::getenv("KSIM_TABLE_ALL") && e.kind != CombatEvent::Kind::PositionCorrected)
				std::printf("ev t %.2f kind %d who %llu other %llu code %d\n", e.time, static_cast<int>(e.kind), static_cast<unsigned long long>(e.who), static_cast<unsigned long long>(e.other), e.code);
			if (e.kind != CombatEvent::Kind::TechniqueStart && e.kind != CombatEvent::Kind::Engage)
				return;
			const state::Fighter* a = arena.find(e.who);
			const state::Fighter* b = arena.find(e.other);
			if (a == nullptr || b == nullptr || e.who != 1)
				return;
			const bool across = crossesRing(*a, *b);
			if (e.kind == CombatEvent::Kind::TechniqueStart)
			{
				++swings;
				swingsAcross += across;
			} else {
				engagesAcross += across && e.code == 0;
			}
			std::printf("t %.2f %s who %llu at (%.1f %.1f) -> %llu at (%.1f %.1f) d %.1f code %d across-ring %d\n", e.time,
				e.kind == CombatEvent::Kind::Engage ? "engage" : "swing", static_cast<unsigned long long>(e.who), a->x, a->z,
				static_cast<unsigned long long>(e.other), b->x, b->z, std::hypot(a->x - b->x, a->z - b->z), e.code, across);
		});
		const state::Fighter* ends[2] = {};
		for (int i = 0; i < 2; ++i)
		{
			std::uint64_t rng = i == 0 ? 11 : 12;
			state::Fighter f = data->makeFighter(gd, named(gd, i == 0 ? "Blood Raider" : "Wanderer")->stringID, "", rng);
			f.ranged = state::FighterRanged{};
			const double x = i == 0 ? kAx : kBx;
			const double z = i == 0 ? kAz : kBz;
			const knav::Position at = nav->walker().snap({x, 1077.0, z}, 10.0).value_or(knav::Position{x, 1077.0, z});
			f.x = at.x;
			f.y = at.y;
			f.z = at.z;
			f.team = i;
			f.orderTarget = i == 0 ? 2 : 0; // the raider is ordered onto the Wanderer (rwh.187: an ordered hostile)
			arena.add(i + 1, std::move(f));
		}
		ends[0] = arena.find(1);
		ends[1] = arena.find(2);
		const Route route = nav->routeFrom({ends[0]->x, ends[0]->y, ends[0]->z}, ends[1]->x, ends[1]->z, ends[1]->y);
		double walk = 0.0;
		for (std::size_t i = 0; route.found && i < route.points.size(); ++i)
			walk += std::hypot(route.points[i].x - (i ? route.points[i - 1].x : ends[0]->x), route.points[i].z - (i ? route.points[i - 1].z : ends[0]->z));
		std::printf("start: straight %.1f, walk %.1f (found %d), segment crosses a ring %d\n", std::hypot(ends[0]->x - ends[1]->x, ends[0]->z - ends[1]->z),
			walk, route.found, crossesRing(*ends[0], *ends[1]));
		test::require(crossesRing(*ends[0], *ends[1]), "the fixture has a furniture ring between the two");
		for (int step = 0; step < 400; ++step) // 20 s
		{
			arena.step(kDt);
			if (std::getenv("KSIM_TABLE_ALL") && step % 20 == 0)
				std::printf("  t=%d raider (%.1f %.1f) state %d target %llu route %zu/%zu\n", step / 20, ends[0]->x, ends[0]->z, static_cast<int>(ends[0]->state), static_cast<unsigned long long>(ends[0]->target), static_cast<std::size_t>(ends[0]->routeNext), ends[0]->route.size());
		}
		std::printf("end: raider (%.1f %.1f) wanderer (%.1f %.1f)\n", ends[0]->x, ends[0]->z, ends[1]->x, ends[1]->z);
		std::printf("swings by the raider: %zu, across a ring: %zu; engages across a ring: %zu\n", swings, swingsAcross, engagesAcross);
		test::require(swingsAcross == 0 && engagesAcross == 0, "the raider engaged or swung with a furniture ring between it and its target");
		std::printf("ksim_melee_table: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
