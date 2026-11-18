// Where a body comes to rest (task body-rest, PHYSICS.md 3; OURS): the release of a walking carrier lands the body 13-20 u down the
// carrier's heading, a heavy hit pushes a body along the hit, a body on a steep slope slides downhill a capped distance, and all of it is
// seeded, on the navmesh, and the same after a save/load. PHYSICS.md 6 tests 1 and 4.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;


// The whole world on the Hub mesh: building membership, the put-down spot and a save/load across the spread.
struct WorldRig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::unique_ptr<Simulation> sim;
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 7;

	WorldRig(std::shared_ptr<const kdata::GameData> g, std::shared_ptr<const CombatData> d, std::shared_ptr<MeshNavigator> n)
		: gd(std::move(g))
		, data(std::move(d))
		, nav(std::move(n))
		, sim(std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 77)))
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim->setGameData(gd);
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	std::uint64_t spawn(double x, double y, double z, bool body, double yaw = 0.0)
	{
		state::Fighter fighter = data->makeFighter(*gd, servant->stringID, "", rng);
		fighter.x = x;
		fighter.y = y;
		fighter.z = z;
		fighter.yaw = yaw;
		fighter.playerControlled = true; // the residents have no business with a player character
		fighter.unconscious = body;
		fighter.koTimer = 1.0e6;
		return sim->spawnFighter(std::move(fighter)).get<state::Id>()->value;
	}
};

// A body's rest from a copy: the arena alone (no world), on the mesh.
struct Probe
{
	CombatArena arena;
	state::Fighter body;
	Probe(const std::shared_ptr<const CombatData>& data, const std::shared_ptr<MeshNavigator>& nav, const state::Fighter& f)
		: arena(data, CombatSettings{}, 5)
		, body(f)
	{
		arena.setNavigation(nav);
	}
	// `body` after a ragdoll start at (x, y, z) with the push (0 = none).
	state::Fighter rest(const knav::Position& at, double pushX, double pushZ, double push)
	{
		state::Fighter f = body;
		f.x = at.x;
		f.y = at.y;
		f.z = at.z;
		arena.rollRagdoll(f, 9, pushX, pushZ, push);
		return f;
	}
};

struct Dropped
{
	double x;
	double y;
	double z;
	double along;
	double apart;
};

// A walking carrier at a point with a person in its arms is removed: dropCarried is the release.
Dropped release(const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<const CombatData>& data,
	const std::shared_ptr<MeshNavigator>& nav, const knav::Position& at, double yaw)
{
	WorldRig rig(gd, data, nav);
	const std::uint64_t carrier = rig.spawn(at.x, at.y, at.z, false, yaw);
	const std::uint64_t person = rig.spawn(at.x, at.y, at.z, true);
	state::Fighter& c = rig.f(carrier);
	c.carrying = person;
	c.route.push_back({at.x + std::sin(yaw) * 500.0, at.y, at.z + std::cos(yaw) * 500.0});
	c.routeNext = 0;
	rig.f(person).carriedBy = carrier;
	rig.sim->removeFighter(carrier);
	const state::Fighter& p = rig.f(person);
	const double apart = std::hypot(p.x - at.x, p.z - at.z);
	return {p.x, p.y, p.z, ((p.x - at.x) * std::sin(yaw) + (p.z - at.z) * std::cos(yaw)) / std::max(apart, 1.0e-9), apart};
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_body_rest HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		const auto onMesh = [&](double x, double y, double z)
		{
			return nav->walker().onMesh({x, y, z});
		};

		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		std::uint64_t rng = 7;
		state::Fighter lying = data->makeFighter(*gd, servant->stringID, "", rng);
		lying.unconscious = true;
		lying.koTimer = 1.0e6;
		Probe probe(data, nav, lying);

		// ---- the ground: flat and steep spots of a scan of the Hub ----
		const auto hub = nav->walker().snap({kHubX, 0.0, kHubZ}, 1.0e4);
		test::require(hub.has_value(), "no mesh at the Hub");
		std::vector<knav::Position> flat;
		std::vector<knav::Position> steep;
		for (double dx = -600.0; dx <= 600.0; dx += 12.0)
		{
			for (double dz = -600.0; dz <= 600.0; dz += 12.0)
			{
				const auto spot = nav->walker().snap({hub->x + dx, hub->y, hub->z + dz}, 12.0);
				if (!spot)
					continue;
				const double slid = std::hypot(probe.rest(*spot, 0, 0, 0.0).x - spot->x, probe.rest(*spot, 0, 0, 0.0).z - spot->z);
				(slid == 0.0 ? flat : steep).push_back(*spot);
			}
		}
		std::printf("scan: %zu flat spots, %zu that slide\n", flat.size(), steep.size());
		test::require(!flat.empty() && !steep.empty(), "the Hub scan found no flat or no steep ground");

		// ---- 4. slope slide: downhill, capped, lower, on the mesh, the same twice ----
		// The baked detail mesh floats 0-4 u above a slope (Walker::kGroundSnap), so a heights compare is noisy: most slides end lower
		// and none ends more than that higher.
		std::size_t lower = 0;
		for (const knav::Position& at : steep)
		{
			const state::Fighter a = probe.rest(at, 0, 0, 0.0);
			const state::Fighter b = probe.rest(at, 0, 0, 0.0);
			const double slid = std::hypot(a.x - at.x, a.z - at.z);
			test::require(a.x == b.x && a.z == b.z && a.y == b.y, "the slide is not deterministic");
			test::require(slid <= CombatArena::kSlideCap + 1.0e-9, "a slide of " + std::to_string(slid) + " u past the cap");
			test::require(a.y < at.y + knav::Walker::kGroundSnap, "the slide went uphill by " + std::to_string(a.y - at.y));
			lower += a.y < at.y ? 1 : 0;
			test::require(onMesh(a.x, a.y, a.z), "a slid body rests off the navmesh");
		}
		test::require(lower * 10 >= steep.size() * 8, "only " + std::to_string(lower) + " of " + std::to_string(steep.size()) + " slides ended lower");
		// A body that is dead when its ragdoll starts rests at its death point, however steep.
		for (const knav::Position& at : steep)
		{
			probe.body.dead = true;
			const state::Fighter d = probe.rest(at, 0, 0, 0.0);
			probe.body.dead = false;
			test::require(d.x == at.x && d.y == at.y && d.z == at.z, "a dead body slid off its death point");
		}
		std::printf("ok  slide: %zu spots, %zu ended lower, all within the cap and on the mesh\n", steep.size(), lower);

		// ---- 3. knockback: along the hit, on the mesh, seeded; a spot with room moves the full distance ----
		int full = 0;
		std::size_t offMesh = 0;
		for (const knav::Position& at : flat)
		{
			for (const double angle : {0.0, 1.5707963267948966, 3.141592653589793, 4.71238898038469})
			{
				for (const double push : {3.0, 12.0})
				{
					const state::Fighter f = probe.rest(at, std::sin(angle), std::cos(angle), push);
					const double moved = std::hypot(f.x - at.x, f.z - at.z);
					// The landing may be a slope: the slide adds up to its cap.
					test::require(moved <= push + CombatArena::kSlideCap + 1.0e-6,
						"a knockback of " + std::to_string(moved) + " past " + std::to_string(push));
					full += std::fabs(moved - push) < 0.05 && push == 3.0 ? 1 : 0;
					if (!onMesh(f.x, f.y, f.z))
					{
						double d = -1.0;
						nav->walker().snap({f.x, f.y, f.z}, 20.0, &d);
						std::printf("  off: from (%.2f %.2f %.2f) angle %.2f push %.0f to (%.2f %.2f %.2f), snap %.3f\n", at.x, at.y, at.z, angle, push, f.x, f.y, f.z, d);
						++offMesh;
					}
				}
			}
		}
		// ---- 4 (PHYSICS 6). no body rests off the navmesh, after a push or a slide ----
		for (const knav::Position& at : steep)
		{
			for (const double push : {3.0, 12.0})
				offMesh += onMesh(probe.rest(at, 1.0, 0.0, push).x, probe.rest(at, 1.0, 0.0, push).y, probe.rest(at, 1.0, 0.0, push).z) ? 0 : 1;
		}
		test::require(offMesh == 0, std::to_string(offMesh) + " bodies rest off the navmesh");
		test::require(full > 0, "no knockback of 3 u moved the full distance anywhere");
		std::printf("ok  knockback: %d full 3 u pushes; no body off the navmesh\n", full);

		// ---- 2. drop offset: a walking carrier's release lands 13-20 u down its heading ----
		int landedFar = 0;
		for (int i = 0; i < 8; ++i)
		{
			const double yaw = static_cast<double>(i) * 0.7853981633974483;
			const Dropped a = release(gd, data, nav, *hub, yaw);
			const Dropped b = release(gd, data, nav, *hub, yaw);
			test::require(a.x == b.x && a.y == b.y && a.z == b.z, "the drop is not deterministic");
			test::require(onMesh(a.x, a.y, a.z), "a dropped body rests off the navmesh");
			if (a.apart >= CombatArena::kDropMin - 1.0e-6 && a.apart <= CombatArena::kDropMax + CombatArena::kSlideCap)
			{
				test::require(a.along > 0.5 || a.apart > CombatArena::kDropMax, "the drop is not along the heading");
				++landedFar;
				std::printf("    yaw %.2f: landed %.2f u away, %.3f along\n", yaw, a.apart, a.along);
			}
		}
		test::require(landedFar > 0, "no release landed 13-20 u from the carrier");
		std::printf("ok  drop: %d of 8 headings landed 13-20 u down the heading, deterministic, on the mesh\n", landedFar);

		// ---- 1. a save/load mid-fall continues the same; two runs agree on the rest point ----
		{
			double restX[2] = {};
			double restZ[2] = {};
			for (int run = 0; run < 2; ++run)
			{
				WorldRig rig(gd, data, nav);
				const std::uint64_t id = rig.spawn(steep.front().x, steep.front().y, steep.front().z, true);
				rig.tick(1); // the arena's first step starts the ragdoll and slides the body
				if (run == 0)
				{
					const std::uint64_t atSave = rig.sim->stateHash();
					const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_body_rest.ksav";
					rig.sim->saveGame(path);
					Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path);
					CombatSettings settings;
					settings.substeps = 2;
					loaded.setGameData(gd);
					loaded.enableCombat(data, settings);
					loaded.enableNavigation(nav);
					test::require(loaded.stateHash() == atSave, "a save mid-fall loads to another state");
					for (int i = 0; i < 200; ++i)
					{
						loaded.tick();
						rig.tick();
					}
					test::require(loaded.stateHash() == rig.sim->stateHash(), "a load mid-fall continues differently");
					std::remove(path.c_str());
				} else {
					rig.tick(200);
				}
				restX[run] = rig.f(id).x;
				restZ[run] = rig.f(id).z;
				test::require(onMesh(rig.f(id).x, rig.f(id).y, rig.f(id).z), "the body rests off the navmesh");
			}
			test::require(restX[0] == restX[1] && restZ[0] == restZ[1], "two runs of one seed rest at different points");
			std::printf("ok  mid-fall: save/load hash equal, two runs rest at (%.2f, %.2f)\n", restX[0], restZ[0]);
		}
		std::printf("ksim_body_rest: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_body_rest: FAILED: " << e.what() << "\n";
		return 1;
	}
}
