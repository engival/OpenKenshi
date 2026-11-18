// KO wake (client re-test on master 057ec506): the duel servant "goes down and never gets up". It is `down` (a collapsed leg), not
// KO'd: flags 0x45225 (combat | drawn | prone | run | hostile) have no KO bit, the ragdoll (event 48 cause 0) was the collapse's
// 3.0-unit forced KO, over within seconds. The down flag has no timer: it clears when the leg's h - s + pool rises back over the KO
// point, and with no natural regeneration (MEDICAL_HUNGER_LIVE (d)) only first aid does that. This test pins the law the live run
// shows: one swing leaves a fresh leg standing, a collapsed leg is down and conscious for 30 sim minutes, first aid stands it.
//
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

state::FighterPart* partNamed(state::Fighter& f, std::string_view name)
{
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == name)
			return &p;
	}
	return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_ko_wake HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");

		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		simlink::ServerContext context;
		context.gameData = gd;
		context.dataHash = simlink::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		const simlink::Duel duel = simlink::spawnDuel(sim, *gd, *data, context);
		fighterOf(sim, duel.enemy)->ranged = state::FighterRanged{}; // disarmed: the servant's `crossbows` chance (k16.6.24) is not what this test pins
		const CombatRules& rules = sim.combat()->rules();

		state::Fighter* servant = fighterOf(sim, duel.enemy);
		test::require(servant != nullptr, "no servant");
		state::FighterPart* leg = partNamed(*servant, "Right Leg");
		test::require(leg != nullptr, "no Right Leg");

		// 1. The client's swing: two landed impacts on the Right Leg (the live log shows 12.7 / 75.1 and 12.7 / 74.8 cut / blunt, the
		// combo's two blows). The first leaves a fresh leg over the KO point (v about +12 at those numbers), the second takes it to about
		// -75 (BC saw -78.3 live): the collapse, the forced KO (ragdoll, event 48 cause 0), then `down` and conscious. So one fresh
		// servant and one fight is enough; no worn leg is needed.
		for (state::FighterPart& p : servant->parts)
		{
			if (&p != leg)
				p.chance = 0.0; // only the leg is a target: the real hit path lands on it
		}
		sim.enqueue(Command::fighterAttack(state::Id{ duel.player }, state::Id{ duel.enemy }));
		int hits = 0;
		double lastStun = 0.0;
		bool collapsedAt2 = false;
		for (int tick = 1; tick <= 4000 && hits < 2; ++tick)
		{
			sim.tick();
			servant = fighterOf(sim, duel.enemy);
			leg = partNamed(*servant, "Right Leg");
			if (leg->stun > lastStun + 1.0)
			{
				++hits;
				if (hits == 1)
					test::require(!servant->down && CombatRules::partEffective(*leg) > rules.koPoint(*servant),
						"the first impact collapsed a fresh leg (v " + std::to_string(CombatRules::partEffective(*leg)) + ")");
				else
					collapsedAt2 = servant->down;
			}
			lastStun = leg->stun;
		}
		test::require(hits == 2, "the swing did not land twice on the leg");
		test::require(collapsedAt2 && CombatRules::partEffective(*leg) < rules.koPoint(*servant),
			"the second impact did not collapse the leg (v " + std::to_string(CombatRules::partEffective(*leg)) + ")");
		test::require(servant->koTimer > 0.0 || !servant->unconscious, "the collapse left a KO with no timer");

		// 2. A leg an earlier fight left half gone, then that swing: down, conscious, and it stays so (MEDICAL_HUNGER_LIVE (d), (e)):
		// the forced KO of the collapse is the 3.0-unit timer (the ragdoll, event 48 cause 0), it ends within seconds, and the down flag has
		// no timer of its own - no natural regeneration lifts the leg and an untreated, over-20%-damaged part sinks, so nothing stands the
		// body unaided. UNSURE (FEATURES): whether the original's lone downed NPC ever stands; the decode says first aid only.
		state::Fighter probe = *servant;
		state::FighterPart* hurt = partNamed(probe, "Right Leg");
		hurt->health = 26.0;
		hurt->stun = 126.0;
		hurt->pool = 0.0;
		hurt->collapsed = false; // a fresh collapse of this part (the copy already carries the first one)
		probe.koTimer = 0.0;
		probe.unconscious = false;
		rules.checkCollapse(probe);
		test::require(probe.down, "a leg at v -100 did not put the body down");
		test::require(probe.koTimer > 0.0, "the collapse started no forced KO timer");
		const double healthAtCollapse = hurt->health;
		const double bound = 1800.0; // 30 sim minutes
		bool woke = false;
		for (double t = 0.0; t < bound; t += kTickSeconds)
		{
			rules.updateMedical(probe, kTickSeconds);
			if (!woke && !probe.unconscious)
			{
				woke = true;
				test::require(t < 60.0, "the forced KO of a collapse outlasted a minute");
			}
			test::require(probe.down, "an untreated collapsed leg stood unaided at " + std::to_string(t) + " s");
		}
		test::require(woke && !probe.unconscious, "the collapse's forced KO never ended");
		test::require(hurt->health <= healthAtCollapse, "an untreated leg gained health on its own");

		// 3. First aid is the way up: the bandage pool counts at once, the body stands.
		hurt->pool = std::max(0.0, rules.koPoint(probe) - CombatRules::partEffective(*hurt) + 1.0);
		rules.checkCollapse(probe);
		test::require(!probe.down, "a leg with a pool over its deficit stayed down");

		std::cout << "ksim_ko_wake: OK\n";
		return 0;
	} catch (const std::exception& e) {
		std::cout << "ksim_ko_wake FAILED: " << e.what() << "\n";
		return 1;
	}
}
