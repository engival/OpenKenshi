// Squad order 0x3e, hold still (kenshi-k16.6.674; original 140269ff0, posted by 25 / 249 / 60 on the target's own squad package, cancelled at
// the poster's stop). Medic and patient on the Hub mesh, the patient ordered to walk off.
//   A - same squad: the patient does not walk off while the aid is pending, the aid lands, the hold is gone and it walks.
//   B - another squad: no hold (no freeze exploit); the patient walks off while the medic works.
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
		test::require(argc == 2, "usage: ksim_medic_hold_order HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		for (const bool sameSquad : {true, false})
		{
			Rig rig(data, gd, nav);
			const Trio t = place(rig);
			const char* tag = sameSquad ? "A" : "B";
			rig.at(t.medic).squad = 1001;
			rig.at(t.patient).squad = sameSquad ? 1001 : 1002;
			double gx = 0.0, gz = 0.0;
			test::require(rig.spotAt(45.0, gx, gz, 48), "no walk-off spot");
			rig.sim.enqueue(Command::fighterMoveFar(state::Id{t.patient}, gx, gz, 1535.0, 1));
			rig.sim.run(2);
			const double x0 = rig.at(t.patient).x;
			const double z0 = rig.at(t.patient).z;
			const double before = rig.injury(t.patient);
			rig.treat(t.medic, t.patient);
			rig.sim.run(2);
			test::require(rig.treating(t.medic, t.patient), "the medic holds a first aid interact on the patient");
			int landed = -1;
			double drift = 0.0;
			bool held = false;
			for (int tick = 0; tick < 20 * 90 && landed < 0; ++tick)
			{
				rig.sim.run(1);
				drift = std::max(drift, moved(rig, t.patient, x0, z0));
				held = held || rig.at(t.patient).heldForAid;
				if (!rig.treating(t.medic, t.patient))
					landed = tick;
			}
			std::printf("%s: held %d, drift %.2f u, landed after %d ticks, injury %.1f -> %.1f\n", tag, held ? 1 : 0, drift, landed, before,
				rig.injury(t.patient));
			if (sameSquad)
			{
				test::require(held && drift < 12.0, "A: the same-squad patient held still while the aid was pending");
				test::require(landed >= 0 && rig.injury(t.patient) < before, "A: the aid landed");
				rig.sim.run(2); // the flag is derived at the start of a tick
				test::require(!rig.at(t.patient).heldForAid, "A: the stop cancelled the hold");
				rig.sim.enqueue(Command::fighterMoveFar(state::Id{t.patient}, gx, gz, 1535.0, 2));
				const double xa = rig.at(t.patient).x;
				const double za = rig.at(t.patient).z;
				rig.sim.run(20 * 5);
				test::require(moved(rig, t.patient, xa, za) > 8.0, "A: the patient walks again once the hold is gone");
			} else {
				test::require(!held, "B: a patient in another squad is never held");
				test::require(drift > 8.0, "B: the other-squad patient walked off");
			}
		}
		std::printf("ksim_medic_hold_order: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_medic_hold_order: %s\n", e.what());
		return 1;
	}
}
