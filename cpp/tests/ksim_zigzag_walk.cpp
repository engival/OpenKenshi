// kenshi-e9s9.15 probe: the Heft gate -> stone camp walk (67047,-72097 -> 65406,-50249), every route install logged (tick, position, corners, the
// largest distance of a corner from the line start -> leg end). Prints the plan table (ours_plans.tsv columns) to the file in argv[2].
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"
#include <cmath>
#include <cstdio>
#include <iostream>
using namespace ksim;
int main(int argc, char** argv)
{
	const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/gate_heft.kscen"));
	if (argc < 3 || !scenario_rig::hasRegion(file, argv[1])) { std::cout << "SKIPPED\n"; return 77; }
	scenario_rig::Rig rig(file, argv[1]);
	Simulation& sim = rig.sim;
	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 1);
	const std::uint64_t id = placed.players.front();
	sim.run(40);
	{ state::Fighter* f = fighterOf(sim, id); f->x = 67046.79; f->y = 1270.58; f->z = -72096.90; }
	sim.run(60);
	sim.enqueue(Command::fighterMoveFar(state::Id{id}, 65406.09, -50248.60, 779.53, 16));
	std::FILE* out = std::fopen(argv[2], "w");
	std::fprintf(out, "plan\tt\tleg_end_x\tleg_end_z\tn_corners\tlen\tbody_x\tbody_z\tmaxoff\tcorners\n");
	std::uint32_t last = fighterOf(sim, id)->routeVersion;
	int plan = 0;
	double t0 = 0;
	const double seconds = argc > 3 ? std::atof(argv[3]) : 300.0;
	// argv[4]: walked trail, one row per tick (t x z), from the order until 3 u from the goal (kenshi-e9s9.24)
	std::FILE* trail = argc > 4 ? std::fopen(argv[4], "w") : nullptr;
	if (trail) std::fprintf(trail, "t\tx\tz\n");
	for (int tick = 0; tick < seconds * 20; ++tick)
	{
		sim.run(1);
		state::Fighter* f = fighterOf(sim, id);
		if (trail) std::fprintf(trail, "%.2f\t%.2f\t%.2f\n", (tick + 1) / 20.0, f->x, f->z);
		if (trail && std::hypot(f->x - 65406.09, f->z + 50248.60) < 3.0) break;
		if (tick % 20 == 0) { f->blood = 150.0; f->bleeding = 0.0; f->hunger = 300.0; for (auto& p : f->parts) { p.health = p.maxHealth; p.stun = 0.0; } f->koTimer = 0.0; }
		if (f->routeVersion == last) continue;
		last = f->routeVersion;
		if (f->route.empty()) continue;
		const auto& e = f->route.back();
		double len = 0, px = f->x, pz = f->z, off = 0;
		const double dx = e.x - f->x, dz = e.z - f->z, dl = std::max(1e-6, std::hypot(dx, dz));
		std::string corners;
		for (const auto& p : f->route)
		{
			len += std::hypot(p.x - px, p.z - pz); px = p.x; pz = p.z;
			off = std::max(off, std::fabs((p.x - f->x) * dz - (p.z - f->z) * dx) / dl);
			char b[64]; std::snprintf(b, sizeof b, " (%.0f %.0f)", p.x, p.z); corners += b;
		}
		const double t = tick / 20.0;
		std::fprintf(out, "%d\t%.2f\t%.1f\t%.1f\t%zu\t%.1f\t%.1f\t%.1f\t%.1f\t%s\n", ++plan, t, e.x, e.z, f->route.size(), len, f->x, f->z, off, corners.c_str());
		(void)t0;
	}
	std::fclose(out);
	if (trail) std::fclose(trail);
	std::cout << "plans " << plan << " end " << fighterOf(sim, id)->x << " " << fighterOf(sim, id)->z << "\n";
	return 0;
}
