// A fighter backed against a wall never ends on the other side of it (user 2026-11-11, kenshi-h2v.26: "What we DON'T want is kenshi's original 'backed
// into a wall' buggy behavior. It can sometimes glitch by leaping backward THROUGH the wall"; and a dodge that teleported up and down a ledge). OURS, a
// rewrite rule: every backward move (the dodge and backstep, the depenetration and squadmate push, a stagger, the body layer after a KO) goes through
// the Walker, which stops at or slides along the mesh edge.
//  1. A pile of upright fighters stacked beside the Storm House's wall pushes each other (and so one of them) at the wall for 30 s: nobody crosses.
//  2. A real fight in a corner of the house: one fighter with its back to the wall, three attackers on the open side and two allies in the way,
//     fought for 60 s (dodges, blocks, staggers, hits): nobody crosses, and no step teleports (a step far longer than a run, a jump in height).
//  3. The same fighter knocked out at the wall, bodies piled on it: the body layer keeps every body inside.
// "Crossed" is: off the mesh, or on another level than the house floor (outside, the ground is about 40 u lower).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat.hpp"
#include "ksim/navigator.hpp"

#include "kworld/gamedata.hpp"

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
// The Storm House interior (ksim_body_layer's spot).
constexpr double kHouseX = -51192.0;
constexpr double kHouseZ = 3192.0;
constexpr double kHouseFloorY = 1576.5;
// The longest honest step: a sprint is about 20 u/s, a lunge or a dodge a few times that; a teleport is metres.
constexpr double kMaxStep = 4.0;
constexpr double kMaxRise = 1.5;
// The house floor slopes a little; the ground outside is about 40 u lower.
constexpr double kLevelSlack = 8.0;

struct Wall
{
	double x = 0.0; // the point where a probe from the house's middle was stopped
	double z = 0.0;
	double ux = 0.0; // unit direction from the inside toward the wall
	double uz = 0.0;
	double y = 0.0;
};

struct Rig
{
	const kdata::GameData& gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	CombatArena arena;
	const kdata::Record* servant = nullptr;
	Wall wall;
	std::uint64_t rng = 7;
	std::uint64_t nextId = 1;
	std::vector<std::uint64_t> ids;
	std::vector<state::Fighter> last;
	std::string failure;

	Rig(const kdata::GameData& g, std::shared_ptr<const CombatData> d, std::shared_ptr<MeshNavigator> n, const Wall& w, std::uint64_t seed)
		: gd(g)
		, data(std::move(d))
		, nav(std::move(n))
		, arena(data, CombatSettings{}, seed)
		, wall(w)
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		arena.setNavigation(nav);
	}

	// A fighter `back` units inside the wall (along -u) and `side` along the wall.
	std::uint64_t spawn(double back, double side, int team, bool fights)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.ranged = state::FighterRanged{};
		f.x = wall.x - wall.ux * back - wall.uz * side;
		f.z = wall.z - wall.uz * back + wall.ux * side;
		f.y = nav->walker().snap({f.x, wall.y, f.z}, 10.0).value_or(knav::Position{f.x, wall.y, f.z}).y; // the floor under it
		f.yaw = std::atan2(-wall.ux, -wall.uz);
		f.team = team;
		f.playerControlled = !fights;
		const std::uint64_t id = nextId++;
		arena.add(id, std::move(f));
		ids.push_back(id);
		last.push_back(*arena.find(id));
		return id;
	}
	state::Fighter& me(std::uint64_t id) { return *arena.find(id); }

	// One step, then the checks on everybody; the first failure is kept with who, when and how.
	void step(int n)
	{
		arena.step(kDt);
		for (std::size_t i = 0; i < ids.size() && failure.empty(); ++i)
		{
			const state::Fighter& f = me(ids[i]);
			const state::Fighter& was = last[i];
			const double moved = std::hypot(f.x - was.x, f.z - was.z);
			std::string what;
			if (!nav->walker().onMesh({f.x, f.y, f.z}))
				what = "off the mesh";
			else if (std::fabs(f.y - wall.y) > kLevelSlack)
				what = "on another level (y " + std::to_string(f.y) + ", the floor is " + std::to_string(wall.y) + ")";
			else if (moved > kMaxStep)
				what = "teleported " + std::to_string(moved) + " u in one step";
			else if (std::fabs(f.y - was.y) > kMaxRise)
				what = "jumped " + std::to_string(f.y - was.y) + " in height in one step";
			if (!what.empty())
			{
				what += " [at " + std::to_string(f.x) + " " + std::to_string(f.z) + " y " + std::to_string(f.y) + ", before " + std::to_string(was.x) + " " + std::to_string(was.z) + "]";
			}
			if (!what.empty())
				failure = "fighter " + std::to_string(ids[i]) + " at step " + std::to_string(n) + ": " + what;
			last[i] = f;
		}
	}
	void run(int steps)
	{
		for (int i = 0; i < steps && failure.empty(); ++i)
			step(i);
	}
};

// The nearest wall of the house's middle that is flat over 20 u (a probe on each side is stopped at the same distance).
Wall findWall(const MeshNavigator& nav)
{
	const auto spot = nav.walker().snap({kHouseX, kHouseFloorY, kHouseZ}, 60.0);
	test::require(spot.has_value(), "no mesh inside the Storm House");
	Wall best;
	double bestDistance = 1.0e9;
	for (int k = 0; k < 24; ++k)
	{
		const double angle = k * 0.26179938779914946;
		const double ux = std::sin(angle);
		const double uz = std::cos(angle);
		const auto probe = [&](double side)
		{
			const knav::StepResult r = nav.walker().step({spot->x - uz * side, spot->y, spot->z + ux * side}, ux * 400.0, uz * 400.0);
			return r;
		};
		const knav::StepResult mid = probe(0.0);
		const knav::StepResult left = probe(10.0);
		const knav::StepResult right = probe(-10.0);
		if (!mid.blocked || !left.blocked || !right.blocked || mid.moved > 150.0 || mid.moved < 25.0)
			continue;
		if (std::fabs(left.moved - mid.moved) > 3.0 || std::fabs(right.moved - mid.moved) > 3.0 || std::fabs(mid.at.y - spot->y) > 3.0)
			continue;
		if (mid.moved < bestDistance)
		{
			bestDistance = mid.moved;
			best = {mid.at.x, mid.at.z, ux, uz, mid.at.y};
		}
	}
	test::require(bestDistance < 1.0e8, "no flat wall found inside the Storm House");
	return best;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_fight_wall HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		const Wall wall = findWall(*nav);
		std::printf("wall at (%.1f, %.1f) y %.1f, toward (%.2f, %.2f), %.1f u from the house's middle\n", wall.x, wall.z, wall.y, wall.ux, wall.uz,
			std::hypot(wall.x - kHouseX, wall.z - kHouseZ));

		// ---- 1. a pile at the wall: the depenetration push never carries anyone through ----
		{
			Rig rig(gd, data, nav, wall, 5);
			for (int i = 0; i < 8; ++i)
				rig.spawn(2.0 + 0.3 * i, 0.4 * i - 1.5, 0, false);
			rig.run(600);
			test::require(rig.failure.empty(), "pile: " + rig.failure);
			std::printf("ok  pile: eight fighters stacked at the wall for 30 s, none crossed\n");
		}

		// ---- 2. a fight with a back to the wall ----
		for (std::uint64_t seed = 5; seed < 8; ++seed)
		{
			Rig rig(gd, data, nav, wall, seed);
			const std::uint64_t held = rig.spawn(3.0, 0.0, 0, true);
			rig.spawn(8.0, 3.0, 0, true);
			rig.spawn(8.0, -3.0, 0, true);
			for (int i = 0; i < 3; ++i)
				rig.spawn(16.0 + 2.0 * i, 6.0 * i - 6.0, 1, true);
			rig.run(1200);
			test::require(rig.failure.empty(), "fight (seed " + std::to_string(seed) + "): " + rig.failure);
			std::printf("ok  fight seed %llu: 60 s, back to the wall, held fighter %s, nobody crossed or teleported\n",
				static_cast<unsigned long long>(seed), rig.me(held).dead ? "dead" : "alive");
		}

		// ---- 3. knocked out at the wall, bodies piled on it ----
		{
			Rig rig(gd, data, nav, wall, 5);
			for (int i = 0; i < 6; ++i)
			{
				const std::uint64_t id = rig.spawn(1.5 + 0.2 * i, 0.3 * i - 1.0, 0, false);
				rig.me(id).unconscious = true;
				rig.me(id).koTimer = 1.0e6;
			}
			rig.run(600);
			test::require(rig.failure.empty(), "bodies: " + rig.failure);
			std::printf("ok  bodies: six knocked-out bodies piled at the wall for 30 s, none crossed\n");
		}
		std::printf("ksim_fight_wall: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
