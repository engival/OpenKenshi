// ksim_fight: runs the fight of a scenario file (docs/reference/SCENARIO_IMPORT.md) N times with different seeds and writes a per-fight
// summary JSON per run plus a distribution table over the runs, to compare with the original's captures (kinspect's fight fixtures).
//
//   ksim_fight SCENARIO [--navworld DIR (the bake; default: the runtime mesh)] [--runs N] [--seed S] [--out DIR] [--max-seconds T] [--no-trigger] [--optional] [--check] [--mods FILE]
//
// Per run (seed S + i): the default world (Simulation::newGame, seed), the file applied (scenario::apply), then the trigger: at tick 0 the first
// player-controlled character gets an attack order on the nearest character of another team (the existing attack order; the wire is not
// involved), and the sim runs at full speed until one side has nobody standing (dead, knocked out or down), plus 3 s, or T game seconds (default 300).
// --no-trigger: nobody is ordered (the AI-less characters fight only what notices them). --optional: exit 77 with a message when the
// scenario file does not exist (the converted real fixtures are game-derived and never in the repo).
//
// Output in OUT (default `.`): rN_summary.json, distribution.json, distribution.md. Format: docs/reference/SCENARIO_IMPORT.md "Summary format".

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/spawn.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Outcomes
{
	int through = 0;
	int blocked = 0;
	int dodged = 0;
};

struct PartDamage
{
	int hits = 0;
	double cut = 0.0;
	double blunt = 0.0;
	double total = 0.0; // dmg[0] + dmg[1] + dmg[2]
};

struct CharLog
{
	std::uint64_t id = 0;
	std::string name;
	int team = 0;
	bool player = false;
	std::string squad;
	std::vector<double> ko;
	std::optional<double> down; // first tick the fighter was down (a leg or torso part collapsed)
	std::optional<double> death;
	double dmgLanded = 0.0; // dmg[0] + dmg[1] of the hits this character landed (after armour and toughness; kinspect's `hit` dmg is before armour)
	std::vector<std::string> partNames;
	std::vector<double> partStart;
	std::vector<double> partEnd;
	double bloodStart = 0.0;
	double bloodEnd = 0.0;
	Outcomes made; // this character's swings, by what the defender did
	Outcomes received; // swings at this character, by what it did (blocked / dodged = its blocks and dodges)
	std::set<std::uint64_t> attackers; // who swung at it
	std::map<std::string, PartDamage> parts; // damage that went through, per part
	std::vector<std::array<double, 4>> path; // t, x, y, z at 1 Hz
	double startX = 0.0;
	double startZ = 0.0;
	bool standingAtEnd = true;
	bool attackerSide = false; // the file ordered someone of its squad to attack (KSCEN 2 orders): kinspect's "side A" of fight_joiners.py
	std::optional<double> join; // first second a character of the other side had this attacker-side character as its target
};

struct RunLog
{
	int run = 0;
	std::uint64_t seed = 0;
	std::string winner; // "player", "enemy" or "none"
	double duration = 0.0; // seconds from the trigger to the moment one side was down
	double logLength = 0.0; // seconds simulated (the settle plus the 3 s linger)
	double hours0 = 0.0; // game clock at the trigger, hours since day 0 (kinspect: day * 24 + hour + minute / 60)
	GameTime clockStart;
	std::vector<CharLog> chars;
	std::set<std::string> sideA; // squad names of the attacker side
	// Task AQ's cost: the melee gates' navmesh route queries and cache hits over the run (the sim is fresh per run).
	std::uint64_t pathQueries = 0;
	std::uint64_t pathCacheHits = 0;
	std::uint64_t ticks = 0;
};

std::string jsonString(const std::string& s)
{
	std::string out = "\"";
	for (const char c : s)
	{
		if (c == '"' || c == '\\')
			out += std::string("\\") + c;
		else if (static_cast<unsigned char>(c) < 0x20)
			out += ' ';
		else
			out += c;
	}
	return out + "\"";
}

std::string num(double v)
{
	char buf[48];
	std::snprintf(buf, sizeof buf, "%.4g", v);
	return buf;
}

std::string fixed(double v, int digits)
{
	char buf[48];
	std::snprintf(buf, sizeof buf, "%.*f", digits, v);
	return buf;
}

// The schema of kinspect's tools/fight_summary.py (findings/fight_fixtures/*/runs/rN_summary.json), so one tool reads both: top-level `duration_s`, `hours0`,
// `squads`, `chars` keyed by id with ko / down / dead (seconds from the trigger), hits_landed / hits_taken (swings: their `hit` event fires for blocked and dodged
// swings too), dmg_landed, def / att {hit, block, dodge}, pos [t, x, y, z] at 1 Hz, part_loss, blood [first, last]. Ours add: run, seed, winner, fight_s,
// game_clock_start, and per char `side`, `team`, `standing_at_end`, `ko_times`, `attackers` (distinct characters that swung at it), `damage_by_part`.
// Their `duration_s` is the log length; ours is the simulated length (the settle plus 3 s), `fight_s` the settle alone.
std::string summaryJson(const RunLog& run)
{
	std::ostringstream o;
	o << "{\n \"run\": " << run.run << ", \"seed\": " << run.seed << ", \"winner\": " << jsonString(run.winner) << ", \"fight_s\": " << fixed(run.duration, 2)
		<< ", \"duration_s\": " << fixed(run.logLength, 2) << ", \"hours0\": " << fixed(run.hours0, 4) << ",\n \"game_clock_start\": {\"day\": " << run.clockStart.day
		<< ", \"hour\": " << run.clockStart.hour << ", \"minute\": " << run.clockStart.minute << "},\n \"squads\": {";
	std::map<std::string, std::array<int, 4>> squads; // n, ko, dead, standing
	for (const CharLog& c : run.chars)
	{
		std::array<int, 4>& q = squads[c.squad.empty() ? "?" : c.squad];
		++q[0];
		++(c.death ? q[2] : !c.ko.empty() || c.down ? q[1] : q[3]);
	}
	bool first = true;
	for (const auto& [name, q] : squads)
	{
		o << (first ? "\n  " : ",\n  ") << jsonString(name) << ": {\"n\": " << q[0] << ", \"ko\": " << q[1] << ", \"dead\": " << q[2] << ", \"standing\": " << q[3] << "}";
		first = false;
	}
	o << "\n },\n \"chars\": {";
	first = true;
	const auto time = [](const std::optional<double>& t) { return t ? fixed(*t, 2) : std::string("null"); };
	for (const CharLog& c : run.chars)
	{
		o << (first ? "\n" : ",\n");
		first = false;
		o << "  \"" << c.id << "\": {\"name\": " << jsonString(c.name) << ", \"squad\": " << jsonString(c.squad) << ", \"player\": " << (c.player ? "true" : "false")
			<< ", \"side\": " << (c.player ? "\"player\"" : "\"enemy\"") << ", \"attacker_side\": " << (c.attackerSide ? "true" : "false")
			<< ", \"join\": " << time(c.join) << ", \"team\": " << c.team << ", \"standing_at_end\": " << (c.standingAtEnd ? "true" : "false")
			<< ",\n   \"ko\": " << time(c.ko.empty() ? std::nullopt : std::optional<double>(c.ko.front())) << ", \"down\": " << time(c.down) << ", \"dead\": " << time(c.death)
			<< ", \"ko_times\": [";
		for (std::size_t i = 0; i < c.ko.size(); ++i)
			o << (i ? "," : "") << fixed(c.ko[i], 2);
		const int landed = c.made.through + c.made.blocked + c.made.dodged;
		const int taken = c.received.through + c.received.blocked + c.received.dodged;
		o << "],\n   \"hits_landed\": " << landed << ", \"hits_taken\": " << taken << ", \"dmg_landed\": " << fixed(c.dmgLanded, 2)
			<< ", \"def\": {\"hit\": " << c.received.through << ", \"block\": " << c.received.blocked << ", \"dodge\": " << c.received.dodged
			<< "}, \"att\": {\"hit\": " << c.made.through << ", \"block\": " << c.made.blocked << ", \"dodge\": " << c.made.dodged << "}, \"attackers\": " << c.attackers.size()
			<< ",\n   \"part_loss\": {";
		bool firstPart = true;
		for (std::size_t i = 0; i < c.partNames.size() && i < c.partStart.size() && i < c.partEnd.size(); ++i)
		{
			const double loss = c.partStart[i] - c.partEnd[i];
			if (loss > 0.01)
			{
				o << (firstPart ? "" : ", ") << jsonString(c.partNames[i]) << ": " << fixed(loss, 2);
				firstPart = false;
			}
		}
		o << "}, \"blood\": [" << fixed(c.bloodStart, 2) << ", " << fixed(c.bloodEnd, 2) << "],\n   \"damage_by_part\": {";
		firstPart = true;
		for (const auto& [part, d] : c.parts)
		{
			o << (firstPart ? "" : ", ") << jsonString(part) << ": {\"hits\": " << d.hits << ", \"cut\": " << fixed(d.cut, 2) << ", \"blunt\": " << fixed(d.blunt, 2)
				<< ", \"total\": " << fixed(d.total, 2) << "}";
			firstPart = false;
		}
		o << "},\n   \"pos\": [";
		for (std::size_t i = 0; i < c.path.size(); ++i)
			o << (i ? "," : "") << "[" << num(c.path[i][0]) << "," << fixed(c.path[i][1], 1) << "," << fixed(c.path[i][2], 1) << "," << fixed(c.path[i][3], 1) << "]";
		o << "]}";
	}
	o << "\n }\n}\n";
	return o.str();
}

// The crowd view in the schema of kinspect's tools/fight_joiners.py output (rN_joiners.json): who of the other side first targeted a side-A
// character, and when, with how everyone ended. Ours lack `faction` and `goals` (the sim has no goal numbers).
std::string joinersJson(const RunLog& run)
{
	const auto end = [](const CharLog& c) { return c.death ? "dead" : !c.standingAtEnd ? "down" : "standing"; };
	const auto everDown = [](const CharLog& c) { return c.ko.empty() ? c.down : std::optional<double>(c.ko.front()); };
	std::map<std::string, int> sideA = {{"n", 0}, {"ever_down", 0}, {"dead", 0}, {"down_at_end", 0}, {"standing", 0}};
	std::map<std::string, int> joined = sideA;
	std::vector<const CharLog*> joiners;
	for (const CharLog& c : run.chars)
	{
		if (c.attackerSide || c.join)
		{
			std::map<std::string, int>& side = c.attackerSide ? sideA : joined;
			++side["n"];
			side["ever_down"] += everDown(c) ? 1 : 0;
			++side[c.death ? "dead" : !c.standingAtEnd ? "down_at_end" : "standing"];
		}
		if (c.join && !c.attackerSide)
			joiners.push_back(&c);
	}
	std::sort(joiners.begin(), joiners.end(), [](const CharLog* a, const CharLog* b) { return *a->join < *b->join; });
	std::ostringstream o;
	const auto sideJson = [&](const std::map<std::string, int>& m)
	{
		std::ostringstream t;
		t << "{\"n\": " << m.at("n") << ", \"ever_down\": " << m.at("ever_down") << ", \"dead\": " << m.at("dead") << ", \"down_at_end\": " << m.at("down_at_end")
			<< ", \"standing\": " << m.at("standing") << "}";
		return t.str();
	};
	o << "{\n \"log\": \"ksim_fight run " << run.run << " seed " << run.seed << "\", \"duration_s\": " << fixed(run.logLength, 2) << ", \"side_a\": [";
	bool first = true;
	for (const std::string& name : run.sideA)
	{
		o << (first ? "" : ", ") << jsonString(name);
		first = false;
	}
	o << "],\n \"sides\": {\"A\": " << sideJson(sideA) << ", \"B_joined\": " << sideJson(joined) << "},\n \"joiners\": [";
	first = true;
	for (const CharLog* c : joiners)
	{
		const std::optional<double> down = everDown(*c);
		o << (first ? "\n  " : ",\n  ") << "{\"id\": \"" << c->id << "\", \"name\": " << jsonString(c->name) << ", \"squad\": " << jsonString(c->squad)
			<< ", \"join_s\": " << fixed(*c->join, 2) << ", \"by\": \"target\", \"end\": \"" << end(*c) << "\", \"ever_down\": "
			<< (down ? fixed(*down, 2) : std::string("null")) << "}";
		first = false;
	}
	o << "\n ]}\n";
	return o.str();
}

double quantile(std::vector<double> v, double p)
{
	if (v.empty())
		return std::nan("");
	std::sort(v.begin(), v.end());
	const double k = static_cast<double>(v.size() - 1) * p;
	const std::size_t i = static_cast<std::size_t>(k);
	return i + 1 >= v.size() ? v[i] : v[i] + (v[i + 1] - v[i]) * (k - static_cast<double>(i));
}

std::string spread(const std::vector<double>& v, int digits = 1)
{
	if (v.empty())
		return "-";
	return fixed(quantile(v, 0.5), digits) + " [" + fixed(quantile(v, 0.0), digits) + ".." + fixed(quantile(v, 1.0), digits) + "] n=" + std::to_string(v.size());
}

struct Options
{
	std::string scenario;
	std::string navworld;
	std::string out = ".";
	std::string mods;
	int runs = 3;
	std::uint64_t seed = 1;
	double maxSeconds = 300.0;
	bool trigger = true;
	bool ai = true; // --no-ai: the characters are plain fighters (no squad AI, residents' routines or world carry), the first version of this tool
	bool optional = false;
	bool traceDeaths = false; // --trace-deaths: one DEATH line per death (blood, bleed rate, wounds, every part), stdout
	int fightStride = 1; // --fight-stride N / --idle-stride N: CombatSettings::fightStride / idleStride (arena strides, kenshi-k16.6.636)
	int idleStride = 1;
	double medicalClock = 0.0; // --medical-clock X: CombatSettings::medicalClock for this run (0 = the default)
	bool check = false; // --check: exit 1 unless every run really fought and settled
	bool traceAll = false; // --trace-all: --trace plus the state changes of every character
	bool relations = false; // --relations: the matrix of faction relations (what row thinks of column) among the file's squads, stdout, then exit
	bool traceTargets = false; // --trace-targets: one line per change of target of a character that is not on side A (who it picks, how far, its team), stdout
	bool trace = false; // --trace: one line per combat event of the first player character (and every KO with its part values) on stdout
};

int usage()
{
	std::cerr << "usage: ksim_fight SCENARIO [--navworld DIR (the bake; default: the runtime mesh)] [--runs N] [--seed S] [--out DIR] [--max-seconds T] [--no-trigger] [--no-ai] [--optional] [--check] [--mods FILE]\n";
	return 2;
}

RunLog runOne(const Options& options, const kworld::LoadOrder& order, std::shared_ptr<const CombatData>& combatData, const scenario::File& file, int index)
{
	const std::uint64_t seed = options.seed + static_cast<std::uint64_t>(index);
	SpawnOptions spawn;
	spawn.seed = seed;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
	const std::shared_ptr<const kdata::GameData> gameData = sim.gameData();
	sim.enableWeather(*gameData);
	if (combatData == nullptr)
		combatData = CombatData::build(*gameData, KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	settings.fightStride = options.fightStride;
	settings.idleStride = options.idleStride;
	if (options.medicalClock > 0.0)
		settings.medicalClock = options.medicalClock;
	sim.enableCombat(combatData, settings);
	std::vector<kworld::ZoneCell> hubZones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	}
	if (options.navworld.empty()) // the server's default: the shipped navtiles as the runtime mesh, cached per load order
		sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, hubZones, {}, gameData.get(),
			NativeCacheOptions{ {}, true, kworld::dataHash(kworld::resolveLoadOrder(KENSHI_DATA, options.mods)) }));
	else
		sim.enableNavigation(MeshNavigator::openWorld(options.navworld, hubZones, KENSHI_DATA));
	simlink::ServerContext context;
	context.gameData = gameData;
	const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, file, seed);
	if (index == 0)
		std::cerr << "ksim_fight: " << file.squads.size() << " squads in the file: " << placed.claimedSquads << " took over a default-world squad, " << placed.removedSquads
			<< " default-world squads removed, " << placed.orders << " attack orders\n";
	if (placed.players.empty() && placed.orders == 0)
		throw std::runtime_error("the scenario has no player-controlled character to give the attack order to");
	if (options.relations)
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
		std::vector<std::pair<std::string, flecs::entity>> factions;
		std::set<std::string> seen;
		for (const scenario::Squad& q : file.squads)
		{
			if (q.faction.empty() || !seen.insert(q.faction).second)
				continue;
			ecs.each([&](flecs::entity entity, const state::FactionState& state)
			{
				if (state.templateSid == q.faction)
					factions.emplace_back(gameData->find(q.faction)->name, entity);
			});
		}
		std::printf("faction relations (row thinks of column; hostile at or below %.0f)\n%-24s", FactionBook::kHostile, "");
		for (const auto& column : factions)
			std::printf(" %8.8s", column.first.c_str());
		std::printf("\n");
		for (const auto& row : factions)
		{
			std::printf("%-24s", row.first.c_str());
			for (const auto& column : factions)
				std::printf(" %8.0f", factionRelation(*gameData, row.second, column.second));
			std::printf("\n");
		}
		std::exit(0);
	}
	if (options.ai)
	{
		// The normal world AI, as ksim_server with --bake: the squads of the file (scenario::apply made them world squads) run their routines.
		ServerAiOptions aiOptions;
		aiOptions.registryPath = KSIM_AI_TABLE;
		aiOptions.bake = options.navworld; // empty: the AI routes over the simulation's own (runtime) mesh
		aiOptions.regionX0 = 4; aiOptions.regionZ0 = 7; aiOptions.regionX1 = 6; aiOptions.regionZ1 = 9;
		enableServerAi(sim, aiOptions);
	}

	RunLog log;
	log.run = index + 1;
	log.seed = seed;
	log.clockStart = gameTimeAt(sim.clockTick());
	log.hours0 = static_cast<double>(log.clockStart.day) * 24.0 + log.clockStart.hour + log.clockStart.minute / 60.0;
	std::map<std::uint64_t, std::size_t> slot;
	std::map<std::uint64_t, std::string> squadNameOf;
	for (const scenario::Char& c : file.chars)
	{
		for (const scenario::Squad& q : file.squads)
		{
			if (q.id == c.squad)
				squadNameOf[placed.idOf.at(c.id)] = q.name;
		}
	}
	const auto addChars = [&](const std::vector<std::uint64_t>& ids)
	{
		for (const std::uint64_t id : ids)
		{
			const state::Fighter& f = *sim.combat()->find(id);
			CharLog c;
			c.id = id;
			c.name = f.name;
			c.team = f.team;
			c.player = f.team == 0;
			c.startX = f.x;
			c.startZ = f.z;
			c.bloodStart = f.blood;
			for (const state::FighterPart& part : f.parts)
			{
				c.partNames.push_back(part.name);
				c.partStart.push_back(part.health);
			}
			const auto squadName = squadNameOf.find(id);
			if (squadName != squadNameOf.end())
				c.squad = squadName->second;
			slot[id] = log.chars.size();
			log.chars.push_back(std::move(c));
		}
	};
	addChars(placed.players);
	addChars(placed.others);
	// Side A (kinspect's fight_joiners.py): the squads of the characters the file orders to attack. Their outsiders "join" when they first target one.
	for (const std::uint64_t id : placed.attackers)
		log.sideA.insert(log.chars[slot.at(id)].squad);
	for (CharLog& c : log.chars)
		c.attackerSide = log.sideA.count(c.squad) != 0;

	const std::uint64_t startTick = sim.clockTick();
	const double t0 = static_cast<double>(startTick) * Simulation::kTickSeconds; // arena time of the first tick is (now - 1) * dt
	const std::uint64_t tracedId = placed.players.front();
	sim.setCombatObserver([&](const CombatEvent& e)
	{
		const auto who = slot.find(e.who);
		if (who == slot.end())
			return;
		CharLog& self = log.chars[who->second];
		const double t = e.time - t0;
		const auto other = slot.find(e.other);
		if (options.trace && (e.who == tracedId || e.other == tracedId || e.kind == CombatEvent::Kind::Knockout
			|| (options.traceAll && e.kind == CombatEvent::Kind::State)))
		{
			static const char* const kinds[] = {"tech", "pickdef", "resolve", "hit", "xp", "engage", "slot+", "slot-", "stumble", "KO", "death",
				"state", "poscorr", "driveblk", "noticed", "escroll", "giveup"};
			std::printf("T r%d %7.2f %-8s %s%s -> %s code=%d/%d tech=%d part=%s", index + 1, t, kinds[static_cast<int>(e.kind)], who->second == 0 ? "*" : "",
				self.name.c_str(), other != slot.end() ? log.chars[other->second].name.c_str() : "-", e.code, e.code2, e.technique, e.part.c_str());
			std::printf(" #%zu", who->second); // the character's index in the scenario (the bandits share a name)
			if (e.kind == CombatEvent::Kind::Knockout)
			{
				const state::Fighter& f = *sim.combat()->find(e.who);
				for (const state::FighterPart& part : f.parts)
					std::printf(" [%s %.1f stun %.1f]", part.name.c_str(), part.health, part.stun);
			}
			if (e.technique >= 0)
				std::printf(" (%s)", combatData->techniques[static_cast<std::size_t>(e.technique)].name.c_str());
			if (e.kind == CombatEvent::Kind::TechniqueStart)
			{
				std::printf(" d=%.1f speed=%.3f", e.a, e.b);
				if (e.who == tracedId)
				{
					const state::Fighter& me = *sim.combat()->find(tracedId);
					std::vector<double> near;
					for (const CharLog& c : log.chars)
					{
						const state::Fighter& o = *sim.combat()->find(c.id);
						if (c.id != tracedId && !o.dead && !o.unconscious && std::hypot(o.x - me.x, o.z - me.z) < 60.0)
							near.push_back(std::hypot(o.x - me.x, o.z - me.z));
					}
					std::sort(near.begin(), near.end());
					std::printf(" near:");
					for (const double d : near)
						std::printf(" %.0f", d);
				}
			}
			std::printf("\n");
		}
		if (e.kind == CombatEvent::Kind::Death && options.traceDeaths)
		{
			const state::Fighter& f = *sim.combat()->find(e.who);
			std::printf("DEATH r%d %7.2f %s #%zu blood %.1f bleed %.4f hunger %.3f wounds %zu", index + 1, t, self.name.c_str(), who->second, f.blood, f.bleeding, f.hunger, f.wounds.size());
			for (const state::FighterPart& part : f.parts)
				std::printf(" [%s %.1f/%.1f stun %.1f%s]", part.name.c_str(), part.health, part.maxHealth - part.wear, part.stun, part.severed() ? " SEV" : "");
			std::printf("\n");
		}
		switch (e.kind)
		{
		case CombatEvent::Kind::Resolve:
		{
			Outcomes& mine = self.received;
			(e.code == 0 ? mine.dodged : e.code == 5 ? mine.blocked : mine.through) += 1;
			self.attackers.insert(e.other);
			if (other != slot.end())
				(e.code == 0 ? log.chars[other->second].made.dodged : e.code == 5 ? log.chars[other->second].made.blocked : log.chars[other->second].made.through) += 1;
			break;
		}
		case CombatEvent::Kind::Hit:
		{
			if (other != slot.end())
				log.chars[other->second].dmgLanded += e.dmg[0] + e.dmg[1];
			PartDamage& d = self.parts[e.part];
			++d.hits;
			d.cut += e.dmg[0];
			d.blunt += e.dmg[1];
			d.total += e.dmg[0] + e.dmg[1] + e.dmg[2];
			break;
		}
		case CombatEvent::Kind::Knockout:
			self.ko.push_back(t);
			break;
		case CombatEvent::Kind::Death:
			if (!self.death)
				self.death = t;
			break;
		default:
			break;
		}
	});

	// The trigger: the file's own orders (queued by scenario::apply), else at frame 0 the first player character attacks the nearest character of another team.
	if (options.trigger && placed.orders == 0)
	{
		const state::Fighter& p = *sim.combat()->find(placed.players.front());
		std::uint64_t nearest = 0;
		double best = 1.0e30;
		for (const std::uint64_t id : placed.others)
		{
			const state::Fighter& f = *sim.combat()->find(id);
			if (f.team == p.team || f.dead)
				continue;
			const double d = std::hypot(f.x - p.x, f.z - p.z);
			if (d < best)
			{
				best = d;
				nearest = id;
			}
		}
		if (nearest != 0)
			sim.enqueue(Command::fighterAttack(state::Id{placed.players.front()}, state::Id{nearest}));
		else
			std::cerr << "ksim_fight: no character of another team to attack (the scenario has no enemy)\n";
	}

	// With orders in the file the two sides are the attacker squads and everybody else (kinspect's bandits_down rule); else team 0 against the rest.
	const bool ordered = !log.sideA.empty();
	const auto standing = [&](bool playerSide)
	{
		int n = 0;
		for (const CharLog& c : log.chars)
		{
			const state::Fighter* f = sim.combat()->find(c.id);
			if ((ordered ? !c.attackerSide : c.player) == playerSide && f != nullptr && !f->dead && !f->unconscious && !f->down)
				++n;
		}
		return n;
	};
	std::map<std::uint64_t, std::uint64_t> lastTarget;
	std::optional<std::uint64_t> settledAt;
	std::string settledWinner;
	const std::uint64_t maxTicks = static_cast<std::uint64_t>(options.maxSeconds / Simulation::kTickSeconds);
	const std::uint64_t lingerTicks = static_cast<std::uint64_t>(3.0 / Simulation::kTickSeconds);
	std::uint64_t ticks = 0;
	for (std::uint64_t n = 0; n <= maxTicks; ++n)
	{
		ticks = n + 1;
		if (n % 20 == 0)
		{
			for (CharLog& c : log.chars)
			{
				const state::Fighter* f = sim.combat()->find(c.id);
				if (f != nullptr)
				{
					c.path.push_back({static_cast<double>(n) * Simulation::kTickSeconds, f->x, f->y, f->z});
					if (f->down && !c.down)
						c.down = static_cast<double>(n) * Simulation::kTickSeconds;
				}
			}
		}
		if (options.traceTargets && n == 200)
		{
			int routines = 0;
			int squads = 0;
			const flecs::world& ecs = sim.world().ecs();
			ecs.each([&](const state::Routine&) { ++routines; });
			ecs.each([&](const state::GroupAI&) { ++squads; });
			ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
			{
				int fighters = 0;
				ecs.query_builder<const state::Fighter>().with<state::MemberOf>(squad).build().each([&](const state::Fighter&) { ++fighters; });
				if (fighters == 0)
					return;
				const state::GroupAI* group = squad.get<state::GroupAI>();
				const Detail* detail = squad.get<Detail>();
				std::printf("TT squad %s source %d home %d fighters %d detail %d goal %d fallback %d routine %d\n", instance.templateSid.c_str(), static_cast<int>(instance.source),
					instance.homeTown.id != 0 ? 1 : 0, fighters, detail != nullptr ? static_cast<int>(detail->level) : -1, group != nullptr ? static_cast<int>(group->goal) : -1,
					group != nullptr ? static_cast<int>(group->fallback) : -1, squad.has<state::SquadRoutine>() ? 1 : 0);
			});
			for (const CharLog& c : log.chars)
			{
				for (const CombatArena::Member& m : sim.combat()->members())
				{
					if (m.id == c.id && !c.attackerSide)
						std::printf("TT join %s (%s) goals %d %s team %d faction %llu squad %llu\n", c.name.c_str(), c.squad.c_str(), static_cast<int>(m.joinGoals),
							m.joinDriven ? "driven" : "plain", m.fighter.team, static_cast<unsigned long long>(m.fighter.faction), static_cast<unsigned long long>(m.fighter.squad));
				}
			}
			std::printf("TT r%d at 10 s: %d characters hold a Routine, %d squads hold a GroupAI\n", index + 1, routines, squads);
		}
		if (options.traceTargets)
		{
			for (CharLog& c : log.chars)
			{
				const state::Fighter* f = sim.combat()->find(c.id);
				if (f == nullptr || f->dead || f->target == lastTarget[c.id])
					continue;
				lastTarget[c.id] = f->target;
				const state::Fighter* t = f->target != 0 ? sim.combat()->find(f->target) : nullptr;
				const auto tslot = slot.find(f->target);
				std::printf("TT r%d %7.2f %s (%s) -> %s (%s) d=%.0f team %d->%d\n", index + 1, static_cast<double>(n) * Simulation::kTickSeconds, c.name.c_str(), c.squad.c_str(),
					t != nullptr ? t->name.c_str() : "-", tslot != slot.end() ? log.chars[tslot->second].squad.c_str() : "", t != nullptr ? std::hypot(t->x - f->x, t->z - f->z) : 0.0,
					f->team, t != nullptr ? t->team : -1);
			}
		}
		if (ordered)
		{
			for (CharLog& c : log.chars)
			{
				const state::Fighter* f = sim.combat()->find(c.id);
				if (c.attackerSide || c.join || f == nullptr || f->dead || f->target == 0)
					continue;
				const auto target = slot.find(f->target);
				if (target != slot.end() && log.chars[target->second].attackerSide)
					c.join = static_cast<double>(n) * Simulation::kTickSeconds;
			}
		}
		if (!settledAt && (standing(true) == 0 || standing(false) == 0))
		{
			// The winner is the side still standing when one side goes down; the linger only extends the log.
			// A short KO (a stun-shortened timer) can end inside the linger and stand its side back up, which
			// must not turn a decided fight into "none".
			const int playersUp = standing(true);
			const int enemiesUp = standing(false);
			settledWinner = playersUp > 0 && enemiesUp == 0 ? "player" : enemiesUp > 0 && playersUp == 0 ? "enemy" : "none";
			settledAt = n;
		}
		if (settledAt && n >= *settledAt + lingerTicks)
			break;
		sim.tick();
	}
	log.logLength = static_cast<double>(std::min(maxTicks, settledAt ? *settledAt + lingerTicks : maxTicks)) * Simulation::kTickSeconds;
	log.duration = settledAt ? static_cast<double>(*settledAt) * Simulation::kTickSeconds : options.maxSeconds;
	log.winner = settledAt ? settledWinner : "none";
	log.pathQueries = sim.combat()->pathQueries();
	log.pathCacheHits = sim.combat()->pathCacheHits();
	log.ticks = ticks;
	for (CharLog& c : log.chars)
	{
		const state::Fighter* f = sim.combat()->find(c.id);
		c.standingAtEnd = f != nullptr && !f->dead && !f->unconscious && !f->down;
		if (f != nullptr)
		{
			c.bloodEnd = f->blood;
			for (const state::FighterPart& part : f->parts)
				c.partEnd.push_back(part.health);
		}
	}
	return log;
}

void writeDistribution(const Options& options, const scenario::File& file, const std::vector<RunLog>& runs)
{
	int wins[3] = {0, 0, 0};
	std::vector<double> duration;
	std::vector<double> firstPlayerKo;
	std::vector<double> playerThrough;
	std::vector<double> playerBlocked;
	std::vector<double> playerDodged;
	std::vector<double> attackersPerPlayer;
	std::vector<double> playerHitsLanded;
	std::vector<double> enemiesDown;
	std::vector<double> enemyAttackersPerVictim;
	std::map<std::string, std::vector<double>> playerPart;
	std::vector<double> pathQueriesPerTick;
	std::vector<double> pathCacheHitRate;
	for (const RunLog& r : runs)
	{
		++wins[r.winner == "player" ? 0 : r.winner == "enemy" ? 1 : 2];
		duration.push_back(r.duration);
		if (r.ticks > 0)
			pathQueriesPerTick.push_back(1000.0 * static_cast<double>(r.pathQueries) / static_cast<double>(r.ticks));
		const std::uint64_t lookups = r.pathQueries + r.pathCacheHits;
		if (lookups > 0)
			pathCacheHitRate.push_back(100.0 * static_cast<double>(r.pathCacheHits) / static_cast<double>(lookups));
		double firstKo = 1.0e30;
		double through = 0.0;
		double blocked = 0.0;
		double dodged = 0.0;
		double landed = 0.0;
		double down = 0.0;
		std::map<std::string, double> parts;
		for (const CharLog& c : r.chars)
		{
			if (c.player)
			{
				if (!c.ko.empty())
					firstKo = std::min(firstKo, c.ko.front());
				through += c.received.through;
				blocked += c.received.blocked;
				dodged += c.received.dodged;
				landed += c.made.through;
				if (!c.attackers.empty())
					attackersPerPlayer.push_back(static_cast<double>(c.attackers.size()));
				for (const auto& [part, d] : c.parts)
					parts[part] += d.total;
			} else {
				if (!c.standingAtEnd)
					down += 1.0;
				if (!c.attackers.empty())
					enemyAttackersPerVictim.push_back(static_cast<double>(c.attackers.size()));
			}
		}
		if (firstKo < 1.0e29)
			firstPlayerKo.push_back(firstKo);
		playerThrough.push_back(through);
		playerBlocked.push_back(blocked);
		playerDodged.push_back(dodged);
		playerHitsLanded.push_back(landed);
		enemiesDown.push_back(down);
		for (const auto& [part, v] : parts)
			playerPart[part].push_back(v);
	}
	std::ostringstream md;
	md << "# ksim_fight distribution: " << file.save << " (" << runs.size() << " runs, seeds " << runs.front().seed << ".." << runs.back().seed << ")\n\n"
		<< "Scenario `" << file.path << "`, " << file.chars.size() << " characters, clock day " << file.day << " " << file.hour << ":" << file.minute
		<< ". Cells: median [min..max]. Times in game seconds from the attack order.\n\n"
		<< "| measure | ours |\n|---|---|\n"
		<< "| winner player / enemy / none | " << wins[0] << " / " << wins[1] << " / " << wins[2] << " |\n"
		<< "| fight duration (one side down) s | " << spread(duration) << " |\n"
		<< "| first player-side KO s | " << spread(firstPlayerKo) << " |\n"
		<< "| player side: swings through | " << spread(playerThrough, 0) << " |\n"
		<< "| player side: swings blocked | " << spread(playerBlocked, 0) << " |\n"
		<< "| player side: swings dodged | " << spread(playerDodged, 0) << " |\n"
		<< "| player side: attackers per victim | " << spread(attackersPerPlayer, 0) << " |\n"
		<< "| player side: hits landed (swings through) | " << spread(playerHitsLanded, 0) << " |\n"
		<< "| enemy side: down at the end (KO or dead) | " << spread(enemiesDown, 0) << " |\n"
		<< "| enemy side: attackers per victim (hit ones) | " << spread(enemyAttackersPerVictim, 0) << " |\n"
		<< "| melee gate path queries per 1000 ticks | " << spread(pathQueriesPerTick, 0) << " |\n"
		<< "| melee gate cache hit rate % | " << spread(pathCacheHitRate, 0) << " |\n";
	for (const auto& [part, v] : playerPart)
		md << "| player side: damage taken, " << part << " (cut+blunt+pierce) | " << spread(v) << " |\n";
	md << "\nPer-run files: rN_summary.json (docs/reference/SCENARIO_IMPORT.md \"Summary format\").\n";
	std::ofstream(options.out + "/distribution.md") << md.str();

	std::ostringstream js;
	js << "{\"scenario\": " << jsonString(file.path) << ", \"runs\": " << runs.size() << ", \"winners\": {\"player\": " << wins[0] << ", \"enemy\": " << wins[1]
		<< ", \"none\": " << wins[2] << "},\n \"per_run\": [";
	for (std::size_t i = 0; i < runs.size(); ++i)
	{
		js << (i ? "," : "") << "\n  {\"run\": " << runs[i].run << ", \"seed\": " << runs[i].seed << ", \"winner\": " << jsonString(runs[i].winner)
			<< ", \"duration_s\": " << fixed(runs[i].duration, 2) << ", \"first_player_ko_s\": ";
		if (i < firstPlayerKo.size() && firstPlayerKo.size() == runs.size())
			js << fixed(firstPlayerKo[i], 2);
		else
			js << "null";
		js << ", \"player_through\": " << num(playerThrough[i]) << ", \"player_blocked\": " << num(playerBlocked[i]) << ", \"player_dodged\": " << num(playerDodged[i])
			<< ", \"enemies_down\": " << num(enemiesDown[i]) << ", \"path_queries\": " << runs[i].pathQueries
			<< ", \"path_cache_hits\": " << runs[i].pathCacheHits << "}";
	}
	js << "\n ]}\n";
	std::ofstream(options.out + "/distribution.json") << js.str();
	std::cout << md.str();
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		try
		{
			if (arg == "--navworld" && hasValue)
				options.navworld = argv[++i];
			else if (arg == "--runs" && hasValue)
				options.runs = std::stoi(argv[++i]);
			else if (arg == "--seed" && hasValue)
				options.seed = std::stoull(argv[++i]);
			else if (arg == "--out" && hasValue)
				options.out = argv[++i];
			else if (arg == "--max-seconds" && hasValue)
				options.maxSeconds = std::stod(argv[++i]);
			else if (arg == "--mods" && hasValue)
				options.mods = argv[++i];
			else if (arg == "--fight-stride" && hasValue)
				options.fightStride = std::stoi(argv[++i]);
			else if (arg == "--idle-stride" && hasValue)
				options.idleStride = std::stoi(argv[++i]);
			else if (arg == "--no-trigger")
				options.trigger = false;
			else if (arg == "--trace-all")
				options.trace = options.traceAll = true;
			else if (arg == "--relations")
				options.relations = true;
			else if (arg == "--trace-targets")
				options.traceTargets = true;
			else if (arg == "--no-ai")
				options.ai = false;
			else if (arg == "--trace")
				options.trace = true;
			else if (arg == "--check")
				options.check = true;
			else if (arg == "--optional")
				options.optional = true;
			else if (arg == "--trace-deaths")
				options.traceDeaths = true;
			else if (arg == "--medical-clock" && i + 1 < argc)
				options.medicalClock = std::atof(argv[++i]);
			else if (arg[0] != '-' && options.scenario.empty())
				options.scenario = arg;
			else
				return usage();
		}
		catch (const std::exception&)
		{
			return usage();
		}
	}
	if (options.scenario.empty() || options.runs < 1)
		return usage();
	if (!std::filesystem::exists(options.scenario))
	{
		if (options.optional)
		{
			std::cout << "ksim_fight: SKIP, scenario file " << options.scenario << " does not exist (converted fixtures are game-derived and live outside the repo)\n";
			return 77;
		}
		std::cerr << "ksim_fight: no such scenario file " << options.scenario << "\n";
		return 1;
	}
	if (!options.navworld.empty() && !std::filesystem::exists(options.navworld + "/"))
	{
		std::cout << "ksim_fight: SKIP, no navworld bake at " << options.navworld << "\n";
		return 77;
	}
	try
	{
		std::filesystem::create_directories(options.out);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, options.mods);
		for (const std::string& missing : order.missing)
			throw std::runtime_error("--mods lists a missing file: " + missing);
		const scenario::File file = scenario::parseFile(options.scenario);
		std::shared_ptr<const CombatData> combatData;
		std::vector<RunLog> runs;
		for (int i = 0; i < options.runs; ++i)
		{
			runs.push_back(runOne(options, order, combatData, file, i));
			std::ofstream(options.out + "/r" + std::to_string(i + 1) + "_summary.json") << summaryJson(runs.back());
			if (!runs.back().sideA.empty())
				std::ofstream(options.out + "/r" + std::to_string(i + 1) + "_joiners.json") << joinersJson(runs.back());
			std::cerr << "ksim_fight: run " << i + 1 << "/" << options.runs << " seed " << runs.back().seed << ": winner " << runs.back().winner << ", "
				<< fixed(runs.back().duration, 1) << " s\n";
		}
		writeDistribution(options, file, runs);
		if (options.check)
		{
			for (const RunLog& r : runs)
			{
				int swings = 0;
				for (const CharLog& c : r.chars)
					swings += c.made.through + c.made.blocked + c.made.dodged;
				if (swings == 0 || r.winner == "none" || r.chars.front().path.size() < 2)
				{
					std::cerr << "ksim_fight: --check failed on run " << r.run << " (swings " << swings << ", winner " << r.winner << ")\n";
					return 1;
				}
			}
			std::cout << "ksim_fight: check ok\n";
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_fight: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
