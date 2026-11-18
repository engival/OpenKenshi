// The live WORLD_EVENT_STATE set (include/ksim/world/world_state.hpp, docs/reference/WORLD_MODEL.md "WORLD_EVENT_STATE semantics", task AN), on the
// real data:
//
//  1. the evaluator: a state is the AND of its entries; `NPC is` / `NPC is NOT` compare a unique character's status (alive 1, dead 0,
//     imprisoned 2); a character with no instance counts as alive; `player ally` / `player enemy` read the player faction's relations; a
//     `town okay` state is not expressible yet (false). The fresh world (every unique NPC alive, no player relations, involvement ignored)
//     has 69 of the 152 states true (WORLD_MODEL.md a);
//  2. the dialogue `world state` gate is live: the data's own "Tinfist is not alive" root (root 59107-Dialogue.mod "test1", line
//     59108-Dialogue.mod under EV_PLAYER_TALK_TO_ME, speaker "Test2 copy") opens nothing while Tinfist is alive and speaks its line once he
//     is dead. The kill is the player's: a player controlled fighter with an ordered attack lands the blow (Tinfist's body is prepared at
//     the brink of a vital part's death limit, so the first landed hit is the kill -- the real hit path attributes it);
//  3. the involvement set: the player's kill and a direct `noteNpcImprisoned` both land in state::WorldEventState::playerTouched, and a save
//     and load keep the dead and involvement sets (v72) with the same state hash.

#include "ksim/core/clock.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/world_state.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr std::string_view kSpeaker = "63278-Dialogue.mod"; // CHARACTER "Test2 copy": its package roots the "Tinfist is not alive" line
constexpr std::string_view kTinfist = "59030-Dialogue.mod"; // CHARACTER Tinfist (unique)
constexpr std::string_view kEsata = "43159-Dialogue.mod"; // CHARACTER Esata the Stone Golem (unique)
constexpr std::string_view kRoot = "59107-Dialogue.mod"; // DIALOGUE "test1"
constexpr std::string_view kLine = "59108-Dialogue.mod"; // its only line, text "Tinfist is not alive 1", gated on state 59029
constexpr std::string_view kTinfistDead = "59029-Dialogue.mod"; // WORLD_EVENT_STATE "Tinfist is not alive" (`NPC is NOT` Tinfist v0 1)
constexpr std::string_view kEsataImprisoned = "58586-Dialogue.mod"; // WORLD_EVENT_STATE "Esata Imprisoned by player" (v0 2)
constexpr std::string_view kPhoenixKilled = "58291-Dialogue.mod"; // WORLD_EVENT_STATE "Player killed Phoenix" (`NPC is` Phoenix v0 0)
constexpr std::string_view kPhoenix = "17225-Dialogue.mod"; // CHARACTER Holy Lord Phoenix (unique)

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
	std::string servant; // the body every rig character is built from
	std::string nameless, bandits;

	Data()
	{
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA)));
		data = CombatData::build(*gd, KENSHI_DATA);
		book = DialogueBook::build(gd);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "Nameless")
				nameless = r->stringID;
			if (r->name == "Dust Bandits")
				bandits = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant" && servant.empty())
				servant = r->stringID;
		}
		check(!servant.empty() && !nameless.empty() && !bandits.empty(), "data: the servant body, the Nameless and Dust Bandits factions");
		for (const std::string_view sid : {kSpeaker, kTinfist, kEsata, kPhoenix, kRoot, kLine, kTinfistDead, kEsataImprisoned, kPhoenixKilled})
			check(gd->find(sid) != nullptr, "record " + std::string(sid));
	}
};

struct Rig
{
	const Data& w;
	std::unique_ptr<Simulation> sim;
	std::uint64_t rng = 7;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> nothingToSay; // (actor, target) of a talk that opened nothing

	explicit Rig(const Data& world, std::uint64_t seed = 7)
		: w(world)
	{
		CombatSettings settings;
		settings.substeps = 2;
		// No degeneration or bleeding death: the test prepares one vital part near its death limit and needs it to stay there until the
		// player's blow (the setting scales the medical sink and the bleed only, not the hit damage).
		settings.chanceOfDeath = 0.0;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim->enableCombat(w.data, settings);
		sim->enableDialogue(w.book);
		sim->setTalkObserver([this](std::uint64_t actor, std::uint64_t target, std::uint32_t) { nothingToSay.emplace_back(actor, target); });
	}

	// A character of `character` ("" = none) in `faction` at (x, z) facing yaw.
	std::uint64_t add(const std::string& character, const std::string& faction, int cls, double x, double z, double yaw, bool player = false)
	{
		state::Fighter f = w.data->makeFighter(*w.gd, w.servant, "", rng);
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
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// The last pick the dialogue log holds for `speaker`, or null.
	const DialoguePick* lastPick(std::uint64_t speaker) const
	{
		for (auto it = sim->dialogue()->log().rbegin(); it != sim->dialogue()->log().rend(); ++it)
		{
			if (it->event.speaker == speaker)
				return &*it;
		}
		return nullptr;
	}
};

using Facts = worldstate::Facts;

Facts freshFacts() { return Facts{}; }

// ---- 1. the evaluator ----

void evaluator(const Data& w)
{
	check(worldstate::countStates(*w.gd) == 152, "the data holds 152 WORLD_EVENT_STATE records");
	check(worldstate::countTrue(*w.gd, freshFacts()) == 69, "69 are true on a fresh world (every unique NPC alive, no player relations)");

	const Facts fresh = freshFacts();
	check(!worldstate::stateHolds(*w.gd, fresh, kTinfistDead), "a fresh world: \"Tinfist is not alive\" is false");
	Facts deadTinfist = fresh;
	deadTinfist.npcStatus[std::string(kTinfist)] = worldstate::kDead;
	check(worldstate::stateHolds(*w.gd, deadTinfist, kTinfistDead), "Tinfist dead: \"Tinfist is not alive\" holds");
	// The saved dead set remembers a unique character whose body is gone (no instance left): the facts still read dead.
	{
		flecs::world scratch;
		state::registerComponents(scratch);
		state::WorldEventState events;
		events.dead.push_back(std::string(kTinfist));
		scratch.set<state::WorldEventState>(std::move(events));
		Facts fromWorld;
		worldstate::collectStatuses(scratch, fromWorld);
		check(worldstate::stateHolds(*w.gd, fromWorld, kTinfistDead), "the dead set alone flips the state with no instance in the world");
	}

	// A character with no instance counts as alive: the `NPC is alive` state of a character the sim never spawns fails to hold only when the
	// facts say dead, and a state that requires death starts false.
	check(!worldstate::stateHolds(*w.gd, fresh, kPhoenixKilled), "\"Player killed Phoenix\" is false with Phoenix never seen (not seen = alive)");
	Facts deadPhoenix = fresh;
	deadPhoenix.npcStatus[std::string(kPhoenix)] = worldstate::kDead;
	check(worldstate::stateHolds(*w.gd, deadPhoenix, kPhoenixKilled), "Phoenix dead: \"Player killed Phoenix\" holds");
	check(!worldstate::involved(*w.gd, fresh, *w.gd->find(kPhoenixKilled)), "involvement is not set by the status alone");
	check(!worldstate::involved(*w.gd, deadPhoenix, *w.gd->find(kPhoenixKilled)), "nor by the death: only the player's own action sets it");
	Facts touchedPhoenix = deadPhoenix;
	touchedPhoenix.playerTouched.insert(std::string(kPhoenix));
	check(worldstate::involved(*w.gd, touchedPhoenix, *w.gd->find(kPhoenixKilled)), "the involvement set is what marks the player's part");

	// Imprisonment: `NPC is` v0 2 compares the status 2 (a held slave or a caged body), not 1.
	Facts esataCaged = fresh;
	esataCaged.npcStatus[std::string(kEsata)] = worldstate::kImprisoned;
	check(worldstate::stateHolds(*w.gd, esataCaged, kEsataImprisoned), "Esata imprisoned by the player: the state holds at status 2");
	Facts esataAlive = fresh;
	esataAlive.npcStatus[std::string(kEsata)] = worldstate::kAlive;
	check(!worldstate::stateHolds(*w.gd, esataAlive, kEsataImprisoned), "and not at status 1");

	// The player relation entries: an ally state needs a relation at or above 50, an enemy one at or below -30.
	Facts ally = fresh;
	ally.playerRelation = [](std::string_view) { return 80.0; };
	Facts hostile = fresh;
	hostile.playerRelation = [](std::string_view) { return -60.0; };
	std::size_t allyTrue = 0, hostileTrue = 0, allyTrueHostile = 0;
	for (const kdata::Record* state : w.gd->byType(kworld::itemTypeByName("WORLD_EVENT_STATE")))
	{
		const bool a = worldstate::stateHolds(*w.gd, ally, *state);
		const bool h = worldstate::stateHolds(*w.gd, hostile, *state);
		allyTrue += a ? 1 : 0;
		hostileTrue += h ? 1 : 0;
		allyTrueHostile += a && h && state->findRefs("player ally") != nullptr ? 1 : 0;
	}
	std::printf("    states true with the player allied to all factions: %zu; hostile to all: %zu\n", allyTrue, hostileTrue);
	check(allyTrue > 69 && hostileTrue > 69, "player relations move the states that name them");
	check(allyTrueHostile == 0, "no `player ally` state holds when the player is hostile to all");
}

// ---- 2. the dialogue gate before and after the flip ----

void dialogueGate(const Data& w)
{
	Rig rig(w);
	rig.sim->createPlayerFaction(w.nameless);
	// Far from the fight: nobody sees it, so the speaker stays out of it.
	const std::uint64_t speaker = rig.add(std::string(kSpeaker), w.nameless, 1, 900.0, 900.0, 0.0);
	const std::uint64_t player = rig.add("", w.nameless, 9, 0.0, 0.0, 0.0, true);
	const std::uint64_t tinfist = rig.add(std::string(kTinfist), w.bandits, 9, 0.0, 8.0, 3.14159265);

	// Before: Tinfist alive, so the only line under the root fails its world-state gate. The talk is fired, the root is offered, and nothing
	// is spoken (the gate is decidable: the fail-closed counter stays 0).
	rig.sim->talkTo(player, speaker);
	rig.tick(4);
	const DialoguePick* before = rig.lastPick(speaker);
	check(before != nullptr, "the talk was evaluated");
	check(before != nullptr && before->result != DialoguePick::Result::Picked, "Tinfist alive: the world-state line does not pick");
	check(before != nullptr && before->candidates >= 1 && before->passed == 0,
		"the root is offered and no line passes (candidates " + std::to_string(before != nullptr ? before->candidates : 0) + ", passed "
			+ std::to_string(before != nullptr ? before->passed : 0) + ")");
	check(!rig.nothingToSay.empty(), "a talk that opens nothing is nothing_to_say");
	check(rig.sim->dialogue()->coverage().worldStateFailClosed == 0, "a decidable world state is evaluated, not failed closed");

	// The flip: the player's own ordered attack. Tinfist's Chest is prepared just above its death limit (maxHealth 10, health -9: the death
	// check is health - stun < -(maxHealth - wear), so a landed hit of 1 or more kills; a toughness of 100 puts the KO point at -99, so the
	// body is conscious until that hit). With the degeneration sink off it stays there; the player's blows are made to land on it.
	state::Fighter& victim = rig.f(tinfist);
	victim.skills.toughness = 100.0;
	for (state::FighterPart& part : victim.parts)
	{
		if (part.name == "Chest")
		{
			part.maxHealth = 10.0;
			part.wear = 0.0;
			part.health = -9.0;
			part.stun = 0.0;
			part.chance = 1000.0; // the hit table: the player's blows land on the prepared part
		} else {
			part.chance = 0.01;
		}
	}
	for (state::FighterPart& part : rig.f(player).parts)
	{
		part.maxHealth = 1000.0;
		part.health = 1000.0;
	}
	rig.sim->enqueue(Command::fighterAttack(state::Id{ player }, state::Id{ tinfist }));
	int ticks = 0;
	for (; ticks < 20 * 60 && !rig.f(tinfist).dead; ++ticks)
		rig.tick();
	std::printf("    Tinfist dead after %d ticks (%s)\n", ticks, ticks < 20 * 60 ? "the player's blow" : "NOT KILLED");
	check(ticks < 20 * 60, "the player's ordered attack kills Tinfist");

	const state::WorldEventState* events = rig.sim->worldEventState();
	check(events != nullptr && std::find(events->dead.begin(), events->dead.end(), kTinfist) != events->dead.end(),
		"the death records Tinfist in the dead set");
	check(events != nullptr && std::find(events->playerTouched.begin(), events->playerTouched.end(), kTinfist) != events->playerTouched.end(),
		"the player's kill records the involvement");
	check(worldstate::stateHolds(*w.gd, rig.sim->worldStateFacts(), kTinfistDead), "the live facts now hold \"Tinfist is not alive\"");

	// After: the same talk speaks the line.
	const std::size_t failsBefore = rig.nothingToSay.size();
	rig.sim->talkTo(player, speaker);
	rig.tick(4);
	const DialoguePick* after = rig.lastPick(speaker);
	check(after != nullptr && after->result == DialoguePick::Result::Picked, "Tinfist dead: the talk picks");
	check(after != nullptr && after->line == w.book->lineIndex(kLine), "and it is the world-state line "
		+ std::string(w.book->sidOf(w.book->line(after != nullptr ? after->line : 0).sid)));
	if (after != nullptr && after->dialogue != kNoIndex)
		std::printf("    picked root %s, line %s\n", std::string(w.book->sidOf(w.book->dialogue(after->dialogue).sid)).c_str(),
			std::string(w.book->sidOf(w.book->line(after->line).sid)).c_str());
	check(rig.nothingToSay.size() == failsBefore, "this talk does not end in nothing_to_say");

	// Imprisonment through a direct effect: the player's capture of Esata sets the involvement and the status (a caged body), and the
	// imprisoned state reads it.
	const std::uint64_t esata = rig.add(std::string(kEsata), w.bandits, 9, 2000.0, 2000.0, 0.0);
	rig.sim->noteNpcImprisoned(esata, rig.sim->playerFactionSid());
	check(rig.sim->playerFactionSid() == w.nameless, "the player faction is the created one (" + rig.sim->playerFactionSid() + ")");
	rig.f(esata).cageBuilding = 77;
	rig.tick(2);
	const state::WorldEventState* afterCapture = rig.sim->worldEventState();
	check(afterCapture != nullptr && std::find(afterCapture->playerTouched.begin(), afterCapture->playerTouched.end(), kEsata) != afterCapture->playerTouched.end(),
		"the direct imprisonment records the involvement");
	check(worldstate::stateHolds(*w.gd, rig.sim->worldStateFacts(), kEsataImprisoned), "\"Esata Imprisoned by player\" holds with the body caged");

	// The save keeps the dead and involvement sets (v72) and the state hash.
	const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_world_state.save";
	rig.sim->saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
	loaded.enableCombat(w.data, CombatSettings{});
	const state::WorldEventState* back = loaded.worldEventState();
	check(back != nullptr && std::find(back->dead.begin(), back->dead.end(), kTinfist) != back->dead.end(), "the loaded save keeps Tinfist dead");
	check(back != nullptr && std::find(back->playerTouched.begin(), back->playerTouched.end(), kTinfist) != back->playerTouched.end()
		&& std::find(back->playerTouched.begin(), back->playerTouched.end(), kEsata) != back->playerTouched.end(),
		"and the involvement set (the kill and the imprisonment)");
	check(worldstate::stateHolds(*w.gd, loaded.worldStateFacts(), kTinfistDead), "the loaded facts hold the flipped state");
	check(loaded.stateHash() == rig.sim->stateHash(), "the save round-trips to the same state hash");
	std::remove(path.c_str());
}

} // namespace

int main()
{
	try {
		const Data w;
		evaluator(w);
		dialogueGate(w);
		std::printf("ksim_world_state: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_world_state: " << e.what() << "\n";
		return 1;
	}
}
