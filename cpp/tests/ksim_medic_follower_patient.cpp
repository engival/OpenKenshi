// kenshi-k16.6.673 (special-case shim, user 2026-11-21; oracle kenshi-rwh.262): a follower whose followed actor is first-aiding it holds
// still until the aid is gone (Fighter::heldForAid, followStep). Medic and patient on the Hub mesh, the patient following the medic.
//   A - the patient 40 u off holds still while the medic walks up and treats; the injury falls; it then follows again.
//   B - the patient follows someone else (not the medic): it keeps following while the medic works.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/interact.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "hub_point.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Rig
{
	Simulation sim;
	std::shared_ptr<const CombatData> data;
	const kdata::GameData& gd;
	std::shared_ptr<MeshNavigator> nav;
	std::string sid;
	std::string kit;
	std::uint64_t rng = 4242;

	Rig(const std::shared_ptr<const CombatData>& d, const kdata::GameData& g, const std::shared_ptr<MeshNavigator>& n)
		: sim(Simulation::load(KENSHI_DATA, 77)), data(d), gd(g), nav(n)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ITEM")))
		{
			if (r->name == "Basic First Aid Kit")
				kit = r->stringID;
		}
		test::require(!kit.empty(), "the Basic First Aid Kit exists");
	}

	std::uint64_t spawn(double x, double z)
	{
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.team = 0;
		f.playerControlled = true;
		const auto spot = nav->walker().snap({x, 1535.0, z}, 200.0);
		test::require(spot.has_value() && std::hypot(spot->x - x, spot->z - z) < 6.0, "no mesh at the spawn spot");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}
	bool spotAt(double distance, double& x, double& z, int first)
	{
		for (int k = 0; k < 64; ++k)
		{
			const double a = (first + k) * 0.0981748;
			x = kHubX + std::sin(a) * distance;
			z = kHubZ + std::cos(a) * distance;
			const auto spot = nav->walker().snap({x, 1535.0, z}, 20.0);
			if (spot && std::hypot(spot->x - x, spot->z - z) < 1.0)
				return true;
		}
		return false;
	}
	state::Fighter& at(std::uint64_t id) { return *fighterOf(sim, id); }
	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
		return found;
	}
	bool treating(std::uint64_t medic, std::uint64_t patient)
	{
		const state::InteractTask* t = entityOf(medic).get<state::InteractTask>();
		return t != nullptr && t->action == static_cast<std::uint8_t>(InteractAction::FirstAid) && t->target == patient;
	}
	double injury(std::uint64_t id)
	{
		double sum = 0.0;
		for (const state::FighterPart& part : at(id).parts)
			sum += std::max(0.0, (part.maxHealth - part.wear) - part.health - part.pool);
		return sum;
	}
	void hurt(std::uint64_t id, double amount)
	{
		for (state::FighterPart& part : at(id).parts)
		{
			if (!part.robotic && part.maxHealth - part.wear > 0.0)
			{
				part.health = std::max(0.0, part.maxHealth - part.wear - amount);
				return;
			}
		}
	}
	void treat(std::uint64_t medic, std::uint64_t patient)
	{
		const InteractTarget target = findInteractTarget(ecsOf(sim), sim.combat(), patient);
		const Route route = routeToInteract(*nav, at(medic), target, static_cast<std::uint8_t>(InteractAction::FirstAid), false, sim.pathRadius(at(medic)));
		test::require(route.found, "no route to the patient");
		sim.enqueue(Command::fighterInteract(state::Id{medic}, patient, static_cast<std::uint8_t>(InteractAction::FirstAid), 1, route.points, route.heights, route.approach));
	}
};

struct Trio
{
	std::uint64_t medic = 0, patient = 0, other = 0;
};

Trio place(Rig& rig)
{
	Trio t;
	double x = 0.0, z = 0.0;
	test::require(rig.spotAt(10.0, x, z, 0), "no medic spot");
	t.medic = rig.spawn(x, z);
	test::require(rig.spotAt(50.0, x, z, 16), "no patient spot");
	t.patient = rig.spawn(x, z);
	test::require(rig.spotAt(10.0, x, z, 40), "no other spot");
	t.other = rig.spawn(x, z);
	state::Inventory inv;
	rig.entityOf(t.medic).set<state::Inventory>(inv);
	rig.entityOf(t.medic).get_mut<state::Inventory>()->add(rig.kit, 1);
	rig.hurt(t.patient, 40.0);
	test::require(rig.injury(t.patient) > 0.0, "the patient is injured");
	return t;
}

double moved(Rig& rig, std::uint64_t id, double x0, double z0) { return std::hypot(rig.at(id).x - x0, rig.at(id).z - z0); }

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_medic_follower_patient HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// A - the patient follows the medic and is treated: it holds still while the aid is pending, the aid lands, it follows again.
		{
			Rig rig(data, gd, nav);
			const Trio t = place(rig);
			rig.sim.enqueue(Command::fighterFollow(state::Id{t.patient}, state::Id{t.medic}));
			rig.sim.run(2);
			const double before = rig.injury(t.patient);
			const double x0 = rig.at(t.patient).x;
			const double z0 = rig.at(t.patient).z;
			rig.treat(t.medic, t.patient);
			rig.sim.run(2);
			test::require(rig.treating(t.medic, t.patient), "A: the medic holds a first aid interact on the patient");
			int landed = -1;
			double drift = 0.0;
			for (int tick = 0; tick < 20 * 90 && landed < 0; ++tick)
			{
				rig.sim.run(1);
				drift = std::max(drift, moved(rig, t.patient, x0, z0));
				if (!rig.treating(t.medic, t.patient))
					landed = tick;
			}
			std::printf("A: aid %s after %d ticks; patient drift %.2f u; injury %.1f -> %.1f\n", landed >= 0 ? "landed" : "NOT landed", landed,
				drift, before, rig.injury(t.patient));
			test::require(landed >= 0 && landed < 20 * 60, "A: the aid lands within 60 s");
			test::require(rig.injury(t.patient) < before, "A: the injury fell");
			// The 5-8 u it shifts is the medic's walk-up pushing it aside (the repro without the hold: it ran 30 u to its slot and the aid never landed).
			test::require(drift < 12.0, "A: the patient held still while the aid was pending");
			// The aid is gone: the follow resumes. The medic walks off; the patient trails it.
			rig.sim.enqueue(Command::fighterDrive(state::Id{t.medic}, 1, 0.0, 1));
			rig.sim.run(20 * 6);
			const double after = moved(rig, t.patient, x0, z0);
			std::printf("A: after the aid the patient moved %.1f u\n", after);
			test::require(after > 10.0, "A: the patient follows again once the aid is done");
		}

		// B - the patient follows someone else: the hold never applies, it keeps following while the medic works.
		{
			Rig rig(data, gd, nav);
			const Trio t = place(rig);
			rig.sim.enqueue(Command::fighterFollow(state::Id{t.patient}, state::Id{t.other}));
			rig.sim.enqueue(Command::fighterDrive(state::Id{t.other}, 1, 0.0, 1));
			rig.sim.run(2);
			const double x0 = rig.at(t.patient).x;
			const double z0 = rig.at(t.patient).z;
			rig.treat(t.medic, t.patient);
			rig.sim.run(20 * 4);
			const double away = moved(rig, t.patient, x0, z0);
			std::printf("B: the patient following someone else moved %.1f u in 4 s (medic treating: %d)\n", away,
				rig.treating(t.medic, t.patient) ? 1 : 0);
			test::require(away > 5.0, "B: the patient keeps following someone else");
			test::require(!rig.at(t.patient).heldForAid, "B: never held for a medic it does not follow");
		}
		std::printf("ksim_medic_follower_patient: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_medic_follower_patient: %s\n", e.what());
		return 1;
	}
}
