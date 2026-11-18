// The Hub bar's door (a Storm House at -50612, -1158; the user's F12 2026-11-14, kenshi-k16.6.221 / .222): a character inside cannot
// leave through the door, it grinds against the frame, and the route in zig-zags. PROBE: prints routes and walks.
// argv[1]: the navworld bake directory (exit 77 without the region file).

#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;
namespace ks = ksim::simlink;

namespace {

constexpr double kBarX = -50612.7;
constexpr double kBarZ = -1158.0;

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::uint32_t seq = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& data, const std::shared_ptr<MeshNavigator>& n,
		const kworld::LoadOrder& order)
		: gd(g)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		sim->removeFighter(duel.enemy);
	}
	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	state::Fighter& player() { return *const_cast<CombatArena*>(sim->combat())->find(duel.player); }
	void put(double x, double y, double z)
	{
		state::Fighter& p = player();
		p.x = x;
		p.y = y;
		p.z = z;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_storm_door NAVWORLD");
		const kworld::ZoneCell cell = kworld::zoneCellOf(kBarX, kBarZ);
		if (!test::worldRegionReady(std::string(argv[1]), cell.x, cell.z))
		{
			std::cout << "ksim_storm_door: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({ cell.x + dx, cell.z + dz });
		Rig rig(gd, data, test::openWorldNav(argv[1], zones), order);
		const state::Door* door = nullptr;
		rig.ecs().each([&](const state::Door& d)
		{
			if (d.recordSid == "3392-TwoStorey.mod" && std::hypot(d.x - kBarX, d.z - kBarZ) < 20.0)
				door = &d;
		});
		test::require(door != nullptr, "the Hub bar's door is in the world");
		const double doorX = door->x;
		const double doorZ = door->z;
		struct Pt { double x, z; };
		const Pt outside[] = { { -50642.5, -1095.3 }, { -50660.0, -1120.0 } };
		const Pt inside[] = { { -50612.7, -1158.0 }, { -50610.3, -1159.5 }, { -50600.0, -1190.0 }, { -50620.0, -1180.0 }, { -50585.0, -1225.0 } };
		int walks = 0;
		for (const std::uint8_t doorState : { 1, 0 })
		for (const bool leaving : { true, false })
		for (const Pt& a : inside)
		for (const Pt& b : outside)
		{
			rig.ecs().each([&](state::Door& d) { if (d.recordSid == "3392-TwoStorey.mod") d.state = doorState; });
			rig.sim->run(2);
			const Pt from = leaving ? a : b;
			const Pt to = leaving ? b : a;
			const std::string what = std::string(leaving ? "out " : "in ") + std::to_string(int(from.x)) + "," + std::to_string(int(from.z)) + " -> "
				+ std::to_string(int(to.x)) + "," + std::to_string(int(to.z)) + (doorState != 0 ? " (door shut)" : " (door open)") + ": ";
			const auto spot = rig.nav->walker().snap({ from.x, leaving ? 1081.0 : 1089.0, from.z }, 40.0);
			test::require(spot.has_value(), what + "no mesh at the start");
			rig.put(from.x, spot->y, from.z);
			rig.sim->run(2);
			const state::Fighter& p = rig.player();
			const Route route = rig.nav->routeFrom({ p.x, p.y, p.z }, to.x, to.z, leaving ? std::optional<double>(1088.8) : std::nullopt, false, 4.0, true);
			test::require(route.found, what + "no route");
			double length = 0.0;
			double middle = 1.0e9;
			double px = p.x;
			double pz = p.z;
			for (const auto& point : route.points)
			{
				const double dx = point.x - px;
				const double dz = point.z - pz;
				const double square = dx * dx + dz * dz;
				const double t = square < 1.0e-9 ? 0.0 : std::clamp(((doorX - px) * dx + (doorZ - pz) * dz) / square, 0.0, 1.0);
				middle = std::min(middle, std::hypot(doorX - (px + dx * t), doorZ - (pz + dz * t)));
				length += std::sqrt(square);
				px = point.x;
				pz = point.z;
			}
			test::require(middle < 1.5, what + "the route passes the door " + std::to_string(middle) + " u off its middle");
			// Near-straight through the door (the user: "a giant Z pattern to get in"): within 1.2x of the walk start -> door middle -> goal, which
			// is the shortest there is when the door is not on the straight line. The goal the route ends at (pushed off a wall), not the click.
			const double viaDoor = std::hypot(doorX - p.x, doorZ - p.z) + std::hypot(route.points.back().x - doorX, route.points.back().z - doorZ);
			// (An end in the doorway itself, east of the strip behind the jamb, goes round the jamb first, and one across the room goes round the furniture: only the walk bound covers those.)
			const double reach = std::hypot(a.x - doorX, a.z - doorZ);
			const bool nook = reach < 8.0 || reach > 60.0; // or across the bar room, round its furniture
			test::require(nook || length <= 1.2 * viaDoor + 4.0, what + "the route is " + std::to_string(length) + " u, " + std::to_string(viaDoor) + " u via the door's middle");
			rig.sim->enqueue(Command::fighterMoveTo(state::Id{ rig.duel.player }, route.points, route.heights, ++rig.seq));
			double walked = 0.0;
			double lx = p.x;
			double lz = p.z;
			int still = 0;
			int idle = 0;
			bool arrived = false;
			for (int tick = 0; tick < 1200 && !arrived; ++tick)
			{
				rig.sim->tick();
				const state::Fighter& me = rig.player();
				const bool opening = rig.ecs().count<state::DoorStep>() > 0;
				const double moved = std::hypot(me.x - lx, me.z - lz);
				walked += moved;
				lx = me.x;
				lz = me.z;
				still = me.walking() && !opening && moved < 0.1 ? still + 1 : 0;
				test::require(still <= 20, what + "pressed against the frame for a second at " + std::to_string(me.x) + ", " + std::to_string(me.z));
				idle = !me.walking() && !opening ? idle + 1 : 0;
				arrived = idle > 5;
			}
			test::require(arrived, what + "the walk never ended");
			const double left = std::hypot(rig.player().x - to.x, rig.player().z - to.z);
			test::require(left < 9.0, what + "ended " + std::to_string(left) + " u from the goal");
			test::require(walked <= 1.25 * length + 10.0, what + "walked " + std::to_string(walked) + " u on a " + std::to_string(length) + " u route (backing off and retrying)");
			++walks;
		}
		std::cout << "ksim_storm_door: " << walks << " walks through the Hub bar's door ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_storm_door: " << e.what() << "\n";
		return 1;
	}
}
