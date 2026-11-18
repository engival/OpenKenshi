// SIMLINK server end to end (docs/formats/SIMLINK.md): an in-process ksim server (a thread
// running SimLinkServer on a loopback port) and a test client built on the client session's own
// decoder (client/src/net/simlink.cpp, compiled in unchanged, so the two halves are checked
// against each other). Checks: HELLO both ways, refusal on a data hash / protocol / garbage
// mismatch, the string table before first use, snapshot bracketing, orders answered with event
// 9 or 10 carrying the tick, move_to / attack / stop / block / pause / speed / interest / view
// working, and that the session replayed from the order log reaches the same state hash. Then
// the recorded sample stream in docs/formats decodes without error.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/debugview/enums.hpp"
#include "ksim/navigator.hpp"
#include "ksim/perception.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#if KSIM_DEBUG_VIEW
#include "ksim/debugview/decisions.hpp"
#endif

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "net/simlink.hpp" // client/src/net/simlink.hpp

#include "test_util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <thread>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_SAMPLE_BIN
#error KSIM_SAMPLE_BIN must be defined
#endif

namespace {

namespace ks = ksim::simlink;
using Wall = std::chrono::steady_clock;

// ---- the server under test ----

struct World
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const ksim::CombatData> data;
	ks::ServerContext context;
	std::unique_ptr<ksim::Simulation> sim;
	ks::Duel duel;
};

// What the test thread may know of a running server: read on the server's own thread (SimLinkServer::call), never through w.sim or
// w.context from the test thread (k16.15.5: a data race under TSan). stateHash costs a world walk, so it is only taken on ask.
struct Seen
{
	std::uint64_t tick = 0;
	bool paused = false;
	bool idle = false;
	bool pausedNow = false;
	float speed = 0.0f;
	std::uint64_t stateHash = 0;
	std::size_t orderLog = 0;
};

Seen lookAt(ks::SimLinkServer& server, const World& w, bool hash = false)
{
	Seen seen;
	server.call([&]()
	{
		seen.tick = w.sim->clockTick();
		seen.paused = w.context.paused;
		seen.idle = w.context.idle;
		seen.pausedNow = w.context.pausedNow();
		seen.speed = w.context.speed;
		seen.orderLog = w.context.orderLog.size();
		if (hash)
			seen.stateHash = w.sim->stateHash();
	});
	return seen;
}

World makeWorld(const kworld::LoadOrder& order, const std::shared_ptr<const ksim::CombatData>& data,
	const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<ksim::MeshNavigator>& nav, int limbLossLevel = 1)
{
	World w;
	ksim::SpawnOptions options;
	options.seed = 7;
	options.settleSeconds = 0; // the tests pin the t0 world and clock
	w.sim = std::make_unique<ksim::Simulation>(ksim::Simulation::newGame(KENSHI_DATA, order, options));
	w.gd = gd;
	w.data = data;
	w.context.gameData = gd;
	w.context.dataHash = ks::dataHash(order);
	w.context.squadLimit = 50;
	ksim::CombatSettings settings;
	settings.substeps = 2;
	settings.limbLossLevel = limbLossLevel;
	w.sim->enableCombat(data, settings);
	w.sim->enableNavigation(nav);
	w.duel = ks::spawnDuel(*w.sim, *gd, *data, w.context);
	return w;
}

// Task CE: the bare world the scenario wire places into, as ksim_server starts one (markPristine before any scenario).
World makeScenarioWorld(const kworld::LoadOrder& order, const std::shared_ptr<const ksim::CombatData>& data,
	const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<ksim::MeshNavigator>& nav)
{
	World w;
	ksim::SpawnOptions options;
	options.seed = 7;
	options.settleSeconds = 0; // the tests pin the t0 world and clock
	w.sim = std::make_unique<ksim::Simulation>(ksim::Simulation::newGame(KENSHI_DATA, order, options));
	w.gd = gd;
	w.data = data;
	w.context.gameData = gd;
	w.context.dataHash = ks::dataHash(order);
	w.context.squadLimit = 50;
	ksim::CombatSettings settings;
	settings.substeps = 2;
	w.sim->enableCombat(data, settings);
	w.sim->enableNavigation(nav);
	w.sim->markPristine(); // the bare world: scenario_load places the file on it
	return w;
}

// --scenario newgame (ksim_server): the world as the server builds it, with the scenario's NEW_GAME enqueued for the next tick.
// The sim is idle with no ready session, so the first tick a client's connect triggers is the one that restarts the world.
World makeNewGameWorld(const kworld::LoadOrder& order, const std::shared_ptr<const ksim::CombatData>& data,
	const std::shared_ptr<const kdata::GameData>& gd, const std::shared_ptr<ksim::MeshNavigator>& nav)
{
	World w;
	ksim::SpawnOptions options;
	options.seed = 7;
	options.settleSeconds = 0; // the tests pin the t0 world and clock
	w.sim = std::make_unique<ksim::Simulation>(ksim::Simulation::newGame(KENSHI_DATA, order, options));
	w.gd = gd;
	w.data = data;
	w.context.gameData = gd;
	w.context.dataHash = ks::dataHash(order);
	w.context.squadLimit = 50;
	ksim::CombatSettings settings;
	settings.substeps = 2;
	w.sim->enableCombat(data, settings);
	w.sim->enableNavigation(nav);
	w.sim->markPristine(); // the server's mark, before the scenario's command: order 84 new_world returns here
	const std::string start = "1980-gamedata.base";
	test::require(w.sim->checkNewGame(start, "Nameless", {}).empty(), "--scenario newgame: the start is refused");
	ksim::Command command = ksim::Command::newGame(start, "Nameless", 0);
	command.tick = w.sim->clockTick() + 1;
	w.sim->enqueue(command);
	return w;
}

// ---- what the client's decoder does not know yet ----
//
// The decoder (client/src/net/simlink.cpp, owned by kenshi-0f) reads CHAR_DYNAMIC with `driveSeq` and
// CHAR_ROUTE (type 23), but has no event 12 (position_corrected) yet. The test therefore reads event 12
// itself and keeps every CHAR_ROUTE it sees (the decoder holds only the latest per character).
// Delete the event 12 part when the client decoder learns it.

struct RouteRecord
{
	std::uint64_t tick = 0;
	std::uint64_t id = 0;
	std::uint32_t clientSeq = 0;
	std::uint32_t version = 0;
	std::uint8_t flags = 0;
	std::vector<std::array<float, 3>> points;
};

struct Extras
{
	std::vector<RouteRecord> routes;
	std::vector<std::pair<std::uint64_t, float>> corrections; // id, distance
	std::vector<std::pair<std::uint64_t, std::uint64_t>> noticed; // EVENT 15: observer, target
	struct Hit
	{
		std::uint64_t victim;
		std::uint8_t result;
		float cut, blunt, cutAbsorbed, bluntAbsorbed;
	};
	std::vector<Hit> hits; // event 1 with its absorbed tail
	struct Medical
	{
		std::uint16_t severed; // the trailing field
		std::vector<std::uint32_t> partStrids; // row i = bit i
		float bloodMax = 0.0f, bleed = 0.0f, hunger = 0.0f; // the panel tail
		std::uint8_t flags = 0;
		std::vector<float> partMax, bandage;
		std::vector<std::uint8_t> limb;
	};
	std::map<std::uint64_t, Medical> medical; // the latest CHAR_MEDICAL by id
	std::vector<std::pair<std::uint64_t, std::uint32_t>> limbSevered; // EVENT 47: id, part strid
	// EVENT 48 (task BG), read here because the client's decoder predates it: id, cause, the push direction, the strength,
	// the hit part and the flop seed; plus the CHAR_DYNAMIC rest tail it follows.
	struct Ragdoll
	{
		std::uint64_t tick = 0;
		std::uint64_t id = 0;
		std::uint8_t cause = 0;
		float dir[3] = {};
		float strength = 0.0f;
		std::uint32_t part = 0;
		std::uint32_t seed = 0;
	};
	std::vector<Ragdoll> ragdolls;
	std::vector<std::uint16_t> eventKinds; // every EVENT kind in wire order
	std::vector<float> restYaws; // each prone CHAR_DYNAMIC's trailing restYaw
	std::set<std::uint64_t> proneSeen; // ids streamed with flag bit 5
	std::set<std::uint64_t> uprightSeen; // ids streamed without it
	std::map<std::uint64_t, float> aimYaws; // the last CHAR_DYNAMIC aim tail by id (flag bit 20)
	std::set<std::uint64_t> aimSeen; // ids streamed with bit 20
	std::set<std::uint64_t> aimCleared; // ids that streamed bit 20 and later a record without it
	std::map<std::uint64_t, std::uint64_t> turrets; // the last CHAR_DYNAMIC mounted tail by id (flag bit 21)
	std::map<std::uint64_t, std::size_t> mountedLength; // the record length of the last mounted frame, by id
	std::set<std::uint64_t> mountedSeen; // ids streamed with bit 21
	std::set<std::uint64_t> mountedCleared; // ids that streamed bit 21 and later a record without it
	struct Shot // EVENT 49 (client-approved 2026-11-10)
	{
		std::uint64_t shooter = 0;
		std::uint64_t target = 0;
		float from[3] = {};
		float to[3] = {};
		std::uint8_t result = 0;
		std::uint32_t weapon = 0;
		std::uint64_t turret = 0;
	};
	std::vector<Shot> shots;
	struct StealthKo // EVENT 50 (client-approved 2026-11-10)
	{
		std::uint64_t assassin = 0;
		std::uint64_t victim = 0;
		std::uint8_t result = 0;
		float chance = 0.0f;
	};
	std::vector<StealthKo> stealthKos;
	bool recordedSample = false; // the recorded sample stream predates the CHAR_MEDICAL severed field (an old server's frames)
	std::size_t detects = 0; // CHAR_DETECT frames (type 26)
	std::size_t qualities = 0; // ITEM_QUALITY frames (type 27)
	std::map<std::uint64_t, std::uint32_t> portraits; // CHAR_APPEARANCE portraitSerial (protocol 5, the last field) by id
	struct WeatherWind
	{
		std::uint64_t tick = 0;
		bool present = false;
		float speed = 0.0f;
		float yaw = 0.0f;
		std::vector<std::uint32_t> groups; // per region: group strid
		std::vector<std::uint32_t> weathers; // per region: weather strid
		std::vector<float> strengths; // per region
	};
	std::vector<WeatherWind> winds; // every WEATHER frame's optional trailing pair
	// The debug view (frames 200-202, docs/formats/SIMLINK.md "DEBUG VIEW"): read here, then consumed, the client's decoder predates them.
	struct DebugInfo
	{
		std::uint8_t version = 0;
		std::uint8_t enabled = 0;
	};
	struct DebugSquads
	{
		std::uint64_t tick = 0;
		std::vector<std::uint64_t> ids;
		std::vector<std::uint32_t> templates; // the row's template strid; 0 for a player squad (it has no SQUAD_TEMPLATE)
	};
	struct DebugTree
	{
		std::uint64_t tick = 0;
		std::uint8_t kind = 0;
		std::uint64_t id = 0;
		std::uint16_t rows = 0;
		std::vector<std::string> text; // per row: "depth key = value"
	};
	std::vector<DebugInfo> debugInfos;
	std::vector<DebugSquads> debugSquads;
	std::vector<DebugTree> debugTrees;
	// The playtest scenario wire (task CE; SIMLINK.md SCENARIO): read here, the client's decoder predates them (they are answer frames
	// with no `tick`).
	struct ScenarioRow
	{
		std::string name, title, setup, lookFor, bead;
		std::uint64_t fileHash = 0;
		std::vector<std::string> metrics;
	};
	struct ScenarioList
	{
		std::uint32_t seq = 0;
		std::vector<ScenarioRow> rows;
	};
	struct ScenarioStarted
	{
		std::uint32_t seq = 0;
		std::uint8_t status = 0;
		std::string error, name, simHash, bead;
		std::uint64_t fileHash = 0;
		std::uint32_t seed = 0;
		double startGameTime = 0.0;
		// The 2026-11-08 tail (the file's centre): a frame without it leaves these NaN, as the server writes for a refusal.
		float centreX = std::numeric_limits<float>::quiet_NaN();
		float centreZ = std::numeric_limits<float>::quiet_NaN();
		bool hasCentre = false;
	};
	struct ScenarioMetric
	{
		std::string name;
		double value = 0.0;
		std::uint8_t valid = 0;
	};
	struct ScenarioMetrics
	{
		std::uint32_t seq = 0;
		std::string name;
		double elapsedGameS = 0.0;
		std::vector<ScenarioMetric> metrics;
	};
	std::vector<ScenarioList> scenarioLists;
	std::vector<ScenarioStarted> scenarioStarted;
	std::vector<ScenarioMetrics> scenarioMetrics;
	std::vector<std::uint64_t> lastSquads; // the latest SQUADS (30) id set, in wire order
	std::uint64_t lastSquadsTick = 0;
	bool countSquads = false; // set once DEBUG_INFO says the view is on: every SQUADS must be followed by its DEBUG_SQUADS
	bool squadsPending = false;
};

template <typename T>
T readAt(const std::uint8_t* p, std::size_t at)
{
	T v;
	std::memcpy(&v, p + at, sizeof(T));
	return v;
}

// A wire `str` (u16 length + UTF-8 bytes) at `at` in a frame payload.
std::string wireString(const std::uint8_t* p, std::size_t n, std::size_t& at)
{
	test::require(n - at >= 2, "wire str past the frame's end");
	std::uint16_t len = 0;
	std::memcpy(&len, p + at, 2);
	at += 2;
	test::require(n - at >= len, "wire str bytes past the frame's end");
	std::string text(reinterpret_cast<const char*>(p + at), len);
	at += len;
	return text;
}

// False when the frame was consumed here (event 12). Otherwise it is for the decoder.
bool intercept(std::uint16_t type, const std::uint8_t* p, std::size_t n, Extras& extras)
{
	if (type == ks::kScenarioList || type == ks::kScenarioStarted || type == ks::kScenarioMetrics)
	{
		// The scenario wire's three server records (task CE): answer frames, no `tick`; every string inline (str, not strid).
		std::size_t at = 4; // clientSeq
		if (type == ks::kScenarioList)
		{
			Extras::ScenarioList list;
			std::memcpy(&list.seq, p, 4);
			test::require(n - at >= 2, "SCENARIO_LIST too short");
			std::uint16_t count = 0;
			std::memcpy(&count, p + at, 2);
			at += 2;
			for (std::uint16_t i = 0; i < count; ++i)
			{
				Extras::ScenarioRow row;
				row.name = wireString(p, n, at);
				row.title = wireString(p, n, at);
				row.setup = wireString(p, n, at);
				row.lookFor = wireString(p, n, at);
				test::require(n - at >= 8, "SCENARIO_LIST row hash");
				std::memcpy(&row.fileHash, p + at, 8);
				at += 8;
				test::require(n - at >= 1, "SCENARIO_LIST row metric count");
				const std::uint8_t metrics = p[at++];
				for (std::uint8_t m = 0; m < metrics; ++m)
					row.metrics.push_back(wireString(p, n, at));
				list.rows.push_back(std::move(row));
			}
			// The bead tail (2026-11-10): u16 n, n x str bead; a client that predates it stops after the rows.
			if (n - at >= 2)
			{
				std::uint16_t beads = 0;
				std::memcpy(&beads, p + at, 2);
				at += 2;
				test::require(beads == count, "SCENARIO_LIST bead tail count equals n");
				for (std::uint16_t i = 0; i < beads; ++i)
					list.rows[i].bead = wireString(p, n, at);
			}
			test::require(at == n, "SCENARIO_LIST has trailing bytes");
			extras.scenarioLists.push_back(std::move(list));
		} else if (type == ks::kScenarioStarted) {
			Extras::ScenarioStarted started;
			std::memcpy(&started.seq, p, 4);
			test::require(n - at >= 1, "SCENARIO_STARTED too short");
			started.status = p[at++];
			test::require(started.status <= 1, "SCENARIO_STARTED status is 0 or 1");
			started.error = wireString(p, n, at);
			started.name = wireString(p, n, at);
			test::require(n - at >= 8, "SCENARIO_STARTED fileHash");
			std::memcpy(&started.fileHash, p + at, 8);
			at += 8;
			test::require(n - at >= 4, "SCENARIO_STARTED seed");
			std::memcpy(&started.seed, p + at, 4);
			at += 4;
			started.simHash = wireString(p, n, at);
			test::require(n - at >= 8, "SCENARIO_STARTED startGameTime");
			std::memcpy(&started.startGameTime, p + at, 8);
			at += 8;
			// The centre tail (2026-11-08): f32 centreX, centreZ after startGameTime; a client that predates it stops here.
			if (n - at >= 8)
			{
				std::memcpy(&started.centreX, p + at, 4);
				std::memcpy(&started.centreZ, p + at + 4, 4);
				started.hasCentre = true;
				at += 8;
				if (n - at >= 2)
					started.bead = wireString(p, n, at); // the 2026-11-10 tail
			}
			test::require(at == n, "SCENARIO_STARTED has trailing bytes");
			test::require((started.status == 0) == started.error.empty(), "SCENARIO_STARTED: status 0 with an error, or a refusal without one");
			extras.scenarioStarted.push_back(std::move(started));
		} else {
			Extras::ScenarioMetrics metrics;
			std::memcpy(&metrics.seq, p, 4);
			metrics.name = wireString(p, n, at);
			test::require(n - at >= 8, "SCENARIO_METRICS elapsedGameS");
			std::memcpy(&metrics.elapsedGameS, p + at, 8);
			at += 8;
			test::require(n - at >= 1, "SCENARIO_METRICS metric count");
			const std::uint8_t count = p[at++];
			for (std::uint8_t i = 0; i < count; ++i)
			{
				Extras::ScenarioMetric metric;
				metric.name = wireString(p, n, at);
				test::require(n - at >= 9, "SCENARIO_METRICS row");
				std::memcpy(&metric.value, p + at, 8);
				at += 8;
				metric.valid = p[at++];
				test::require(metric.valid <= 1, "SCENARIO_METRICS valid is 0 or 1");
				metrics.metrics.push_back(std::move(metric));
			}
			test::require(at == n, "SCENARIO_METRICS has trailing bytes");
			extras.scenarioMetrics.push_back(std::move(metrics));
		}
		return false; // consumed here: the client's decoder does not know them
	}
	if (type == simlink::kWeather)
	{
		// WEATHER (41): tick, u16 n, n x 24 bytes, then the optional trailing pair (f32 windSpeed, f32 windYaw) when the client
		// has a view focus in a region.
		test::require(n >= 10, "WEATHER too short");
		Extras::WeatherWind wind;
		wind.tick = readAt<std::uint64_t>(p, 0);
		const std::uint16_t count = readAt<std::uint16_t>(p, 8);
		for (std::uint16_t i = 0; i < count; ++i)
		{
			wind.groups.push_back(readAt<std::uint32_t>(p, 10u + 24u * i));
			wind.weathers.push_back(readAt<std::uint32_t>(p, 14u + 24u * i));
			wind.strengths.push_back(readAt<float>(p, 30u + 24u * i));
		}
		const std::size_t base = 10u + 24u * count;
		test::require(n == base || n == base + 8, "WEATHER length does not match its count");
		if (n == base + 8)
		{
			wind.present = true;
			wind.speed = readAt<float>(p, base);
			wind.yaw = readAt<float>(p, base + 4);
		}
		extras.winds.push_back(wind);
	}
	if (type == simlink::kSquads)
	{
		// SQUADS (30): tick, u32 n, n x {u64 id, strid faction, f32 x, f32 z, u16 members}. The decoder reads it too; kept here so
		// DEBUG_SQUADS can be checked against the SQUADS it follows.
		test::require(n >= 12, "SQUADS too short");
		const std::uint32_t count = readAt<std::uint32_t>(p, 8);
		test::require(n == 12u + 22u * count, "SQUADS length does not match its count");
		extras.lastSquadsTick = readAt<std::uint64_t>(p, 0);
		extras.lastSquads.clear();
		for (std::uint32_t i = 0; i < count; ++i)
			extras.lastSquads.push_back(readAt<std::uint64_t>(p, 12u + 22u * i));
		if (extras.countSquads)
		{
			test::require(!extras.squadsPending, "a SQUADS frame came without the previous one's DEBUG_SQUADS");
			extras.squadsPending = true;
		}
	}
	if (type == ksim::debugview::kDebugInfo)
	{
		// DEBUG_INFO (200): u8 debugVersion, u8 enabled.
		test::require(n == 2, "DEBUG_INFO length");
		extras.debugInfos.push_back({p[0], p[1]});
		return false; // consumed here: the client's decoder does not know it
	}
	if (type == ksim::debugview::kDebugSquads)
	{
		// DEBUG_SQUADS (201): tick, u32 n, then per squad the AI fields of the SQUADS set (55 fixed bytes + 8 a member).
		test::require(n >= 12, "DEBUG_SQUADS too short");
		Extras::DebugSquads frame;
		frame.tick = readAt<std::uint64_t>(p, 0);
		const std::uint32_t count = readAt<std::uint32_t>(p, 8);
		std::size_t at = 12;
		for (std::uint32_t i = 0; i < count; ++i)
		{
			test::require(n >= at + 55, "DEBUG_SQUADS row too short");
			frame.ids.push_back(readAt<std::uint64_t>(p, at));
			frame.templates.push_back(readAt<std::uint32_t>(p, at + 8));
			test::require(p[at + 16] < ksim::debugview::kGoalKindCount && p[at + 17] < ksim::debugview::kAiReasonCount
				&& p[at + 18] < ksim::debugview::kOrderStatusCount && p[at + 19] < ksim::debugview::kDetailCount
				&& p[at + 24] < ksim::debugview::kTargetKindCount && p[at + 45] <= 1,
				"DEBUG_SQUADS enum or route flag out of range");
			at += 55u + 8u * p[at + 54];
		}
		test::require(at == n, "DEBUG_SQUADS length does not match its rows");
		if (extras.countSquads)
		{
			if (!(frame.tick == extras.lastSquadsTick && frame.ids == extras.lastSquads))
			{
				std::string detail = "DEBUG_SQUADS does not repeat the SQUADS it follows (tick " + std::to_string(frame.tick)
					+ " vs " + std::to_string(extras.lastSquadsTick) + ")";
				for (std::size_t i = 0; i < frame.ids.size() && i < extras.lastSquads.size(); ++i)
				{
					if (frame.ids[i] != extras.lastSquads[i])
						detail += " first difference at " + std::to_string(i) + ": " + std::to_string(frame.ids[i])
							+ " vs " + std::to_string(extras.lastSquads[i]);
				}
				test::require(false, detail);
			}
			extras.squadsPending = false;
		}
		extras.debugSquads.push_back(std::move(frame));
		return false;
	}
	if (type == ksim::debugview::kDebugTree)
	{
		// DEBUG_TREE (202): tick, u8 kind, u64 id, u16 nRows, then per row u16 depth, str key, str value (inline strings).
		test::require(n >= 19, "DEBUG_TREE too short");
		Extras::DebugTree tree;
		tree.tick = readAt<std::uint64_t>(p, 0);
		tree.kind = p[8];
		tree.id = readAt<std::uint64_t>(p, 9);
		tree.rows = readAt<std::uint16_t>(p, 17);
		test::require(tree.rows <= 4096, "DEBUG_TREE row count");
		std::size_t at = 19;
		for (std::uint16_t i = 0; i < tree.rows; ++i)
		{
			test::require(n >= at + 4, "DEBUG_TREE row truncated");
			const std::uint16_t depth = readAt<std::uint16_t>(p, at);
			test::require(depth == 0 ? i == 0 : depth <= 8, "DEBUG_TREE depths are pre-order, from 0");
			const std::uint16_t keyLength = readAt<std::uint16_t>(p, at + 2);
			test::require(n >= at + 4 + keyLength + 2, "DEBUG_TREE row key truncated");
			const std::uint16_t valueLength = readAt<std::uint16_t>(p, at + 4u + keyLength);
			test::require(n >= at + 4u + keyLength + 2u + valueLength, "DEBUG_TREE row value truncated");
			tree.text.push_back(std::to_string(depth) + " " + std::string(reinterpret_cast<const char*>(p + at + 4), keyLength) + " = "
				+ std::string(reinterpret_cast<const char*>(p + at + 6u + keyLength), valueLength));
			at += 4u + keyLength + 2u + valueLength;
		}
		test::require(at == n, "DEBUG_TREE length does not match its rows");
		extras.debugTrees.push_back(tree);
		return false;
	}
	if (type == 23)
	{
		test::require(n >= 27, "CHAR_ROUTE too short");
		RouteRecord r;
		r.tick = readAt<std::uint64_t>(p, 0);
		r.id = readAt<std::uint64_t>(p, 8);
		r.clientSeq = readAt<std::uint32_t>(p, 16);
		r.version = readAt<std::uint32_t>(p, 20);
		r.flags = p[24];
		const std::uint16_t count = readAt<std::uint16_t>(p, 25);
		test::require(n == 27 + 12u * count, "CHAR_ROUTE length does not match its count");
		for (std::uint16_t i = 0; i < count; ++i)
			r.points.push_back({readAt<float>(p, 27 + 12u * i), readAt<float>(p, 31 + 12u * i), readAt<float>(p, 35 + 12u * i)});
		extras.routes.push_back(std::move(r));
	}
	if (type == simlink::kAppearance)
	{
		test::require(n >= 8 + 8 + 4, "CHAR_APPEARANCE too short");
		extras.portraits[readAt<std::uint64_t>(p, 8)] = readAt<std::uint32_t>(p, n - 4);
	}
	if (type == simlink::kMedical)
	{
		// CHAR_MEDICAL (22): tick, id, f32 blood, u8 n, n x {strid part, f32 health, f32 stun}, u16 severed (always present, bit i = row i;
		// n <= 16). Checked here, then decoded by the client.
		test::require(n >= 21 && p[20] > 0 && p[20] <= 16, "CHAR_MEDICAL count");
		// Then the panel tail: f32 bloodMax, f32 bleed, f32 hunger, u8 flags, u8 n, n x {f32 max, f32 bandage, f32 splint, u8 limb}.
		const bool old = extras.recordedSample && n == 21u + 12u * p[20];
		const std::size_t tail = 23u + 12u * p[20];
		test::require(old || n == tail + 14u + 13u * p[20], "CHAR_MEDICAL length does not match its count plus the severed field and the panel tail");
		Extras::Medical m;
		m.severed = old ? 0 : readAt<std::uint16_t>(p, 21u + 12u * p[20]);
		for (std::uint8_t i = 0; i < p[20]; ++i)
			m.partStrids.push_back(readAt<std::uint32_t>(p, 21u + 12u * i));
		test::require((m.severed >> p[20]) == 0, "CHAR_MEDICAL severed bit beyond the part rows");
		if (!old)
		{
			m.bloodMax = readAt<float>(p, tail);
			m.bleed = readAt<float>(p, tail + 4);
			m.hunger = readAt<float>(p, tail + 8);
			m.flags = p[tail + 12];
			test::require(p[tail + 13] == p[20], "CHAR_MEDICAL tail rows match the part rows");
			test::require(m.bloodMax > 0.0f && m.bleed >= 0.0f && m.hunger >= 0.0f && m.hunger <= 3.0f && (m.flags & ~15u) == 0,
				"CHAR_MEDICAL tail: bloodMax > 0, bleed >= 0, hunger 0..3, flags bits 0-3 only");
			for (std::uint8_t i = 0; i < p[20]; ++i)
			{
				const std::size_t at = tail + 14u + 13u * i;
				m.partMax.push_back(readAt<float>(p, at));
				m.bandage.push_back(readAt<float>(p, at + 4));
				m.limb.push_back(p[at + 12]);
				test::require(m.partMax.back() > 0.0f && m.bandage.back() >= 0.0f && (m.limb.back() & ~7u) == 0,
					"CHAR_MEDICAL tail row: max > 0, bandage >= 0, limb bits 0-2 only");
				test::require(((m.limb.back() & 3u) == 1u) == (((m.severed >> i) & 1u) != 0), "CHAR_MEDICAL limb state 1 agrees with the severed bit");
			}
		}
		extras.medical[readAt<std::uint64_t>(p, 8)] = std::move(m);
	}
	if (type == 26)
	{
		// CHAR_DETECT (26): tick, id, f32 meter, u8 state, u64 by.
		test::require(n == 8 + 8 + 4 + 1 + 8, "CHAR_DETECT length");
		test::require(p[20] <= 2 && readAt<float>(p, 16) >= 0.0f && readAt<float>(p, 16) <= 1.0f, "CHAR_DETECT state and meter range");
		++extras.detects;
	}
	if (type == 27)
	{
		// ITEM_QUALITY (27, additive: older decoders skip it): tick, id, version, n, n x {u16 index, strid maker, strid model, f32 level, u8 grade}.
		test::require(n >= 22 && n == 22u + 15u * readAt<std::uint16_t>(p, 20) && readAt<std::uint16_t>(p, 20) > 0, "ITEM_QUALITY length");
		const std::uint16_t count = readAt<std::uint16_t>(p, 20);
		for (std::uint16_t i = 0; i < count; ++i)
			test::require(readAt<float>(p, 32 + 15u * i) >= 0.0f && (p[36 + 15u * i] <= 5 || p[36 + 15u * i] == 255), "ITEM_QUALITY level / grade range");
		++extras.qualities;
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 1)
	{
		// Event 1: tick, kind, victim, attacker, part, pos x3, dir, damage x4, result, then (additive) f32 cutAbsorbed, f32 bluntAbsorbed.
		// (The recorded sample stream in docs/formats predates the tail: 60 bytes.)
		test::require(n == 68 || n == 60, "event 1 length (with the absorbed tail)");
		if (n == 68)
			extras.hits.push_back({readAt<std::uint64_t>(p, 10), p[59], readAt<float>(p, 43), readAt<float>(p, 47), readAt<float>(p, 60), readAt<float>(p, 64)});
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 15)
	{
		test::require(n == 26, "event 15 length");
		extras.noticed.push_back({readAt<std::uint64_t>(p, 10), readAt<std::uint64_t>(p, 18)});
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 47)
	{
		test::require(n == 22, "event 47 length (tick, kind, id, part strid)");
		extras.limbSevered.push_back({readAt<std::uint64_t>(p, 10), readAt<std::uint32_t>(p, 18)});
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 12)
	{
		test::require(n == 22, "event 12 length");
		extras.corrections.push_back({readAt<std::uint64_t>(p, 10), readAt<float>(p, 18)});
		return false;
	}
	if (type == simlink::kEvent && n >= 10)
		extras.eventKinds.push_back(readAt<std::uint16_t>(p, 8));
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 48)
	{
		// EVENT 48 (task BG): tick, kind, id, cause u8, dir f32 x3, strength f32, part strid, seed u32 (43 bytes). Additive:
		// the client's decoder does not know it yet, so the test consumes it here.
		test::require(n == 43, "event 48 length");
		Extras::Ragdoll r;
		r.tick = readAt<std::uint64_t>(p, 0);
		r.id = readAt<std::uint64_t>(p, 10);
		r.cause = p[18];
		test::require(r.cause <= 3, "event 48 cause is 0..3");
		for (int i = 0; i < 3; ++i)
			r.dir[i] = readAt<float>(p, 19u + 4u * static_cast<std::size_t>(i));
		r.strength = readAt<float>(p, 31);
		r.part = readAt<std::uint32_t>(p, 35);
		r.seed = readAt<std::uint32_t>(p, 39);
		const float len = std::sqrt(r.dir[0] * r.dir[0] + r.dir[1] * r.dir[1] + r.dir[2] * r.dir[2]);
		test::require(len < 1e-4f || std::fabs(len - 1.0f) < 1e-3f, "event 48 direction is unit or all zeros");
		test::require(r.strength >= 0.0f, "event 48 strength is not negative");
		extras.ragdolls.push_back(r);
		return false;
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 50)
	{
		// EVENT 50 'stealth_ko': tick, kind, assassin u64, victim u64, result u8, chance f32 (31 bytes).
		test::require(n == 31, "event 50 length");
		Extras::StealthKo s;
		s.assassin = readAt<std::uint64_t>(p, 10);
		s.victim = readAt<std::uint64_t>(p, 18);
		s.result = p[26];
		s.chance = readAt<float>(p, 27);
		test::require(s.result <= 2, "event 50 result is 0..2");
		test::require(s.chance >= 0.0f && s.chance <= 100.0f, "event 50 chance is 0..100");
		extras.stealthKos.push_back(s);
		return false;
	}
	if (type == simlink::kEvent && n >= 10 && readAt<std::uint16_t>(p, 8) == 49)
	{
		// EVENT 49 'shot': tick, kind, shooter u64, target u64, from f32 x3, to f32 x3, result u8, weapon strid, turret u64 (63 bytes).
		// The client's decoder does not know it yet, so the test consumes it here.
		test::require(n == 63, "event 49 length");
		Extras::Shot s;
		s.shooter = readAt<std::uint64_t>(p, 10);
		s.target = readAt<std::uint64_t>(p, 18);
		for (int i = 0; i < 3; ++i)
		{
			s.from[i] = readAt<float>(p, 26u + 4u * static_cast<std::size_t>(i));
			s.to[i] = readAt<float>(p, 38u + 4u * static_cast<std::size_t>(i));
		}
		s.result = p[50];
		test::require(s.result <= 2, "event 49 result is 0..2");
		s.weapon = readAt<std::uint32_t>(p, 51);
		s.turret = readAt<std::uint64_t>(p, 55);
		extras.shots.push_back(s);
		return false;
	}
	if (type == simlink::kDynamic && n >= 44)
	{
		// CHAR_DYNAMIC's rest tail (task BG): present only while flag bit 5 (prone) is set and bits 15 / 16 are clear. The aim
		// tail (client-approved 2026-11-07, bit 20) comes after both, so it is the last 4 bytes when set.
		const std::uint64_t id = readAt<std::uint64_t>(p, 8);
		const std::uint32_t flags = readAt<std::uint32_t>(p, 40);
		const bool pose = (flags & ((1u << 15) | (1u << 16))) != 0;
		const bool aim = (flags & (1u << 20)) != 0;
		// The mounted tail (client-approved 2026-11-10, bit 21): a u64 turret after the aim tail, the very end of the record.
		const bool mounted = (flags & (1u << 21)) != 0;
		const std::size_t mountedBytes = mounted ? 8 : 0;
		if (mounted)
		{
			extras.turrets[id] = readAt<std::uint64_t>(p, n - 8);
			extras.mountedLength[id] = n;
			extras.mountedSeen.insert(id);
		} else if (extras.mountedSeen.count(id) != 0) {
			extras.mountedCleared.insert(id);
		}
		if (aim)
		{
			const float aimYaw = readAt<float>(p, n - 4 - mountedBytes);
			test::require(aimYaw > -3.141593f && aimYaw <= 3.141593f, "the aim tail's yaw is not in (-pi, pi]");
			test::require(!pose && (flags & (1u << 5)) == 0, "an aim tail rides a seated / asleep / prone record");
			extras.aimYaws[id] = aimYaw;
			extras.aimSeen.insert(id);
		} else if (extras.aimSeen.count(id) != 0) {
			extras.aimCleared.insert(id);
		}
		if ((flags & (1u << 5)) != 0 && !pose)
		{
			const float yaw = readAt<float>(p, n - (aim ? 8 : 4) - mountedBytes);
			test::require(yaw >= 0.0f && yaw < 6.283186f, "the rest tail's yaw is not in [0, 2 pi)");
			extras.restYaws.push_back(yaw);
			extras.proneSeen.insert(id);
		} else if ((flags & (1u << 5)) == 0) {
			extras.uprightSeen.insert(id);
		}
	}
	return true;
}

// ---- the test client ----

struct Client
{
	int fd = -1;
	std::vector<std::uint8_t> raw;
	simlink::SimLinkDecoder dec;
	std::vector<simlink::Event> events; // every event so far
	std::vector<std::uint16_t> types; // every frame type so far
	std::uint64_t lastSnapshotEnd = 0;
	std::uint64_t snapshots = 0;
	std::vector<std::uint64_t> snapTicks; // the tick of every snapshot so far
	std::uint64_t openTick = 0;
	bool inSnap = false;
	bool closed = false;
	std::uint64_t lastEventTick = 0;
	bool allowTickRewind = false; // order 84 new_world rewinds the clock: its events carry smaller ticks
	std::size_t checkedFrames = 0;
	Extras extras;
	Wall::time_point lastPing = Wall::now();

	// The server drops a silent socket after 30 s (ServerOptions::pingTimeoutMs): a long wait sends PING every 4 s, as a
	// real client does (SIMLINK.md section 11).
	void keepAlive()
	{
		if (Wall::now() - lastPing < std::chrono::seconds(4))
			return;
		std::vector<std::uint8_t> frame;
		simlink::OrderWriter::ping(frame);
		send(frame);
		lastPing = Wall::now();
	}

	explicit Client(int port)
	{
		fd = ::socket(AF_INET, SOCK_STREAM, 0);
		test::require(fd >= 0, "client socket");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<std::uint16_t>(port));
		::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
		test::require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "connect");
	}
	~Client()
	{
		if (fd >= 0)
			::close(fd);
	}

	void send(const std::vector<std::uint8_t>& bytes)
	{
		std::size_t at = 0;
		while (at < bytes.size())
		{
			const ssize_t n = ::send(fd, bytes.data() + at, bytes.size() - at, MSG_NOSIGNAL);
			test::require(n > 0, "client send");
			at += static_cast<std::size_t>(n);
		}
	}

	// One decoded frame: bracketing rules and "no string id used before it was defined".
	void onFrame(std::uint16_t type, const std::uint8_t* p, std::size_t n)
	{
		types.push_back(type);
		const auto tickOf = [&]() { std::uint64_t t = 0; test::require(n >= 8, "short frame"); std::memcpy(&t, p, 8); return t; };
		switch (type)
		{
		case simlink::kSnapBegin:
			test::require(!inSnap, "SNAPSHOT_BEGIN inside a snapshot");
			openTick = tickOf();
			test::require(openTick > lastSnapshotEnd || snapshots == 0 || allowTickRewind, "snapshot ticks do not increase");
			inSnap = true;
			break;
		case simlink::kSnapEnd:
			test::require(inSnap, "SNAPSHOT_END without BEGIN");
			test::require(tickOf() == openTick, "SNAPSHOT_END tick differs from BEGIN");
			lastSnapshotEnd = openTick;
			++snapshots;
			snapTicks.push_back(openTick);
			inSnap = false;
			break;
		case simlink::kAppearance:
		case simlink::kDynamic:
		case 23: // CHAR_ROUTE
		case simlink::kSquads:
		case simlink::kBounds:
			test::require(inSnap, "character/squad/bounds record outside a snapshot");
			test::require(tickOf() == openTick, "record tick differs from its snapshot");
			break;
		case simlink::kEvent:
			test::require(!inSnap, "EVENT inside a snapshot");
			if (!allowTickRewind) // a new world resets the clock
				test::require(tickOf() >= lastEventTick, "events out of tick order");
			lastEventTick = tickOf();
			break;
		default:
			break;
		}
		if (!intercept(type, p, n, extras))
			return; // event 12: only this test reads it
		// The client decoder (kenshi-0f's) rejects trailing bytes and does not know the event 1 absorbed tail yet: it gets the frame without it.
		std::vector<std::uint8_t> trimmed;
		const std::uint8_t* frame = p - 6;
		std::size_t frameSize = n + 6;
		// Likewise the 2026-11-02 tails (CHAR_DYNAMIC actionFlags u8; the kind 4 INTERACTABLE progress f32, rate f32, progressTick u64) and 2026-11-04's.
		std::size_t cut = 0;
		if (type == simlink::kEvent && n == 68 && readAt<std::uint16_t>(p, 8) == 1)
			cut = 8;
		else if (type == simlink::kDynamic) // + the 2026-11-04 pose tail while flag 15 (seated) or 16 (asleep) is set,
		{ //   + the 2026-11-06 rest tail (f32 restYaw) while flag 5 (prone) is set and neither 15 nor 16 is
		  //   + the 2026-11-07 aim tail (f32 aimYaw) while flag 20 is set
		  //   + the 2026-11-10 mounted tail (u64 turret) while flag 21 is set, last
			const std::uint32_t flags = readAt<std::uint32_t>(p, 40);
			const bool pose = (flags & ((1u << 15) | (1u << 16))) != 0;
			cut = 1 + (pose ? 20 : 0) + (!pose && (flags & (1u << 5)) != 0 ? 4 : 0) + ((flags & (1u << 20)) != 0 ? 4 : 0) + ((flags & (1u << 21)) != 0 ? 8 : 0);
		}
		else if (type == simlink::kInteractable && n > 16) // + the 2026-11-04 ownership tail (strid owner, u16 lockLevel, strid model, f32 yaw)
			cut = p[16] == ks::kInteractableBed ? 0 : (p[16] == ks::kInteractableProducer ? 16 : p[16] == ks::kInteractableContainer ? 10 : 0) + 14; // the client decodes a bed whole (188febfb)
		else if (type == simlink::kWeather && n == 10u + 24u * readAt<std::uint16_t>(p, 8) + 8)
			cut = 8; // the 2026-11-06 wind pair
		if (cut != 0)
		{
			trimmed.assign(frame, frame + frameSize - cut);
			const std::uint32_t len = static_cast<std::uint32_t>(n - cut + 2);
			std::memcpy(trimmed.data(), &len, 4);
			frame = trimmed.data();
			frameSize = trimmed.size();
		}
		test::require(dec.feed(frame, frameSize), "decoder (frame type " + std::to_string(type) + "): " + dec.error()); // the decoder takes whole frames
		while (!dec.events.empty())
		{
			events.push_back(dec.events.front());
			dec.events.pop_front();
		}
		// String ids used by what the frame carried must already resolve.
		for (const auto& [id, c] : dec.chars)
		{
			if (c.hasAppearance)
			{
				test::require(!dec.str(c.look.race).empty(), "race string used before defined");
				for (const simlink::Item& item : c.look.items)
					test::require(!dec.str(item.item).empty(), "item string used before defined");
				if (c.look.faction != 0)
					test::require(!dec.str(c.look.faction).empty(), "faction string used before defined");
			}
			if (c.samples > 0 && c.cur.action != 0)
				test::require(!dec.str(c.cur.action).empty(), "action string used before defined");
		}
		for (const simlink::Squad& s : dec.squads)
		{
			if (s.faction != 0)
				test::require(!dec.str(s.faction).empty(), "squad faction string used before defined");
		}
		for (const simlink::Event& e : events)
		{
			if (e.kind == 10)
				test::require(!dec.str(e.str).empty(), "rejection reason used before defined");
		}
		++checkedFrames;
	}

	// Reads until pred() or the timeout; frames are split here and handed to onFrame whole.
	bool pump(const std::function<bool()>& pred, int timeoutMs)
	{
		const auto end = Wall::now() + std::chrono::milliseconds(timeoutMs);
		while (!pred())
		{
			if (Wall::now() > end)
				return false;
			keepAlive();
			pollfd p{fd, POLLIN, 0};
			if (::poll(&p, 1, 20) <= 0)
				continue;
			std::uint8_t block[65536];
			const ssize_t n = ::recv(fd, block, sizeof(block), 0);
			if (n <= 0)
			{
				closed = true;
				return pred();
			}
			raw.insert(raw.end(), block, block + n);
			std::size_t at = 0;
			while (raw.size() - at >= 6)
			{
				std::uint32_t len;
				std::uint16_t type;
				std::memcpy(&len, &raw[at], 4);
				if (raw.size() - at - 4 < len)
					break;
				std::memcpy(&type, &raw[at + 4], 2);
				// Frames with an error end the decode: let the caller look at dec.error().
				if (type == simlink::kError)
				{
					dec.feed(&raw[at], 4 + len);
					at += 4 + len;
					raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at));
					return true;
				}
				onFrame(type, &raw[at + 6], len - 2);
				at += 4 + len;
			}
			raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at));
		}
		return true;
	}

	const simlink::Event* find(std::uint16_t kind, std::uint32_t seq) const
	{
		for (const simlink::Event& e : events)
		{
			if (e.kind == kind && (kind != 9 && kind != 10 ? true : e.a == seq))
				return &e;
		}
		return nullptr;
	}
};

std::vector<std::uint8_t> rawOrder(std::uint32_t seq, std::uint16_t kind, const std::function<void(std::vector<std::uint8_t>&)>& body)
{
	std::vector<std::uint8_t> payload;
	const auto put = [&](const void* data, std::size_t size)
	{
		const auto* b = static_cast<const std::uint8_t*>(data);
		payload.insert(payload.end(), b, b + size);
	};
	put(&seq, 4);
	put(&kind, 2);
	if (body)
		body(payload);
	std::vector<std::uint8_t> out;
	const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 2);
	const std::uint16_t type = simlink::kOrder;
	out.resize(6);
	std::memcpy(out.data(), &len, 4);
	std::memcpy(out.data() + 4, &type, 2);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

template <typename T>
void put(std::vector<std::uint8_t>& v, T value)
{
	const auto* b = reinterpret_cast<const std::uint8_t*>(&value);
	v.insert(v.end(), b, b + sizeof(T));
}

std::vector<std::uint8_t> helloFrame(std::uint64_t hash, std::uint16_t protocol = simlink::kProtocol)
{
	std::vector<std::uint8_t> out;
	simlink::OrderWriter::hello(out, hash, "test");
	if (protocol != simlink::kProtocol)
		std::memcpy(out.data() + 6 + 4, &protocol, 2); // the protocol field
	return out;
}

// A client frame of any type (the debug view's control frames are not orders).
std::vector<std::uint8_t> rawFrame(std::uint16_t type, const std::vector<std::uint8_t>& payload)
{
	std::vector<std::uint8_t> out;
	const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 2);
	out.resize(6);
	std::memcpy(out.data(), &len, 4);
	std::memcpy(out.data() + 4, &type, 2);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

std::string reasonOf(const Client& c, const simlink::Event& e)
{
	return c.dec.str(e.str);
}

} // namespace

int main()
{
	try
	{
		const auto started = Wall::now();
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const ksim::CombatData> data = ksim::CombatData::build(*gd, KENSHI_DATA);

		// ---- the live session ----
		// The duel's navmesh, baked from the game data for the Hub zone (the server's startup does the same).
		const auto bakeStart = Wall::now();
		const std::shared_ptr<ksim::MeshNavigator> nav = ksim::MeshNavigator::bake(KENSHI_DATA,
			{ks::kHubZoneX, ks::kHubZoneZ}, {ks::kHubZoneX, ks::kHubZoneZ}, 4);
		std::cout << "navmesh bake: " << std::chrono::duration<double>(Wall::now() - bakeStart).count() << " s\n";
		// ---- the default pause policy: the sim idles while no ready session is connected ----
		{
			World w = makeWorld(order, data, gd, nav);
			std::promise<int> portPromise;
			ks::ServerOptions policyOptions;
			policyOptions.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer policyServer(*w.sim, w.context, policyOptions);
			policyServer.listen();
			const int policyPort = portPromise.get_future().get();
			std::atomic<bool> policyStop{false};
			std::thread policyThread([&]() { policyServer.run(policyStop); });
			const struct PolicyJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~PolicyJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} policyJoiner{policyStop, policyThread};
			const auto settled = [&](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); return lookAt(policyServer, w).tick; };
			const std::uint64_t tick0 = lookAt(policyServer, w).tick;
			auto client = std::make_unique<Client>(policyPort);
			client->pump([]() { return false; }, 700); // the server's HELLO arrives; the sim must not move
			test::require(client->dec.helloSeen() && client->dec.error().empty(), "policy: no server HELLO");
			test::require(lookAt(policyServer, w).tick == tick0 && lookAt(policyServer, w).pausedNow, "policy: the sim ticked before HELLO completed");
			client->send(helloFrame(w.context.dataHash));
			test::require(client->pump([&]() { return client->snapshots >= 1; }, 5000), "policy: no initial snapshot");
			const std::uint64_t first = client->lastSnapshotEnd;
			test::require(client->pump([&]() { return client->lastSnapshotEnd > first + 4; }, 5000), "policy: the sim did not start after HELLO");
			test::require(!lookAt(policyServer, w).pausedNow && lookAt(policyServer, w).speed == 1.0f && client->dec.clock.valid && !client->dec.clock.paused, "policy: not running at speed 1 after HELLO");
			// a speed change, then the last session leaves: the sim stops, the speed stays
			client->send(rawOrder(2001, 41, [](auto& v) { put<float>(v, 2.0f); }));
			test::require(client->pump([&]() { return lookAt(policyServer, w).speed == 2.0f; }, 5000), "policy: speed order");
			client.reset();
			settled(400);
			const std::uint64_t stopped = settled(400);
			test::require(settled(400) == stopped && lookAt(policyServer, w).idle && lookAt(policyServer, w).speed == 2.0f, "policy: the sim kept ticking after the last session left (or lost its speed)");
			// the next session resumes it at the speed it had
			client = std::make_unique<Client>(policyPort);
			client->send(helloFrame(w.context.dataHash));
			test::require(client->pump([&]() { return client->snapshots >= 1; }, 5000), "policy: no snapshot on reconnect");
			test::require(client->pump([&]() { return lookAt(policyServer, w).tick > stopped + 4; }, 5000) && lookAt(policyServer, w).speed == 2.0f && !lookAt(policyServer, w).idle, "policy: reconnect did not resume at the old speed");
			// an admin pause survives leaving and joining
			client->send(rawOrder(2002, 40, [](auto& v) { put<std::uint8_t>(v, 1); }));
			test::require(client->pump([&]() { return client->dec.clock.paused; }, 5000), "policy: pause order");
			client.reset();
			settled(400);
			client = std::make_unique<Client>(policyPort);
			client->send(helloFrame(w.context.dataHash));
			test::require(client->pump([&]() { return client->snapshots >= 1; }, 5000), "policy: no snapshot after the paused reconnect");
			const std::uint64_t pausedAt = settled(300);
			test::require(client->dec.clock.paused && lookAt(policyServer, w).paused && settled(500) == pausedAt, "policy: a joining session unpaused an admin pause");
			std::cout << "pause policy: held before HELLO, runs with a session, stops without one, keeps speed, admin pause survives\n";
		}
		// ---- one snapshot per catch-up batch at speed > 1 (SIM_SCHEDULING 5O): floor(speed) ticks per snapshot, events keep their own tick ----
		if (std::getenv("KSIM_SNAPSHOT_PER_TICK") == nullptr)
		{
			World w = makeWorld(order, data, gd, nav);
			std::promise<int> portPromise;
			ks::ServerOptions batchOptions;
			batchOptions.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer batchServer(*w.sim, w.context, batchOptions);
			batchServer.listen();
			const int batchPort = portPromise.get_future().get();
			std::atomic<bool> batchStop{false};
			std::thread batchThread([&]() { batchServer.run(batchStop); });
			const struct BatchJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~BatchJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} batchJoiner{batchStop, batchThread};
			Client c(batchPort);
			c.send(helloFrame(w.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "batch: no initial snapshot");
			// 1x: one snapshot per tick
			const std::size_t at1 = c.snapTicks.size();
			test::require(c.pump([&]() { return c.snapTicks.size() >= at1 + 20; }, 15000), "batch: no snapshots at 1x");
			for (std::size_t i = at1; i < c.snapTicks.size(); ++i)
				test::require(c.snapTicks[i] == c.snapTicks[i - 1] + 1, "batch: a 1x snapshot did not advance exactly one tick");
			// 5x: a snapshot covers up to 5 ticks, and at least one covers more than 1
			c.send(rawOrder(3001, 41, [](auto& v) { put<float>(v, 5.0f); }));
			test::require(c.pump([&]() { return lookAt(batchServer, w).speed == 5.0f; }, 5000), "batch: speed order");
			const std::size_t at5 = c.snapTicks.size() + 2; // skip the snapshot in flight when the speed changed
			const std::size_t evAt = c.events.size();
			test::require(c.pump([&]() { return c.snapTicks.size() >= at5 + 40; }, 30000), "batch: no snapshots at 5x");
			std::uint64_t ticks = 0;
			std::uint64_t widest = 0;
			for (std::size_t i = at5; i < c.snapTicks.size(); ++i)
			{
				const std::uint64_t step = c.snapTicks[i] - c.snapTicks[i - 1];
				test::require(step >= 1 && step <= 5, "batch: a 5x snapshot covers more than floor(speed) ticks");
				ticks += step;
				widest = std::max(widest, step);
			}
			test::require(widest > 1 && ticks > c.snapTicks.size() - at5, "batch: at 5x every snapshot still covered one tick");
			// each event lies in the tick span (previous snapshot, this one]: its own tick, not the snapshot's
			std::size_t ownTick = 0;
			for (std::size_t i = evAt; i < c.events.size(); ++i)
			{
				const auto next = std::lower_bound(c.snapTicks.begin(), c.snapTicks.end(), c.events[i].tick);
				test::require(next != c.snapTicks.end(), "batch: an event past the last snapshot");
				if (*next != c.events[i].tick)
					++ownTick;
			}
			std::cout << "snapshot batch: 1x one per tick; 5x " << (c.snapTicks.size() - at5) << " snapshots over " << ticks << " ticks (widest " << widest
				<< "), " << (c.events.size() - evAt) << " events, " << ownTick << " with a tick between two snapshots\n";
		}
		{
			World w = makeWorld(order, data, gd, nav);
			std::promise<int> portPromise;
			ks::ServerOptions freeOptions;
			freeOptions.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			freeOptions.runWithoutClients = true;
			ks::SimLinkServer freeServer(*w.sim, w.context, freeOptions);
			freeServer.listen();
			portPromise.get_future().get();
			std::atomic<bool> freeStop{false};
			std::thread freeThread([&]() { freeServer.run(freeStop); });
			const std::uint64_t tick0 = lookAt(freeServer, w).tick;
			std::this_thread::sleep_for(std::chrono::milliseconds(700));
			const std::uint64_t later = lookAt(freeServer, w).tick;
			freeStop.store(true);
			freeThread.join();
			test::require(later > tick0 + 4, "run-without-clients: the sim did not tick with no client");
			std::cout << "run-without-clients: ticks with no client\n";
		}

		// ---- a group move: the first selected gets the exact click, the others a count-seeded scatter ----
		// kinspect 0789d56 (DECODED): the others get click + r(sin a, cos a), r = u1 x sqrt(count x 60) + 5,
		// srand(count): the scatter is deterministic per member count and selection order, the same on every click.
		{
			World held = makeWorld(order, data, gd, nav);
			std::string servantSid;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
			{
				if (r->name == "Escaped Servant")
					servantSid = r->stringID;
			}
			test::require(!servantSid.empty(), "no Escaped Servant in the data");
			std::vector<std::uint64_t> group;
			group.push_back(held.duel.player);
			// A copy: spawnFighter below grows the arena's vector, which moves every Fighter (a reference here was a use-after-free).
			const ksim::state::Fighter first = *held.sim->combat()->find(held.duel.player);
			for (int i = 0; i < 2; ++i)
			{
				std::uint64_t rng = 4242 + i;
				ksim::state::Fighter extra = data->makeFighter(*gd, servantSid, "", rng);
				extra.playerControlled = true;
				extra.team = 0;
				const auto spot = nav->walker().snap({first.x + 6.0 + i, first.y, first.z + 6.0 + i}, 20.0);
				test::require(spot.has_value() && std::hypot(spot->x - (first.x + 6.0 + i), spot->z - (first.z + 6.0 + i)) < 2.0,
					"no mesh near the group spawn");
				extra.x = spot->x;
				extra.y = spot->y;
				extra.z = spot->z;
				group.push_back(held.sim->spawnFighter(std::move(extra)).get<ksim::state::Id>()->value);
			}
			// An open click: 25 u of mesh all around it so a scatter goal is not snapped away.
			double cx = 0.0;
			double cz = 0.0;
			bool open = false;
			for (double d = 40.0; d < 200.0 && !open; d += 20.0)
			{
				for (int k = 0; k < 16 && !open; ++k)
				{
					const double px = first.x + std::sin(k * 0.392699) * d;
					const double pz = first.z + std::cos(k * 0.392699) * d;
					open = true;
					for (int q = 0; q < 12 && open; ++q)
					{
						const auto spot = nav->walker().snap({px + std::sin(q * 0.523599) * 25.0, first.y,
							pz + std::cos(q * 0.523599) * 25.0}, 30.0);
						open = spot.has_value()
							&& std::hypot(spot->x - (px + std::sin(q * 0.523599) * 25.0),
								spot->z - (pz + std::cos(q * 0.523599) * 25.0)) < 5.0;
					}
					if (open)
					{
						cx = px;
						cz = pz;
					}
				}
			}
			test::require(open, "no open 25 u click point near the squad");
			std::promise<int> gPort;
			ks::ServerOptions gOptions;
			gOptions.onListening = [&gPort](int listening) { gPort.set_value(listening); };
			ks::SimLinkServer gServer(*held.sim, held.context, gOptions);
			gServer.listen();
			const int groupPort = gPort.get_future().get();
			std::atomic<bool> gStop{false};
			std::thread gThread([&]() { gServer.run(gStop); });
			const struct GroupJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~GroupJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} groupJoiner{gStop, gThread};
			Client c(groupPort);
			c.send(helloFrame(held.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "group move: no initial snapshot");
			const auto sendMove = [&](std::uint32_t seq, const std::vector<std::uint64_t>& ids)
			{
				c.send(rawOrder(seq, 1, [&](auto& v) {
					put<std::uint16_t>(v, static_cast<std::uint16_t>(ids.size()));
					for (const std::uint64_t id : ids)
						put<std::uint64_t>(v, id);
					put<float>(v, static_cast<float>(cx));
					put<float>(v, static_cast<float>(cz));
					put<float>(v, 0.0f);
					put<std::uint8_t>(v, 0);
				}));
			};
			const auto endpoints = [&](std::uint32_t seq, std::size_t count,
				std::map<std::uint64_t, std::array<float, 3>>& out) -> bool
			{
				out.clear();
				for (const RouteRecord& r : c.extras.routes)
				{
					// A clear carries the order's seq too (the route arrived): only a record with waypoints is a goal.
					if (r.clientSeq == seq && !r.points.empty())
						out[r.id] = r.points.back();
				}
				return out.size() == count;
			};
			sendMove(5001, group);
			std::map<std::uint64_t, std::array<float, 3>> goals;
			test::require(c.pump([&]() { return endpoints(5001, group.size(), goals); }, 10000), "group move: no route for every member");
			// The first selected gets the exact click; each other lands in the disc r = u1 x sqrt(3 x 60) + 5 around it.
			test::require(std::hypot(goals[group[0]][0] - cx, goals[group[0]][2] - cz) < 3.0,
				"the first selected did not get the exact click");
			for (std::size_t i = 1; i < group.size(); ++i)
			{
				const double d = std::hypot(goals[group[i]][0] - cx, goals[group[i]][2] - cz);
				test::require(d <= std::sqrt(group.size() * 60.0) + 5.0 + 3.0,
					"a scatter goal lies outside the disc bound sqrt(count x 60) + 5");
				test::require(d >= 5.0 - 3.0, "a scatter goal lies inside the disc's 5 u minimum radius");
			}
			// srand(count): the same group order again (a different seq) lands the same goals...
			sendMove(5002, group);
			std::map<std::uint64_t, std::array<float, 3>> again;
			test::require(c.pump([&]() { return endpoints(5002, group.size(), again); }, 10000), "group move: no route on the repeat order");
			for (const std::uint64_t id : group)
				test::require(std::hypot(goals[id][0] - again[id][0], goals[id][2] - again[id][2]) < 0.5,
					"the repeat order landed a different goal: the scatter is not count-seeded");
			// ... and a group of two draws its offsets from srand(2), a different scatter than srand(3)'s.
			const std::vector<std::uint64_t> pair = {group[0], group[1]};
			sendMove(5003, pair);
			std::map<std::uint64_t, std::array<float, 3>> pairGoals;
			test::require(c.pump([&]() { return endpoints(5003, pair.size(), pairGoals); }, 10000), "group move: no route for the pair");
			test::require(std::hypot(goals[group[1]][0] - pairGoals[group[1]][0], goals[group[1]][2] - pairGoals[group[1]][2]) > 1.0,
				"a group of two drew the same offset as a group of three: the scatter is not seeded by the count");
			std::cout << "group move: exact click + count-seeded scatter, disc bound sqrt(3 x 60) + 5 = " << std::sqrt(3 * 60.0) + 5.0 << "\n";
		}

		// ---- the event 1 absorbed tail on a bare and on an armoured victim ----
		// A server with the default pause policy (the sim runs once HELLO is in). The player attacks the servant, whose armour is stripped
		// (`bare`) or replaced by one piece that covers every part (so any landed hit is absorbed); the first landed hit on it is returned.
		const auto strike = [&](bool bare) -> Extras::Hit
		{
			World held = makeWorld(order, data, gd, nav);
			ksim::state::Fighter& servant = *const_cast<ksim::CombatArena*>(held.sim->combat())->find(held.duel.enemy);
			if (bare)
			{
				servant.armour.clear();
			} else {
				ksim::state::FighterArmour plate = servant.armour.at(0);
				plate.cut = plate.blunt = 0.4;
				plate.pierce = 0.0;
				plate.coverage.clear();
				for (const ksim::state::FighterPart& part : servant.parts)
					plate.coverage.push_back({part.name, 1.0});
				servant.armour = {plate};
			}
			std::promise<int> holdPort;
			ks::ServerOptions holdOptions;
			holdOptions.onListening = [&holdPort](int listening) { holdPort.set_value(listening); };
			ks::SimLinkServer holdServer(*held.sim, held.context, holdOptions);
			holdServer.listen();
			const int heldPort = holdPort.get_future().get();
			std::atomic<bool> holdStop{false};
			std::thread holdThread([&]() { holdServer.run(holdStop); });
			const struct HoldJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~HoldJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} holdJoiner{holdStop, holdThread};
			Client c(heldPort);
			c.send(helloFrame(held.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "no initial snapshot after HELLO");
			c.send(rawOrder(1, 2, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, held.duel.player); put<std::uint64_t>(v, held.duel.enemy); }));
			const auto landed = [&]() -> const Extras::Hit*
			{
				for (const Extras::Hit& h : c.extras.hits)
				{
					if (h.victim == held.duel.enemy && h.result == 0)
						return &h;
				}
				return nullptr;
			};
			test::require(c.pump([&]() { return landed() != nullptr; }, 90000), "no landed hit on the servant");
			return *landed();
		};
		{
			const Extras::Hit bareHit = strike(true);
			test::require(bareHit.cutAbsorbed == 0.0f && bareHit.bluntAbsorbed == 0.0f, "event 1 tail: an unarmoured victim absorbed damage");
			const Extras::Hit armouredHit = strike(false);
			test::require(armouredHit.cutAbsorbed > 0.0f && armouredHit.bluntAbsorbed > 0.0f, "event 1 tail: an armoured victim absorbed nothing");
			// armour 0.4 on cut and blunt: the absorbed share is 0.4 / 0.6 of what was taken (toughness factor aside, so only the sign and order are checked)
			std::cout << "event 1 tail: bare 0 / 0, armoured " << armouredHit.cutAbsorbed << " / " << armouredHit.bluntAbsorbed << "\n";
		}

		// ---- CHAR_MEDICAL trailing `severed` and event 47 (Dismemberment 2: the sink path severs a limb already past -M) ----
		// The servant's leg is set before the server starts, below -(max - wear), and the sim is held by the admin pause: the first
		// CHAR_MEDICAL shows the bit clear, an unpause lets the first medical update sever it, the next frame shows the bit and event 47 came once.
		{
			World sw = makeWorld(order, data, gd, nav, 2);
			sw.context.paused = true;
			ksim::state::Fighter& servant = *const_cast<ksim::CombatArena*>(sw.sim->combat())->find(sw.duel.enemy);
			test::require(servant.parts.size() <= 16, "a character has more than 16 parts: the severed u16 is too small");
			std::size_t legIndex = servant.parts.size();
			for (std::size_t i = 0; i < servant.parts.size(); ++i)
			{
				if (servant.parts[i].limb() && servant.parts[i].name.find("Foreleg") == std::string::npos)
					legIndex = i;
			}
			test::require(legIndex < servant.parts.size(), "the servant has no limb");
			servant.parts[legIndex].health = -(servant.parts[legIndex].maxHealth - servant.parts[legIndex].wear) - 1.0;
			std::promise<int> sPort;
			ks::ServerOptions sOptions;
			sOptions.onListening = [&sPort](int listening) { sPort.set_value(listening); };
			ks::SimLinkServer sServer(*sw.sim, sw.context, sOptions);
			sServer.listen();
			const int severPort = sPort.get_future().get();
			std::atomic<bool> sStop{false};
			std::thread sThread([&]() { sServer.run(sStop); });
			const struct SeverJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~SeverJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} severJoiner{sStop, sThread};
			Client c(severPort);
			c.send(helloFrame(sw.context.dataHash));
			test::require(c.pump([&]() { return c.extras.medical.count(sw.duel.enemy) != 0; }, 5000), "no CHAR_MEDICAL for the servant");
			bool legSevered = true;
			sServer.call([&]() { legSevered = servant.parts[legIndex].severed(); });
			test::require(c.extras.medical[sw.duel.enemy].severed == 0 && !legSevered, "severed bits set before any sever");
			const std::uint32_t legStrid = c.extras.medical[sw.duel.enemy].partStrids.at(legIndex);
			c.send(rawOrder(3001, 40, [](auto& v) { put<std::uint8_t>(v, 0); }));
			test::require(c.pump([&]() { return !c.extras.limbSevered.empty(); }, 10000), "no event 47 at the sever");
			test::require(c.pump([&]() { const auto it = c.extras.medical.find(sw.duel.enemy); return it != c.extras.medical.end() && it->second.severed != 0; }, 5000),
				"CHAR_MEDICAL never showed the severed bit");
			c.pump([]() { return false; }, 1500); // let a second sever event show up if there were one
			test::require(c.extras.medical[sw.duel.enemy].severed == (1u << legIndex), "the severed bit is not the leg's row");
			test::require(c.extras.limbSevered.size() == 1 && c.extras.limbSevered[0].first == sw.duel.enemy && c.extras.limbSevered[0].second == legStrid,
				"event 47 is not exactly one, for the servant and the same part strid as the CHAR_MEDICAL row");
			const Extras::Medical& cut = c.extras.medical[sw.duel.enemy];
			test::require(cut.bleed > 0.0f && cut.partMax[legIndex] < 6.0f && (cut.limb[legIndex] & 3u) == 1u,
				"the panel tail shows the stump: bleeding, the row's max forced to 5, limb state 1");
			std::cout << "CHAR_MEDICAL severed bit and event 47: bit " << legIndex << " set after the sink sever, one event; tail bleed " << cut.bleed << "\n";
		}

		// ---- CHAR_MEDICAL flags bit 2 hurt and bit 3 putIn (kenshi-k16.6.160, the portrait's red background and dim face) ----
		{
			World pw = makeWorld(order, data, gd, nav);
			ksim::Simulation& psim = *pw.sim;
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(psim.combat());
			ksim::state::Fighter& body = *arena->find(pw.duel.enemy);
			const std::uint64_t id = pw.duel.enemy;
			flecs::world& ecs = const_cast<flecs::world&>(psim.world().ecs());
			flecs::entity entity;
			ecs.each([&](flecs::entity e, const ksim::state::Id& who) { if (who.value == id) entity = e; });
			test::require(entity.is_valid(), "the servant has no entity");
			const auto hurt = [&]() { return psim.portraitHurt(id, *arena->find(id)); };
			const auto putIn = [&]() { return psim.lyingInBed(id, *arena->find(id)); };
			// hurt: a fighter in a fight sets it, it holds 15 s (300 ticks) after the fight ends, then clears.
			test::require(!hurt(), "hurt before any fight");
			body.target = pw.duel.player;
			psim.tick();
			const std::uint64_t seen = psim.clockTick();
			test::require(hurt(), "a fighter with a living target is not hurt");
			body.target = 0;
			body.engaged.clear();
			psim.setClockTick(seen + 299);
			test::require(hurt(), "hurt dropped before the 15 s hold ended");
			psim.setClockTick(seen + 301);
			test::require(!hurt(), "hurt held past 15 s");
			// A carried body keeps it up too; a KO / a cage clears it.
			psim.setClockTick(seen);
			test::require(hurt(), "hurt lost on the clock rewind inside the hold");
			body.unconscious = true;
			test::require(!hurt(), "a KO'd fighter is hurt");
			body.unconscious = false;
			body.cageBuilding = 1;
			test::require(!hurt(), "a caged fighter is hurt");
			body.cageBuilding = 0;
			// putIn: a bed sleeper on its home-bed routine and a put-in-bed body (no goal, a Bed site) set it; a floor sleeper and a cage do not.
			ksim::state::Routine routine;
			routine.goal = ksim::routines::task::GoHomeToBed;
			entity.set<ksim::state::Routine>(routine);
			test::require(!putIn(), "putIn while awake");
			body.asleep = true;
			test::require(putIn(), "a bed sleeper is not putIn");
			body.cageBuilding = 1;
			test::require(!putIn(), "a caged sleeper is putIn");
			body.cageBuilding = 0;
			routine.goal = 0;
			routine.target.kind = static_cast<std::uint8_t>(ksim::routines::SiteKind::Bed);
			entity.set<ksim::state::Routine>(routine);
			test::require(putIn(), "a put-in-bed body (no goal, Bed site) is not putIn");
			routine.target.kind = static_cast<std::uint8_t>(ksim::routines::SiteKind::HomePoint);
			entity.set<ksim::state::Routine>(routine);
			test::require(!putIn(), "a floor sleeper is putIn");
			// The wire: the first CHAR_MEDICAL carries both bits (awake and engaged: bit 2 only; bit 3 is covered through lyingInBed, since an asleep body's CHAR_DYNAMIC needs a pose tail).
			routine.target.kind = static_cast<std::uint8_t>(ksim::routines::SiteKind::Bed);
			entity.set<ksim::state::Routine>(routine);
			body.asleep = false;
			body.target = pw.duel.player;
			psim.tick();
			pw.context.paused = true;
			std::promise<int> hPort;
			ks::ServerOptions hOptions;
			hOptions.onListening = [&hPort](int listening) { hPort.set_value(listening); };
			ks::SimLinkServer hServer(psim, pw.context, hOptions);
			hServer.listen();
			const int hurtPort = hPort.get_future().get();
			std::atomic<bool> hStop{false};
			std::thread hThread([&]() { hServer.run(hStop); });
			const struct HurtJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~HurtJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} hurtJoiner{hStop, hThread};
			Client c(hurtPort);
			c.send(helloFrame(pw.context.dataHash));
			test::require(c.pump([&]() { return c.extras.medical.count(id) != 0; }, 5000), "no CHAR_MEDICAL for the portrait bits");
			test::require(c.extras.medical[id].flags == 4u, "the CHAR_MEDICAL flags are not hurt alone");
			std::cout << "CHAR_MEDICAL hurt / putIn bits: hold 15 s, KO and cage clear, bed vs floor sleeper\n";
		}

		// ---- EVENT 48, the ragdoll start (task BG; PHYSICS.md 2-3), and CHAR_DYNAMIC's rest tail ----
		// The first world: a KO with no blow (cause 0), the get-up (nothing), a death while standing with no blow (cause 1).
		{
			World w = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(w.sim->combat());
			ksim::state::Fighter& servant = *arena->find(w.duel.enemy);
			const ksim::CombatRules& rules = arena->rules();
			ksim::state::FighterPart* chest = nullptr;
			for (ksim::state::FighterPart& part : servant.parts)
				if (part.vital && chest == nullptr)
					chest = &part;
			test::require(chest != nullptr, "the servant has no vital part");
			// A vital part one point below the KO point: the first medical step KOs the body, with no blow behind it.
			chest->stun = chest->health + chest->bonus - rules.koPoint(servant) + 1.0;
			w.context.paused = true;
			std::promise<int> portPromise;
			ks::ServerOptions options;
			options.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer server(*w.sim, w.context, options);
			server.listen();
			const int port = portPromise.get_future().get();
			std::atomic<bool> stop{false};
			std::thread thread([&]() { server.run(stop); });
			const struct Joiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~Joiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} joiner{stop, thread};
			Client c(port);
			c.send(helloFrame(w.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "ragdoll: no initial snapshot");
			const auto countCause = [&](std::uint8_t cause) {
				int n = 0;
				for (const Extras::Ragdoll& r : c.extras.ragdolls)
					n += r.cause == cause && r.id == w.duel.enemy ? 1 : 0;
				return n;
			};
			c.send(rawOrder(4001, 40, [](auto& v) { put<std::uint8_t>(v, 0); })); // unpause
			test::require(c.pump([&]() { return countCause(0) == 1; }, 10000), "no EVENT 48 cause 0 at a KO with no blow");
			{
				const Extras::Ragdoll* ko = nullptr;
				for (const Extras::Ragdoll& r : c.extras.ragdolls)
					if (r.cause == 0 && r.id == w.duel.enemy)
						ko = &r;
				test::require(ko->strength == 0.0f && ko->part == 0 && ko->dir[0] == 0.0f && ko->dir[2] == 0.0f,
					"a collapse carries a push direction, a strength or a part");
				// It follows the KO event (3) of the same tick, and only after it.
				const std::vector<std::uint16_t>& kinds = c.extras.eventKinds;
				std::size_t at = kinds.size();
				while (at > 0 && kinds[at - 1] != 48)
					--at;
				test::require(at >= 2 && kinds[at - 2] == 3, "EVENT 48 does not follow the KO (3) event of its tick");
				const simlink::Event* koEvent = c.find(3, 0);
				test::require(koEvent != nullptr && koEvent->tick == ko->tick, "EVENT 3 and EVENT 48 are not on the same tick");
				test::require(c.extras.proneSeen.count(w.duel.enemy) != 0, "the KO'd body never streamed prone (flag bit 5)");
				test::require(!c.extras.restYaws.empty(), "a prone CHAR_DYNAMIC carried no restYaw tail");
			}
			// The get-up: the chest recovers above the KO point, the flag clears; the client gets no event for it.
			c.send(rawOrder(4002, 40, [](auto& v) { put<std::uint8_t>(v, 1); }));
			test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "ragdoll: the admin pause");
			chest->stun = 0.0;
			servant.koTimer = 0.0;
			const std::size_t ragdollsAtWake = c.extras.ragdolls.size();
			c.send(rawOrder(4003, 40, [](auto& v) { put<std::uint8_t>(v, 0); }));
			test::require(c.pump([&]() { return c.find(4, 0) != nullptr; }, 10000), "no wake (4) after the chest recovered");
			test::require(c.pump([&]() { return c.extras.uprightSeen.count(w.duel.enemy) != 0; }, 5000), "the servant never streamed upright again");
			c.pump([]() { return false; }, 600);
			test::require(c.extras.ragdolls.size() == ragdollsAtWake, "an EVENT 48 was sent for the get-up");
			// A death while standing and with no blow: starvation (the body was up again, so the cause-1 collapse is sent).
			c.send(rawOrder(4004, 40, [](auto& v) { put<std::uint8_t>(v, 1); }));
			test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "ragdoll: the admin pause before the death");
			servant.hunger = -0.1;
			c.send(rawOrder(4005, 40, [](auto& v) { put<std::uint8_t>(v, 0); }));
			test::require(c.pump([&]() { return countCause(1) == 1; }, 10000), "no EVENT 48 cause 1 at a death with no blow");
			for (const Extras::Ragdoll& r : c.extras.ragdolls)
			{
				if (r.cause == 1 && r.id == w.duel.enemy)
					test::require(r.strength == 0.0f && r.part == 0 && r.dir[0] == 0.0f && r.dir[2] == 0.0f,
						"a death collapse carries a push direction, a strength or a part");
			}
			std::cout << "EVENT 48: a KO and a death with no blow, cause 0 and 1, dir 0; no event on the get-up; "
				<< c.extras.restYaws.size() << " prone frames with the rest tail\n";
		}
		// The second world (BG2): a conscious crawler is not a ragdoll - its blows send no EVENT 48 and its KO is cause 0 - then a put-down (cause 2).
		{
			World w = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(w.sim->combat());
			ksim::state::Fighter& servant = *arena->find(w.duel.enemy);
			const ksim::CombatRules& rules = arena->rules();
			// One leg below the KO point: a conscious crawler (down, not a ragdoll, BG2, the BF/ksim_ko_long limb rule). The
			// vitals are far above the duel's damage, except that one vital part sits just above its KO point, so the next blow
			// that lands on it KOs the crawler: the crawler -> KO case (3 + EVENT 48 cause 0).
			ksim::state::FighterPart* leg = nullptr;
			ksim::state::FighterPart* chest = nullptr;
			for (ksim::state::FighterPart& part : servant.parts)
			{
				part.maxHealth = 5000.0;
				part.health = 5000.0;
				part.stun = 0.0;
				part.pool = 0.0;
				if (leg == nullptr && part.limb() && part.name.find("Foreleg") == std::string::npos)
					leg = &part;
				if (chest == nullptr && part.vital)
					chest = &part;
			}
			test::require(leg != nullptr && chest != nullptr, "the servant has no leg or no vital part");
			leg->health = rules.koPoint(servant) - 1.0;
			w.context.paused = true;
			std::promise<int> portPromise;
			ks::ServerOptions options;
			options.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer server(*w.sim, w.context, options);
			server.listen();
			const int port = portPromise.get_future().get();
			std::atomic<bool> stop{false};
			std::thread thread([&]() { server.run(stop); });
			const struct Joiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~Joiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} joiner{stop, thread};
			Client c(port);
			c.send(helloFrame(w.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "ragdoll: no initial snapshot");
			const auto countCause = [&](std::uint8_t cause) {
				int n = 0;
				for (const Extras::Ragdoll& r : c.extras.ragdolls)
					n += r.cause == cause && r.id == w.duel.enemy ? 1 : 0;
				return n;
			};
			c.send(rawOrder(5000, 40, [](auto& v) { put<std::uint8_t>(v, 0); })); // unpause
			test::require(c.pump([&]() { return c.extras.proneSeen.count(w.duel.enemy) != 0; }, 10000),
				"the collapsed leg did not put the servant down");
			// The collapse fires the forced KO writer (task BL, kinspect down_ko_trigger.md / fb0248d): 3.0 medical units,
			// then awake with down still set. It is a ragdoll start: one EVENT 48 cause 0 after its event 3, like any KO.
			test::require(c.pump([&]() { return countCause(0) == 1; }, 10000),
				"a limb collapse did not send its one cause-0 EVENT 48 (task BL)");
			{
				const Extras::Ragdoll* collapse = nullptr;
				for (const Extras::Ragdoll& r : c.extras.ragdolls)
					if (r.cause == 0 && r.id == w.duel.enemy)
						collapse = &r;
				test::require(collapse->strength == 0.0f && collapse->part == 0 && collapse->dir[0] == 0.0f
						&& collapse->dir[2] == 0.0f, "a collapse carries a push direction, a strength or a part");
				const std::vector<std::uint16_t>& kinds = c.extras.eventKinds;
				std::size_t at = kinds.size();
				while (at > 0 && kinds[at - 1] != 48)
					--at;
				test::require(at >= 2 && kinds[at - 2] == 3, "the collapse's EVENT 48 does not follow its event 3");
			}
			const std::size_t ragdollsAtCollapse = c.extras.ragdolls.size();
			// A down body is nobody's target (task BK; kinspect 84a2ab0, and its "a crawler under a player attack order" question
			// answered no): once the collapse's 3.0-unit KO has run, a player attack order on the conscious crawler completes at once
			// - no target, no blow, and so no EVENT 48. (Checked a few ticks after the order: `pickTarget` also drops an order while
			// its target is unconscious, so the completion is what the order has to reach.)
			test::require(c.pump([&]() { bool awake = false; server.call([&]() { awake = !servant.unconscious; }); return awake; }, 15000),
				"the collapsed servant never woke from its 3.0-unit KO");
			const std::uint64_t orderTick = c.lastSnapshotEnd;
			c.send(rawOrder(5001, 2, [&](auto& v) {
				put<std::uint16_t>(v, 1);
				put<std::uint64_t>(v, w.duel.player);
				put<std::uint64_t>(v, w.duel.enemy);
			}));
			test::require(c.pump([&]() { return c.lastSnapshotEnd > orderTick + 40; }, 10000),
				"ragdoll: the sim did not tick past the attack order");
			std::uint64_t playerTarget = 1;
			server.call([&]() { playerTarget = w.sim->combat()->find(w.duel.player)->target; });
			test::require(playerTarget == 0, "the attack order on a crawler did not complete");
			for (const Extras::Hit& hit : c.extras.hits)
				test::require(hit.victim != w.duel.enemy, "a blow landed on a down body");
			test::require(c.extras.ragdolls.size() == ragdollsAtCollapse, "the completed order sent an EVENT 48");
			// The crawler goes unconscious (a vital part below its KO point while the sim is paused): that is a second
			// cause-0 ragdoll start after its event 3 (the first was the collapse's).
			c.send(rawOrder(5002, 40, [](auto& v) { put<std::uint8_t>(v, 1); }));
			test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "ragdoll: the admin pause before the KO");
			server.call([&]() { chest->stun = chest->health + chest->bonus - rules.koPoint(servant) + 1.0; });
			c.send(rawOrder(5003, 40, [](auto& v) { put<std::uint8_t>(v, 0); }));
			test::require(c.pump([&]() { return countCause(0) == 2; }, 10000), "the crawler was not KO'd");
			test::require(countCause(3) == 0, "a conscious crawler's blows were reported as launches (cause 3)");
			{
				const Extras::Ragdoll* ko = nullptr;
				for (const Extras::Ragdoll& r : c.extras.ragdolls)
					if (r.cause == 0 && r.id == w.duel.enemy)
						ko = &r;
				test::require(ko != nullptr, "no cause-0 ragdoll recorded");
				test::require(ko->strength == 0.0f && ko->part == 0, "a no-blow KO carries a strength or a part");
				const std::vector<std::uint16_t>& kinds = c.extras.eventKinds;
				std::size_t at = kinds.size();
				while (at > 0 && kinds[at - 1] != 48)
					--at;
				test::require(at >= 2 && kinds[at - 2] == 3, "the crawler's KO EVENT 48 does not follow its event 3");
			}
			// A put-down needs an unconscious body: carry the KO'd crawler and put it down (cause 2).
			const auto interactDone = [&](std::uint8_t action) {
				for (const simlink::Event& e : c.events)
					if (e.kind == 13 && e.a == w.duel.player && e.dir == action && e.result == 0)
						return true;
				return false;
			};
			c.send(rawOrder(5004, 60, [&](auto& v) {
				put<std::uint16_t>(v, 1);
				put<std::uint64_t>(v, w.duel.player);
				put<std::uint64_t>(v, w.duel.enemy);
				put<std::uint8_t>(v, 8); // carry
				put<std::uint8_t>(v, 0);
			}));
			test::require(c.pump([&]() { return interactDone(8); }, 30000), "the lift never finished");
			c.send(rawOrder(5005, 60, [&](auto& v) {
				put<std::uint16_t>(v, 1);
				put<std::uint64_t>(v, w.duel.player);
				put<std::uint64_t>(v, w.duel.enemy);
				put<std::uint8_t>(v, 13); // put down
				put<std::uint8_t>(v, 0);
			}));
			test::require(c.pump([&]() { return countCause(2) == 1; }, 30000), "no EVENT 48 cause 2 at a put-down");
			for (const Extras::Ragdoll& r : c.extras.ragdolls)
			{
				if (r.cause == 2 && r.id == w.duel.enemy)
					test::require(r.strength == 0.0f && r.part == 0 && r.dir[0] == 0.0f && r.dir[2] == 0.0f,
						"a put-down carries a push direction, a strength or a part");
			}
			std::cout << "EVENT 48: a crawler is no ragdoll (blows send none), its KO is cause 0; a put-down is cause 2\n";
		}

		// ---- the dropped limb item and the boots rule (the sim alone: Dismemberment 2 severs on the first tick) ----
		// One leg past -M: its `severed limbs` item lies on the ground, the boots stay worn. Both legs: the boots drop too.
		{
			std::string bootsSid;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ARMOUR")))
			{
				if (r->intOr("slot", 0) == 9)
				{
					bootsSid = r->stringID;
					break;
				}
			}
			test::require(!bootsSid.empty(), "no boots ARMOUR (slot 9) in the data");
			for (const bool bothLegs : {false, true})
			{
				World sw = makeWorld(order, data, gd, nav, 2);
				ksim::state::Fighter& servant = *const_cast<ksim::CombatArena*>(sw.sim->combat())->find(sw.duel.enemy);
				flecs::world& ecs = const_cast<flecs::world&>(sw.sim->world().ecs());
				const auto limbItem = [&](int slot) -> std::string
				{
					if (const kdata::Record* race = gd->find(servant.raceSid))
					{
						if (const kdata::RefCategory* limbs = race->findRefs("severed limbs"))
						{
							for (const kdata::RefEntry& e : limbs->entries)
							{
								if (e.v0 == slot)
									return e.targetID;
							}
						}
					}
					return {};
				};
				const std::string leftLeg = limbItem(2);
				test::require(!leftLeg.empty(), "the servant's race has no `severed limbs` left leg item: " + servant.raceSid);
				ksim::state::InventoryEntry boots;
				boots.itemSid = bootsSid;
				boots.count = 1;
				boots.slot = ksim::state::Inventory::kArmourBase + 9;
				boots.section = ksim::state::InventoryEntry::kEquipped;
				ecs.each([&](flecs::entity, const ksim::state::Id& id, ksim::state::Inventory& inv)
				{
					if (id.value == sw.duel.enemy)
						inv.items.push_back(boots);
				});
				std::size_t severedParts = 0;
				for (ksim::state::FighterPart& p : servant.parts)
				{
					const bool left = p.name == "Left Leg";
					const bool right = p.name == "Right Leg";
					if (left || (bothLegs && right))
					{
						p.health = -(p.maxHealth - p.wear) - 1.0;
						++severedParts;
					}
				}
				test::require(severedParts == (bothLegs ? 2u : 1u), "the servant lacks Left Leg / Right Leg parts");
				sw.sim->run(3);
				std::size_t limbsOnGround = 0, bootsOnGround = 0, bootsWorn = 0;
				ecs.each([&](flecs::entity, const ksim::state::GroundItem& g)
				{
					limbsOnGround += g.itemSid == leftLeg ? 1 : 0;
					bootsOnGround += g.itemSid == bootsSid ? 1 : 0;
				});
				ecs.each([&](flecs::entity, const ksim::state::Id& id, const ksim::state::Inventory& inv)
				{
					if (id.value != sw.duel.enemy)
						return;
					for (const ksim::state::InventoryEntry& i : inv.items)
						bootsWorn += i.itemSid == bootsSid && i.slot == ksim::state::Inventory::kArmourBase + 9 ? 1 : 0;
				});
				test::require(limbsOnGround == 1, "the severed left leg item is not on the ground exactly once");
				test::require(bothLegs ? (bootsOnGround == 1 && bootsWorn == 0) : (bootsOnGround == 0 && bootsWorn == 1),
					bothLegs ? "both legs gone: the boots did not drop" : "one leg gone: the boots must stay worn");
				std::cout << (bothLegs ? "both legs severed" : "one leg severed") << ": " << leftLeg << " on the ground, boots "
					<< (bothLegs ? "dropped" : "worn") << "\n";
			}
		}

		// ---- the CHAR_DYNAMIC aim tail (bit 20; client-approved 2026-11-07) ----
		// A fighter swinging a technique that has a clip streams the trailing f32 aimYaw, equal to its live Fighter::aimYaw; after
		// the clip the bit clears and the record is the old length again (no tail bytes: the old client decoder below reads every
		// frame whole once the known tails are cut, and rejects trailing bytes). Seated and prone records never carry the bit.
		{
			World aw = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(aw.sim->combat());
			aw.context.paused = true;
			std::promise<int> portPromise;
			ks::ServerOptions options;
			options.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer server(*aw.sim, aw.context, options);
			server.listen();
			const int port = portPromise.get_future().get();
			std::atomic<bool> stop{false};
			std::thread thread([&]() { server.run(stop); });
			const struct AimJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~AimJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} joiner{stop, thread};
			Client c(port);
			c.send(helloFrame(aw.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "aim tail: no initial snapshot");
			c.send(rawOrder(6000, 40, [](auto& v) { put<std::uint8_t>(v, 0); })); // unpause
			c.send(rawOrder(6001, 2, [&](auto& v) {
				put<std::uint16_t>(v, 1);
				put<std::uint64_t>(v, aw.duel.player);
				put<std::uint64_t>(v, aw.duel.enemy);
			}));
			// A frame whose aim equals the fighter's live aim while its technique still runs: the swing that frame is of.
			const auto swingFrame = [&]() -> std::uint64_t
			{
				std::uint64_t swinging = 0;
				server.call([&]()
				{
					for (const auto& [id, aim] : c.extras.aimYaws)
					{
						const ksim::state::Fighter* f = arena->find(id);
						if (f != nullptr && f->technique >= 0 && !f->techniqueFinished && std::fabs(f->aimYaw - aim) < 1.0e-4)
						{
							swinging = id;
							return;
						}
					}
				});
				return swinging;
			};
			test::require(c.pump([&]() { return swingFrame() != 0; }, 30000),
				"no CHAR_DYNAMIC carried the aim tail (bit 20) for a live swing");
			const std::uint64_t swinger = swingFrame();
			test::require(c.extras.aimSeen.count(swinger) != 0, "the swing's aim tail is missing");
			// The swing ends: the next frames of the same fighter carry no bit 20 and so no tail (the old decoder reads them whole).
			test::require(c.pump([&]() { return c.extras.aimCleared.count(swinger) != 0; }, 30000),
				"the aim tail (bit 20) never cleared after the swing");
			std::cout << "aim tail: a swing's frame carries aimYaw " << c.extras.aimYaws.at(swinger)
				<< " matching the live fighter, cleared after the clip\n";
		}

		// ---- order 60 action 14, the stealth KO (client-approved 2026-11-10) ----
		// Refusals are answered at once (bad_target, not_allowed, bad_value for a queue flag and for any action above 14); an accepted
		// order is applied at the next tick and the assassin starts the walk (Fighter::stealthKo).
		{
			World sw = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(sw.sim->combat());
			sw.context.paused = true;
			std::promise<int> portPromise;
			ks::ServerOptions options;
			options.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer server(*sw.sim, sw.context, options);
			server.listen();
			const int port = portPromise.get_future().get();
			std::atomic<bool> stop{false};
			std::thread thread([&]() { server.run(stop); });
			const struct StealthJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~StealthJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} joiner{stop, thread};
			Client c(port);
			c.send(helloFrame(sw.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "stealth KO: no initial snapshot");
			const auto stealthOrder = [&](std::uint32_t seq, std::uint64_t target, std::uint8_t action, std::uint8_t queue)
			{
				c.send(rawOrder(seq, 60, [&](auto& v) {
					put<std::uint16_t>(v, 1);
					put<std::uint64_t>(v, sw.duel.player);
					put<std::uint64_t>(v, target);
					put<std::uint8_t>(v, action);
					put<std::uint8_t>(v, queue);
				}));
			};
			const auto refusal = [&](std::uint32_t seq, std::uint64_t target, std::uint8_t action, std::uint8_t queue, const char* reason)
			{
				stealthOrder(seq, target, action, queue);
				test::require(c.pump([&]() { return c.find(10, seq) != nullptr; }, 5000), "stealth KO: no answer to order " + std::to_string(seq));
				test::require(reasonOf(c, *c.find(10, seq)) == reason, std::string("stealth KO order ") + std::to_string(seq) + " is not " + reason);
			};
			refusal(7001, 99999999, 14, 0, "bad_target");
			refusal(7002, sw.duel.player, 14, 0, "bad_target");
			refusal(7003, sw.duel.enemy, 21, 0, "bad_value");
			refusal(7004, sw.duel.enemy, 22, 0, "bad_value");
			refusal(7005, sw.duel.enemy, 14, 1, "bad_value");
			server.call([&]() { arena->find(sw.duel.enemy)->unconscious = true; });
			refusal(7006, sw.duel.enemy, 14, 0, "not_allowed");
			bool startedRefused = true;
			server.call([&]() { arena->find(sw.duel.enemy)->unconscious = false; startedRefused = arena->find(sw.duel.player)->stealthKo.active(); });
			test::require(!startedRefused, "a refused stealth KO started");
			// The victim already knows the assassin: the task aborts at its first tick, event 50 result 2, chance 0 (once, no roll).
			server.call([&]() { ksim::perception::notice(*arena->find(sw.duel.enemy), sw.duel.player); });
			stealthOrder(7007, sw.duel.enemy, 14, 0);
			c.send(rawOrder(7008, 40, [](auto& v) { put<std::uint8_t>(v, 0); })); // unpause
			test::require(c.pump([&]() { return c.find(9, 7007) != nullptr; }, 30000), "stealth KO: the accepted order was never applied");
			test::require(c.find(10, 7007) == nullptr, "stealth KO: the accepted order was also refused");
			test::require(c.pump([&]() {
				bool started = false;
				server.call([&]() { started = arena->find(sw.duel.player)->stealthKo.active() || arena->find(sw.duel.enemy)->unconscious; });
				return started;
			}, 30000),
				"the accepted stealth KO never started");
			test::require(c.pump([&]() { return !c.extras.stealthKos.empty(); }, 30000), "stealth KO: no event 50 for the aborted attempt");
			test::require(c.extras.stealthKos.size() == 1 && c.extras.stealthKos[0].assassin == sw.duel.player
					&& c.extras.stealthKos[0].victim == sw.duel.enemy && c.extras.stealthKos[0].result == 2 && c.extras.stealthKos[0].chance == 0.0f,
				"stealth KO: event 50 is not one abort (result 2, chance 0) of the player on the enemy");
			std::cout << "stealth KO order: bad_target x2, bad_value x3, not_allowed, accepted\n";
		}

		// ---- SEEN_BY (type 59, client-approved 2026-11-11, kenshi-h2v.1.3) ----
		// A sneaking player character is listed by the hostile characters that know it, which is the test the stealth KO's approach aborts
		// on: unaware -> nothing and the KO's walk goes on; aware -> listed and the KO aborts; a team change drops and re-adds it; the
		// sneak ending sends n = 0 once. Driven on a Session directly (no clock), so every record is seen in the tick that made it.
		{
			World bw = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(bw.sim->combat());
			ksim::state::Fighter& player = *arena->find(bw.duel.player);
			ksim::state::Fighter& enemy = *arena->find(bw.duel.enemy);
			test::require(player.playerControlled && player.team != enemy.team, "SEEN_BY: the duel is a player and a hostile");
			const std::int32_t enemyTeam = enemy.team;
			enemy.faction = 9999; // not the player's faction (the duel's are equal, which sensor::ally counts as allied)
			ks::Session session(*bw.sim, bw.context);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session.begin(w);
			const std::vector<std::uint8_t> hello = helloFrame(bw.context.dataHash);
			test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "SEEN_BY: hello refused");
			session.sendTick(w, true);
			struct Seen59
			{
				std::uint64_t id;
				std::vector<std::uint64_t> by;
			};
			const auto step = [&]()
			{
				bytes.clear();
				ks::BinaryWriter out(bytes);
				session.sendTick(out, false);
				std::vector<Seen59> got;
				for (std::size_t at = 0; at < bytes.size();)
				{
					std::uint32_t len = 0;
					std::uint16_t type = 0;
					std::memcpy(&len, &bytes[at], 4);
					std::memcpy(&type, &bytes[at + 4], 2);
					if (type == ks::kSeenBy)
					{
						Seen59 r;
						std::uint16_t n = 0;
						std::memcpy(&r.id, &bytes[at + 6 + 8], 8);
						std::memcpy(&n, &bytes[at + 6 + 16], 2);
						test::require(len == 2 + 8 + 8 + 2 + 8u * n, "SEEN_BY length");
						for (std::uint16_t i = 0; i < n; ++i)
						{
							std::uint64_t by = 0;
							std::memcpy(&by, &bytes[at + 6 + 18 + 8 * i], 8);
							r.by.push_back(by);
						}
						got.push_back(std::move(r));
					}
					at += 4 + len;
				}
				return got;
			};
			// The KO's walk starts and is still on after a tick, or aborts at once.
			const auto koAborts = [&]()
			{
				test::require(bw.sim->orderStealthKo(bw.duel.player, bw.duel.enemy).empty(), "SEEN_BY: the stealth KO order was refused");
				bw.sim->run(1);
				const bool aborted = !player.stealthKo.active();
				arena->cancelStealthKo(bw.duel.player);
				player.sneak = true; // the ticks may have ended it (the order, a fight): the next check wants the sneak
				return aborted;
			};
			const std::vector<std::uint64_t> none;
			player.sneak = true;
			enemy.sight.clear();
			test::require(step().empty(), "SEEN_BY: a record for an unaware observer");
			test::require(!koAborts(), "SEEN_BY: the KO aborted on an unaware victim");
			enemy.sight.clear();
			test::require(step().empty(), "SEEN_BY: a record for an unaware observer (second try)");
			ksim::perception::notice(enemy, bw.duel.player);
			auto got = step();
			test::require(got.size() == 1 && got[0].id == bw.duel.player && got[0].by == std::vector<std::uint64_t>{bw.duel.enemy}, "SEEN_BY: the aware hostile is not listed");
			test::require(step().empty(), "SEEN_BY: resent with no change");
			test::require(koAborts(), "SEEN_BY: the KO did not abort on a listed observer");
			ksim::perception::notice(enemy, bw.duel.player);
			// A neutral observer (the player's team, no ally relation) is still listed (kenshi-k16.6.331); an allied one is not.
			enemy.team = player.team;
			test::require(!ksim::sensor::ally(bw.sim->sensorContext(), enemy, player), "SEEN_BY: the neutral observer is an ally");
			test::require(step().empty(), "SEEN_BY: a neutral observer left the set");
			test::require(koAborts(), "SEEN_BY: the KO did not abort on a neutral observer");
			ksim::perception::notice(enemy, bw.duel.player);
			enemy.faction = player.faction; // an ally while it knows the sneaker: it leaves the set
			got = step();
			test::require(got.size() == 1 && got[0].by == none, "SEEN_BY: an allied observer stayed in the set");
			enemy.faction = 9999; // non-allied again while aware: it enters the set on that change
			enemy.team = enemyTeam;
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<std::uint64_t>{bw.duel.enemy}, "SEEN_BY: a non-allied observer did not enter the set");
			player.sneak = false;
			got = step();
			test::require(got.size() == 1 && got[0].id == bw.duel.player && got[0].by == none, "SEEN_BY: no n = 0 when the sneak ended");
			test::require(step().empty(), "SEEN_BY: a record after the clear");
			player.sneak = true;
			got = step(); // the sneak starts with the observer already aware
			test::require(got.size() == 1 && got[0].by == std::vector<std::uint64_t>{bw.duel.enemy}, "SEEN_BY: a sneak starting aware is not listed");
			std::cout << "SEEN_BY: unaware none, aware listed (KO aborts), hostile flip, sneak end n = 0, sneak start\n";
		}

		// ---- DETECTED_BY (type 60, client-approved 2026-11-12, kenshi-h2v.27.1) ----
		// The stealth arrows' records: a sneaking player character's detection map as (observer, state, friendly). A neutral townsperson
		// walking-past scan gives a yellow (2) entry while the meter fills and a red (1) one once it crosses 1; a caged observer never
		// holds one; the sneak ending sends n = 0 once; a friendly one only through a force-notice; a connect snapshot carries the set.
		{
			World dw = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(dw.sim->combat());
			ksim::state::Fighter& player = *arena->find(dw.duel.player);
			ksim::state::Fighter& enemy = *arena->find(dw.duel.enemy);
			enemy.faction = 9999; // a neutral townsperson: another faction, no relation
			enemy.yaw = std::atan2(player.x - enemy.x, player.z - enemy.z); // facing the sneaker
			ks::Session session(*dw.sim, dw.context);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session.begin(w);
			const std::vector<std::uint8_t> hello = helloFrame(dw.context.dataHash);
			test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "DETECTED_BY: hello refused");
			session.sendTick(w, true);
			struct Det60
			{
				std::uint64_t sneaker = 0;
				std::vector<std::array<std::uint64_t, 3>> by; // observer, state, friendly
			};
			const auto decode = [](const std::vector<std::uint8_t>& b)
			{
				std::vector<Det60> got;
				for (std::size_t at = 0; at < b.size();)
				{
					std::uint32_t len = 0;
					std::uint16_t type = 0;
					std::memcpy(&len, &b[at], 4);
					std::memcpy(&type, &b[at + 4], 2);
					if (type == ks::kDetectedBy)
					{
						Det60 r;
						std::uint16_t n = 0;
						std::memcpy(&r.sneaker, &b[at + 6 + 8], 8);
						std::memcpy(&n, &b[at + 6 + 16], 2);
						test::require(len == 2 + 8 + 8 + 2 + 10u * n, "DETECTED_BY length");
						for (std::uint16_t i = 0; i < n; ++i)
						{
							std::uint64_t observer = 0;
							std::memcpy(&observer, &b[at + 6 + 18 + 10 * i], 8);
							r.by.push_back({observer, b[at + 6 + 18 + 10 * i + 8], b[at + 6 + 18 + 10 * i + 9]});
						}
						got.push_back(std::move(r));
					}
					at += 4 + len;
				}
				return got;
			};
			const auto step = [&]()
			{
				bytes.clear();
				ks::BinaryWriter out(bytes);
				session.sendTick(out, false);
				return decode(bytes);
			};
			using Row = std::array<std::uint64_t, 3>;
			const std::uint64_t foe = dw.duel.enemy;
			const auto scanTo = [&](std::uint32_t state)
			{
				const auto record = [&]() { return ksim::perception::find(enemy, dw.duel.player); };
				for (int i = 0; i < 80 && (record() == nullptr || record()->state != state); ++i)
				{
					std::vector<ksim::perception::Noticed> noticed;
					const std::vector<ksim::perception::Other> others{{dw.duel.player, &player}};
					ksim::perception::scan(foe, enemy, 1.0, 0.5, others, ksim::perception::Environment{}, noticed);
				}
				test::require(record() != nullptr && record()->state == state, "DETECTED_BY: the scan never reached the state");
			};
			player.sneak = true;
			enemy.sight.clear();
			test::require(step().empty(), "DETECTED_BY: an entry with no record");
			scanTo(2);
			auto got = step();
			test::require(got.size() == 1 && got[0].sneaker == dw.duel.player && got[0].by == std::vector<Row>{{foe, 2, 0}}, "DETECTED_BY: no yellow entry for the passer-by");
			test::require(step().empty(), "DETECTED_BY: resent with no change");
			// A force-notice turns the yellow entry red at once (140858f40 writes state 1; kenshi-h2v.27.10), then back to yellow for the scan below.
			ksim::perception::notice(enemy, dw.duel.player);
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<Row>{{foe, 1, 0}}, "DETECTED_BY: a force-notice did not turn the yellow entry red");
			enemy.sight.clear();
			scanTo(2);
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<Row>{{foe, 2, 0}}, "DETECTED_BY: the yellow entry did not return");
			scanTo(1);
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<Row>{{foe, 1, 0}}, "DETECTED_BY: the yellow entry did not turn red");
			enemy.cageBuilding = 77; // a caged observer holds no record
			got = step();
			test::require(got.size() == 1 && got[0].by.empty(), "DETECTED_BY: a caged observer kept its entry");
			test::require(step().empty(), "DETECTED_BY: a record while caged and empty");
			enemy.cageBuilding = 0;
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<Row>{{foe, 1, 0}}, "DETECTED_BY: the entry did not return");
			// A reconnecting client's snapshot carries the current set.
			{
				ks::Session again(*dw.sim, dw.context);
				std::vector<std::uint8_t> snap;
				ks::BinaryWriter sw(snap);
				again.begin(sw);
				test::require(again.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, sw), "DETECTED_BY: second hello refused");
				snap.clear();
				ks::BinaryWriter sw2(snap);
				again.sendTick(sw2, true);
				const auto joined = decode(snap);
				test::require(joined.size() == 1 && joined[0].sneaker == dw.duel.player && joined[0].by == std::vector<Row>{{foe, 1, 0}},
					"DETECTED_BY: the snapshot lacks the current set");
			}
			// A friendly observer appears only through a force-notice.
			enemy.faction = player.faction;
			enemy.sight.clear();
			scanTo(2);
			got = step();
			test::require(got.size() == 1 && got[0].by.empty(), "DETECTED_BY: a friendly observer was listed from sight");
			ksim::perception::notice(enemy, dw.duel.player);
			got = step();
			test::require(got.size() == 1 && got[0].by == std::vector<Row>{{foe, 1, 1}}, "DETECTED_BY: a force-noticed friendly is not listed with friendly = 1");
			player.sneak = false;
			got = step();
			test::require(got.size() == 1 && got[0].sneaker == dw.duel.player && got[0].by.empty(), "DETECTED_BY: no n = 0 when the sneak ended");
			test::require(step().empty(), "DETECTED_BY: a record after the clear");
			std::cout << "DETECTED_BY: yellow then red, caged none, snapshot, friendly via force-notice, sneak end n = 0\n";
		}

		// ---- event 49 'shot' and CHAR_DYNAMIC bit 21 'mounted' (client-approved 2026-11-10) ----
		// A mounted gunner: bit 21 and the turret key as the last 8 bytes of its record, cleared after the dismount. Every rangedFire
		// that fires a bolt sends one event 49 (hit 0, missed 2; 1 blocked is reserved, a held shot sends nothing) with the gun's sid, the turret key (0 = hand crossbow).
		{
			World rw = makeWorld(order, data, gd, nav);
			ksim::CombatArena* arena = const_cast<ksim::CombatArena*>(rw.sim->combat());
			rw.context.paused = true;
			const kdata::Record* crossbow = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CROSSBOW")))
				if (r->name == "Eagle's Cross")
					crossbow = r;
			test::require(crossbow != nullptr, "ranged wire: no Eagle's Cross in the data");
			ksim::state::Fighter* shooter = arena->find(rw.duel.player);
			ksim::state::Fighter* victim = arena->find(rw.duel.enemy);
			victim->x = shooter->x;
			victim->y = shooter->y;
			victim->z = shooter->z + 200.0;
			shooter->ranged = data->makeRanged(*crossbow, 0.5);
			constexpr std::uint64_t kTurretKey = 0x1234567890abcdefULL;
			shooter->mount.building = kTurretKey;
			shooter->mount.x = shooter->x;
			shooter->mount.z = shooter->z;
			shooter->mount.yaw = 0.0; // the arc's middle: toward +z, where the victim stands
			shooter->mount.gun = shooter->ranged;
			std::promise<int> portPromise;
			ks::ServerOptions options;
			options.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer server(*rw.sim, rw.context, options);
			server.listen();
			const int port = portPromise.get_future().get();
			std::atomic<bool> stop{false};
			std::thread thread([&]() { server.run(stop); });
			const struct RangedJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~RangedJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} joiner{stop, thread};
			Client c(port);
			c.send(helloFrame(rw.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "ranged wire: no initial snapshot");
			const auto setPaused = [&](std::uint32_t seq, bool paused)
			{
				c.send(rawOrder(seq, 40, [&](auto& v) { put<std::uint8_t>(v, paused ? 1 : 0); }));
				test::require(c.pump([&]() { return c.find(9, seq) != nullptr && c.dec.clock.paused == paused; }, 5000), "ranged wire: the pause order never applied");
			};
			setPaused(8001, false);
			test::require(c.pump([&]() { return c.extras.turrets.count(rw.duel.player) != 0; }, 30000), "no CHAR_DYNAMIC carried the mounted tail (bit 21)");
			test::require(c.extras.turrets.at(rw.duel.player) == kTurretKey, "the mounted tail is not the turret key");
			test::require(c.extras.turrets.count(rw.duel.enemy) == 0, "an unmounted fighter carried a mounted tail");
			const std::size_t mountedLength = c.extras.mountedLength.at(rw.duel.player);
			setPaused(8002, true);
			// The shots, with the sim stopped: a turret miss (the deviation is huge at skill 0), a turret hit, a turret shot a wall holds (no event),
			// then the hand crossbow after the dismount.
			struct Expect
			{
				std::uint8_t result;
				std::uint64_t turret;
			};
			std::vector<Expect> expect;
			const auto fire = [&](ksim::CombatArena::Shot wanted, std::uint8_t result, int tries)
			{
				for (int i = 0; i < tries; ++i)
				{
					shooter->gun().timer = 0.0;
					shooter->gun().ammo = 50;
					const ksim::CombatArena::Shot got = arena->rangedFire(rw.duel.player, rw.duel.enemy, 0.0);
					// A bolt that leaves the gun sends an event 49; a Blocked return is a held shot and sends none.
					if (got == ksim::CombatArena::Shot::Hit || got == ksim::CombatArena::Shot::Missed)
						expect.push_back({got == ksim::CombatArena::Shot::Hit ? std::uint8_t{0} : std::uint8_t{2}, shooter->mount.building});
					if (got == wanted)
					{
						test::require(result == 1 || expect.back().result == result, "ranged wire: the shot's result code"); // 1: held, no event
						return;
					}
				}
				test::require(false, "ranged wire: never got the wanted shot");
			};
			// The sim is paused but the server thread owns it: the shots are fired on that thread, and what the checks below read of the
			// shooter and the victim is copied out there (the sim runs again after setPaused(8003)).
			std::string gunSid;
			double shooterX = 0.0, shooterY = 0.0, shooterZ = 0.0, victimX = 0.0, victimZ = 0.0;
			server.call([&]()
			{
				shooter->mount.gun.deviation = 89.0;
				shooter->skills.turrets = 0.0;
				fire(ksim::CombatArena::Shot::Missed, 2, 200);
				shooter->mount.gun.deviation = 0.0;
				shooter->skills.turrets = 100.0;
				fire(ksim::CombatArena::Shot::Hit, 0, 1);
				arena->environmentMutable().shotBlocker = [](const std::array<double, 3>&, const ksim::state::Fighter&) { return 0.5; };
				fire(ksim::CombatArena::Shot::Blocked, 1, 1);
				arena->environmentMutable().shotBlocker = nullptr;
				shooter->mount = {}; // the dismount: the record loses bit 21 and its tail
				shooter->skills.crossbows = 100.0;
				shooter->ranged.deviation = 0.0;
				fire(ksim::CombatArena::Shot::Hit, 0, 1);
				gunSid = shooter->ranged.sid;
				shooterX = shooter->x;
				shooterY = shooter->y;
				shooterZ = shooter->z;
				victimX = victim->x;
				victimZ = victim->z;
			});
			setPaused(8003, false);
			test::require(c.pump([&]() { return c.extras.shots.size() >= expect.size(); }, 30000), "ranged wire: not every shot sent an event 49");
			test::require(c.extras.shots.size() == expect.size(), "ranged wire: more event 49 than shots fired");
			bool seen[3] = {};
			for (std::size_t i = 0; i < expect.size(); ++i)
			{
				const Extras::Shot& s = c.extras.shots[i];
				test::require(s.shooter == rw.duel.player && s.target == rw.duel.enemy, "event 49: shooter / target");
				test::require(s.result == expect[i].result && s.turret == expect[i].turret, "event 49: result / turret, shot " + std::to_string(i));
				test::require(c.dec.str(s.weapon) == gunSid, "event 49: the weapon is the gun's sid");
				test::require(std::fabs(s.from[0] - static_cast<float>(shooterX)) < 1e-3f && std::fabs(s.from[2] - static_cast<float>(shooterZ)) < 1e-3f
					&& s.from[1] > static_cast<float>(shooterY), "event 49: `from` is the muzzle");
				// the victim settles on the ground a few cm after the shot (which resident stands there moves with the Hub's population, kenshi-k16.6.33)
				test::require(std::fabs(s.to[0] - static_cast<float>(victimX)) < 0.5f && std::fabs(s.to[2] - static_cast<float>(victimZ)) < 0.5f,
					"event 49: `to` is the target");
				seen[s.result] = true;
			}
			test::require(seen[0] && !seen[1] && seen[2], "event 49: hit and missed are sent, a held (blocked) shot is not");
			test::require(c.extras.shots.front().turret == kTurretKey && c.extras.shots.back().turret == 0, "event 49: turret key, then 0 for the hand crossbow");
			test::require(c.pump([&]() { return c.extras.mountedCleared.count(rw.duel.player) != 0; }, 30000), "the mounted bit (21) never cleared");
			std::cout << "shot events: " << expect.size() << " event 49 (hit and missed, none for the held shot; turret " << kTurretKey << " then hand), mounted tail on a "
				<< mountedLength << "-byte record, cleared after the dismount\n";
		}

		World live = makeWorld(order, data, gd, nav);
		const_cast<ksim::CombatArena*>(live.sim->combat())->find(live.duel.enemy)->faction = 9999; // the duel's factions are equal, which sensor::ally counts as allied: the servant stays a hostile observer
		live.context.paused = true;
		live.context.debugView = true; // the debug view is on; the loopback client is its peer (--debug-view on the server)
		std::promise<int> portPromise;
		ks::ServerOptions serverOptions;
		serverOptions.onListening = [&portPromise](int port) { portPromise.set_value(port); };
		serverOptions.helloTimeoutMs = 1500; // short, so the timeout checks below are quick
		serverOptions.frameTimeoutMs = 1500;
		serverOptions.pingTimeoutMs = 60000;
		ks::SimLinkServer server(*live.sim, live.context, serverOptions);
		server.listen();
		const int port = portPromise.get_future().get();
		std::atomic<bool> stop{false};
		std::exception_ptr serverError;
		std::thread thread([&]()
		{
			try
			{
				server.run(stop);
			} catch (...) {
				serverError = std::current_exception();
			}
		});
		// A failed require unwinds through here: stop and join, so the failure message survives.
		const struct Joiner
		{
			std::atomic<bool>& stop;
			std::thread& thread;
			~Joiner()
			{
				stop.store(true);
				if (thread.joinable())
					thread.join();
			}
		} joiner{stop, thread};

		// Refusals: wrong data hash, wrong protocol, garbage first frame. Each gets HELLO then ERROR
		// (or ERROR alone) and a closed connection.
		{
			Client c(port);
			c.send(helloFrame(live.context.dataHash ^ 1));
			c.pump([&]() { return c.closed || !c.dec.error().empty(); }, 5000);
			test::require(c.dec.helloSeen(), "server HELLO comes first");
			test::require(c.dec.error().find("mismatch") != std::string::npos, "data hash mismatch refused: " + c.dec.error());
			test::require(c.dec.serverDataHash() == live.context.dataHash, "server HELLO carries its data hash");
		}
		{
			Client c(port);
			c.send(helloFrame(live.context.dataHash, simlink::kProtocol + 1));
			c.pump([&]() { return c.closed || !c.dec.error().empty(); }, 5000);
			test::require(c.dec.error().find("protocol") != std::string::npos, "protocol mismatch refused: " + c.dec.error());
		}
		{
			Client c(port);
			const auto garbage = rawOrder(1, 1, nullptr); // an order before HELLO
			c.send(garbage);
			c.pump([&]() { return c.closed || !c.dec.error().empty(); }, 5000);
			test::require(c.dec.error().find("HELLO") != std::string::npos, "order before HELLO refused: " + c.dec.error());
		}

		// Untrusted input over the socket (SIMLINK.md section 11): each fault kicks only that client
		// with an ERROR and a close, and the server keeps serving (the good client below proves it).
		const auto kicked = [&](Client& k, const std::string& why, const std::string& label)
		{
			k.pump([&]() { return !k.dec.error().empty() || k.closed; }, 8000);
			test::require(k.dec.error().find(why) != std::string::npos, label + ": " + k.dec.error());
			test::require(k.pump([&]() { return k.closed; }, 8000), label + ": not closed");
		};
		const auto frameHeader = [](std::uint32_t len, std::uint16_t type)
		{
			std::vector<std::uint8_t> v;
			put(v, len);
			put(v, type);
			return v;
		};
		{
			Client k(port); // silent socket: no HELLO
			kicked(k, "HELLO timeout", "silent socket");
		}
		{
			Client k(port); // HELLO, then half a frame and silence
			k.send(helloFrame(live.context.dataHash));
			k.send(frameHeader(100, simlink::kOrder));
			kicked(k, "frame timeout", "stalled frame");
		}
		{
			Client k(port); // client frames above 64 KiB
			k.send(helloFrame(live.context.dataHash));
			k.send(frameHeader(ks::kMaxClientFrame + 1, simlink::kOrder));
			kicked(k, "bad frame length", "oversized frame");
		}
		{
			Client k(port); // a 16 MiB length field must not make the server wait or allocate
			k.send(frameHeader(16u << 20, simlink::kOrder));
			kicked(k, "bad frame length", "huge frame");
		}
		{
			Client k(port); // NaN in an order
			k.send(helloFrame(live.context.dataHash));
			k.send(rawOrder(1, 51, [](auto& v) { put<float>(v, std::nanf("")); put<float>(v, 0.0f); }));
			kicked(k, "malformed order", "NaN order");
		}
		{
			Client k(port); // more ids than the cap
			k.send(helloFrame(live.context.dataHash));
			k.send(rawOrder(1, 4, [](auto& v) { put<std::uint16_t>(v, ks::kMaxOrderIds + 1); for (int i = 0; i < ks::kMaxOrderIds + 1; ++i) put<std::uint64_t>(v, 1); put<std::uint8_t>(v, 0); }));
			kicked(k, "malformed order", "too many ids");
		}
		{
			Client k(port); // order flood
			k.send(helloFrame(live.context.dataHash));
			std::vector<std::uint8_t> flood;
			for (std::uint32_t i = 0; i < ks::kDefaultOrderRate + 50; ++i)
			{
				const auto one = rawOrder(i, 51, [](auto& v) { put<float>(v, 0.0f); put<float>(v, 0.0f); });
				flood.insert(flood.end(), one.begin(), one.end());
			}
			k.send(flood);
			kicked(k, "order rate", "order flood");
		}
		{
			Client k(port); // random bytes
			std::mt19937 rng(5);
			std::vector<std::uint8_t> noise(3000);
			for (auto& b : noise)
				b = static_cast<std::uint8_t>(rng());
			k.send(helloFrame(live.context.dataHash));
			k.send(noise);
			k.pump([&]() { return k.closed || !k.dec.error().empty(); }, 8000);
			test::require(k.pump([&]() { return k.closed; }, 8000), "random bytes: not closed");
		}
		{
			Client k(port); // PING keeps a client alive and is not an order
			k.send(helloFrame(live.context.dataHash));
			test::require(k.pump([&]() { return k.snapshots >= 1; }, 10000), "ping client: no snapshot");
			std::vector<std::uint8_t> pings;
			for (int i = 0; i < 300; ++i)
			{
				const auto one = frameHeader(2, ks::kPing);
				pings.insert(pings.end(), one.begin(), one.end());
			}
			k.send(pings); // far over the order rate
			k.pump([&]() { return false; }, 1500);
			test::require(!k.closed && k.dec.error().empty(), "PINGs were punished: " + k.dec.error());
		}

		// The good client.
		Client c(port);
		c.send(helloFrame(live.context.dataHash));
		test::require(c.pump([&]() { return c.snapshots >= 1; }, 10000), "no first snapshot");
		test::require(c.dec.helloSeen() && c.dec.error().empty(), "handshake");
		test::require(c.types.front() == simlink::kHello, "HELLO is the first frame");
		// STRTAB_APPEND comes before the first record that uses a string.
		std::size_t firstStrtab = c.types.size();
		std::size_t firstAppearance = c.types.size();
		for (std::size_t i = 0; i < c.types.size(); ++i)
		{
			if (c.types[i] == simlink::kStrtab)
				firstStrtab = std::min(firstStrtab, i);
			if (c.types[i] == simlink::kAppearance)
				firstAppearance = std::min(firstAppearance, i);
		}
		test::require(firstStrtab < firstAppearance, "string table before the first appearance");
		const std::uint64_t player = live.duel.player;
		const std::uint64_t enemy = live.duel.enemy;
		test::require(c.dec.chars.count(player) && c.dec.chars.at(player).hasAppearance, "player appearance in the first snapshot");
		test::require(c.dec.chars.count(enemy) && c.dec.chars.at(enemy).hasAppearance, "enemy appearance in the first snapshot");
		test::require((c.dec.chars.at(player).cur.flags & simlink::kPlayer) != 0, "player flag");
		test::require((c.dec.chars.at(enemy).cur.flags & simlink::kPlayer) == 0, "enemy is not player-controlled");
		// portraitSerial (protocol 5, CHAR_APPEARANCE's last field) is the sim's Fighter::portraitSerial.
		for (const std::uint64_t id : {player, enemy})
		{
			test::require(c.extras.portraits.count(id) == 1, "CHAR_APPEARANCE carries a portraitSerial");
			std::uint32_t portraitSerial = 0;
			server.call([&]() { portraitSerial = live.sim->combat()->find(id)->portraitSerial; });
			test::require(c.extras.portraits.at(id) == portraitSerial, "wire portraitSerial equals the state's");
		}
		test::require(!c.dec.bounds.empty(), "DETAIL_BOUNDS after the hello");
		test::require(c.dec.clock.valid, "CLOCK in the first snapshot");
		test::require(!c.dec.squads.empty() && c.dec.squads.size() <= 50, "squads in the first snapshot");
		// WEATHER (type 41): one entry per region, strings from the table, the fades and strength in 0..1, and it matches the
		// sim's regions.
		{
			const ksim::WeatherSystem* weather = live.sim->weather();
			test::require(weather != nullptr, "the new game has a weather system");
			test::require(!c.dec.weather.empty() && c.dec.weather.size() == weather->model().groups().size(),
				"WEATHER carries one entry per region in the first snapshot");
			std::vector<ksim::state::WeatherRegion> regions;
			server.call([&]() { regions = live.sim->world().ecs().get<ksim::state::WeatherRegions>()->regions; });
			test::require(regions.size() == c.dec.weather.size(), "WEATHER entries equal the regions");
			for (std::size_t i = 0; i < regions.size(); ++i)
			{
				const auto& w = c.dec.weather[i];
				test::require(c.dec.str(w.group) == regions[i].group, "WEATHER group string");
				test::require(c.dec.str(w.weather) == regions[i].weather && !regions[i].weather.empty(), "WEATHER weather string");
				test::require(w.fogBlend >= 0.0f && w.fogBlend <= 1.0f && w.skyBlend >= 0.0f && w.skyBlend <= 1.0f, "WEATHER blends in 0..1");
				test::require(w.strength >= 0.0f && w.strength <= 1.0f, "WEATHER strength in 0..1");
			}
			// The first WEATHER frame is sent at the handshake, before any view focus: the optional wind pair (SIMLINK "WEATHER")
			// is absent (a later frame may carry one, e.g. after another client's orders set a focus).
			test::require(!c.extras.winds.empty() && !c.extras.winds.front().present,
				"the first WEATHER frame carries a wind pair");
		}

		simlink::OrderWriter orders;
		std::vector<std::uint8_t> out;

		// The server started paused so the duel did not run during the socket checks above.
		out = rawOrder(1000, 40, [](auto& v) { put<std::uint8_t>(v, 0); });
		c.send(out);
		test::require(c.pump([&]() { return c.dec.clock.valid && !c.dec.clock.paused && c.find(9, 1000) != nullptr; }, 5000), "unpause");

		// Speed up (not a sim command): answered with event 9, appliedTick = the current tick.
		out = rawOrder(1001, 41, [](auto& v) { put<float>(v, 16.0f); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, 1001) != nullptr; }, 5000), "speed order not acknowledged");
		test::require(c.pump([&]() { return c.dec.clock.speed == 16.0f; }, 5000), "CLOCK on speed change");

		// kenshi-k16.5.13: a fighter whose weapon / armour has no record by that name (a type 2 weapon with an empty name, an unknown piece) is sent
		// without that item; the snapshot completes and the other characters still arrive.
		{
			std::size_t itemsBefore = 0;
			ksim::state::FighterWeapon weaponBefore;
			server.call([&]() {
				ksim::state::Fighter* f = const_cast<ksim::CombatArena*>(live.sim->combat())->find(enemy);
				weaponBefore = f->weapon;
				itemsBefore = f->armour.size() + (f->weapon.weaponType != 5 ? 1 : 0);
				f->weapon.weaponType = 2;
				f->weapon.name.clear();
				ksim::state::FighterArmour bad;
				bad.name = "no such armour piece";
				f->armour.push_back(bad);
			});
			const std::size_t snapshotsBefore = c.snapshots;
			test::require(c.pump([&]() { return c.snapshots >= snapshotsBefore + 6; }, 10000), "snapshots continue after a fighter with an unknown weapon and armour name");
			test::require(c.dec.error().empty(), "no client fault after an unknown item name: " + c.dec.error());
			test::require(c.dec.chars.at(enemy).look.items.size() <= itemsBefore, "the unknown items are not sent");
			test::require(c.dec.chars.count(player) && c.dec.chars.at(player).hasAppearance, "the player's appearance survives the bad item");
			server.call([&]() {
				ksim::state::Fighter* f = const_cast<ksim::CombatArena*>(live.sim->combat())->find(enemy);
				f->armour.pop_back();
				f->weapon = weaponBefore;
			});
		}

		// The debug view (SIMLINK.md "DEBUG VIEW"): DEBUG_SUBSCRIBE is answered with DEBUG_INFO in the next tick's send; while it is on,
		// every SQUADS frame is followed by a DEBUG_SQUADS repeating its id set (checked in intercept); DEBUG_QUERY is answered with
		// one DEBUG_TREE, an unknown id with 0 rows. Every frame in 200-219 is consumed by the dispatch, never a protocol error.
		{
			c.send(rawFrame(ksim::debugview::kDebugSubscribe, {1}));
#if KSIM_DEBUG_VIEW
			test::require(c.pump([&]() { return !c.extras.debugInfos.empty(); }, 5000), "subscribe not answered with DEBUG_INFO");
			test::require(c.extras.debugInfos.front().version == ksim::debugview::kDebugVersion
				&& c.extras.debugInfos.front().enabled == ksim::debugview::kEnabled, "DEBUG_INFO: debugVersion 1, enabled");
			// From here every SQUADS must be followed by its DEBUG_SQUADS.
			c.extras.countSquads = true;
			test::require(c.pump([&]() { return c.extras.debugSquads.size() >= 2; }, 5000), "no DEBUG_SQUADS after the subscribe");
			test::require(!c.extras.debugSquads.back().ids.empty(), "DEBUG_SQUADS carries no squads");
			const std::uint64_t squad = c.extras.debugSquads.back().ids.front();
			std::vector<std::uint8_t> query{static_cast<std::uint8_t>(ksim::debugview::QueryKind::Squad)};
			put<std::uint64_t>(query, squad);
			c.send(rawFrame(ksim::debugview::kDebugQuery, query));
			test::require(c.pump([&]() { return !c.extras.debugTrees.empty(); }, 5000), "no DEBUG_TREE answer");
			const Extras::DebugTree& tree = c.extras.debugTrees.front();
			test::require(tree.kind == static_cast<std::uint8_t>(ksim::debugview::QueryKind::Squad) && tree.id == squad
				&& tree.rows > 0, "DEBUG_TREE: the queried squad with rows");
			query.assign(1, static_cast<std::uint8_t>(ksim::debugview::QueryKind::Squad));
			put<std::uint64_t>(query, 0xffffffffffffffffull);
			c.send(rawFrame(ksim::debugview::kDebugQuery, query));
			test::require(c.pump([&]() { return c.extras.debugTrees.size() >= 2; }, 5000), "no answer to an unknown id");
			test::require(c.extras.debugTrees[1].id == 0xffffffffffffffffull && c.extras.debugTrees[1].rows == 0,
				"an unknown id answered rows > 0");
			// The decisions subtree (kenshi-k16.6.43): a pick recorded for a member comes back newest first, one row per candidate.
			{
				ksim::trace::Sink sink = ksim::debugview::traceSink();
				for (std::uint64_t tick = 1; tick <= 10; ++tick) // ten picks into a ring of 8
				{
					ksim::trace::Pick pick;
					pick.tick = tick;
					pick.member = 424242;
					pick.source = tick == 10 ? ksim::trace::Source::Combat : ksim::trace::Source::TownScorer; // the newest is a fight pick (k16.6.59)
					pick.result = ksim::trace::Result::Chosen;
					pick.task = 43;
					pick.candidates = {{43, 2, 0, 1.0, 1.0, 1.0, 1.25, ksim::goalsel::Why::Chosen},
						{12, 1, 0, 1.0, 1.0, 1.0, 1.0, ksim::goalsel::Why::LowerTier}};
					sink.record(pick);
				}
				const std::size_t before = c.extras.debugTrees.size();
				std::vector<std::uint8_t> characterQuery{static_cast<std::uint8_t>(ksim::debugview::QueryKind::Character)};
				put<std::uint64_t>(characterQuery, 424242);
				c.send(rawFrame(ksim::debugview::kDebugQuery, characterQuery));
				test::require(c.pump([&]() { return c.extras.debugTrees.size() > before; }, 5000), "no DEBUG_TREE for the character query");
				const Extras::DebugTree& characterTree = c.extras.debugTrees[before];
				std::vector<std::string> picks;
				std::size_t candidates = 0;
				bool header = false;
				for (const std::string& row : characterTree.text)
				{
					header = header || row == "1 decisions = 8 picks";
					picks.push_back(row.rfind("2 pick = ", 0) == 0 ? row : "");
					candidates += row.rfind("3 candidate ", 0) == 0 ? 1 : 0;
				}
				test::require(header && candidates == 16, "the decisions subtree: 8 picks of 2 candidates");
				const auto firstPick = std::find_if(picks.begin(), picks.end(), [](const std::string& row) { return !row.empty(); });
				test::require(firstPick != picks.end() && firstPick->find("tick 10 Combat") != std::string::npos,
					"the decisions subtree lists the newest pick first, a combat pick under its source name");
				test::require(std::find(characterTree.text.begin(), characterTree.text.end(),
					"3 candidate 12 = LowerTier tier 1 target 0 utility 1.00 weight 1.00 crowd 1.00 score 1.00") != characterTree.text.end(),
					"the decisions subtree: a candidate row carries its verdict");
			}
			// Paused (kenshi-k16.2.104.1): no tick, no snapshot, but the queued DEBUG_QUERY is still answered, for the paused tick.
			{
				c.send(rawOrder(2900, 40, [](auto& v) { put<std::uint8_t>(v, 1); }));
				test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "paused debug: the pause order");
				c.pump([&]() { return false; }, 300); // drain what was in flight
				const std::uint64_t pausedAt = c.lastSnapshotEnd;
				const std::size_t before = c.extras.debugTrees.size();
				std::vector<std::uint8_t> characterQuery{static_cast<std::uint8_t>(ksim::debugview::QueryKind::Character)};
				put<std::uint64_t>(characterQuery, 424242);
				c.send(rawFrame(ksim::debugview::kDebugQuery, characterQuery));
				test::require(c.pump([&]() { return c.extras.debugTrees.size() > before; }, 5000), "paused debug: no DEBUG_TREE while paused");
				test::require(c.extras.debugTrees[before].id == 424242 && c.extras.debugTrees[before].tick == pausedAt && c.lastSnapshotEnd == pausedAt,
					"paused debug: the answer is for the paused tick and no snapshot came with it");
				// A subscribe answered while paused carries its DEBUG_SQUADS (a paused F12 shot learns its squads from it): the paused
				// tick, the id set of the last SQUADS (nothing ticked since). Outside the SQUADS cadence, so the follow check is off.
				c.extras.countSquads = false;
				const std::size_t squadsBefore = c.extras.debugSquads.size();
				c.send(rawFrame(ksim::debugview::kDebugSubscribe, {1}));
				test::require(c.pump([&]() { return c.extras.debugSquads.size() > squadsBefore; }, 5000),
					"paused debug: no DEBUG_SQUADS for a subscribe while paused");
				test::require(c.extras.debugSquads[squadsBefore].tick == pausedAt && c.extras.debugSquads[squadsBefore].ids == c.extras.lastSquads
					&& c.lastSnapshotEnd == pausedAt, "paused debug: DEBUG_SQUADS is the paused tick's, the last SQUADS set, no snapshot");
				c.pump([&]() { return false; }, 300);
				test::require(c.extras.debugSquads.size() == squadsBefore + 1, "paused debug: one DEBUG_SQUADS per subscribe, not one per loop pass");
				c.extras.countSquads = true;
				c.send(rawOrder(2901, 40, [](auto& v) { put<std::uint8_t>(v, 0); }));
				test::require(c.pump([&]() { return c.lastSnapshotEnd > pausedAt; }, 5000), "paused debug: no snapshots after the unpause");
			}
			test::require(c.dec.error().empty(), "a debug frame was a protocol error");
#else
			// No debug view in this build: the control frame is accepted and dropped (never a protocol error or a close), unanswered.
			const std::uint64_t snapshotsBefore = c.snapshots;
			test::require(c.pump([&]() { return c.snapshots >= snapshotsBefore + 5; }, 5000), "the session died on a debug frame");
			test::require(c.extras.debugInfos.empty() && c.extras.debugSquads.empty() && c.extras.debugTrees.empty(),
				"a KSIM_DEBUG_VIEW=OFF server sent debug frames");
#endif
		}

		// Rejections: not a player character, an unknown id, a follow of itself, a bad value.
		out.clear();
		const std::uint32_t seqEnemy = orders.moveTo(out, {enemy}, 1.0f, 1.0f, false);
		const std::uint32_t seqNobody = orders.moveTo(out, {999999999ull}, 1.0f, 1.0f, false);
		c.send(out);
		out = rawOrder(1002, 3, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint64_t>(v, player); });
		c.send(out);
		out = rawOrder(1003, 41, [](auto& v) { put<float>(v, -1.0f); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(10, 1003) != nullptr; }, 5000), "rejections not answered");
		test::require(reasonOf(c, *c.find(10, seqEnemy)) == "not_player_controlled", "enemy move rejected");
		test::require(reasonOf(c, *c.find(10, seqNobody)) == "unknown_id", "unknown id rejected");
		test::require(reasonOf(c, *c.find(10, 1002)) == "bad_target", "follow of itself rejected");
		test::require(reasonOf(c, *c.find(10, 1003)) == "bad_value", "negative speed rejected");

		// move_to: applied at the next tick, and the character walks there.
		const simlink::Dynamic before = c.dec.chars.at(player).cur;
		const float targetX = before.pos[0] - 120.0f;
		const float targetZ = before.pos[2] - 20.0f;
		out.clear();
		const std::uint32_t seqMove = orders.moveTo(out, {player}, targetX, targetZ, false);
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, seqMove) != nullptr; }, 5000), "move_to not acknowledged");
		const simlink::Event& moveAck = *c.find(9, seqMove);
		test::require(moveAck.b == moveAck.tick + 1, "applied tick is the next tick after the answer");
		test::require(c.pump([&]()
		{
			const simlink::Dynamic& d = c.dec.chars.at(player).cur;
			return std::hypot(d.pos[0] - targetX, d.pos[2] - targetZ) < 6.0;
		}, 20000), "player did not reach the move_to target");

		// The route went out as CHAR_ROUTE with the order's seq, and was cleared on arrival; y is valid.
		test::require(c.pump([&]()
		{
			// Speed 16 batches 8 ticks into one snapshot (SIMLINK "Cadence"): the arrival's clear and the chase route that follows it can
			// share a snapshot, so the order's route is over when a clear or a record of another route follows its own.
			bool sawOwn = false;
			for (const RouteRecord& r : c.extras.routes)
			{
				if (r.id != player)
					continue;
				if (r.clientSeq == seqMove && r.points.empty())
					return true;
				if (r.clientSeq != seqMove && sawOwn)
					return true;
				sawOwn = sawOwn || (r.clientSeq == seqMove && !r.points.empty());
			}
			return false;
		}, 5000), "no CHAR_ROUTE clear after the walk");
		{
			const RouteRecord* first = nullptr;
			const RouteRecord* last = nullptr;
			for (const RouteRecord& r : c.extras.routes)
			{
				if (r.id == player && r.clientSeq == seqMove)
				{
					first = first != nullptr ? first : &r;
					last = &r;
				}
			}
			test::require(!first->points.empty() && (first->flags & 3) == 0, "first CHAR_ROUTE: waypoints, no flags");
			test::require(first->points[0][1] > 1.0f, "route waypoints carry the surface y");
			const auto& end = first->points.back();
			test::require(std::hypot(end[0] - targetX, end[2] - targetZ) < 45.0, "route ends near the target (within the 40 unit snap)");
			test::require(last == first || last->version > first->version, "the clear bumps routeVersion");
			const simlink::Dynamic& d = c.dec.chars.at(player).cur;
			test::require((d.flags & simlink::kYValid) != 0 && d.pos[1] > 1.0f, "y is valid and set in CHAR_DYNAMIC");
		}
		test::require(c.extras.corrections.empty(), "a character was corrected off the navmesh during normal play");

		// No route: a point far outside the baked zone is answered no_route, carrying the order's seq.
		out.clear();
		const std::uint32_t seqNoRoute = orders.moveTo(out, {player}, targetX + 30000.0f, targetZ, false);
		c.send(out);
		test::require(c.pump([&]() { return c.find(10, seqNoRoute) != nullptr; }, 5000), "off-mesh move_to not answered");
		test::require(reasonOf(c, *c.find(10, seqNoRoute)) == "no_route", "off-mesh move_to is no_route");

		// Direct drive: forward along a heading, then stop; driveSeq follows; a bad mode is bad_value.
		// The drive and strafe checks stop a drive with an order, so how far the character went depends on how many ticks ran between the
		// client's two frames. At 16x under load that is a long way (a wall, a navmesh correction, a strafe slid along an edge: k16.8.19), so
		// the block runs at speed 1 and the speed comes back after it.
		const auto setSpeed = [&](std::uint32_t seq, float speed)
		{
			c.send(rawOrder(seq, 41, [&](auto& v) { put<float>(v, speed); }));
			test::require(c.pump([&]() { return c.find(9, seq) != nullptr && c.dec.clock.speed == speed; }, 5000), "speed order not applied");
		};
		setSpeed(1030, 1.0f);
		const std::uint64_t routesBeforeDrive = c.extras.routes.size();
		const simlink::Dynamic driveFrom = c.dec.chars.at(player).cur;
		out = rawOrder(1010, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 1); put<float>(v, 1.5f); });
		c.send(out);
		out = rawOrder(1011, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 9); put<float>(v, 0.0f); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, 1010) != nullptr && c.find(10, 1011) != nullptr; }, 5000), "drive orders not answered");
		test::require(reasonOf(c, *c.find(10, 1011)) == "bad_value", "drive mode 9 is bad_value");
		test::require(c.pump([&]()
		{
			const simlink::Dynamic& d = c.dec.chars.at(player).cur;
			return d.drive == 1 && d.driveSeq == 1010
				&& std::hypot(d.pos[0] - driveFrom.pos[0], d.pos[2] - driveFrom.pos[2]) > 20.0;
		}, 60000), "driven character did not move");
		out = rawOrder(1012, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<float>(v, 1.5f); });
		c.send(out);
		test::require(c.pump([&]() { return c.dec.chars.at(player).cur.drive == 0 && c.dec.chars.at(player).cur.driveSeq == 1012; }, 5000),
			"drive stop not applied");
		// Strafe (k16.8.17): order 20's optional trailing i8. Right: perpendicular to the heading, flags bit 23; left: bit 22; a 13-byte
		// body is strafe 0 (both bits clear); 2 is bad_value.
		{
			const auto flagsOf = [&]() { return c.dec.chars.at(player).cur.flags; };
			// What the check measures is the strafe's own motion, taken from CHAR_DYNAMIC's fwd / lat: the body's actual step for the tick,
			// not the commanded speed (SIMLINK CHAR_DYNAMIC). Two movers other than the strafe turn up here under load, and neither is the
			// drive's doing: the walker slides the body along an edge, and separate() shoves it away from a neighbour - half the overlap a
			// step, 3.86 u in master CI e88g / 3ge6 / 4mqi when the late stop leaves the drive ending on the duel partner (dx -3.859375
			// dz -0.062988 at heading 1.5). Both move the body along the heading, where a sidestep's fwd is a sliver of its lat, so a step
			// whose fwd is not that sliver is left out of the displacement (k16.8.21). A blocked event (kind 8) in the window still skips
			// the check, as for k16.8.18: the perpendicular test only holds for an unobstructed strafe.
			const std::size_t eventsBefore = c.events.size();
			const auto blockedIn = [&]()
			{
				for (std::size_t i = eventsBefore; i < c.events.size(); ++i)
					if (c.events[i].kind == 8 && c.events[i].a == player)
						return true;
				return false;
			};
			std::uint64_t measuredTick = 0; // the frame the measured displacement ends at; 0 before the strafe's first
			double strafeX = 0.0;
			double strafeZ = 0.0;
			std::size_t shovedSteps = 0;
			out = rawOrder(1013, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<float>(v, 1.5f); put<std::int8_t>(v, 1); });
			c.send(out);
			out = rawOrder(1014, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<float>(v, 1.5f); put<std::int8_t>(v, 2); });
			c.send(out);
			test::require(c.pump([&]() { return c.find(10, 1014) != nullptr; }, 5000), "the bad strafe was not answered");
			test::require(reasonOf(c, *c.find(10, 1014)) == "bad_value", "strafe 2 is bad_value");
			test::require(c.pump([&]()
			{
				const simlink::Character& ch = c.dec.chars.at(player);
				const simlink::Dynamic& d = ch.cur;
				if (d.driveSeq != 1013)
					return false;
				if (d.tick != measuredTick && ch.prev.tick != 0 && ch.prev.tick <= d.tick)
				{
					if (std::fabs(d.fwd) <= 0.1 * (std::fabs(d.lat) + 1.0))
					{
						strafeX += d.pos[0] - ch.prev.pos[0];
						strafeZ += d.pos[2] - ch.prev.pos[2];
					} else {
						++shovedSteps;
					}
					measuredTick = d.tick;
				}
				return std::hypot(strafeX, strafeZ) > 3.0 || blockedIn();
			}, 60000), "the strafing character did not move");
			const simlink::Dynamic mid = c.dec.chars.at(player).cur;
			test::require((flagsOf() & (1u << 22)) == 0 && mid.drive == 0, "strafe right set the left bit or a drive");
			const double dx = strafeX, dz = strafeZ;
			const bool blocked = blockedIn();
			if (blocked)
				std::cout << "strafe right was blocked by the mesh edge: perpendicular check skipped (dx " << dx << " dz " << dz << ")\n";
			else if (shovedSteps != 0)
				std::cout << "strafe right: " << shovedSteps << " step(s) were a shove, not the strafe, and left out (dx " << dx << " dz " << dz << ")\n";
			test::require(blocked || std::fabs(dx * std::sin(1.5) + dz * std::cos(1.5)) < 0.05 * std::hypot(dx, dz),
				"the strafe was not perpendicular to the heading: dx " + std::to_string(dx) + " dz " + std::to_string(dz) + " heading 1.5");
			out = rawOrder(1015, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<float>(v, 1.5f); put<std::int8_t>(v, -1); });
			c.send(out);
			test::require(c.pump([&]() { return c.dec.chars.at(player).cur.driveSeq == 1015 && (flagsOf() & (1u << 22)) != 0 && (flagsOf() & (1u << 23)) == 0; }, 5000),
				"strafe left did not set bit 22 only");
			out = rawOrder(1016, 20, [&](auto& v) { put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<float>(v, 1.5f); });
			c.send(out);
			test::require(c.pump([&]() { return c.dec.chars.at(player).cur.driveSeq == 1016 && (flagsOf() & ((1u << 22) | (1u << 23))) == 0; }, 5000),
				"the 13-byte drive order did not clear the strafe");
		}
		if (c.extras.routes.size() != routesBeforeDrive)
		{
			for (std::size_t i = routesBeforeDrive; i < c.extras.routes.size(); ++i)
			{
				const RouteRecord& r = c.extras.routes[i];
				std::cerr << "extra route: tick " << r.tick << " id " << r.id << " seq " << r.clientSeq << " ver " << r.version
					<< " flags " << int(r.flags) << " points " << r.points.size() << " (drive applied " << c.find(9, 1010)->b << ")\n";
			}
		}
		setSpeed(1031, 16.0f);
		// Driving must not set a route on the driven character. Only records with waypoints, emitted from the tick the
		// drive was applied on, count. A route set earlier is the character's own AI (an idle player character
		// approaches the hostile servant; the walk's end and this drive land at timing-dependent ticks): the drive
		// then clears it, which is a record without waypoints.
		const std::uint64_t driveApplied = c.find(9, 1010)->b;
		for (std::size_t i = routesBeforeDrive; i < c.extras.routes.size(); ++i)
		{
			const RouteRecord& r = c.extras.routes[i];
			test::require(r.id != player || r.points.empty() || r.tick < driveApplied, "driving made a route");
		}
		test::require(c.extras.corrections.empty(), "a driven character was corrected off the navmesh");

		// Speed modes (order 10 which 5): the flags bits 12 and 17-18, the which-2 alias, and above 3 bad_value.
		const auto speedModeOf = [&] { return (c.dec.chars.at(player).cur.flags >> ks::kFlagSpeedModeShift) & 3u; };
		out = rawOrder(1020, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 5); put<std::uint8_t>(v, 4); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(10, 1020) != nullptr; }, 5000), "speed mode 4 not answered");
		test::require(reasonOf(c, *c.find(10, 1020)) == "bad_value", "speed mode 4 is bad_value");
		out = rawOrder(1021, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 5); put<std::uint8_t>(v, 1); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, 1021) != nullptr && speedModeOf() == 1; }, 5000), "which 5 Jog not applied");
		test::require((c.dec.chars.at(player).cur.flags & ks::kFlagRun) != 0, "Jog cleared the run flag");
		out = rawOrder(1022, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 5); put<std::uint8_t>(v, 0); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, 1022) != nullptr && speedModeOf() == 0; }, 5000), "which 5 Walk not applied");
		test::require((c.dec.chars.at(player).cur.flags & ks::kFlagRun) == 0, "Walk left the run flag set");
		out = rawOrder(1023, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 2); put<std::uint8_t>(v, 1); });
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, 1023) != nullptr && speedModeOf() == 2 && (c.dec.chars.at(player).cur.flags & ks::kFlagRun) != 0; }, 5000),
			"the which-2 run alias is not Run");

		// Passive / hold position (order 10 which 6 / 7, k16.6.313): flags bits 24 / 25, independent of each other and of bit 13; above 1 bad_value.
		{
			const auto flagsOf = [&] { return c.dec.chars.at(player).cur.flags; };
			const auto toggle = [&](std::uint32_t seq, std::uint8_t which, std::uint8_t value)
			{
				out = rawOrder(seq, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, which); put<std::uint8_t>(v, value); });
				c.send(out);
			};
			toggle(1030, 6, 2);
			test::require(c.pump([&]() { return c.find(10, 1030) != nullptr; }, 5000), "passive 2 not answered");
			test::require(reasonOf(c, *c.find(10, 1030)) == "bad_value", "passive 2 is bad_value");
			toggle(1031, 6, 1);
			test::require(c.pump([&]() { return c.find(9, 1031) != nullptr && (flagsOf() & ks::kFlagPassive) != 0; }, 5000), "which 6 passive not applied");
			test::require((flagsOf() & ks::kFlagHoldPosition) == 0, "passive set the hold position bit");
			toggle(1032, 7, 1);
			test::require(c.pump([&]() { return c.find(9, 1032) != nullptr && (flagsOf() & ks::kFlagHoldPosition) != 0; }, 5000), "which 7 hold position not applied");
			test::require((flagsOf() & ks::kFlagPassive) != 0, "hold position cleared passive");
			toggle(1033, 6, 0);
			test::require(c.pump([&]() { return c.find(9, 1033) != nullptr && (flagsOf() & ks::kFlagPassive) == 0; }, 5000), "which 6 passive not cleared");
			test::require((flagsOf() & ks::kFlagHoldPosition) != 0, "clearing passive cleared hold position");
			toggle(1034, 7, 0);
			test::require(c.pump([&]() { return c.find(9, 1034) != nullptr && (flagsOf() & ks::kFlagHoldPosition) == 0; }, 5000), "which 7 hold position not cleared");
		}

		// attack: the player hits (or is dodged by) the servant; a hit event arrives.
		out.clear();
		const std::uint32_t seqAttack = orders.attack(out, {player}, enemy);
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, seqAttack) != nullptr; }, 5000), "attack not acknowledged");
		// The wait is in sim ticks (the sim's pace follows the box's load, k16.8.19); the wall limit only catches a hung server.
		const std::uint64_t attackTick = c.find(9, seqAttack)->b;
		bool swung = false;
		test::require(c.pump([&]()
		{
			for (const simlink::Event& e : c.events)
				swung = swung || (e.kind == 1 && e.tick >= attackTick && e.b == player);
			return swung || c.dec.clock.tick > attackTick + 20000;
		}, 300000) && swung, "no swing at the servant after the attack order");
		// Perception: the servant became aware of the player (EVENT 15, observer = servant, target = player), and the player's
		// detection meter was reported (CHAR_DETECT) while the servant was aware of it.
		{
			bool servantNoticed = false;
			for (const auto& [observer, target] : c.extras.noticed)
				servantNoticed = servantNoticed || (observer == enemy && target == player);
			test::require(servantNoticed, "no EVENT 15 noticed (servant -> player)");
			test::require(c.extras.detects > 0, "no CHAR_DETECT for the player");
			test::require(c.extras.qualities > 0, "no ITEM_QUALITY after the player's INVENTORY (its armour has a quality)");
			// ITEM_GRID (28) as the client's decoder reads it: the 8 x 10 main grid and, for every INVENTORY entry, equipped (255) or a cell inside it.
			{
				const auto held = c.dec.inventories.find(player);
				test::require(held != c.dec.inventories.end() && !held->second.grids.empty(), "no ITEM_GRID decoded for the player's INVENTORY");
				const auto& grids = held->second.grids;
				test::require(grids[0].section == 0 && grids[0].w == 8 && grids[0].h == 10, "ITEM_GRID: the main grid is not 8 x 10");
				bool material = false;
				for (const auto& e : held->second.items)
				{
					test::require(e.slot != 0 ? e.section == 255 : (e.section == 0 && e.x < 8 && e.y < 10), "ITEM_GRID: an entry's section / cell");
					material = material || e.material != 0;
				}
				test::require(material, "ITEM_GRID: no entry carries a material (the player's armour has one)");
			}
		}
		// The hit event is well formed: victim and attacker are the two characters.
		for (const simlink::Event& e : c.events)
		{
			if (e.kind == 1)
			{
				test::require((e.a == player || e.a == enemy) && (e.b == player || e.b == enemy), "hit event ids");
				test::require(e.result <= 2, "hit result");
			}
		}
		// The absorbed tail is never negative, and a blocked / dodged event 1 sends 0, 0 (armoured and bare victims: the held runs above).
		for (const Extras::Hit& h : c.extras.hits)
		{
			test::require(h.cutAbsorbed >= 0.0f && h.bluntAbsorbed >= 0.0f, "negative absorption");
			if (h.result != 0)
				test::require(h.cutAbsorbed == 0.0f && h.bluntAbsorbed == 0.0f, "a blocked / dodged event 1 carries absorption");
		}

		// Block mode on, then off: the flag follows.
		out = rawOrder(1004, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<std::uint8_t>(v, 1); });
		c.send(out);
		test::require(c.pump([&]() { return (c.dec.chars.at(player).cur.flags & simlink::kBlockMode) != 0; }, 5000), "block mode flag");
		out = rawOrder(1005, 10, [&](auto& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, player); put<std::uint8_t>(v, 0); put<std::uint8_t>(v, 0); });
		c.send(out);
		test::require(c.pump([&]() { return (c.dec.chars.at(player).cur.flags & simlink::kBlockMode) == 0; }, 5000), "block mode off");

		// stop (hold), interest, view.
		out.clear();
		const std::uint32_t seqStop = orders.stop(out, {player}, true);
		const std::uint32_t seqInterest = orders.setInterest(out, targetX, targetZ, 500.0f);
		const std::uint32_t seqView = orders.setView(out, targetX, targetZ);
		c.send(out);
		test::require(c.pump([&]() { return c.find(9, seqStop) && c.find(9, seqInterest) && c.find(9, seqView); }, 5000),
			"stop / interest / view not acknowledged");

		// The WEATHER wind pair (SIMLINK "WEATHER"): with a view focus in a region, the frame carries that group's wind speed and
		// direction. The speed is the same raw value as the PROSPECT frame's `wind` (windOf of the instance). The frame must belong
		// to the instance the camera group has now: a frame sent just before the focus or a re-pick is waited out.
		{
			const ksim::WeatherSystem* weather = live.sim->weather();
			const std::string group = weather->groupAt(targetX, targetZ);
			test::require(!group.empty(), "the view target lies in no weather group");
			// A copy taken on the server's thread; `region` below points into it, so it lives as long as the loop's pass.
			std::vector<ksim::state::WeatherRegion> regionCopy;
			const auto regionsNow = [&]() -> const std::vector<ksim::state::WeatherRegion>&
			{
				server.call([&]() { regionCopy = live.sim->world().ecs().get<ksim::state::WeatherRegions>()->regions; });
				return regionCopy;
			};
			bool matched = false;
			std::size_t seen = c.extras.winds.size();
			for (int attempt = 0; attempt < 4 && !matched; ++attempt)
			{
				test::require(c.pump([&]() { return c.extras.winds.size() > seen; }, 10000),
					"no WEATHER frame with the wind pair after a view focus");
				seen = c.extras.winds.size();
				const Extras::WeatherWind& wind = c.extras.winds.back();
				test::require(wind.present, "a WEATHER frame lost the wind pair after a view focus");
				test::require(wind.yaw >= 0.0f && wind.yaw < 6.2831855f, "the WEATHER wind yaw is outside [0, 2 pi)");
				// The camera group's entry in the same frame names the instance the pair should describe.
				const ksim::state::WeatherRegion* region = nullptr;
				for (std::size_t i = 0; i < wind.groups.size() && region == nullptr; ++i)
				{
					if (c.dec.str(wind.groups[i]) != group)
						continue;
					for (const ksim::state::WeatherRegion& r : regionsNow())
					{
						if (r.group == group && r.weather == c.dec.str(wind.weathers[i])
							&& static_cast<float>(r.strength) == wind.strengths[i])
							region = &r;
					}
				}
				if (region == nullptr)
					continue; // the weather changed since the frame: take the next one
				const ksim::WeatherDef* def = weather->model().weather(region->weather);
				test::require(def != nullptr, "the frame's camera-group weather is unknown");
				const float speed = static_cast<float>(def->windMin + (def->windMax - def->windMin) * region->strength);
				matched = wind.speed == speed && wind.yaw == static_cast<float>(ksim::windYaw(group, *region));
			}
			test::require(matched, "the WEATHER wind pair does not describe the camera group's wind");
			const Extras::WeatherWind& wind = c.extras.winds.back();
			std::cout << "WEATHER wind: " << wind.speed << " u/s toward yaw " << wind.yaw << " in " << group << "\n";
		}

		// Pause: the clock says so and ticks stop; unpause resumes.
		out = rawOrder(1006, 40, [](auto& v) { put<std::uint8_t>(v, 1); });
		c.send(out);
		test::require(c.pump([&]() { return c.dec.clock.paused; }, 5000), "pause not reported");
		c.pump([&]() { return false; }, 300); // drain what was in flight
		const std::uint64_t pausedAt = c.lastSnapshotEnd;
		c.pump([&]() { return false; }, 400);
		test::require(c.lastSnapshotEnd == pausedAt, "snapshots kept coming while paused");
		out = rawOrder(1007, 40, [](auto& v) { put<std::uint8_t>(v, 0); });
		c.send(out);
		test::require(c.pump([&]() { return c.lastSnapshotEnd > pausedAt; }, 5000), "no snapshots after unpause");

		// Run a little longer so the log holds a stretch of play, then stop the server.
		const std::uint64_t until = c.lastSnapshotEnd + 100;
		c.pump([&]() { return c.lastSnapshotEnd >= until; }, 10000);
		test::require(c.dec.error().empty(), "decoder error: " + c.dec.error());
		stop.store(true);
		thread.join();
		if (serverError)
			std::rethrow_exception(serverError);

		// ---- replay from the order log ----
		const std::uint64_t endTick = live.sim->clockTick();
		const std::uint64_t liveHash = live.sim->stateHash();
		std::size_t simOrders = 0;
		for (const ks::OrderLogEntry& entry : live.context.orderLog)
			simOrders += entry.sim ? 1 : 0;
		test::require(simOrders >= 6, "order log holds the sim commands");
		World replay = makeWorld(order, data, gd, nav);
		const_cast<ksim::CombatArena*>(replay.sim->combat())->find(replay.duel.enemy)->faction = 9999; // as the live world
		test::require(replay.duel.player == player && replay.duel.enemy == enemy, "same ids in the replay");
		for (std::uint64_t t = replay.sim->clockTick() + 1; t <= endTick; ++t)
		{
			for (const ks::OrderLogEntry& entry : live.context.orderLog)
			{
				if (entry.sim && entry.tick == t)
					replay.sim->enqueue(entry.command);
			}
			replay.sim->tick();
		}
		test::require(replay.sim->clockTick() == endTick, "replay ran to the same tick");
		test::require(replay.sim->stateHash() == liveHash, "replayed session differs from the live one (state hash)");

		// The client's report (2026-11-05): with --scenario newgame the world restarts at its start tick right after the connect
		// (the sim is idle with no ready session, so the connect's first tick applies the scenario's NEW_GAME), and the debug view
		// must survive it: the subscribe that rides with HELLO is answered in the next send rather than at the 1 Hz SQUADS cadence,
		// a re-subscribe is answered again, and the new game's player squad (state::PlayerSquad: no SquadInstance, no template)
		// answers a tree with rows.
		{
			World fresh = makeNewGameWorld(order, data, gd, nav);
			fresh.context.debugView = true;
			const std::uint64_t newGameTick = fresh.sim->clockTick() + 1; // the command's tick, before the server runs
			std::promise<int> freshPortPromise;
			ks::ServerOptions freshOptions;
			freshOptions.onListening = [&freshPortPromise](int p) { freshPortPromise.set_value(p); };
			freshOptions.pingTimeoutMs = 60000;
			ks::SimLinkServer freshServer(*fresh.sim, fresh.context, freshOptions);
			freshServer.listen();
			const int freshPort = freshPortPromise.get_future().get();
			std::atomic<bool> freshStop{false};
			std::thread freshThread([&]()
			{
				try
				{
					freshServer.run(freshStop);
				} catch (...) {
				}
			});
			const struct FreshJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~FreshJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} freshJoiner{freshStop, freshThread};

			Client c(freshPort);
			const std::vector<std::uint8_t> subscribe = rawFrame(ksim::debugview::kDebugSubscribe, {1});
			// The camera order rides with HELLO too (as the client's does): the session's set_view comes before the restart.
			const std::vector<std::uint8_t> view = rawOrder(2000, 51, [&](auto& v) { put<float>(v, targetX); put<float>(v, targetZ); });
#if KSIM_DEBUG_VIEW
			// HELLO, the subscribe and the camera in one write: the server reads all three before its first tick.
			std::vector<std::uint8_t> hello = helloFrame(fresh.context.dataHash);
			hello.insert(hello.end(), subscribe.begin(), subscribe.end());
			hello.insert(hello.end(), view.begin(), view.end());
			c.send(hello);
			test::require(c.pump([&]() { return !c.extras.debugInfos.empty(); }, 15000),
				"the subscribe that rode with HELLO was not answered");
			test::require(c.extras.debugInfos.front().enabled == ksim::debugview::kEnabled, "DEBUG_INFO: enabled");
			const std::uint64_t answerTick = c.extras.lastSquadsTick; // the last SQUADS before the answer
			c.extras.countSquads = true; // from here every SQUADS carries its DEBUG_SQUADS
			test::require(c.pump([&]() { return !c.extras.debugSquads.empty(); }, 15000), "no DEBUG_SQUADS for the subscribe");
			// The first one comes with the next SQUADS write: within one cadence (20 ticks), as SIMLINK.md promises.
			test::require(c.extras.debugSquads.front().tick <= answerTick + 20,
				"DEBUG_SQUADS came later than the SQUADS cadence (tick " + std::to_string(c.extras.debugSquads.front().tick)
				+ " after " + std::to_string(answerTick) + ")");
			// Past the restart and one SQUADS cadence: the world the client sees is the new game's, player squad included.
			test::require(c.pump([&]() { return c.extras.lastSquadsTick > newGameTick + 21; }, 30000),
				"the world did not tick past the new game");
			const auto afterRestart = std::find_if(c.extras.debugSquads.begin(), c.extras.debugSquads.end(),
				[&](const Extras::DebugSquads& frame) { return frame.tick > newGameTick; });
			test::require(afterRestart != c.extras.debugSquads.end(), "no DEBUG_SQUADS after the new game's first tick");
			test::require(!afterRestart->ids.empty(), "DEBUG_SQUADS carries no squads after the new game");
			// A world squad has its template; the player squad has none (it is not a SQUAD_TEMPLATE) and is still there.
			const auto player = std::find(afterRestart->templates.begin(), afterRestart->templates.end(), 0u);
			test::require(std::count_if(afterRestart->templates.begin(), afterRestart->templates.end(),
				[](std::uint32_t tpl) { return tpl != 0; }) > 0, "no DEBUG_SQUADS row has a template");
			test::require(player != afterRestart->templates.end(), "the new game's player squad is not in DEBUG_SQUADS");
			const std::uint64_t playerSquad = afterRestart->ids[static_cast<std::size_t>(player - afterRestart->templates.begin())];
			std::vector<std::uint8_t> query{static_cast<std::uint8_t>(ksim::debugview::QueryKind::Squad)};
			put<std::uint64_t>(query, playerSquad);
			c.extras.debugTrees.clear();
			c.send(rawFrame(ksim::debugview::kDebugQuery, query));
			test::require(c.pump([&]() { return !c.extras.debugTrees.empty(); }, 15000),
				"no DEBUG_TREE for the player squad");
			test::require(c.extras.debugTrees.front().id == playerSquad && c.extras.debugTrees.front().rows > 0,
				"the player squad's DEBUG_TREE has no rows");
			// A re-subscribe (off, then on again) is answered again.
			c.send(rawFrame(ksim::debugview::kDebugSubscribe, {0}));
			c.send(subscribe);
			test::require(c.pump([&]() { return c.extras.debugInfos.size() >= 2; }, 15000), "a re-subscribe is not answered");
			test::require(c.extras.debugInfos.back().enabled == ksim::debugview::kEnabled, "the re-subscribe answered disabled");
			test::require(c.dec.error().empty(), "a debug frame was a protocol error");
#else
			std::vector<std::uint8_t> hello = helloFrame(fresh.context.dataHash);
			hello.insert(hello.end(), subscribe.begin(), subscribe.end());
			hello.insert(hello.end(), view.begin(), view.end());
			c.send(hello);
			test::require(c.pump([&]() { return c.snapshots >= 2; }, 15000), "the session died on a debug frame");
			test::require(c.extras.debugInfos.empty() && c.extras.debugSquads.empty() && c.extras.debugTrees.empty(),
				"a KSIM_DEBUG_VIEW=OFF server sent debug frames");
#endif
			// The WEATHER wind tail (SIMLINK "WEATHER") follows the session's camera through a restart: the set_view that rode with
			// HELLO, no second one, and the pair is still there after the new game.
			{
				std::string group;
				freshServer.call([&]() { group = fresh.sim->weather()->groupAt(targetX, targetZ); });
				test::require(!group.empty(), "the view target lies in no weather group");
				test::require(c.pump([&]()
				{
					for (const Extras::WeatherWind& wind : c.extras.winds)
					{
						if (wind.present && wind.tick > newGameTick)
							return true;
					}
					return false;
				}, 15000), "the wind tail did not survive the new game");
				// Order 84 new_world: back to the pristine mark, which has no ViewFocus of its own. The session's camera must
				// restore it, and the tail must stay on the frames sent after the reset without another set_view.
				c.allowTickRewind = true;
				c.send(rawOrder(2003, 84, {}));
				test::require(c.pump([&]() { return c.find(9, 2003) != nullptr; }, 15000), "new_world not acknowledged");
				const std::size_t afterAck = c.extras.winds.size();
				test::require(c.pump([&]()
				{
					for (std::size_t i = afterAck; i < c.extras.winds.size(); ++i)
					{
						if (c.extras.winds[i].present)
							return true;
					}
					return false;
				}, 15000), "the wind tail did not survive a new world");
				test::require(c.pump([&]()
				{
					bool restored = false;
					freshServer.call([&]()
					{
						const ksim::state::ViewFocus* focus = fresh.sim->world().ecs().get<ksim::state::ViewFocus>();
						restored = focus != nullptr && focus->x == static_cast<double>(targetX) && focus->z == static_cast<double>(targetZ);
					});
					return restored;
				}, 15000), "the world's ViewFocus was not re-applied after a new world");
				c.allowTickRewind = false;
			}
		}

		// ---- the debug view does not change the run ----
		// SIMLINK.md "DEBUG VIEW": no state, RNG stream or tick changes, so a run with and without a debug client is identical. Two
		// worlds from the same save and seed run the same ticks with squad AI on; the second one also has a subscribed debug client
		// and the decision sink, which must fire (the sink is the only sim-facing hook). The state hashes must be equal.
#if KSIM_DEBUG_VIEW
		{
			constexpr std::uint64_t kTicks = 120;
			World plain = makeWorld(order, data, gd, nav);
			World viewed = makeWorld(order, data, gd, nav);
			std::size_t decisions = 0;
			const auto sink = ksim::debugview::decisionSink();
			for (World* w : {&plain, &viewed})
			{
				ksim::AiSystem::Config ai;
				ai.registry = std::make_shared<ksim::BehaviourRegistry>(w->sim->world(), ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
				ai.navigator = nav;
				if (w == &viewed)
				{
					ai.decision = [sink, &decisions](std::uint64_t squad, const ksim::GoalChoice& chosen,
						const std::vector<ksim::GoalChoice>& top)
					{
						++decisions;
						sink(squad, chosen, top);
					};
				}
				w->sim->enableAi(std::move(ai));
			}
			viewed.context.debugView = true;
			ks::Session session(*viewed.sim, viewed.context);
			session.setDebugPeer(true);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter writer(bytes);
			session.begin(writer);
			const std::vector<std::uint8_t> hello = helloFrame(viewed.context.dataHash);
			test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, writer), "the debug session's hello was refused");
			session.sendTick(writer, true);
			const std::uint8_t subscribe = 1;
			test::require(session.onFrame(ksim::debugview::kDebugSubscribe, &subscribe, 1, writer), "the debug subscribe was refused");
			bytes.clear();
			for (std::uint64_t t = 0; t < kTicks; ++t)
			{
				plain.sim->tick();
				viewed.sim->tick();
				session.sendTick(writer, false);
				bytes.clear();
			}
			test::require(decisions > 0, "the decision sink never fired");
			test::require(viewed.sim->stateHash() == plain.sim->stateHash(), "a debug client changed the run (state hash)");
			std::cout << "debug view: " << decisions << " decisions, hash " << std::hex << plain.sim->stateHash() << std::dec << "\n";
		}
#endif

		// ---- the playtest scenario wire: list, load, the world swap, the run's metrics (task CE; SIMLINK.md SCENARIO) ----
		{
			World w = makeScenarioWorld(order, data, gd, nav);
			w.context.scenariosDir = KSIM_SCENARIO_FIXTURE_DIR;
			std::promise<int> portPromise;
			ks::ServerOptions scenarioOptions;
			scenarioOptions.onListening = [&portPromise](int listening) { portPromise.set_value(listening); };
			ks::SimLinkServer scenarioServer(*w.sim, w.context, scenarioOptions);
			scenarioServer.listen();
			const int scenarioPort = portPromise.get_future().get();
			std::atomic<bool> scenarioStop{false};
			std::thread scenarioThread([&]() { scenarioServer.run(scenarioStop); });
			const struct ScenarioJoiner
			{
				std::atomic<bool>& stop;
				std::thread& thread;
				~ScenarioJoiner()
				{
					stop.store(true);
					if (thread.joinable())
						thread.join();
				}
			} scenarioJoiner{scenarioStop, scenarioThread};
			Client c(scenarioPort);
			c.send(helloFrame(w.context.dataHash));
			test::require(c.pump([&]() { return c.snapshots >= 1; }, 5000), "scenario: no initial snapshot");
			// 1. SCENARIO_LIST_REQ -> SCENARIO_LIST: the fixture directory's one header-carrying file.
			{
				std::vector<std::uint8_t> payload;
				put<std::uint32_t>(payload, 9001);
				c.send(rawFrame(ks::kScenarioListReq, payload));
			}
			// pump() also returns on an ERROR frame: a refusal must not read as an answer.
			test::require(c.pump([&]() { return !c.extras.scenarioLists.empty(); }, 5000) && c.dec.error().empty(),
				"SCENARIO_LIST_REQ got no SCENARIO_LIST: " + c.dec.error());
			// Copies: a later pump appends to these vectors and may reallocate them.
			const Extras::ScenarioList list = c.extras.scenarioLists.back();
			test::require(list.seq == 9001, "SCENARIO_LIST clientSeq");
			test::require(list.rows.size() == 1 && list.rows[0].name == "wire", "SCENARIO_LIST: the fixture scenario");
			const Extras::ScenarioRow row = list.rows[0];
			test::require(row.title == "Wire scenario" && !row.setup.empty() && !row.lookFor.empty(), "SCENARIO_LIST: the header's text");
			test::require(row.fileHash != 0, "SCENARIO_LIST: the file hash");
			test::require(row.bead == "kenshi-test.1", "SCENARIO_LIST: the bead tail");
			test::require(row.metrics.size() == 3 && row.metrics[0] == "targeted_player" && row.metrics[2] == "player_downed_s",
				"SCENARIO_LIST: the header's metrics, in order");
			// 2. SCENARIO_LOAD -> event 9, SCENARIO_STARTED, then the load's full resend.
			const std::uint64_t beforeTick = lookAt(scenarioServer, w).tick;
			{
				std::vector<std::uint8_t> payload;
				put<std::uint32_t>(payload, 9002);
				const std::string name = "wire";
				put<std::uint16_t>(payload, static_cast<std::uint16_t>(name.size()));
				payload.insert(payload.end(), name.begin(), name.end());
				put<std::uint32_t>(payload, 11);
				c.send(rawFrame(ks::kScenarioLoad, payload));
			}
			test::require(c.pump([&]() { return !c.extras.scenarioStarted.empty(); }, 15000), "SCENARIO_LOAD got no SCENARIO_STARTED");
			const Extras::ScenarioStarted started = c.extras.scenarioStarted.back();
			test::require(started.seq == 9002 && started.status == 0, "SCENARIO_STARTED refused: " + started.error);
			test::require(started.name == "wire" && started.fileHash == row.fileHash, "SCENARIO_STARTED: name and file hash");
			test::require(started.seed == 11, "SCENARIO_STARTED: the seed the client asked for");
			test::require(!started.simHash.empty() && started.simHash != "unknown", "SCENARIO_STARTED: the build's git hash");
			test::require(started.startGameTime > 0.0, "SCENARIO_STARTED: the scenario's clock");
			// The 2026-11-08 tail: the fixture's `source centre=-50978.8,2932.48`, so a spectator camera can frame the scenario.
			test::require(started.hasCentre, "SCENARIO_STARTED carried no centre tail");
			test::require(std::fabs(started.centreX - -50978.8f) < 0.05f && std::fabs(started.centreZ - 2932.48f) < 0.05f,
				"SCENARIO_STARTED: the scenario's centre (" + std::to_string(started.centreX) + ", " + std::to_string(started.centreZ) + ")");
			test::require(started.bead == "kenshi-test.1", "SCENARIO_STARTED: the bead tail");
			test::require(c.find(9, 9002) != nullptr, "SCENARIO_LOAD was not acked with event 9");
			test::require(lookAt(scenarioServer, w).tick >= beforeTick, "the clock went backwards");
			// The world swap: the file's player character exists and is player-controlled. Read from the wire: Simulation::playerSquads()
			// walks the ECS, which the server thread is ticking (a main-thread call here crashed or saw a half-built world, k16.15.4).
			test::require(c.pump([&]() { return c.dec.playerSquads.valid && c.dec.playerSquads.squads.size() == 1
				&& c.dec.playerSquads.squads[0].members.size() == 1; }, 5000), "the loaded scenario has no player faction");
			// 3. a few seconds of run, then SCENARIO_END -> SCENARIO_METRICS, every metric the header names.
			const std::uint64_t runStart = lookAt(scenarioServer, w).tick;
			test::require(c.pump([&]() { return lookAt(scenarioServer, w).tick > runStart + 100; }, 15000), "the loaded scenario did not run");
			{
				std::vector<std::uint8_t> payload;
				put<std::uint32_t>(payload, 9003);
				c.send(rawFrame(ks::kScenarioEnd, payload));
			}
			test::require(c.pump([&]() { return !c.extras.scenarioMetrics.empty(); }, 5000), "SCENARIO_END got no SCENARIO_METRICS");
			const Extras::ScenarioMetrics metrics = c.extras.scenarioMetrics.back();
			test::require(metrics.seq == 9003 && metrics.name == "wire", "SCENARIO_METRICS: the run");
			test::require(metrics.elapsedGameS > 0.0, "SCENARIO_METRICS: elapsedGameS");
			test::require(metrics.metrics.size() == row.metrics.size(), "SCENARIO_METRICS: one row per header metric");
			for (std::size_t i = 0; i < row.metrics.size(); ++i)
				test::require(metrics.metrics[i].name == row.metrics[i], "SCENARIO_METRICS: the header's order");
			// A second scenario_end repeats the run's numbers (the world is still the scenario's).
			{
				std::vector<std::uint8_t> payload;
				put<std::uint32_t>(payload, 9004);
				c.send(rawFrame(ks::kScenarioEnd, payload));
			}
			test::require(c.pump([&]() { return c.extras.scenarioMetrics.size() >= 2; }, 5000), "a second SCENARIO_END got no answer");
			test::require(c.extras.scenarioMetrics.back().metrics.size() == row.metrics.size(), "the second SCENARIO_METRICS lost metrics");
			// A name the server does not carry is a refusal with a reason, not a dropped session.
			{
				std::vector<std::uint8_t> payload;
				put<std::uint32_t>(payload, 9005);
				const std::string name = "no_such_scenario";
				put<std::uint16_t>(payload, static_cast<std::uint16_t>(name.size()));
				payload.insert(payload.end(), name.begin(), name.end());
				put<std::uint32_t>(payload, 0);
				c.send(rawFrame(ks::kScenarioLoad, payload));
			}
			test::require(c.pump([&]() { return c.extras.scenarioStarted.size() >= 2; }, 5000), "an unknown scenario got no SCENARIO_STARTED");
			test::require(c.extras.scenarioStarted.back().status == 1 && !c.extras.scenarioStarted.back().error.empty(),
				"an unknown scenario was not refused with a reason");
			test::require(c.extras.scenarioStarted.back().hasCentre && std::isnan(c.extras.scenarioStarted.back().centreX)
					&& std::isnan(c.extras.scenarioStarted.back().centreZ),
				"a refusal's centre tail is not NaN");
			test::require(c.extras.scenarioStarted.back().bead.empty(), "a refusal's bead tail is empty");
			test::require(!c.closed && c.dec.error().empty(), "the scenario wire ended the session");
			std::cout << "scenario wire: list, load (seed 11), started (hash " << started.simHash << "), end -> "
				<< metrics.metrics.size() << " metrics (" << metrics.elapsedGameS << " game s)\n";
		}

		// ---- the recorded sample stream decodes ----
		{
			std::ifstream in(KSIM_SAMPLE_BIN, std::ios::binary);
			test::require(static_cast<bool>(in), std::string("cannot read ") + KSIM_SAMPLE_BIN);
			const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			simlink::SimLinkDecoder decoder;
			Extras sampleExtras;
			sampleExtras.recordedSample = true;
			const auto* base = reinterpret_cast<const std::uint8_t*>(bytes.data());
			std::size_t fed = 0;
			while (fed < bytes.size())
			{
				test::require(bytes.size() - fed >= 6, "sample stream: truncated frame header");
				std::uint32_t len;
				std::uint16_t type;
				std::memcpy(&len, base + fed, 4);
				std::memcpy(&type, base + fed + 4, 2);
				test::require(bytes.size() - fed >= 4u + len, "sample stream: truncated frame");
				if (intercept(type, base + fed + 6, len - 2, sampleExtras))
					test::require(decoder.feed(base + fed, 4u + len), "sample stream: " + decoder.error());
				fed += 4u + len;
			}
			test::require(!sampleExtras.routes.empty(), "sample stream has a CHAR_ROUTE");
			test::require(decoder.helloSeen() && decoder.chars.size() == 2 && decoder.lastCompleteTick > 100,
				"sample stream content");
			bool applied = false;
			bool hit = false;
			for (const simlink::Event& e : decoder.events)
			{
				applied = applied || e.kind == 9;
				hit = hit || e.kind == 1;
			}
			test::require(applied && hit, "sample stream has an applied order and a hit");
		}

		// ---- fuzz: random, truncated, oversized and NaN frames straight into sessions ----
		// The server is stopped, so the sim is ours. A frame that is not accepted (ERROR or
		// event 10) must leave the sim, the order log and the shared pause/speed untouched.
		{
			const std::uint64_t id = player;
			std::vector<std::vector<std::uint8_t>> bases;
			const auto base = [&](std::uint16_t kind, const std::function<void(std::vector<std::uint8_t>&)>& body)
			{
				bases.push_back(rawOrder(7, kind, body));
			};
			const auto ids1 = [&](std::vector<std::uint8_t>& v) { put<std::uint16_t>(v, 1); put<std::uint64_t>(v, id); };
			base(1, [&](auto& v) { ids1(v); put<float>(v, 100.0f); put<float>(v, 200.0f); put<std::uint8_t>(v, 0); });
			base(2, [&](auto& v) { ids1(v); put<std::uint64_t>(v, enemy); });
			base(4, [&](auto& v) { ids1(v); put<std::uint8_t>(v, 1); });
			base(10, [&](auto& v) { ids1(v); put<std::uint8_t>(v, 0); put<std::uint8_t>(v, 1); });
			base(40, [](auto& v) { put<std::uint8_t>(v, 1); });
			base(41, [](auto& v) { put<float>(v, 4.0f); });
			base(50, [](auto& v) { put<float>(v, 0.0f); put<float>(v, 0.0f); put<float>(v, 500.0f); });
			base(51, [](auto& v) { put<float>(v, 0.0f); put<float>(v, 0.0f); });
			base(3, [&](auto& v) { ids1(v); put<std::uint64_t>(v, enemy); });
			base(20, [&](auto& v) { put<std::uint64_t>(v, id); put<std::uint8_t>(v, 1); put<float>(v, 0.0f); });

			std::mt19937 rng(20261031);
			const auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
			const float evil[] = {std::nanf(""), INFINITY, -INFINITY, 1e30f, -1e30f, 3.0e5f, -1.0f, 0.0f};
			std::size_t accepted = 0, errors = 0, rejected = 0;
			for (int round = 0; round < 800; ++round) // each round hashes the world, so keep the count modest
			{
				std::vector<std::uint8_t> frame = bases[pick(bases.size())];
				switch (pick(8))
				{
				case 0: // truncate
					frame.resize(6 + pick(frame.size() - 5));
					break;
				case 1: // trailing junk
					for (std::size_t i = pick(40) + 1; i > 0; --i)
						frame.push_back(static_cast<std::uint8_t>(rng()));
					break;
				case 2: // overwrite a float-sized window with something evil
					if (frame.size() >= 10)
						std::memcpy(&frame[6 + pick(frame.size() - 9)], &evil[pick(8)], 4);
					break;
				case 3: // the id count
					if (frame.size() >= 14)
					{
						const std::uint16_t n = static_cast<std::uint16_t>(rng());
						std::memcpy(&frame[12], &n, 2);
					}
					break;
				case 4: // flip bytes
					for (std::size_t i = pick(6) + 1; i > 0; --i)
						frame[6 + pick(frame.size() - 6)] ^= static_cast<std::uint8_t>(1u << pick(8));
					break;
				case 5: // a random frame type and payload
					for (std::size_t i = 4; i < frame.size(); ++i)
						frame[i] = static_cast<std::uint8_t>(rng());
					break;
				case 6: // oversized junk payload
					frame.resize(6 + 70000, 0xAB);
					break;
				default: // unchanged
					break;
				}
				const std::uint16_t type = round % 11 == 0 ? static_cast<std::uint16_t>(rng()) : (frame.size() >= 6 ? simlink::kOrder : 0);

				ks::Session session(*live.sim, live.context);
				std::vector<std::uint8_t> hi;
				ks::BinaryWriter hw(hi);
				session.begin(hw);
				const auto hello = helloFrame(live.context.dataHash);
				hi.clear();
				session.onFrame(simlink::kHello, hello.data() + 6, hello.size() - 6, hw);
				test::require(session.ready(), "fuzz session not ready");

				const std::uint64_t hashBefore = live.sim->stateHash();
				const std::size_t logBefore = live.context.orderLog.size();
				const bool pausedBefore = live.context.paused;
				const float speedBefore = live.context.speed;
				std::vector<std::uint8_t> reply;
				ks::BinaryWriter rw(reply);
				bool alive = true;
				try
				{
					alive = session.onFrame(type, frame.data() + 6, frame.size() - 6, rw);
				} catch (...) {
					test::require(false, "an exception escaped Session::onFrame");
				}
				// What came back: ERROR, or event 9 (accepted) / 10 (rejected).
				bool sawError = false, sawApplied = false;
				for (std::size_t at = 0; at + 6 <= reply.size();)
				{
					std::uint32_t len;
					std::uint16_t t;
					std::memcpy(&len, &reply[at], 4);
					std::memcpy(&t, &reply[at + 4], 2);
					sawError = sawError || t == simlink::kError;
					if (t == simlink::kEvent && at + 16 <= reply.size())
					{
						std::uint16_t kind;
						std::memcpy(&kind, &reply[at + 14], 2);
						sawApplied = sawApplied || kind == 9;
					}
					at += 4 + len;
				}
				if (!alive)
				{
					test::require(sawError, "a session ended without an ERROR frame");
					++errors;
				}
				if (sawApplied)
				{
					++accepted;
					// Whatever was accepted must have been wholly valid: the order log grew by what it enqueued.
					test::require(alive && !sawError, "accepted and failed in one frame");
					// Pause / speed / interest are real effects of valid frames: undo the ones the shared context saw.
					live.context.paused = pausedBefore;
					live.context.speed = speedBefore;
				} else {
					++rejected;
					test::require(live.sim->stateHash() == hashBefore, "a rejected frame changed the sim");
					test::require(live.context.orderLog.size() == logBefore, "a rejected frame reached the order log");
					test::require(live.context.paused == pausedBefore && live.context.speed == speedBefore,
						"a rejected frame changed pause/speed");
				}
			}
			test::require(accepted > 20 && errors > 100 && rejected > 200, "fuzz did not exercise both paths");
			std::cout << "simlink fuzz: " << accepted << " accepted, " << errors << " errors, " << rejected << " rejected\n";
		}

		std::cout << "simlink: " << c.checkedFrames << " frames checked, " << c.snapshots << " snapshots, "
			<< simOrders << " sim orders replayed to tick " << endTick << ", hash " << std::hex << liveHash << std::dec
			<< ", " << std::chrono::duration<double>(Wall::now() - started).count() << " s\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
