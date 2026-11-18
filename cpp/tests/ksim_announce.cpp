// 103 MAKE_ANNOUNCEMENT / 129 MAKE_ANNOUNCEMENT_FAST (kinspect dt24_smalltasks section 5, Task_MakeAnnouncement::vf2 140355770): at the package
// target, with the squad's leader up (the registry's isLeaderDown = 0 and atPackageTarget = 1), the announcer says the announcement line
// (DIALOGUE_SELECTION_STATIC event 0x02 ANNOUNCEMENT) once; it says it again only after it has left the target and come back. A bare world.

#include "dt27_rig.hpp"

#include "ksim/ai/routines.hpp"
#include "ksim/world/towncircle.hpp"

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::uint16_t kTask = routines::task::MakeAnnouncementFast;

void place(Rig& rig, double x, double z)
{
	for (const std::uint64_t id : {rig.leader, rig.member})
	{
		state::Fighter& f = rig.fighterOf(id);
		f.x = x + (id == rig.member ? 3.0 : 0.0);
		f.z = z;
	}
}

void announces()
{
	Rig rig({{kTask}}, 2);
	test::require(dataHasRow(*rig.gd, kTask), "no AI_PACKAGE carries the 129 row");
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	rig.tick(60);
	test::require(rig.sim.dialogueEventCount(0x02) == 0, "129: announced away from the package target");

	place(rig, home.circle.x, home.circle.z);
	test::require(rig.until(5.0, [&] { return rig.sim.dialogueEventCount(0x02) > 0; }), "129: no announcement at the package target");
	const auto said = rig.sim.dialogueEventCount(0x02);
	rig.tick(200);
	test::require(rig.sim.dialogueEventCount(0x02) == said, "129: announced again without leaving the target");

	place(rig, home.circle.x + home.circle.radius + 600.0, home.circle.z);
	rig.tick(60);
	place(rig, home.circle.x, home.circle.z);
	test::require(rig.until(5.0, [&] { return rig.sim.dialogueEventCount(0x02) > said; }), "129: no announcement on the second arrival");
	std::cout << "  129: one announcement per arrival at the package target\n";
}

void leaderDown()
{
	Rig rig({{kTask}}, 2);
	const Town home = makeTown(rig, 2, kX + 8000.0, kZ);
	rig.squad.get_mut<state::SquadInstance>()->homeTown.id = home.entity.id();
	place(rig, home.circle.x, home.circle.z);
	state::Fighter& leader = rig.fighterOf(rig.leader);
	leader.unconscious = true;
	leader.koTimer = 600.0;
	rig.tick(100);
	test::require(rig.sim.dialogueEventCount(0x02) == 0, "129: announced with the leader down (isLeaderDown = 1)");
	std::cout << "  129: not with the leader down\n";
}

} // namespace

int main()
{
	try
	{
		announces();
		leaderDown();
		std::cout << "ksim_announce: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_announce: FAILED: " << e.what() << "\n";
		return 1;
	}
}
