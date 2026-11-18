// A fighter pushed or backing off a ledge stays on its own floor (user 2026-11-11, kenshi-h2v.26 / h2v.30: "Agnu dodged backward. He teleported from the top
// ledge, down to the ramp, then back up to the ledge, and then settled on the ramp. Very ugly."). OURS, a rewrite rule (ENGAGEMENT.md 15): a backward move stays
// on walkable ground continuous with where it started: no drop off a ledge, no snap between levels, no jitter between two heights; a step that is not walkable
// is shortened or skipped. The same three set-ups as ksim_fight_wall, but at the edge of an upper floor over a lower one (the Hub's upper floor near the Bar,
// about 66 u above the floor below) instead of a wall, every tick of every fighter logged (y range per fighter, printed):
//  1. a pile of upright fighters stacked at the ledge pushes each other (and so one of them) off it for 30 s.
//  2. a real fight with one fighter's back to the ledge, attackers on the open side, 60 s.
//  3. knocked-out bodies piled at the ledge.
//  4. a fighter set 3 u past the edge at the upper floor's height (over the lower floor): `correct()` must bring it back to its own level.
//  5. a fighter set 2, 6 and 12 u past the edge of a ledge with a stair flight beside it (the Hub's, at -51094 2512: the floor y 1649.5, the stairs 7 u down at
//     the edge): it ends back on its own floor. Before kenshi-h2v.30 `correct()` snapped to the nearest mesh point and the 12 u case landed on the stairs.
// "Changed level" is: a y more than 8 u from the upper floor's, or a jump in y of more than 1.5 in one step, or an up-then-down jump of more than 1 u within 2 s
// (a flip-flop between two heights). Walking down a stair flight is not one: the stairs' top joins the floor in the mesh (a fight at it is not a test of this).
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
// The upper floor near the Bar (Storm House 3384 at -51181, 2639): floor y about 1646-1648, the lower floor about 1580.
constexpr double kAreaX = -51181.0;
constexpr double kAreaZ = 2639.0;
constexpr double kAreaRadius = 70.0;
constexpr double kUpperY = 1646.0;
constexpr double kMaxStep = 4.0;
constexpr double kMaxRise = 1.5;
// The upper floor's own slope; the lower floor is 60 u or more under it.
constexpr double kLevelSlack = 8.0;
// A fighter put beside a stair flight ends within this of the ledge's height (the stairs are 7 u down).
constexpr double kStairSlack = 4.0;

struct Wall
{
	double x = 0.0; // the point where a probe from the house's middle was stopped
	double z = 0.0;
	double ux = 0.0; // unit direction from the floor toward the ledge
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
	double slack = kLevelSlack;
	std::uint64_t corrections = 0; // this step's correct() snaps
	double maxRise = kMaxRise; // a stair riser is allowed where the set-up has stairs
	std::vector<int> lastJumpStep; // per fighter: the step and direction of its last jump in y over 1 u, for the flip-flop check
	std::vector<int> lastJumpDir;
	double yLo = 1.0e9; // every tick of every fighter: the y range seen
	double yHi = -1.0e9;

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

	// A fighter `back` units back from the ledge (along -u) and `side` along it.
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
		const std::uint64_t corrected = arena.positionCorrections();
		arena.step(kDt);
		corrections = arena.positionCorrections() - corrected;
		for (std::size_t i = 0; i < ids.size() && failure.empty(); ++i)
		{
			const state::Fighter& f = me(ids[i]);
			const state::Fighter& was = last[i];
			yLo = std::min(yLo, f.y);
			yHi = std::max(yHi, f.y);
			const double moved = std::hypot(f.x - was.x, f.z - was.z);
			std::string what;
			if (!nav->walker().onMesh({f.x, f.y, f.z}))
				what = "off the mesh";
			else if (std::fabs(f.y - wall.y) > slack)
				what = "on another level (y " + std::to_string(f.y) + ", the upper floor is " + std::to_string(wall.y) + ")";
			else if (moved > kMaxStep)
				what = "teleported " + std::to_string(moved) + " u in one step";
			else if (std::fabs(f.y - was.y) > maxRise)
				what = "jumped " + std::to_string(f.y - was.y) + " in height in one step";
			if (what.empty() && std::fabs(f.y - was.y) > 1.0)
			{
				// the user's shot: down the ramp and back up to the ledge within moments is a flip-flop, whatever each step's size
				lastJumpStep.resize(ids.size(), -1000);
				lastJumpDir.resize(ids.size(), 0);
				const int dir = f.y > was.y ? 1 : -1;
				if (lastJumpDir[i] == -dir && n - lastJumpStep[i] < 40)
					what = "flip-flopped between two heights (y " + std::to_string(was.y) + " -> " + std::to_string(f.y) + ", the opposite jump " + std::to_string(n - lastJumpStep[i]) + " steps ago)";
				lastJumpStep[i] = n;
				lastJumpDir[i] = dir;
			}
			if (!what.empty())
			{
				what += " [corrections " + std::to_string(corrections) + " state " + std::to_string(static_cast<int>(f.state)) + " drive " + std::to_string(static_cast<int>(f.drive)) + " fwd " + std::to_string(f.forwardSpeed) + " lat " + std::to_string(f.lateralSpeed) + " at " + std::to_string(f.x) + " " + std::to_string(f.z) + " y " + std::to_string(f.y) + ", before " + std::to_string(was.x) + " " + std::to_string(was.z) + " y " + std::to_string(was.y) + "]";
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

// The ledge with the most upper floor behind it: a flat straight edge (probes 10 u to each side stop at the same distance), a lower floor under the far
// side of it, and at least 40 u of upper floor behind the edge point. The returned wall point is the edge; y is the upper floor's.
Wall findLedge(const MeshNavigator& nav)
{
	Wall best;
	double bestRoom = 0.0;
	for (double z = kAreaZ - kAreaRadius; z <= kAreaZ + kAreaRadius; z += 3.0)
	{
		for (double x = kAreaX - kAreaRadius; x <= kAreaX + kAreaRadius; x += 3.0)
		{
			const auto up = nav.walker().surfaceY(x, kUpperY, z, 3.0);
			if (!up || *up < kUpperY - 8.0 || *up > kUpperY + 8.0)
				continue;
			for (int k = 0; k < 24; ++k)
			{
				const double angle = k * 0.26179938779914946;
				const double ux = std::sin(angle);
				const double uz = std::cos(angle);
				const auto probe = [&](double side, double dir)
				{
					return nav.walker().step({x - uz * side, *up, z + ux * side}, dir * ux * 400.0, dir * uz * 400.0, false);
				};
				const knav::StepResult mid = probe(0.0, 1.0);
				const knav::StepResult left = probe(10.0, 1.0);
				const knav::StepResult right = probe(-10.0, 1.0);
				if (!mid.blocked || !left.blocked || !right.blocked || mid.moved > 12.0)
					continue;
				if (std::fabs(left.moved - mid.moved) > 2.0 || std::fabs(right.moved - mid.moved) > 2.0 || std::fabs(mid.at.y - *up) > 3.0)
					continue;
				const double ex = mid.at.x;
				const double ez = mid.at.z;
				const auto below = nav.walker().surfaceY(ex + ux * 6.0, 1580.0, ez + uz * 6.0, 14.0);
				if (!below || *below > *up - 30.0)
					continue;
				const knav::StepResult back = nav.walker().step({ex, mid.at.y, ez}, -ux * 400.0, -uz * 400.0, false);
				if (back.moved > bestRoom)
				{
					bestRoom = back.moved;
					best = {ex, ez, ux, uz, mid.at.y};
				}
			}
		}
	}
	test::require(bestRoom >= 40.0, "no ledge with 40 u of floor behind it near the Bar (best " + std::to_string(bestRoom) + ")");
	return best;
}

struct StairLedge
{
	knav::Position edge;
	double ux = 0.0; // toward the ledge
	double uz = 0.0;
	knav::Position over; // 2 u past the edge, at the edge's height
	double nearestY = 0.0; // where the plain nearest-point snap of `over` goes
};

// Ledges whose nearest-point snap from 2 u past the edge lands on another level (more than 6 u up or down) while the edge itself is 2 u away.
std::vector<StairLedge> findStairLedges(const MeshNavigator& nav, std::size_t most)
{
	std::vector<StairLedge> found;
	for (double z = 2400.0; z <= 2900.0 && found.size() < most; z += 3.0)
	{
		for (double x = -51400.0; x <= -50950.0 && found.size() < most; x += 3.0)
		{
			const auto s = nav.walker().snap({x, kUpperY, z}, 30.0);
			if (!s)
				continue;
			for (int k = 0; k < 16; ++k)
			{
				const double ux = std::sin(k * 0.3926990817);
				const double uz = std::cos(k * 0.3926990817);
				const knav::StepResult r = nav.walker().step(*s, ux * 400.0, uz * 400.0, false);
				if (!r.blocked || r.moved > 3.0)
					continue;
				// 3 u past the edge, not 2: Detour prefers a polygon far below whose footprint is under the point (its distance is the height beyond a
				// climb) to a floor edge 2 u away once the stair is more than 7 u tall; the exact-height bake (k16.7.36) put this flight at 7.3.
				const knav::Position over{r.at.x + ux * 3.0, r.at.y, r.at.z + uz * 3.0};
				const auto nearest = nav.walker().snap(over, 256.0);
				const auto own = nav.walker().snap(over, 256.0, nullptr, 4.0);
				if (!nearest || !own || std::fabs(nearest->y - r.at.y) <= 6.0 || std::fabs(own->y - r.at.y) > 4.0)
					continue;
				bool spread = true;
				for (const StairLedge& other : found)
					spread = spread && std::hypot(other.edge.x - r.at.x, other.edge.z - r.at.z) > 25.0;
				if (spread)
				{
					found.push_back({r.at, ux, uz, over, nearest->y});
					break;
				}
			}
		}
	}
	return found;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_fight_levels HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");
		const Wall wall = findLedge(*nav);
		std::printf("ledge at (%.1f, %.1f) y %.1f, toward (%.2f, %.2f), %.1f u from the Bar\n", wall.x, wall.z, wall.y, wall.ux, wall.uz,
			std::hypot(wall.x - kAreaX, wall.z - kAreaZ));

		// ---- 1. a pile at the ledge: the depenetration push never carries anyone off it ----
		{
			Rig rig(gd, data, nav, wall, 5);
			for (int i = 0; i < 8; ++i)
				rig.spawn(2.0 + 0.3 * i, 0.4 * i - 1.5, 0, false);
			rig.run(600);
			test::require(rig.failure.empty(), "pile: " + rig.failure);
			std::printf("ok  pile: eight fighters stacked at the ledge for 30 s, none crossed (y %.1f..%.1f)\n", rig.yLo, rig.yHi);
		}

		// ---- 2. a fight with a back to the ledge ----
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
			std::printf("ok  fight seed %llu: 60 s, back to the ledge, held fighter %s, nobody changed level or teleported (y %.1f..%.1f)\n",
				static_cast<unsigned long long>(seed), rig.me(held).dead ? "dead" : "alive", rig.yLo, rig.yHi);
		}

		// ---- 3. knocked out at the ledge, bodies piled on it ----
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
			std::printf("ok  bodies: six knocked-out bodies piled at the ledge for 30 s, none crossed (y %.1f..%.1f)\n", rig.yLo, rig.yHi);
		}
		// ---- 4. put off the edge: correct() brings a fighter back to its own level ----
		{
			Rig rig(gd, data, nav, wall, 5);
			const std::uint64_t over = rig.spawn(0.0, 0.0, 0, false);
			rig.me(over).x = wall.x + wall.ux * 3.0; // past the edge, at the upper floor's height
			rig.me(over).z = wall.z + wall.uz * 3.0;
			rig.last[0] = rig.me(over);
			rig.run(40);
			test::require(rig.failure.empty(), "put off the edge: " + rig.failure);
			std::printf("ok  put off the edge: 3 u past it, back on the upper floor (y %.1f..%.1f)\n", rig.yLo, rig.yHi);
		}
		// ---- 5. beside a stair flight: the fighter goes back to its own floor, not onto the stairs ----
		{
			const std::vector<StairLedge> ledges = findStairLedges(*nav, 3);
			test::require(!ledges.empty(), "no stair-side ledge near the Bar");
			for (const StairLedge& ledge : ledges)
			{
				for (const double past : {2.0, 6.0, 12.0})
				{
					Wall here = wall;
					here.y = ledge.edge.y;
					Rig rig(gd, data, nav, here, 5);
					const std::uint64_t id = rig.spawn(0.0, 0.0, 0, false);
					rig.me(id).x = ledge.edge.x + ledge.ux * past;
					rig.me(id).y = ledge.edge.y;
					rig.me(id).z = ledge.edge.z + ledge.uz * past;
					for (int i = 0; i < 20; ++i)
						rig.arena.step(kDt); // the first step's correct() is the whole move: not a walk, so not the Rig's step limits
					const state::Fighter& f = rig.me(id);
					const bool ok = nav->walker().onMesh({f.x, f.y, f.z}) && std::fabs(f.y - ledge.edge.y) <= kStairSlack;
					std::printf("%s  stair ledge (%.1f, %.1f) y %.1f, set %.0f u past the edge: now y %.1f, %.1f u back (the nearest point of the mesh at 3 u is on y %.1f)\n",
						ok ? "ok " : "BAD", ledge.edge.x, ledge.edge.z, ledge.edge.y, past, f.y, std::hypot(f.x - ledge.edge.x, f.z - ledge.edge.z), ledge.nearestY);
					test::require(ok, "a fighter set " + std::to_string(past) + " u past a stair-side ledge ended on y " + std::to_string(f.y) + ", not its own floor (y " + std::to_string(ledge.edge.y) + ")");
				}
			}
		}
		std::printf("ksim_fight_levels: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
