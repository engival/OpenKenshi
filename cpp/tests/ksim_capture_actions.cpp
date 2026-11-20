// Task k16.6.137 (k16.6.136 parts 1-3, rwh.114): the slavers_downed capture on the scenario path (the server's world: runtime mesh over the shipped
// navtiles, the squad AI): the cuff 166 takes its 9.9 game s (not 0.3 s), and the capturer's CHAR_DYNAMIC action is 'medic' for the strip (0.8 s) and
// the cuff, then 'shoulder lift left' for the lift. Exit 77 without the shipped navtiles.
#include "ksim/ai/ai.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
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
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace
{

constexpr std::int64_t kForceTicks = 400; // the forced aid's span (20 s) after the cuff
constexpr std::int64_t kLiftBoundTicks = 1200; // the lift starts within 60 s of the aid's end

int play(const std::uint64_t seed, const bool forceAid = false)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
	SpawnOptions spawn;
	spawn.seed = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
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
	const std::uint64_t kPlayer = firstId + 2;
	const std::uint64_t kBoss = firstId + 3;
	scenario::apply(sim, *context.gameData, *data, context, file, seed);
	ServerAiOptions ai;
	ai.registryPath = KSIM_AI_TABLE;
	enableServerAi(sim, ai);
	std::vector<std::pair<std::uint64_t, std::string>> lines;
	sim.setTaskLog([&](const std::string& line) { lines.emplace_back(sim.clockTick(), line); });
	Command attack = Command::fighterAttack(state::Id{kPlayer}, state::Id{kBoss});
	attack.tick = sim.clockTick() + 100;
	sim.enqueue(attack);
	const std::string tag = " (seed " + std::to_string(seed) + ")";
	const auto when = [&](const std::string& needle)
	{
		for (const auto& [tick, line] : lines)
			if (line.find(needle) != std::string::npos)
				return static_cast<std::int64_t>(tick);
		return std::int64_t{-1};
	};
	// The capturer's action per tick once the strip is logged: the 'medic' ticks and what the lift shows.
	std::uint64_t capturer = 0;
	std::uint64_t bodyId = 0;
	std::int64_t medicTicks = 0;
	std::uint64_t medicStart = 0;
	std::int64_t liftTicks = 0;
	std::uint64_t liftStart = 0;
	std::int64_t attachTick = -1;
	bool loopWrong = false;
	for (int i = 0; i < 9000; ++i)
	{
		sim.tick();
		if (capturer == 0 && when("capture 181 strip member") >= 0)
		{
			for (const auto& [tick, line] : lines)
				if (const std::size_t at = line.find("capture 181 strip member "); at != std::string::npos)
				{
					capturer = std::stoull(line.substr(at + 25));
					bodyId = std::stoull(line.substr(line.find(" body ") + 6));
				}
		}
		if (capturer == 0)
			continue;
		if (forceAid)
		{
			// The interruption set directly, not left to a seed's phase: from the cuff the body is held down (the squad's medics treat it on 58,
			// the lift waits on the aid) for kForceTicks, then let go still unconscious; the lift must follow through 168 (a capture an aid
			// interrupted is not a dead end).
			const std::int64_t cuffed = when("capture 166 restrain done");
			if (cuffed >= 0 && static_cast<std::int64_t>(sim.clockTick()) < cuffed + kForceTicks)
			{
				state::Fighter* body = const_cast<CombatArena*>(sim.combat())->find(bodyId);
				body->unconscious = true;
				body->down = true;
				body->koTimer = 1.0e6;
			}
		}
		if (const std::optional<Simulation::CaptureAction> action = sim.captureAction(capturer))
		{
			if (action->clip == "medic")
			{
				medicStart = medicTicks == 0 ? action->start : medicStart;
				loopWrong = loopWrong || !action->loop || action->start != medicStart;
				++medicTicks;
			} else if (action->clip == "shoulder lift left") {
				liftStart = liftTicks == 0 ? action->start : liftStart;
				loopWrong = loopWrong || action->loop || action->start != liftStart;
				++liftTicks;
				if (attachTick < 0 && sim.combat()->find(capturer)->carrying != 0)
					attachTick = static_cast<std::int64_t>(sim.clockTick());
			}
		}
		if (liftTicks > 0 && sim.combat()->find(capturer)->carrying != 0 && !sim.captureAction(capturer))
			break; // the lift is done
	}
	const std::int64_t strip = when("capture 181 strip member");
	const std::int64_t done = when("capture 166 restrain done");
	test::require(strip >= 0 && done >= 0, "the capture runs to the cuff" + tag);
	// (1) The cuff is not cut short on the scenario path: the bar runs its whole 12 s (240 ticks; the exe's task is 9.9 game s, rwh.114 section 3, and
	// the live span measured 12.0 s with the strip and the AI step around it). The client lead's 0.3 s did not reproduce here (kenshi-k16.6.137).
	const double bar = static_cast<double>(done - strip) * kTickSeconds;
	std::printf("seed %llu: strip at tick %lld, cuff done at %lld (%.2f s), 'medic' %lld ticks, lift %lld ticks, attach at %lld\n", static_cast<unsigned long long>(seed),
		static_cast<long long>(strip), static_cast<long long>(done), bar, static_cast<long long>(medicTicks), static_cast<long long>(liftTicks),
		static_cast<long long>(attachTick));
	test::require(bar >= 11.0 && bar <= 13.0, "166 lasts 12 +- 1 s on the scenario path (" + std::to_string(bar) + ")" + tag);
	// (2) The capturer shows 'medic' (looping, one start) for the whole bar.
	test::require(std::fabs(static_cast<double>(medicTicks) * kTickSeconds - kRestrainSeconds) <= 0.2, "'medic' shows for the whole bar (" + std::to_string(medicTicks) + " ticks)" + tag);
	test::require(!loopWrong, "the actions keep one start tick each, medic loops and the lift does not" + tag);
	if (liftTicks > 0)
	{
		// The lift clip: attach at 40 % (0.987 s), done at 95 % (2.343 s).
		test::require(std::fabs(static_cast<double>(liftTicks) * kTickSeconds - kLiftSeconds) <= 2.0 * kTickSeconds, "'shoulder lift left' shows for 2.343 s (" + std::to_string(liftTicks) + " ticks)" + tag);
		test::require(attachTick >= 0 && std::fabs(static_cast<double>(attachTick - static_cast<std::int64_t>(liftStart)) * kTickSeconds - kLiftAttachSeconds) <= 2.0 * kTickSeconds,
			"the body attaches at 0.987 s of the lift" + tag);
	}
	if (forceAid)
	{
		test::require(when("goal Medic member " + std::to_string(capturer) + " target " + std::to_string(bodyId)) >= done
				|| when(" target " + std::to_string(bodyId) + " score") >= done, "the squad's medics took the cuffed body on goal 58 after the cuff" + tag);
		test::require(liftTicks > 0, "a capture interrupted by an aid still lifts" + tag);
		test::require(liftTicks > 0 && static_cast<std::int64_t>(liftStart) <= done + kForceTicks + kLiftBoundTicks,
			"the lift follows the aid's end within " + std::to_string(kLiftBoundTicks) + " ticks (started " + std::to_string(static_cast<std::int64_t>(liftStart) - done - kForceTicks) + ")" + tag);
	}
	return liftTicks > 0 ? 1 : 0; // lifted on this seed
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_capture_actions: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		// Recipe 30's ring (k16.7.95) moved the fights: seeds 1, 2, 3, 7, 101, 706960716 and 1151889715 show no lift (the slave is up and follows after the aid);
		// 4, 6, 100, 103 and 104 are carried (survey 2026-11-19, kenshi-k16.7.104). The forced aid below still runs on seed 1.
		int lifted = 0;
		for (const std::uint64_t seed : {std::uint64_t{4}, std::uint64_t{6}, std::uint64_t{100}})
			lifted += play(seed);
		test::require(lifted >= 1, "at least one seed shows the capture's lift");
		play(1, true);
		std::cout << "ksim_capture_actions ok\n";
	} catch (const std::exception& e) {
		std::cerr << "ksim_capture_actions: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
