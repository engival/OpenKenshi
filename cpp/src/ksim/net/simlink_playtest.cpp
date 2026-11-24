// Playtest scenarios (task CE; SIMLINK.md "SCENARIO", docs/reference/PLAYTEST.md): the client frames SCENARIO_LIST_REQ (102),
// SCENARIO_LOAD (103, the world swap) and SCENARIO_END (104, the run's metrics), and the records SCENARIO_LIST (54), SCENARIO_STARTED
// (55), SCENARIO_METRICS (56). Session controls like load_game: run between ticks, never in the order log, every answer a frame. Any
// session may play: the playtest window is the user's own client, not the loopback admin session (UNSURE for multiplayer, where
// MULTIPLAYER 6.2's pause policy decides). The run itself is observation only (ScenarioRun).

#include "ksim/net/simlink.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <stdexcept>

namespace ksim::simlink {

namespace {

// The metric vocabulary (SIMLINK.md "SCENARIO"): the names a scenario header may list. Appending is the only change allowed.
struct MetricName
{
	std::string_view name;
	const char* meaning;
};

constexpr MetricName kMetricNames[] = {
	{ "targeted_player", "characters that ever had a player character as their target" },
	{ "crew_join_s", "game seconds from the provoke (the first player fight) to a bystander fighting a player's attacker (the bar's crew)" },
	{ "bodies_down", "fighters that were in the fight and are down (KO, unconscious or dead) at the end" },
	{ "fight_s", "game seconds from the first engagement to the last" },
	{ "player_downed_s", "game seconds from the start to the first player character down" },
	{ "captured", "1 when a player character was carried or shackled during the run" },
};

// A name that is a file name only: no path, no traversal. The scenario's name is the stem of `scenarios/<name>.kscen`.
bool validScenarioName(const std::string& name)
{
	if (name.empty() || name.size() > 64 || name == "." || name == "..")
		return false;
	return name.find('/') == std::string::npos && name.find('\\') == std::string::npos;
}

} // namespace

bool isScenarioMetric(std::string_view name)
{
	for (const MetricName& metric : kMetricNames)
	{
		if (metric.name == name)
			return true;
	}
	return false;
}

double ScenarioRun::elapsedSeconds(std::uint64_t tick) const
{
	return static_cast<double>(ticksSince(tick, startTick)) * kTickSeconds;
}

void ScenarioRun::reset(std::string scenarioName, std::uint64_t scenarioFileHash, std::uint32_t runSeed, std::uint64_t start,
	std::vector<std::string> metricNames)
{
	*this = ScenarioRun();
	active = true;
	name = std::move(scenarioName);
	fileHash = scenarioFileHash;
	seed = runSeed;
	startTick = start;
	metrics = std::move(metricNames);
}

void ScenarioRun::sample(const Simulation& sim, std::uint64_t tick)
{
	const CombatArena* arena = sim.combat();
	if (arena == nullptr)
		return;
	// This tick's player characters. The set grows over the run, so one spawned after the load counts from its first tick. A
	// player-less file (spectate_fight, spectator mode) has none: the fight metrics are still sampled, because its two NPC squads
	// fight on their own (2026-11-08: the sample used to return here, so that run reported no metrics at all).
	const PlayerSquadsView squads = sim.playerSquads();
	for (const PlayerSquadsView::Squad& squad : squads.squads)
		players.insert(squad.members.begin(), squad.members.end());
	bool engagedNow = false;
	downNow.clear();
	for (const CombatArena::Member& member : arena->members())
	{
		const state::Fighter& f = member.fighter;
		const bool isPlayer = players.count(member.id) != 0;
		const bool playerTarget = f.target != 0 && players.count(f.target) != 0;
		const bool engaged = !f.engaged.empty() || (playerTarget && !isPlayer);
		if (engaged)
		{
			engagedNow = true;
			everEngaged.insert(member.id);
		}
		if (playerTarget && !isPlayer)
			everTargetedPlayer.insert(member.id);
		// Down is every arena member's, so bodies_down ("fighters that were in the fight and are down", SIMLINK.md "Metrics")
		// counts the NPCs it always meant to; firstDownTick stays the player's (player_downed_s).
		if (f.down || f.unconscious || f.dead || f.koTimer > 0.0)
		{
			downNow.insert(member.id);
			if (isPlayer && firstDownTick == 0)
				firstDownTick = tick;
		}
		if (isPlayer)
		{
			// The provoke: the first tick a player character has a target or is attacked.
			if (f.target != 0 || !f.engaged.empty())
				if (provokeTick == 0)
					provokeTick = tick;
			if (f.carriedBy != 0 || f.slaveState != 0)
				playerCarried = true;
		}
	}
	if (engagedNow)
	{
		if (firstEngageTick == 0)
			firstEngageTick = tick;
		lastEngageTick = tick;
	}
	// The crew: a bystander (never a player's attacker itself) fighting a character that ever targeted a player — the bar's guards going
	// at the lure's bandits.
	if (crewTick == 0 && provokeTick != 0 && !everTargetedPlayer.empty())
	{
		for (const CombatArena::Member& member : arena->members())
		{
			if (players.count(member.id) != 0 || everTargetedPlayer.count(member.id) != 0)
				continue;
			const state::Fighter& f = member.fighter;
			bool crew = everTargetedPlayer.count(f.target) != 0;
			for (const std::uint64_t attacker : f.engaged)
				crew = crew || everTargetedPlayer.count(attacker) != 0;
			if (crew)
			{
				crewTick = tick;
				break;
			}
		}
	}
}

double ScenarioRun::value(std::string_view metric, std::uint64_t tick, std::uint8_t& valid) const
{
	valid = 1;
	if (metric == "targeted_player")
		return static_cast<double>(everTargetedPlayer.size());
	if (metric == "crew_join_s")
	{
		if (crewTick == 0)
		{
			valid = 0;
			return 0.0;
		}
		return elapsedSeconds(crewTick) - elapsedSeconds(provokeTick); // the provoke is the first player's fight, as PLAYTEST.md 3 says
	}
	if (metric == "bodies_down")
	{
		std::size_t down = 0;
		for (const std::uint64_t id : downNow)
		{
			if (everEngaged.count(id) != 0)
				++down;
		}
		return static_cast<double>(down);
	}
	if (metric == "fight_s")
	{
		if (firstEngageTick == 0)
		{
			valid = 0;
			return 0.0;
		}
		return static_cast<double>(lastEngageTick - firstEngageTick) * kTickSeconds;
	}
	if (metric == "player_downed_s")
	{
		if (firstDownTick == 0)
		{
			valid = 0;
			return 0.0;
		}
		return elapsedSeconds(firstDownTick);
	}
	if (metric == "captured")
		return playerCarried ? 1.0 : 0.0;
	(void)tick;
	valid = 0;
	return 0.0;
}

bool Session::handleScenarioFrame(std::uint16_t type, const std::uint8_t* payload, std::size_t size, RecordWriter& out)
{
	Reader r{payload, size};
	const std::uint32_t seq = r.u32();
	if (type == kScenarioListReq)
	{
		if (r.bad || r.at != size)
		{
			refuse(out, "malformed SCENARIO_LIST_REQ");
			return false;
		}
		try
		{
			runScenarioList(seq, out);
		} catch (const std::exception& e) {
			refuse(out, std::string("scenario_list: ") + e.what());
			return false;
		}
		return true;
	}
	std::string name;
	std::uint32_t seed = 0;
	if (type == kScenarioLoad)
	{
		name = r.str();
		seed = r.u32();
	}
	if (r.bad || r.at != size)
	{
		refuse(out, type == kScenarioLoad ? "malformed SCENARIO_LOAD" : "malformed SCENARIO_END");
		return false;
	}
	// Not the loopback admin gate of 80-84: a playtest runs on the user's own client, which is a normal session (PLAYTEST.md 2).
	if (type == kScenarioLoad)
		runScenarioLoad(seq, name, seed, out);
	else
		runScenarioEnd(seq, out);
	return true;
}

void Session::writeScenarioStarted(RecordWriter& out, std::uint32_t seq, std::uint8_t status, const std::string& error,
	const std::string& name, std::uint64_t hash, std::uint32_t runSeed, float centreX, float centreZ, const std::string& bead)
{
	out.begin(kScenarioStarted);
	out.u32("clientSeq", seq);
	out.u8("status", status);
	out.str("error", error);
	out.str("name", name);
	out.u64("fileHash", hash);
	out.u32("seed", runSeed);
	out.str("simHash", std::string(buildHash()));
	out.f64("startGameTime", status == 0 ? static_cast<double>(sim_.clockTick()) * kTickSeconds : 0.0);
	// The tail (SIMLINK.md "SCENARIO"): the file's centre, so the client can frame a player-less scenario.
	out.f32("centreX", centreX);
	out.f32("centreZ", centreZ);
	out.str("bead", bead); // the tail after the centre (2026-11-10, client-approved): "" on a refusal and for a file without bead=
	out.end();
}

void Session::runScenarioList(std::uint32_t seq, RecordWriter& out)
{
	std::vector<scenario::ScenarioInfo> found;
	if (!ctx_.scenariosDir.empty())
		found = scenario::listScenarios(ctx_.scenariosDir); // a file that does not parse throws: the caller answers the ERROR
	out.begin(kScenarioList);
	out.u32("clientSeq", seq);
	out.list("scenarios", found.size(), 16);
	for (const scenario::ScenarioInfo& info : found)
	{
		out.item();
		out.str("name", info.name);
		out.str("title", info.title);
		out.str("setup", info.setup);
		out.str("lookFor", info.lookFor);
		out.u64("fileHash", info.fileHash);
		out.u8("metricCount", static_cast<std::uint8_t>(info.metrics.size()));
		for (const std::string& metric : info.metrics)
			out.str("metric", metric);
	}
	out.listEnd();
	// The frame tail (2026-11-10, client-approved): the bead ids in list order ("" = no bead=). Not a per-entry field: the entries are
	// packed back to back, so an older client would misparse the next one.
	out.u16("beadCount", static_cast<std::uint16_t>(found.size()));
	for (const scenario::ScenarioInfo& info : found)
		out.str("bead", info.bead);
	out.end();
}

void Session::runScenarioLoad(std::uint32_t seq, const std::string& name, std::uint32_t seed, RecordWriter& out)
{
	if (!validScenarioName(name))
	{
		writeScenarioStarted(out, seq, 1, "bad scenario name '" + name + "'", name, 0, seed);
		return;
	}
	if (ctx_.scenariosDir.empty())
	{
		writeScenarioStarted(out, seq, 1, "the server has no scenario directory", name, 0, seed);
		return;
	}
	if (!sim_.hasPristine())
	{
		writeScenarioStarted(out, seq, 1, "the server has no pristine world", name, 0, seed);
		return;
	}
	if (sim_.combat() == nullptr)
	{
		writeScenarioStarted(out, seq, 1, "the server has no combat arena", name, 0, seed);
		return;
	}
	scenario::File file;
	scenario::ScenarioInfo info;
	try
	{
		const std::vector<scenario::ScenarioInfo> found = scenario::listScenarios(ctx_.scenariosDir);
		const auto it = std::find_if(found.begin(), found.end(), [&](const scenario::ScenarioInfo& s) { return s.name == name; });
		if (it == found.end())
		{
			writeScenarioStarted(out, seq, 1, "no scenario '" + name + "'", name, 0, seed);
			return;
		}
		info = *it;
		file = scenario::parseFile(info.path);
		if (ctx_.gameData != nullptr)
			scenario::check(*ctx_.gameData, file);
	} catch (const std::exception& e) {
		writeScenarioStarted(out, seq, 1, e.what(), name, info.fileHash, seed);
		return;
	}
	// 0 = the server picks: the tick mixed, so two plays of one scenario differ.
	const std::uint32_t runSeed = seed != 0 ? seed : static_cast<std::uint32_t>(splitMix64(sim_.clockTick() ^ 0x5CE7A110ull));
	try
	{
		sim_.newWorld(); // the pristine world: no player faction, no scenario
		restoreViewFocus();
		const scenario::Placed placed = scenario::apply(sim_, *ctx_.gameData, sim_.combat()->data(), ctx_, file, runSeed);
		if (ctx_.log)
			ctx_.log("scenario_load " + name + " (seed " + std::to_string(runSeed) + "): " + std::to_string(placed.players.size()) + " player-controlled, "
				+ std::to_string(placed.others.size()) + " other characters");
	} catch (const std::exception& e) {
		// The world has already been swapped (the scenario's, or the bare one if apply stopped halfway): tell the session what it
		// has before the refusal, so a failed load never leaves a stale world on the client.
		ctx_.looks.clear();
		ctx_.cameraStart.reset(); // the world is no longer the import
		resetKnown();
		sendTick(out, true);
		writeScenarioStarted(out, seq, 1, e.what(), name, info.fileHash, runSeed);
		return;
	}
	// The world is the scenario's now: nothing this session sent before is true.
	ctx_.looks.clear();
	ctx_.cameraStart.reset(); // the world is no longer the import: a later session gets no CAMERA_START
	resetKnown();
	progressLevel_ = 1; // the client sends order 98 again after SCENARIO_STARTED
	ctx_.scenario.reset(name, info.fileHash, runSeed, sim_.clockTick(), info.metrics);
	ackApplied(out, seq, sim_.clockTick());
	writeScenarioStarted(out, seq, 0, "", name, info.fileHash, runSeed,
		static_cast<float>(file.centreX), static_cast<float>(file.centreZ), info.bead);
	sendTick(out, true);
	notifySaves(true);
}

void Session::runScenarioEnd(std::uint32_t seq, RecordWriter& out)
{
	ackApplied(out, seq, sim_.clockTick());
	writeScenarioMetrics(out, seq);
	ctx_.scenario.active = false; // the run is over; the world stays until the next scenario_load or new_world
}

void Session::writeScenarioMetrics(RecordWriter& out, std::uint32_t seq)
{
	const ScenarioRun& run = ctx_.scenario;
	out.begin(kScenarioMetrics);
	out.u32("clientSeq", seq);
	out.str("name", run.name);
	out.f64("elapsedGameS", run.name.empty() ? 0.0 : run.elapsedSeconds(sim_.clockTick()));
	out.u8("metricCount", static_cast<std::uint8_t>(run.metrics.size()));
	for (const std::string& metric : run.metrics)
	{
		std::uint8_t valid = 0;
		const double value = run.name.empty() ? 0.0 : run.value(metric, sim_.clockTick(), valid);
		out.str("metric", metric);
		out.f64("value", value);
		out.u8("valid", valid);
	}
	out.end();
}

} // namespace ksim::simlink
