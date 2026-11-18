// The dialogue economy and membership effects (docs/DIALOGUE.md 5 "Economy / membership", D5 / decision 10; the readings of
// docs/reference/DIALOGUE_ACTIONS_STATIC.md), each pinned through a real data line where one exists:
//
//  - DA_TAKE_MONEY (8): "Skeleton for hire" 47621 / 47623. The payer's wallet is debited and the speaker's faction is credited
//    (state::FactionState::cash, save v70; D5: the exe burns the cats). The payment is refused, and nothing changes, when the wallet
//    no longer holds it while the menu waits (the exe's own check); the join in front of it still runs.
//  - give item: Big Grim's "talks to new ally" 61857 / 61860 (ten Building Materials into the target's inventory; the stack rule of
//    DIALOGUE_ACTIONS_STATIC 1.5).
//  - DA_SEPARATE_TO_MY_OWN_SQUAD (28): a Citizen's "Freed from cannibal cage" 6021 under EV 13 (every first line wants the speaker in a
//    Cannibal town; the speaker splits into a SquadInstance of its own and the old squad keeps its mates).
//  - DA_REMOVE_SLAVE_STATUS (53): Grey's 59269 under EV 1 carries it on 59278, behind the DC_IS_SLAVE line 59270 (task AY) and the
//    root's world states (task AN); the test answers "Yes. I heard you can help me..." and the slave is freed, its orders cleared. The
//    cage fields of the same effect are a direct fixture (the sim's IS_IMPRISONED is still a stub).
//  - DA_GIVE_TARGET_MY_SLAVES (24): a Reaver's 59465 under EV 39 (every slave of the speaker's faction changes captor; a carried one is
//    dropped, pinned by a direct call).
//  - DA_KNOCKOUT (58): a direct effect fixture. Its one data use (line 1533728) hangs under a biome `arrival dialog` root that the sim has
//    no producer for, so no line is reachable; the fixture calls Simulation::knockOut.
//
// The fixtures order only the player (the talk and the reply); the pushed events stand in for the sim's own producers.

#include "ksim/clock.hpp"
#include "ksim/dialogue_events.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

struct Data
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<const DialogueBook> book;
	std::string body; // the rig body every fighter is built from
	std::string nameless, guild, dust, holy, cannibals, antiSlavers, reavers; // FACTION sids
	std::string recruit, grim, citizen, grey, reaver; // CHARACTER sids
	std::string camp; // the Reaver SQUAD_TEMPLATE whose `dialog squad` roots 59465
	std::string item; // Big Grim's gift
	std::string cannibalTown; // a Cannibal TOWN record the separate fixture places

	Data()
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		data = CombatData::build(*gd, KENSHI_DATA);
		book = DialogueBook::build(gd);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "Nameless")
				nameless = r->stringID;
			if (r->name == "Dust Bandits")
				dust = r->stringID;
			if (r->name == "The Holy Nation")
				holy = r->stringID;
			if (r->name == "Anti-Slavers")
				antiSlavers = r->stringID;
			if (r->name == "Cannibals")
				cannibals = r->stringID;
			if (r->name == "Reavers")
				reavers = r->stringID;
			if (r->stringID == "1233-gamedata.base")
				guild = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant" && body.empty())
				body = r->stringID;
			if (r->name == "Skeleton recruit 2500" && recruit.empty())
				recruit = r->stringID;
			if (r->name == "Big Grim" && grim.empty())
				grim = r->stringID;
			if (r->name == "Citizen" && citizen.empty())
				citizen = r->stringID;
			if (r->name == "Grey" && grey.empty())
				grey = r->stringID;
			if (r->name == "Reaver Ironclad" && reaver.empty())
				reaver = r->stringID;
		}
		if (const kdata::Record* record = gd->find("96149-Dialogue.mod"); record != nullptr && record->name == "Reaver residents (camp)")
			camp = record->stringID;
		if (const kdata::Record* record = gd->find("580-gamedata.base"); record != nullptr && record->name == "Building Material")
			item = record->stringID;
		if (const kdata::Record* record = gd->find("1224-gamedata.base"); record != nullptr && record->name == "Cannibal Village")
			cannibalTown = record->stringID;
		check(!body.empty() && !nameless.empty() && !guild.empty() && !dust.empty() && !holy.empty() && !cannibals.empty()
				&& !antiSlavers.empty() && !reavers.empty(), "data records: the factions and the servant body");
		check(cannibals == "518-gamedata.base" && cannibalTown == "1224-gamedata.base", "the Cannibals and their village record");
		check(recruit == "47629-Dialogue.mod" && grim == "48447-Dialogue.mod" && citizen == "465-gamedata.base"
				&& grey == "56734-Dialogue.mod" && reaver == "65265-rebirth.mod" && !camp.empty() && item == "580-gamedata.base",
			"data records: the recruit, Big Grim, a Citizen, Grey, a Reaver Ironclad, the Reaver camp squad and Building Material");

		// Every effect has a real line: the root is in the speaker's package set under the event the fixture pushes.
		const auto rooted = [&](const std::string& character, const std::string& squadTemplate, const std::string& faction, int event,
								 std::string_view root)
		{
			SpeakerKey key;
			key.character = character;
			key.squadTemplate = squadTemplate;
			key.faction = faction;
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
		check(rooted(recruit, "", guild, 1, "47621-Dialogue.mod"), "the recruit roots \"Skeleton for hire\" (47621) under EV 1");
		check(rooted(grim, "", nameless, 1, "61857-Dialogue.mod"), "Big Grim (in the player faction) roots 61857 under EV 1");
		check(rooted(citizen, "", dust, 13, "6021-Dialogue.mod"), "a Citizen roots 6021 under EV 13");
		check(rooted(grey, "", antiSlavers, 1, "59269-Dialogue.mod"), "Grey roots 59269 under EV 1");
		check(rooted(reaver, camp, reavers, 39, "59465-Dialogue.mod"),
			"a Reaver Ironclad in the Reaver camp squad roots 59465 under EV 39");

		// The lines behind them: the money line, the gift, the actions.
		const auto hasAct = [&](std::string_view lineSid, int kind, int operand)
		{
			const std::uint32_t line = book->lineIndex(lineSid);
			if (line == kNoIndex)
				return false;
			for (const ActDef& act : book->effects(book->line(line).effects))
			{
				if (act.kind == kind && (operand < 0 || act.operand == operand))
					return true;
			}
			return false;
		};
		check(hasAct("47623-Dialogue.mod", 3, -1) && hasAct("47623-Dialogue.mod", 8, 6000),
			"the hire answer 47623 joins (3) and takes 6000 cats (8)");
		check(hasAct("6022-Dialogue.mod", 28, -1) && hasAct("59278-Dialogue.mod", 53, -1) && hasAct("59468-Dialogue.mod", 24, -1),
			"the separate, free and slave-delivery lines carry actions 28, 53 and 24");
		const std::uint32_t gift = book->lineIndex("61860-Dialogue.mod");
		bool giftOk = gift != kNoIndex && book->line(gift).giveItems.count == 1;
		if (giftOk)
		{
			const ItemGive& give = book->giveItems(book->line(gift).giveItems)[0];
			giftOk = book->sidOf(give.item) == item && give.count == 10;
		}
		check(giftOk, "61860 gives ten Building Materials (" + item + ", count 10)");
	}
};

// A world with one player faction (one squad, one character), the wallet at `money`, and reply menus on when the fixture answers one.
struct Rig
{
	const Data& w;
	std::unique_ptr<Simulation> sim;
	std::vector<Simulation::ReplyMenuEvent> menus;
	std::uint64_t playerSquad = 0;
	std::uint64_t player = 0;
	std::uint64_t rng = 3;

	Rig(const Data& data, std::int64_t money = 0, bool replyMenus = false) : w(data)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 7));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim->enableCombat(w.data, settings);
		sim->enableDialogue(w.book);
		sim->setReplyMenus(replyMenus);
		sim->setReplyMenuObserver([this](const Simulation::ReplyMenuEvent& e) { menus.push_back(e); });
		sim->createPlayerFaction(w.nameless);
		playerSquad = sim->addPlayerSquad();
		state::Fighter f = w.data->makeFighter(*w.gd, w.body, "", rng);
		f.playerControlled = true;
		f.team = 0;
		f.x = 100.0;
		f.z = 100.0;
		f.yaw = 0.0;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{ w.nameless });
		player = e.get<state::Id>()->value;
		sim->addPlayerMember(playerSquad, player);
		if (money != 0)
			check(sim->adjustPlayerMoney(money), "the wallet is credited " + std::to_string(money));
	}
	// A FACTION instance the dialogue economy can credit (templateSid `sid`).
	flecs::entity factionState(const std::string& sid)
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity entity = state::createInstance(ecs);
		entity.set<state::FactionState>(state::FactionState{ sid });
		return entity;
	}
	// A TownState of `townSid` (its record supplies the size radius), owned by `faction`, at (x, z): the dialogue's town lookup
	// (the `in town of` gate, GlInTownOf).
	void placeTown(flecs::entity faction, const std::string& townSid, double x, double z)
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity town = state::createInstance(ecs);
		town.set<state::TownState>(state::TownState{ townSid, townSid, state::EntityRef{ faction.id() }, x, z });
	}
	// A SquadInstance of `templateSid`, owned by `faction` when one is given.
	flecs::entity squad(const std::string& templateSid, flecs::entity faction = {})
	{
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity entity = state::createInstance(ecs);
		state::SquadInstance instance;
		instance.templateSid = templateSid;
		if (faction.is_valid())
			instance.faction = state::EntityRef{ faction.id() };
		entity.set<state::SquadInstance>(std::move(instance));
		return entity;
	}
	// An NPC of CHARACTER `character` ("" = none) with `faction`, `captor` chained on it at spawn.
	std::uint64_t add(const std::string& character, const std::string& faction, double x, double z, const std::string& captor = "")
	{
		state::Fighter f = w.data->makeFighter(*w.gd, w.body, "", rng);
		f.team = 0;
		f.x = x;
		f.z = z;
		f.yaw = 0.0;
		f.attention = 0.5;
		if (const kdata::Record* record = !character.empty() ? w.gd->find(character) : nullptr)
			f.npcClass = static_cast<std::uint8_t>(record->intOr("NPC class", 0));
		f.captorSid = captor;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{ faction });
		if (!character.empty())
			e.set<state::CharacterInstance>(state::CharacterInstance{ character });
		return e.get<state::Id>()->value;
	}
	flecs::entity body(std::uint64_t id)
	{
		flecs::entity found{};
		const_cast<flecs::world&>(sim->world().ecs()).each([&](flecs::entity entity, const state::Id& key)
		{
			if (key.value == id)
				found = entity;
		});
		return found;
	}
	void join(std::uint64_t character, flecs::entity squad)
	{
		body(character).add<state::MemberOf>(squad);
		f(character).squad = squad.get<state::Id>()->value;
	}
	// A town resident's baggage: the role, routine and job list a member of a squad carries.
	void resident(std::uint64_t character)
	{
		body(character).set<state::MemberRole>(state::MemberRole{ state::kRoleSquad });
		body(character).set<state::Routine>(state::Routine{});
		body(character).set<state::Jobs>(state::Jobs{});
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	const DialogueCoverage& cov() const { return sim->dialogue()->coverage(); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// A producer's message (EV 13, EV 39, ...) at the current tick.
	void push(int event, std::uint64_t speaker, std::uint64_t target)
	{
		DialogueEvent message;
		message.event = static_cast<std::uint8_t>(event);
		message.speaker = speaker;
		message.target = target;
		message.tick = sim->clockTick();
		sim->dialogue()->push(message);
	}
	// Ticks until `done` (checked before each tick), at most `cap`.
	bool run(int cap, const std::function<bool()>& done)
	{
		for (int i = 0; i < cap && !done(); ++i)
			sim->tick();
		return done();
	}
	void force(const std::string& character, const std::string& root)
	{
		sim->dialogue()->force(character, w.book->dialogueIndex(root));
	}
	// The last dialogue result of `speaker`, nullptr when it never spoke.
	const DialoguePick* lastPick(std::uint64_t speaker) const
	{
		const DialoguePick* found = nullptr;
		for (const DialoguePick& pick : sim->dialogue()->log())
		{
			if (pick.event.speaker == speaker)
				found = &pick;
		}
		return found;
	}
	const Simulation::ReplyMenuEvent* open(std::uint64_t speaker, std::uint64_t target)
	{
		menus.clear();
		sim->talkTo(target, speaker);
		tick();
		return menus.empty() ? nullptr : &menus.front();
	}
};

int optionOf(const Simulation::ReplyMenuEvent& menu, const std::string& line)
{
	for (std::size_t i = 0; i < menu.options.size(); ++i)
	{
		if (menu.options[i].first == line)
			return static_cast<int>(i);
	}
	return -1;
}

bool playerSquadHas(const PlayerSquadsView& view, std::uint64_t squad, std::uint64_t id)
{
	for (const PlayerSquadsView::Squad& list : view.squads)
	{
		if (list.id == squad)
			return std::find(list.members.begin(), list.members.end(), id) != list.members.end();
	}
	return false;
}

std::int64_t factionCash(const Rig& world, const std::string& sid)
{
	std::int64_t cash = 0;
	const_cast<flecs::world&>(world.sim->world().ecs()).each([&](const state::FactionState& faction)
	{
		if (faction.templateSid == sid)
			cash = faction.cash;
	});
	return cash;
}

// The recruit's fixture: the NPC squad (with the role, routine and jobs of a resident) the hire takes it out of.
std::uint64_t recruitInSquad(Rig& world, flecs::entity guild)
{
	const std::uint64_t recruit = world.add(world.w.recruit, world.w.guild, 100.0, 104.0);
	const flecs::entity squad = world.squad("5006-squad.mod", guild);
	world.join(recruit, squad);
	world.resident(recruit);
	return recruit;
}

// The hire answer's reply: the menu's seq and option.
void hireReply(Rig& world, const Simulation::ReplyMenuEvent& menu, std::uint64_t recruit)
{
	world.sim->enqueue(Command::dialogueReply(state::Id{ world.player }, state::Id{ recruit }, menu.menuSeq,
		static_cast<std::uint32_t>(optionOf(menu, "47623-Dialogue.mod"))));
}

} // namespace

int main()
{
	const Data w;

	// 1. DA_TAKE_MONEY credits the speaker's faction (D5 / decision 10): the wallet pays 6000 and the guild's cash takes it, save v70.
	{
		Rig world(w, 10000, true);
		const flecs::entity guild = world.factionState(w.guild);
		world.force(w.recruit, "47621-Dialogue.mod");
		const std::uint64_t recruit = recruitInSquad(world, guild);
		const Simulation::ReplyMenuEvent* menu = world.open(recruit, world.player);
		check(menu != nullptr && optionOf(*menu, "47623-Dialogue.mod") >= 0, "the recruit's hire answer is offered");
		if (menu == nullptr || optionOf(*menu, "47623-Dialogue.mod") < 0)
			return 1;
		hireReply(world, *menu, recruit);
		world.tick();
		world.tick();
		check(world.sim->playerSquads().money == 4000, "the payer's wallet is debited 6000 (10000 -> 4000)");
		check(factionCash(world, w.guild) == 6000, "the speaker's faction is credited 6000 (the D5 divergence: the exe burns the cats)");
		check(world.cov().actApplied[8] == 1 && world.cov().actSkipped[8] == 0, "DA_TAKE_MONEY applied once");
		const std::uint64_t before = world.sim->stateHash();
		std::stringstream bytes;
		world.sim->save(bytes);
		world.sim->reload(bytes);
		check(world.sim->stateHash() == before, "save / load keeps the hash");
		check(factionCash(world, w.guild) == 6000 && world.sim->playerSquads().money == 4000,
			"save v70 round-trips the faction cash and the wallet");
	}

	// 2. A payment the wallet no longer holds is refused and changes nothing; the join in front of it still runs.
	{
		Rig world(w, 10000, true);
		const flecs::entity guild = world.factionState(w.guild);
		world.force(w.recruit, "47621-Dialogue.mod");
		const std::uint64_t recruit = recruitInSquad(world, guild);
		const Simulation::ReplyMenuEvent* menu = world.open(recruit, world.player);
		check(menu != nullptr && optionOf(*menu, "47623-Dialogue.mod") >= 0, "the menu opens while the wallet holds the 6000");
		if (menu == nullptr || optionOf(*menu, "47623-Dialogue.mod") < 0)
			return 1;
		check(world.sim->adjustPlayerMoney(-9500), "the wallet drops to 500 while the menu waits");
		hireReply(world, *menu, recruit);
		world.tick();
		world.tick();
		check(world.sim->playerSquads().money == 500 && factionCash(world, w.guild) == 0,
			"the refused payment debits nothing and credits nothing");
		check(world.cov().actSkipped[8] == 1, "DA_TAKE_MONEY counted as refused");
		check(playerSquadHas(world.sim->playerSquads(), world.playerSquad, recruit), "the join in front of it still happened");
	}

	// 3. `give item`: Big Grim's gift line puts ten Building Materials in the target's inventory (the count is kept: 2..49).
	{
		Rig world(w);
		world.force(w.grim, "61857-Dialogue.mod");
		const std::uint64_t grim = world.add(w.grim, w.nameless, 102.0, 100.0);
		world.open(grim, world.player);
		const DialoguePick* picked = world.lastPick(grim);
		check(picked != nullptr && picked->result == DialoguePick::Result::Picked
				&& w.book->sidOf(w.book->dialogue(picked->dialogue).sid) == "61857-Dialogue.mod",
			"the talk roots 61857");
		check(world.run(3000, [&] { return world.cov().itemsGiven == 1; }), "the gift line 61860 runs (the conversation walks to it)");
		const state::Inventory* stock = world.body(world.player).get<state::Inventory>();
		check(stock != nullptr && stock->carried(w.item) == 10, "the player carries ten Building Materials");
		check(world.cov().itemsGiven == 1, "one give-item entry applied");
	}

	// 4. DA_SEPARATE_TO_MY_OWN_SQUAD: the speaker leaves its squad into one of its own; the mate stays behind.
	{
		Rig world(w);
		world.force(w.citizen, "6021-Dialogue.mod");
		const flecs::entity faction = world.factionState(w.dust);
		world.placeTown(world.factionState(w.cannibals), w.cannibalTown, 100.0, 100.0);
		const flecs::entity oldSquad = world.squad("5006-squad.mod", faction);
		const std::uint64_t citizen = world.add(w.citizen, w.dust, 102.0, 100.0);
		const std::uint64_t mate = world.add(w.citizen, w.dust, 104.0, 100.0);
		world.join(citizen, oldSquad);
		world.join(mate, oldSquad);
		world.push(13, citizen, world.player);
		check(world.run(200, [&] { return world.cov().actApplied[28] == 1; }), "the separate line 6022 runs");
		const flecs::entity moved = world.body(citizen).target<state::MemberOf>();
		check(moved.is_valid() && moved != oldSquad, "the speaker is a member of a new squad");
		check(world.body(mate).target<state::MemberOf>() == oldSquad, "the mate is still in the old squad");
		if (!moved.is_valid())
			return 1;
		std::size_t members = 0;
		const_cast<flecs::world&>(world.sim->world().ecs()).query_builder<const state::Id>().with<state::MemberOf>(moved).build().each(
			[&](const state::Id&) { ++members; });
		check(members == 1, "the new squad holds only the speaker");
		check(world.f(citizen).squad == moved.get<state::Id>()->value, "the fighter's squad key follows the new squad");
		const state::SquadInstance* instance = moved.get<state::SquadInstance>();
		check(instance != nullptr && instance->templateSid == "5006-squad.mod" && instance->faction.id == faction.id(),
			"the new squad keeps the template and the speaker's faction");
	}

	// 5. DA_REMOVE_SLAVE_STATUS: Grey's "talk to slave" root 59269 carries it on 59278. Its first line 59270 gates on DC_IS_SLAVE
	// (the target's slave state 1 or 2), which task AY implemented; the four WORLD_EVENT_STATE entries (DIALOGUE_ACTIONS_STATIC 1.6) are
	// live since task AN and all hold on this rig (a fresh world: no player relations, Bo / Jaegar / Tinfist alive). The player answers
	// "Yes. I heard you can help me..." and the chain reaches the effect.
	{
		Rig world(w, 0, true);
		world.force(w.grey, "59269-Dialogue.mod");
		const std::uint64_t grey = world.add(w.grey, w.antiSlavers, 102.0, 100.0);
		world.f(world.player).slaveState = 1;
		world.f(world.player).captorSid = w.antiSlavers;
		world.f(world.player).route.push_back(state::RoutePoint{ 300.0, 100.0, 100.0 });
		world.f(world.player).routeNext = 1;
		world.f(world.player).drive = state::DriveMode::Forward;
		world.f(world.player).hold = true;
		world.f(world.player).orderTarget = grey;
		world.sim->talkTo(world.player, grey);
		world.tick();
		const DialoguePick* pick = world.lastPick(grey);
		check(pick != nullptr && pick->result == DialoguePick::Result::Picked && world.cov().worldStateFailClosed == 0,
			"the slave root's world-state gate evaluates (not failed closed) and its DC_IS_SLAVE line is offered");
		// The client answers: "Yes. I heard you can help me...", then the "[...]" continue entry that opens each following line.
		bool done = false;
		for (int i = 0; i < 4000 && !done; ++i)
		{
			if (const auto menu = world.sim->replyMenu(grey))
			{
				int option = optionOf(*menu, "59274-Dialogue.mod");
				if (option < 0)
					option = optionOf(*menu, std::string()); // "" = the "[...]" continue entry
				if (option >= 0)
					world.sim->enqueue(Command::dialogueReply(state::Id{ world.player }, state::Id{ grey }, menu->menuSeq,
						static_cast<std::uint32_t>(option)));
			}
			world.tick();
			done = world.cov().actApplied[53] == 1;
		}
		check(done, "\"Yes. I heard you can help me...\" runs Grey's DA_REMOVE_SLAVE_STATUS line 59278");
		check(world.f(world.player).slaveState == 0 && world.f(world.player).captorSid.empty(),
			"the slave is free: no slave state, no captor");
		check(world.f(world.player).drive == state::DriveMode::Stop && world.f(world.player).route.empty()
				&& world.f(world.player).orderTarget == 0 && world.f(world.player).orderStop && !world.f(world.player).hold,
			"its orders are cleared (drive stopped, route dropped, the target released)");
		// The cage fields belong to the same effect; a hand-placed cage (no fixture line can set one and still pass the exe's
		// IS_IMPRISONED root gate, which the sim still answers 0) is freed by the direct call.
		world.f(world.player).cageBuilding = 901;
		world.f(world.player).cageSlot = 4;
		check(world.sim->removeSlaveStatus(world.player)
				&& world.f(world.player).cageBuilding == 0 && world.f(world.player).cageSlot == 0,
			"out of the cage slot");
		const std::uint64_t before = world.sim->stateHash();
		std::stringstream bytes;
		world.sim->save(bytes);
		world.sim->reload(bytes);
		check(world.sim->stateHash() == before, "save / load keeps the freed state");
	}

	// 6. DA_GIVE_TARGET_MY_SLAVES: the speaker's faction slaves change captor to the target's; other factions' stay.
	{
		Rig world(w);
		world.force(w.reaver, "59465-Dialogue.mod");
		const flecs::entity faction = world.factionState(w.reavers);
		const flecs::entity camp = world.squad(w.camp, faction);
		const std::uint64_t reaver = world.add(w.reaver, w.reavers, 102.0, 100.0);
		world.join(reaver, camp);
		world.resident(reaver); // a resident routine keeps the world-squad demotion from culling the fixture
		const std::uint64_t one = world.add("", w.reavers, 104.0, 104.0, w.reavers);
		const std::uint64_t two = world.add("", w.reavers, 106.0, 104.0, w.reavers);
		const std::uint64_t foreign = world.add("", w.dust, 108.0, 104.0, w.dust);
		world.push(39, reaver, world.player);
		check(world.run(2000, [&] { return world.cov().actApplied[24] == 1; }), "the delivery line 59468 runs");
		check(world.f(one).captorSid == w.nameless && world.f(two).captorSid == w.nameless,
			"the speaker's faction slaves are re-assigned to the player faction");
		check(world.f(foreign).captorSid == w.dust, "another faction's slave is untouched");
		// A carried slave is dropped from the list (the exe: "the carried are dropped"), a direct call since the fixture cannot carry one.
		const std::uint64_t carried = world.add("", w.reavers, 110.0, 104.0, w.reavers);
		world.f(carried).carriedBy = reaver;
		check(world.sim->giveSlavesTo(w.reavers, world.player), "the direct delivery call applies");
		check(world.f(carried).captorSid == w.reavers, "a carried slave keeps its captor");
	}

	// 7. DA_KNOCKOUT: a direct effect (its one line is unreachable), the KO timer is forced up and the target drops.
	{
		Rig world(w);
		const std::uint64_t npc = world.add("", w.dust, 102.0, 100.0);
		check(world.sim->knockOut(npc, 60.0), "knockOut applies to a living character");
		check(world.f(npc).unconscious && world.f(npc).koTimer >= 60.0, "it is unconscious with a 60 s timer");
		world.tick();
		check(world.f(npc).unconscious, "it stays down after a tick");
		world.f(npc).dead = true;
		check(!world.sim->knockOut(npc, 60.0), "a dead character cannot be knocked out");
	}

	std::printf("ksim_dialogue_econ: all checks passed\n");
	return 0;
}
