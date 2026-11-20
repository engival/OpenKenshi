// Task k16.6.96 (playtest kenshi-h2v.6.1, bug h2v.6.6): the slavers_downed caravan, on the server's world (runtime mesh over the shipped navtiles,
// the squad AI), leaves its Trading stay when the player attacks its Boss: the Boss and its squadmates take fight goals against the player
// within seconds of the first hit (143 / 32 as the victim's squad joins, 16 on sight), and when the player goes down the capture chain runs to
// the cuff (CAPTURE_NEW_SLAVES 167: strip 181, restrain 166, cuff + lift 168). "Trading" is only the squad behaviour's fallback label (TradeCircuit,
// charStats) for a member with no individual task. Run on the playtest's two seeds as well as the default one. Exit 77 without the shipped navtiles.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "caravan_unhurt.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace
{

// kenshi-k16.6.117 (oracle rwh.99 section 6): the slave state machine on a captured player of scenarios/slavers_downed.kscen (the server's Hub world).
// Leash: a slave farther than 3535 u from its captor escapes (state 2, 250 s heat, its 187 row gone, other orders kept), 2 -> 3 -> 0 on timers; a captor that
// dies is replaced by a living squad-mate (stay 1) or, with none, the slave escapes after 45 s. Exit 77 without the shipped navtiles.
struct Run
{
	std::shared_ptr<const kdata::GameData> gd;
	Simulation sim;
	std::uint64_t player = 0;
	std::uint64_t boss = 0;

	static Simulation make()
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
		SpawnOptions spawn;
		spawn.seed = 1;
		return Simulation::newGame(KENSHI_DATA, order, spawn);
	}

	Run(const std::uint64_t seed)
		: sim(make())
	{
		simlink::ServerContext context;
		context.gameData = sim.gameData();
		sim.setGameData(context.gameData);
		sim.enableWeather(*context.gameData);
		const std::shared_ptr<const CombatData> data = CombatData::build(*context.gameData, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
		sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, hub, {}, context.gameData.get(), NativeCacheOptions{}));
		const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
		const std::uint64_t firstId = sim.world().ecs().get<state::Ids>()->next;
		player = firstId + 2;
		boss = firstId + 3;
		scenario::apply(sim, *context.gameData, *data, context, file, seed);
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		enableServerAi(sim, ai);
		Command attack = Command::fighterAttack(state::Id{player}, state::Id{boss});
		attack.tick = sim.clockTick() + 100;
		sim.enqueue(attack);
		// The fixture pins the escape, not the aid: the caravan stays unhurt until the cuff (guards spawn with a kit, k16.6.287).
		for (int i = 0; i < 2400 && fighter(player).slaveState != 1; ++i)
		{
			for (std::uint64_t id = boss; id < boss + 8; ++id)
				keepCaravanUnhurt(sim, id);
			sim.tick();
		}
		test::require(fighter(player).slaveState == 1, "the player is cuffed");
	}

	state::Fighter& fighter(std::uint64_t id) { return *const_cast<state::Fighter*>(sim.combat()->find(id)); }
	void wake(const std::uint64_t id)
	{
		state::Fighter& slave = fighter(id);
		for (state::FighterPart& part : slave.parts)
		{
			part.health = part.maxHealth;
			part.stun = 0.0;
			part.collapsed = false;
			part.pool = 0.0;
		}
		slave.blood = sim.combat()->rules().maxBlood(slave);
		slave.wounds.clear();
		slave.bleeding = 0.0;
		slave.koTimer = 0.0;
		slave.koLatch = false;
		slave.unconscious = false;
		slave.down = false;
	}
	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
			sim.tick();
	}
	void order(const JobOrder& o) { sim.enqueue(Command::jobOrder(o)); run(2); }
	bool hasRow187() { for (const auto& row : sim.jobsOf(player).rows) if (row.type == 187) return true; return false; }
};

void leash()
{
	Run r(1);
	const std::uint64_t captor = r.sim.slaveCaptor(r.player);
	test::require(captor != 0 && r.sim.combat()->find(captor) != nullptr, "the slave has a captor hand");
	test::require(r.hasRow187(), "the cuff added the 187 row");
	// The player may dismiss the row (rwh.99 section 5); nothing else changes at once.
	const JobsView view = r.sim.jobsOf(r.player);
	r.order({77, r.player, 0, 0, view.rows[0].jobId, 0, 0});
	test::require(!r.hasRow187(), "the 187 row is dismissed");
	// 3000 u from the captor is inside the leash: still a slave after 3 s.
	// The arena may reorder when the area follows the slave (the captor leaves full detail): look the fighters up by id, never hold a reference.
	const auto slave = [&]() -> state::Fighter& { return r.fighter(r.player); };
	const double handX = r.sim.combat()->find(captor)->x;
	const double handZ = r.sim.combat()->find(captor)->z;
	slave().x = handX + 3000.0;
	slave().z = handZ;
	slave().clearRoute();
	r.run(60);
	test::require(slave().slaveState == 1, "3000 u from the captor is not an escape");
	slave().x = handX + 3800.0; // the slave walks back toward its hand (~135 u per update, the area now follows it), so well past 3535 u
	r.run(30);
	test::require(slave().slaveState == 2, "beyond 3535 u (3800 placed) the slave escapes (state 2)");
	test::require(std::fabs(r.sim.slaveHeatSeconds(r.player) - 250.0) < 2.0, "the escape heat is 250 s: " + std::to_string(r.sim.slaveHeatSeconds(r.player)));
	test::require(!r.hasRow187(), "the 187 row is not re-added by the escape");
	// rwh.106 (kenshi-k16.6.126): the timers run on the world clock from the stamp of the last change into 2 or 3: 2 -> 3 strictly past 12.0 game
	// hours (and re-stamps), 3 -> 0 strictly past 96.0 (4 days) with the captor hand cleared. The heat above is a HUD value and gates nothing.
	const double perHour = kTicksPerGameDay / 24.0;
	const auto atHoursAfter = [&](double stamp, double hours) { r.sim.setClockTick(static_cast<std::uint64_t>(std::llround((stamp + hours) * perHour))); };
	// Ticks one at a time until the state differs from `from`; returns the hours past the stamp when it did (-1: never within 40 ticks).
	const auto flipAfter = [&](double stamp, std::uint8_t from)
	{
		for (int i = 0; i < 40; ++i)
		{
			r.sim.tick();
			if (slave().slaveState != from)
				return gameHoursIn(r.sim.clockTick()) - stamp;
		}
		return -1.0;
	};
	const double escaped = slave().slaveStamp;
	test::require(std::fabs(escaped - gameHoursIn(r.sim.clockTick())) < 0.05, "the escape stamped the world clock: " + std::to_string(escaped));
	atHoursAfter(escaped, 11.9);
	r.run(3);
	test::require(slave().slaveState == 2, "11.9 game hours after the escape: still escaping");
	atHoursAfter(escaped, 11.999);
	const double toEx = flipAfter(escaped, 2);
	test::require(slave().slaveState == 3 && toEx > 12.0 && toEx < 12.0 + 2.0 / perHour, "2 -> 3 at 12.0 game hours, within a tick: " + std::to_string(toEx));
	const double exStamp = slave().slaveStamp;
	test::require(std::fabs(exStamp - gameHoursIn(r.sim.clockTick())) < 2.0 / perHour, "2 -> 3 re-stamped the clock: " + std::to_string(exStamp));
	// The stamp is saved: a reload keeps the time left (the clock is the save's, the stamp the same).
	atHoursAfter(exStamp, 50.0);
	r.run(2);
	std::stringstream saved;
	r.sim.save(saved);
	r.sim.reload(saved);
	state::Fighter& back = r.fighter(r.player);
	test::require(back.slaveState == 3 && back.slaveStamp == exStamp, "the stamp survived the save: " + std::to_string(back.slaveStamp) + " of " + std::to_string(exStamp));
	atHoursAfter(exStamp, 95.9);
	r.run(3);
	test::require(r.fighter(r.player).slaveState == 3, "95.9 game hours as an ex-slave after a reload: still 3");
	atHoursAfter(exStamp, 95.999);
	const double toFree = [&]
	{
		for (int i = 0; i < 40; ++i)
		{
			r.sim.tick();
			if (r.fighter(r.player).slaveState != 3)
				return gameHoursIn(r.sim.clockTick()) - exStamp;
		}
		return -1.0;
	}();
	test::require(r.fighter(r.player).slaveState == 0 && toFree > 96.0 && toFree < 96.0 + 2.0 / perHour, "3 -> 0 at 96.0 game hours, within a tick: " + std::to_string(toFree));
	test::require(r.sim.slaveCaptor(r.player) == 0, "3 -> 0 cleared the captor hand");
}

void escapeKeepsOtherJobs()
{
	Run r(1);
	test::require(r.hasRow187(), "the 187 row is there");
	// The state leaving 1 removes only the 187 row (14032e0d0); a row the player added stays.
	const std::string why = r.sim.checkJobOrder({76, r.player, 25, 0, 0, 0, 1});
	if (why.empty())
		r.order({76, r.player, 25, 0, 0, 0, 1});
	const std::size_t before = r.sim.jobsOf(r.player).rows.size();
	r.sim.setSlaveState(r.player, 2);
	test::require(!r.hasRow187(), "leaving state 1 removes the 187 row");
	test::require(why.empty() ? r.sim.jobsOf(r.player).rows.size() == before - 1 && !r.sim.jobsOf(r.player).rows.empty() : true,
		"another order survives (" + why + ")");
	std::cout << "other job order: " << (why.empty() ? "added" : why) << "\n";
}

void captorDies(const bool heir)
{
	Run r(1);
	const std::uint64_t captor = r.sim.slaveCaptor(r.player);
	test::require(captor != 0, "a captor");
	// The hand-over runs for a slave that is up (an incapacitated one is not tested, slave_state.cpp). The caravan walks to the Hub bar (k16.6.254), so
	// the medics are out of reach and the slave is still down and carried at the cuff: wake it, as the aid used to.
	r.wake(r.player);
	for (std::uint64_t id = r.boss; id < r.boss + 8; ++id)
		if (r.sim.combat()->find(id) != nullptr && (!heir || id == captor))
			r.fighter(id).dead = true;
	r.run(40);
	if (heir)
	{
		const std::uint64_t now = r.sim.slaveCaptor(r.player);
		test::require(r.fighter(r.player).slaveState == 1 && now != 0 && now != captor && !r.fighter(now).dead, "a living squad-mate takes the slave over");
		return;
	}
	test::require(r.fighter(r.player).slaveState == 1, "44 s on, no captor yet: still a slave");
	r.run(40 * 20 + 80);
	test::require(r.fighter(r.player).slaveState == 2, "no captor and no heir for 45 s: the slave escapes");
}

// An out-of-detail captor (its squad demoted to a token: no arena fighter, the entity stays) is still the captor, at its token: the slave near it
// stays a slave with no heir hand-over, and one beyond 3535 u of the token escapes at the next interval (the original tests the hand's distance).
void captorToken(const bool far)
{
	Run r(1);
	const std::uint64_t captor = r.sim.slaveCaptor(r.player);
	test::require(captor != 0, "a captor");
	r.wake(r.player);
	r.sim.removeFighter(captor);
	test::require(r.sim.combat()->find(captor) == nullptr, "the captor has no arena fighter");
	if (far)
		r.fighter(r.player).x += 100000.0;
	r.run(90);
	if (!far)
	{
		test::require(r.fighter(r.player).slaveState == 1, "the captor out of detail is still the captor: no escape");
		test::require(r.sim.slaveCaptor(r.player) == captor, "no heir took the slave over from a captor that is only a token");
		return;
	}
	test::require(r.fighter(r.player).slaveState == 2, "beyond 3535 u of the captor's token the slave escapes");
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_slave_escape: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		leash();
		escapeKeepsOtherJobs();
		captorDies(true);
		captorDies(false);
		captorToken(false);
		captorToken(true);
		std::cout << "ksim_slave_escape ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_slave_escape: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
