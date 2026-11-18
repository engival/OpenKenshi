// 128 RETREAT_HOMETOWN and 127 RUN_AWAY_HOMETOWN (task home-gate; kinspect dt24_128 section 2): Task_GoHomeTown / Task_GoHomeTownFast walk the
// member to its home town (dt33). A member outside its squad's home town takes the goal; the town gives ONE cached point within 200 u of
// its centre (the same for every member), no home town gives the member's own position plus two independent draws in +-5000, and
// there is no arrival test: the task ends when the walker stops or gives up (the bare rig has no mesh, so it ends at once). 127 also
// holds a goal-commit lock (0.05 + rand x 0.15 s) that 128 lacks. A bare world: the live selector does the rest.

#include "dt27_rig.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

namespace {

using namespace ksim;
using namespace dt27;

void place(Rig& rig, double x, double z)
{
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = x + (id == rig.member ? 3.0 : 0.0);
		f.z = z;
	}
}

struct Walk
{
	std::uint64_t member = 0;
	double x = 0.0;
	double z = 0.0;
	int mode = -1;
};

// Every "home <task> member N walks to X Z (speed mode M)" line of the log.
std::vector<Walk> walksOf(const Rig& rig, std::uint16_t task)
{
	std::vector<Walk> out;
	const std::string prefix = "home " + std::to_string(task) + " member ";
	for (const std::string& line : rig.log)
	{
		const std::size_t at = line.find(prefix);
		Walk w;
		unsigned long long id = 0;
		if (at != std::string::npos
			&& std::sscanf(line.c_str() + at + prefix.size(), "%llu walks to %lf %lf (speed mode %d)", &id, &w.x, &w.z, &w.mode) == 4)
		{
			w.member = id;
			out.push_back(w);
		}
	}
	return out;
}

void walksHome(std::uint16_t task, const char* name, int townType)
{
	Rig rig({{task}}, 2);
	test::require(dataHasRow(*rig.gd, task), std::string(name) + ": no AI_PACKAGE carries the row");
	const Town home = makeTown(rig, townType, kX + 8000.0, kZ);
	test::require(home.circle.radius > 0.0, "the fixture town has no radius");
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	if (townType == 0)
		makeTown(rig, 2, kX + 8000.0 + home.circle.radius + 2500.0 + 4000.0, kZ);

	place(rig, home.circle.x, home.circle.z); // at home: the goal is not offered
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != task, std::string(name) + ": offered inside the home town");

	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == task; }),
		std::string(name) + ": the member outside its home town did not take the goal");
	rig.tick(10);
	const std::vector<Walk> walks = walksOf(rig, task);
	if (walks.empty()) for (const std::string& l : rig.log) std::cout << "  log " << l << "\n";
	test::require(!walks.empty(), std::string(name) + ": no walk was issued");
	for (const Walk& w : walks)
	{
		test::require(std::hypot(w.x - home.circle.x, w.z - home.circle.z) <= 200.0 + 1e-6,
			std::string(name) + ": the town point is not within 200 u of the centre");
		test::require(std::fabs(w.x - walks.front().x) < 1e-6 && std::fabs(w.z - walks.front().z) < 1e-6,
			std::string(name) + ": two members got different town points (one cached point per town)");
		test::require(w.mode == (task == routines::task::RunAwayHometown ? 2 : 0), std::string(name) + ": 128 walks (speed mode 0), 127 runs (2)");
	}
	// No arrival test: the member stands 600 u out at the pick, and the bare rig has no mesh, so the walk gives up and the goal ends.
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != task; }), std::string(name) + ": the task outlived its walk");
	test::require(rig.fighterOf(rig.member).moveMode == 2, std::string(name) + ": the speed mode is back to the default after the goal");
	std::cout << "  " << name << (townType == 0 ? " (nest home)" : "") << ": one town point, ends with its walk\n";
}

// Two members, one town: the same cached point.
void sharedPoint(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 0);
	const std::uint64_t second = rig.spawn(rig.body, kX, kZ);
	rig.join(second, 0);
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	for (const std::uint64_t id : {rig.leader, rig.member, second})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = home.circle.x + home.circle.radius + 600.0 + (id == rig.member ? 5.0 : id == second ? 9.0 : 0.0);
		f.z = home.circle.z;
	}
	rig.until(5.0, [&] { return walksOf(rig, task).size() >= 2; });
	std::vector<Walk> walks = walksOf(rig, task);
	std::set<std::uint64_t> members;
	for (const Walk& w : walks)
		members.insert(w.member);
	test::require(members.size() >= 2, std::string(name) + ": fewer than two members took the goal");
	for (const Walk& w : walks)
		test::require(std::fabs(w.x - walks.front().x) < 1e-6 && std::fabs(w.z - walks.front().z) < 1e-6,
			std::string(name) + ": members of one town got different points");
	std::cout << "  " << name << ": " << members.size() << " members, one point\n";
}

// No home town: own position plus two independent uniform draws in +-5000 (a box).
void wandersWithoutHome(std::uint16_t task, const char* name)
{
	Rig rig({{task}}, 2);
	place(rig, kX + 500.0, kZ);
	rig.until(60.0, [&] { return walksOf(rig, task).size() >= 12; });
	const std::vector<Walk> walks = walksOf(rig, task);
	test::require(walks.size() >= 12, std::string(name) + ": too few picks with no home town (" + std::to_string(walks.size()) + ")");
	double farthest = 0.0;
	double maxDx = 0.0;
	double maxDz = 0.0;
	for (const Walk& w : walks)
	{
		const double dx = w.x - (kX + 500.0 + (w.member == rig.member ? 3.0 : 0.0));
		const double dz = w.z - kZ;
		test::require(std::fabs(dx) <= 5000.0 + 1e-6 && std::fabs(dz) <= 5000.0 + 1e-6,
			std::string(name) + ": a point outside the +-5000 box");
		farthest = std::max(farthest, std::hypot(dx, dz));
		maxDx = std::max(maxDx, std::fabs(dx));
		maxDz = std::max(maxDz, std::fabs(dz));
	}
	test::require(farthest > 300.0, std::string(name) + ": every point within 300 u: still the old 50-250 ring");
	test::require(maxDx > 1000.0 && maxDz > 1000.0, std::string(name) + ": the two axes are not both spread over the box");
	std::cout << "  " << name << " (no home town): " << walks.size() << " picks in the +-5000 box, farthest " << fmt(farthest) << "\n";
}

// The goal-commit lock (dt33): 127 refuses to drop a goal for 0.05 + rand x 0.15 s after the pick, 128 (0 / 0) does not. In the bare rig
// the walk fails at once, so the shortest a goal can last is its lock.
void commitLock()
{
	const auto runs = [&](std::uint16_t task)
	{
		Rig rig({{task}}, 2);
		place(rig, kX + 500.0, kZ);
		std::vector<int> out;
		int run = 0;
		for (int i = 0; i < 6000 && out.size() < 40; ++i)
		{
			rig.tick(1);
			if (rig.goalOf(rig.member) == task)
				++run;
			else if (run > 0)
			{
				out.push_back(run);
				run = 0;
			}
		}
		return out;
	};
	const std::vector<int> fast = runs(routines::task::RunAwayHometown);
	const std::vector<int> slow = runs(routines::task::RetreatHometown);
	test::require(fast.size() >= 10 && slow.size() >= 10, "the commit test saw too few goals");
	const int fastMin = *std::min_element(fast.begin(), fast.end());
	const int slowMin = *std::min_element(slow.begin(), slow.end());
	const int fastMax = *std::max_element(fast.begin(), fast.end());
	std::cout << "  goal lengths (ticks): 127 min " << fastMin << " max " << fastMax << ", 128 min " << slowMin << "\n";
	test::require(fastMin >= slowMin + 1, "127 dropped its goal inside the commit window (not longer than 128's no-lock goal)");
	test::require(fastMin >= 1 && fastMax <= 6, "127's goal is outside 0.05 + [0, 0.15) s (1-4 ticks, plus the end's tick)");
}

} // namespace

int main()
{
	try
	{
		walksHome(routines::task::RetreatHometown, "128", 2);
		walksHome(routines::task::RunAwayHometown, "127", 2);
		walksHome(routines::task::RetreatHometown, "128", 0);
		sharedPoint(routines::task::RetreatHometown, "128");
		sharedPoint(routines::task::RunAwayHometown, "127");
		wandersWithoutHome(routines::task::RetreatHometown, "128");
		wandersWithoutHome(routines::task::RunAwayHometown, "127");
		commitLock();
		std::cout << "ksim_home_walk: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_home_walk: FAILED: " << e.what() << "\n";
		return 1;
	}
}
