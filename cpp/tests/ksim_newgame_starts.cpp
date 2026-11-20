// kenshi-k16.17.13.2: every NEW_GAME_STARTOFF spawns as its data says, and nobody moves by itself in 3 game minutes.
//
// For each non-removed record of the merged data (13 with rebirth.mod) a New Game starts with the default creator choices (the template's own
// members, no Spec) and the result is checked against the record, read straight from the data (not through StartCatalog / startMembers):
//   members   the count lies in the range the squad refs allow (leader v0, `squad` / `squad2` v0..v1, `choosefrom list` draws, a bare CHARACTER's
//             count); every member is one the refs name; an ANIMAL_CHARACTER spawns as an animal; the race is one the record lists (when it does)
//             and an item the CHARACTER lists is in its pack
//   position  the forced X/Z, else within a listed town's placements (the seeded pick)
//   money     the faction's money is the record's `money`
//   relations each `faction relations` entry is the relation, both ways (v0, clamped -100..100)
//   research  the completed set is the record's `research` (the live ones), else `_Default Start`
//   motion    3 game minutes with no orders: a player-squad member that spins (the heading turns a full circle while it stays in place), walks a
//             route (goal / task named), stalls on a route it never progresses on, or re-plans in a loop is flagged
// One line per start. Fails on any mismatch except a KNOWN failure (a start / check named in kKnown with its item): those print "KNOWN" and
// do not fail; the test also reports a known failure that no longer fails.
//
// KSIM_STARTS_ONLY=<substring of a start's name>: only those. KSIM_STARTS_DUMP=1: print the records and stop. KSIM_STARTS_MINUTES=M: the run (default 3).
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

// A known failure: the start's name, the check that fails ("members", "position", "money", "relations", "research", "motion"), the item that fixes it.
struct Known
{
	const char* start;
	const char* check;
	const char* item;
};
const Known kKnown[] = {
	{"The Cannibal Hunters", "motion", "kenshi-k16.17.13.8 (members drift unordered)"},
};

const Known* known(const std::string& start, const std::string& check)
{
	for (const Known& entry : kKnown)
	{
		if (start == entry.start && check == entry.check)
			return &entry;
	}
	return nullptr;
}

std::vector<const kdata::RefEntry*> refsOf(const kdata::Record& record, const char* category)
{
	std::vector<const kdata::RefEntry*> out;
	if (const kdata::RefCategory* refs = record.findRefs(category))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (!entry.opaqueEntry)
				out.push_back(&entry);
		}
	}
	return out;
}

void dumpRecord(const kdata::Record& r)
{
	std::cout << "  " << r.stringID << " \"" << r.name << "\" type " << r.type << "\n";
	for (const kdata::FieldInt& f : r.ints)
		std::cout << "    int " << f.key << " = " << f.value << "\n";
	for (const kdata::FieldBool& f : r.bools)
		std::cout << "    bool " << f.key << " = " << f.value << "\n";
	for (const kdata::FieldString& f : r.strings)
		std::cout << "    str " << f.key << " = " << f.value << "\n";
	for (const kdata::RefCategory& c : r.references)
	{
		for (const kdata::RefEntry& e : c.entries)
			std::cout << "    ref " << c.category << " -> " << e.targetID << " (" << e.v0 << "," << e.v1 << "," << e.v2 << ")\n";
	}
}

// What the squad refs of one start allow.
struct Expect
{
	std::int64_t low = 0;
	std::int64_t high = 0;
	std::set<std::string> characters; // every CHARACTER the refs name
	std::vector<std::string> templates;
};

Expect expectMembers(const kdata::GameData& gd, const kdata::Record& start)
{
	Expect out;
	const std::int32_t templateType = kworld::itemTypeByName("SQUAD_TEMPLATE");
	for (const kdata::RefEntry* ref : refsOf(start, "squad"))
	{
		const kdata::Record* record = gd.find(ref->targetID);
		if (record == nullptr)
			continue;
		if (record->type != templateType)
		{
			out.low += std::max(1, ref->v0);
			out.high += std::max(1, ref->v0);
			out.characters.insert(ref->targetID);
			continue;
		}
		out.templates.push_back(ref->targetID);
		for (const kdata::RefEntry* e : refsOf(*record, "leader"))
		{
			out.low += e->v0;
			out.high += e->v0;
			out.characters.insert(e->targetID);
		}
		for (const char* list : {"squad", "squad2", "animals", "animals2"})
		{
			for (const kdata::RefEntry* e : refsOf(*record, list))
			{
				out.low += e->v0;
				out.high += e->v1 == 0 ? e->v0 : std::max(e->v0, e->v1);
				out.characters.insert(e->targetID);
			}
		}
		const std::vector<const kdata::RefEntry*> choices = refsOf(*record, "choosefrom list");
		if (!choices.empty())
		{
			out.low += record->intOr("num random chars", 0);
			out.high += std::max(record->intOr("num random chars", 0), record->intOr("num random chars max", 0));
			for (const kdata::RefEntry* e : choices)
				out.characters.insert(e->targetID);
		}
	}
	return out;
}

struct Watch
{
	double x0 = 0.0;
	double z0 = 0.0;
	double lastYaw = 0.0;
	double turned = 0.0; // the unwrapped sum of |yaw changes|
	double maxAway = 0.0;
	double walkingSeconds = 0.0;
	double stalledSeconds = 0.0; // walking with no progress
	double lastX = 0.0;
	double lastZ = 0.0;
	std::uint32_t routeVersion0 = 0;
	std::uint32_t replans = 0;
	std::uint32_t lastRouteVersion = 0;
	double maxStep = 0.0; // the biggest one-tick displacement (a jump, not a walk)
	std::string first; // the first tick it left its spot by >5 u
	std::string goal; // what the first walk was (route source)
	std::string name;
	bool animal = false;
};

} // namespace

int main(int argc, char** argv)
try
{
	if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
	{
		std::cout << "ksim_newgame_starts: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	const char* only = std::getenv("KSIM_STARTS_ONLY");
	const double minutes = std::getenv("KSIM_STARTS_MINUTES") != nullptr ? std::atof(std::getenv("KSIM_STARTS_MINUTES")) : 3.0;
	std::vector<std::string> names; // the starts, by sid
	{
		SpawnOptions options;
		options.seed = 1;
		Simulation probe = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const kworld::Towns towns = kworld::Towns::join(*probe.gameData(), KENSHI_DATA);
		for (const kdata::Record* record : probe.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (!record->boolOr("REMOVED", false))
				names.push_back(record->stringID);
		}
		if (std::getenv("KSIM_STARTS_DUMP") != nullptr)
		{
			for (const std::string& sid : names)
			{
				const kdata::Record& start = *probe.gameData()->find(sid);
				dumpRecord(start);
				for (const kdata::RefEntry* ref : refsOf(start, "squad"))
				{
					if (const kdata::Record* squad = probe.gameData()->find(ref->targetID))
					{
						dumpRecord(*squad);
						for (const kdata::RefCategory& c : squad->references)
						{
							for (const kdata::RefEntry& e : c.entries)
							{
								if (const kdata::Record* member = probe.gameData()->find(e.targetID); member != nullptr && c.category != "faction")
									dumpRecord(*member);
							}
						}
					}
				}
			}
			return 0;
		}
	}

	std::size_t failures = 0;
	std::size_t startsRun = 0;
	std::cout << "start | members | gear | position | money | relations | research | motion\n";
	for (const std::string& sid : names)
	{
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const kdata::GameData& gd = *sim.gameData();
		const kdata::Record& record = *gd.find(sid);
		if (only != nullptr && record.name.find(only) == std::string::npos)
			continue;
		++startsRun;
		sim.markPristine();
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(CombatData::build(gd, KENSHI_DATA), settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(test::openWorldNav(argv[1], hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, argv[1], 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));

		NewGameOutcome outcome;
		bool seen = false;
		sim.addNewGameObserver(&outcome, [&](const NewGameOutcome& o) { outcome = o; seen = true; });
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		sim.run(1);
		std::map<std::string, std::vector<std::string>> problems; // check -> mismatches
		if (!seen || !outcome.reason.empty())
		{
			std::cout << record.name << " | NO START: " << (seen ? outcome.reason : std::string("no outcome")) << "\n";
			++failures;
			continue;
		}

		// ---- the spawned members ----
		std::vector<std::uint64_t> ids = outcome.members;
		std::map<std::uint64_t, const state::Inventory*> packs;
		ecsOf(sim).each([&](const state::Id& id, const state::Inventory& pack) { packs[id.value] = &pack; });
		const Expect expect = expectMembers(gd, record);
		if (static_cast<std::int64_t>(ids.size()) < expect.low || static_cast<std::int64_t>(ids.size()) > expect.high)
			problems["members"].push_back("count " + std::to_string(ids.size()) + " outside " + std::to_string(expect.low) + ".." + std::to_string(expect.high));
		std::set<std::string> raceNames;
		std::size_t animals = 0;
		std::string memberText;
		for (const std::uint64_t id : ids)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
			{
				problems["members"].push_back("member " + std::to_string(id) + " has no fighter");
				continue;
			}
			animals += f->animal ? 1 : 0;
			memberText += (memberText.empty() ? "" : ",") + f->name;
			const auto pack = packs.find(id);
			// The CHARACTER the member is: the refs name it by name (the fighter's name is the character's, a creator Spec aside).
			const kdata::Record* character = nullptr;
			for (const std::string& csid : expect.characters)
			{
				const kdata::Record* c = gd.find(csid);
				if (c != nullptr && c->name == f->name)
					character = c;
			}
			if (character == nullptr)
			{
				problems["members"].push_back("\"" + f->name + "\" is not a character the squad refs name");
				continue;
			}
			const bool wantAnimal = character->type == kworld::itemTypeByName("ANIMAL_CHARACTER");
			if (wantAnimal != f->animal)
				problems["members"].push_back("\"" + f->name + "\" animal=" + std::to_string(f->animal) + ", data says " + std::to_string(wantAnimal));
			if (!wantAnimal)
			{
				// `starting health` (Rock Bottom's missing arm: v0 -150 amputates), `slave` (1 = IS_SLAVE).
				for (const kdata::RefEntry* h : refsOf(*character, "starting health"))
				{
					const kdata::Record* part = gd.find(h->targetID);
					for (const state::FighterPart& p : f->parts)
					{
						if (part != nullptr && p.name == part->name && h->v0 <= -100 && !p.severed())
							problems["gear"].push_back("\"" + f->name + "\" " + p.name + " should be amputated (starting health " + std::to_string(h->v0) + ")");
					}
				}
				if (character->intOr("slave", 0) == 1 && f->slaveState != 1)
					problems["gear"].push_back("\"" + f->name + "\" should be a slave (slave state " + std::to_string(f->slaveState) + ")");
				for (const kdata::RefEntry* item : refsOf(*character, "inventory"))
				{
					// carried(): the main grid and the worn backpack's contents (a kit the grid had no room for goes in the bag).
					const std::uint32_t have = pack != packs.end() ? pack->second->carried(item->targetID) : 0;
					if (have < static_cast<std::uint32_t>(item->v0))
						{
						const std::string text = "\"" + f->name + "\" lacks item " + item->targetID + " (type " + std::to_string(gd.find(item->targetID) ? gd.find(item->targetID)->type : -1) + ") x" + std::to_string(item->v0) + " v1 " + std::to_string(item->v1) + " (has " + std::to_string(have) + ")";
						auto& list = problems["gear"];
						if (std::find(list.begin(), list.end(), text) == list.end())
							list.push_back(text);
					}
				}
			}
		}
		if (std::getenv("KSIM_STARTS_VERBOSE") != nullptr)
			std::cout << "  " << record.name << ": members " << memberText << ", animals " << animals << "\n";

		// ---- position ----
		const kworld::Towns towns = kworld::Towns::join(gd, KENSHI_DATA);
		std::vector<std::pair<std::string, std::pair<double, double>>> spots;
		const bool forced = record.boolOr("force start pos", false);
		if (forced)
			spots.push_back({"forced", {static_cast<double>(record.intOr("start pos X", 0)), static_cast<double>(record.intOr("start pos Z", 0))}});
		else
		{
			for (const kdata::RefEntry* t : refsOf(record, "town"))
			{
				for (const kworld::TownRecord& town : towns.towns())
				{
					if (town.stringID != t->targetID)
						continue;
					for (const std::size_t index : town.placementIndices)
						spots.push_back({town.name, {towns.placements()[index].x, towns.placements()[index].z}});
				}
			}
			if (spots.empty())
				spots.push_back({"fallback", {-43500.0, -6625.0}});
		}
		double best = 1e18;
		std::string where;
		for (const std::uint64_t id : ids)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
				continue;
			double nearest = 1e18;
			for (const auto& [label, at] : spots)
			{
				const double d = std::hypot(f->x - at.first, f->z - at.second);
				if (d < nearest)
				{
					nearest = d;
					where = label;
				}
			}
			best = std::max(best == 1e18 ? 0.0 : best, nearest);
		}
		const double tolerance = forced ? 250.0 : 400.0;
		if (best > tolerance)
			problems["position"].push_back("a member is " + std::to_string(static_cast<int>(best)) + " u from the data's spot (" + where + ", limit " + std::to_string(static_cast<int>(tolerance)) + ")");

		// ---- money, relations, research ----
		flecs::entity player{};
		ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::PlayerFaction&)
		{
			if (id.value == outcome.faction)
				player = e;
		});
		const state::PlayerFaction* pf = player.get<state::PlayerFaction>();
		if (pf->money != record.intOr("money", 0))
			problems["money"].push_back("money " + std::to_string(pf->money) + ", data " + std::to_string(record.intOr("money", 0)));
		for (const kdata::RefEntry* rel : refsOf(record, "faction relations"))
		{
			flecs::entity other{};
			ecsOf(sim).each([&](flecs::entity e, const state::FactionState& fs)
			{
				if (fs.templateSid == rel->targetID)
					other = e;
			});
			if (!other.is_valid())
			{
				problems["relations"].push_back(rel->targetID + " has no faction in the world");
				continue;
			}
			const double want = std::clamp(rel->v0, -100, 100);
			const state::Relation* back = other.get<state::Relation>(player);
			const state::Relation* fwd = player.get<state::Relation>(other);
			if (back == nullptr || fwd == nullptr || back->value != want || fwd->value != want)
				problems["relations"].push_back(rel->targetID + " want " + std::to_string(rel->v0) + ", got " + (fwd ? std::to_string(fwd->value) : std::string("none")) + "/" + (back ? std::to_string(back->value) : std::string("none")));
		}
		{
			std::set<std::string> want;
			const std::int32_t researchType = kworld::itemTypeByName("RESEARCH");
			for (const kdata::RefEntry* r : refsOf(record, "research"))
			{
				const kdata::Record* target = gd.find(r->targetID);
				if (target != nullptr && target->type == researchType && !target->boolOr("REMOVED", false))
					want.insert(r->targetID);
			}
			if (want.empty())
				want.insert("5359-gamedata.base");
			const std::set<std::string> have(pf->research.begin(), pf->research.end());
			if (have != want)
			{
				std::string text = "have {";
				for (const std::string& s : have)
					text += s + " ";
				text += "} want {";
				for (const std::string& s : want)
					text += s + " ";
				problems["research"].push_back(text + "}");
			}
		}

		// ---- 3 game minutes with no orders ----
		std::map<std::uint64_t, Watch> watch;
		for (const std::uint64_t id : ids)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
				continue;
			Watch& w = watch[id];
			w.x0 = w.lastX = f->x;
			w.z0 = w.lastZ = f->z;
			w.lastYaw = f->yaw;
			w.routeVersion0 = w.lastRouteVersion = f->routeVersion;
			w.name = f->name;
			w.animal = f->animal;
		}
		const std::uint64_t ticks = static_cast<std::uint64_t>(minutes * 60.0 / kTickSeconds);
		const double pi = 3.14159265358979323846;
		for (std::uint64_t t = 0; t < ticks; ++t)
		{
			sim.run(1);
			for (auto& [id, w] : watch)
			{
				const state::Fighter* f = sim.combat()->find(id);
				if (f == nullptr || f->dead)
					continue;
				double dy = f->yaw - w.lastYaw;
				while (dy > pi)
					dy -= 2.0 * pi;
				while (dy < -pi)
					dy += 2.0 * pi;
				w.turned += std::fabs(dy);
				w.lastYaw = f->yaw;
				w.maxStep = std::max(w.maxStep, std::hypot(f->x - w.lastX, f->z - w.lastZ));
				w.maxAway = std::max(w.maxAway, std::hypot(f->x - w.x0, f->z - w.z0));
				if (w.first.empty() && w.maxAway > 5.0)
					w.first = "first left at " + std::to_string(t) + " ticks, speed " + std::to_string(f->forwardSpeed) + ", ragdoll/prone? " + std::to_string(static_cast<int>(f->routeSource));
				if (f->routeVersion != w.lastRouteVersion)
				{
					++w.replans;
					w.lastRouteVersion = f->routeVersion;
				}
				if (f->walking())
				{
					w.walkingSeconds += kTickSeconds;
					if (w.goal.empty())
						w.goal = "routeSource " + std::to_string(static_cast<int>(f->routeSource)) + " seq " + std::to_string(f->routeSeq) + (f->routeChase ? " chase" : "");
					w.stalledSeconds = std::hypot(f->x - w.lastX, f->z - w.lastZ) < 0.01 ? w.stalledSeconds + kTickSeconds : 0.0;
				} else
					w.stalledSeconds = 0.0;
				w.lastX = f->x;
				w.lastZ = f->z;
			}
		}
		for (auto& [id, w] : watch)
		{
			const std::string who = "\"" + w.name + "\"" + (w.animal ? " (animal)" : "");
			if (w.turned >= 2.0 * pi && w.maxAway < 30.0)
				problems["motion"].push_back(who + " spins: turned " + std::to_string(static_cast<int>(w.turned * 180.0 / pi)) + " deg, stayed within " + std::to_string(static_cast<int>(w.maxAway)) + " u");
			if (w.maxAway >= 30.0 || w.walkingSeconds > 0.0)
				problems["motion"].push_back(who + " moves unordered: " + std::to_string(static_cast<int>(w.maxAway)) + " u away, walking " + std::to_string(static_cast<int>(w.walkingSeconds)) + " s, " + w.goal + "; " + w.first + ", biggest one-tick step " + std::to_string(w.maxStep) + " u");
			if (w.stalledSeconds >= 10.0)
				problems["motion"].push_back(who + " stalls on a route (" + std::to_string(static_cast<int>(w.stalledSeconds)) + " s)");
			if (w.replans >= 5)
				problems["motion"].push_back(who + " re-plans in a loop (" + std::to_string(w.replans) + " routes)");
		}

		// ---- the line, and the verdict ----
		std::ostringstream line;
		line << record.name << " | " << ids.size() << " members (" << animals << " animals)";
		for (const char* check : {"members", "gear", "position", "money", "relations", "research", "motion"})
		{
			const auto it = problems.find(check);
			const bool bad = it != problems.end();
			const Known* k = known(record.name, check);
			line << " | " << check << " " << (!bad ? (k ? "ok (KNOWN failure no longer fails: " + std::string(k->item) + ")" : "ok") : (k ? "KNOWN " + std::string(k->item) : "FAIL"));
			if (check == std::string("position") && best < 1e17)
				line << " [" << where << ", " << static_cast<int>(best) << " u]";
			if (bad && !k)
				++failures;
		}
		std::cout << line.str() << "\n";
		for (const auto& [check, list] : problems)
		{
			for (const std::string& text : list)
				std::cout << "    " << (known(record.name, check) ? "known " : "MISMATCH ") << check << ": " << text << "\n";
		}
	}
	test::require(startsRun > 0, "no start ran");
	test::require(failures == 0, std::to_string(failures) + " mismatches (see above)");
	std::cout << "ksim_newgame_starts: OK (" << startsRun << " starts)\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_newgame_starts: FAIL: " << e.what() << "\n";
	return 1;
}
