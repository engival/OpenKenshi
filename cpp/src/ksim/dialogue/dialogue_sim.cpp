// The dialogue events of the sim (docs/DIALOGUE.md 8 slice 1, steps 3 and 4): the producers that are not the crime hooks (shoo from a
// building, the I_SEE_* family) and the adapter that lets the evaluator (dialogue_events.cpp) read the world.
//
// Producers run every kDialogueProduceSeconds from stepDialogue, after stepCrime (whose witness events were queued by fireDialogue):
//   SHOO (EV 9)   an NPC inside a building it owns (the door owner is its faction, or the building is its squad home), a player-
//                 faction character in the same building the observer is aware of, neither hostile nor friendly to it, the house not
//                 locked (a locked house is the trespass sensor's INTRUDER path, stepCrime), and the building not open to public (the
//                 exe's vf+0x290, its owning SQUAD_TEMPLATE's `public day`: a public bar never shoos). Re-fires every kShooRefire = 2 s
//                 (measured).
//   I_SEE_*       an NPC aware of a player-faction character: EV 66 when hostile (relation <= -30), EV 67 when friendly (>= 50), else EV 3.
//                 Fires when the awareness starts and re-fires every kSeeRefire = 30 s while it holds (UNSURE, DIALOGUE.md 2.1).
// The throttles are the DialogueSystem's transient map.
//
// Conversations (step 5, DIALOGUE.md 3): a Picked event opens a state::Conversation on its speaker (a busy speaker drops the event) and speaks
// the first line; when the line's speech timer runs out the next child is picked (DialogueSystem::next) or the conversation ends. A speaker
// that goes down or leaves the arena ends it early (SPEECH_END). Every spoken line stamps itself and its root in state::DialogueShown (the
// repetition limit). Saved: the conversations and the stamps.

#include "ksim/core/simulation.hpp"

#include "ksim/ai/blackboard.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <unordered_map>

namespace ksim
{

namespace
{

constexpr double kProduceSeconds = 0.5; // the producers' cadence
constexpr double kShooRefire = 2.0; // seconds (TRESPASS_LIVE: measured)
constexpr double kSeeRefire = 30.0; // seconds (UNSURE)
constexpr std::uint64_t kProduceTicks = static_cast<std::uint64_t>(kProduceSeconds / kTickSeconds + 0.5);
constexpr std::uint64_t kShooTicks = static_cast<std::uint64_t>(kShooRefire / kTickSeconds + 0.5);
constexpr std::uint64_t kSeeTicks = static_cast<std::uint64_t>(kSeeRefire / kTickSeconds + 0.5);
constexpr double kAlarmTimer = 30.0; // DA_CRIME_ALARM's crime timer, seconds (DIALOGUE_ACTIONS_LIVE d: live 30; the static read said 1)
constexpr int kEvPlayerTalkToMe = 1; // EV_PLAYER_TALK_TO_ME
constexpr double kTalkingRange = 260.0; // TALKING_RANGE (DAT_1416fdfec); the interjector's reach (Q10 PROVISIONAL)
constexpr int kKindShoo = 1;

// The events whose lines the original draws in the large font, at twice the bubble range (kinspect findings/speech_bubble.md): SPEECH flags
// bit 3.
bool largeBubble(int event)
{
	for (const int large : { 2, 11, 17, 26, 31, 34, 36, 40, 45, 74 })
	{
		if (event == large)
			return true;
	}
	return false;
}

// The "important" events (range x1.33; findings/speech_bubble.md 6, row +0x14a): the large ones and these. SPEECH flags bit 4.
bool importantBubble(int event)
{
	for (const int important : { 6, 13, 30, 46, 48, 51, 54 })
	{
		if (event == important)
			return true;
	}
	return largeBubble(event);
}
constexpr int kKindSee = 2;

} // namespace

void Simulation::enableDialogue(std::shared_ptr<const DialogueBook> book)
{
	dialogue_ = book != nullptr ? std::make_unique<DialogueSystem>(std::move(book), seed_) : nullptr;
}

const state::Conversation* Simulation::conversation(std::uint64_t id) const
{
	return combat_ != nullptr && combat_->find(id) != nullptr ? fighterEntity(id).get<state::Conversation>() : nullptr;
}

const state::DialogueMemory* Simulation::dialogueMemory(std::uint64_t id) const
{
	return combat_ != nullptr && combat_->find(id) != nullptr ? fighterEntity(id).get<state::DialogueMemory>() : nullptr;
}

Simulation::ReplyMenuEvent Simulation::menuEvent(std::uint64_t speaker, const state::Conversation& talk) const
{
	ReplyMenuEvent out;
	out.speaker = speaker;
	out.target = talk.target;
	out.menuSeq = static_cast<std::uint32_t>(talk.menuTick);
	for (const std::string& sid : talk.replies)
	{
		const std::uint32_t line = sid.empty() || dialogue_ == nullptr ? kNoIndex : dialogue_->book().lineIndex(sid);
		// The texts are rolled at the tick the menu opened, so a resend says the same (the exe rolls a variant, no repetition stamp)
		out.options.emplace_back(sid, line == kNoIndex ? std::string("[...]") : dialogue_->speech(line, talk.target, talk.menuTick).text);
	}
	return out;
}

std::optional<Simulation::ReplyMenuEvent> Simulation::replyMenu(std::uint64_t speaker) const
{
	const state::Conversation* talk = conversation(speaker);
	if (talk == nullptr || talk->replies.empty())
		return std::nullopt;
	ReplyMenuEvent out = menuEvent(speaker, *talk);
	out.resend = true;
	return out;
}

std::string Simulation::checkDialogueReply(std::uint64_t character, std::uint64_t speaker, std::uint32_t menuSeq, std::uint32_t option) const
{
	const state::Conversation* talk = conversation(speaker);
	if (talk == nullptr || talk->replies.empty() || talk->target != character)
		return "bad_target";
	const bool answered = std::any_of(dialogueAnswers_.begin(), dialogueAnswers_.end(), [&](const DialogueAnswer& a) { return a.speaker == speaker; });
	if (static_cast<std::uint32_t>(talk->menuTick) != menuSeq || answered || option >= talk->replies.size())
		return "stale";
	return std::string();
}

std::string Simulation::runDialogueReply(std::uint64_t character, std::uint64_t speaker, std::uint32_t menuSeq, std::uint32_t option, bool commit)
{
	std::string reason = checkDialogueReply(character, speaker, menuSeq, option);
	if (reason.empty() && commit)
		dialogueAnswers_.push_back({speaker, static_cast<std::int32_t>(option), 0});
	return reason;
}

// How long a character the player talked to stands still (state::TalkHold). OURS: the original holds while the window is open; the sim
// cannot see it, so this is the safety net that always ends the hold. A minute of game time, long enough to read a menu and trade.
constexpr double kTalkHoldSeconds = 60.0;
// An open menu keeps its speaker's hold this far ahead (renewed every other second or so).
constexpr double kMenuHoldSeconds = 10.0;

void Simulation::talkTo(std::uint64_t actor, std::uint64_t target, std::uint32_t seq)
{
	if (combat_ != nullptr)
		if (state::Fighter* who = combat_->find(target); who != nullptr && !who->playerControlled && !who->dead)
		{
			fighterEntity(target).set<state::TalkHold>(state::TalkHold{clockTick() + static_cast<std::uint64_t>(kTalkHoldSeconds / kTickSeconds)});
			who->clearRoute(); // he stops where he stands
			mirrorFighter(target);
		}
	if (dialogue_ == nullptr)
	{
		if (talkObserver_)
			talkObserver_(actor, target, seq);
		return;
	}
	talkSeqs_[actor] = seq;
	fireDialogue(target, actor, kEvPlayerTalkToMe);
}

std::vector<Simulation::ReplyMenuEvent> Simulation::openMenus() const
{
	std::vector<ReplyMenuEvent> out;
	if (combat_ == nullptr)
		return out;
	world_.ecs().each([&](const state::Id& id, const state::Conversation& talk)
	{
		if (talk.replies.empty())
			return;
		out.push_back(menuEvent(id.value, talk));
		out.back().resend = true;
	});
	std::sort(out.begin(), out.end(), [](const ReplyMenuEvent& a, const ReplyMenuEvent& b) { return a.speaker < b.speaker; });
	return out;
}

void Simulation::endMenusOf(std::uint64_t character, std::uint8_t reason)
{
	std::vector<std::uint64_t> speakers;
	world_.ecs().each([&](const state::Id& id, const state::Conversation& talk)
	{
		if (talk.target == character && !talk.replies.empty())
			speakers.push_back(id.value);
	});
	std::sort(speakers.begin(), speakers.end());
	for (const std::uint64_t speaker : speakers)
		runEndConversation(character, speaker, reason, true);
}

std::string Simulation::checkEndConversation(std::uint64_t character, std::uint64_t speaker) const
{
	const state::Conversation* talk = conversation(speaker);
	if (talk == nullptr || talk->replies.empty() || talk->target != character)
		return "bad_target";
	return std::string();
}

std::string Simulation::runEndConversation(std::uint64_t character, std::uint64_t speaker, std::uint8_t reason, bool commit)
{
	std::string refused = checkEndConversation(character, speaker);
	if (refused.empty() && commit)
		dialogueAnswers_.push_back({speaker, -1, reason});
	return refused;
}

std::uint32_t Simulation::menuEntries(std::uint64_t actor, std::uint64_t target)
{
	if (combat_ == nullptr || combat_->find(target) == nullptr)
		return 0;
	if (dialogue_ == nullptr)
		return traderSquad(target) != 0 ? 1 : 0;
	TalkProbe probe;
	probe.actor = actor;
	probe.target = target;
	stepDialogue(clockTick(), &probe);
	return probe.passes ? 1 : 0;
}

void Simulation::stepDialogue(std::uint64_t now, TalkProbe* probe)
{
	const std::vector<DialogueAnswer> answers = probe != nullptr ? std::vector<DialogueAnswer>() : std::move(dialogueAnswers_);
	if (probe == nullptr)
		dialogueAnswers_.clear();
	if (dialogue_ == nullptr || combat_ == nullptr || combat_->empty())
		return;
	flecs::world& ecs = ecsMutable();
	const kdata::GameData* data = gameData_.get();

	// Faction relations by sid (the crime step's rule: 100 for the same faction, the live value when both have a FactionState, else the
	// record's default relation), and the towns, both built on first use.
	std::unordered_map<std::string, flecs::entity_t> factions;
	bool factionsBuilt = false;
	std::unordered_map<std::string, double> relationCache;
	const auto relation = [&](const std::string& from, const std::string& to) -> double
	{
		if (from.empty() || to.empty())
			return 0.0;
		if (from == to)
			return 100.0;
		if (data == nullptr)
			return 0.0;
		const auto cached = relationCache.find(from + "\n" + to);
		if (cached != relationCache.end())
			return cached->second;
		if (!factionsBuilt)
		{
			factionsBuilt = true;
			ecs.each([&](flecs::entity entity, const state::FactionState& faction) { factions[faction.templateSid] = entity.id(); });
		}
		double value;
		const auto a = factions.find(from);
		const auto b = factions.find(to);
		if (a != factions.end() && b != factions.end())
		{
			value = factionRelation(*data, flecs::entity(ecs.c_ptr(), a->second), flecs::entity(ecs.c_ptr(), b->second));
		} else {
			value = seededRelation(*data, from, to);
		}
		relationCache.emplace(from + "\n" + to, value);
		return value;
	};
	const auto sideOf = [&](std::uint64_t id) -> std::string
	{
		const state::Allegiance* side = fighterEntity(id).get<state::Allegiance>();
		return side != nullptr ? side->factionSid : std::string();
	};
	const auto usable = [](const state::Fighter& f) { return !f.dead && !f.unconscious && !f.down && !f.asleep && f.carriedBy == 0; };

	// ---- the producers ----
	if (probe == nullptr && now % kProduceTicks == 0)
	{
		const auto& members = combat_->members();
		std::unordered_map<std::uint64_t, std::string> owners; // building -> the faction of its doors, built on first use
		bool ownersBuilt = false;
		std::unordered_map<std::uint64_t, bool> publicBuildings; // building -> open to public (vf+0x290)
		const auto openToPublic = [&](std::uint64_t key)
		{
			const auto found = publicBuildings.find(key);
			if (found != publicBuildings.end())
				return found->second;
			const bool value = world_.openToPublic(key, true);
			publicBuildings.emplace(key, value);
			return value;
		};
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const state::Fighter& observer = members[i].fighter;
			if (observer.playerControlled || !usable(observer))
				continue;
			const std::uint64_t observerId = members[i].id;
			const std::string observerSide = sideOf(observerId);
			bool ownBuilding = false;
			if (observer.building != 0)
			{
				ownBuilding = observer.homeBuilding == observer.building;
				if (!ownBuilding && !observerSide.empty())
				{
					if (!ownersBuilt)
					{
						ownersBuilt = true;
						ecs.each([&](const state::Door& door)
						{
							if (door.building != 0 && !door.ownerSid.empty())
								owners.emplace(door.building, door.ownerSid);
						});
					}
					const auto owner = owners.find(observer.building);
					ownBuilding = owner != owners.end() && owner->second == observerSide;
				}
			}
			for (const state::SightRecord& sighting : observer.sight)
			{
				const std::uint64_t seeKey = DialogueSystem::fireKey(kKindSee, observerId, sighting.target);
				const state::Fighter* other = combat_->find(sighting.target);
				if (!sighting.seen || other == nullptr || !usable(*other))
				{
					dialogue_->forgetFired(seeKey);
					continue;
				}
				if (!other->playerControlled)
					continue; // UNSURE: NPC-to-NPC sightings are squad-level events (slice 3); only player-faction targets now
				const std::string otherSide = sideOf(sighting.target);
				const double rel = relation(observerSide, otherSide);
				if (dialogue_->due(seeKey, now, kSeeTicks))
				{
					DialogueEvent seen;
					seen.event = static_cast<std::uint8_t>(rel <= crime::kHostileRelation ? ev::ISeeEnemyPlayer
						: rel >= crime::kFriendlyRelation ? ev::ISeeAllyPlayer : ev::ISeeNeutralSquad);
					seen.speaker = observerId;
					seen.target = sighting.target;
					seen.tick = now;
					dialogue_->push(seen);
				}
				// SHOO: a stranger the owner is aware of inside its building (task 161, TRESPASS_STATIC 1.3): not hostile, not friendly,
				// the house not locked, and the building not open to public (the exe's vf+0x290: a public bar never shoos; kinspect
				// findings/dialogue_pacing, live: 2.93 game h at a public bar's counter, zero dialogue).
				// A public building's staff shoo too once their lock / secure-bed goal is blocked by the stranger (k16.6.503: the original's closing
				// chain at a shop, findings shop_closing and dialogue_pacing).
				const state::Routine* routine = fighterEntity(observerId).get<state::Routine>();
				const bool closing = routine != nullptr && (routine->goal == routines::task::LockAllMyDoors
					|| routine->goal == routines::task::LockDoorsStayOutside || routine->goal == routines::task::GoHomeToBedSecure);
				if (ownBuilding && !observer.houseLocked && (!openToPublic(observer.building) || closing)
					&& other->building == observer.building && rel > crime::kHostileRelation
					&& rel < crime::kFriendlyRelation && dialogue_->due(DialogueSystem::fireKey(kKindShoo, observerId, sighting.target), now, kShooTicks))
				{
					DialogueEvent shoo;
					shoo.event = ev::ShooFromMyBuilding;
					shoo.speaker = observerId;
					shoo.target = sighting.target;
					shoo.tick = now;
					dialogue_->push(shoo);
				}
			}
			// The plan road (rwh.221 sections 2 / 3): the shoo plan step 161 Task_TalktoPlayer re-sends EV 9 to the goal's hand (the 158 /
			// 283 finder's occupancy scan, no sighting needed) while the plan runs. Same gates and pacing as the sighting road above (a
			// record's shoo and this one share a key). UNSURE: 140335e00 sends 0x11 instead of 9 when the house is locked and the target
			// not friendly; ours sends nothing then and leaves the intruder event to crime.cpp's trespass sensor.
			const state::Routine* plan = fighterEntity(observerId).get<state::Routine>();
			const bool planClosing = plan != nullptr && (plan->goal == routines::task::LockAllMyDoors
				|| plan->goal == routines::task::LockDoorsStayOutside || plan->goal == routines::task::GoHomeToBedSecure);
			if (plan != nullptr && plan->chainTarget != 0 && ownBuilding && !observer.houseLocked
				&& (planClosing || ((plan->goal == routines::task::ShooStrangers || plan->goal == routines::task::ShooStrangersPrivate)
					&& !openToPublic(observer.building))))
			{
				const state::Fighter* hand = combat_->find(plan->chainTarget);
				if (hand != nullptr && usable(*hand) && hand->building == observer.building)
				{
					const double rel = relation(observerSide, sideOf(plan->chainTarget));
					if (rel > crime::kHostileRelation && rel < crime::kFriendlyRelation
						&& dialogue_->due(DialogueSystem::fireKey(kKindShoo, observerId, plan->chainTarget), now, kShooTicks))
					{
						DialogueEvent shoo;
						shoo.event = ev::ShooFromMyBuilding;
						shoo.speaker = observerId;
						shoo.target = plan->chainTarget;
						shoo.tick = now;
						dialogue_->push(shoo);
					}
				}
			}
		}
	}
	if (probe == nullptr && dialogue_->queued() == 0 && ecs.count<state::Conversation>() == 0)
		return;

	// ---- the world, as the evaluator reads it ----
	struct Town
	{
		double x, z, radius;
		std::string owner;
		bool playerOwned;
		std::uint64_t ownerId = 0; // the owner's state::Id
		mutable int level = -1; // the size level (buildings::townLevel), computed on first use
	};
	std::vector<Town> towns;
	bool townsBuilt = false;
	const auto townAt = [&](double x, double z) -> const Town*
	{
		if (!townsBuilt)
		{
			townsBuilt = true;
			ecs.each([&](const state::TownState& town)
			{
				if (town.owner.id == 0 || !ecs.is_alive(town.owner.id))
					return;
				const flecs::entity owner(ecs.c_ptr(), town.owner.id);
				const state::FactionState* faction = owner.get<state::FactionState>();
				if (faction != nullptr && data != nullptr)
				{
					// The exe's current town (towncircle.hpp: `Character vf+0x78` = getTown, DECODED 1405c89b0 / GOG 1405c8350, the
					// Town radius vf+0x2a0; state block +0x88 carries it into the conditions - DIALOGUE_CONDITIONS_STATIC).
					const towncircle::Circle circle = towncircle::of(*data, town);
					towns.push_back({circle.x, circle.z, circle.radius, faction->templateSid, owner.has<state::PlayerFaction>(), owner.get<state::Id>()->value});
				}
			});
			// A later placement of a TOWN record (a save's outpost: the Skin Bandits' tower village): one TownState per record, so the
			// other placements are ExtraTowns; they are towns a character stands in all the same (kenshi-cym.28).
			ecs.each([&](const state::ExtraTown& extra)
			{
				if (!extra.placed || extra.owner.id == 0 || !ecs.is_alive(extra.owner.id) || data == nullptr)
					return;
				const flecs::entity owner(ecs.c_ptr(), extra.owner.id);
				const state::FactionState* faction = owner.get<state::FactionState>();
				if (faction == nullptr)
					return;
				state::TownState placed;
				placed.rootSid = extra.rootSid;
				placed.activeSid = extra.activeSid;
				placed.x = extra.x;
				placed.z = extra.z;
				const towncircle::Circle circle = towncircle::of(*data, placed);
				towns.push_back({circle.x, circle.z, circle.radius, faction->templateSid, owner.has<state::PlayerFaction>(), owner.get<state::Id>()->value});
			});
			// The player's own outposts (state::PlayerTown, BUILDINGS_SLICE 2): they are not record towns, so they carry their own
			// centre and radius. DC_IN_A_PLAYER_TOWN needs them (a player base is a town too, DIALOGUE_CONDITIONS_STATIC 75).
			ecs.each([&](const state::PlayerTown& town)
			{
				if (town.owner == 0 || !ecs.is_alive(town.owner) || town.radius <= 0.0)
					return;
				const flecs::entity owner(ecs.c_ptr(), town.owner);
				const state::FactionState* faction = owner.get<state::FactionState>();
				if (faction != nullptr)
					towns.push_back({town.x, town.z, town.radius, faction->templateSid, true, owner.get<state::Id>()->value});
			});
		}
		const Town* found = nullptr;
		double nearest = 1.0e18;
		for (const Town& town : towns)
		{
			const double d = std::hypot(x - town.x, z - town.z);
			if (d <= town.radius && d < nearest)
			{
				nearest = d;
				found = &town;
			}
		}
		return found;
	};

	// A player-owned town's size level (towncircle::levelOf), cached on the town.
	const auto townLevel = [&](const Town& town)
	{
		if (town.level < 0)
			town.level = data != nullptr ? towncircle::levelOf(ecs, *data, town.ownerId, {town.x, town.z, town.radius}) : 0;
		return town.level;
	};

	struct SquadStats
	{
		std::string templateSid;
		std::vector<std::uint64_t> members; // state::Id, sorted
	};
	std::unordered_map<flecs::entity_t, SquadStats> squads;
	const auto squadOf = [&](flecs::entity squad) -> const SquadStats&
	{
		auto [at, inserted] = squads.try_emplace(squad.id());
		if (inserted)
		{
			if (const state::SquadInstance* instance = squad.get<state::SquadInstance>())
				at->second.templateSid = instance->templateSid;
			ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](const state::Id& id)
			{
				at->second.members.push_back(id.value);
			});
			std::sort(at->second.members.begin(), at->second.members.end());
		}
		return at->second;
	};

	std::unordered_map<std::string, bool> traders;
	const DialogueBook& book = dialogue_->book();

	class Adapter : public DialogueWorld
	{
	public:
		Adapter(Simulation& sim, const std::function<double(const std::string&, const std::string&)>& relation,
			const std::function<const Town*(double, double)>& townAt, const std::function<const SquadStats&(flecs::entity)>& squadOf,
			const std::function<int(const Town&)>& townLevel, const std::function<void(const std::function<void(const Town&)>&)>& eachTown,
			std::unordered_map<std::string, bool>& traders, const DialogueBook& book, std::uint64_t now)
			: sim_(sim)
			, relation_(relation)
			, townAt_(townAt)
			, squadOf_(squadOf)
			, townLevel_(townLevel)
			, eachTown_(eachTown)
			, traders_(traders)
			, book_(book)
			, data_(sim.gameData())
			, now_(now)
		{
		}

		bool actor(std::uint64_t id, DialogueActor& out) const override
		{
			out = DialogueActor{};
			const state::Fighter* f = sim_.combat_->find(id);
			if (f == nullptr)
				return false;
			const flecs::entity entity = sim_.fighterEntity(id);
			out.exists = true;
			out.id = id;
			if (const state::CharacterInstance* character = entity.get<state::CharacterInstance>())
				out.character = character->templateSid;
			if (const state::Allegiance* side = entity.get<state::Allegiance>())
				out.faction = side->factionSid;
			out.race = f->raceSid;
			out.npcClass = crime::effectiveClass(f->npcClass, f->playerControlled);
			out.playerFaction = f->playerControlled;
			out.animal = f->animal;
			out.dead = f->dead;
			out.ko = f->unconscious;
			out.down = f->down;
			out.carried = f->carriedBy != 0;
			out.slaveState = f->slaveState;
			out.captor = f->captorSid;
			out.carries = f->carrying;
			// The location state 2, a cage (char+0x2f8; world_state.cpp counts a held slave too, UNSURE): the building the slave trade
			// caged it in, and the faction that owns the hold — the captor at the cuff, else the town the cage building stands in.
			out.imprisoned = f->cageBuilding != 0;
			if (out.imprisoned)
			{
				out.imprisonedBy = f->captorSid;
				if (out.imprisonedBy.empty())
				{
					const BuildingSite* cage = sim_.world().buildingSite(f->cageBuilding);
					if (cage != nullptr)
						if (const Town* town = townAt_(cage->cx, cage->cz))
							out.imprisonedBy = town->owner;
				}
				// IS_IN_LOCKED_CAGE: the cage's warden squad locked its cages (OURS: one flag per squad, not a per-cage lock).
				sim_.world().ecs().each([&](const state::SquadInstance& instance, const state::SquadRoutine& routine)
				{
					if (instance.homeBuilding == f->cageBuilding && routine.cagesLocked)
						out.cageLocked = true;
				});
			}
			// Within the walls (the gate regions of perception.hpp; DC_WITHIN_TOWN_WALLS's cached +0xa0, an approximation, UNSURE).
			out.inWalls = sim_.combat_->environment().inWalls(f->x, f->z);
			out.inCombat = perception::inCombat(*f);
			out.sneaking = f->sneak;
			out.unarmed = f->weapon.weaponType == 5;
			out.starving = f->hunger < 1.7;
			out.running = f->moveMode != 0 && perception::isMoving(*f);
			for (const state::FighterPart& part : f->parts)
			{
				if (part.type == 1 && part.health < 0.0)
					out.brokenLeg = true;
			}
			out.brokenArm = !f->rightArmOk || !f->leftArmOk;
			for (const state::FighterPart& part : f->parts)
			{
				if (part.type == 3) // the head: the first such part (UNSURE: the exe reads the medical system's first part)
				{
					out.damagedHead = part.health - part.stun < 0.2;
					break;
				}
			}
			// DC_NEARLY_KO / DC_IS_NEARLY_KO: the medical system's minimum over the organic parts of (health - stun) / max, clamped
			// at 1, below 0.1 (DIALOGUE_CONDITIONS_LIVE (d), DIALOGUE_CONDITIONS_STATIC 30 / 42). UNSURE: a severed limb reads as
			// nearly KO by the literal formula.
			double worst = 1.0;
			for (const state::FighterPart& part : f->parts)
			{
				if (part.robotic || part.maxHealth <= 0.0)
					continue;
				worst = std::min(worst, (part.health - part.stun) / part.maxHealth);
			}
			out.nearlyKO = worst < 0.1;
			out.x = f->x;
			out.z = f->z;
			out.building = f->building;
			out.homeBuilding = f->homeBuilding;
			out.houseLocked = f->houseLocked;
			// WEARING_LOCKED_SHACKLES: a worn ARMOUR piece marked `is locked` in the boots slot (9) or the body slot (5); the data's
			// three locked records are the prisoner shackles (DIALOGUE_CONDITIONS_STATIC 64).
			if (data_ != nullptr)
			{
				if (const state::Inventory* stock = entity.get<state::Inventory>())
				{
					for (const state::InventoryEntry& entry : stock->items)
					{
						if (entry.slot != state::Inventory::kArmourBase + 5 && entry.slot != state::Inventory::kArmourBase + 9)
							continue;
						const kdata::Record* record = data_->find(entry.itemSid);
						if (record != nullptr && record->boolOr("is locked", false))
							out.shackles = true;
					}
				}
			}
			out.stat = f->skills.toughness;
			if (const Town* town = townAt_(f->x, f->z))
			{
				out.townFaction = town->owner;
				out.inNonPlayerTown = !town->playerOwned;
				out.playerTown = town->playerOwned;
			}
			if (!out.character.empty())
			{
				const auto found = traders_.find(out.character);
				if (found != traders_.end())
					out.trader = found->second;
				else
					out.trader = traders_[out.character] = book_.characterFacts(out.character).trader;
			}
			if (const flecs::entity squad = entity.target<state::MemberOf>(); squad.is_valid())
			{
				const SquadStats& stats = squadOf_(squad);
				out.squadTemplate = stats.templateSid;
				out.squadSize = static_cast<int>(stats.members.size());
				out.squadAble = 0;
				out.squadOnlyAnimals = !stats.members.empty();
				out.leader = false;
				bool leaderFound = false;
				for (const std::uint64_t member : stats.members)
				{
					const state::Fighter* m = sim_.combat_->find(member);
					const bool able = m != nullptr && !m->dead && !m->unconscious && !m->down && m->carriedBy == 0;
					out.squadAble += able ? 1 : 0;
					if (m == nullptr || !m->animal)
						out.squadOnlyAnimals = false;
					if (!leaderFound && able)
					{
						leaderFound = true;
						out.leader = member == id; // the first able member in Id order (residents.cpp leaderOf)
					}
				}
			}
			// NUM_BACKPACKS (DIALOGUE_CONDITIONS_STATIC 37): over W's squad (W alone when it has none), the members wearing a
			// CONTAINER in the BACKPACK slot (the data's bags are CONTAINER records with `slot` 12).
			out.backpacks = backpackOf(id) ? 1 : 0;
			if (const flecs::entity squad = entity.target<state::MemberOf>(); squad.is_valid())
			{
				for (const std::uint64_t member : squadOf_(squad).members)
					out.backpacks += member != id && backpackOf(member) ? 1 : 0;
			}
			return true;
		}

		double relation(std::string_view from, std::string_view to) const override { return relation_(std::string(from), std::string(to)); }

		std::int32_t bounty(std::uint64_t id, std::string_view factionSid) const override
		{
			return sim_.bountyOn(id, std::string(factionSid));
		}

		bool metHours(std::uint64_t speaker, std::uint64_t target, int& hours) const override
		{
			const state::Fighter* f = sim_.combat_->find(speaker);
			std::uint64_t stamp = 0;
			if (f == nullptr || !f->metBefore(target, stamp))
				return false;
			hours = static_cast<int>((now_ > stamp ? now_ - stamp : 0) / kTicksPerGameHour);
			return true;
		}

		bool aware(std::uint64_t speaker, std::uint64_t other) const override
		{
			const state::Fighter* f = sim_.combat_->find(speaker);
			if (f == nullptr)
				return false;
			for (const state::SightRecord& sighting : f->sight)
			{
				if (sighting.seen && sighting.target == other)
					return true;
			}
			return false;
		}

		bool targetSquadCarries(std::uint64_t target, std::uint64_t speaker, std::span<const std::uint32_t> ids, std::uint64_t& member) const override
		{
			const auto listed = [&](std::uint32_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
			const auto test = [&](std::uint64_t who) -> bool
			{
				const state::Fighter* f = sim_.combat_->find(who);
				if (f == nullptr || f->dead || f->carriedBy != 0 || f->carrying == 0)
					return false;
				const state::CharacterInstance* character = sim_.fighterEntity(f->carrying).get<state::CharacterInstance>();
				if (character == nullptr || !listed(book_.idOf(character->templateSid)))
					return false;
				if (who != target && !aware(speaker, who))
					return false;
				member = who;
				return true;
			};
			member = 0;
			if (test(target))
				return true;
			const flecs::entity squad = sim_.fighterEntity(target).target<state::MemberOf>();
			if (!squad.is_valid())
				return false;
			for (const std::uint64_t mate : squadOf_(squad).members)
			{
				if (mate != target && test(mate))
					return true;
			}
			return false;
		}

		bool targetSquadHasItem(std::uint64_t target, std::span<const std::uint32_t> ids, std::uint64_t& member) const override
		{
			const auto pred = [&](const state::InventoryEntry& entry)
			{
				const std::uint32_t id = book_.idOf(entry.itemSid);
				return id != kNoIndex && std::find(ids.begin(), ids.end(), id) != ids.end();
			};
			return squadHolds(target, member, pred);
		}

		bool targetSquadHasItemType(std::uint64_t target, std::span<const std::uint32_t> ids, std::uint64_t& member) const override
		{
			if (data_ == nullptr)
				return false;
			// rwh.213: the exe reads only the first list entry's item function (item+0x124). Function 0 (Luxury Goods...) is
			// no constraint at all: the line passes for any target, empty hands included, with no retarget.
			const kdata::Record* first = ids.empty() ? nullptr : data_->find(book_.sidOf(ids.front()));
			const std::int32_t wanted = first != nullptr ? first->intOr("item function", 0) : 0;
			if (wanted == 0)
			{
				member = target;
				return true;
			}
			const auto pred = [&](const state::InventoryEntry& entry) // the main flat inventory only, not the backpack
			{
				const kdata::Record* record = data_->find(entry.itemSid);
				return entry.slot == state::Inventory::kCarried && record != nullptr && record->intOr("item function", 0) == wanted;
			};
			return squadHolds(target, member, pred, false);
		}

		bool illegalItem(std::uint64_t speaker, std::uint64_t who, std::uint64_t& member) const override
		{
			const state::Fighter* me = sim_.combat_->find(speaker);
			if (me == nullptr || data_ == nullptr)
				return false;
			// The law faction: the owner of the town S stands in, else S's own (DIALOGUE_CONDITIONS_STATIC 69).
			std::string law = sideOf(speaker);
			if (const Town* town = townAt_(me->x, me->z))
				law = town->owner;
			if (law.empty())
				return false;
			const std::vector<std::string>& banned = illegalList(law);
			const auto test = [&](std::uint64_t id) -> bool
			{
				const state::Inventory* stock = sim_.fighterEntity(id).get<state::Inventory>();
				return stock != nullptr && anyEntry(stock->items, [&](const state::InventoryEntry& entry)
				{
					return std::find(banned.begin(), banned.end(), entry.itemSid) != banned.end();
				});
			};
			member = 0;
			if (test(who))
			{
				member = who;
				return true;
			}
			// The squad members S knows (the exe reads the threat table); the first hit retargets the dialogue to that member.
			const flecs::entity squad = sim_.fighterEntity(who).target<state::MemberOf>();
			if (!squad.is_valid())
				return false;
			for (const std::uint64_t mate : squadOf_(squad).members)
			{
				if (mate == who || !aware(speaker, mate))
					continue;
				if (test(mate))
				{
					member = mate;
					return true;
				}
			}
			return false;
		}

		bool copsAround(std::uint64_t speaker) const override
		{
			const state::Fighter* f = sim_.combat_->find(speaker);
			if (f == nullptr)
				return false;
			for (const state::SightRecord& sighting : f->sight)
			{
				const state::Fighter* other = sim_.combat_->find(sighting.target);
				if (!sighting.seen || other == nullptr || other->dead)
					continue;
				const std::string side = sideOf(sighting.target);
				const Town* town = townAt_(other->x, other->z);
				if (crime::isLaw(crime::effectiveClass(other->npcClass, other->playerControlled), town != nullptr && town->owner == side))
					return true;
			}
			return false;
		}

		void threat(std::uint64_t speaker, int& enemies, int& allies) const override
		{
			enemies = allies = 0;
			const state::Fighter* f = sim_.combat_->find(speaker);
			if (f == nullptr)
				return;
			const std::string mine = sideOf(speaker);
			for (const state::SightRecord& sighting : f->sight)
			{
				const state::Fighter* other = sim_.combat_->find(sighting.target);
				if (!sighting.seen || other == nullptr || other->dead)
					continue;
				const double rel = relation_(mine, sideOf(sighting.target));
				if (rel <= crime::kHostileRelation)
					++enemies;
				else if (rel >= crime::kFriendlyRelation && !other->unconscious && !other->down)
					++allies;
			}
		}

		bool lastShown(std::uint32_t record, std::uint64_t& tick) const override
		{
			const state::DialogueShown* shown = sim_.world().ecs().get<state::DialogueShown>();
			if (shown == nullptr)
				return false;
			const std::string_view sid = book_.sidOf(record);
			const auto at = std::lower_bound(shown->stamps.begin(), shown->stamps.end(), sid,
				[](const state::ShownStamp& stamp, std::string_view key) { return stamp.sid < key; });
			if (at == shown->stamps.end() || at->sid != sid)
				return false;
			tick = at->tick;
			return true;
		}

		std::uint64_t interjector(std::uint64_t speaker) const override
		{
			const state::Fighter* me = sim_.combat_->find(speaker);
			if (me == nullptr)
				return 0;
			const flecs::entity squad = sim_.fighterEntity(speaker).target<state::MemberOf>();
			if (!squad.is_valid())
				return 0;
			std::uint64_t best = 0;
			double nearest = kTalkingRange;
			for (const std::uint64_t mate : squadOf_(squad).members)
			{
				const state::Fighter* f = sim_.combat_->find(mate);
				if (mate == speaker || f == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0 || f->playerControlled
					|| busy(mate))
					continue;
				const double d = std::hypot(f->x - me->x, f->z - me->z);
				if (d < nearest)
				{
					nearest = d;
					best = mate;
				}
			}
			return best;
		}

		bool squadBusy(std::uint64_t id) const override
		{
			if (sim_.combat_->find(id) == nullptr)
				return false;
			const flecs::entity squad = sim_.fighterEntity(id).target<state::MemberOf>();
			if (!squad.is_valid())
				return false;
			for (const std::uint64_t mate : squadOf_(squad).members)
			{
				if (busy(mate))
					return true;
			}
			return false;
		}

		bool busy(std::uint64_t id) const override
		{
			return sim_.combat_->find(id) != nullptr && sim_.fighterEntity(id).has<state::Conversation>();
		}

		bool playerContext(std::uint64_t target, DialoguePlayerContext& out) const override
		{
			if (target == 0)
				return false;
			const state::Fighter* t = sim_.combat_->find(target);
			if (t == nullptr || !t->playerControlled)
				return false;
			const PlayerSquadsView player = sim_.playerSquads();
			if (player.faction == 0)
				return false;
			out.money = player.money;
			out.roster = player.factionCount;
			out.cap = player.limits.faction;
			return true;
		}

		// DC_TOWN_LEVEL_CURRENT_LOCATION: the size level of the player-owned town the speaker stands in; false = none / not a player's.
		bool currentTownLevel(std::uint64_t speaker, int& level) const override
		{
			const state::Fighter* f = sim_.combat_->find(speaker);
			const Town* town = f != nullptr ? townAt_(f->x, f->z) : nullptr;
			if (town == nullptr || !town->playerOwned)
				return false;
			level = townLevel_(*town);
			return true;
		}

		// DC_PLAYERS_BEST_TOWN_LEVEL: the highest size level over the towns of the target's player faction, -1 with none.
		bool bestTownLevel(std::uint64_t target, int& level) const override
		{
			DialoguePlayerContext ignored;
			if (!playerContext(target, ignored))
				return false;
			const std::uint64_t faction = sim_.playerSquads().faction;
			level = -1;
			eachTown_([&](const Town& town)
			{
				if (town.playerOwned && town.ownerId == faction)
					level = std::max(level, townLevel_(town));
			});
			return true;
		}

		bool lockState(std::uint64_t speaker, std::uint32_t record, bool& locked) const override
		{
			if (sim_.combat_->find(speaker) == nullptr)
				return false;
			const state::DialogueMemory* memory = sim_.fighterEntity(speaker).get<state::DialogueMemory>();
			if (memory == nullptr)
				return false;
			const std::string_view sid = book_.sidOf(record);
			const auto at = std::lower_bound(memory->locks.begin(), memory->locks.end(), sid,
				[](const state::DialogueLock& lock, std::string_view key) { return lock.sid < key; });
			if (at == memory->locks.end() || at->sid != sid)
				return false;
			locked = at->locked;
			return true;
		}

		bool worldState(std::uint32_t state, bool& out) const override
		{
			if (!data_)
				return false;
			const std::string_view sid = book_.sidOf(state);
			if (data_->find(sid) == nullptr)
				return false; // a record the data does not define: undecidable, fails closed
			if (!facts_)
				facts_ = sim_.worldStateFacts(); // built on first use: a step that evaluates no world-state gate pays nothing
			out = worldstate::stateHolds(*data_, *facts_, sid);
			return true;
		}

	private:
		std::string sideOf(std::uint64_t id) const
		{
			const state::Allegiance* side = sim_.fighterEntity(id).get<state::Allegiance>();
			return side != nullptr ? side->factionSid : std::string();
		}

		// Every entry of an inventory, the worn backpack's contents and containers inside containers included.
		bool anyEntry(const std::vector<state::InventoryEntry>& items, const std::function<bool(const state::InventoryEntry&)>& pred) const
		{
			for (const state::InventoryEntry& entry : items)
			{
				if (pred(entry) || (!entry.contents.empty() && anyEntry(entry.contents, pred)))
					return true;
			}
			return false;
		}

		// The target's squad (the target alone when it has none) holds an entry `pred` accepts; true and `member` = the holder.
		bool squadHolds(std::uint64_t target, std::uint64_t& member, const std::function<bool(const state::InventoryEntry&)>& pred, bool deep = true) const
		{
			const auto test = [&](std::uint64_t who) -> bool
			{
				const state::Fighter* f = sim_.combat_->find(who);
				if (f == nullptr || f->dead || f->carriedBy != 0)
					return false;
				const state::Inventory* stock = sim_.fighterEntity(who).get<state::Inventory>();
				if (stock == nullptr || !(deep ? anyEntry(stock->items, pred) : std::any_of(stock->items.begin(), stock->items.end(), pred)))
					return false;
				member = who;
				return true;
			};
			member = 0;
			if (test(target))
				return true;
			const flecs::entity squad = sim_.fighterEntity(target).target<state::MemberOf>();
			if (!squad.is_valid())
				return false;
			for (const std::uint64_t mate : squadOf_(squad).members)
			{
				if (mate != target && test(mate))
					return true;
			}
			return false;
		}

		// NUM_BACKPACKS: a CONTAINER record worn in the BACKPACK slot (the data's bags are CONTAINER records with `slot` 12).
		bool backpackOf(std::uint64_t who) const
		{
			if (data_ == nullptr)
				return false;
			const state::Inventory* stock = sim_.fighterEntity(who).get<state::Inventory>();
			if (stock == nullptr)
				return false;
			for (const state::InventoryEntry& entry : stock->items)
			{
				if (entry.slot != state::Inventory::kBackpackSlot)
					continue;
				const kdata::Record* record = data_->find(entry.itemSid);
				if (record != nullptr && record->type == containerType_)
					return true;
			}
			return false;
		}

		// The law faction's illegal items: its FACTION `forbidden items` and the `illegal goods` of each of its ITEMS_CULTURE
		// trade cultures. UNSURE: the exe reads one list at faction+0x100; the Holy Nation lists Hashish only in its culture, so
		// this is their union (DIALOGUE_CONDITIONS_STATIC 69).
		const std::vector<std::string>& illegalList(const std::string& law) const
		{
			if (const auto found = illegal_.find(law); found != illegal_.end())
				return found->second;
			std::vector<std::string> banned;
			if (const kdata::Record* faction = data_->find(law))
			{
				if (const kdata::RefCategory* items = faction->findRefs("forbidden items"))
					for (const kdata::RefEntry& entry : items->entries)
						banned.push_back(entry.targetID);
				if (const kdata::RefCategory* cultures = faction->findRefs("trade culture"))
				{
					for (const kdata::RefEntry& entry : cultures->entries)
					{
						const kdata::Record* culture = data_->find(entry.targetID);
						if (culture == nullptr)
							continue;
						if (const kdata::RefCategory* goods = culture->findRefs("illegal goods"))
							for (const kdata::RefEntry& good : goods->entries)
								banned.push_back(good.targetID);
					}
				}
			}
			std::sort(banned.begin(), banned.end());
			banned.erase(std::unique(banned.begin(), banned.end()), banned.end());
			return illegal_.emplace(law, std::move(banned)).first->second;
		}

		Simulation& sim_;
		const std::function<double(const std::string&, const std::string&)>& relation_;
		const std::function<const Town*(double, double)>& townAt_;
		const std::function<const SquadStats&(flecs::entity)>& squadOf_;
		const std::function<int(const Town&)>& townLevel_;
		const std::function<void(const std::function<void(const Town&)>&)>& eachTown_;
		std::unordered_map<std::string, bool>& traders_;
		std::int32_t containerType_ = kworld::itemTypeByName("CONTAINER");
		mutable std::unordered_map<std::string, std::vector<std::string>> illegal_; // law faction sid -> its banned item sids
		const DialogueBook& book_;
		mutable std::optional<worldstate::Facts> facts_; // the dead / imprisoned sets and the player relations, on first use
		std::shared_ptr<const kdata::GameData> data_;
		std::uint64_t now_; // the step's tick: the age of the dialogue memory's sightings when the gate is evaluated
	};

	const std::function<double(const std::string&, const std::string&)> relationFn = relation;
	const std::function<const Town*(double, double)> townFn = townAt;
	const std::function<const SquadStats&(flecs::entity)> squadFn = squadOf;
	const std::function<int(const Town&)> levelFn = townLevel;
	const std::function<void(const std::function<void(const Town&)>&)> eachTownFn = [&](const std::function<void(const Town&)>& visit)
	{
		townAt(0.0, 0.0); // builds the list
		for (const Town& town : towns)
			visit(town);
	};
	const Adapter adapter(*this, relationFn, townFn, squadFn, levelFn, eachTownFn, traders, book, now);

	if (probe != nullptr)
	{
		// EV_PLAYER_TALK_TO_ME as a talk fires it: the target speaks, the actor is spoken to (the picks of the loop at the end of this step).
		const DialogueEvent event{kEvPlayerTalkToMe, probe->target, probe->actor, 0, now, 0};
		DialoguePick pick = dialogue_->evaluate(adapter, event);
		if (pick.result == DialoguePick::Result::Busy)
		{
			const state::Conversation* running = fighterEntity(probe->target).get<state::Conversation>();
			const std::uint32_t root = running != nullptr && !running->interrupt.empty() && running->replies.empty()
				? book.dialogueIndex(running->interrupt) : kNoIndex;
			if (root != kNoIndex)
				pick = dialogue_->interrupt(adapter, event, root);
		}
		probe->passes = pick.result == DialoguePick::Result::Picked && combat_->find(probe->target) != nullptr;
		return;
	}

	// ---- effects (step 6, DIALOGUE.md 5; the executor 140680560 and its order, DIALOGUE_ACTIONS_STATIC 1) ----
	// The world effects act with S = the line's voice (an interjector for an interjection's child), T = the conversation's target. The
	// dialogue's own state (the lock map, the speech timer) is the conversation owner's (UNSURE: which dlg the exe runs an interjection on).
	const auto lockRoot = [&](std::uint64_t who, std::string_view sid, bool locked)
	{
		flecs::entity entity = fighterEntity(who);
		state::DialogueMemory memory;
		if (const state::DialogueMemory* held = entity.get<state::DialogueMemory>())
			memory = *held;
		const auto at = std::lower_bound(memory.locks.begin(), memory.locks.end(), sid,
			[](const state::DialogueLock& lock, std::string_view key) { return lock.sid < key; });
		if (at != memory.locks.end() && at->sid == sid)
			at->locked = locked;
		else
			memory.locks.insert(at, state::DialogueLock{std::string(sid), locked});
		entity.set<state::DialogueMemory>(std::move(memory));
	};
	const auto factionOf = [&](const std::string& sid) -> flecs::entity
	{
		if (!factionsBuilt)
		{
			factionsBuilt = true;
			ecs.each([&](flecs::entity entity, const state::FactionState& faction) { factions[faction.templateSid] = entity.id(); });
		}
		const auto found = factions.find(sid);
		return found != factions.end() ? flecs::entity(ecs.c_ptr(), found->second) : flecs::entity();
	};
	// FactionRelations vf4 (1406b2ea0): `from`'s relation towards `to` += amount, clamped to [-100, 100], thresholds noted. A player faction's
	// own relations are a no-op (PlayerFactionRelations stubs it). False when a side has no FactionState (nothing to write).
	const auto affect = [&](const std::string& from, const std::string& to, double amount)
	{
		if (data == nullptr || from.empty() || to.empty() || from == to)
			return false;
		const flecs::entity a = factionOf(from);
		const flecs::entity b = factionOf(to);
		if (!a.is_valid() || !b.is_valid())
			return false;
		if (a.has<state::PlayerFaction>())
			return true;
		const double before = factionRelation(*data, a, b);
		const double after = std::clamp(before + amount, -100.0, 100.0);
		a.set<state::Relation>(b, state::Relation{after});
		noteRelationChange(ecs, a, b, before, after);
		relationCache.clear();
		return true;
	};
	// A replace=1 order (Character::giveOrder 1405d20d0): the character's current task ends first.
	const auto attack = [&](std::uint64_t who, std::uint64_t target)
	{
		state::Fighter* f = combat_->find(who);
		CombatArena::endFollow(*f);
		f->clearRoute();
		f->hold = false;
		f->orderTarget = target;
		mirrorFighter(who);
	};
	const auto able = [](const state::Fighter* f) { return f != nullptr && !f->dead && !f->unconscious && !f->down && f->carriedBy == 0; };
	// Conversations a DA_TRADE ended this step: the exe ends the dialogue before it opens the window (DIALOGUE_ACTIONS_STATIC 3).
	std::set<std::uint64_t> tradeEnded;
	const auto execute = [&](std::uint64_t owner, state::Conversation& talk, std::uint32_t dialogue, std::uint32_t line, std::uint64_t voice)
	{
		const LineDef& def = book.line(line);
		DialogueCoverage& cov = dialogue_->coverageMutable();
		const std::string rootSid(book.sidOf(book.dialogue(dialogue).sid));
		const auto dialogueSid = [&](std::uint32_t index) { return book.sidOf(book.dialogue(index).sid); };
		// 1. the lock maps: `unlock but keep me` (and the current root locked), `unlocks`, `locks`
		if (def.unlockKeepMe.count != 0)
		{
			for (const std::uint32_t index : book.ids(def.unlockKeepMe))
				lockRoot(owner, dialogueSid(index), false);
			lockRoot(owner, rootSid, true);
		}
		for (const std::uint32_t index : book.ids(def.unlocks))
			lockRoot(owner, dialogueSid(index), false);
		for (const std::uint32_t index : book.ids(def.locks))
			lockRoot(owner, dialogueSid(index), true);
		const state::Fighter* target = talk.target != 0 ? combat_->find(talk.target) : nullptr;
		const std::string mySide = sideOf(voice);
		const std::string myLegal = crime::legalFaction(data, mySide); // the faction that holds the bounties this speaker deals in
		const std::string theirSide = target != nullptr ? sideOf(talk.target) : std::string();
		// 3. `change relations`: the listed faction towards the target's player faction (UNSURE in multiplayer: T's, DIALOGUE.md 6)
		if (def.relChanges.count != 0)
		{
			const flecs::entity theirs = theirSide.empty() ? flecs::entity() : factionOf(theirSide);
			for (const RelChange& change : book.relChanges(def.relChanges))
			{
				if (theirs.is_valid() && theirs.has<state::PlayerFaction>())
					affect(std::string(book.sidOf(change.faction)), theirSide, change.amount);
			}
		}
		// 4. interrupt: the root the player's talk opens while this conversation runs
		if (def.interrupt != kNoIndex)
			talk.interrupt = std::string(dialogueSid(def.interrupt));
		// 5. give item: each entry into T's inventory, only with a target. The stack quantity is the exe's: 2..49 keeps `count`, else 1
		// (DIALOGUE_ACTIONS_STATIC 1.5). The entry is unplaced until the next inventory order settles the grid (settling here would drop
		// it on a full pack: grid::settle returns the overflow for the caller to destroy). The "Received <item>" HUD line is the
		// client's (a NOTICE later); an unknown record is skipped.
		if (def.giveItems.count != 0 && target != nullptr)
		{
			state::Inventory* stock = fighterEntity(talk.target).get_mut<state::Inventory>();
			for (const ItemGive& give : book.giveItems(def.giveItems))
			{
				const std::string_view sid = book.sidOf(give.item);
				if (stock == nullptr || sid.empty() || data == nullptr || data->find(std::string(sid)) == nullptr)
					continue;
				stock->add(std::string(sid), static_cast<std::uint32_t>(give.count >= 2 && give.count < 0x32 ? give.count : 1));
				++cov.itemsGiven;
			}
		}
		// 6. The package stack (dt19 section 3): `change AI` replaces the speaker's whole table with one priority-0 package
		// (non-player speakers only, as the original), and each `AI contract` record is pushed as a timed contract with the
		// entry's val0 hours (0 -> 10 min). The original's contract push clears the platoon members' persistent orders (for a
		// non-player faction); ours has no persistent orders - the selector re-issues its tasks - so nothing is cleared.
		{
			bool handled = true;
			const flecs::entity speakerSquad = fighterEntity(voice).target<state::MemberOf>();
			blackboard::Board* board = speakerSquad.is_valid() ? blackboardOf(speakerSquad) : nullptr;
			if (board == nullptr || data == nullptr)
			{
				handled = def.contracts.count == 0 && def.changeAi == kNoIndex;
			} else {
				const flecs::entity speakerFaction = factionOf(mySide);
				const bool playerSpeaker = speakerFaction.is_valid() && speakerFaction.has<state::PlayerFaction>();
				if (def.changeAi != kNoIndex && !playerSpeaker)
				{
					blackboard::Package package;
					if (blackboard::readPackage(*data, book.sidOf(def.changeAi), 0, package))
						blackboard::changeAI(*board, std::move(package));
				}
				const double hours = trader::gameHours(now);
				for (const AiContract& contract : book.contracts(def.contracts))
				{
					blackboard::Package package;
					if (blackboard::readPackage(*data, book.sidOf(contract.package), 0, package))
						blackboard::pushContract(*board, std::move(package), contract.hours, hours);
				}
			}
			if (!handled)
				++cov.lineListsSkipped;
		}
		// 7. crowd trigger: not applied yet (slice 3)
		if (def.crowdTrigger != kNoIndex)
			++cov.lineListsSkipped;
		// 8. the `effects` loop, in list order
		for (const ActDef& action : book.effects(def.effects))
		{
			bool applied = true;
			const bool haveTarget = able(target);
			switch (action.kind)
			{
			case 4: // DA_AFFECT_RELATIONS: S's faction towards T's
				applied = haveTarget && affect(mySide, theirSide, action.operand);
				break;
			case 3: // DA_JOIN_SQUAD_WITH_EDIT
			case 18: // DA_JOIN_SQUAD_FAST: the conversation's owner joins playerContext (T's player faction). The character editor window
				// of 3 is the client's: skipped, counted (DIALOGUE.md 5). Enqueued, like every cross-tick write; a join that cannot be made
				// aborts the line's remaining effects (DIALOGUE.md 3.4, H).
			{
				const std::uint64_t recruit = owner; // the NPC whose conversation this is (a reply is the target's line in it)
				applied = haveTarget && target->playerControlled && checkChangeSquad(recruit, 0).empty();
				if (applied)
				{
					Command join = Command::changeSquad(state::Id{recruit}, 0);
					join.tick = now + 1;
					enqueue(join);
					if (action.kind == 3)
						++cov.editorWindowsSkipped;
				}
				break;
			}
			case 19: // DA_REMEMBER_CHARACTER: S's dialogue memory takes an entry for T, stamped now (it backs DC_MET_TARGET_BEFORE /
				// DC_TARGET_LAST_SEEN_X_HOURS_AGO; the exe's entry is in the squad memory, UNSURE)
			{
				state::Fighter* me = combat_->find(voice);
				applied = haveTarget && me != nullptr;
				if (applied)
					me->rememberMet(talk.target, now);
				break;
			}
			case 8: // DA_TAKE_MONEY: T's faction pays, S's faction is credited (DIALOGUE.md D5 / decision 10, the divergence). The exe
				// refuses a payment over the payer's cash and carries on; a refused payment is counted here and changes nothing.
			{
				const bool paid = haveTarget && target->playerControlled && playerSquads().money >= action.operand
					&& adjustPlayerMoney(-static_cast<std::int64_t>(action.operand));
				if (paid)
					adjustFactionCash(mySide, action.operand); // no faction instance to credit = the exe's burn
				applied = paid;
				break;
			}
			case 9: // DA_GIVE_MONEY: playerContext's wallet is credited (the exe debits nobody)
				applied = haveTarget && target->playerControlled && adjustPlayerMoney(action.operand);
				break;
			case 10: // DA_PAY_BOUNTY: S buys the person T carries off T. T's side is paid that person's bounty under S's legal faction once
				// (the claim flag; halved when it is dead), then S lifts (213) and cages (112) the person. The PRISONER tags are a HOOK.
			{
				applied = haveTarget && target->carrying != 0;
				if (!applied)
					break;
				const state::Fighter* prisoner = combat_->find(target->carrying);
				if (prisoner == nullptr)
					break;
				const std::int32_t reward = claimBounty(target->carrying, myLegal, prisoner->dead);
				if (reward > 0 && !theirSide.empty())
				{
					if (target->playerControlled)
						adjustPlayerMoney(reward);
					else
						adjustFactionCash(theirSide, reward);
				}
				orderArrest(voice, target->carrying, goap::kLiftPersonSnatching);
				break;
			}
			case 30: // DA_ARREST_TARGETS_CARRIED_PERSON: S lifts (213) and cages (112) the person T carries
				applied = haveTarget && target->carrying != 0 && orderArrest(voice, target->carrying, goap::kLiftPersonSnatching);
				break;
			case 49: // DA_CLEAR_BOUNTY: S's faction erases T's bounty and clears its pending crime (the tag forgiveness is a HOOK)
				applied = haveTarget;
				if (applied)
					forgiveCrime(talk.target, myLegal);
				break;
			case 24: // DA_GIVE_TARGET_MY_SLAVES: every slave of S's faction is re-assigned to T (the carried ones are dropped)
				applied = haveTarget && giveSlavesTo(mySide, talk.target);
				break;
			case 28: // DA_SEPARATE_TO_MY_OWN_SQUAD: S splits into a squad of its own, or T when S is a player character (T may be none)
			{
				const state::Fighter* speaker = combat_->find(voice);
				const std::uint64_t who = speaker != nullptr && speaker->playerControlled ? talk.target : voice;
				applied = who != 0 && separateToOwnSquad(who);
				break;
			}
			case 53: // DA_REMOVE_SLAVE_STATUS: T is freed: no slave state or captor, out of the cage, its orders cleared
				applied = haveTarget && removeSlaveStatus(talk.target);
				break;
			case 25: // DA_TAG_ESCAPED_SLAVE: T becomes an escaping slave; its slave orders are cleared (UNSURE: the exe's escape
				// timer and the escapers' waypoint are not modelled)
			{
				state::Fighter* f = haveTarget ? combat_->find(talk.target) : nullptr;
				applied = f != nullptr;
				if (applied)
				{
					setSlaveState(talk.target, 2);
					f->goal = state::IndividualGoal{};
					cancelOrders(talk.target);
					CombatArena::endFollow(*f);
					f->clearRoute();
					f->drive = state::DriveMode::Stop;
					f->driveStrafe = 0;
					f->orderTarget = 0;
					f->orderStop = true;
					f->hold = false;
					mirrorFighter(talk.target);
				}
				break;
			}
			case 58: // DA_KNOCKOUT: S's knockout timer is forced to (at least) the operand, in sim seconds here (UNSURE: the exe's field units)
				applied = knockOut(voice, std::max(0.0, static_cast<double>(action.operand)));
				break;
			case 6: // DA_ATTACK_CHASE_FOREVER: TEMP_ENEMY (skipped by a friendly faction), DEFEAT_SQUAD with no range
			case 55: // DA_ATTACK_STAY_NEAR_HOME: the same with the chase range limited (task 248; UNSURE: no leash yet, step 6b)
			case 57: // DA_ATTACK_IF_NO_COEXIST: 6 unless S's faction coexists with T's
			{
				const state::Fighter* me = combat_->find(voice);
				applied = haveTarget && !(action.kind == 57 && combat_->coexists(me->faction, target->faction));
				if (!applied)
					break;
				perception::notice(*combat_->find(voice), talk.target); // 140858f40(S.threat, T, 0): the speaker force-notices T
				if (action.kind == 55 || relation(mySide, theirSide) < crime::kFriendlyRelation)
					combat_->markHostile(voice, talk.target);
				attack(voice, talk.target);
				break;
			}
			case 56: // DA_MASSIVE_ALARM: every loaded member of S's faction notices T, hears EV 66 (I_SEE_ENEMY_PLAYER) about it and gets
				// a queued task-29 move to T's position (replace=0). The reach is the faction's squads, not a distance (DIALOGUE_ACTIONS_LIVE (e));
				// UNSURE: the exe reaches only some squads of the faction (38 of 54 live), which squads is not decoded: all are taken
			{
				const state::Fighter* me = combat_->find(voice);
				applied = haveTarget && me != nullptr;
				if (!applied)
					break;
				std::vector<std::uint64_t> alarmed;
				for (const auto& member : combat_->members())
				{
					if (member.fighter.faction == me->faction && !member.fighter.playerControlled && !member.fighter.dead)
						alarmed.push_back(member.id);
				}
				for (const std::uint64_t id : alarmed)
				{
					perception::notice(*combat_->find(id), talk.target);
					DialogueEvent alarm;
					alarm.event = static_cast<std::uint8_t>(ev::ISeeEnemyPlayer);
					alarm.speaker = id;
					alarm.target = talk.target;
					alarm.tick = now;
					dialogue_->push(alarm);
					flecs::entity entity = fighterEntity(id);
					state::OrderQueue queue;
					if (const state::OrderQueue* held = entity.get<state::OrderQueue>())
						queue = *held;
					queue.orders.push_back({1, 0, 0, 0, target->x, target->y, target->z});
					entity.set<state::OrderQueue>(std::move(queue));
				}
				break;
			}
			case 29: // DA_ARREST_TARGET: the melee branch (TEMP_ENEMY + MELEE_ATTACK); and, when S stands in its own home town and T is not
				// carried, the arrest chain (240 lift, then 112): S carries T to a cage once it is down
				applied = haveTarget;
				if (applied)
				{
					combat_->markHostile(voice, talk.target);
					attack(voice, talk.target);
					if (target->carriedBy == 0 && atHomeTown(voice))
						orderArrest(voice, talk.target, goap::kLiftPersonSnatchingInTown);
				}
				break;
			case 20: // DA_FLAG_TEMP_ALLY
				applied = haveTarget;
				if (applied)
					combat_->forgiveHostile(voice, talk.target);
				break;
			case 21: // DA_FLAG_TEMP_ENEMY (direct: ignores relations)
				applied = haveTarget;
				if (applied)
					combat_->markHostile(voice, talk.target);
				break;
			case 16: // DA_FOLLOW_WHILE_TALKING: task 164 on T, ends with the conversation
			{
				applied = haveTarget && !combat_->followLoops(voice, talk.target);
				if (!applied)
					break;
				state::Fighter* f = combat_->find(voice);
				CombatArena::endFollow(*f);
				f->clearRoute();
				f->hold = false;
				f->orderTarget = 0;
				f->followTarget = talk.target;
				mirrorFighter(voice);
				talk.follower = voice;
				break;
			}
			case 34: // DA_RUN_AWAY: task 280 RUN_AWAY_FORCED from T (step 6b); UNSURE: the AGGRESSOR tag on T is not kept
				applied = haveTarget && orderRunAwayGoal(voice, talk.target);
				break;
			case 7: // DA_GO_HOME: task 19 GO_HOMEBUILDING (needs T, step 6b); UNSURE: the AGGRESSOR tag on T is not kept
				applied = haveTarget && giveIndividualGoal(voice, state::IndividualKind::GoHome, talk.target);
				break;
			case 52: // DA_SUMMON_MY_SQUAD: every other member of S's squad runs to S (task 67, step 6b)
			{
				applied = false;
				const flecs::entity squad = fighterEntity(voice).target<state::MemberOf>();
				if (!squad.is_valid())
					break;
				std::vector<std::uint64_t> mates;
				ecs.query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](const state::Id& mate)
				{
					if (mate.value != voice)
						mates.push_back(mate.value);
				});
				std::sort(mates.begin(), mates.end());
				for (const std::uint64_t mate : mates)
				{
					if (giveIndividualGoal(mate, state::IndividualKind::Summoned, voice))
						applied = true;
				}
				break;
			}
			case 12: // DA_FORCE_SPEECH_TIMER: seconds; the line holds until both timers ran out
				talk.until = std::max(talk.until, now + static_cast<std::uint64_t>(std::llround(std::max(0, action.operand) / kTickSeconds)));
				break;
			case 36: // DA_LOCK_THIS_DIALOG
				lockRoot(owner, rootSid, true);
				break;
			case 33: // DA_CRIME_ALARM: T's crime timer to 30 s (live), TRESPASSING when T has none and stands in S's faction's town
			{
				applied = target != nullptr && !target->dead;
				if (!applied)
					break;
				perception::notice(*combat_->find(voice), talk.target); // 140858f40(S.threat, T, 1): T goes on the speaker's alert list
				const state::CrimeRecord* held = crimeRecord(talk.target);
				const Town* town = townAt(target->x, target->z);
				if ((held == nullptr || held->crime == crime::kNone) && town != nullptr && town->owner == mySide)
					reportCrime(talk.target, crime::kTrespassing, mySide, 0);
				if (const state::CrimeRecord* after = crimeRecord(talk.target); after != nullptr && after->crime != crime::kNone)
				{
					state::CrimeRecord record = *after;
					crime::refreshTimer(record, kAlarmTimer);
					fighterEntity(talk.target).set<state::CrimeRecord>(std::move(record));
				}
				// The town's alarm manager (1409335e0; dt41_E section 1): kind = T's crime (char+0x148), raised by the speaker at T. UNSURE: whose
				// position the dialogue path passes (ours: the criminal's) and the UI "Alarm raised" message; the guards' answer is 209 / 210.
				if (const state::CrimeRecord* raised = crimeRecord(talk.target); raised != nullptr && raised->crime != crime::kNone)
					raiseAlarm(raised->crime, target->x, target->z, voice, talk.target);
				break;
			}
			case 32: // DA_ASSIGN_BOUNTY: a flat add when T has no pending crime, else commit it when the speaker assigns bounties
			{
				applied = target != nullptr && !myLegal.empty();
				if (!applied)
					break;
				const state::CrimeRecord* held = crimeRecord(talk.target);
				if ((held == nullptr || held->crime == crime::kNone) && action.operand > 0)
				{
					addBounty(talk.target, myLegal, action.operand);
				} else if (held != nullptr) {
					// UNSURE: the exe reads `assigns bounties` through the speaker's faction (1405a4470); the data has it on CHARACTER
					const state::CharacterInstance* who = fighterEntity(voice).get<state::CharacterInstance>();
					const kdata::Record* record = who != nullptr && data != nullptr ? data->find(who->templateSid) : nullptr;
					applied = record != nullptr && record->boolOr("assigns bounties", false);
					if (applied)
					{
						state::CrimeRecord copy = *held;
						crime::commit(copy, myLegal, crime::effectiveClass(target->npcClass, target->playerControlled),
							gameHoursIn(clockTick()));
						fighterEntity(talk.target).set<state::CrimeRecord>(std::move(copy));
					}
				}
				break;
			}
			case 1: // DA_TRADE: the trade window of S's squad opens for T (the exe ends the dialogue first, then opens it; DIALOGUE_ACTIONS_STATIC 3)
				applied = haveTarget && target->playerControlled && traderSquad(voice) != 0;
				if (applied)
				{
					// The end goes out first (event 44 reason 0, finished), so no reply menu stays open behind TRADE.
					if (talk.menuTick != 0 && replyMenuObserver_)
					{
						ReplyMenuEvent out;
						out.end = true;
						out.speaker = owner;
						out.target = talk.target;
						out.menuSeq = static_cast<std::uint32_t>(talk.menuTick);
						replyMenuObserver_(out);
					}
					talk.menuTick = 0;
					tradeEnded.insert(owner);
					if (windowObserver_)
						windowObserver_(voice, talk.target, 1);
				}
				break;
			case da::None:
				break;
			default:
				applied = false;
				break;
			}
			const std::size_t kind = action.kind < kDialogueActionCount ? action.kind : 0;
			++(applied ? cov.actApplied : cov.actSkipped)[kind];
			if (!applied && (action.kind == 3 || action.kind == 18))
				break; // DIALOGUE.md 3.4 (H): a JOIN_SQUAD that fails aborts the line's remaining effects
		}
	};

	// ---- conversations ----
	const auto stamp = [&](std::string_view sid)
	{
		state::DialogueShown shown;
		if (const state::DialogueShown* current = ecs.get<state::DialogueShown>())
			shown = *current;
		const auto at = std::lower_bound(shown.stamps.begin(), shown.stamps.end(), sid,
			[](const state::ShownStamp& entry, std::string_view key) { return entry.sid < key; });
		if (at != shown.stamps.end() && at->sid == sid)
			at->tick = now;
		else
			shown.stamps.insert(at, state::ShownStamp{std::string(sid), now});
		ecs.set<state::DialogueShown>(std::move(shown));
	};
	// `voice` says the line; its effects act for `owner` (a reply is the target's line in the speaker's conversation, DIALOGUE.md 3.5),
	// for the voice otherwise. The text variant is rolled at `rolled` (a menu answer says what its menu showed).
	const auto speak = [&](std::uint64_t owner, state::Conversation& talk, std::uint32_t dialogue, std::uint32_t line, std::uint64_t voice,
		std::uint64_t rolled)
	{
		const bool reply = voice == talk.target;
		const DialogueSystem::Speech said = dialogue_->speech(line, voice, rolled);
		talk.dialogue = std::string(book.sidOf(book.dialogue(dialogue).sid));
		talk.line = std::string(book.sidOf(book.line(line).sid));
		talk.voice = voice;
		talk.until = now + static_cast<std::uint64_t>(std::llround(said.hours * static_cast<double>(kTicksPerGameHour)));
		stamp(talk.line);
		stamp(talk.dialogue);
		// The effects run when the line starts (the exe shows the text first); the bubble shows until both timers ran out, so SPEECH is sent
		// after them with the forced timer (DA_FORCE_SPEECH_TIMER) folded into its duration.
		execute(owner, talk, dialogue, line, reply ? owner : voice);
		if (speechObserver_)
		{
			SpeechEvent out;
			out.speaker = voice;
			out.target = reply ? owner : talk.target; // whom it is said to: a reply answers the conversation's owner
			out.dialogue = talk.dialogue;
			out.line = talk.line;
			out.text = said.text;
			// The wire carries wall seconds at 1x (the client's bubble); the line itself holds the game hours in ticks.
			out.seconds = std::max(said.hours * kGameHourSeconds, static_cast<double>(talk.until - now) * kTickSeconds);
			out.flags = static_cast<std::uint8_t>((voice != owner && !reply ? 2 : 0) | (reply ? 4 : 0) | (largeBubble(talk.event) ? 8 : 0) | (importantBubble(talk.event) ? 16 : 0));
			speechObserver_(out);
		}
	};
	// After a line of the speaker's: the reply menu for a player-controlled target (DIALOGUE.md 3.3; DIALOGUE_SELECTION_STATIC Q8: a
	// root that is not a monologue, or the player's talk). A target already answering another menu gets none (13.6), as one with no client.
	const auto offer = [&](std::uint64_t owner, state::Conversation& talk, std::uint32_t dialogue, std::uint32_t line)
	{
		talk.replies.clear();
		const state::Fighter* target = replyMenus_ && talk.target != 0 ? combat_->find(talk.target) : nullptr;
		if (!able(target) || !target->playerControlled || (book.dialogue(dialogue).monologue && talk.event != kEvPlayerTalkToMe))
			return;
		bool held = false;
		ecs.each([&](const state::Id& id, const state::Conversation& other)
		{
			held = held || (id.value != owner && other.target == talk.target && !other.replies.empty());
		});
		if (held)
			return;
		for (const std::uint32_t option : dialogue_->menu(adapter, owner, talk.target, line))
			talk.replies.push_back(option == kNoIndex ? std::string() : std::string(book.sidOf(book.line(option).sid)));
		if (talk.replies.empty())
			return;
		talk.menuTick = now;
		if (replyMenuObserver_)
			replyMenuObserver_(menuEvent(owner, talk));
	};
	// The end of a conversation: a follow DA_FOLLOW_WHILE_TALKING started ends with it (a goal that expires, DIALOGUE.md 3.6); one that
	// showed a menu tells the target's client (CONVERSATION_END `reason`).
	const auto finish = [&](flecs::entity entity, const state::Conversation& talk, std::uint8_t reason)
	{
		if (talk.menuTick != 0 && replyMenuObserver_)
		{
			ReplyMenuEvent out;
			out.end = true;
			out.reason = reason;
			out.speaker = entity.get<state::Id>()->value;
			out.target = talk.target;
			out.menuSeq = static_cast<std::uint32_t>(talk.menuTick);
			replyMenuObserver_(out);
		}
		if (state::Fighter* f = talk.follower != 0 ? combat_->find(talk.follower) : nullptr; f != nullptr && f->followTarget == talk.target)
		{
			CombatArena::endFollow(*f);
			mirrorFighter(talk.follower);
		}
		entity.remove<state::Conversation>();
	};
	const auto cutShort = [&](std::uint64_t voice)
	{
		if (!speechObserver_)
			return;
		SpeechEvent out;
		out.end = true;
		out.speaker = voice;
		speechObserver_(out);
	};

	// The answers of this tick's commands: a reply speaks the target's line in the speaker's conversation ("[...]": the speaker goes on at
	// once), an end closes it.
	for (const DialogueAnswer& answer : answers)
	{
		if (combat_->find(answer.speaker) == nullptr)
			continue;
		const flecs::entity entity = fighterEntity(answer.speaker);
		const state::Conversation* held = entity.get<state::Conversation>();
		if (held == nullptr || held->replies.empty())
			continue;
		state::Conversation talk = *held;
		if (answer.option < 0 || static_cast<std::size_t>(answer.option) >= talk.replies.size())
		{
			finish(entity, talk, answer.reason);
			continue;
		}
		const std::string sid = talk.replies[static_cast<std::size_t>(answer.option)];
		talk.replies.clear();
		const std::uint32_t dialogue = book.dialogueIndex(talk.dialogue);
		const std::uint32_t line = sid.empty() ? kNoIndex : book.lineIndex(sid);
		if (sid.empty())
			talk.until = now;
		else if (dialogue != kNoIndex && line != kNoIndex)
			speak(answer.speaker, talk, dialogue, line, talk.target, talk.menuTick);
		if (tradeEnded.erase(answer.speaker) != 0)
		{
			finish(entity, talk, 0);
			continue;
		}
		entity.set<state::Conversation>(std::move(talk));
	}

	// The running ones first, in Id order: a line whose timer ran out gives way to the next child, or the conversation ends.
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> talking;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::Conversation&)
	{
		talking.emplace_back(id.value, entity.id());
	});
	std::sort(talking.begin(), talking.end());
	for (const auto& [owner, entityId] : talking)
	{
		const flecs::entity entity(ecs.c_ptr(), entityId);
		state::Conversation talk = *entity.get<state::Conversation>();
		const state::Fighter* me = combat_->find(owner);
		const state::Fighter* voice = combat_->find(talk.voice);
		if (!able(me) || !able(voice) || (!talk.replies.empty() && !able(combat_->find(talk.target))))
		{
			if (now < talk.until)
				cutShort(talk.voice);
			finish(entity, talk, 3);
			continue;
		}
		// Under Hold the speaker of an open menu stands where it is while the menu waits (§13.4): its TalkHold is kept a few seconds ahead.
		if (!talk.replies.empty() && !me->playerControlled)
		{
			const std::uint64_t until = now + static_cast<std::uint64_t>(kMenuHoldSeconds / kTickSeconds);
			const state::TalkHold* hold = entity.get<state::TalkHold>();
			if (hold == nullptr || hold->until < until - static_cast<std::uint64_t>(0.5 * kMenuHoldSeconds / kTickSeconds))
				entity.set<state::TalkHold>(state::TalkHold{until});
		}
		// An open menu's leash (§13.4): S and T further apart than 2 x TALKING_RANGE (a push, a carry; the grace's Hold) walked away.
		if (const state::Fighter* target = combat_->find(talk.target);
			!talk.replies.empty() && std::hypot(me->x - target->x, me->z - target->z) > 2.0 * kTalkingRange)
		{
			finish(entity, talk, 4);
			continue;
		}
		if (now < talk.until || !talk.replies.empty())
			continue;
		const std::uint32_t dialogue = book.dialogueIndex(talk.dialogue);
		const std::uint32_t line = book.lineIndex(talk.line);
		const DialoguePick next = dialogue != kNoIndex && line != kNoIndex
			? dialogue_->next(adapter, owner, talk.target, dialogue, line, now) : DialoguePick{};
		if (next.result != DialoguePick::Result::Picked)
		{
			finish(entity, talk, 0);
			continue;
		}
		speak(owner, talk, next.dialogue, next.line, next.voice, now);
		if (next.voice == owner)
			offer(owner, talk, next.dialogue, next.line);
		entity.set<state::Conversation>(std::move(talk));
	}

	const auto nothingToSay = [&](std::uint64_t actor, std::uint64_t target)
	{
		const auto seq = talkSeqs_.find(actor);
		const std::uint32_t clientSeq = seq != talkSeqs_.end() ? seq->second : 0;
		if (seq != talkSeqs_.end())
			talkSeqs_.erase(seq);
		if (talkObserver_)
			talkObserver_(actor, target, clientSeq);
	};
	// New events: a picked root opens a conversation (a speaker that started one earlier in this step drops the rest). The player's talk
	// to a busy speaker opens the running conversation's interrupt root in its place; a talk that opens nothing is nothing_to_say.
	dialogue_->step(adapter, now, [&](DialoguePick& pick)
	{
		const bool talked = pick.event.event == kEvPlayerTalkToMe;
		if (talked && pick.result == DialoguePick::Result::Busy && combat_->find(pick.event.speaker) != nullptr)
		{
			const flecs::entity entity = fighterEntity(pick.event.speaker);
			const state::Conversation* running = entity.get<state::Conversation>();
			const std::uint32_t root = running != nullptr && !running->interrupt.empty() ? book.dialogueIndex(running->interrupt) : kNoIndex;
			if (root == kNoIndex && running != nullptr && running->replies.empty() && running->event != kEvPlayerTalkToMe)
			{
				// No interrupt root: the player's talk ends the running ambient conversation (no menu open) and is evaluated again next step
				// (UNSURE what the original does, DIALOGUE.md 13: without this the barman's own greeting to the arriving player made the talk
				// nothing_to_say, k16.6.169).
				const state::Conversation old = *running;
				if (now < old.until)
					cutShort(old.voice);
				finish(entity, old, 2);
				dialogue_->push(pick.event);
				return;
			}
			if (root != kNoIndex && running->replies.empty())
			{
				pick = dialogue_->interrupt(adapter, pick.event, root);
				if (pick.result == DialoguePick::Result::Picked)
				{
					const state::Conversation old = *running;
					if (now < old.until)
						cutShort(old.voice);
					finish(entity, old, 2);
				}
			}
		}
		if (pick.result != DialoguePick::Result::Picked || combat_->find(pick.event.speaker) == nullptr)
		{
			if (talked)
				nothingToSay(pick.event.target, pick.event.speaker);
			return;
		}
		const flecs::entity entity = fighterEntity(pick.event.speaker);
		if (entity.has<state::Conversation>())
		{
			if (talked)
				nothingToSay(pick.event.target, pick.event.speaker);
			return;
		}
		state::Conversation talk;
		talk.target = pick.event.target;
		talk.started = now;
		talk.event = pick.event.event;
		speak(pick.event.speaker, talk, pick.dialogue, pick.line, pick.voice != 0 ? pick.voice : pick.event.speaker, now);
		if (talked)
			talkSeqs_.erase(pick.event.target);
		if (tradeEnded.erase(pick.event.speaker) != 0)
			return; // a root line that opened the trade ended its conversation (no menu, nothing kept)
		if (talk.voice == pick.event.speaker)
			offer(pick.event.speaker, talk, pick.dialogue, pick.line);
		entity.set<state::Conversation>(std::move(talk));
	});
}

} // namespace ksim
