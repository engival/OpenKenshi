// The chase give-up monitor (kinspect findings/s73_static.md sections 3 and 5: Task_Move chaseGiveUpMonitor 14033a800). A chaser whose
// target stays sensed never gives up; one whose target got away (no scan senses it) gives up when the chase has run 5 game minutes and
// the target has been unsensed for more than 12 U = 13.2 game minutes (or the chaser stands within 60 u of the record's position),
// with EV_GIVE_UP_CHASE (whose dialogues all go home). Timed in game minutes: a game day is kGameDaySeconds = 1440 game minutes.

#include "ksim/combat.hpp"
#include "ksim/perception.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kGameMinuteSeconds = kGameDaySeconds / 1440.0;

struct Rig
{
	Simulation sim;
	std::uint64_t hunter = 0;
	std::uint64_t quarry = 0;
	std::size_t giveUps = 0;
	double giveUpTime = -1.0; // sim seconds since the rig started
	double giveUpChase = 0.0; // the event's a: chase seconds
	double giveUpUnseen = 0.0; // the event's b: unsensed seconds

	Rig(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, double quarryX)
		: sim(Simulation::load(KENSHI_DATA, 5, RegionConfig{}))
	{
		sim.enableCombat(data, CombatSettings{});
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: full daylight
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		std::uint64_t rng = 7;
		const auto spawn = [&](double x, int team, bool player)
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.x = x;
			f.z = 0.0;
			f.yaw = 1.5707963267948966; // facing +x
			f.team = team;
			f.playerControlled = player;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		quarry = spawn(quarryX, 0, true); // a player character picks no fights of its own
		hunter = spawn(0.0, 1, false);
		const_cast<CombatArena*>(sim.combat())->setObserver([this](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::GiveUpChase && e.who == hunter)
			{
				++giveUps;
				giveUpTime = e.time;
				giveUpChase = e.a;
				giveUpUnseen = e.b;
			}
		});
	}

	state::Fighter& me(std::uint64_t id) { return *const_cast<state::Fighter*>(sim.combat()->find(id)); }
};

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		std::printf("monitor: starts after %.2f s (5 game minutes), unsensed limit %.2f s (13.2 game minutes), 1 game minute = %.3f s\n",
			5.0 * kGameMinuteSeconds, 13.2 * kGameMinuteSeconds, kGameMinuteSeconds);

		// ---- a chase of a target that stays in sight never ends (here for 40 game minutes = 22 s... and then some) ----
		{
			Rig rig(gd, data, 300.0);
			const double seconds = 60.0 * kGameMinuteSeconds; // an hour of game time, far past 13.2 game minutes
			for (int i = 0; i < static_cast<int>(seconds * 20.0); ++i)
			{
				// The quarry runs ahead of the hunter at a constant gap, always in its cone.
				rig.me(rig.quarry).x = rig.me(rig.hunter).x + 300.0;
				rig.me(rig.quarry).z = rig.me(rig.hunter).z;
				rig.sim.run(1);
			}
			test::require(rig.giveUps == 0, "a chaser that keeps its target in sight never gives up");
			test::require(rig.me(rig.hunter).target == rig.quarry, "the chaser still has its target after an hour of game time");
			std::printf("ok  in sight: no give-up after %.0f game minutes of chase\n", seconds / kGameMinuteSeconds);
		}

		// ---- the target gets away: nobody senses it from then on ----
		{
			Rig rig(gd, data, 300.0);
			// Let the hunter notice it and set off.
			rig.sim.run(20 * 4);
			test::require(rig.me(rig.hunter).target == rig.quarry && rig.me(rig.hunter).state == state::CombatState::Approach,
				"the hunter noticed the quarry and is moving in on it");
			const double chaseStart = rig.sim.combat()->time();
			// The quarry vanishes 4 km away (beyond any sight range) and stays there; the hunter's scan stops sensing it.
			rig.me(rig.quarry).x = 4000.0;
			const double lost = rig.sim.combat()->time();
			for (int i = 0; i < 20 * 120 && rig.giveUps == 0; ++i)
			{
				// Pin it: a player character joins the fight against its attacker (PROTECT_ALLIES) and would walk back into sight.
				rig.me(rig.quarry).x = 4000.0;
				rig.me(rig.quarry).z = 0.0;
				rig.me(rig.quarry).target = 0;
				rig.sim.run(1);
				if (i % 100 == 0 && std::getenv("KSIM_CHASE_VERBOSE") != nullptr)
				{
					const state::Fighter& h = rig.me(rig.hunter);
					std::printf("  t+%d s: x %.0f state %d target %d chaseId %d age %.1f unseen %.1f\n", i / 20, h.x, static_cast<int>(h.state), h.target != 0, h.chaseId != 0, h.chaseAge, h.chaseUnseen);
				}
			}
			test::require(rig.giveUps == 1, "a chaser whose target is out of sight gives up");
			const double unseenMinutes = (rig.giveUpTime - lost) / kGameMinuteSeconds;
			const double chaseMinutes = (rig.giveUpTime - chaseStart) / kGameMinuteSeconds;
			std::printf("    gave up %.2f game minutes after losing the target (event: chase %.2f, unsensed %.2f game minutes), %.2f since the chase began\n",
				unseenMinutes, rig.giveUpChase / kGameMinuteSeconds, rig.giveUpUnseen / kGameMinuteSeconds, chaseMinutes);
			// More than 12 U = 13.2 game minutes unsensed, and not before 5 game minutes of chase; the scan that last sensed it and
			// the 1.25-scan "sensed" slack add up to about 2 sim seconds (about 3.6 game minutes) of lag.
			test::require(rig.giveUpUnseen > 13.2 * kGameMinuteSeconds, "the monitor waits for more than 13.2 game minutes unsensed");
			test::require(unseenMinutes > 13.2 && unseenMinutes < 13.2 + 5.0, "the give-up comes just after 13.2 game minutes without the target");
			test::require(rig.giveUpChase >= 5.0 * kGameMinuteSeconds, "the monitor starts after 5 game minutes of chase");
			const state::Fighter& h = rig.me(rig.hunter);
			test::require(h.target == 0 && h.chaseId == 0, "the chase is over: no target");
			test::require(!perception::aware(h, rig.quarry), "the chaser forgot its quarry");
			for (int i = 0; i < 20 * 10; ++i)
			{
				rig.me(rig.quarry).x = 4000.0;
				rig.me(rig.quarry).target = 0;
				rig.sim.run(1);
				if (std::getenv("KSIM_CHASE_VERBOSE") != nullptr && rig.me(rig.hunter).target != 0)
				{
					std::printf("  retargeted after %d ticks, aware %d, quarry target %d, quarry x %.0f\n", i, perception::aware(rig.me(rig.hunter), rig.quarry), rig.me(rig.quarry).target != 0, rig.me(rig.quarry).x);
					break;
				}
			}
			test::require(rig.me(rig.hunter).target == 0, "it does not take the quarry up again from memory");
			test::require(rig.giveUps == 1, "one give-up, not one per tick");
			std::printf("ok  out of sight: gave up after %.2f game minutes unsensed (limit 13.2), the target is dropped and forgotten\n", unseenMinutes);
		}

		// ---- within 60 u of the record's position: the chaser that walks up to a target nobody senses gives up on arrival ----
		{
			Rig rig(gd, data, 1000.0);
			state::Fighter& q = rig.me(rig.quarry);
			state::Fighter& h = rig.me(rig.hunter);
			q.building = 7; // in another building (a fake id): no sight scan reaches it, and standing still it makes no noise
			h.target = rig.quarry; // the hunter was told of it (the attacker's onTargetedBy)
			perception::notice(h, rig.quarry);
			rig.sim.run(1);
			const double start = rig.sim.combat()->time();
			for (int i = 0; i < 20 * 120 && rig.giveUps == 0; ++i)
			{
				q.x = 1000.0; // pinned: a player character would walk to its attacker
				q.z = 0.0;
				q.building = 7; // pinned again: a fighter the sim sees move is placed in its building by position (stepBuildings)
				q.target = 0;
				rig.sim.run(1);
			}
			test::require(rig.giveUps == 1, "a chaser that walks up to its unsensed target's record gives up");
			const double dist = std::hypot(h.x - q.x, h.z - q.z);
			const double minutes = (rig.giveUpTime - start) / kGameMinuteSeconds;
			std::printf("    gave up %.2f game minutes into the chase, %.0f u from the target, unsensed %.2f game minutes\n", minutes, dist,
				rig.giveUpUnseen / kGameMinuteSeconds);
			test::require(dist < 60.0 + 10.0, "it gave up within 60 u of the record's position");
			test::require(rig.giveUpUnseen < 13.2 * kGameMinuteSeconds, "not by the age rule: the radius rule fired first");
			test::require(rig.giveUpChase >= 5.0 * kGameMinuteSeconds, "not before 5 game minutes of chase");
			std::printf("ok  near the record: gave up %.2f game minutes into the chase\n", minutes);
		}
		std::printf("ksim_chase_giveup: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
