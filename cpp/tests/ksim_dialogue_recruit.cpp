// Recruitment through dialogue (DIALOGUE.md 5 and 6, FEATURES "Recruiting"): DA_JOIN_SQUAD_* as Command::ChangeSquad, DA_TAKE_MONEY /
// DA_GIVE_MONEY on the player faction's wallet, DC_PLAYERMONEY and DC_IS_RECRUITABLE with the roster cap.
//
// The fixture is a generic recruit ("Skeleton recruit 2500"), whose package roots "Skeleton for hire" (47621) under EV_PLAYER_TALK_TO_ME:
// "Here for recruits? I'm available for hire. 6000 Cats." and the answers "You're hired." (47623, DA_JOIN_SQUAD_WITH_EDIT then
// DA_TAKE_MONEY 6000, gated DC_PLAYERMONEY 6000), "I'm not interested" (47624) and "I don't have enough money..." (47625). The recruit
// stands in an NPC squad with its role, routine and job list.
//
//  1. the player has 10000 cats: the menu offers the hire; picking it pays 6000 and the recruit leaves its squad and joins the player
//     squad (a player member: no MemberOf / Routine / MemberRole / Jobs, playerControlled, team 0, the player faction's side). A save and
//     load keeps the wallet, the membership and the hash;
//  2. DC_PLAYERMONEY: with 5000 cats the hire answer is not offered (the "not interested" one is);
//  3. DC_IS_RECRUITABLE's roster cap: a full faction (30) roots nothing, so no menu opens.

#include "ksim/core/clock.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
	std::string nameless; // the player's faction (NEW_GAME's template)
	std::string guild; // a non-player faction the recruit stands with
	std::string recruit; // "Skeleton recruit 2500"
	int recruitClass = 0;

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
			if (r->stringID == "1233-gamedata.base")
				guild = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant" && body.empty())
				body = r->stringID;
			if (r->name == "Skeleton recruit 2500" && recruit.empty())
			{
				recruit = r->stringID;
				recruitClass = r->intOr("NPC class", 0);
			}
		}
		check(!nameless.empty() && !guild.empty() && !body.empty() && recruit == "47629-Dialogue.mod",
			"data records: the Nameless faction, the Adventurers Guild, a body and the generic recruit (" + recruit + ")");
		check(recruitClass == 9, "the generic recruit is an adventurer (NPC class 9: recruit-able)");
		check(book->dialogueIndex("47621-Dialogue.mod") != kNoIndex && book->lineIndex("47622-Dialogue.mod") != kNoIndex
			&& book->lineIndex("47623-Dialogue.mod") != kNoIndex && book->lineIndex("47624-Dialogue.mod") != kNoIndex,
			"the book has the recruit's root and answers");
	}
};

// A world with one player faction (a squad with `roster` characters), its wallet at `money`, and the generic recruit in an NPC squad 50 U
// away.
struct Rig
{
	const Data& w;
	std::unique_ptr<Simulation> sim;
	std::vector<Simulation::ReplyMenuEvent> menus;
	std::uint64_t playerSquad = 0;
	std::uint64_t player = 0;
	std::uint64_t npcSquad = 0;
	std::uint64_t recruit = 0;
	std::uint64_t rng = 3;

	Rig(const Data& data, std::int64_t money, int roster) : w(data)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 7));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim->enableCombat(w.data, settings);
		sim->enableDialogue(w.book);
		sim->setReplyMenus(true);
		sim->setReplyMenuObserver([this](const Simulation::ReplyMenuEvent& e) { menus.push_back(e); });
		sim->createPlayerFaction(w.nameless);
		playerSquad = sim->addPlayerSquad();
		for (int i = 0; i < roster; ++i)
		{
			state::Fighter f = w.data->makeFighter(*w.gd, w.body, "", rng);
			f.playerControlled = true;
			f.team = 0;
			f.x = 100.0 + 2.0 * i;
			f.z = 100.0;
			f.yaw = 0.0;
			flecs::entity e = sim->spawnFighter(std::move(f));
			e.set<state::Allegiance>(state::Allegiance{ w.nameless });
			const std::uint64_t id = e.get<state::Id>()->value;
			if (player == 0)
				player = id;
			sim->addPlayerMember(playerSquad, id);
		}
		if (money != 0)
			check(sim->adjustPlayerMoney(money), "the wallet is credited " + std::to_string(money));
		check(sim->playerSquads().money == money && sim->playerSquads().squads.size() == 1, "the player faction's wallet and one squad");

		// The NPC squad: a SquadInstance the recruit is a member of, with the role, routine and job list a town resident carries.
		flecs::world& ecs = const_cast<flecs::world&>(sim->world().ecs());
		flecs::entity squad = state::createInstance(ecs);
		squad.set<state::SquadInstance>(state::SquadInstance{"5006-squad.mod"});
		npcSquad = squad.get<state::Id>()->value;
		state::Fighter f = w.data->makeFighter(*w.gd, w.body, "", rng);
		f.playerControlled = false;
		f.team = 1;
		f.npcClass = static_cast<std::uint8_t>(w.recruitClass);
		f.x = 100.0;
		f.z = 150.0;
		f.yaw = 0.0;
		f.attention = 0.5;
		f.squad = npcSquad;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{ w.guild });
		e.set<state::CharacterInstance>(state::CharacterInstance{ w.recruit });
		e.add<state::MemberOf>(squad);
		e.set<state::MemberRole>(state::MemberRole{ state::kRoleSquad });
		e.set<state::Routine>(state::Routine{});
		e.set<state::Jobs>(state::Jobs{});
		recruit = e.get<state::Id>()->value;
	}
	// Grows the player faction after the fixture (a menu that waits while the roster fills up).
	void addMembers(int count)
	{
		for (int i = 0; i < count; ++i)
		{
			state::Fighter f = w.data->makeFighter(*w.gd, w.body, "", rng);
			f.playerControlled = true;
			f.team = 0;
			f.x = 100.0 + 2.0 * (30 + i);
			f.z = 100.0;
			flecs::entity e = sim->spawnFighter(std::move(f));
			e.set<state::Allegiance>(state::Allegiance{ w.nameless });
			sim->addPlayerMember(playerSquad, e.get<state::Id>()->value);
		}
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// The player's talk: the recruit's root is evaluated on the next tick.
	const Simulation::ReplyMenuEvent* open()
	{
		menus.clear();
		sim->talkTo(player, recruit);
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

bool playerSquadHas(const PlayerSquadsView& view, std::uint64_t playerSquad, std::uint64_t id)
{
	for (const PlayerSquadsView::Squad& squad : view.squads)
	{
		if (squad.id == playerSquad)
			return std::find(squad.members.begin(), squad.members.end(), id) != squad.members.end();
	}
	return false;
}

// The entity carrying state::Id `id` (Simulation::fighterEntity is private).
flecs::entity entityOf(const Simulation& sim, std::uint64_t id)
{
	flecs::entity found{};
	const_cast<flecs::world&>(sim.world().ecs()).each([&](flecs::entity entity, const state::Id& key)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

} // namespace

int main()
{
	const Data w;

	// 1. the hire: the wallet pays, the recruit joins, the join survives a save.
	{
		Rig world(w, 10000, 1);
		const Simulation::ReplyMenuEvent* menu = world.open();
		check(menu != nullptr && menu->speaker == world.recruit && menu->target == world.player && !menu->end,
			"the recruit's talk opens \"Skeleton for hire\" and a reply menu");
		if (menu == nullptr)
			return 1;
		for (const auto& option : menu->options)
			std::printf("    option %s \"%s\"\n", option.first.c_str(), option.second.c_str());
		const int hire = optionOf(*menu, "47623-Dialogue.mod");
		check(hire >= 0 && optionOf(*menu, "47624-Dialogue.mod") >= 0, "the menu offers \"You're hired.\" and \"I'm not interested\"");
		if (hire < 0)
			return 1;
		const std::uint32_t menuSeq = menu->menuSeq;
		world.sim->enqueue(Command::dialogueReply(state::Id{ world.player }, state::Id{ world.recruit }, menuSeq, static_cast<std::uint32_t>(hire)));
		world.tick(); // the reply: the line's effects run, ChangeSquad is enqueued for the next tick
		world.tick(); // ChangeSquad applies
		const PlayerSquadsView view = world.sim->playerSquads();
		check(view.money == 4000, "DA_TAKE_MONEY debited 6000 from the player faction's wallet (10000 -> 4000)");
		check(playerSquadHas(view, world.playerSquad, world.recruit), "the recruit is a member of the player's squad");
		const auto squads = world.sim->characterSquads();
		check(squads.count(world.recruit) != 0 && squads.at(world.recruit) == world.playerSquad, "the recruit's squad is the player's (no MemberOf left)");
		const flecs::entity body = entityOf(*world.sim, world.recruit);
		check(!body.target<state::MemberOf>().is_valid() && !body.has<state::Routine>() && !body.has<state::MemberRole>() && !body.has<state::Jobs>(),
			"the recruit left its NPC squad: no MemberOf, Routine, MemberRole or Jobs");
		const state::Fighter& f = world.f(world.recruit);
		check(f.playerControlled && f.team == 0 && f.faction == 0 && f.squad == ((std::uint64_t{1} << 62) | world.recruit),
			"the recruit is a player member: playerControlled, team 0, no faction, a squad of its own");
		const state::Allegiance* side = body.get<state::Allegiance>();
		check(side != nullptr && side->factionSid == w.nameless, "the recruit's allegiance is the player faction");
		const DialogueCoverage& cov = world.sim->dialogue()->coverage();
		check(cov.actApplied[3] == 1 && cov.actApplied[8] == 1 && cov.editorWindowsSkipped == 1,
			"the counters: JOIN_SQUAD_WITH_EDIT applied once (its editor window skipped), TAKE_MONEY once");

		// The saved world: the wallet, the membership and the hash.
		const std::uint64_t before = world.sim->stateHash();
		std::stringstream bytes;
		world.sim->save(bytes);
		world.sim->reload(bytes);
		const PlayerSquadsView loaded = world.sim->playerSquads();
		check(world.sim->stateHash() == before, "save / load keeps the hash");
		check(loaded.money == 4000 && playerSquadHas(loaded, world.playerSquad, world.recruit),
			"save / load keeps the wallet and the recruit in the player's squad");
		check(world.f(world.recruit).playerControlled, "save / load keeps the recruit player-controlled");
	}

	// 2. DC_PLAYERMONEY: 5000 cats do not buy a 6000-cat recruit.
	{
		Rig world(w, 5000, 1);
		const Simulation::ReplyMenuEvent* menu = world.open();
		check(menu != nullptr && optionOf(*menu, "47623-Dialogue.mod") < 0 && optionOf(*menu, "47624-Dialogue.mod") >= 0,
			"with 5000 cats the hire is not offered (DC_PLAYERMONEY 6000), the refusal is");
	}

	// 3. DC_IS_RECRUITABLE's roster cap: a full faction has no room for the recruit.
	{
		Rig world(w, 5000, 30);
		check(world.sim->playerSquads().factionCount == 30, "the fixture's player faction is full (30)");
		check(world.open() == nullptr && world.sim->conversation(world.recruit) == nullptr,
			"a full faction roots nothing: no conversation, no menu");
	}

	// 4. A join that cannot be made aborts the rest (DIALOGUE.md 3.4, H): the roster fills up while the menu waits, so the hire's
	// ChangeSquad is refused and the payment behind it never runs.
	{
		Rig world(w, 10000, 1);
		const Simulation::ReplyMenuEvent* menu = world.open();
		check(menu != nullptr, "the hire menu opens before the roster fills");
		const int hire = menu != nullptr ? optionOf(*menu, "47623-Dialogue.mod") : -1;
		if (hire < 0)
			return 1;
		const std::uint32_t menuSeq = menu->menuSeq;
		world.addMembers(29); // 30 now: the cap
		world.sim->enqueue(Command::dialogueReply(state::Id{ world.player }, state::Id{ world.recruit }, menuSeq, static_cast<std::uint32_t>(hire)));
		world.tick();
		world.tick();
		const PlayerSquadsView view = world.sim->playerSquads();
		check(view.money == 10000 && !playerSquadHas(view, world.playerSquad, world.recruit),
			"a failed join pays nothing and joins nobody");
		const DialogueCoverage& cov = world.sim->dialogue()->coverage();
		check(cov.actSkipped[3] == 1 && cov.actApplied[8] == 0, "the counters: the join skipped, the payment never reached");
	}

	std::printf("ksim_dialogue_recruit: all checks passed\n");
	return 0;
}
