// kenshi-k16.7.154: the open-sand hole at gate_heft (~66340..66445, -53200..-53030) is in the original's shipped tile and in the bake alike
// (knav_draw --native and --navworld show the same outline); routes bend round it. Pins that the runtime mesh (or the bake, with
// KSIM_NAVWORLD_BAKE) has the hole at its centre, and that the leg that does not cross it stays near its line.
#include "ksim/nav/navigator.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "nav_source.hpp"
#include "test_util.hpp"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace ksim;
int main(int argc, char** argv)
{
	test::require(argc >= 2, "usage: ksim_gate_heft_hole NAVWORLD");
	const double sx = 66496.56, sz = -53185.93, gx = 66254.188, gz = -52593.66;
	const kworld::ZoneCell cell = kworld::zoneCellOf(sx, sz);
	if (!test::worldRegionReady(argv[1], cell.x, cell.z)) { std::printf("SKIPPED\n"); return 77; }
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz) for (int dx = -1; dx <= 1; ++dx) zones.push_back({cell.x + dx, cell.z + dz});
	const auto nav = test::openWorldNav(argv[1], zones);
	test::require(nav != nullptr, "zones opened");
	const knav::NavQuery query(nav->walker().mesh());
	// No ground at radius 0 across the hole, ground on both sides at radius 4.
	test::require(!query.walkLeg({66390.f, 970.f, -53110.f}, {66391.f, 970.f, -53110.f}, 0.0f), "hole centre is no ground");
	test::require(!query.walkLeg({66360.f, 970.f, -53150.f}, {66361.f, 970.f, -53150.f}, 0.0f), "hole west part is no ground");
	knav::TravelFilter t4; t4.radius = 4.0f;
	test::require(query.walkLeg({66470.f, 970.f, -53110.f}, {66471.f, 970.f, -53110.f}, 4.0f, t4), "ground east of the hole");
	// The leg whose line clears the hole is near-straight on either mesh (native 641.5 u of 639.9).
	const Route r = nav->routeFrom({(float)sx, 970.f, (float)sz}, gx, gz, std::nullopt, false, 4.0, true, false);
	test::require(r.found, "route found");
	double len = 0, px = sx, pz = sz;
	for (const auto& p : r.points) { len += std::hypot(p.x - px, p.z - pz); px = p.x; pz = p.z; }
	const double straight = std::hypot(gx - sx, gz - sz);
	std::printf("length %.1f straight %.1f\n", len, straight);
	test::require(len < straight * 1.01, "clear leg within 1 % of the line");
	return 0;
}
