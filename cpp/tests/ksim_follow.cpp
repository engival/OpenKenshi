// The follow task and the fight join (docs/reference/SQUAD_FOLLOW.md s4, s7, s9): a follower keeps its slot,
// the 180-degree heading case (original behaviour, and the squad.follow_heading_fix option), direct and path
// steering with their hysteresis, PROTECT_ALLIES (idle members only, never sneaking or following ones,
// within range), determinism and a save in the middle of a follow.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;

struct Rig
{
	Simulation sim;
	std::shared_ptr<const CombatData> data;
	const kdata::GameData& gd;
	std::shared_ptr<MeshNavigator> nav;
	std::string sid;
	std::uint64_t rng = 12345;

	Rig(const std::shared_ptr<const CombatData>& d, const kdata::GameData& g,
		const std::shared_ptr<MeshNavigator>& n, bool headingFix = false, int formation = 0)
		: sim(Simulation::load(KENSHI_DATA, 77))
		, data(d)
		, gd(g)
		, nav(n)
	{
		CombatSettings settings;
		settings.substeps = 2;
		settings.squadFollowHeadingFix = headingFix;
		settings.squadFormation = formation;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute)); // 10:00, full daylight
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
	}

	// A character on the mesh point nearest (x, z); it must be within 6 units of it.
	// `yaw` is the facing (0 = +z, towards +x): perception only sees what is in front of a character.
	std::uint64_t spawn(double x, double z, int team = 0, bool player = true, double yaw = 0.0)
	{
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.team = team;
		f.yaw = yaw;
		f.playerControlled = player;
		const auto spot = nav->walker().snap({x, 1535.0, z}, 200.0);
		test::require(spot.has_value() && std::hypot(spot->x - x, spot->z - z) < 6.0, "no mesh at the spawn spot");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	// A spot at `distance` from (cx, cz) on the mesh, trying directions from `first`.
	bool spotAt(double cx, double cz, double distance, double& x, double& z, int first = 0)
	{
		for (int k = 0; k < 64; ++k)
		{
			const double a = (first + k) * 0.0981748;
			x = cx + std::sin(a) * distance;
			z = cz + std::cos(a) * distance;
			const auto spot = nav->walker().snap({x, 1535.0, z}, 20.0);
			if (spot && std::hypot(spot->x - x, spot->z - z) < 1.0)
				return true;
		}
		return false;
	}

	const state::Fighter& at(std::uint64_t id) const { return *sim.combat()->find(id); }
	double dist(std::uint64_t a, std::uint64_t b) const
	{
		return std::hypot(at(a).x - at(b).x, at(a).z - at(b).z);
	}
	void follow(std::uint64_t id, std::uint64_t leader) { sim.enqueue(Command::fighterFollow(state::Id{id}, state::Id{leader})); }
	void run(int ticks) { sim.run(static_cast<std::uint64_t>(ticks)); }
	// Direct drive at heading `a` (radians).
	void drive(std::uint64_t id, double a) { sim.enqueue(Command::fighterDrive(state::Id{id}, 1, a, 1)); }
};

// A heading along which a driven character runs `length` units from the Hub spot without being blocked.
double openHeading(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd,
	const std::shared_ptr<MeshNavigator>& nav, double length)
{
	for (int k = 0; k < 16; ++k)
	{
		Rig probe(data, gd, nav);
		const std::uint64_t id = probe.spawn(kHubX, kHubZ);
		const double a = k * kPi / 8.0;
		probe.drive(id, a);
		probe.run(7 * 20); // a drive starts slowly: the footwork ramp is 15 u/s^2
		if (std::hypot(probe.at(id).x - kHubX, probe.at(id).z - kHubZ) >= length)
			return a;
	}
	throw test::Failure("no open heading near the Hub");
}

double headingDot(const state::Fighter& f, double dirX, double dirZ)
{
	const double h = std::hypot(f.followHx, f.followHz);
	return h > 0.0 ? (f.followHx * dirX + f.followHz * dirZ) / h : 0.0;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_follow HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const double heading = openHeading(data, gd, nav, 250.0);

		// ---- an unordered member stays put; a follower keeps its slot while the leader walks and stops ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z), "no spot beside the leader");
			const std::uint64_t follower = rig.spawn(x, z);
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z, 20), "no second spot beside the leader");
			const std::uint64_t idle = rig.spawn(x, z);
			const double ix = rig.at(idle).x;
			const double iz = rig.at(idle).z;
			rig.follow(follower, leader);
			rig.drive(leader, heading);
			rig.run(6 * 20); // 6 s of walking
			test::require(rig.at(follower).followTarget == leader, "follow order not applied");
			test::require(std::hypot(rig.at(idle).x - ix, rig.at(idle).z - iz) < 0.01, "an unordered member moved");
			const double apart = rig.dist(leader, follower);
			std::cout << "walking: follower " << apart << " u from the leader\n";
			test::require(apart > 4.0 && apart < 60.0, "the follower is not at its slot while walking");
			const double seen = std::atan2(rig.at(follower).followHx, rig.at(follower).followHz);
			test::require(std::fabs(std::remainder(seen - heading, 2.0 * kPi)) < 0.2, "the smoothed heading does not follow the leader");
			rig.sim.enqueue(Command::fighterStop(state::Id{leader}, false));
			rig.run(15 * 20);
			// A stopped leader: the frozen heading, the follower at its random-mode slot. The disc of
			// radius sqrt(n x 100) only seeds the draw; the push-out sets the slot on the leader ring
			// |o| = 1.5 x s + s + 15 = 25 at a random angle (kinspect f834513; live 2-member 25.0).
			const state::Fighter& f = rig.at(follower);
			const state::Fighter& l = rig.at(leader);
			double ox = 0.0;
			double oz = 0.0;
			rig.sim.combat()->formationOffset(follower, leader, ox, oz);
			const double orad = std::hypot(ox, oz);
			test::require(orad >= 25.0 - 1e-6 && orad <= 25.0 + 1e-6,
				"the 2-member random offset is not the leader ring radius 25 (1.5 s + s + 15)");
			const double slotX = l.x - f.followHx * ox + f.followHz * oz;
			const double slotZ = l.z - f.followHz * ox - f.followHx * oz;
			const double off = std::hypot(f.x - slotX, f.z - slotZ);
			std::cout << "stopped: follower " << off << " u from its slot, " << rig.dist(leader, follower) << " from the leader\n";
			test::require(off < 20.5, "a stopped leader's follower is not within the speed table's dead zone (20) of its slot");
		}

		// ---- direct steer (rwh110): facing = velocity while moving, the approach facing kept at the slot ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			std::vector<std::uint64_t> members;
			for (int i = 0; i < 3; ++i)
			{
				double x = 0.0;
				double z = 0.0;
				test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z, i * 20), "no spot for a steer member");
				members.push_back(rig.spawn(x, z, 0, true, i * 2.0)); // facings off the way in
				rig.follow(members.back(), leader);
			}
			rig.drive(leader, heading);
			std::vector<double> px, pz, last;
			for (const std::uint64_t id : members)
			{
				px.push_back(rig.at(id).x);
				pz.push_back(rig.at(id).z);
				last.push_back(rig.at(id).yaw);
			}
			double worst = 0.0;
			int fast = 0;
			for (int t = 0; t < 8 * 20; ++t)
			{
				rig.run(1);
				for (std::size_t i = 0; i < members.size(); ++i)
				{
					const state::Fighter& f = rig.at(members[i]);
					const double vx = f.x - px[i];
					const double vz = f.z - pz[i];
					px[i] = f.x;
					pz[i] = f.z;
					const double speed = std::hypot(vx, vz) * 20.0;
					// outside turns: the velocity heading is steady over two ticks
					if (speed > 5.0 && f.followDirect && std::fabs(std::remainder(std::atan2(vx, vz) - last[i], 2.0 * kPi)) < 0.1)
					{
						++fast;
						worst = std::max(worst, std::fabs(std::remainder(f.yaw - std::atan2(vx, vz), 2.0 * kPi)));
					}
					if (speed > 0.1)
						last[i] = std::atan2(vx, vz);
				}
			}
			std::cout << "steer: " << fast << " fast direct samples, worst |yaw - velocity| " << worst << " rad\n";
			test::require(fast > 20, "no member ran in direct steer above 5 u/s");
			test::require(worst < 0.26, "a member above 5 u/s faces more than 15 degrees off its velocity");
			rig.sim.enqueue(Command::fighterStop(state::Id{leader}, false));
			for (int t = 0; t < 20 * 20; ++t)
			{
				rig.run(1);
				for (std::size_t i = 0; i < members.size(); ++i)
				{
					const state::Fighter& f = rig.at(members[i]);
					const double vx = f.x - px[i];
					const double vz = f.z - pz[i];
					px[i] = f.x;
					pz[i] = f.z;
					if (std::hypot(vx, vz) * 20.0 > 0.1)
						last[i] = std::atan2(vx, vz);
				}
			}
			for (std::size_t i = 0; i < members.size(); ++i)
				test::require(std::fabs(std::remainder(rig.at(members[i]).yaw - last[i], 2.0 * kPi)) < 0.05,
					"a member at its slot does not keep its last approach heading");
		}

		// ---- a group move order (k16.6.182): four members each walk their own scatter route; yaw follows the velocity, no strafe ----
		{
			Rig rig(data, gd, nav);
			std::vector<std::uint64_t> members;
			for (int i = 0; i < 4; ++i)
			{
				double x = 0.0;
				double z = 0.0;
				test::require(rig.spotAt(kHubX, kHubZ, 8.0, x, z, i * 16), "no spot for a walker");
				members.push_back(rig.spawn(x, z));
			}
			std::uint64_t scatter = 99;
			const auto uniform01 = [&scatter]() { scatter = scatter * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<double>(scatter >> 11) / 9007199254740992.0; };
			std::vector<double> px, pz, sx, sz;
			for (std::size_t i = 0; i < members.size(); ++i)
			{
				double gx = kHubX + 220.0 * std::sin(heading);
				double gz = kHubZ + 220.0 * std::cos(heading);
				if (i != 0)
				{
					const double radius = uniform01() * std::sqrt(4.0 * 60.0) + 5.0;
					const double angle = uniform01() * 2.0 * kPi;
					gx += radius * std::sin(angle);
					gz += radius * std::cos(angle);
				}
				const state::Fighter& f = rig.at(members[i]);
				Route route = nav->routeFrom({f.x, f.y, f.z}, gx, gz);
				test::require(route.found, "no route for a walker");
				rig.sim.enqueue(Command::fighterMoveTo(state::Id{members[i]}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
				px.push_back(f.x);
				pz.push_back(f.z);
				sx.push_back(f.x);
				sz.push_back(f.z);
			}
			std::vector<std::vector<double>> dy(members.size());
			std::vector<double> lateral(members.size(), 0.0);
			std::vector<double> offLine(members.size(), 0.0);
			for (int t = 0; t < 30 * 20; ++t)
			{
				std::vector<bool> walking;
				for (const std::uint64_t id : members)
					walking.push_back(!rig.at(id).route.empty());
				rig.run(1);
				for (std::size_t i = 0; i < members.size(); ++i)
				{
					const state::Fighter& f = rig.at(members[i]);
					const double vx = f.x - px[i];
					const double vz = f.z - pz[i];
					px[i] = f.x;
					pz[i] = f.z;
					lateral[i] = std::max(lateral[i], std::fabs(f.lateralSpeed));
					if (walking[i] && std::hypot(vx, vz) * 20.0 > 5.0)
						dy[i].push_back(std::fabs(std::remainder(f.yaw - std::atan2(vx, vz), 2.0 * kPi)) * 180.0 / kPi);
				}
			}
			for (std::size_t i = 0; i < members.size(); ++i)
			{
				std::sort(dy[i].begin(), dy[i].end());
				const double p95 = dy[i].empty() ? 0.0 : dy[i][dy[i].size() * 95 / 100];
				std::cout << "walk " << i << ": " << dy[i].size() << " moving ticks, |yaw - velocity| p95 " << p95 << " max "
					<< (dy[i].empty() ? 0.0 : dy[i].back()) << " deg, lateral " << lateral[i] << ", moved "
					<< std::hypot(px[i] - sx[i], pz[i] - sz[i]) << "\n";
				test::require(dy[i].size() > 20, "a walker did not walk");
				test::require(p95 < 5.0, "a walking member's yaw is more than 5 degrees off its velocity at p95");
				test::require(lateral[i] == 0.0, "a walking member has lateral speed");
			}
		}

		// ---- the formation layouts (kinspect 0789d56): Random's disc and push-out, the Caravan gaps ----
		{
			// Random (0): the disc of radius sqrt(n x 100) seeds the draw; the push-out sets the slot:
			// 25 against the fixed leader, 17 member-member (kinspect f834513; live 2-member 25.0).
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			std::vector<std::uint64_t> members;
			for (int i = 0; i < 7; ++i)
			{
				double x = 0.0;
				double z = 0.0;
				test::require(rig.spotAt(kHubX, kHubZ, 6.0, x, z, i * 9), "no spot for member " + std::to_string(i));
				members.push_back(rig.spawn(x, z));
			}
			for (const std::uint64_t id : members)
				rig.follow(id, leader);
			rig.run(1); // the follow orders apply
			const double reach = std::sqrt(8.0 * 100.0);
			std::vector<std::pair<double, double>> offsets;
			for (const std::uint64_t id : members)
			{
				double ox = 0.0;
				double oz = 0.0;
				rig.sim.combat()->formationOffset(id, leader, ox, oz);
				offsets.emplace_back(ox, oz);
				test::require(std::hypot(ox, oz) <= reach + 1.0,
					"a random-mode member lies outside the disc of radius sqrt(n x 100)");
			}
			for (std::size_t i = 0; i < offsets.size(); ++i)
				for (std::size_t j = i + 1; j < offsets.size(); ++j)
					test::require(std::hypot(offsets[i].first - offsets[j].first, offsets[i].second - offsets[j].second)
							>= 17.0 - 1e-3,
						"two random-mode members are closer than the member-member push-out (1.5 s + 1.5 s + 5 = 17)");
			// Caravan (1): a single file behind the leader, gap = rand(10, 20) + r_prev + r_i
			// (r = 6), |z| <= 1.5 (live: Nomads 16, gaps 21..35.5).
			Rig caravanRig(data, gd, nav, false, 1);
			const std::uint64_t cLeader = caravanRig.spawn(kHubX, kHubZ);
			std::vector<std::uint64_t> file;
			for (int i = 0; i < 4; ++i)
			{
				double x = 0.0;
				double z = 0.0;
				test::require(caravanRig.spotAt(kHubX, kHubZ, 6.0, x, z, i * 9), "no caravan spot " + std::to_string(i));
				file.push_back(caravanRig.spawn(x, z));
			}
			for (const std::uint64_t id : file)
				caravanRig.follow(id, cLeader);
			caravanRig.run(1);
			double prevX = 0.0;
			for (std::size_t i = 0; i < file.size(); ++i)
			{
				double ox = 0.0;
				double oz = 0.0;
				caravanRig.sim.combat()->formationOffset(file[i], cLeader, ox, oz);
				const double gap = ox - prevX;
				test::require(gap >= 22.0 - 1e-6 && gap <= 32.0 + 1e-6,
					"a caravan gap lies outside rand(10, 20) + 2r = 22..32");
				test::require(std::fabs(oz) <= 1.5 + 1e-9, "a caravan member is more than 1.5 off the file line");
				prevX = ox;
			}
		}

		// ---- the 180-degree reversal: stale heading with the option off, fixed with it on ----
		for (const bool fix : {false, true})
		{
			Rig rig(data, gd, nav, fix);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 30.0, x, z), "no spot beside the leader");
			const std::uint64_t follower = rig.spawn(x, z);
			rig.follow(follower, leader);
			rig.drive(leader, heading);
			rig.run(4 * 20);
			const double dirX = std::sin(heading);
			const double dirZ = std::cos(heading);
			test::require(headingDot(rig.at(follower), dirX, dirZ) > 0.99, "heading not on the leader's way before the turn");
			rig.drive(leader, heading + kPi);
			// Two seconds isolates the reversal law. The 25 u ring brings the stale-heading follower
			// head-on into the leader in the first second (minimum separation 5.55 < 8; leader velocity
			// dot 0.24); by 5 s that contact's heading drift, not the reversal, decides the old check.
			rig.run(2 * 20);
			const double after = headingDot(rig.at(follower), -dirX, -dirZ); // against the new direction
			std::cout << "reversal, option " << (fix ? "on" : "off") << ": heading . new direction = " << after << "\n";
			if (fix)
				test::require(after > 0.95, "the fix did not turn the heading");
			else
				test::require(after < -0.5, "the original's heading did not stay stale after a 180-degree turn");
		}

		// ---- steering mode: path beyond 100, direct below 50, hysteresis between ----
		// One follower per leader; with a fresh heading (-X forward) its slot is the leader plus the
		// offset in world xz.
		for (const double start : {75.0, 220.0})
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 5.0, x, z), "no spot beside the leader");
			const std::uint64_t follower = rig.spawn(x, z);
			rig.follow(follower, leader);
			rig.run(1); // the follow order applies, so the offset below is the member's real one
			double ox = 0.0;
			double oz = 0.0;
			rig.sim.combat()->formationOffset(follower, leader, ox, oz);
			const double slotX = rig.at(leader).x + ox;
			const double slotZ = rig.at(leader).z + oz;
			test::require(rig.spotAt(slotX, slotZ, start, x, z, 7), "no spot " + std::to_string(start) + " from the slot");
			{
				state::Fighter* f = const_cast<CombatArena*>(rig.sim.combat())->find(follower);
				const auto spot = nav->walker().snap({x, f->y, z}, 20.0);
				test::require(spot.has_value() && std::hypot(spot->x - x, spot->z - z) < 1.0, "no mesh at the teleport spot");
				f->x = spot->x;
				f->y = spot->y;
				f->z = spot->z;
			}
			rig.run(1);
			const state::Fighter& f = rig.at(follower);
			if (start < 100.0)
			{
				test::require(f.followDirect && f.route.empty(), "a fresh task in the band does not steer direct (the flag starts set)");
				continue;
			}
			test::require(!f.followDirect && f.routeChase && !f.route.empty(), "a member beyond 100 does not path-find");
			double flipAt = -1.0;
			double lowest = 1.0e9;
			for (int i = 0; i < 60 * 20 && flipAt < 0.0; ++i)
			{
				rig.run(1);
				const double d = std::hypot(f.x - slotX, f.z - slotZ);
				if (f.followDirect)
					flipAt = d;
				else
					lowest = std::min(lowest, d);
			}
			std::cout << "path -> direct at " << flipAt << " u from the slot (path mode held down to " << lowest << ")\n";
			test::require(lowest > 40.0 && flipAt > 0.0 && flipAt <= 50.5, "path mode ended away from 50 (the hysteresis is broken)");
			rig.run(1);
			test::require(f.route.empty(), "the route is not cleared in direct mode");
		}

		// ---- the fight join ----
		{
			Rig rig(data, gd, nav);
			const auto towardsFight = [](double fromX, double fromZ) { return std::atan2(kHubX - fromX, kHubZ - fromZ); };
			const std::uint64_t victim = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z), "no spot for the enemy");
			const std::uint64_t enemy = rig.spawn(x, z, 1, false, towardsFight(x, z));
			const auto ring = [&](double distance, int first)
			{
				test::require(rig.spotAt(kHubX, kHubZ, distance, x, z, first), "no spot at " + std::to_string(distance));
				return rig.spawn(x, z, 0, true, towardsFight(x, z));
			};
			const std::uint64_t idle = ring(300.0, 3);
			const std::uint64_t sneaker = ring(300.0, 11);
			const std::uint64_t follower = ring(300.0, 19);
			const std::uint64_t walker = ring(300.0, 27);
			rig.sim.enqueue(Command::fighterSneak(state::Id{sneaker}, true));
			rig.follow(follower, victim);
			double mx = 0.0;
			double mz = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 200.0, mx, mz, 40), "no walk destination");
			Route route = nav->routeFrom({rig.at(walker).x, rig.at(walker).y, rig.at(walker).z}, mx, mz);
			test::require(route.found, "no route for the walker");
			rig.sim.enqueue(Command::fighterMoveTo(state::Id{walker}, std::move(route.points), std::move(route.heights), 5));
			rig.run(100); // 5 s: the enemy's first scan (<= 0.5 s), then the members' (<= 0.5 s each)
			test::require(rig.at(enemy).target == victim, "the hostile did not pick the victim");
			test::require(rig.at(victim).target == enemy, "the attacked member did not fight back");
			test::require(rig.at(idle).target == enemy, "an idle member within range did not join");
			test::require(rig.at(sneaker).target == 0, "a sneaking member joined");
			test::require(rig.at(follower).target == 0, "a following member joined");
			test::require(rig.at(walker).target == 0, "a member on a move order joined");
			test::require(rig.at(follower).followTarget == victim && rig.at(sneaker).sneak, "orders lost");
		}
		// The join range is the sight geometry, not a distance (PERCEPTION_LIVE.md; the retired proxy cut it at 880). 1000 units
		// away on a clear day: a member facing the fight sees the attacker (R 1500, c near 1) and joins; one facing away does not
		// (behind the cone), and one whose view is turned 70 degrees off needs c >= 0.67 (dot >= 0.74) so does not either.
		{
			Rig rig(data, gd, nav);
			const std::uint64_t victim = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z), "no spot for the enemy");
			const std::uint64_t enemy = rig.spawn(x, z, 1, false, std::atan2(kHubX - x, kHubZ - z));
			test::require(rig.spotAt(kHubX, kHubZ, 1000.0, x, z, 5), "no spot 1000 away");
			const double toFight = std::atan2(kHubX - x, kHubZ - z);
			const std::uint64_t facing = rig.spawn(x, z, 0, true, toFight);
			const std::uint64_t away = rig.spawn(x, z, 0, true, toFight + 3.14159265358979);
			const std::uint64_t off = rig.spawn(x, z, 0, true, toFight + 1.22);
			rig.run(100);
			test::require(rig.at(enemy).target == victim, "the hostile did not pick the victim");
			test::require(rig.at(facing).target == enemy, "a member facing the fight 1000 away did not join");
			test::require(rig.at(away).target == 0, "a member facing away from the fight joined");
			test::require(rig.at(off).target == 0, "a member 70 degrees off the fight (outside the reach there) joined");
		}

		// ---- SELF_PRESERVATION (goal 32; kinspect f5784bb + c1a305f): a player answers an attacker on ITSELF only
		// within 150 u (a placeholder, UNSURE: live flipped at 146 u after bandits picked it at 374-462 u), at once
		// after a hit at any range, and under a follow order (goal 32 presumably outranks the task; UNSURE) ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t player = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 450.0, x, z, 3), "no spot 450 u from the player");
			const std::uint64_t bandit = rig.spawn(x, z, 1, false, std::atan2(kHubX - x, kHubZ - z));
			const_cast<state::Fighter&>(*rig.sim.combat()->find(player)).yaw = std::atan2(rig.at(bandit).x - rig.at(player).x, rig.at(bandit).z - rig.at(player).z);
			for (int i = 0; i < 60 * 10 && rig.at(bandit).target != player; ++i)
				rig.run(1);
			test::require(rig.at(bandit).target == player, "the bandit did not pick the player 450 u away");
			std::cout << "self-defence: the bandit picked the player at " << rig.dist(player, bandit) << " u, player target " << rig.at(player).target << "\n";
			test::require(rig.at(player).target == 0, "the player answered a 450 u attacker at once");
			double flipAt = -1.0;
			for (int i = 0; i < 60 * 120 && flipAt < 0.0; ++i)
			{
				const double before = rig.dist(player, bandit);
				rig.run(1);
				if (rig.at(player).target == bandit)
					flipAt = before;
				else
					test::require(rig.at(player).target == 0, "the player took a target other than its attacker");
			}
			test::require(flipAt > 0.0, "the player never answered the attacker");
			test::require(flipAt <= 150.0, "the player answered at " + std::to_string(flipAt) + " u, before the self-defence range");
			std::cout << "self-defence: the player answered its attacker at " << flipAt << " u\n";
			// A hit answers the same attacker at any range: the live shot did; ranged combat is not modelled, so here
			// the bandit lands one at melee, then moves back out past the gate.
			const auto hitByBandit = [&]
			{
				for (const CombatArena::Member& m : rig.sim.combat()->members())
					if (m.id == player)
						return std::find(m.hitBy.begin(), m.hitBy.end(), bandit) != m.hitBy.end();
				return false;
			};
			for (int i = 0; i < 60 * 180 && !hitByBandit(); ++i)
				rig.run(1);
			test::require(hitByBandit(), "the bandit never landed a hit on the player");
			test::require(!rig.at(bandit).dead && !rig.at(bandit).unconscious, "the bandit went down before the range check");
			double bx = 0.0;
			double bz = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 400.0, bx, bz, 3), "no spot 400 u from the player");
			state::Fighter& b = const_cast<state::Fighter&>(*rig.sim.combat()->find(bandit));
			b.x = bx;
			b.z = bz;
			b.clearRoute();
			// orderStop drops the target at the next update; the pick that follows answers the attacker that hit it,
			// once the hit reaction that marks it (state 8 holds the pick off) has played out.
			rig.sim.enqueue(Command::fighterStop(state::Id{player}, false));
			rig.run(1); // applies the stop (orderStop): the target is dropped before any pick can answer
			const bool dropped = rig.at(player).target == 0;
			int ticks = 1;
			while (ticks < 120 && rig.at(player).target != bandit)
			{
				rig.run(1);
				++ticks;
			}
			const double back = rig.dist(player, bandit);
			test::require(rig.at(player).target == bandit, "a hit did not let the player answer at range");
			test::require(back > 150.0, "the re-acquisition was not across the self-defence range");
			std::cout << "self-defence: after a hit the player answered its attacker at " << back << " u after " << ticks << " ticks ("
				<< (dropped ? "the stop dropped the target first" : "the pick ran in the stop tick") << ")\n";
			// The hit memory lasts the fight it was made in: with the bandit's fight over (no target, nothing engaged on
			// it) and the player's target dropped, the stale hit must not answer it any more.
			b.target = 0;
			b.engaged.clear();
			rig.sim.enqueue(Command::fighterStop(state::Id{player}, false));
			rig.run(2);
			test::require(!hitByBandit(), "a hit was remembered past the end of the fight");
		}
		// A player's target that dies is dropped: the player branch kept `f.target` on the corpse (no live enemy left, yet the
		// wire's combat flag stayed on and the client held the combat pose; user playtest 2026-11-05).
		{
			Rig rig(data, gd, nav);
			const std::uint64_t player = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 60.0, x, z, 3), "no spot 60 u from the player");
			const std::uint64_t bandit = rig.spawn(x, z, 1, false, std::atan2(kHubX - x, kHubZ - z));
			const_cast<state::Fighter&>(*rig.sim.combat()->find(player)).yaw = std::atan2(x - kHubX, z - kHubZ);
			for (int i = 0; i < 60 * 20 && rig.at(player).target != bandit; ++i)
				rig.run(1);
			test::require(rig.at(player).target == bandit, "the player never answered the bandit at 60 u");
			const_cast<state::Fighter&>(*rig.sim.combat()->find(bandit)).dead = true;
			rig.run(20);
			test::require(rig.at(player).target == 0, "the player kept its dead attacker as a target (combat flag stuck)");
			std::cout << "dead target: dropped\n";
		}
		// A following player answers an attacker inside the range: goal 32 drops the follow task (UNSURE whether the
		// original resumes it).
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 40.0, x, z), "no spot for the follower");
			const std::uint64_t follower = rig.spawn(x, z);
			rig.follow(follower, leader);
			// The bandit on the same ray, 100 u beyond the follower (140 from the leader): it picks the nearer one.
			test::require(rig.spotAt(kHubX, kHubZ, 140.0, x, z), "no spot for the bandit");
			const std::uint64_t bandit = rig.spawn(x, z, 1, false, std::atan2(rig.at(follower).x - x, rig.at(follower).z - z));
			const_cast<state::Fighter&>(*rig.sim.combat()->find(follower)).yaw = std::atan2(x - rig.at(follower).x, z - rig.at(follower).z);
			for (int i = 0; i < 60 * 10 && rig.at(bandit).target != follower; ++i)
				rig.run(1);
			test::require(rig.at(bandit).target == follower, "the bandit did not pick the following player");
			const double apart = rig.dist(follower, bandit);
			for (int i = 0; i < 20 * 5 && rig.at(follower).target != bandit; ++i)
				rig.run(1);
			test::require(rig.at(follower).target == bandit, "a following player did not answer its attacker within range");
			test::require(rig.at(follower).followTarget == 0, "goal 32 did not drop the follow task");
			std::cout << "self-defence: the following player answered at " << apart << " u and the follow task was dropped\n";
		}

		// ---- followers mirror the leader's sneak; an order ends the follow ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			double x = 0.0;
			double z = 0.0;
			test::require(rig.spotAt(kHubX, kHubZ, 30.0, x, z), "no spot");
			const std::uint64_t follower = rig.spawn(x, z);
			rig.follow(follower, leader);
			rig.sim.enqueue(Command::fighterSneak(state::Id{leader}, true));
			rig.run(3);
			test::require(rig.at(follower).sneak, "the follower did not copy the leader's sneak");
			rig.sim.enqueue(Command::fighterSneak(state::Id{leader}, false));
			rig.run(3);
			test::require(!rig.at(follower).sneak, "the follower kept the sneak");
			rig.sim.enqueue(Command::fighterStop(state::Id{follower}, false));
			rig.run(2);
			test::require(rig.at(follower).followTarget == 0, "an order did not end the follow");
		}

		// ---- determinism, and a save in the middle of a follow ----
		const auto scripted = [&](Rig& r, int ticks)
		{
			if (r.sim.combat()->members().empty())
			{
				const std::uint64_t leader = r.spawn(kHubX, kHubZ);
				double x = 0.0;
				double z = 0.0;
				test::require(r.spotAt(kHubX, kHubZ, 30.0, x, z), "no spot");
				const std::uint64_t follower = r.spawn(x, z);
				r.follow(follower, leader);
				r.drive(leader, heading);
			}
			r.run(ticks);
		};
		Rig a(data, gd, nav);
		scripted(a, 160);
		Rig b(data, gd, nav);
		scripted(b, 160);
		test::require(a.sim.stateHash() == b.sim.stateHash(), "two identical follow runs differ");
		Rig split(data, gd, nav);
		scripted(split, 50);
		test::require(split.sim.combat()->members().back().fighter.followTarget != 0, "the save point is not mid-follow");
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_follow.save";
		split.sim.saveGame(path);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
		CombatSettings settings;
		settings.substeps = 2;
		loaded.enableCombat(data, settings);
		loaded.enableNavigation(nav);
		loaded.run(110);
		test::require(loaded.stateHash() == a.sim.stateHash(), "save mid-follow + load + run differs from the uninterrupted run");
		std::remove(path.c_str());

		// (k16.6.200's save under a squadmate push on the walk is retired with the push itself: a player's move walk takes none, k16.6.231,
		// ksim_group_settle. Fighter::walkHeading / walkFacing stay in the save, equal to the yaw.)

		// ---- a follower whose slot is behind a corner and moving re-plans on the slot's 2 u drift, not 30 u (k16.8.26, rwh.144 s1 "Dedup"): it ends
		// at its slot (about the leader's distance for its formation offset, as a follower that was never behind a corner) and the steer / route
		// choice does not flip-flop over 600 ticks ----
		{
			Rig rig(data, gd, nav);
			const std::uint64_t leader = rig.spawn(kHubX, kHubZ);
			bool found = false;
			double x = 0.0;
			double z = 0.0;
			double best = 1.1;
			double distance = 0.0;
			for (int first = 0; first < 64; ++first)
			{
				for (const double d : {60.0, 90.0, 120.0, 160.0, 200.0})
				{
					double sx = 0.0;
					double sz = 0.0;
					if (!rig.spotAt(kHubX, kHubZ, d, sx, sz, first))
						continue;
					const Route route = nav->routeFrom({sx, 1535.0, sz}, kHubX, kHubZ);
					double len = 0.0;
					for (std::size_t i = 1; i < route.points.size(); ++i)
						len += std::hypot(route.points[i].x - route.points[i - 1].x, route.points[i].z - route.points[i - 1].z);
					if (route.found && len > best * d)
					{
						best = len / d;
						found = true;
						x = sx;
						z = sz;
						distance = d;
					}
				}
			}
			test::require(found, "no spot behind a corner near the Hub");
			const std::uint64_t follower = rig.spawn(x, z);
			rig.follow(follower, leader);
			rig.drive(leader, heading);
			int switches = 0;
			bool was = true;
			for (int t = 0; t < 600; ++t)
			{
				rig.run(1);
				const bool onRoute = rig.at(follower).routeChase;
				if (t > 0 && onRoute != was)
					++switches;
				was = onRoute;
			}
			rig.sim.enqueue(Command::fighterDrive(state::Id{leader}, 0, 0.0, 2));
			rig.run(400);
			std::cout << "corner follower: " << switches << " steer/route switches, " << rig.dist(follower, leader) << " u from the leader (start " << distance << " u, route x" << best << ")\n";
			test::require(switches <= 6, "the follower flip-flops between steer and route");
			test::require(rig.dist(follower, leader) < 90.0,  "the follower stopped short of its slot");
		}

		// ---- a formation slot off the mesh (inside a solid) comes out clear of it (clearFormationSlot, k16.6.320) ----
		{
			Rig rig(data, gd, nav);
			const knav::Walker& walker = nav->walker();
			const double radius = data->pathRadius(data->makeFighter(gd, rig.sid, "", rig.rng).raceSid);
			int tried = 0;
			for (int k = 0; k < 64 && tried < 3; ++k)
			{
				const double a = k * 0.0981748;
				const knav::StepResult wall = walker.step({kHubX, 1535.0, kHubZ}, std::sin(a) * 300.0, std::cos(a) * 300.0);
				if (!wall.onMesh || !wall.blocked)
					continue;
				double ax = wall.at.x + std::sin(a) * 5.0; // 5 u into the solid beyond the mesh edge the walk stopped at
				double az = wall.at.z + std::cos(a) * 5.0;
				if (walker.onMesh({ax, wall.at.y, az}))
					continue;
				++tried;
				test::require(clearFormationSlot(walker, wall.at.y, radius, ax, az), "a slot inside a solid was not moved");
				test::require(walker.onMesh({ax, wall.at.y, az}), "the moved slot is off the mesh");
				for (int dir = 0; dir < 8; ++dir)
				{
					const double b = dir * kPi / 4.0;
					const knav::StepResult probe = walker.step({ax, wall.at.y, az}, std::sin(b) * 0.9 * radius, std::cos(b) * 0.9 * radius);
					test::require(probe.onMesh && !probe.blocked, "the moved slot is within a body radius of the solid");
				}
				double bx = wall.at.x - std::sin(a) * 5.0; // a slot on the mesh is left alone
				double bz = wall.at.z - std::cos(a) * 5.0;
				test::require(!clearFormationSlot(walker, wall.at.y, radius, bx, bz), "a slot on the mesh was moved");
			}
			test::require(tried > 0, "no wall found from the Hub to put a slot behind");
		}

		std::cout << "ksim_follow ok, hash " << std::hex << a.sim.stateHash() << std::dec << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
