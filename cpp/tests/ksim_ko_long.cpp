// Task BC: the duel's long KO is the KO model working, not a bug (kinspect findings/bc_unarmed_hit.md 1af739b).
// The client saw the Escaped Servant go prone, take hits on the ground at 51-75 stun each and stay KO'd for 30,580 ticks.
// Our unarmed hit there is faithful: the live `shotei L` does about -12.9 health / +76.2 stun per part, and receiveHit's
// toughness factor scales it by ~1.47 for this servant, landing on our 12.72 / 74.71. In the live duel that swing hit the
// Right Leg (v -78.3, chest only +11.3), so the leg collapsed and the servant crawled; here the same swing hits the Chest,
// whose v goes to -48.4, far below the KO point (-21.49): the latch holds the body down until the stun decays back to its
// health. The KO timer (T 77.9) expired long before the wake. The length is (s-h)/decay, and the decay runs on the medical
// clock: 2.0 in the run that found this (about 2x the original's medical units per second); since task BT the default is the
// original's nominal 1/1.04, which only stretches the same wake (this test computes it from the settings).
//
// This test pins that faithful behaviour: the timer is not what holds the KO, no hit lands after the KO, the wake needs
// every vital back at h - s >= 0, and the wake time is the latched excess over the documented decay. It also pins the
// limb rule the live case shows: a collapsed leg puts the body down but never knocks it out, and down stays until it rises.
//
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache). The duel takes orders, so it needs a mesh.

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

std::string partsLine(const state::Fighter& f)
{
	std::string out;
	for (const state::FighterPart& p : f.parts)
	{
		if (!p.vital)
			continue;
		char buffer[96];
		std::snprintf(buffer, sizeof buffer, "%s h %.1f s %.1f v %.1f E %.1f; ", p.name.c_str(), p.health, p.stun,
			p.health - p.stun + p.bonus, p.maxHealth - p.wear);
		out += buffer;
	}
	return out;
}

// The deepest vital below zero: -v and the part, or {0, nullptr} when every vital is at v >= 0.
std::pair<double, const state::FighterPart*> latchedDepth(const state::Fighter& f)
{
	std::pair<double, const state::FighterPart*> out{0.0, nullptr};
	for (const state::FighterPart& p : f.parts)
	{
		const double v = CombatRules::partEffective(p);
		if (p.vital && v < 0.0 && -v > out.first)
			out = {-v, &p};
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_ko_long HUB.knav");
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
		const double medical = rules.settings().medicalClock;

		// The limb rule (REPLAY_GAPS_LIVE.md d, MEDICAL_HUNGER_LIVE.md e): a leg below the KO point puts the body down and
		// leaves it conscious (the live servant crawled after the leg hit); it does not knock it out, and down clears when
		// the part rises back above the point.
		{
			state::Fighter probe = *fighterOf(sim, duel.enemy);
			state::FighterPart* leg = partNamed(probe, "Right Leg");
			test::require(leg != nullptr, "the servant has no Right Leg");
			leg->stun = leg->health + leg->bonus - rules.koPoint(probe) + 1.0;
			rules.checkCollapse(probe);
			test::require(probe.down, "a leg below the KO point did not put the body down");
			test::require(!probe.unconscious, "a leg collapse knocked the character out (it never does)");
			leg->stun = 0.0;
			rules.checkCollapse(probe);
			test::require(!probe.down, "down did not clear when the leg rose above the KO point");
		}

		// The player's attack order (the only order in this test).
		sim.enqueue(Command::fighterAttack(state::Id{ duel.player }, state::Id{ duel.enemy }));

		const double decayPerTick = kTickSeconds * medical // medical time per tick
			* 0.05 * data->k.stunRecoveryRate * data->k.healRateMult;  // updateHealing's stun rate, x E/100 and heal factor below
		std::int64_t koTick = -1;
		std::int64_t wakeTick = -1;
		std::int64_t deathTick = -1;
		double timerAtKo = 0.0;
		double depthAtKo = 0.0;
		double ratePerTick = 0.0;
		const state::FighterPart* koPart = nullptr;
		double lastStun = 0.0;
		const int limit = 200000; // 50 min of sim time; the client's 30,580 ticks is inside
		for (int tick = 1; tick <= limit; ++tick)
		{
			sim.tick();
			state::Fighter* f = fighterOf(sim, duel.enemy);
			test::require(f != nullptr, "the servant vanished");
			double stun = 0.0;
			for (const state::FighterPart& p : f->parts)
				stun += p.stun;
			if (koTick > 0)
			{
				// An unconscious body is not an impact target (kinspect): no hit lands after the KO, so the stun only decays.
				test::require(stun <= lastStun + 1e-6, "a hit landed on the KO'd servant at tick " + std::to_string(tick));
				const auto depth = latchedDepth(*f);
				test::require(f->unconscious == (f->koTimer > 0.0 || depth.second != nullptr || f->blood <= 0.0
						|| f->hunger < 0.1),
					"the KO state disagrees with the timer/latch at tick " + std::to_string(tick));
			}
			lastStun = stun;
			if (f->unconscious && koTick < 0)
			{
				koTick = tick;
				timerAtKo = f->koTimer;
				const auto depth = latchedDepth(*f);
				test::require(depth.second != nullptr, "the KO has no latched vital: this test pins a latched KO");
				depthAtKo = depth.first;
				koPart = partNamed(*f, depth.second->name);
				test::require(koPart != nullptr, "the latched part vanished");
				// The documented stun decay for the latched part: 0.05 x stun recovery x heal rate mult x E/100 x heal factor.
				ratePerTick = decayPerTick * (koPart->maxHealth - koPart->wear) / 100.0 * f->healFactor;
				std::printf("KO at tick %d: T %.1f latch %d, %s over health %.1f, decay %.6f/tick, %s\n", tick,
					f->koTimer, f->koLatch ? 1 : 0, koPart->name.c_str(), depthAtKo, ratePerTick, partsLine(*f).c_str());
				continue;
			}
			if (koTick > 0 && !f->unconscious && !f->dead)
			{
				wakeTick = tick;
				const auto depth = latchedDepth(*f);
				test::require(depth.second == nullptr, "the servant woke with a vital still below zero");
				test::require(f->koTimer <= 0.0, "the servant woke with the KO timer still running");
				break;
			}
			if (f->dead)
			{
				deathTick = tick;
				break;
			}
		}
		test::require(koTick > 0, "the servant was never knocked out");
		test::require(wakeTick > 0 || deathTick > 0, "the servant neither woke nor died within the limit");
		if (deathTick > 0)
		{
			std::printf("ksim_ko_long: ok (KO %lld, death %lld)\n", static_cast<long long>(koTick),
				static_cast<long long>(deathTick));
			return 0;
		}
		const double expected = depthAtKo / ratePerTick;
		const std::int64_t lasted = wakeTick - koTick;
		std::printf("woke at tick %lld, %lld after the KO (documented latch time %.0f)\n", static_cast<long long>(wakeTick),
			static_cast<long long>(lasted), expected);
		// The latch, not the timer, holds the body: the timer's own run is 100x shorter here.
		const double timerTicks = timerAtKo / (kTickSeconds * medical); // updateMedical: the timer loses dt each tick
		test::require(static_cast<double>(lasted) > 10.0 * timerTicks, "the KO timer, not the latch, ended the KO");
		test::require(std::fabs(static_cast<double>(lasted) - expected) <= 0.05 * expected + 100.0,
			"the wake time is not the latched excess over the documented decay (got " + std::to_string(lasted)
				+ ", expected " + std::to_string(expected) + ")");
		std::printf("ksim_ko_long: ok (KO %lld, wake %lld)\n", static_cast<long long>(koTick),
			static_cast<long long>(wakeTick));
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_ko_long: FAIL: " << e.what() << "\n";
		return 1;
	}
}
