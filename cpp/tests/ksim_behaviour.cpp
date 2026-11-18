// The Lua behaviour layer, first slice: the faction intent / temperament scoring as a
// script (data/behaviour/default/faction_intent.lua; API: docs/reference/LUA_API.md).
//
//  1. equivalence: the default script and the native C++ scoring give the same state
//     hash, every few game-hours, over a scenario with raids, kills, town changes and
//     relation flips (the scoring's arithmetic is the same, in the same order);
//  2. fallback: a script that errors, loops forever, allocates without bound, returns
//     garbage, has no `decide`, does not compile, or fails on some hours only, ends in
//     the native scoring: the sim goes on and, since the state the script left is
//     untouched, the hash equals the native run's;
//  3. subscriptions and declared fields: a timer with a phase, raid outcomes, relation
//     crossings and squad losses wake a script; its typed fields are saved;
//  4. 1 = 4 workers and save + load + run == the uninterrupted run, scripts on.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/factions.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using ksim::BehaviourScripts;

class FakeNavigator final : public ksim::Navigator
{
public:
	bool covers(double, double) const override { return true; }

	ksim::Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		ksim::Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

ksim::Simulation newGame(const kworld::LoadOrder& order, int workers, std::uint64_t seed = 5)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	ksim::RegionConfig config;
	config.workerThreads = workers;
	return ksim::Simulation::newGame(KENSHI_DATA, order, options, nullptr, config);
}

// What a run says about the scripts, from the log lines.
struct Logs
{
	std::vector<std::string> lines;
	std::size_t about(const std::string& what) const
	{
		return static_cast<std::size_t>(std::count_if(lines.begin(), lines.end(),
			[&](const std::string& line) { return line.find(what) != std::string::npos; }));
	}
};

void enable(ksim::Simulation& simulation, BehaviourScripts scripts, Logs& logs, double frequency = 40.0)
{
	ksim::AiSystem::Config config;
	config.registry = std::make_shared<ksim::BehaviourRegistry>(simulation.world(),
		ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<FakeNavigator>();
	config.factions = ksim::FactionBook::build(simulation.gameData());
	config.factionTuning.raidFrequency = frequency;
	config.factionTuning.npcRaids = true; // raids on NPC towns (world.npc_raids on; the default is player towns only, kenshi-51x.5)
	config.scripts = std::move(scripts);
	config.log = [&logs](std::string_view line) { logs.lines.emplace_back(line); };
	simulation.enableAi(std::move(config));
}

BehaviourScripts native()
{
	BehaviourScripts scripts;
	scripts.mode = BehaviourScripts::Mode::Native;
	return scripts;
}

BehaviourScripts scripted()
{
	BehaviourScripts scripts;
	scripts.mode = BehaviourScripts::Mode::Scripted;
	return scripts;
}

BehaviourScripts withSource(const std::string& source)
{
	BehaviourScripts scripts = scripted();
	scripts.factionIntentSource = source;
	scripts.factionIntentName = "test.lua";
	return scripts;
}

std::string readDefault()
{
	std::string text;
	std::FILE* file = std::fopen(KSIM_DEFAULT_SCRIPT, "rb");
	test::require(file != nullptr, "cannot read the default script");
	char buffer[4096];
	for (std::size_t n; (n = std::fread(buffer, 1, sizeof buffer, file)) > 0;)
		text.append(buffer, n);
	std::fclose(file);
	return text;
}

std::vector<std::uint64_t> idsOf(const ksim::Simulation& simulation, bool squads)
{
	std::vector<std::uint64_t> ids;
	const flecs::world& ecs = simulation.world().ecs();
	if (squads)
	{
		ecs.each([&](const ksim::state::Id& id, const ksim::state::SquadInstance& squad)
		{
			if (squad.source != ksim::state::SpawnSource::Campaign && squad.faction.id != 0)
				ids.push_back(id.value);
		});
	} else {
		ecs.each([&](const ksim::state::Id& id, const ksim::state::FactionState&) { ids.push_back(id.value); });
	}
	std::sort(ids.begin(), ids.end());
	return ids;
}

std::vector<std::uint64_t> townIds(const ksim::Simulation& simulation)
{
	std::vector<std::uint64_t> ids;
	simulation.world().ecs().each([&](const ksim::state::Id& id, const ksim::state::TownState&)
	{
		ids.push_back(id.value);
	});
	std::sort(ids.begin(), ids.end());
	return ids;
}

// The same commands in every run: kills with a killer, town changes and relation
// flips, spread over the factions, so the temperament moves and the relations cross.
void inject(ksim::Simulation& simulation, int hour)
{
	using ksim::state::Id;
	const std::vector<std::uint64_t> factions = idsOf(simulation, false);
	if (factions.size() < 4)
		return;
	if (hour % 5 == 2)
	{
		const std::vector<std::uint64_t> squads = idsOf(simulation, true);
		if (!squads.empty())
			simulation.enqueue(ksim::Command::removeSquad(Id{squads[static_cast<std::size_t>(hour) * 7 % squads.size()]},
				Id{factions[static_cast<std::size_t>(hour) * 3 % factions.size()]}));
	}
	if (hour % 24 == 10)
	{
		// A massacre: four squads of one faction in one hour (grief enough to mourn).
		std::vector<std::pair<std::uint64_t, std::uint64_t>> byFaction; // (faction Id, squad Id)
		const flecs::world& ecs = simulation.world().ecs();
		ecs.each([&](const ksim::state::Id& id, const ksim::state::SquadInstance& squad)
		{
			if (squad.source != ksim::state::SpawnSource::Campaign && squad.faction.id != 0)
				byFaction.emplace_back(flecs::entity(ecs.c_ptr(), squad.faction.id).get<Id>()->value, id.value);
		});
		std::sort(byFaction.begin(), byFaction.end());
		std::size_t from = static_cast<std::size_t>(hour / 24) * 29 % std::max<std::size_t>(byFaction.size(), 1);
		for (std::size_t tries = 0; tries < byFaction.size(); ++tries)
		{
			const std::size_t at = (from + tries) % byFaction.size();
			std::size_t count = 0;
			while (at + count < byFaction.size() && byFaction[at + count].first == byFaction[at].first)
				++count;
			if (at > 0 && byFaction[at - 1].first == byFaction[at].first)
				continue; // start at the first squad of a faction
			if (count < 4)
				continue;
			for (std::size_t i = 0; i < 4; ++i)
				simulation.enqueue(ksim::Command::removeSquad(Id{byFaction[at + i].second}));
			break;
		}
	}
	if (hour % 9 == 4)
	{
		const std::vector<std::uint64_t> towns = townIds(simulation);
		if (!towns.empty())
			simulation.enqueue(ksim::Command::setTownOwner(Id{towns[static_cast<std::size_t>(hour) * 11 % towns.size()]},
				Id{factions[static_cast<std::size_t>(hour) * 5 % factions.size()]}));
	}
	if (hour % 13 == 6)
	{
		const std::size_t a = static_cast<std::size_t>(hour) * 3 % factions.size();
		const std::size_t b = (a + 1 + static_cast<std::size_t>(hour) % 5) % factions.size();
		simulation.enqueue(ksim::Command::setFactionRelation(Id{factions[a]}, Id{factions[b]},
			(hour / 13) % 2 == 0 ? -60 : 60));
	}
}

struct Run
{
	std::vector<std::uint64_t> hashes; // every kHashEvery hours
	ksim::FactionStats stats;
	std::uint64_t finalHash = 0;
	std::uint32_t planned = 0;
	std::uint32_t completed = 0;
	std::uint32_t failed = 0;
	std::size_t goalless = 0;
};

constexpr int kHashEvery = 4;

Run scenario(const kworld::LoadOrder& order, BehaviourScripts scripts, int hours, Logs& logs, int workers = 1)
{
	ksim::Simulation simulation = newGame(order, workers);
	enable(simulation, std::move(scripts), logs);
	Run run;
	for (int hour = 0; hour < hours; ++hour)
	{
		inject(simulation, hour);
		simulation.run(ksim::kTicksPerGameHour);
		if (hour % kHashEvery == kHashEvery - 1)
			run.hashes.push_back(simulation.stateHash());
	}
	run.finalHash = simulation.stateHash();
	run.stats = *simulation.ai()->factionStats();
	run.goalless = simulation.ai()->audit(simulation.world().ecs()).goalless;
	simulation.world().ecs().each([&](const ksim::state::FactionAI& f)
	{
		run.planned += f.planned;
		run.completed += f.completed;
		run.failed += f.failed;
	});
	return run;
}

// A script with the given decide body (and the timer subscription).
std::string scriptWith(const std::string& body)
{
	return "return { api = 0, subscriptions = { { on = 'timer', every_hours = 1 } }, "
		"decide = function(view) " + body + " end }";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const std::string defaultSource = readDefault();

		// 1. Equivalence: the default script against the native scoring.
		constexpr int kDays = 6;
		constexpr int kHours = 24 * kDays;
		Logs nativeLogs;
		Logs scriptLogs;
		const Run reference = scenario(order, native(), kHours, nativeLogs);
		const Run lua = scenario(order, scripted(), kHours, scriptLogs);
		test::require(reference.stats.scriptApplied == 0 && !reference.stats.scriptLoaded,
			"the native run used a script");
		test::require(lua.stats.scriptLoaded && lua.stats.scriptApplied > 0,
			"the scripted run did not run the script");
		test::require(lua.stats.scriptFallbacks == 0 && lua.stats.scriptSleeping == 0
			&& scriptLogs.about("faction script") == 0, "the default script failed or slept");
		test::require(reference.hashes.size() == lua.hashes.size(), "hash count");
		for (std::size_t i = 0; i < lua.hashes.size(); ++i)
			test::require(reference.hashes[i] == lua.hashes[i], "the scripted and native state hashes differ at hour "
				+ std::to_string((i + 1) * kHashEvery));
		test::require(reference.finalHash == lua.finalHash, "final hashes differ");
		test::require(reference.stats.intentSteps == lua.stats.intentSteps, "the intent counts differ");
		test::require(reference.planned > 5 && reference.completed > 0, "the scenario planned too few raids: "
			+ std::to_string(reference.planned));
		test::require(lua.stats.intentSteps[static_cast<std::size_t>(ksim::state::Intent::Defend)] > 0
			&& lua.stats.intentSteps[static_cast<std::size_t>(ksim::state::Intent::Mourn)] > 0
			&& lua.stats.intentSteps[static_cast<std::size_t>(ksim::state::Intent::Raid)] > 0,
			"the scenario never reached Defend, Mourn and Raid: " + std::to_string(lua.stats.intentSteps[0]) + "/"
			+ std::to_string(lua.stats.intentSteps[1]) + "/" + std::to_string(lua.stats.intentSteps[2]) + "/"
			+ std::to_string(lua.stats.intentSteps[3]));
		std::cout << "equivalence: " << kDays << " days, " << lua.hashes.size() << " state hashes identical, "
			<< lua.stats.scriptApplied << " script decisions, intent steps (consolidate/defend/raid/mourn) "
			<< lua.stats.intentSteps[0] << "/" << lua.stats.intentSteps[1] << "/" << lua.stats.intentSteps[2]
			<< "/" << lua.stats.intentSteps[3] << ", raids planned " << lua.planned << ", completed "
			<< lua.completed << ", failed " << lua.failed << ", script peak " << lua.stats.scriptPeakBytes
			<< " bytes\n";

		// 2. Fallback: every kind of broken script ends in the native scoring.
		{
			constexpr int kFallbackHours = 48;
			Logs ignored;
			const Run base = scenario(order, native(), kFallbackHours, ignored);
			// The default script behind a guard that sometimes misbehaves.
			const auto wrapDefault = [&](const std::string& guard)
			{
				return "local inner = (function() " + defaultSource + " end)() "
					"return { api = 0, subscriptions = inner.subscriptions, decide = function(view) "
					+ guard + " return inner.decide(view) end }";
			};
			struct Case
			{
				const char* what;
				std::string source;
				std::uint64_t ksim::FactionStats::* counter; // the failure that must have happened
				bool loads;
			};
			const std::vector<Case> cases = {
				{"errors", scriptWith("error('boom')"), &ksim::FactionStats::scriptErrors, true},
				{"loops forever", scriptWith("while true do end"), &ksim::FactionStats::scriptBudget, true},
				{"loops under pcall", scriptWith("repeat pcall(function() while true do end end) until false"),
					&ksim::FactionStats::scriptBudget, true},
				{"allocates without bound", scriptWith("local s = 'x' while true do s = s .. s end"),
					&ksim::FactionStats::scriptMemory, true},
				{"returns a number", scriptWith("return 42"), &ksim::FactionStats::scriptUnusable, true},
				{"returns a function", scriptWith("return function() end"), &ksim::FactionStats::scriptUnusable, true},
				{"returns a list", scriptWith("return { 1, 2, 3 }"), &ksim::FactionStats::scriptUnusable, true},
				{"returns an unknown intent", scriptWith("return { set_intent = 'banana' }"),
					&ksim::FactionStats::scriptUnusable, true},
				{"returns an unknown command", scriptWith("return { set_everything = 1 }"),
					&ksim::FactionStats::scriptUnusable, true},
				{"returns a temperament outside 0..1", scriptWith("return { set_temperament = { alarm = 2.0 } }"),
					&ksim::FactionStats::scriptUnusable, true},
				{"returns a NaN", scriptWith("return { set_temperament = { grief = 0/0 } }"),
					&ksim::FactionStats::scriptUnusable, true},
				{"sets an undeclared field", scriptWith("return { set_state = { nope = 1 } }"),
					&ksim::FactionStats::scriptUnusable, true},
				{"raids a faction with nothing to raid", wrapDefault(
					"if not view.can_raid then return { set_intent = 'raid' } end"),
					&ksim::FactionStats::scriptUnusable, true},
				{"fails on some hours only", wrapDefault("if view.hour % 3 == 0 then error('every third hour') end"),
					&ksim::FactionStats::scriptErrors, true},
				{"has no decide", "return { api = 0, subscriptions = { { on = 'timer' } } }", nullptr, false},
				{"declares another api", "return { api = 7, decide = function() end }", nullptr, false},
				{"does not compile", "return { api = 0, decide = function( end }", nullptr, false},
				{"subscribes to nothing known", "return { api = 0, subscriptions = { { on = 'weather' } }, "
					"decide = function() end }", nullptr, false},
				{"is not text", std::string("\x1bLua\x55", 5), nullptr, false},
			};
			for (const Case& c : cases)
			{
				Logs logs;
				const Run broken = scenario(order, withSource(c.source), kFallbackHours, logs);
				test::require(broken.finalHash == base.finalHash, std::string("a script that ") + c.what
					+ " changed the outcome (the native fallback must decide from the untouched state)");
				test::require(broken.hashes == base.hashes, std::string("hashes differ for a script that ") + c.what);
				test::require(broken.goalless == 0, std::string("squads without a goal after a script that ") + c.what);
				test::require(broken.planned == base.planned && broken.planned > 0, "raids stopped");
				test::require(broken.stats.scriptLoaded == c.loads, std::string("load status of a script that ") + c.what);
				if (c.counter != nullptr)
				{
					test::require(broken.stats.*(c.counter) > 0, std::string("no ") + c.what + " recorded");
					test::require(broken.stats.scriptFallbacks > 0, "fallbacks");
					test::require(logs.about("faction script 'test.lua'") > 0, std::string("no log line for a script that ") + c.what);
				} else {
					test::require(logs.about("not loaded") == 1, std::string("no load failure logged for a script that ")
						+ c.what + ": " + (logs.lines.empty() ? std::string("(nothing)") : logs.lines[0]));
				}
				std::cout << "fallback: a script that " << c.what << ": " << broken.stats.scriptFallbacks
					<< " fallbacks, " << logs.lines.size() << " log lines"
					<< ", state identical to native\n";
			}
		}

		// 3. Subscriptions and declared fields.
		// The default script wrapped: it still decides, and a second layer counts what woke it
		// in typed fields. Woken by a 6-hour timer with a phase and by the events only.
		const std::string fieldsScript =
			"local inner = (function() " + defaultSource + " end)()\n"
			"return { api = 0,\n"
			"  subscriptions = { { on = 'timer', every_hours = 6, phase_hours = 2 }, { on = 'raid_outcome' },\n"
			"    { on = 'relation_crossed' }, { on = 'squad_lost' }, { on = 'town_lost' }, { on = 'town_gained' } },\n"
			"  fields = { { name = 'wakes', type = 'integer' }, { name = 'outcomes', type = 'integer' },\n"
			"    { name = 'crossings', type = 'integer' }, { name = 'lost', type = 'integer' },\n"
			"    { name = 'weight', type = 'number', default = 0.5 }, { name = 'seen_win', type = 'boolean' } },\n"
			"  decide = function(view)\n"
			"    local result = inner.decide(view)\n"
			"    local s = view.state\n"
			"    local out = { wakes = s.wakes + 1, weight = s.weight * 0.5 + 0.25 * ksim.random(), seen_win = s.seen_win }\n"
			"    out.outcomes, out.crossings, out.lost = s.outcomes, s.crossings, s.lost\n"
			"    for i = 1, #view.events do\n"
			"      local e = view.events[i]\n"
			"      if e.on == 'raid_outcome' then out.outcomes = out.outcomes + 1\n"
			"        if e.result == 'victory' then out.seen_win = true end\n"
			"      elseif e.on == 'relation_crossed' then\n"
			"        assert(e.threshold == -30 or e.threshold == 50) assert(e.direction == 'up' or e.direction == 'down')\n"
			"        assert(math.type(e.other) == 'integer')\n"
			"        out.crossings = out.crossings + 1\n"
			"      elseif e.on == 'squad_lost' then out.lost = out.lost + 1 end\n"
			"    end\n"
			"    result.set_state = out\n"
			"    return result\n"
			"  end }\n";
		{
			Logs logs;
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, withSource(fieldsScript), logs);
			for (int hour = 0; hour < 24 * 5; ++hour)
			{
				inject(simulation, hour);
				simulation.run(ksim::kTicksPerGameHour);
			}
			const ksim::FactionStats& stats = *simulation.ai()->factionStats();
			test::require(logs.lines.empty() || logs.about("faction script") == 0, "the fields script failed: "
				+ (logs.lines.empty() ? std::string() : logs.lines[0]));
			test::require(stats.scriptFallbacks == 0, "the fields script fell back");
			test::require(stats.scriptSleeping > stats.scriptApplied && stats.scriptApplied > 0,
				"a 6-hour timer plus events should wake the script on a minority of the steps: applied "
				+ std::to_string(stats.scriptApplied) + ", sleeping " + std::to_string(stats.scriptSleeping));
			std::int64_t wakes = 0;
			std::int64_t outcomes = 0;
			std::int64_t crossings = 0;
			std::int64_t lost = 0;
			std::size_t winners = 0;
			std::size_t withFields = 0;
			simulation.world().ecs().each([&](const ksim::state::FactionAI& f)
			{
				if (f.script.empty())
					return;
				++withFields;
				test::require(f.script.size() == 6 && f.script[0].name == "crossings" && f.script[5].name == "weight",
					"the fields are not the declared six, sorted by name");
				for (const ksim::state::ScriptField& field : f.script)
				{
					wakes += field.name == "wakes" ? field.integer : 0;
					outcomes += field.name == "outcomes" ? field.integer : 0;
					crossings += field.name == "crossings" ? field.integer : 0;
					lost += field.name == "lost" ? field.integer : 0;
					winners += field.name == "seen_win" ? static_cast<std::size_t>(field.integer) : 0;
					if (field.name == "weight")
						test::require(field.number >= 0.0 && field.number <= 1.0, "weight out of range");
				}
			});
			test::require(static_cast<std::uint64_t>(wakes) == stats.scriptApplied, "wake counter != decisions: "
				+ std::to_string(wakes) + " vs " + std::to_string(stats.scriptApplied));
			test::require(outcomes > 0 && winners > 0, "no raid outcome reached the script");
			test::require(crossings > 0, "no relation crossing reached the script");
			test::require(lost > 0, "no squad loss reached the script");
			std::cout << "subscriptions: " << wakes << " wakes (timer 6 h phase 2, raid outcomes " << outcomes
				<< ", relation crossings " << crossings << ", squads lost " << lost << ", " << winners
				<< " factions saw a victory), " << stats.scriptSleeping << " steps slept, " << withFields
				<< " factions hold fields\n";
		}

		// 4. Workers and continuation, scripts on (the default script, and one with fields).
		for (const bool fields : {false, true})
		{
			const auto make = [&]() { return fields ? withSource(fieldsScript) : scripted(); };
			Logs logs;
			constexpr int kRunHours = 24 * 3;
			const Run one = scenario(order, make(), kRunHours, logs, 1);
			const Run four = scenario(order, make(), kRunHours, logs, 4);
			test::require(one.finalHash == four.finalHash && one.hashes == four.hashes,
				"1 worker and 4 workers differ with scripts on");

			ksim::Simulation reference = newGame(order, 1);
			enable(reference, make(), logs);
			for (int hour = 0; hour < 30; ++hour)
			{
				inject(reference, hour);
				reference.run(ksim::kTicksPerGameHour);
			}
			reference.run(37);
			const std::uint64_t split = reference.stateHash();
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_behaviour_test.ksav";
			reference.saveGame(path);
			const auto tail = [&](ksim::Simulation& simulation)
			{
				for (int hour = 30; hour < 30 + 40; ++hour)
				{
					inject(simulation, hour);
					simulation.run(ksim::kTicksPerGameHour);
				}
			};
			tail(reference);
			for (const int workers : {4, 1})
			{
				ksim::RegionConfig config;
				config.workerThreads = workers;
				ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, config);
				test::require(loaded.stateHash() == split, "the loaded game hashes differently");
				enable(loaded, make(), logs);
				tail(loaded);
				test::require(loaded.stateHash() == reference.stateHash(), std::string("load + run differs from the "
					"uninterrupted run with scripts on (") + std::to_string(workers) + " workers"
					+ (fields ? ", declared fields)" : ")"));
			}
			std::remove(path.c_str());
			test::require(logs.about("faction script") == 0, "a script failed in the determinism runs");
			std::cout << "determinism" << (fields ? " (declared fields)" : "") << ": 1 = 4 workers over "
				<< kRunHours << " hours, save + load + run identical (1 and 4 workers)\n";
		}

		std::cout << "ksim_behaviour: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_behaviour: " << error.what() << "\n";
		return 1;
	}
}
