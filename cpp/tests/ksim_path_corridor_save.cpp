// PATH_WALK.md 3a (kenshi-e9s9.4, save v128): a walker's corridor (portals without refs, portalNext, vx / vz) and a DoorStep's stashed corridor
// survive a save and load; a v126 save carries none, and setRoute / rebuildCorridor lays one from the saved points (no re-plan).

#include "dt27_rig.hpp"

#include "ksim/nav/navigator.hpp"
#include "ksim/save/save.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdio>

namespace {

using namespace ksim;
using namespace dt27;

state::RoutePortal portal(double lx, double lz, double rx, double rz, std::uint32_t point)
{
	state::RoutePortal p;
	p.lx = lx;
	p.lz = lz;
	p.rx = rx;
	p.rz = rz;
	p.clearLeft = 1.5;
	p.clearRight = 2.5;
	p.point = point;
	p.ref = 12345; // never saved
	return p;
}

const state::Fighter* loadedFighter(Simulation& sim, std::uint64_t id)
{
	const state::Fighter* found = nullptr;
	sim.world().ecs().each([&](const state::Id& who, const state::Fighter& f)
	{
		if (who.value == id)
			found = &f;
	});
	return found;
}

void roundTrip()
{
	Rig rig({}, 0);
	const std::uint64_t id = rig.spawn(rig.body, 30.0, 20.0);
	state::Fighter& f = rig.fighterOf(id);
	setRoute(f, {{30.0, 0.0, 20.0}, {40.0, 0.0, 20.0}, {40.0, 0.0, 40.0}}, nullptr);
	f.portals = {portal(35.0, 18.0, 35.0, 22.0, 1), portal(40.0, 20.0, 40.0, 20.0, 1), portal(38.0, 30.0, 42.0, 30.0, 2)};
	f.routeNext = 2;
	syncPortalNext(f);
	f.vx = 3.25;
	f.vz = -1.5;
	test::require(f.portalNext == 2, "portalNext is not the first portal of point 2");
	state::DoorStep step;
	step.door = 7;
	step.route = f.route;
	step.portals = f.portals;
	step.portalNext = f.portalNext;
	rig.entityOf(id).set<state::DoorStep>(step);
	rig.entityOf(id).set<state::Fighter>(f);

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_path_corridor_save.save";
	const std::uint64_t held = rig.sim.stateHash();
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == held, "load gives the saved hash");
	const state::Fighter* back = loadedFighter(loaded, id);
	test::require(back != nullptr && back->portals.size() == 3 && back->portalNext == 2 && back->vx == 3.25 && back->vz == -1.5,
		"the corridor, portalNext or the velocity vector changed in the save");
	test::require(back->portals[2].lx == 38.0 && back->portals[2].rx == 42.0 && back->portals[2].clearLeft == 1.5 && back->portals[2].clearRight == 2.5
		&& back->portals[2].point == 2, "a portal changed in the save");
	test::require(back->portals[0].ref == 0, "the polygon ref was saved");
	const state::DoorStep* stash = nullptr;
	loaded.world().ecs().each([&](const state::DoorStep& s) { stash = &s; });
	test::require(stash != nullptr && stash->portals.size() == 3 && stash->portalNext == 2, "the DoorStep's stashed corridor changed in the save");
	std::remove(path.c_str());

	// A save of a version before the corridor (v127 or older): the portals are not in it, the route is. The reader takes only the current and the previous
	// version, so this runs only while the previous one predates v128; later it is the older versions' own tests that matter.
	if (kSaveVersion - 1 < 128)
	{
		// A v126 save: the portals are not in it, the route is. The installer lays a corridor from the saved points.
		const std::string old = std::string(KSIM_BINARY_DIR) + "/ksim_path_corridor_prev.save";
		writeSaveFile(old, rig.sim.world().ecs(), rig.sim.world().loadOrder(), {}, kSaveVersion - 1);
		Simulation legacy = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), old, false, nullptr, RegionConfig{});
		const state::Fighter* oldFighter = loadedFighter(legacy, id);
		test::require(oldFighter != nullptr && oldFighter->route.size() == 3 && oldFighter->portals.empty(), "a v126 save carries a corridor");
		state::Fighter again = *oldFighter;
		rebuildCorridor(again, nullptr);
		test::require(again.portals.size() == 2 && again.portalNext == 0 + 1, "no corridor from the saved points"); // routeNext 2: the portal of point 2
		std::remove(old.c_str());
	}
	std::cout << "  corridor, portalNext, velocity vector and the door-step stash survive save / load; a v126 save gets its corridor from the points\n";
}

} // namespace

int main()
{
	try
	{
		roundTrip();
		std::cout << "ksim_path_corridor_save: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_path_corridor_save: FAILED: " << e.what() << "\n";
		return 1;
	}
}
