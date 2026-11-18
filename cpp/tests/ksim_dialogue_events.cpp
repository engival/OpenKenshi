// Dialogue events, conditions and root selection (include/ksim/dialogue/dialogue_events.hpp, docs/DIALOGUE.md 8 slice 1 steps 3 and 4), on the real data:
//
//  1. the condition coverage: which kinds the evaluator implements, which fail closed (and are counted), how many of the slice-1 roots and
//     first lines are fully evaluable;
//  2. the scenarios of 8 step 7: a player in a Holy Citizen home -> SHOO every 2 s and no crime; a thug house -> the Bandit thug shoo root,
//     follow, attack; a witnessed theft -> the law root, the arrest and a 2500 bounty (CRIME_LIVE a); starving bandits sighting a player ->
//     the mugging root ("starver marks a target"), whose attack waits for the player's reply (slice 2);
//  3. determinism: the same inputs give the same picks, a different seed does not have to, and the personality input moves the line.

#include "ksim/world/buildings.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "knav/walk.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace cr = ksim::crime;

constexpr int kHonorable = 1;
constexpr int kNormal = 13;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

std::string str(std::size_t v)
{
	return std::to_string(v);
}

// The option a reply menu offers for the DIALOGUE_LINE `line`, -1 when it is not offered.
int optionOf(const Simulation::ReplyMenuEvent& menu, std::string_view line)
{
	for (std::size_t i = 0; i < menu.options.size(); ++i)
	{
		if (menu.options[i].first == line)
			return static_cast<int>(i);
	}
	return -1;
}

struct Data
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<const DialogueBook> book;
	std::string servant; // the body every rig character is built from
	std::string holy, nameless, bandits, starving, antiSlavers;
	std::string citizen; // "Holy Citizen"
	std::string thug; // a CHARACTER whose set roots "Bandit thug shoo" under SHOO
	std::string guard; // a law CHARACTER whose set roots "Law witness looting ally" under EV 42 and "Law spotting a generic crime" under EV 41
	std::string starver; // a CHARACTER with `dialogue package` 3781 (Starving Bandits)
	std::string grey; // "Grey": root "Grey talk to slave" (59269) under EV 1, its lines gated on DC_IS_SLAVE / DC_IS_ESCAPED_SLAVE
	std::string seta; // "High Inquisitor Seta": root "Inquisitor heretic bounty" (49639) under EV 1, its lines on DC_CARRYING_BOUNTY_*
	std::string police; // "Police Sergeant": package `police`, DA_ASSIGN_BOUNTY on the EV_THIEF_CAUGHT_STEALING_FROM_ME lines
	std::string dust; // "Dust Bandit": "Imprisoned thug" (13575, IS_IMPRISONED) under EV 1 and "Healed by ally" (9310, MET_TARGET_BEFORE)
	std::string hungry; // "Hungry bandit": "captured - meets stranger" (3310, IMPRISONED_BY_OTHER) and "- meets imprisoner" (3305) under EV 1
	std::string simion; // "Boss Simion": "Talk to simion ally (noble killed)" (1533640, TARGET_LAST_SEEN >= 24 h) under EV 1
	std::string gateGuard; // "Samurai Gate Guard": "Gate Guard Partner In Crime" (9521, WITHIN_TOWN_WALLS) under EV 3
	std::string paladin; // "Paladin": "Inquisitor searching player base" (96051, IN_A_PLAYER_TOWN) under EV 3
	// The slice-2 condition roots (task BS): "bodyguard finished job" (5095) under EV 30, "Looting" (7151) under EV 33, the
	// Rebirth slave trader (49695) under EV 1, the slaver's "slave escape" (5251) under EV 36, and King Gurgler (a carried body).
	std::string swordForHire, dustKing, captain, slaver, gurgler;
	std::string townSid; // a TOWN record of the Holy Nation
	double townX = 5000.0, townZ = 5000.0; // where the rig puts that town
	double wildX = -30000.0, wildZ = 1000.0; // outside every town

	Data()
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		data = CombatData::build(*gd, KENSHI_DATA);
		book = DialogueBook::build(gd);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "The Holy Nation")
				holy = r->stringID;
			if (r->name == "Nameless")
				nameless = r->stringID;
			if (r->name == "Dust Bandits")
				bandits = r->stringID;
			if (r->name == "Anti-Slavers")
				antiSlavers = r->stringID;
			if (r->stringID == "1305-gamedata.base")
				starving = r->stringID;
		}
		const auto rootsUnder = [&](const kdata::Record& character, int event, std::string_view root)
		{
			SpeakerKey key;
			key.character = character.stringID;
			key.leader = true;
			const PackageSet& set = book->packageSet(book->packageSetFor(key));
			const std::uint32_t index = book->dialogueIndex(root);
			for (const std::uint32_t position : set.rootsFor(event))
			{
				if (set.roots[position] == index)
					return true;
			}
			return false;
		};
		std::vector<const kdata::Record*> characters = gd->byType(kworld::itemTypeByName("CHARACTER"));
		std::sort(characters.begin(), characters.end(), [](const kdata::Record* a, const kdata::Record* b) { return a->stringID < b->stringID; });
		// The first CHARACTER called `name` whose package set roots `root` under `event` (several characters share a name).
		const auto findRooted = [&](std::string_view name, int event, std::string_view root)
		{
			for (const kdata::Record* r : characters)
			{
				if (r->name == name && rootsUnder(*r, event, root))
					return r->stringID;
			}
			return std::string();
		};
		for (const kdata::Record* r : characters)
		{
			if (r->name == "Escaped Servant" && servant.empty())
				servant = r->stringID;
			if (r->name == "Holy Citizen" && citizen.empty())
				citizen = r->stringID;
			if (thug.empty() && rootsUnder(*r, ev::ShooFromMyBuilding, "13509-Escapes.mod"))
				thug = r->stringID;
			if (guard.empty() && r->intOr("NPC class", 0) == cr::kClassLaw && rootsUnder(*r, ev::WitnessLootingAlly, "13657-Escapes.mod")
				&& rootsUnder(*r, ev::WitnessGenericAssault, "13643-Escapes.mod"))
				guard = r->stringID;
			if (starver.empty() && rootsUnder(*r, ev::ISeeEnemyPlayer, "4853-D-dialogue-02.mod")
				&& rootsUnder(*r, ev::ISeeNeutralSquad, "4853-D-dialogue-02.mod"))
				starver = r->stringID;
			if (r->name == "Grey" && grey.empty())
				grey = r->stringID;
			if (r->name == "High Inquisitor Seta" && seta.empty())
				seta = r->stringID;
			if (r->name == "Police Sergeant" && police.empty())
				police = r->stringID;
			if (r->name == "Dust Bandit" && dust.empty())
				dust = r->stringID;
			if (r->name == "Hungry bandit" && hungry.empty())
				hungry = r->stringID;
			if (r->name == "Boss Simion" && simion.empty())
				simion = r->stringID;
			if (r->name == "Samurai Gate Guard" && gateGuard.empty())
				gateGuard = r->stringID;
			if (r->name == "Paladin" && paladin.empty())
				paladin = r->stringID;
			if (r->name == "King Gurgler" && gurgler.empty())
				gurgler = r->stringID;
			if (r->name == "Sword for hire" && swordForHire.empty())
				swordForHire = r->stringID;
			if (r->name == "Samurai Captain" && captain.empty())
				captain = r->stringID;
		}
		dustKing = findRooted("Dust King", 33, "7151-Dialogue.mod");
		slaver = findRooted("Slaver", 36, "5251-gamedata.base");
		check(!swordForHire.empty() && !dustKing.empty() && !captain.empty() && !slaver.empty() && !gurgler.empty(),
			"data records: Sword for hire " + swordForHire + ", Dust King " + dustKing + ", Samurai Captain " + captain + ", Slaver "
				+ slaver + ", King Gurgler " + gurgler);
		check(!holy.empty() && !nameless.empty() && !bandits.empty() && !starving.empty() && !servant.empty(),
			"data records: factions Holy Nation / Nameless / Dust Bandits / Starving Bandits, the servant body");
		check(!citizen.empty() && !thug.empty() && !guard.empty() && !starver.empty(),
			"data records: Holy Citizen " + citizen + ", thug " + thug + ", law guard " + guard + ", starver " + starver);
		check(!antiSlavers.empty() && !grey.empty() && !seta.empty() && !police.empty(),
			"data records: Anti-Slavers, Grey " + grey + ", High Inquisitor Seta " + seta + ", Police Sergeant " + police);
		check(!dust.empty() && !hungry.empty() && !simion.empty() && !gateGuard.empty() && !paladin.empty(),
			"data records: Dust Bandit " + dust + ", Hungry bandit " + hungry + ", Boss Simion " + simion + ", Samurai Gate Guard "
				+ gateGuard + ", Paladin " + paladin);
		// The town record the rig places (Simulation::load makes templates only): a Holy Nation TOWN with a size radius.
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("TOWN")))
		{
			bool owned = false;
			if (const kdata::RefCategory* refs = r->findRefs("faction"))
			{
				for (const kdata::RefEntry& entry : refs->entries)
					owned = owned || entry.targetID == holy;
			}
			if (owned && r->floatOr("size radius", 0.0) > 200.0 && (townSid.empty() || r->stringID < townSid))
				townSid = r->stringID;
		}
		check(!townSid.empty(), "a Holy Nation TOWN record with a size radius: " + townSid);
	}
};

struct Rig
{
	const Data& w;
	std::unique_ptr<Simulation> sim;
	std::uint64_t rng = 3;
	std::vector<cr::DialogueEvent> events;
	bool fights = false;
	// Dialogue replies (a conversation that waits for the player): the menus the rig answers are read with Simulation::replyMenu.
	explicit Rig(const Data& world, std::uint64_t seed = 7, int hour = kNewGameHour, bool replyMenus = false)
		: w(world)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, hour, kNewGameMinute));
		sim->enableCombat(w.data, settings);
		sim->enableDialogue(w.book);
		sim->setDialogueObserver([this](const cr::DialogueEvent& e) { events.push_back(e); });
		if (replyMenus)
			sim->setReplyMenus(true);
	}

	// The player faction (the wallet, D5) and a squad for its character.
	void playerFaction()
	{
		sim->createPlayerFaction(w.nameless);
	}
	void joinPlayerSquad(std::uint64_t id)
	{
		sim->addPlayerMember(sim->addPlayerSquad(), id);
	}

	// A character of `character` (a CHARACTER sid, "" = none) in `faction` at (x, z) facing yaw.
	std::uint64_t add(const std::string& character, const std::string& faction, int cls, double x, double z, double yaw, bool player = false, int team = 0)
	{
		state::Fighter f = w.data->makeFighter(*w.gd, w.servant, "", rng);
		f.team = fights ? team : 0;
		f.x = x;
		f.z = z;
		f.yaw = yaw;
		f.npcClass = static_cast<std::uint8_t>(cls);
		f.playerControlled = player;
		f.attention = 0.5;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{ faction });
		if (!character.empty())
			e.set<state::CharacterInstance>(state::CharacterInstance{ character });
		return e.get<state::Id>()->value;
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	// The rig's houses are bare building ids with no geometry: the sim re-derives the building of a character that moves (stepBuildings),
	// so a house is pinned on its occupants after every tick (a dialogue follow moves the owner inside its house).
	std::vector<std::pair<std::uint64_t, std::uint64_t>> pinned; // (character, building)
	void house(std::uint64_t building, std::initializer_list<std::uint64_t> ids)
	{
		for (const std::uint64_t id : ids)
		{
			f(id).building = building;
			pinned.emplace_back(id, building);
		}
	}
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
		{
			sim->tick();
			for (const auto& [id, building] : pinned)
				f(id).building = building;
		}
	}
	std::uint64_t steal(std::uint64_t who, const std::string& owner)
	{
		state::Container box;
		box.recordSid = "box";
		box.x = f(who).x;
		box.z = f(who).z;
		box.y = f(who).y;
		box.useX = box.x;
		box.useY = box.y;
		box.useZ = box.z;
		box.ownerSid = owner;
		state::Inventory stock;
		for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("ITEM")))
		{
			stock.add(r->stringID, 1);
			sim->enqueue(Command::containerTransfer(state::Id{ who }, 0, r->stringID, 1, true, 1));
			break;
		}
		flecs::entity c = sim->spawnContainer(box, stock);
		const std::uint64_t id = c.get<state::Id>()->value;
		for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("ITEM")))
		{
			sim->enqueue(Command::containerTransfer(state::Id{ who }, id, r->stringID, 1, true, 1));
			break;
		}
		return id;
	}
	std::uint64_t count(int event) const { return sim->dialogueEventCount(event); }
	// A producer's message (EV 8, ...) at the current tick.
	void push(int event, std::uint64_t speaker, std::uint64_t target)
	{
		DialogueEvent message;
		message.event = static_cast<std::uint8_t>(event);
		message.speaker = speaker;
		message.target = target;
		message.tick = sim->clockTick();
		sim->dialogue()->push(message);
	}
	// A town of the Holy Nation (the real TOWN record) at the data's rig place: its FactionState owner and the TownState.
	void placeTown()
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity faction = state::createInstance(ecs);
		faction.set<state::FactionState>(state::FactionState{ w.holy, 0.0 });
		flecs::entity town = state::createInstance(ecs);
		town.set<state::TownState>(state::TownState{ w.townSid, w.townSid, state::EntityRef{ faction.id() }, w.townX, w.townZ });
	}
	// The player's own outpost (state::PlayerTown, BUILDINGS_SLICE 2) at (x, z): owned by the player faction, so DC_IN_A_PLAYER_TOWN
	// sees a player town there (playerFaction() first).
	void placePlayerTown(double x, double z, double radius)
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		std::uint64_t owner = 0;
		ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState&)
		{
			if (owner == 0)
				owner = entity.id();
		});
		flecs::entity town = state::createInstance(ecs);
		town.set<state::PlayerTown>(state::PlayerTown{"base", owner, x, z, radius, {}});
	}
	// The walls of a walled region (perception.hpp): a circle at (x, z) with no gates. The Simulation rebuilds the regions from the
	// gate sites every 200 ticks; this one holds off that rebuild for them (the sim has no gate sites in the rig).
	void placeWalls(double x, double z, double radius)
	{
		perception::Environment& environment = const_cast<CombatArena*>(sim->combat())->environmentMutable();
		environment.walled.clear();
		environment.walled.push_back({ x, z, radius, {} });
		environment.walledTick = sim->clockTick();
	}
	// The speaker's lock map entry for a root (state::DialogueMemory): `locked` false unlocks a `locked` record (the map DA_LOCK_THIS_DIALOG
	// and a line's unlocks write; the map is ascending by sid, one entry here).
	void setLock(std::uint64_t speaker, const std::string& root, bool locked)
	{
		flecs::entity entity{};
		sim->world().ecs().each([&](flecs::entity found, const state::Id& id)
		{
			if (id.value == speaker)
				entity = found;
		});
		state::DialogueMemory memory;
		if (const state::DialogueMemory* held = entity.get<state::DialogueMemory>())
			memory = *held;
		memory.locks.push_back(state::DialogueLock{ root, locked });
		entity.set<state::DialogueMemory>(std::move(memory));
	}
	// The character's entity (the rig's ids are state::Id values).
	flecs::entity entity(std::uint64_t id)
	{
		flecs::entity found{};
		const_cast<flecs::world&>(sim->world().ecs()).each([&](flecs::entity e, const state::Id& key)
		{
			if (key.value == id)
				found = e;
		});
		return found;
	}
	// An item in a character's inventory (the component is made on first use); a non-carried slot makes one worn piece.
	void giveItem(std::uint64_t id, const std::string& itemSid, std::uint32_t count, std::uint8_t slot = state::Inventory::kCarried)
	{
		flecs::entity target = entity(id);
		state::Inventory stock;
		if (const state::Inventory* held = target.get<state::Inventory>())
			stock = *held;
		stock.add(itemSid, count, slot);
		target.set<state::Inventory>(std::move(stock));
	}
	// A wound: the part of `type` (0 torso, 1 leg, 2 arm, 3 head) is left with `health` of its max, no stun.
	void wound(std::uint64_t id, int type, double health)
	{
		for (state::FighterPart& part : f(id).parts)
		{
			if (part.type == type)
			{
				part.health = health;
				part.stun = 0.0;
				return;
			}
		}
	}
	// A world squad (state::SquadInstance + SquadRoutine): its template, home building and whether its cages are locked.
	flecs::entity worldSquad(std::uint64_t homeBuilding, bool cagesLocked, const std::string& templateSid = "")
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity squad = state::createInstance(ecs);
		state::SquadInstance instance;
		instance.templateSid = templateSid;
		instance.homeBuilding = homeBuilding;
		squad.set<state::SquadInstance>(std::move(instance));
		squad.set<state::SquadRoutine>(state::SquadRoutine{ 0, 0, static_cast<std::uint8_t>(cagesLocked ? 1 : 0) });
		return squad;
	}
	// `character` joins `squad` (state::MemberOf, the Fighter's squad id).
	void join(std::uint64_t character, flecs::entity squad)
	{
		entity(character).add<state::MemberOf>(squad);
		f(character).squad = squad.get<state::Id>()->value;
	}
	// Advance to a tick whose dialogue step runs no producers (their 0.5 s cadence): the pushed event is the only one.
	void settle()
	{
		for (int i = 0; i < 20 && (sim->clockTick() + 1) % 10 == 0; ++i)
			sim->tick();
	}
	// The first picked result of `event` for `speaker` (a duplicate event is dropped as busy, so look for the pick).
	const DialoguePick* picked(int event, std::uint64_t speaker) const
	{
		for (const DialoguePick& p : dlg().log())
		{
			if (p.event.event == event && p.event.speaker == speaker && p.result == DialoguePick::Result::Picked)
				return &p;
		}
		return nullptr;
	}
	const DialogueSystem& dlg() const { return *sim->dialogue(); }
	std::vector<DialoguePick> picks(int event, std::uint64_t speaker) const
	{
		std::vector<DialoguePick> found;
		for (const DialoguePick& p : dlg().log())
		{
			if (p.event.event == event && p.event.speaker == speaker)
				found.push_back(p);
		}
		return found;
	}
	// The speaker's last pick of any event, nullptr when it never spoke.
	const DialoguePick* lastPick(std::uint64_t speaker) const
	{
		const DialoguePick* found = nullptr;
		for (const DialoguePick& p : dlg().log())
		{
			if (p.event.speaker == speaker)
				found = &p;
		}
		return found;
	}
	// Ticks until `done` (checked before each tick), at most `cap`.
	bool run(int cap, const std::function<bool()>& done)
	{
		for (int i = 0; i < cap && !done(); ++i)
			sim->tick();
		return done();
	}
};

std::string rootName(const DialogueBook& book, const DialoguePick& pick)
{
	return pick.dialogue == kNoIndex ? "-" : std::string(book.sidOf(book.dialogue(pick.dialogue).sid));
}

std::string describe(const DialogueBook& book, const DialoguePick& p)
{
	return std::string("ev ") + dialogueEventName(p.event.event) + " result " + str(static_cast<std::size_t>(p.result)) + " root " + rootName(book, p)
		+ " cands " + str(p.candidates) + " passed " + str(p.passed);
}

// ---- 1. coverage ----

void coverage(const Data& w)
{
	const DialogueBook& book = *w.book;
	const DialogueCensus& census = book.census();
	std::size_t used = 0, implemented = 0, passKinds = 0, closed = 0;
	std::size_t refsTotal = 0, refsImplemented = 0;
	for (int kind = 1; kind < kDialogueCondCount; ++kind)
	{
		if (census.condUse[kind] == 0)
			continue;
		++used;
		refsTotal += census.condUse[kind];
		if (dialogueCondImplemented(kind))
		{
			++implemented;
			refsImplemented += census.condUse[kind];
		} else if (kind == dc::FactionVariable || kind == dc::ImprisonmentIsDeathrow || kind == dc::MyMissionIsFriendly || kind == dc::TargetSquadSizeDisabled) {
			++passKinds;
		} else {
			++closed;
		}
	}
	// The kinds whose ids the exe has no handler for pass; kinds the data uses that we have not implemented fail closed.
	std::printf("    condition kinds used %zu: implemented %zu (%zu refs of %zu), no-handler (pass) %zu, fail closed %zu\n", used, implemented,
		refsImplemented, refsTotal, passKinds, closed);
	check(implemented >= 30 && implemented + passKinds + closed == used, "at least 30 condition kinds are implemented, the rest is counted as closed");

	// The slice-1 events over every package set the data can produce (one per distinct CHARACTER key): roots whose gate (and whose first lines'
	// gates) the evaluator can decide.
	DialogueSystem system(w.book, 1);
	std::set<std::uint32_t> roots;
	std::set<std::uint32_t> evaluableRoots, evaluableWithLines;
	const int events[] = { ev::ShooFromMyBuilding, ev::IntruderFound, ev::ISeeNeutralSquad, ev::ISeeEnemyPlayer, ev::ISeeAllyPlayer,
		ev::ThiefCaughtStealingFromMe, ev::WitnessLootingAlly, ev::WitnessThiefOrLockpick, ev::WitnessGenericAssault, ev::BountySpotted };
	for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("CHARACTER")))
	{
		for (const bool leader : { true, false })
		{
			SpeakerKey key;
			key.character = r->stringID;
			key.leader = leader;
			const PackageSet& set = book.packageSet(book.packageSetFor(key));
			for (const int event : events)
			{
				for (const std::uint32_t position : set.rootsFor(event))
					roots.insert(set.roots[position]);
			}
		}
	}
	for (const std::uint32_t index : roots)
	{
		const DialogueDef& root = book.dialogue(index);
		if (!system.gateEvaluable(root.gate))
			continue;
		evaluableRoots.insert(index);
		bool lines = true;
		for (const ChildRef& child : book.children(root.firstLines))
			lines = lines && system.gateEvaluable(book.line(child.line).gate);
		if (lines)
			evaluableWithLines.insert(index);
	}
	std::printf("    slice-1 events: %zu roots offered by some package set; root gate fully evaluable %zu, root and all first lines evaluable %zu\n",
		roots.size(), evaluableRoots.size(), evaluableWithLines.size());
	check(!roots.empty() && evaluableRoots.size() * 10 >= roots.size() * 5, "at least half of the slice-1 roots have a fully evaluable gate");
	for (const std::string_view sid : { "13509-Escapes.mod", "13643-Escapes.mod", "4853-D-dialogue-02.mod", "13926-Escapes.mod", "3124-gamedata.base" })
	{
		const std::uint32_t index = book.dialogueIndex(sid);
		check(index != kNoIndex && system.gateEvaluable(book.dialogue(index).gate), "root " + std::string(sid) + " is fully evaluable at its own gate");
	}
}

// A world of two hand-made characters, to drive the evaluator without a Simulation.
class FakeWorld : public DialogueWorld
{
public:
	std::map<std::uint64_t, DialogueActor> actors;
	double rel = 0.0;
	bool actor(std::uint64_t id, DialogueActor& out) const override
	{
		const auto found = actors.find(id);
		if (found == actors.end())
			return false;
		out = found->second;
		return true;
	}
	double relation(std::string_view from, std::string_view to) const override { return from == to ? 100.0 : rel; }
	bool copsAround(std::uint64_t) const override { return false; }
	void threat(std::uint64_t, int& enemies, int& allies) const override { enemies = allies = 0; }
};

void failClosed(const Data& w)
{
	// A root whose first condition is a kind the evaluator does not implement, with nothing else gating it (no masks, lists, chance): the gate
	// reaches the condition, fails closed and counts the kind. Searched over every CHARACTER and event of the data.
	const DialogueBook& book = *w.book;
	for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("CHARACTER")))
	{
		SpeakerKey key;
		key.character = r->stringID;
		key.leader = true;
		const PackageSet& set = book.packageSet(book.packageSetFor(key));
		for (int event = 1; event < kDialogueEventCount; ++event)
		{
			for (const std::uint32_t position : set.rootsFor(event))
			{
				const Gate& gate = book.dialogue(set.roots[position]).gate;
				std::span<const CondDef> conds = book.conds(gate.conds);
				while (!conds.empty() && conds[0].op == CondOp::Pass)
					conds = conds.subspan(1); // a no-handler condition always passes: the first real one decides
				if (conds.empty() || dialogueCondImplemented(conds[0].kind) || gate.reqPersonality != 0
					|| gate.forbPersonality != 0 || gate.reqMemory != 0 || gate.forbMemory != 0
					|| gate.locked || gate.targetType != 0 || gate.worldState.count != 0)
					continue;
				bool listed = false;
				for (const Span& list : gate.lists)
					listed = listed || list.count != 0;
				if (listed)
					continue;
				FakeWorld world;
				DialogueActor speaker;
				speaker.exists = true;
				speaker.id = 1;
				speaker.character = r->stringID;
				world.actors[1] = speaker;
				DialogueActor target;
				target.exists = true;
				target.id = 2;
				target.playerFaction = true;
				world.actors[2] = target;
				DialogueSystem system(w.book, 5);
				system.force(r->stringID, set.roots[position]); // the chance rolls pass: the gate reaches the condition
				DialogueEvent e;
				e.event = static_cast<std::uint8_t>(event);
				e.speaker = 1;
				e.target = 2;
				const std::uint64_t before = system.coverage().condFailClosed[conds[0].kind];
				const DialoguePick pick = system.evaluate(world, e);
				const std::string what = std::string(dialogueEventName(event)) + ", " + std::string(book.sidOf(book.dialogue(set.roots[position]).sid))
					+ ", " + dialogueCondName(conds[0].kind);
				check(system.coverage().condFailClosed[conds[0].kind] > before, "a closed condition kind is counted when the gate reaches it (" + what + ")");
				check(!(pick.result == DialoguePick::Result::Picked && pick.dialogue == set.roots[position]), "and the root is not picked (" + what + ")");
				return;
			}
		}
	}
	check(false, "found a root whose first condition fails closed to test the counter with");
}

// A FakeWorld with the town levels: `level` is the speaker's current town's size level (DC 73; -1 = it stands in no player town) and the
// player faction's best one (DC 74; -1 = it has no town).
class TownLevelWorld : public FakeWorld
{
public:
	int level = 0;
	bool currentTownLevel(std::uint64_t, int& out) const override
	{
		out = level;
		return level >= 0;
	}
	bool bestTownLevel(std::uint64_t, int& out) const override
	{
		out = level;
		return true;
	}
};

// DC_TOWN_LEVEL_CURRENT_LOCATION (73) and DC_PLAYERS_BEST_TOWN_LEVEL (74) read the level instead of failing closed: over the data's roots that
// carry the condition and pass at some level, the pick follows the condition's own comparison at every level 0..3 (below, at and above its
// value); with no town the condition is false (73) whatever its comparison.
void townLevelConditions(const Data& w)
{
	const DialogueBook& book = *w.book;
	for (const int kind : { dc::TownLevelCurrentLocation, dc::PlayersBestTownLevel })
	{
		check(dialogueCondImplemented(kind), std::string(dialogueCondName(kind)) + " is implemented");
		int pinned = 0, below = 0, at = 0, above = 0;
		std::set<std::uint32_t> seen;
		for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			SpeakerKey key;
			key.character = r->stringID;
			key.leader = true;
			const PackageSet& set = book.packageSet(book.packageSetFor(key));
			for (int event = 1; event < kDialogueEventCount; ++event)
			{
				for (const std::uint32_t position : set.rootsFor(event))
				{
					const std::uint32_t root = set.roots[position];
					const Gate& gate = book.dialogue(root).gate;
					const CondDef* cond = nullptr;
					for (const CondDef& c : book.conds(gate.conds))
						cond = c.kind == kind && c.op != CondOp::Pass ? &c : cond;
					DialogueSystem probe(w.book, 5);
					if (cond == nullptr || !probe.gateEvaluable(gate) || !seen.insert(root).second)
						continue;
					const auto holds = [&](int level)
					{
						switch (cond->op)
						{
						case CondOp::Eq: return level == cond->value;
						case CondOp::Lt: return level < cond->value;
						case CondOp::Gt: return level > cond->value;
						case CondOp::Ge: return level >= cond->value;
						default: return (level != 0) == (cond->value > 0);
						}
					};
					const auto picked = [&](int level)
					{
						TownLevelWorld world;
						world.level = level;
						DialogueActor speaker;
						speaker.exists = true;
						speaker.id = 1;
						speaker.character = r->stringID;
						DialogueActor target;
						target.exists = true;
						target.id = 2;
						target.playerFaction = true;
						target.playerTown = true;
						world.actors[1] = speaker;
						world.actors[2] = target;
						DialogueSystem system(w.book, 5);
						system.force(r->stringID, root);
						DialogueEvent e;
						e.event = static_cast<std::uint8_t>(event);
						e.speaker = 1;
						e.target = 2;
						const DialoguePick pick = system.evaluate(world, e);
						return pick.result == DialoguePick::Result::Picked && pick.dialogue == root;
					};
					bool follows = true, anyPick = false;
					for (int level = 0; level <= 3; ++level)
					{
						const bool got = picked(level);
						anyPick = anyPick || got;
						follows = follows && got == holds(level);
					}
					if (!anyPick || !follows || cond->value < 1 || cond->value > 3)
						continue;
					++pinned;
					below += picked(cond->value - 1) == holds(cond->value - 1) ? 1 : 0;
					at += picked(cond->value) == holds(cond->value) ? 1 : 0;
					above += picked(3) == holds(3) ? 1 : 0;
					check(!picked(-1) || kind == dc::PlayersBestTownLevel, "with no player town the current-town condition is false");
				}
			}
		}
		const std::string name = dialogueCondName(kind);
		check(pinned > 0, name + ": a root in the data follows the level at every level 0..3 (" + str(pinned) + ")");
		check(below == pinned && at == pinned && above == pinned, name + ": below, at and above its value agree with the comparison");
	}
}

// The same two conditions in a running sim (kenshi-k16.9.22, no fake DialogueWorld): the Adapter's currentTownLevel / bestTownLevel over
// real record towns the player faction owns and the PlayerBuilding rows inside them. A root carrying the condition is forced on a
// speaker, its event pushed at the player and one tick run (a fresh Town cache per pass); the dialogue log says whether it was picked. The pick has to
// follow the condition's own comparison at the level the town really has, level 0 -> 1 -> 2, in the town and outside it, and best over two.
const kdata::Record* levelBuilding(const Data& w, int function)
{
	for (const kdata::Record* record : w.gd->byType(kworld::itemTypeByName("BUILDING")))
	{
		buildings::TownCounters counters;
		buildings::countTownBuilding(*w.gd, *record, counters);
		if (function == buildings::kFunctionGate ? counters.gates >= 1 : counters.shells == 1 && counters.research == 0 && counters.turrets == 0 && counters.gates == 0)
			return record;
	}
	return nullptr;
}

void townLevelSim(const Data& w)
{
	const kdata::Record* shell = levelBuilding(w, 0);
	const kdata::Record* gate = levelBuilding(w, buildings::kFunctionGate);
	check(shell != nullptr && gate != nullptr, "data: a one-shell BUILDING and a gate BUILDING");
	constexpr double kFar = 4000.0; // the second town's offset
	for (const int kind : { dc::TownLevelCurrentLocation, dc::PlayersBestTownLevel })
	{
		struct Candidate { std::string character; std::uint32_t root; int event; CondDef cond; };
		std::vector<Candidate> candidates;
		std::set<std::uint32_t> seen;
		for (const kdata::Record* r : w.gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			SpeakerKey key;
			key.character = r->stringID;
			key.leader = true;
			const PackageSet& set = w.book->packageSet(w.book->packageSetFor(key));
			for (int event = 1; event < kDialogueEventCount; ++event)
			for (const std::uint32_t position : set.rootsFor(event))
			{
				const std::uint32_t root = set.roots[position];
				const Gate& gate = w.book->dialogue(root).gate;
				for (const CondDef& c : w.book->conds(gate.conds))
				{
					DialogueSystem probe(w.book, 5);
					if (c.kind == kind && c.op != CondOp::Pass && c.value >= 1 && c.value <= 3 && probe.gateEvaluable(gate) && seen.insert(root).second)
						candidates.push_back({r->stringID, root, event, c});
				}
			}
		}
		const auto holds = [](const CondDef& cond, int level)
		{
			switch (cond.op)
			{
			case CondOp::Eq: return level == cond.value;
			case CondOp::Lt: return level < cond.value;
			case CondOp::Gt: return level > cond.value;
			case CondOp::Ge: return level >= cond.value;
			default: return (level != 0) == (cond.value > 0);
			}
		};
				int pinned = 0;
		for (const Candidate& candidate : candidates)
		{
			if (pinned >= 6)
				break;
			Rig rig(w);
			rig.playerFaction();
			flecs::world& ecs = const_cast<flecs::world&>(rig.sim->world().ecs());
			flecs::entity owner{};
			ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState&) { owner = owner.is_valid() ? owner : entity; });
			const std::uint64_t ownerId = owner.get<state::Id>()->value;
			const auto placeTown = [&](double x, double z)
			{
				state::EntityRef ref;
				ref.id = owner.id();
				state::createInstance(ecs).set<state::TownState>(state::TownState{ w.townSid, w.townSid, ref, x, z });
			};
			const auto build = [&](const kdata::Record* record, double x, double z, int count)
			{
				for (int i = 0; i < count; ++i)
				{
					state::PlayerBuilding building;
					building.recordSid = record->stringID;
					building.ownerFaction = ownerId;
					building.x = x + i;
					building.z = z;
					building.built = true;
					ecs.entity().set<state::PlayerBuilding>(std::move(building));
				}
			};
			const std::uint64_t player = rig.add("", w.nameless, 9, w.townX + 10.0, w.townZ + 60.0, 3.14159265, true);
			rig.joinPlayerSquad(player);
			const auto picks = [&](double x, double z)
			{
				const std::uint64_t speaker = rig.add(candidate.character, kind == dc::PlayersBestTownLevel ? w.nameless : w.holy, cr::kClassLaw, x, z, 0.0, false); // the best-level roots want an ally
				rig.sim->dialogue()->force(candidate.character, candidate.root);
				const std::size_t before = rig.sim->dialogue()->log().size();
				rig.push(candidate.event, speaker, player);
				rig.tick();
				const auto& log = rig.sim->dialogue()->log();
				for (std::size_t i = before; i < log.size(); ++i)
					if (log[i].result == DialoguePick::Result::Picked && log[i].dialogue == candidate.root)
						return true;
				return false;
			};
			// With no player town at all: 73 fails whatever its comparison, 74 reads -1.
			const bool none = picks(w.townX, w.townZ);
			const bool noneWant = kind == dc::PlayersBestTownLevel && holds(candidate.cond, -1);
			placeTown(w.townX, w.townZ);
			placeTown(w.townX + kFar, w.townZ);
			// Town A: 0 buildings (level 0), then 2 shells (1), then a gate with 5 shells (2); town B: 2 shells (1) from the second pass.
			std::vector<bool> inA, outside, expected73, expected74;
			const auto sample = [&](int levelA, int best)
			{
				inA.push_back(picks(w.townX, w.townZ));
				outside.push_back(picks(w.townX + kFar / 2, w.townZ));
				expected73.push_back(kind == dc::TownLevelCurrentLocation ? holds(candidate.cond, levelA) : holds(candidate.cond, best));
			};
			sample(0, 0);
			build(shell, w.townX, w.townZ + 5.0, 2);
			sample(1, 1);
			build(shell, w.townX + kFar, w.townZ + 5.0, 2);
			sample(1, 1);
			build(shell, w.townX, w.townZ + 10.0, 3);
			build(gate, w.townX, w.townZ + 15.0, 1);
			sample(2, 2);
			bool follows = none == noneWant;
			bool any = none;
			for (std::size_t i = 0; i < inA.size(); ++i)
			{
				any = any || inA[i] || outside[i];
				follows = follows && inA[i] == expected73[i];
				if (kind == dc::TownLevelCurrentLocation)
					follows = follows && !outside[i];
				// best: where the speaker stands is not read, but other conditions of the root (in a player town) may care
			}
			if (!any)
				continue; // another condition of the gate fails in the rig; nothing to read here
			++pinned;
			check(follows, std::string(dialogueCondName(kind)) + ": " + candidate.character + " root " + str(candidate.root) + " follows level 0 / 1 / 1 / 2 in the town, outside it and with no town");
		}
		check(pinned > 0, std::string(dialogueCondName(kind)) + ": a root in the data is pinned in the running sim (" + str(pinned) + ")");
	}
}

// ---- 2. scenarios ----

// The SHOO scenario: `character` (of `faction`) at home in a building, a player character (Nameless) inside facing it, at night.
struct ShooResult
{
	std::vector<DialoguePick> picks;
	std::vector<std::uint64_t> shooTicks;
	std::uint64_t resident = 0, intruder = 0;
	bool crimeFree = false; // the intruder has no crime record at the end (CRIME_LIVE d: a shoo reports none)
};

ShooResult shoo(const Data& w, const std::string& character, const std::string& faction, std::uint64_t seed, int personality, int seconds = 12,
	std::uint64_t building = 777)
{
	Rig rig(w, seed, 23);
	const std::uint64_t kHome = building;
	const std::uint64_t resident = rig.add(character, faction, 4, 0.0, 0.0, 0.0, false);
	rig.f(resident).building = kHome;
	rig.f(resident).homeBuilding = kHome;
	rig.sim->dialogue()->setInputs(resident, DialogueInputs{ personality, false, false });
	const std::uint64_t intruder = rig.add("", w.nameless, 9, 0.0, 60.0, 3.14159265, true);
	rig.f(intruder).building = kHome;
	rig.house(kHome, { resident, intruder });
	rig.tick(20 * seconds);
	ShooResult out;
	out.resident = resident;
	out.intruder = intruder;
	out.picks = rig.picks(ev::ShooFromMyBuilding, resident);
	for (const DialoguePick& p : out.picks)
		out.shooTicks.push_back(p.event.tick);
	out.crimeFree = rig.sim->crimeRecord(intruder) == nullptr;
	return out;
}

void shooScenarios(const Data& w)
{
	const DialogueBook& book = *w.book;
	// A Holy Citizen at home, a stranger of the player's faction in the house at night: SHOO every 2 s, and the root is one the citizen's set roots
	// under SHOO ("Shoo civilian" 13926 and "shoo civ" 3124 are both in the Holy Nation civilian chain; the better scored passing one wins).
	{
		const ShooResult r = shoo(w, w.citizen, w.holy, 7, kNormal);
		check(r.picks.size() >= 4, "Holy Citizen home at night: EV_SHOO_FROM_MY_BUILDING fires repeatedly (" + str(r.picks.size()) + " in 12 s)");
		// Never faster than 2 s, and mostly at 2 s: the owner follows the stranger while it talks (DA_FOLLOW_WHILE_TALKING), and a refire is
		// skipped while its sensor has lost sight of it.
		bool every2s = r.picks.size() >= 2;
		std::size_t onTime = 0;
		for (std::size_t i = 1; i < r.shooTicks.size(); ++i)
		{
			every2s = every2s && r.shooTicks[i] - r.shooTicks[i - 1] >= 40;
			onTime += r.shooTicks[i] - r.shooTicks[i - 1] <= 50 ? 1 : 0;
		}
		every2s = every2s && 2 * onTime >= r.shooTicks.size() - 1;
		std::string gaps;
		for (std::size_t i = 1; i < r.shooTicks.size(); ++i)
			gaps += " " + str(r.shooTicks[i] - r.shooTicks[i - 1]);
		check(every2s, "the shoo re-fires every 2 s (not faster); gaps in ticks:" + gaps);
		check(r.crimeFree, "the shoo reports no crime against the intruder (CRIME_LIVE d)");
		SpeakerKey key;
		key.character = w.citizen;
		key.leader = true;
		const PackageSet& set = book.packageSet(book.packageSetFor(key));
		std::size_t picked = 0;
		bool inSet = true;
		for (const DialoguePick& p : r.picks)
		{
			std::printf("    %s\n", describe(book, p).c_str());
			if (p.result == DialoguePick::Result::Picked)
			{
				++picked;
				bool found = false;
				for (const std::uint32_t position : set.rootsFor(ev::ShooFromMyBuilding))
					found = found || set.roots[position] == p.dialogue;
				inSet = inSet && found;
			}
		}
		check(picked > 0 && inSet, "a SHOO root is picked, and it is one the citizen's set roots under SHOO");
	}
	// The thug house: the Bandit thug shoo root.
	{
		const ShooResult r = shoo(w, w.thug, w.bandits, 7, kNormal);
		std::size_t picked = 0;
		bool thugRoot = true;
		for (const DialoguePick& p : r.picks)
		{
			std::printf("    %s\n", describe(book, p).c_str());
			if (p.result == DialoguePick::Result::Picked)
			{
				++picked;
				thugRoot = thugRoot && rootName(book, p) == "13509-Escapes.mod";
			}
		}
		check(!r.picks.empty(), "thug house: the stranger draws SHOO events (" + str(r.picks.size()) + ")");
		check(picked > 0 && thugRoot, "thug house: the root is \"Bandit thug shoo\" (13509), first line \"Who the fuck are you?\" chain");
	}
	// A locked house is the trespass sensor's path, not a shoo (TRESPASS_LIVE).
	{
		Rig rig(w, 7, 23);
		const std::uint64_t resident = rig.add(w.citizen, w.holy, 4, 0.0, 0.0, 0.0, false);
		rig.f(resident).building = 777;
		rig.f(resident).homeBuilding = 777;
		rig.f(resident).houseLocked = true;
		const std::uint64_t intruder = rig.add("", w.nameless, 9, 0.0, 60.0, 3.14159265, true);
		rig.f(intruder).building = 777;
		rig.tick(20 * 10);
		check(rig.picks(ev::ShooFromMyBuilding, resident).empty(), "a locked house raises no SHOO");
	}
	// A friend of the house (same faction) is not shooed; nor is anyone in the street.
	{
		Rig rig(w, 7, 23);
		const std::uint64_t resident = rig.add(w.citizen, w.holy, 4, 0.0, 0.0, 0.0, false);
		rig.f(resident).building = 777;
		rig.f(resident).homeBuilding = 777;
		const std::uint64_t friendly = rig.add("", w.holy, 9, 0.0, 60.0, 3.14159265, true);
		rig.f(friendly).building = 777;
		const std::uint64_t outside = rig.add("", w.nameless, 9, 0.0, 80.0, 3.14159265, true);
		(void)outside;
		rig.tick(20 * 10);
		check(rig.picks(ev::ShooFromMyBuilding, resident).empty(), "a friendly character inside, and a stranger outside, raise no SHOO");
	}
}


// ---- 2b. the shoo's public-building gate (kinspect findings/dialogue_pacing, private) ----
// The exe's shoo predicate needs the building it resolves to be non-public: vf+0x290, the owning SQUAD_TEMPLATE's `public day` byte (the
// exe never reads `public night`; no hour and no door state). Live: a player stood at the Holy Barman's counter in Stack's public bar for
// 2.93 game hours and zero dialogue was spoken. A private home still shoos.
void shooGates(const Data& w)
{
	std::uint64_t publicBar = 0;
	std::uint64_t privateHome = 0;
	std::string publicSid;
	bool hubBarPublic = false;
	{
		Rig probe(w, 7);
		for (const BuildingSite& site : probe.sim->world().buildingSites())
		{
			const bool open = probe.sim->world().openToPublic(site.key);
			if (publicBar == 0 && open && site.label == "Bar")
			{
				publicBar = site.key;
				publicSid = site.squadTemplate;
			}
			if (privateHome == 0 && !open && !site.squadTemplate.empty())
				privateHome = site.key;
			// The user's own case: the Hub's Bar (3384-TwoStorey.mod at -51181, 2639, Bar blackmarket 44422: `public day`).
			if (site.label == "Bar" && std::hypot(site.cx + 51181.0, site.cz - 2639.0) < 300.0)
				hubBarPublic = open;
		}
	}
	check(publicBar != 0 && !publicSid.empty(), "the shipped world places a public Bar (label \"Bar\", squad template `public day`)");
	check(hubBarPublic, "the Hub's Bar is public (its barman can never shoo the player)");
	if (publicBar != 0)
	{
		// One game hour (kTicksPerGameHour ticks = 109 s wall at 1x) with the citizen at home and the player inside.
		const ShooResult r = shoo(w, w.citizen, w.holy, 7, kNormal, 110, publicBar);
		std::printf("    public bar %llx (%s): %zu SHOO events in one game hour\n", static_cast<unsigned long long>(publicBar),
			publicSid.c_str(), r.picks.size());
		check(r.picks.empty(), "a player inside a public bar for a game hour draws no SHOO");
	}
	// A placed private building (its template is not `public day`), else the bare id the other stages use.
	const std::uint64_t home = privateHome != 0 ? privateHome : 777;
	const ShooResult r = shoo(w, w.citizen, w.holy, 7, kNormal, 12, home);
	std::printf("    private home %llx: %zu SHOO events in 12 s\n", static_cast<unsigned long long>(home), r.picks.size());
	check(!r.picks.empty(), "a private home still shoos the same stranger");
}

bool isRootOf(const DialogueBook& book, const DialoguePick& pick, std::string_view sid)
{
	return pick.result == DialoguePick::Result::Picked && pick.dialogue == book.dialogueIndex(sid);
}

bool firstLineOf(const DialogueBook& book, const DialoguePick& pick)
{
	for (const ChildRef& child : book.children(book.dialogue(pick.dialogue).firstLines))
	{
		if (child.line == pick.line)
			return true;
	}
	return false;
}

void theft(const Data& w)
{
	const DialogueBook& book = *w.book;
	// The guard of a town sees the player steal from a Holy Nation container: EV_WITNESS_LOOTING_ALLY (42) at the thief, crime 3 as the reason; the
	// guard's set offers "Law witness looting ally" (13657). (The "Law spotting a generic crime" root, 13643, hangs on EV 41, the generic
	// crimes: see `assault`; CRIME_STATIC's event table sends theft to 42.)
	Rig rig(w, 7);
	rig.placeTown();
	const std::uint64_t guard = rig.add(w.guard, w.holy, cr::kClassLaw, w.townX, w.townZ, 0.0, false);
	const std::uint64_t thief = rig.add("", w.nameless, 9, w.townX, w.townZ + 60.0, 3.14159265, true);
	rig.sim->dialogue()->setInputs(guard, DialogueInputs{ kHonorable, false, false });
	rig.tick(20 * 3);
	rig.steal(thief, w.holy);
	rig.tick(20 * 6);
	check(rig.count(cr::kEvWitnessLootingAlly) > 0, "the guard witnesses the theft (EV 42 counted by the crime hook)");
	const std::vector<DialoguePick> picks = rig.picks(ev::WitnessLootingAlly, guard);
	check(!picks.empty() && picks.front().event.target == thief && picks.front().event.reason == cr::kTheft,
		"the event reaches the dialogue queue with the thief as target and crime 3 as the reason");
	std::size_t spoken = 0;
	for (const DialoguePick& p : picks)
	{
		std::printf("    %s\n", describe(book, p).c_str());
		if (isRootOf(book, p, "13657-Escapes.mod") && firstLineOf(book, p))
			++spoken;
	}
	check(spoken > 0, "the guard would speak \"Law witness looting ally\" (13657), one of its first lines");
	// Step 7: the line bites. The guard arrests the thief (the melee branch) and the theft is committed as a 2500 bounty (CRIME_LIVE a).
	bool arrested = false;
	for (int i = 0; i < 20 * 20 && !arrested; ++i)
	{
		rig.tick();
		arrested = rig.f(guard).orderTarget == thief;
	}
	const state::CrimeRecord* record = rig.sim->crimeRecord(thief);
	const std::int32_t bounty = record != nullptr ? cr::bountyOf(*record, w.holy) : -1;
	check(arrested && bounty == 2500, "the guard's line arrests the thief and the theft becomes a 2500 bounty (" + str(bounty) + ")");
}

void assault(const Data& w)
{
	const DialogueBook& book = *w.book;
	// The assault is recorded directly (the attack path of stepCrime needs a fight whose first swing is a roll, ksim_crime covers it): the player
	// character carries a pending Assault against the Holy Nation with the reportCrime timer, the guard sees it.
	Rig rig(w, 7);
	rig.placeTown();
	const std::uint64_t guard = rig.add(w.guard, w.holy, cr::kClassLaw, w.townX, w.townZ, 0.0, false);
	const std::uint64_t victim = rig.add("", w.holy, 4, w.townX + 40.0, w.townZ + 40.0, 3.14159265, false);
	const std::uint64_t attacker = rig.add("", w.nameless, 9, w.townX, w.townZ + 68.0, 3.14159265, true);
	rig.sim->dialogue()->setInputs(guard, DialogueInputs{ kHonorable, false, false });
	rig.tick(20 * 3);
	check(rig.sim->reportCrime(attacker, cr::kAssault, w.holy, victim, cr::kTimerAssault), "an assault is recorded against the Holy Nation");
	rig.tick(20 * 6);
	check(rig.count(cr::kEvWitnessGenericAssault) > 0, "the guard witnesses the assault (EV 41)");
	const std::vector<DialoguePick> picks = rig.picks(ev::WitnessGenericAssault, guard);
	std::size_t spoken = 0;
	std::set<std::uint32_t> lines;
	for (const DialoguePick& p : picks)
	{
		std::printf("    %s\n", describe(book, p).c_str());
		if (isRootOf(book, p, "13643-Escapes.mod") && firstLineOf(book, p))
		{
			++spoken;
			lines.insert(p.line);
		}
	}
	check(spoken > 0, "the guard would speak \"Law spotting a generic crime\" (13643), one of its first lines");
	// The "Stop!" line wants PT_HONORABLE (and the "Assistance!" line the leader), so the personality input decides which lines can be chosen.
	check(!lines.empty() && lines.count(book.lineIndex("13644-Escapes.mod")) + lines.count(book.lineIndex("13646-Escapes.mod")) > 0,
		"with an honorable personality the \"Stop!\" / \"Assistance!\" lines are the candidates");
	// The personality input moves the line: "Stop!" (13644) wants PT_HONORABLE, so a normal guard never opens with it.
	FakeWorld world;
	DialogueActor speaker;
	speaker.exists = true;
	speaker.id = 1;
	speaker.character = w.guard;
	speaker.faction = w.holy;
	speaker.npcClass = cr::kClassLaw;
	speaker.leader = true;
	speaker.inNonPlayerTown = true;
	DialogueActor target;
	target.exists = true;
	target.id = 2;
	target.playerFaction = true;
	target.faction = w.nameless;
	target.inNonPlayerTown = true;
	target.z = 60.0;
	world.actors[1] = speaker;
	world.actors[2] = target;
	const std::uint32_t stop = book.lineIndex("13644-Escapes.mod");
	for (const int personality : { kHonorable, kNormal })
	{
		DialogueSystem system(w.book, 9);
		system.setInputs(1, DialogueInputs{ personality, false, false });
		std::set<std::uint32_t> seen;
		for (std::uint64_t tick = 0; tick < 300; ++tick)
		{
			DialogueEvent e;
			e.event = ev::WitnessGenericAssault;
			e.speaker = 1;
			e.target = 2;
			e.tick = tick;
			const DialoguePick p = system.evaluate(world, e);
			if (isRootOf(book, p, "13643-Escapes.mod"))
				seen.insert(p.line);
		}
		check(!seen.empty() && (seen.count(stop) != 0) == (personality == kHonorable),
			std::string("personality ") + std::to_string(personality) + (personality == kHonorable ? ": \"Stop!\" is among the opening lines" : ": \"Stop!\" never opens")
			+ " (" + str(seen.size()) + " distinct lines)");
	}
}

void starving(const Data& w)
{
	const DialogueBook& book = *w.book;
	// In the sim: starving bandit squad leaders out in the wild and a player character in view.
	{
		Rig rig(w, 7);
		std::vector<std::uint64_t> bandits;
		const std::uint64_t player = rig.add("", w.nameless, 9, w.wildX, w.wildZ + 60.0, 3.14159265, true);
		for (int i = 0; i < 12; ++i)
		{
			const std::uint64_t id = rig.add(w.starver, w.starving, 8, w.wildX + 4.0 * i, w.wildZ, 0.0, false);
			rig.sim->dialogue()->setInputs(id, DialogueInputs{ kNormal, false, false });
			bandits.push_back(id);
		}
		rig.tick(20 * 4);
		std::size_t sighted = 0, mugging = 0;
		for (const std::uint64_t id : bandits)
		{
			for (const int event : { ev::ISeeEnemyPlayer, ev::ISeeNeutralSquad, ev::ISeeAllyPlayer })
			{
				for (const DialoguePick& p : rig.picks(event, id))
				{
					++sighted;
					check(p.event.target == player, "the sighting names the player character");
					if (isRootOf(book, p, "4853-D-dialogue-02.mod"))
						++mugging;
					else
						std::printf("    %s\n", describe(book, p).c_str());
				}
			}
		}
		check(sighted >= bandits.size(), "every starving bandit that sees the player raises an I_SEE event (" + str(sighted) + ")");
		check(mugging > 0, "some starving bandit's pick is \"starver marks a target\" (" + str(mugging) + " of " + str(sighted) + ")");
	}
	// The chances, on hand-made facts: the root is 75% permanent (a hash of speaker and record) and 40% temporary (a hash of the tick).
	{
		FakeWorld world;
		DialogueActor target;
		target.exists = true;
		target.id = 1000;
		target.playerFaction = true;
		target.faction = w.nameless;
		target.race = "x";
		target.squadSize = 1;
		world.actors[1000] = target;
		world.rel = -50.0;
		DialogueSystem system(w.book, 11);
		const std::uint32_t starverRoot = book.dialogueIndex("4853-D-dialogue-02.mod");
		int open = 0, shut = 0;
		for (std::uint64_t id = 1; id <= 24; ++id)
		{
			DialogueActor speaker;
			speaker.exists = true;
			speaker.id = id;
			speaker.character = w.starver;
			speaker.faction = w.starving;
			speaker.race = "x";
			speaker.leader = true;
			speaker.squadSize = 1;
			world.actors[id] = speaker;
			system.setInputs(id, DialogueInputs{ kNormal, false, false });
			int hits = 0;
			for (std::uint64_t tick = 0; tick < 400; ++tick)
			{
				DialogueEvent e;
				e.event = ev::ISeeEnemyPlayer;
				e.speaker = id;
				e.target = 1000;
				e.tick = tick;
				const DialoguePick p = system.evaluate(world, e);
				hits += p.result == DialoguePick::Result::Picked && p.dialogue == starverRoot ? 1 : 0;
			}
			const bool permanent = static_cast<double>(dialogueRoll(11, id, 0, book.dialogue(starverRoot).sid, 2) % 10000) / 100.0 < 75.0;
			if (permanent)
			{
				++open;
				check(hits > 400 * 0.25 && hits < 400 * 0.55, "speaker " + str(id) + ": the permanent chance holds, the temporary one lets ~40% through (" + str(hits) + " of 400)");
			} else {
				++shut;
				check(hits == 0, "speaker " + str(id) + ": the permanent chance failed, never picked");
			}
		}
		check(open > 0 && shut > 0, "both permanent outcomes occurred among 24 speakers (" + str(open) + " / " + str(shut) + ")");
		// Forced (ksim_server --force-dialogue): every speaker of the character picks the root at once, the chance rolls skipped.
		DialogueSystem forced(w.book, 11);
		forced.force(w.starver, starverRoot);
		int picked = 0;
		for (std::uint64_t id = 1; id <= 24; ++id)
		{
			forced.setInputs(id, DialogueInputs{ kNormal, false, false });
			DialogueEvent e;
			e.event = ev::ISeeEnemyPlayer;
			e.speaker = id;
			e.target = 1000;
			const DialoguePick p = forced.evaluate(world, e);
			picked += p.result == DialoguePick::Result::Picked && p.dialogue == starverRoot ? 1 : 0;
		}
		check(picked == 24, "a forced root is every speaker's pick (" + str(picked) + " of 24)");
	}
}

// Task BV (kinspect dt12): a Dust Bandit fight starts by dialogue, not by a squad order. SQUAD_TEMPLATE 1302's `dialog leader` package is
// DIALOGUE_PACKAGE 3185; it roots DIALOGUE 3186 "bandit fight picking new target in wilds" on EV_I_SEE_NEUTRAL_SQUAD (3), chance 20
// permanent / 20 temporary, a monologue, leader only. The 3225-3767 branch (a target weaker than me) ends in DA_ATTACK_CHASE_FOREVER
// (action 6: markHostile + attack). The engine test: the leader of a Dust Bandit squad in the wild sees a weaker player; the root is its
// pick, and the tree leaves the player its enemy. Whether the original's live boss ever gets this far is UNSURE (dt10 far4 saw the
// dialogue evaluated and refused; docs/FEATURES.md).
void dustBanditSight(const Data& w)
{
	const DialogueBook& book = *w.book;
	const std::uint32_t root = book.dialogueIndex("3186-gamedata.base");
	check(root != kNoIndex, "DIALOGUE 3186 exists");
	if (root != kNoIndex)
	{
		const DialogueDef& dialogue = book.dialogue(root);
		check(dialogue.gate.chancePerm == 20.0f && dialogue.gate.chanceTemp == 20.0f,
			"3186's chance is 20 permanent / 20 temporary (the data)");
	}
	// The squad template's leader route is what a world squad uses: `dialog leader` = 3185.
	const SpeakerKey leader{ "1301-gamedata.base", "1302-gamedata.base", w.bandits.c_str(), true, false, false };
	const PackageSet& set = book.packageSet(book.packageSetFor(leader));
	bool rooted = false;
	for (const std::uint32_t position : set.rootsFor(ev::ISeeNeutralSquad))
		rooted = rooted || (root != kNoIndex && set.roots[position] == root);
	check(rooted, "SQUAD_TEMPLATE 1302's leader package roots 3186 on EV_I_SEE_NEUTRAL_SQUAD");
	// The tree's end: line 3767 carries DA_ATTACK_CHASE_FOREVER (6; 55 and 57 are the same effect).
	const std::uint32_t end = book.lineIndex("3767-gamedata.base");
	bool attacks = false;
	if (end != kNoIndex)
		for (const ActDef& act : book.effects(book.line(end).effects))
			attacks = attacks || act.kind == 6 || act.kind == 55 || act.kind == 57;
	check(attacks, "line 3767 ends the tree in DA_ATTACK_CHASE_FOREVER");
	// In the sim: two Dust Bandits of squad template 1302, the leader seeing a weaker player in the wild.
	Rig rig(w, 7);
	const flecs::entity squad = rig.worldSquad(0, false, "1302-gamedata.base");
	rig.sim->createPlayerFaction(w.nameless);
	const std::uint64_t boss = rig.add("1301-gamedata.base", w.bandits, 8, w.wildX, w.wildZ, 0.0);
	const std::uint64_t mate = rig.add("", w.bandits, 8, w.wildX + 6.0, w.wildZ, 0.0);
	rig.join(boss, squad);
	rig.join(mate, squad);
	const std::uint64_t player = rig.add("", w.nameless, 9, w.wildX, w.wildZ + 15.0, 3.14159265, true);
	rig.f(player).skills.toughness = 1.0; // DC_WEAKER_THAN_ME, the 3225 branch's gate
	rig.sim->dialogue()->force("1301-gamedata.base", root, [](const std::string& line) { std::printf("    force: %s\n", line.c_str()); });
	rig.tick(20 * 4);
	std::size_t sighted = 0, picked = 0;
	for (const DialoguePick& p : rig.picks(ev::ISeeNeutralSquad, boss))
	{
		++sighted;
		check(p.event.target == player, "the sighting names the player character");
		std::printf("    %s\n", describe(book, p).c_str());
		picked += p.dialogue == root ? 1 : 0;
	}
	check(sighted > 0, "the leader raises I_SEE_NEUTRAL_SQUAD for the player (" + str(sighted) + ")");
	check(picked > 0, "the forced root is the leader's pick (" + str(picked) + ")");
	// ...and the tree ends in the attack.
	// The leader gate, off the sim's plumbing: a hand-staged world draws the forced root as a leader, and not as a plain member.
	const auto staged = [&](bool leader, std::uint64_t id)
	{
		FakeWorld world;
		DialogueActor speaker;
		speaker.exists = true;
		speaker.id = id;
		speaker.character = "1301-gamedata.base";
		speaker.faction = w.bandits;
		speaker.leader = leader;
		speaker.squadSize = 2;
		speaker.stat = 50.0;
		DialogueActor target;
		target.exists = true;
		target.id = 1000;
		target.playerFaction = true;
		target.faction = w.nameless;
		target.stat = 1.0; // WEAKER_THAN_ME, the 3225 branch's gate
		target.z = 15.0;
		world.actors[id] = speaker;
		world.actors[1000] = target;
		DialogueSystem system(w.book, 11);
		system.force("1301-gamedata.base", root);
		system.setInputs(id, DialogueInputs{ kNormal, false, false });
		DialogueEvent e;
		e.event = ev::ISeeNeutralSquad;
		e.speaker = id;
		e.target = 1000;
		return system.evaluate(world, e);
	};
	const DialoguePick asLeader = staged(true, 3);
	check(asLeader.result == DialoguePick::Result::Picked && asLeader.dialogue == root,
		"a leader with every gate open draws 3186 (" + str(static_cast<int>(asLeader.result)) + ")");
	const DialoguePick asMember = staged(false, 4);
	check(asMember.result != DialoguePick::Result::Picked, "a plain squad member draws nothing (DC_IS_LEADER)");
	// UNSURE: the line chain from 3225 does not reach DA_ATTACK_CHASE_FOREVER in the sim (the conversation ends before 3767); the
	// ending line and its action are checked at the data level above. docs/FEATURES.md (task BV).
}

// kenshi-cym.28 (rwh.97): the Skin Bandits' "sees player" tree, at the data level. Root 62880 sits under EV_I_SEE_NEUTRAL_SQUAD in both the
// base package 47658 and the patrol package 63275, and its chain ends in line 63230: DA_ATTACK_CHASE_FOREVER (6) + DA_MASSIVE_ALARM (56).
// UNSURE: the rig never advances the welcome chain past its first line (65175), so the chain to 63230 and the alarm's reach are checked by the
// SkinHouse soak (ksim_soak --dialogue on --event-log), not here.
void skinBanditAlarm(const Data& w)
{
	const DialogueBook& book = *w.book;
	const std::uint32_t root = book.dialogueIndex("62880-Dialogue.mod");
	check(root != kNoIndex, "DIALOGUE 62880 'Skin bandit sees player' exists");
	const std::uint32_t end = book.lineIndex("63230-Dialogue.mod");
	bool attacks = false, alarms = false;
	if (end != kNoIndex)
		for (const ActDef& act : book.effects(book.line(end).effects))
		{
			attacks = attacks || act.kind == 6;
			alarms = alarms || act.kind == 56;
		}
	check(attacks && alarms, "line 63230 carries DA_ATTACK_CHASE_FOREVER (6) and DA_MASSIVE_ALARM (56)");
}

// ---- 3. conversations (step 5) ----

void conversations(const Data& w)
{
	const DialogueBook& book = *w.book;
	Rig rig(w, 7, 23);
	std::vector<std::pair<std::uint64_t, Simulation::SpeechEvent>> said;
	rig.sim->setSpeechObserver([&](const Simulation::SpeechEvent& e) { said.emplace_back(rig.sim->clockTick(), e); });
	const std::uint64_t kHome = 777;
	const std::uint64_t resident = rig.add(w.citizen, w.holy, 4, 0.0, 0.0, 0.0, false);
	rig.f(resident).building = kHome;
	rig.f(resident).homeBuilding = kHome;
	rig.sim->dialogue()->setInputs(resident, DialogueInputs{ kNormal, false, false });
	const std::uint64_t intruder = rig.add("", w.nameless, 9, 0.0, 60.0, 3.14159265, true);
	rig.house(kHome, { resident, intruder });
	const auto talking = [&] { return rig.sim->conversation(resident); };
	// Run until the citizen speaks, then save mid-line.
	int waited = 0;
	while (talking() == nullptr && waited++ < 20 * 12)
		rig.tick();
	check(talking() != nullptr && !said.empty(), "a picked SHOO opens a conversation on the citizen and speaks its first line");
	const Simulation::SpeechEvent first = said.front().second;
	std::printf("    \"%s\" (%s / %s, %.1f s)\n", first.text.c_str(), first.dialogue.c_str(), first.line.c_str(), first.seconds);
	check(!first.end && first.speaker == resident && first.target == intruder && !first.text.empty()
		&& first.seconds >= 2.0 + 0.12 * static_cast<double>(first.text.size()) - 1e-9
		&& std::abs(first.seconds - static_cast<double>(talking()->until - said.front().first) * kTickSeconds) < 0.06,
		"SPEECH: the citizen to the intruder, a text, a duration of length x 0.12 + 2 s or the forced timer when longer");
	check(book.dialogueIndex(first.dialogue) != kNoIndex && book.lineIndex(first.line) != kNoIndex, "SPEECH names a compiled root and line");
	const state::DialogueShown* shown = rig.sim->world().ecs().get<state::DialogueShown>();
	const auto stamped = [&](const std::string& sid)
	{
		return shown != nullptr && std::any_of(shown->stamps.begin(), shown->stamps.end(), [&](const state::ShownStamp& s) { return s.sid == sid; });
	};
	check(stamped(first.line) && stamped(first.dialogue), "the spoken line and its root are stamped for the repetition limit");
	{
		std::stringstream bytes;
		rig.sim->save(bytes);
		const std::uint64_t before = rig.sim->stateHash();
		rig.sim->reload(bytes);
		check(rig.sim->stateHash() == before && talking() != nullptr && talking()->line == first.line,
			"save / load mid-line keeps the conversation and the hash");
	}
	// While it talks, new SHOOs are dropped (busy), and a line holds for its timer.
	rig.tick(20 * 12);
	std::size_t busy = 0;
	for (const DialoguePick& p : rig.picks(ev::ShooFromMyBuilding, resident))
		busy += p.result == DialoguePick::Result::Busy ? 1 : 0;
	bool spaced = true;
	for (std::size_t i = 1; i < said.size(); ++i)
	{
		if (said[i - 1].second.speaker == said[i].second.speaker && !said[i].second.end)
			spaced = spaced && static_cast<double>(said[i].first - said[i - 1].first) * kTickSeconds + 0.06 >= said[i - 1].second.seconds;
	}
	for (const auto& [tick, e] : said)
		std::printf("    t%llu %s: \"%s\"\n", static_cast<unsigned long long>(tick), e.line.c_str(), e.text.c_str());
	std::printf("    %zu lines in 24 s, %zu SHOOs dropped as busy\n", said.size(), busy);
	check(said.size() >= 2 && spaced, "lines follow one another no faster than their speech timers");
	check(busy > 0, "a SHOO that arrives while the citizen talks is dropped (busy)");
	// The same root is not shown again within its repetition limit (global per record): the citizen falls silent.
	{
		int more = 0;
		while (talking() != nullptr && more++ < 20 * 120)
			rig.tick();
		const std::string root = said.front().second.dialogue;
		const std::size_t before = said.size();
		rig.tick(20 * 12);
		bool again = false;
		for (std::size_t i = before; i < said.size(); ++i)
			again = again || said[i].second.dialogue == root;
		check(talking() == nullptr && !again, "after the conversation, its root (within its repetition limit) is not picked again");
	}
	// A speaker knocked out mid-line: SPEECH_END and the conversation is gone.
	{
		Rig ko(w, 7, 23);
		std::vector<Simulation::SpeechEvent> heard;
		ko.sim->setSpeechObserver([&](const Simulation::SpeechEvent& e) { heard.push_back(e); });
		const std::uint64_t citizen = ko.add(w.citizen, w.holy, 4, 0.0, 0.0, 0.0, false);
		ko.f(citizen).building = kHome;
		ko.f(citizen).homeBuilding = kHome;
		ko.sim->dialogue()->setInputs(citizen, DialogueInputs{ kNormal, false, false });
		const std::uint64_t stranger = ko.add("", w.nameless, 9, 0.0, 60.0, 3.14159265, true);
		ko.house(kHome, { citizen, stranger });
		int more = 0;
		while (ko.sim->conversation(citizen) == nullptr && more++ < 20 * 12)
			ko.tick();
		ko.f(citizen).unconscious = true;
		ko.f(citizen).koTimer = 600.0;
		ko.tick();
		check(ko.sim->conversation(citizen) == nullptr && heard.size() >= 2 && heard.back().end && heard.back().speaker == citizen,
			"a speaker knocked out mid-line: SPEECH_END and the conversation ends");
	}
}

// ---- 3b. effects (step 6) ----

class LockedWorld : public FakeWorld
{
public:
	std::uint32_t lockedRecord = kNoIndex;
	bool lockState(std::uint64_t, std::uint32_t record, bool& locked) const override
	{
		if (record != lockedRecord)
			return false;
		locked = true;
		return true;
	}
};

void effects(const Data& w)
{
	const DialogueBook& book = *w.book;
	const std::uint64_t kHome = 777;
	// The thug house ("Bandit thug shoo" 13509): the first line follows the intruder (DA_FOLLOW_WHILE_TALKING); a later one, the intruder still
	// inside and not running, attacks (DA_ATTACK_CHASE_FOREVER: TEMP_ENEMY on the thug's squad and a chase with no range), which ends the follow.
	{
		Rig rig(w, 7, 23);
		const std::uint64_t thug = rig.add(w.thug, w.bandits, 4, 0.0, 0.0, 0.0, false);
		rig.f(thug).building = kHome;
		rig.f(thug).homeBuilding = kHome;
		rig.sim->dialogue()->setInputs(thug, DialogueInputs{ kNormal, false, false });
		const std::uint64_t intruder = rig.add("", w.nameless, 9, 0.0, 60.0, 3.14159265, true);
		rig.house(kHome, { thug, intruder });
		int waited = 0;
		while (rig.sim->conversation(thug) == nullptr && waited++ < 20 * 12)
			rig.tick();
		const state::Conversation* talk = rig.sim->conversation(thug);
		check(talk != nullptr && talk->line == "13510-Escapes.mod" && talk->follower == thug && rig.f(thug).followTarget == intruder,
			"thug: \"Who the fuck are you?\" (13510) and the thug follows the intruder while it talks");
		bool attacked = false;
		for (int i = 0; i < 20 * 20 && !attacked; ++i)
		{
			rig.tick();
			attacked = rig.f(thug).orderTarget == intruder;
		}
		const std::vector<std::uint64_t>& hostile = rig.f(thug).hostileSquads;
		check(attacked && std::binary_search(hostile.begin(), hostile.end(), rig.f(intruder).squad) && rig.f(thug).followTarget == 0,
			"thug: the attack line orders the chase, tags the intruder's squad TEMP_ENEMY and ends the follow");
		check(rig.dlg().coverage().actApplied[16] > 0 && rig.dlg().coverage().actApplied[6] > 0, "coverage counts FOLLOW_WHILE_TALKING and ATTACK_CHASE_FOREVER as applied");
	}
	// Starving bandits: "Hand over your food" (4871) holds for DA_FORCE_SPEECH_TIMER 15 s and its `unlock but keep me` writes the lock map:
	// "Starver interrupt" (97949) unlocked, the running root (4853) locked. Save / load keeps the map and the hash.
	{
		Rig rig(w, 7);
		const std::uint64_t player = rig.add("", w.nameless, 9, w.townX, w.townZ + 60.0, 3.14159265, true);
		std::vector<std::uint64_t> bandits;
		for (int i = 0; i < 12; ++i)
		{
			const std::uint64_t id = rig.add(w.starver, w.starving, 8, w.townX + 4.0 * i, w.townZ, 0.0, false);
			rig.sim->dialogue()->setInputs(id, DialogueInputs{ kNormal, false, false });
			bandits.push_back(id);
		}
		std::uint64_t speaker = 0;
		for (int i = 0; i < 20 * 40 && speaker == 0; ++i)
		{
			rig.tick();
			for (const std::uint64_t id : bandits)
			{
				const state::Conversation* talk = rig.sim->conversation(id);
				if (talk != nullptr && talk->line == "4871-dialogue-02.mod")
					speaker = id;
			}
		}
		check(speaker != 0, "a starving bandit reaches \"Hand over your food\" (4871)");
		if (speaker != 0)
		{
			const state::Conversation* talk = rig.sim->conversation(speaker);
			check(talk->until >= rig.sim->clockTick() + 20 * 15 - 2, "DA_FORCE_SPEECH_TIMER 15: the line holds 15 s");
			const state::DialogueMemory* memory = rig.sim->dialogueMemory(speaker);
			const auto lockOf = [&](std::string_view sid) -> int
			{
				if (memory == nullptr)
					return -1;
				for (const state::DialogueLock& lock : memory->locks)
				{
					if (lock.sid == sid)
						return lock.locked ? 1 : 0;
				}
				return -1;
			};
			check(lockOf("97949-Dialogue.mod") == 0 && lockOf("4853-D-dialogue-02.mod") == 1,
				"`unlock but keep me`: Starver interrupt unlocked, the running root locked, in the speaker's map");
			std::stringstream bytes;
			rig.sim->save(bytes);
			const std::uint64_t before = rig.sim->stateHash();
			rig.sim->reload(bytes);
			check(rig.sim->stateHash() == before && rig.sim->dialogueMemory(speaker) != nullptr, "save / load keeps the lock map and the hash");
			// Step 7: the threat is a monologue whose children (all weight 100) are 4872, 4880 (whose child is "...and you might live" 4881)
			// and the target's own answer "..." (98039). The roll at the line's end tick picks one, and the corrected speech timer (1/100 game
			// hour, TASK N) moved that tick so the target's line is now the pick here (it used to land on 4880). Either path: the player
			// character answers by itself (the automatic path, 140672bf0) and the attack ("Fine. Have it your way..." 98118) follows. No reply
			// menu: a menu needs the player's talk (slice 2, below).
			bool threatened = false, attacked = false, answered = false;
			std::uint64_t answer = 0;
			rig.sim->setSpeechObserver([&](const Simulation::SpeechEvent& e)
			{
				if (e.speaker == player && (e.flags & 4) != 0)
				{
					answered = true;
					answer = e.line.rfind("98039-", 0) == 0 ? 1 : 2;
				}
			});
			for (int i = 0; i < 20 * 90 && !attacked; ++i)
			{
				rig.tick();
				const state::Conversation* talk = rig.sim->conversation(speaker);
				threatened = threatened || (talk != nullptr && talk->line == "4881-dialogue-02.mod");
				for (const std::uint64_t id : bandits)
					attacked = attacked || rig.f(id).orderTarget == player;
			}
			std::printf("    the threat went through 4881: %s\n", threatened ? "yes" : "no (straight to the answer)");
			check(answered && answer == 1, "the player answers \"...\" (98039) by itself");
			check(attacked, "the answer's child (98118) attacks the player");
		}
	}
	// Slice 2, reply menus (DIALOGUE.md 3.5 / 13). The mugging's lines set their `interrupt` (Starver interrupt 97949, unlocked by 4871):
	// the player's talk to the busy mugger opens it in place of the threat, as EV_PLAYER_TALK_TO_ME, so it is interactive although a
	// monologue: "Drop your weapons and hand over your Cats..." (97952) and a menu of the player's answers (97956..97959). The menu waits,
	// survives a save, re-sends with the same seq and texts, refuses a stale seq, a missing option and the wrong character; the reply is
	// the player's line. end_talk ends a menu with CONVERSATION_END 4; a talk that opens nothing is nothing_to_say.
	{
		struct Mugging
		{
			Rig rig;
			std::vector<Simulation::ReplyMenuEvent> menus;
			std::vector<Simulation::SpeechEvent> said;
			std::vector<std::pair<std::uint64_t, std::uint64_t>> nothing;
			std::uint64_t player = 0, speaker = 0;
			std::vector<std::uint64_t> bandits;

			explicit Mugging(const Data& w) : rig(w, 7)
			{
				rig.sim->setReplyMenus(true);
				rig.sim->setReplyMenuObserver([this](const Simulation::ReplyMenuEvent& e) { menus.push_back(e); });
				rig.sim->setSpeechObserver([this](const Simulation::SpeechEvent& e) { said.push_back(e); });
				rig.sim->setTalkObserver([this](std::uint64_t actor, std::uint64_t target, std::uint32_t) { nothing.emplace_back(actor, target); });
				player = rig.add("", w.nameless, 9, w.townX, w.townZ + 60.0, 3.14159265, true);
				for (int i = 0; i < 12; ++i)
				{
					const std::uint64_t id = rig.add(w.starver, w.starving, 8, w.townX + 4.0 * i, w.townZ, 0.0, false);
					rig.sim->dialogue()->setInputs(id, DialogueInputs{ kNormal, false, false });
					bandits.push_back(id);
				}
				for (int i = 0; i < 20 * 40 && speaker == 0; ++i)
				{
					rig.tick();
					for (const std::uint64_t id : bandits)
					{
						const state::Conversation* talk = rig.sim->conversation(id);
						if (talk != nullptr && talk->line == "4871-dialogue-02.mod")
							speaker = id;
					}
				}
				check(speaker != 0 && menus.empty(), "the mugging (a monologue) opens no menu by itself");
				said.clear();
				rig.sim->talkTo(player, speaker);
				rig.tick();
			}
		};
		Mugging m(w);
		const state::Conversation* talk = m.rig.sim->conversation(m.speaker);
		check(talk != nullptr && talk->dialogue == "97949-Dialogue.mod" && talk->line == "97952-Dialogue.mod" && talk->event == 1,
			"the talk interrupts the threat: Starver interrupt (97949), \"Drop your weapons...\" (97952)");
		check(m.menus.size() == 1 && !m.menus[0].end && m.menus[0].speaker == m.speaker && m.menus[0].target == m.player
			&& !m.menus[0].options.empty() && talk != nullptr && talk->replies.size() == m.menus[0].options.size(),
			"a reply menu for the player character, held by the conversation");
		if (m.menus.size() == 1 && talk != nullptr)
		{
			const Simulation::ReplyMenuEvent menu = m.menus[0];
			for (const auto& option : menu.options)
				std::printf("    option %s \"%s\"\n", option.first.c_str(), option.second.c_str());
			m.menus.clear();
			m.said.clear();
			for (int k = 0; k < 20 * 60; ++k)
				m.rig.tick();
			talk = m.rig.sim->conversation(m.speaker);
			check(talk != nullptr && talk->line == "97952-Dialogue.mod" && m.menus.empty() && m.said.empty(), "an open menu waits a minute: no timeout, no next line");
			check(m.rig.sim->checkDialogueReply(m.player, m.speaker, menu.menuSeq + 1, 0) == "stale", "a reply with another seq is stale");
			check(m.rig.sim->checkDialogueReply(m.player, m.speaker, menu.menuSeq, static_cast<std::uint32_t>(menu.options.size())) == "stale",
				"an option past the menu is stale");
			check(m.rig.sim->checkDialogueReply(m.bandits[0] == m.speaker ? m.bandits[1] : m.bandits[0], m.speaker, menu.menuSeq, 0) == "bad_target",
				"a reply from another character is refused");
			std::stringstream bytes;
			m.rig.sim->save(bytes);
			const std::uint64_t before = m.rig.sim->stateHash();
			m.rig.sim->reload(bytes);
			const std::optional<Simulation::ReplyMenuEvent> again = m.rig.sim->replyMenu(m.speaker);
			check(m.rig.sim->stateHash() == before && again && again->resend && again->menuSeq == menu.menuSeq && again->options == menu.options,
				"save / load keeps the menu; the resend has its seq and texts");
			m.rig.sim->enqueue(Command::dialogueReply(state::Id{m.player}, state::Id{m.speaker}, menu.menuSeq, 0));
			m.rig.tick();
			check(!m.said.empty() && m.said[0].speaker == m.player && m.said[0].target == m.speaker && (m.said[0].flags & 4) != 0 && m.said[0].line == menu.options[0].first
				&& m.said[0].text == menu.options[0].second, "the reply is the player's line to the speaker, with the menu's text");
			talk = m.rig.sim->conversation(m.speaker);
			check(talk == nullptr || talk->replies.empty(), "the reply closes the menu");
		}
		Mugging q(w);
		check(q.menus.size() == 1 && !q.menus[0].end, "a second mugging, a menu");
		if (q.menus.size() == 1)
		{
			const Simulation::ReplyMenuEvent first = q.menus[0];
			q.menus.clear();
			check(q.rig.sim->checkEndConversation(q.bandits[0] == q.speaker ? q.bandits[1] : q.bandits[0], q.speaker) == "bad_target",
				"end_talk from another character is refused");
			q.rig.sim->enqueue(Command::endConversation(state::Id{q.player}, state::Id{q.speaker}, 4));
			q.rig.tick();
			check(q.rig.sim->conversation(q.speaker) == nullptr && q.menus.size() == 1 && q.menus[0].end && q.menus[0].reason == 4
				&& q.menus[0].menuSeq == first.menuSeq, "end_talk closes it: CONVERSATION_END reason 4");
		}
		Mugging l(w);
		if (l.menus.size() == 1)
		{
			// The leash (§13.4): carried off 600 units (past 2 x TALKING_RANGE) with the menu open, the player walked away.
			l.menus.clear();
			l.rig.f(l.player).x += 600.0;
			l.rig.tick();
			check(l.rig.sim->conversation(l.speaker) == nullptr && l.menus.size() == 1 && l.menus[0].end && l.menus[0].reason == 4,
				"a menu past its leash ends: CONVERSATION_END reason 4");
		}
		check(q.nothing.empty(), "an answered talk is not nothing_to_say");
		q.rig.sim->talkTo(q.speaker, q.player);
		q.rig.tick();
		check(q.nothing.size() == 1 && q.nothing[0].first == q.speaker && q.nothing[0].second == q.player,
			"a talk to a character with nothing to say: the talk observer hears it (nothing_to_say)");
	}
	// The gate reads the lock map: a locked root is refused (counted), the same root unlocked passes.
	{
		LockedWorld world;
		DialogueActor target;
		target.exists = true;
		target.id = 1000;
		target.playerFaction = true;
		target.faction = w.nameless;
		target.race = "x";
		world.actors[1000] = target;
		world.rel = -50.0;
		DialogueActor speaker;
		speaker.exists = true;
		speaker.id = 1;
		speaker.character = w.starver;
		speaker.faction = w.starving;
		speaker.race = "x";
		speaker.leader = true;
		world.actors[1] = speaker;
		const std::uint32_t root = book.dialogueIndex("4853-D-dialogue-02.mod");
		const auto picks = [&](bool locked)
		{
			world.lockedRecord = locked ? book.dialogue(root).sid : kNoIndex;
			DialogueSystem system(w.book, 11);
			system.setInputs(1, DialogueInputs{ kNormal, false, false });
			int hits = 0;
			for (std::uint64_t tick = 0; tick < 400; ++tick)
			{
				DialogueEvent e;
				e.event = ev::ISeeEnemyPlayer;
				e.speaker = 1;
				e.target = 1000;
				e.tick = tick;
				const DialoguePick p = system.evaluate(world, e);
				hits += p.result == DialoguePick::Result::Picked && p.dialogue == root ? 1 : 0;
			}
			return std::make_pair(hits, system.coverage().lockedFails);
		};
		const auto open = picks(false);
		const auto shut = picks(true);
		check(open.first > 0 && shut.first == 0 && shut.second > open.second,
			"a root locked in the speaker's map is refused (" + str(open.first) + " picks open, " + str(shut.first) + " locked)");
	}
	// The law: "Stop!" / "Assistance!" (13643) arrest (the melee branch: TEMP_ENEMY and the attack) and raise the alarm (the crime timer to 30 s).
	{
		Rig rig(w, 7);
		rig.placeTown();
		const std::uint64_t guard = rig.add(w.guard, w.holy, cr::kClassLaw, w.townX, w.townZ, 0.0, false);
		const std::uint64_t victim = rig.add("", w.holy, 4, w.townX + 40.0, w.townZ + 40.0, 3.14159265, false);
		const std::uint64_t attacker = rig.add("", w.nameless, 9, w.townX, w.townZ + 68.0, 3.14159265, true);
		rig.sim->dialogue()->setInputs(guard, DialogueInputs{ kHonorable, false, false });
		rig.tick(20 * 3);
		rig.sim->reportCrime(attacker, cr::kAssault, w.holy, victim, cr::kTimerAssault);
		bool arrested = false;
		for (int i = 0; i < 20 * 8 && !arrested; ++i)
		{
			rig.tick();
			arrested = rig.f(guard).orderTarget == attacker;
		}
		const state::CrimeRecord* record = rig.sim->crimeRecord(attacker);
		check(arrested && record != nullptr && record->timer > 25.0, "the guard's arrest line attacks the criminal and its alarm sets the crime timer to 30 s");
		// DA_CRIME_ALARM force-notices the criminal (140858f40(S.threat, T, 1)); only perception::notice writes sinceNoticed (kenshi-h2v.27.7).
		const state::SightRecord* told = perception::find(rig.f(guard), attacker);
		check(told != nullptr && told->state == 1 && told->sinceNoticed < 10.0, "the guard's alarm line force-noticed the criminal");
	}
}

// ---- 3d. slaves and bounties (TASK AY: DIALOGUE.md slice 2) ----

// DC_IS_SLAVE (59) and DC_IS_ESCAPED_SLAVE (62) on the sim's slave state (TOWN_LIFE_SLICE 5.19): Grey's "Grey talk to slave" (59269)
// under EV 1 has one line per state, 59270 (SLAVE or ESCAPING) and 59272 (ESCAPING or EX-SLAVE). A free character draws neither.
void slaves(const Data& w)
{
	const auto talk = [&](int slaveState)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.grey, w.book->dialogueIndex("59269-Dialogue.mod"));
		const std::uint64_t grey = rig.add(w.grey, w.antiSlavers, 8, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		rig.f(player).slaveState = static_cast<std::uint8_t>(slaveState);
		if (slaveState != 0)
			rig.f(player).captorSid = w.antiSlavers;
		rig.sim->talkTo(player, grey);
		rig.tick();
		const state::Conversation* talk = rig.sim->conversation(grey);
		const DialoguePick* pick = rig.lastPick(grey);
		return std::make_pair(pick != nullptr ? pick->result : DialoguePick::Result::NoCandidates,
			talk != nullptr ? talk->line : std::string());
	};
	const auto slave = talk(1);
	check(slave.first == DialoguePick::Result::Picked && slave.second == "59270-Dialogue.mod",
		"a slave draws Grey's DC_IS_SLAVE line 59270 (line \"" + slave.second + "\")");
	// State 3 (EX-SLAVE) isolates DC_IS_ESCAPED_SLAVE: IS_SLAVE is 1 or 2 only, so 59270 cannot pass.
	const auto exSlave = talk(3);
	check(exSlave.first == DialoguePick::Result::Picked && exSlave.second == "59272-Dialogue.mod",
		"an escaped (ex-)slave draws the DC_IS_ESCAPED_SLAVE line 59272 (line \"" + exSlave.second + "\")");
	const auto free = talk(0);
	check(free.first == DialoguePick::Result::NoneSurvived && free.second.empty(),
		"a free character draws neither slave line (none survived)");
}

// The bounty table behind the hand-in (DA_PAY_BOUNTY 10) and the clear (DA_CLEAR_BOUNTY 49), with the conditions that gate their lines:
// High Inquisitor Seta's "Inquisitor heretic bounty" (49639) under EV 1 greets a player carrying a wanted body (49648 alive, 49656 dead,
// DC_CARRYING_BOUNTY_ALIVE / _DEAD 4 / 5), and "What about payment?" walks to 49678, which hands the bounty in. The police "police see
// thief" (12572) under EV 8 assigns one (DA_ASSIGN_BOUNTY 32) that the same table then pays and clears.
void bounties(const Data& w)
{
	const std::uint64_t kHome = 777;
	struct HandIn
	{
		std::string greeting;
		std::uint64_t applied = 0;
		std::int64_t money = 0;
		bool paidTwice = false;
		bool saved = false;
	};
	const auto handIn = [&](bool dead)
	{
		HandIn out;
		Rig rig(w, 7, kNewGameHour, true);
		rig.playerFaction();
		const std::uint64_t seta = rig.add(w.seta, w.holy, cr::kClassMilitary, 100.0, 100.0, 3.14159265, false);
		rig.f(seta).building = kHome;
		rig.f(seta).homeBuilding = kHome;
		rig.house(kHome, { seta });
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		rig.joinPlayerSquad(player);
		const std::uint64_t heretic = rig.add("", w.bandits, 8, 104.0, 100.0, 0.0);
		rig.f(heretic).dead = dead;
		rig.f(heretic).unconscious = true;
		rig.f(heretic).carriedBy = player;
		rig.f(player).carrying = heretic;
		rig.sim->addBounty(heretic, w.holy, 2500);
		rig.sim->talkTo(player, seta);
		rig.tick();
		const state::Conversation* talk = rig.sim->conversation(seta);
		out.greeting = talk != nullptr ? talk->line : std::string();
		// The client answers "Take them, brother...", then "What about payment?", then the "[...]" continue entries that open the
		// following lines. The greeting's reply differs by the body: 49649 alive, 49657 dead.
		const std::string_view first = dead ? "49657-Dialogue.mod" : "49649-Dialogue.mod";
		for (int i = 0; i < 8000 && rig.dlg().coverage().actApplied[10] == 0; ++i)
		{
			if (const auto menu = rig.sim->replyMenu(seta))
			{
				int option = optionOf(*menu, first);
				if (option < 0)
					option = optionOf(*menu, "49654-Dialogue.mod");
				if (option < 0)
					option = optionOf(*menu, std::string_view()); // "" = the "[...]" continue entry
				if (option >= 0)
					rig.sim->enqueue(Command::dialogueReply(state::Id{ player }, state::Id{ seta }, menu->menuSeq,
						static_cast<std::uint32_t>(option)));
			}
			rig.tick();
		}
		out.applied = rig.dlg().coverage().actApplied[10];
		out.money = rig.sim->playerSquads().money;
		out.paidTwice = rig.sim->claimBounty(heretic, w.holy, dead) == 0; // the claim flag: nothing left to pay
		const std::uint64_t before = rig.sim->stateHash();
		std::stringstream bytes;
		rig.sim->save(bytes);
		rig.sim->reload(bytes);
		out.saved = rig.sim->stateHash() == before;
		return out;
	};
	const HandIn alive = handIn(false);
	check(alive.greeting == "49648-Dialogue.mod", "a player carrying a wanted body draws the DC_CARRYING_BOUNTY_ALIVE greeting 49648 (\"" + alive.greeting + "\")");
	check(alive.applied == 1 && alive.money == 2500, "DA_PAY_BOUNTY credits the player's faction the 2500-cat bounty (" + str(static_cast<std::size_t>(alive.money)) + ")");
	check(alive.paidTwice, "a second hand-in of the same body pays nothing (the claim flag)");
	check(alive.saved, "save / load keeps the claimed bounty (the hash)");
	const HandIn dead = handIn(true);
	check(dead.greeting == "49656-Dialogue.mod", "a dead body draws DC_CARRYING_BOUNTY_DEAD's greeting 49656 (\"" + dead.greeting + "\")");
	check(dead.applied == 1 && dead.money == 1250, "a dead prisoner's bounty pays half (1250, " + str(static_cast<std::size_t>(dead.money)) + ")");

	// DA_ASSIGN_BOUNTY: the police line commits the pending theft as a bounty for the police faction, and the same table pays and clears it.
	// The criminal stands out of sight (the witness step would commit the bounty itself), and the root is forced: the police package
	// offers two roots under EV 8 and the other one scores higher.
	{
		Rig rig(w, 7);
		rig.playerFaction();
		const std::uint64_t police = rig.add(w.police, w.holy, cr::kClassLaw, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 600.0, 100.0, -1.5707963, true);
		rig.sim->dialogue()->force(w.police, w.book->dialogueIndex("12572-gamedata.base"));
		rig.sim->reportCrime(player, cr::kTheft, w.holy, 0, cr::kTimerTheftReport);
		rig.push(ev::ThiefCaughtStealingFromMe, police, player);
		rig.run(200, [&] { return rig.dlg().coverage().actApplied[32] > 0; });
		check(rig.dlg().coverage().actApplied[32] == 1, "the police line assigns the pending theft");
		check(rig.sim->bountyOn(player, w.holy) == cr::bountyAmount(cr::kTheft), "the committed bounty is the theft's table amount");
		check(rig.sim->claimBounty(player, w.holy, false) == cr::bountyAmount(cr::kTheft), "the assigned bounty is claimable (DA_PAY_BOUNTY)");
		check(rig.sim->claimBounty(player, w.holy, false) == 0, "and only once");
		check(rig.sim->forgiveCrime(player, w.holy) && rig.sim->bountyOn(player, w.holy) == 0,
			"DA_CLEAR_BOUNTY erases it");
		check(rig.sim->crimeRecord(player) == nullptr, "and clears the pending crime with it");
	}
}

// ---- 3e. cages, the memory of who was met, towns and walls (TASK BB: DIALOGUE.md slice 2) ----

// DC_IS_IMPRISONED (14), DC_IMPRISONED_BY_TARGET (7) and _BY_OTHER (8) on the sim's cage state (Fighter::cageBuilding, the captor
// faction): real EV 1 roots. The Dust Bandit's "Imprisoned thug" (13575) asks that it is caged (its line 13582 wants PT_SMART, an
// input); the Hungry bandit's "captured - meets stranger" (3310) and "- meets imprisoner" (3305) are the by-other / by-target pair,
// and the holder decides which of the two fires.
void cages(const Data& w)
{
	const auto talk = [&](const std::string& character, const std::string& faction, const char* root, bool caged, const std::string& captor)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(character, w.book->dialogueIndex(root));
		rig.playerFaction();
		const std::uint64_t npc = rig.add(character, faction, cr::kClassBandit, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		if (caged)
		{
			// A caged slave: the thug's lines are the ones a slave says (its other lines want a Cannibal town or a bounty).
			rig.f(npc).slaveState = 1;
			rig.f(npc).cageBuilding = 11; // a cage site key of the rig's own (the captor is explicit, no BuildingSite needed)
			rig.f(npc).captorSid = captor;
		}
		rig.sim->talkTo(player, npc);
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(npc);
		return said != nullptr ? said->line : std::string();
	};
	check(talk(w.dust, w.bandits, "13575-Escapes.mod", true, w.holy) == "13577-Escapes.mod",
		"a caged Dust Bandit draws the IS_IMPRISONED line 13577");
	check(talk(w.dust, w.bandits, "13575-Escapes.mod", false, "").empty(), "a free one draws none of it");
	{
		// Two of the root's lines pass for a Greenlander (3312 and 3313): the pool picks one by hash.
		const std::string line = talk(w.hungry, w.starving, "3310-dialogue-01.mod", true, w.holy);
		check(line == "3312-dialogue-01.mod" || line == "3313-dialogue-01.mod",
			"a prisoner of another faction draws IMPRISONED_BY_OTHER (3310 -> 3312 / 3313, got \"" + line + "\")");
	}
	check(talk(w.hungry, w.starving, "3310-dialogue-01.mod", true, w.nameless).empty(),
		"one held by the target's own faction does not");
	check(talk(w.hungry, w.starving, "3305-dialogue-01.mod", true, w.nameless) == "3307-dialogue-01.mod",
		"and draws IMPRISONED_BY_TARGET (3305 -> 3307)");
	check(talk(w.hungry, w.starving, "3305-dialogue-01.mod", true, w.holy).empty(), "while another faction's prisoner does not");

	// The stamp survives a save: the memory is a Fighter field (save v75).
	Rig rig(w, 7);
	rig.playerFaction();
	const std::uint64_t npc = rig.add(w.dust, w.bandits, cr::kClassBandit, 100.0, 100.0, 0.0, false);
	const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, 0.0, true);
	rig.f(npc).rememberMet(player, rig.sim->clockTick());
	rig.tick(); // the tick's mirror puts the arena's memory into the saved component
	const std::uint64_t before = rig.sim->stateHash();
	std::stringstream bytes;
	rig.sim->save(bytes);
	rig.sim->reload(bytes);
	std::uint64_t stamp = 0;
	check(rig.sim->stateHash() == before && rig.f(npc).metBefore(player, stamp), "save / load keeps the dialogue memory and the hash");
}

// DC_TARGET_LAST_SEEN_X_HOURS_AGO (18) and DC_MET_TARGET_BEFORE (20) on the sim's dialogue memory (Fighter::met): the Dust Bandit's
// "Thug sees player after defeat" (66253) under EV 3 wants the target last seen 2+ game hours ago (and the exe's no-entry 9999.99
// hours passes it); its "Healed by ally" (9310) under EV_BEING_HEALED_FINISHED wants a memory entry at all. The sensor writes the
// entries too (perception::scan).
void memory(const Data& w)
{
	const std::uint64_t hour = kTicksPerGameHour;
	const auto seen = [&](long long hoursAgo)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.dust, w.book->dialogueIndex("66253-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t dust = rig.add(w.dust, w.bandits, cr::kClassBandit, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		rig.sim->dialogue()->setMemory(dust, player, 1u << 7); // LT_DEFEATED_MY_SQUAD_ONCE: the root's lines want it
		if (hoursAgo >= 0)
			rig.f(dust).rememberMet(player, rig.sim->clockTick() - static_cast<std::uint64_t>(hoursAgo) * hour);
		rig.push(ev::ISeeNeutralSquad, dust, player);
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(dust);
		return said != nullptr ? said->line : std::string();
	};
	const std::string longAgo = seen(3);
	check(longAgo == "66254-Dialogue.mod" || longAgo == "66257-Dialogue.mod",
		"a target last seen 3 hours ago draws DC_TARGET_LAST_SEEN (66253 -> 66254 / 66257, got \"" + longAgo + "\")");
	check(seen(1).empty(), "one seen an hour ago does not (the root asks for 2+)");
	const std::string never = seen(-1);
	check(never == "66254-Dialogue.mod" || never == "66257-Dialogue.mod",
		"and a target never seen does (the exe's 9999.99 hours pass 2+)");

	const auto healed = [&](bool met)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.dust, w.book->dialogueIndex("9310-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t dust = rig.add(w.dust, w.bandits, cr::kClassBandit, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		if (met)
			rig.f(dust).rememberMet(player, rig.sim->clockTick());
		rig.push(21, dust, player); // EV_BEING_HEALED_FINISHED
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(dust);
		return said != nullptr ? said->line : std::string();
	};
	check(healed(true) == "9314-Dialogue.mod", "a met target draws MET_TARGET_BEFORE (9310 -> 9314)");
	check(healed(false).empty(), "a stranger does not (no memory entry)");

	// The sensor stamps it: two characters in sight, three seconds of scanning, an entry within the last three seconds.
	Rig rig(w, 7);
	const std::uint64_t observer = rig.add("", w.holy, 4, 100.0, 100.0, 0.0, false);
	const std::uint64_t other = rig.add("", w.holy, 4, 100.0, 130.0, 3.14159265, false);
	rig.tick(60);
	std::uint64_t stamp = 0;
	check(rig.f(observer).metBefore(other, stamp) && rig.sim->clockTick() - stamp <= 60,
		"the perception scan stamps the dialogue memory (DC 18/20 read it)");
}

// DC_WITHIN_TOWN_WALLS (57) and DC_IN_A_PLAYER_TOWN (75): the Samurai Gate Guard's "Gate Guard Partner In Crime" (9521) under EV 3
// asks that the target is OUTSIDE the walls and last seen 10+ game hours ago; the Paladin's "Inquisitor searching player base" (96051)
// asks that the speaker stands in a player's town — a state::PlayerTown of the player faction, the player's own outpost.
void townsAndWalls(const Data& w)
{
	const auto partner = [&](bool walls)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.gateGuard, w.book->dialogueIndex("9521-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t guard = rig.add(w.gateGuard, w.holy, cr::kClassMilitary, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 104.0, 100.0, -1.5707963, true);
		// The root is a `locked` record (a bribe unlocks it in play): the guard's own entry says it is open.
		rig.setLock(guard, "9521-Dialogue.mod", false);
		rig.f(guard).rememberMet(player, rig.sim->clockTick() - 12 * kTicksPerGameHour);
		rig.tick();
		if (walls)
			rig.placeWalls(104.0, 100.0, 400.0);
		rig.push(ev::ISeeNeutralSquad, guard, player);
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(guard);
		return said != nullptr ? said->line : std::string();
	};
	check(partner(false) == "9522-Dialogue.mod", "a gate guard draws WITHIN_TOWN_WALLS' line 9522 for a target outside the walls");
	check(partner(true).empty(), "and none for a target inside them");

	const auto searching = [&](bool playerTown)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.paladin, w.book->dialogueIndex("96051-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t paladin = rig.add(w.paladin, w.holy, cr::kClassMilitary, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 104.0, 100.0, -1.5707963, true);
		// The line that passes without the unimplemented DC_MIXED_GENDER_GROUP: the skeleton one (robot limbs, an input).
		rig.sim->dialogue()->setInputs(player, DialogueInputs{ 0, false, true });
		if (playerTown)
			rig.placePlayerTown(100.0, 100.0, 400.0);
		rig.push(ev::ISeeNeutralSquad, paladin, player);
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(paladin);
		return said != nullptr ? said->line : std::string();
	};
	check(searching(true) == "96057-Dialogue.mod", "a Paladin in a player's town draws IN_A_PLAYER_TOWN's line 96057");
	check(searching(false).empty(), "and none outside one");
}

// ---- 3f. the slice-2 conditions and lists (TASK BS): DIALOGUE_CONDITIONS_STATIC 30, 37, 42, 46, 63, 64, 69 and 1.3 steps 7-8 ----
//
// DC_WEARING_LOCKED_SHACKLES (64) and DC_IS_IN_LOCKED_CAGE (63) on the sim's captivity state: the Hungry bandit's "unlocked - still
// have shackles" (5405) and "- still has cage" (5406) under EV 13, held by the starving bandits' package (3781). The item is the
// data's Prisoner Shackles (ARMOUR slot 9, `is locked`), the cage is the warden squad's cage lock (state::SquadRoutine::cagesLocked,
// task AE; OURS: one flag per squad, not a per-cage lock).
void captivity(const Data& w)
{
	const auto line = [&](const char* root, bool shackles, bool cages, bool locked)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.hungry, w.book->dialogueIndex(root));
		rig.playerFaction();
		const std::uint64_t npc = rig.add(w.hungry, w.bandits, cr::kClassBandit, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 2000.0, 100.0, -1.5707963, true);
		if (shackles)
			rig.giveItem(npc, "SHACKLES", 1, state::Inventory::kArmourBase + 9);
		if (cages)
		{
			rig.f(npc).cageBuilding = 11; // a cage site key of the rig's own
			rig.worldSquad(11, locked);
		}
		rig.settle();
		rig.push(13, npc, player); // EV_UNLOCK_MY_CAGE_OR_SHACKLES
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(npc);
		return said != nullptr ? said->line : std::string();
	};
	check(line("5405-gamedata.base", true, false, false) == "5412-gamedata.base",
		"wearing locked shackles draws WEARING_LOCKED_SHACKLES' line 5412");
	check(line("5405-gamedata.base", false, false, false).empty(), "and a free speaker draws nothing (no recursion)");
	check(line("5406-gamedata.base", false, true, true) == "5415-gamedata.base",
		"a caged speaker under locked cages draws IS_IN_LOCKED_CAGE's line 5415");
	check(line("5406-gamedata.base", false, true, false).empty(), "and one whose wardens keep them open draws nothing");
}

// DC_NEARLY_KO (30) and DC_IS_NEARLY_KO (42) on the medical parts. NEARLY_KO: the Sword for hire's "bodyguard finished job" (5095)
// under EV_CONTRACT_JOB_ENDED, line 5101 (the speaker). IS_NEARLY_KO: the Slaver's "shout at slave worker" (5263) under
// EV_SHOUT_AT_SLAVE_WORKER, line 5267 (the target). A part at 5% of its max is under the exe's 0.1.
void wounds(const Data& w)
{
	const auto speech = [&](const std::string& character, const char* root, int event, bool member, bool hurts, bool hurtTarget)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(character, w.book->dialogueIndex(root));
		rig.playerFaction();
		const std::uint64_t npc = rig.add(character, w.bandits, cr::kClassAdventurer, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 2000.0, 100.0, -1.5707963, true);
		if (member)
		{
			// A leader's package list is its squad template's `dialog leader`: "Drifter solo" carries "bodyguard for hire" (5069).
			rig.join(npc, rig.worldSquad(0, false, "2206-gamedata.base"));
		}
		if (hurts)
			rig.wound(npc, 2, 5.0); // an arm at 5 of 100
		if (hurtTarget)
			rig.wound(player, 2, 5.0);
		rig.settle();
		rig.push(event, npc, player);
		rig.tick();
		const DialoguePick* pick = rig.picked(event, npc);
		return std::make_pair(pick != nullptr ? std::string(w.book->sidOf(w.book->line(pick->line).sid)) : std::string(),
			pick != nullptr ? pick->passed : 0u);
	};
	const auto hurtTarget = speech(w.slaver, "5263-gamedata.base", 38, false, false, true); // EV_SHOUT_AT_SLAVE_WORKER
	check(hurtTarget.first == "5267-gamedata.base" && hurtTarget.second == 1,
		"a nearly KO target draws IS_NEARLY_KO's line 5267 (got \"" + hurtTarget.first + "\")");
	const auto healthyTarget = speech(w.slaver, "5263-gamedata.base", 38, false, false, false);
	check(healthyTarget.first != "5267-gamedata.base" && healthyTarget.second == 2,
		"a healthy target draws the plain shout (5264 / 5265, got \"" + healthyTarget.first + "\")");
	const auto hurtSpeaker = speech(w.swordForHire, "5095-gamedata.base", 30, true, true, false);
	check(hurtSpeaker.first == "5101-gamedata.base", "a nearly KO speaker draws NEARLY_KO's line 5101 (got \"" + hurtSpeaker.first + "\")");
	const auto healthy = speech(w.swordForHire, "5095-gamedata.base", 30, true, false, false);
	check(healthy.first == "5096-gamedata.base", "a healthy pair draws the plain line 5096 (got \"" + healthy.first + "\")");
}

// DC_NUM_BACKPACKS (37) and the `target has item type` list (1.3 step 8) on the inventories: the Dust King's "Looting" (7151) under
// EV_LOOTING_EVERYTHING, whose line 7154 wants two squad backpacks and 7158 an item type (Hashish; UNSURE: the reading compares
// record types, and the data's list entries are ITEM records).
void looting(const Data& w)
{
	const auto line = [&](int members, bool hashish)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.dustKing, w.book->dialogueIndex("7151-Dialogue.mod"));
		rig.playerFaction();
		rig.sim->adjustPlayerMoney(1000); // DC_PLAYERMONEY < 500 for line 7152 must fail
		const std::uint64_t king = rig.add(w.dustKing, w.bandits, cr::kClassBandit, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 2000.0, 100.0, -1.5707963, true);
		std::vector<std::uint64_t> squad{ player };
		if (members > 1)
		{
			squad.push_back(rig.add("", w.nameless, 9, 2002.0, 100.0, -1.5707963, true));
			const flecs::entity group = rig.worldSquad(0, false);
			for (const std::uint64_t member : squad)
				rig.join(member, group);
		}
		for (const std::uint64_t member : squad)
			rig.giveItem(member, "576-gamedata.base", 1, state::Inventory::kBackpackSlot); // Small Backpack (CONTAINER, slot 12)
		if (hashish)
			rig.giveItem(player, "1230-gamedata.base", 1); // Hashish (ITEM)
		rig.settle();
		rig.push(33, king, player); // EV_LOOTING_EVERYTHING
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(king);
		return said != nullptr ? said->line : std::string();
	};
	const std::string twoBags = line(2, false);
	check(twoBags == "7154-Dialogue.mod", "two squad backpacks draw NUM_BACKPACKS' line 7154 (got \"" + twoBags + "\")");
	const std::string oneBag = line(1, false);
	check(oneBag.empty(), "one does not (the item-type lines fail, the money line is gated out; got \"" + oneBag + "\")");
	// UNSURE (DIALOGUE_CONDITIONS_STATIC 1.3 step 8): the exe's item-type reading is not decoded, so ours is the record type; every
	// data list entry is an ITEM record, and Hashish passes all four lists (their four lines tie and the hash picks one).
	const std::string hashish = line(1, true);
	check((hashish == "7156-Dialogue.mod" || hashish == "7157-Dialogue.mod" || hashish == "7158-Dialogue.mod"
			|| hashish == "7159-Dialogue.mod"),
		"carrying Hashish passes the item-type lists (7156-7159, got \"" + hashish + "\")");
}

// The `target carrying character` list (1.3 step 7) and `target has item` (step 8): the Samurai Gate Guard's "Guard sees fish head"
// (1534095) under EV_I_SEE_NEUTRAL_SQUAD. Line 1534096 wants the target carrying King Gurgler, 1534099 the Gurgler Head item, 1534097
// neither (its 50% chance passes on a forced root). The carrying case pools 1534096 and 1534097; the hit must raise the pool size.
void carrying(const Data& w)
{
	const auto probe = [&](bool body, bool head)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.gateGuard, w.book->dialogueIndex("1534095-__Fixes.mod"));
		rig.playerFaction();
		const std::uint64_t guard = rig.add(w.gateGuard, w.holy, cr::kClassMilitary, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 104.0, 100.0, -1.5707963, true);
		if (body)
		{
			const std::uint64_t gurgler = rig.add(w.gurgler, w.bandits, cr::kClassBandit, 600.0, 100.0, 0.0, false);
			rig.f(player).carrying = gurgler;
			rig.f(gurgler).carriedBy = player;
		}
		if (head)
			rig.giveItem(player, "96409-Dialogue.mod", 1); // Gurgler Head (ITEM)
		rig.settle();
		rig.push(ev::ISeeNeutralSquad, guard, player);
		rig.tick();
		const DialoguePick* pick = rig.picked(ev::ISeeNeutralSquad, guard);
		return std::make_pair(pick != nullptr ? std::string(w.book->sidOf(w.book->line(pick->line).sid)) : std::string(),
			pick != nullptr ? pick->passed : 0u);
	};
	const auto body = probe(true, false);
	check(body.first == "1534096-__Fixes.mod", "the target carrying King Gurgler passes the carrying list (line 1534096, got \""
			+ body.first + "\")");
	const auto none = probe(false, false);
	check(none.first.empty() && none.second == 0, "without a carried body or the item no line passes");
	const auto head = probe(false, true);
	check(head.first == "1534099-__Fixes.mod", "a Gurgler Head in the inventory draws the item list's line 1534099 (got \""
			+ head.first + "\")");
}

// DC_HAS_ILLEGAL_ITEM (69) on the law faction's list: the Gate Guard's "Gate Guard Partner In Crime" (9521) under EV 3, its line
// 9527 (last seen 400+ hours ago AND the illegal item). Hashish is on the Holy Nation's trade culture's `illegal goods`; a clean
// target draws the plain 9522 instead.
void smuggling(const Data& w)
{
	const auto probe = [&](bool hashish)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.gateGuard, w.book->dialogueIndex("9521-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t guard = rig.add(w.gateGuard, w.holy, cr::kClassMilitary, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 104.0, 100.0, -1.5707963, true);
		rig.setLock(guard, "9521-Dialogue.mod", false); // the root's own `locked` flag: a bribe opens it in play
		// Line 9527 wants the target last seen 400+ hours ago: move the clock into the future first (the new-game tick is ~25 h).
		rig.sim->setClockTick(rig.sim->clockTick() + 500 * kTicksPerGameHour + 60);
		rig.f(guard).rememberMet(player, rig.sim->clockTick() - 500 * kTicksPerGameHour);
		if (hashish)
			rig.giveItem(player, "1230-gamedata.base", 1);
		rig.settle();
		rig.push(ev::ISeeNeutralSquad, guard, player);
		rig.tick();
		const DialoguePick* pick = rig.picked(ev::ISeeNeutralSquad, guard);
		return pick != nullptr ? std::string(w.book->sidOf(w.book->line(pick->line).sid)) : std::string();
	};
	const std::string clean = probe(false);
	check(clean == "9522-Dialogue.mod", "a clean target draws the plain line 9522 (got \"" + clean + "\")");
	const std::string smuggler = probe(true);
	check(smuggler == "9527-Dialogue.mod", "Hashish in the inventory draws HAS_ILLEGAL_ITEM's line 9527 (got \"" + smuggler + "\")");
}

// DC_CARRYING_SOMEONE_TO_ENSLAVE (46) on the carried person's class: the Rebirth slave trader (49695) under EV_PLAYER_TALK_TO_ME,
// line 49715 for a player carrying a bandit-class body, 49696 otherwise.
void enslaving(const Data& w)
{
	const auto line = [&](bool body)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.captain, w.book->dialogueIndex("49695-Dialogue.mod"));
		rig.playerFaction();
		const std::uint64_t trader = rig.add(w.captain, w.holy, cr::kClassTrader, 100.0, 100.0, 3.14159265, false);
		const std::uint64_t player = rig.add("", w.nameless, 9, 102.0, 100.0, -1.5707963, true);
		// The leader of "Holy Slave Processing" gets the "Rebirth shopkeep" package (65029) through `dialog leader`.
		rig.join(trader, rig.worldSquad(0, false, "49390-rebirth.mod"));
		if (body)
		{
			const std::uint64_t victim = rig.add("", w.bandits, cr::kClassBandit, 600.0, 100.0, 0.0, false);
			rig.f(player).carrying = victim;
			rig.f(victim).carriedBy = player;
		}
		rig.settle();
		rig.sim->talkTo(player, trader);
		rig.tick();
		const state::Conversation* said = rig.sim->conversation(trader);
		return said != nullptr ? said->line : std::string();
	};
	check(line(true) == "49715-Dialogue.mod", "the player carrying a bandit draws CARRYING_SOMEONE_TO_ENSLAVE's line 49715");
	check(line(false) == "49696-Dialogue.mod", "empty-handed draws 49696");
}

// DA_TAG_ESCAPED_SLAVE (25) on the existing slave chain: the slaver's "slave escape" (5251) under EV_ESCAPING_SLAVE_SPOTTED; its
// line 5252 tags the spotted slave ESCAPING (state 2). The control: a speaker inside a player town fails the root (IN_A_PLAYER_TOWN).
void escaping(const Data& w)
{
	const auto probe = [&](bool playerTown)
	{
		Rig rig(w, 7);
		rig.sim->dialogue()->force(w.slaver, w.book->dialogueIndex("5251-gamedata.base"));
		rig.playerFaction();
		const std::uint64_t slaver = rig.add(w.slaver, w.bandits, 7, 100.0, 100.0, 3.14159265, false); // OT_SLAVER
		const std::uint64_t player = rig.add("", w.nameless, 9, 2000.0, 100.0, -1.5707963, true);
		if (playerTown)
			rig.placePlayerTown(100.0, 100.0, 400.0);
		rig.settle();
		rig.push(36, slaver, player); // EV_ESCAPING_SLAVE_SPOTTED
		rig.tick();
		return std::make_pair(rig.dlg().coverage().actApplied[25], rig.f(player).slaveState);
	};
	const auto tagged = probe(false);
	check(tagged.first == 1 && tagged.second == 2, "DA_TAG_ESCAPED_SLAVE makes the spotted slave ESCAPING (state 2)");
	const auto gated = probe(true);
	check(gated.first == 0 && gated.second == 0, "and a root that fails its gate tags nobody");
}

void sliceConditions(const Data& w)
{
	captivity(w);
	wounds(w);
	looting(w);
	carrying(w);
	smuggling(w);
	enslaving(w);
	escaping(w);
	// `my subrace` stays failing closed: the RACE data has no subrace field (DIALOGUE_CONDITIONS_STATIC 1.3 step 9).
	check(!dialogueListImplemented(GlMySubrace), "the my-subrace list remains unimplemented (fails closed)");
	check(dialogueListImplemented(GlTargetCarrying) && dialogueListImplemented(GlTargetHasItem)
			&& dialogueListImplemented(GlTargetHasItemType),
		"the carrying and item lists are implemented");
}

// ---- 3c2. speech pacing (TASK N) ----

// The exe sets a line's play time at (2 + 0.12 x characters) hundredths of a GAME HOUR (14067fd80; kinspect findings/dialogue_pacing read
// the constants live and measured the closing chain). The line therefore holds (2 + 0.12 x chars) / 100 game hours, converted at
// kTicksPerGameHour, not at the real-time tick rate. The live chain "shopkeep holy shoo" (43003) is replayed against the measurement:
// nine lines, starts .008 .062 .086 .130 .193 .257 .345 .407 .474 game h, holds .058 .023 .045 .065 .069 .094 .066 .073 .083 h.
void pacing(const Data& w)
{
	Rig rig(w, 7);
	const DialogueSystem& dialogue = *rig.sim->dialogue();
	const std::uint64_t voice = rig.add(w.citizen, w.holy, 4, 0.0, 0.0, 0.0, false);
	// sid, the characters the live text had (word tokens as displayed), and the live hold in hundredths of a game hour.
	const struct ChainLine { const char* sid; std::size_t chars; double liveHundredths; } chain[] = {
		{ "43015-Dialogue.mod", 45, 5.784 },
		{ "46944-Dialogue.mod", 3, 2.328 },
		{ "43016-Dialogue.mod", 23, 4.472 },
		{ "46922-Dialogue.mod", 25, 6.469 },
		{ "43017-Dialogue.mod", 33, 6.913 },
		{ "46920-Dialogue.mod", 63, 9.446 },
		{ "46921-Dialogue.mod", 39, 6.609 },
		{ "43022-Dialogue.mod", 68, 7.268 },
		{ "43021-Dialogue.mod", 70, 8.283 },
	};
	bool formula = true;
	double ourStart = 0.0;
	double liveStart = 0.0;
	for (const ChainLine& entry : chain)
	{
		const std::uint32_t line = w.book->lineIndex(entry.sid);
		const DialogueSystem::Speech said = dialogue.speech(line, voice, 1000);
		formula = formula && std::fabs(said.hours - (2.0 + 0.12 * static_cast<double>(said.text.size())) / 100.0) < 1e-12;
		const double model = (2.0 + 0.12 * static_cast<double>(entry.chars)) / 100.0; // the model on the live text length
		std::printf("    %s: %zu chars, model %.4f h, live %.4f h, start ours %.3f live %.3f\n", entry.sid, entry.chars, model,
			entry.liveHundredths / 100.0, ourStart, liveStart);
		ourStart += model;
		liveStart += entry.liveHundredths / 100.0;
	}
	check(formula, "a line's timer is (2 + 0.12 x characters) hundredths of a game hour");
	std::printf("    9 lines: ours %.4f game h, live %.4f game h, %.1f %% long\n", ourStart, liveStart,
		100.0 * (ourStart - liveStart) / liveStart);
	check(std::fabs(ourStart - liveStart) <= 0.08, "the replayed closing chain runs within 0.08 game h of the live one");
}

// ---- 3c. individual goals (step 6b) ----

// The goals DA_RUN_AWAY, DA_GO_HOME and DA_SUMMON_MY_SQUAD give (Simulation::giveIndividualGoal), on the Hub navmesh. Each ends by itself; a
// fleeing character takes no fight. The caps are ours (individual_goals.cpp).
void individualGoals(const Data& w, const std::shared_ptr<MeshNavigator>& nav)
{
	using state::IndividualKind;
	constexpr double kHubX = -50978.8;
	constexpr double kHubZ = 2932.48;
	const auto place = [&](Rig& rig, std::uint64_t id, double x, double z)
	{
		const auto spot = nav->walker().snap({ x, 1535.0, z }, 300.0);
		test::require(spot.has_value(), "no mesh near " + str(static_cast<std::size_t>(std::fabs(x))) + "," + str(static_cast<std::size_t>(std::fabs(z))));
		rig.f(id).x = spot->x;
		rig.f(id).y = spot->y;
		rig.f(id).z = spot->z;
	};
	// Flee: from an enemy player standing beside it (an NPC would take it as a target), the runner takes no target and runs off until it
	// reaches its point 2000..2400 past the player, or its walk ends with no way on.
	{
		Rig rig(w, 7);
		rig.sim->enableNavigation(nav);
		rig.fights = true;
		const std::uint64_t runner = rig.add("", w.holy, 4, 0.0, 0.0, 0.0, false, 0);
		const std::uint64_t player = rig.add("", w.nameless, 9, 0.0, 0.0, 0.0, true, 1);
		place(rig, player, kHubX, kHubZ);
		place(rig, runner, kHubX + 60.0, kHubZ);
		check(!rig.sim->giveIndividualGoal(player, IndividualKind::Flee, runner), "a player character is given no individual goal");
		check(rig.sim->giveIndividualGoal(runner, IndividualKind::Flee, player), "flee: given");
		bool fought = false;
		double furthest = 0.0;
		int ticks = 0;
		while (rig.f(runner).goal.kind == IndividualKind::Flee && ticks++ < 20 * 130)
		{
			rig.tick();
			fought = fought || rig.f(runner).target != 0;
			furthest = std::max(furthest, std::hypot(rig.f(runner).x - kHubX, rig.f(runner).z - kHubZ));
		}
		check(!fought && furthest > 1000.0 && rig.f(runner).goal.kind == IndividualKind::None,
			"flee: the runner takes no target, gets " + str(static_cast<std::size_t>(furthest)) + " away and the goal ends (" + str(static_cast<std::size_t>(ticks / 20)) + " s)");
	}
	// Summon and go home: a squad of three whose home is a town (no home building).
	{
		Rig rig(w, 7);
		rig.sim->enableNavigation(nav);
		flecs::world& ecs = const_cast<flecs::world&>(rig.sim->world().ecs());
		const auto homeSpot = nav->walker().snap({ kHubX - 300.0, 1535.0, kHubZ }, 300.0);
		test::require(homeSpot.has_value(), "no mesh at the home point");
		flecs::entity town = state::createInstance(ecs);
		town.set<state::TownState>(state::TownState{ w.townSid, w.townSid, state::EntityRef{}, homeSpot->x, homeSpot->z });
		flecs::entity squad = state::createInstance(ecs);
		state::SquadInstance instance;
		instance.homeTown = state::EntityRef{ town.id() };
		// Dismissed: not a world squad, which the resident pass would take off the arena with no player near (stepResidents).
		instance.source = state::SpawnSource::Dismissed;
		squad.set<state::SquadInstance>(instance);
		const std::uint64_t boss = rig.add("", w.holy, 4, 0.0, 0.0, 0.0, false);
		const std::uint64_t a = rig.add("", w.holy, 4, 0.0, 0.0, 0.0, false);
		const std::uint64_t b = rig.add("", w.holy, 4, 0.0, 0.0, 0.0, false);
		place(rig, boss, kHubX, kHubZ);
		place(rig, a, kHubX + 300.0, kHubZ);
		place(rig, b, kHubX, kHubZ + 300.0);
		std::vector<flecs::entity> members;
		ecs.each([&](flecs::entity e, const state::Id& id, const state::Fighter&)
		{
			if (id.value == boss || id.value == a || id.value == b)
				members.push_back(e);
		});
		for (flecs::entity e : members)
			e.add<state::MemberOf>(squad);
		check(rig.sim->giveIndividualGoal(a, IndividualKind::Summoned, boss) && rig.sim->giveIndividualGoal(b, IndividualKind::Summoned, boss)
			&& !rig.sim->giveIndividualGoal(boss, IndividualKind::Summoned, boss), "summon: given to the others, not to the summoner");
		check(rig.sim->giveIndividualGoal(boss, IndividualKind::GoHome, a), "go home: given (the squad has a home town)");
		rig.tick(20 * 2);
		// Save / load mid-goal keeps the goals and the hash.
		std::stringstream bytes;
		rig.sim->save(bytes);
		const std::uint64_t before = rig.sim->stateHash();
		rig.sim->reload(bytes);
		check(rig.sim->stateHash() == before && rig.f(a).goal.kind == IndividualKind::Summoned, "save / load mid-goal keeps the goals and the hash");
		int ticks = 0;
		while ((rig.f(a).goal.kind != IndividualKind::None || rig.f(b).goal.kind != IndividualKind::None || rig.f(boss).goal.kind != IndividualKind::None)
			&& ticks++ < 20 * 120)
			rig.tick();
		const auto near = [&](std::uint64_t id, double reach) { return std::hypot(rig.f(id).x - rig.f(boss).x, rig.f(id).z - rig.f(boss).z) <= reach; };
		check(ticks < 20 * 120 && near(a, 200.0) && near(b, 200.0) && std::hypot(rig.f(boss).x - homeSpot->x, rig.f(boss).z - homeSpot->z) <= 100.0,
			"summon and go home: the mates reach the summoner, it reaches home, and every goal ends (" + str(static_cast<std::size_t>(ticks / 20)) + " s)");
	}
}

// ---- 4. determinism ----

std::string signature(const DialogueBook& book, const std::vector<DialoguePick>& picks)
{
	std::string out;
	for (const DialoguePick& p : picks)
		out += str(p.event.tick) + ":" + str(p.event.event) + ":" + rootName(book, p) + ":" + str(p.line) + ":" + str(p.passed) + ";";
	return out;
}

void determinism(const Data& w)
{
	const DialogueBook& book = *w.book;
	const ShooResult a = shoo(w, w.citizen, w.holy, 7, kNormal);
	const ShooResult b = shoo(w, w.citizen, w.holy, 7, kNormal);
	check(!a.picks.empty() && signature(book, a.picks) == signature(book, b.picks), "same seed and inputs: identical picks");
	// The pick also depends only on (seed, speaker, tick, event): evaluating the same event twice gives the same answer.
	FakeWorld world;
	DialogueActor speaker;
	speaker.exists = true;
	speaker.id = 5;
	speaker.character = w.citizen;
	speaker.faction = w.holy;
	speaker.leader = true;
	speaker.building = speaker.homeBuilding = 9;
	DialogueActor target;
	target.exists = true;
	target.id = 6;
	target.playerFaction = true;
	target.faction = w.nameless;
	target.building = 9;
	target.z = 50.0;
	world.actors[5] = speaker;
	world.actors[6] = target;
	DialogueSystem system(w.book, 3);
	DialogueEvent e;
	e.event = ev::ShooFromMyBuilding;
	e.speaker = 5;
	e.target = 6;
	std::set<std::uint32_t> lines;
	for (std::uint64_t tick = 0; tick < 200; ++tick)
	{
		e.tick = tick;
		const DialoguePick first = system.evaluate(world, e);
		const DialoguePick second = system.evaluate(world, e);
		test::require(first.result == second.result && first.line == second.line && first.dialogue == second.dialogue,
			"tick " + str(tick) + ": the same event evaluates the same twice");
		if (first.line != kNoIndex)
			lines.insert(first.line);
	}
	check(true, "200 ticks: the same event evaluates the same twice (" + str(lines.size()) + " distinct first lines)");
	// The queue is consumed sorted: pushing in the opposite order gives the same log.
	DialogueSystem forward(w.book, 3), backward(w.book, 3);
	std::vector<DialogueEvent> events;
	for (std::uint64_t tick = 0; tick < 6; ++tick)
	{
		e.tick = tick;
		events.push_back(e);
	}
	for (std::size_t i = 0; i < events.size(); ++i)
	{
		forward.push(events[i]);
		backward.push(events[events.size() - 1 - i]);
	}
	forward.step(world, 6);
	backward.step(world, 6);
	std::vector<DialoguePick> f(forward.log().begin(), forward.log().end()), g(backward.log().begin(), backward.log().end());
	check(f.size() == 6 && signature(book, f) == signature(book, g), "the queue is consumed in (tick, speaker, event) order whatever the push order");
	// Aged events are dropped, never answered; the budget keeps the rest queued.
	DialogueSystem aging(w.book, 3);
	e.tick = 0;
	aging.push(e);
	aging.step(world, DialogueSystem::kMaxAgeTicks + 5);
	check(aging.staleDropped() == 1 && aging.log().back().result == DialoguePick::Result::Stale, "an event older than 2 s is dropped as stale");
	DialogueSystem busy(w.book, 3);
	for (std::size_t i = 0; i < DialogueSystem::kEvalsPerTick + 10; ++i)
	{
		e.tick = 100;
		busy.push(e);
	}
	busy.step(world, 100);
	check(busy.log().size() == DialogueSystem::kEvalsPerTick && busy.queued() == 10, "at most 64 evaluations per tick, the rest waits");
}

} // namespace

int main(int argc, char** argv)
{
	test::require(argc == 2, "usage: ksim_dialogue_events HUB.knav");
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const Data w;
	coverage(w);
	failClosed(w);
	townLevelConditions(w);
	townLevelSim(w);
	shooScenarios(w);
	shooGates(w);
	theft(w);
	assault(w);
	starving(w);
	dustBanditSight(w);
	skinBanditAlarm(w);
	conversations(w);
	pacing(w);
	effects(w);
	slaves(w);
	bounties(w);
	cages(w);
	memory(w);
	townsAndWalls(w);
	sliceConditions(w);
	individualGoals(w, MeshNavigator::load(argv[1]));
	determinism(w);
	std::printf("ksim_dialogue_events: ok\n");
	return 0;
}
