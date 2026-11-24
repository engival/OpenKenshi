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
//   roll      (rwh.193 §1) gear through the NPC path: a worn piece is one the CHARACTER's `clothing` lists (v0 != 0, slot matches), at most one per slot, a slot
//             with a qualifying entry (v1 > 0) is filled; a carried weapon is one `weapons` / `crossbows` lists.
//             (The money-item roll a player's squad skips, §1.6, does not exist in the sim at all: no physical money item.)
//   bounty    (§2) a CHARACTER with `bounty chance` c > 0 may hold a bounty (c/101) from its `bounty factions` (empty = United Cities) of amount ..amount+fuzz;
//             none without a chance; a chance >= 100 fires on all but two of the extra seeds
//   placement (§3) over extra seeds a start with several town placements is placed on more than one of them
//   hunger    (§4) a member of a `malnourished` squad template (non-robot race) starts at 1.7, any other at 3.0. Platoon+0x110 is unread: skipped
//   animals   (§5) exactly the animal count the template lists; their age is the rolled one (0 / 0.4-0.5, Fighter::age)
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
#include <cstdio>
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

const char* const kChecks[] = {"members", "gear", "roll", "bounty", "position", "placement", "hunger", "animals", "money", "relations", "research", "motion"};

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


// The United Cities faction sid (`defaultEmpireFactionSID`: the bounty of a record with no `bounty factions`).
std::string unitedCities(const kdata::GameData& gd)
{
	for (const kdata::Record* faction : gd.byType(kworld::itemTypeByName("FACTION")))
	{
		if (faction->name == "United Cities")
			return faction->stringID;
	}
	return std::string();
}

// rwh.193 §2: what the CHARACTER's bounty may be: the factions and the amount range, empty when it has no chance.
struct BountyWant
{
	int chance = 0;
	std::set<std::string> factions;
	int low = 0;
	int high = 0;
};

BountyWant bountyWant(const kdata::GameData& gd, const kdata::Record& character)
{
	BountyWant out;
	out.chance = character.intOr("bounty chance", 0);
	if (out.chance <= 0)
		return out;
	for (const kdata::RefEntry* e : refsOf(character, "bounty factions"))
		out.factions.insert(e->targetID);
	if (out.factions.empty())
		out.factions.insert(unitedCities(gd));
	out.low = character.intOr("bounty amount", 0);
	out.high = out.low + character.intOr("bounty amount fuzz", 0);
	return out;
}

// Checks one human member's bounty against its CHARACTER (`got` = the faction -> amount the character holds); true when it holds one.
bool checkBounty(const BountyWant& want, const std::map<std::string, int>& got, const std::string& who, std::vector<std::string>& problems)
{
	if (want.chance <= 0)
	{
		if (!got.empty())
			problems.push_back(who + " holds a bounty but its record has no `bounty chance`");
		return false;
	}
	if (got.empty())
		return false;
	std::set<std::string> held;
	for (const auto& [faction, amount] : got)
	{
		held.insert(faction);
		if (amount < want.low || amount > want.high)
			problems.push_back(who + " bounty " + std::to_string(amount) + " on " + faction + " outside " + std::to_string(want.low) + ".." + std::to_string(want.high));
	}
	if (held != want.factions)
		problems.push_back(who + " bounty factions differ from the record's `bounty factions`");
	return true;
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
	std::cout << "start | members | gear | roll | bounty | position | placement | hunger | animals | money | relations | research | motion\n";
	// argv[2] = "k/n": only the starts at index k, k+n, ... (a part of the test; ctest runs the parts in parallel, kenshi-k16.5.21).
	int shard = 0;
	int shards = 1;
	if (argc > 2)
		std::sscanf(argv[2], "%d/%d", &shard, &shards);
	std::size_t startIndex = 0;
	for (const std::string& sid : names)
	{
		if (startIndex++ % static_cast<std::size_t>(shards) != static_cast<std::size_t>(shard))
			continue;
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
		const std::shared_ptr<const CombatData> combatData = CombatData::build(gd, KENSHI_DATA);
		sim.enableCombat(combatData, settings);
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
		std::map<std::uint64_t, const state::CrimeRecord*> crimes;
		ecsOf(sim).each([&](const state::Id& id, const state::CrimeRecord& crime) { crimes[id.value] = &crime; });
		// The CHARACTERs a `malnourished` squad template lists (rwh.193 §4), and the animal entries of every template (§5).
		std::set<std::string> malnourished;
		std::vector<const kdata::RefEntry*> animalEntries;
		double animalAgeRandom = 0.0;
		for (const std::string& templateSid : expect.templates)
		{
			const kdata::Record& tpl = *gd.find(templateSid);
			for (const char* list : {"leader", "squad", "squad2", "animals", "animals2"})
			{
				for (const kdata::RefEntry* e : refsOf(tpl, list))
				{
					if (tpl.boolOr("malnourished", false))
						malnourished.insert(e->targetID);
					if (list[0] == 'a')
						animalEntries.push_back(e);
				}
			}
			animalAgeRandom = std::max(animalAgeRandom, static_cast<double>(tpl.floatOr("animal age random", 0.0f)));
		}
		// One run's bounties: each human member against its record; per member name with a chance, whether it holds one.
		const auto checkBounties = [&](const NewGameOutcome& o, std::vector<std::string>& bad)
		{
			std::map<std::string, bool> hit;
			for (const std::uint64_t id : o.members)
			{
				const state::Fighter* f = sim.combat()->find(id);
				const kdata::Record* character = nullptr;
				for (const std::string& csid : expect.characters)
				{
					const kdata::Record* c = gd.find(csid);
					if (f != nullptr && c != nullptr && c->name == f->name)
						character = c;
				}
				if (character == nullptr)
					continue;
				std::map<std::string, int> got;
				if (const auto crime = crimes.find(id); crime != crimes.end())
				{
					for (const state::Bounty& b : crime->second->bounties)
						got[b.factionSid] = b.amount;
				}
				const BountyWant want = bountyWant(gd, *character);
				const bool held = checkBounty(want, got, "\"" + f->name + "\"", bad);
				if (want.chance >= 100)
					hit[f->name] = held;
			}
			return hit;
		};
		{
			std::vector<std::string> bad;
			checkBounties(outcome, bad);
			if (!bad.empty())
				problems["bounty"] = bad;
		}
		std::set<std::string> raceNames;
		std::size_t animals = 0;
		std::vector<std::pair<std::string, double>> animalAges; // (name, Fighter::age) per animal
		std::string memberText;
		// The squad as STARTS lists it, leader first: member i is NEW_GAME's i-th, by CHARACTER record (rolled names are random).
		std::vector<Simulation::StartReady::Member> squad;
		for (const Simulation::StartReady& status : sim.startStatuses())
		{
			if (status.sid == sid)
				squad = status.members;
		}
		if (squad.size() != ids.size())
			problems["members"].push_back("STARTS lists " + std::to_string(squad.size()) + " members, NEW_GAME made " + std::to_string(ids.size()));
		for (std::size_t index = 0; index < ids.size(); ++index)
		{
			const std::uint64_t id = ids[index];
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
			{
				problems["members"].push_back("member " + std::to_string(id) + " has no fighter");
				continue;
			}
			animals += f->animal ? 1 : 0;
			if (f->animal)
				animalAges.emplace_back(f->name, f->age);
			memberText += (memberText.empty() ? "" : ",") + f->name;
			const auto pack = packs.find(id);
			// The CHARACTER the member is: the refs name it by name (the fighter's name is the character's, a creator Spec aside).
			const kdata::Record* character = index < squad.size() ? gd.find(squad[index].character) : nullptr;
			if (character != nullptr && expect.characters.count(character->stringID) == 0)
				character = nullptr;
			if (character != nullptr)
			{
				// A `named` character carries a draw from its sex's name list (rwh.247); any other keeps the record's name.
				const std::vector<std::string>& list = f->look.female ? combatData->namesFemale : combatData->namesMale;
				const bool named = character->boolOr("named", false) && character->type != kworld::itemTypeByName("ANIMAL_CHARACTER");
				const bool ok = named ? std::find(list.begin(), list.end(), f->name) != list.end() : f->name == character->name;
				if (!ok)
					problems["members"].push_back("\"" + f->name + "\" is not " + (named ? "in the name list" : "the record name \"" + character->name + "\""));
			}
			if (character == nullptr)
			{
				problems["members"].push_back("\"" + f->name + "\" is not a character the squad refs name");
				continue;
			}
			const bool wantAnimal = character->type == kworld::itemTypeByName("ANIMAL_CHARACTER");
			if (wantAnimal != f->animal)
				problems["members"].push_back("\"" + f->name + "\" animal=" + std::to_string(f->animal) + ", data says " + std::to_string(wantAnimal));
			{
				const kdata::Record* race = gd.find(f->raceSid);
				const bool robot = race != nullptr && race->boolOr("is robot", false);
				const double wantHunger = malnourished.count(character->stringID) != 0 && !robot ? 1.7 : 3.0;
				if (std::fabs(f->hunger - wantHunger) > 0.01 /* one tick of hunger has passed */)
					problems["hunger"].push_back("\"" + f->name + "\" hunger " + std::to_string(f->hunger) + ", data says " + std::to_string(wantHunger));
			}
			if (!wantAnimal)
			{
				std::map<std::int32_t, int> perSlot;
				for (const state::FighterArmour& piece : f->armour)
				{
					++perSlot[piece.slot];
					bool listed = false;
					for (const kdata::RefEntry* e : refsOf(*character, "clothing"))
					{
						const kdata::Record* armour = gd.find(e->targetID);
						listed = listed || (armour != nullptr && e->v0 != 0 && armour->stringID == piece.sid && armour->intOr("slot", 0) == piece.slot);
					}
					if (!listed)
						problems["roll"].push_back("\"" + f->name + "\" wears " + piece.sid + " (slot " + std::to_string(piece.slot) + "), not a `clothing` entry of its record");
				}
				std::set<std::int32_t> wantSlots;
				for (const kdata::RefEntry* e : refsOf(*character, "clothing"))
				{
					const kdata::Record* armour = gd.find(e->targetID);
					if (armour != nullptr && armour->type == kworld::itemTypeByName("ARMOUR") && e->v0 != 0 && e->v1 > 0)
						wantSlots.insert(armour->intOr("slot", 0));
				}
				for (const std::int32_t slot : wantSlots)
				{
					if (perSlot[slot] != 1)
						problems["roll"].push_back("\"" + f->name + "\" has " + std::to_string(perSlot[slot]) + " pieces in slot " + std::to_string(slot) + ", wants exactly 1");
				}
				for (const auto& [slot, count] : perSlot)
				{
					if (count > 1 || wantSlots.count(slot) == 0)
						problems["roll"].push_back("\"" + f->name + "\" has " + std::to_string(count) + " pieces in slot " + std::to_string(slot) + " the record does not roll");
				}
				if (!f->weapon.sid.empty() && f->weapon.weaponType != 5)
				{
					bool listed = false;
					for (const char* list : {"weapons", "crossbows"})
					{
						for (const kdata::RefEntry* e : refsOf(*character, list))
							listed = listed || (e->targetID == f->weapon.sid && e->v0 > 0);
					}
					if (!listed)
						problems["roll"].push_back("\"" + f->name + "\" carries " + f->weapon.sid + ", not a `weapons` / `crossbows` entry of its record");
				}
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

		// ---- rwh.193 §5: the animals ----
		{
			std::int64_t low = 0;
			std::int64_t high = 0;
			for (const kdata::RefEntry* e : animalEntries)
			{
				low += e->v0;
				high += e->v1 == 0 ? e->v0 : std::max(e->v0, e->v1);
			}
			if (static_cast<std::int64_t>(animals) < low || static_cast<std::int64_t>(animals) > high)
				problems["animals"].push_back(std::to_string(animals) + " animals, the template lists " + std::to_string(low) + ".." + std::to_string(high));
			// age = rand * `animal age random` - 0.1 + v2 * 0.01, clamped >= 0 (Guy with a dog: 0, the Wandering Trader's bull: 0.4-0.5)
			for (const kdata::RefEntry* e : animalEntries)
			{
				const kdata::Record* target = gd.find(e->targetID);
				const double base = e->v2 * 0.01 - 0.1;
				const double lowAge = std::max(0.0, base);
				const double highAge = std::max(0.0, base + animalAgeRandom);
				for (const auto& [name, age] : animalAges)
				{
					if (target == nullptr || name != target->name)
						continue;
					if (age < lowAge - 1e-9 || age > highAge + 1e-9)
					{
						char text[96];
						std::snprintf(text, sizeof text, "animal %s: age %.3f outside %.2f..%.2f", name.c_str(), age, lowAge, highAge);
						problems["animals"].push_back(text);
					}
				}
			}
		}

		// ---- rwh.193 §3 / §2: extra seeds (a later start tick draws the place and the bounty again) ----
		{
			bool chanced = false;
			for (const std::string& csid : expect.characters)
				chanced = chanced || bountyWant(gd, *gd.find(csid)).chance > 0;
			if (!forced && (spots.size() > 1 || chanced))
			{
				constexpr int kRuns = 12;
				std::set<std::size_t> reached;
				std::map<std::string, int> hits;
				std::map<std::string, int> wantHits;
				NewGameOutcome again;
				bool got = false;
				sim.removeNewGameObserver(&outcome);
				sim.addNewGameObserver(&again, [&](const NewGameOutcome& o) { again = o; got = true; });
				for (int run = 0; run < kRuns; ++run)
				{
					sim.newWorld();
					sim.run(static_cast<std::uint64_t>(run + 1));
					again = NewGameOutcome{};
					got = false;
					sim.enqueue(Command::newGame(sid, "Tester", 1));
					sim.run(1);
					if (!got || !again.reason.empty() || again.members.empty())
					{
						problems["placement"].push_back("extra seed " + std::to_string(run) + " did not start: " + again.reason);
						break;
					}
					const state::Fighter* leader = sim.combat()->find(again.members.front());
					std::size_t nearest = 0;
					double best2 = 1e18;
					for (std::size_t i = 0; i < spots.size(); ++i)
					{
						const double d = std::hypot(leader->x - spots[i].second.first, leader->z - spots[i].second.second);
						if (d < best2)
						{
							best2 = d;
							nearest = i;
						}
					}
					reached.insert(nearest);
					crimes.clear();
					ecsOf(sim).each([&](const state::Id& id, const state::CrimeRecord& crime) { crimes[id.value] = &crime; });
					std::vector<std::string> bad;
					const std::map<std::string, bool> hit = checkBounties(again, bad);
					if (!bad.empty())
						problems["bounty"].insert(problems["bounty"].end(), bad.begin(), bad.end());
					for (const auto& [name, held] : hit)
					{
						++wantHits[name];
						hits[name] += held ? 1 : 0;
					}
				}
						sim.removeNewGameObserver(&again);
				if (spots.size() > 1 && reached.size() < 2)
					problems["placement"].push_back("all " + std::to_string(kRuns) + " seeds start on placement " + std::to_string(*reached.begin()) + " of " + std::to_string(spots.size()));
				for (const auto& [name, count] : hits)
				{
					if (count < wantHits[name] - 2)
						problems["bounty"].push_back("\"" + name + "\" (chance >= 100) holds a bounty on " + std::to_string(count) + " of " + std::to_string(wantHits[name]) + " seeds");
				}
				std::cout << "  " << record.name << ": " << reached.size() << " of " << spots.size() << " placements reached over " << kRuns << " seeds\n";
			}
		}

		// ---- the line, and the verdict ----
		std::ostringstream line;
		line << record.name << " | " << ids.size() << " members (" << animals << " animals)";
		for (const char* check : kChecks)
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
