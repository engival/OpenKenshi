#include "ksim/dialogue/dialogue_events.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/core/rng.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <unordered_set>

namespace ksim {

namespace {

constexpr double kTalkingRangeSq = 67600.0; // 260 units (DAT_1416fdfec)
constexpr double kAllyRelation = 50.0;
constexpr double kEnemyRelation = -30.0;
constexpr double kStrongerRatio = 1.5;
constexpr double kOutnumberedRatio = 1.3;
constexpr double kStarvingBelow = 1.7;
constexpr std::uint32_t kLoveTags = 0xc; // LT_MY_LIFESAVER | LT_FREED_ME
constexpr std::uint32_t kHateTags = kLongTermHateTags;
constexpr int kMaxDepth = 12;

bool has(std::span<const std::uint32_t> list, std::uint32_t id)
{
	return std::find(list.begin(), list.end(), id) != list.end();
}

int popcount(std::uint32_t value)
{
	int n = 0;
	for (; value != 0; value &= value - 1)
		++n;
	return n;
}

std::uint64_t mix(std::uint64_t a, std::uint64_t b)
{
	return splitMix64(a ^ splitMix64(b + 0x9e3779b97f4a7c15ull));
}

} // namespace

std::uint64_t dialogueRoll(std::uint64_t seed, std::uint64_t speaker, std::uint64_t tick, std::uint32_t record, std::uint32_t salt) noexcept
{
	return mix(mix(mix(mix(streamSeed(seed, kDialogueStream), speaker), tick), record), salt);
}

bool dialogueCondImplemented(int kind) noexcept
{
	switch (kind)
	{
	case dc::Relations: case dc::PlayerMoney: case dc::IsATrader: case 4: case 5: case dc::ImprisonedByTarget: case dc::ImprisonedByOther:
	case dc::IsImprisoned: case 11: case 16: case 17: case dc::TargetLastSeenXHoursAgo: case dc::IsLeader: case dc::MetTargetBefore: case 21:
	case 22: case 24: case 25: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case dc::SquadSize: case 36: case dc::NumBackpacks:
	case 38: case 39: case 41: case 42: case 43: case 44: case dc::IsFemale: case 46: case 47: case 48: case 52: case 53: case 56:
	case dc::WithinTownWalls: case 59: case 61: case 62: case 63: case 64: case 65: case 67: case 68: case 71: case dc::TownLevelCurrentLocation: case dc::PlayersBestTownLevel: case dc::InAPlayerTown: case 76:
	case dc::IsRecruitable: case dc::HasRobotLimbs: case dc::HasIllegalItem:
		return true;
	default:
		return false;
	}
}

bool dialogueListImplemented(int list) noexcept
{
	switch (list)
	{
	case GlInTownOf: case GlMyFaction: case GlTargetFaction: case GlIsCharacter: case GlHasPackage: case GlTargetRace:
	case GlNoTargetRace: case GlMyRace: case GlTargetCarrying: case GlTargetHasItem: case GlTargetHasItemType:
		return true;
	default: // my subrace: the RACE data has no subrace field (DIALOGUE_CONDITIONS_STATIC 1.3 step 9)
		return false;
	}
}

DialogueSystem::DialogueSystem(std::shared_ptr<const DialogueBook> book, std::uint64_t seed)
	: book_(std::move(book))
	, seed_(seed)
	, lineKeys_(book_->lineCount(), INT_MIN)
{
}

void DialogueSystem::push(DialogueEvent event)
{
	event.seq = seq_++;
	if (event.event < kDialogueEventCount)
		++eventCounts_[event.event];
	queue_.push_back(event);
}

DialogueInputs DialogueSystem::inputs(std::uint64_t id) const
{
	const auto found = inputs_.find(id);
	return found != inputs_.end() ? found->second : DialogueInputs{};
}

bool DialogueSystem::due(std::uint64_t fireKey, std::uint64_t tick, std::uint64_t interval)
{
	const auto [at, inserted] = fired_.try_emplace(fireKey, tick);
	if (inserted)
		return true;
	if (tick < at->second + interval)
		return false;
	at->second = tick;
	return true;
}

bool DialogueSystem::memory(std::uint64_t speaker, std::uint64_t target, std::uint32_t& tags) const
{
	const auto found = memory_.find(key(speaker, target));
	if (found == memory_.end())
		return false;
	tags = found->second;
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Score (DIALOGUE_SELECTION_STATIC Q1.4)
// ---------------------------------------------------------------------------------------------------------------------

int DialogueSystem::score(const Gate& gate, std::uint8_t speaker, bool interjection, bool locked) const
{
	int s = gate.scoreBonus;
	if (speaker != 0)
		s += 1;
	if (speaker >= 3 && speaker <= 6)
		s += 1;
	if (locked)
		s += 2;
	if (interjection)
		s += 2;
	s += static_cast<int>(gate.conds.count);
	for (const GateList list : { GlInTownOf, GlMyFaction, GlTargetFaction, GlTargetRace, GlTargetHasItemType })
	{
		if (gate.lists[list].count != 0)
			s += 2;
	}
	s += 2 * static_cast<int>(gate.lists[GlTargetHasItem].count) + 2 * static_cast<int>(gate.worldState.count);
	// The folded tag entries: +1 each as a condition entry, +1 more when required. The masks keep one bit per distinct tag, so a
	// tag listed twice counts once (UNSURE).
	s += 2 * popcount(gate.reqPersonality) + popcount(gate.forbPersonality) + 2 * popcount(gate.reqMemory) + popcount(gate.forbMemory);
	if (gate.chancePerm < 99.0f)
		s += 1;
	if (gate.targetType > 0)
		s += 1;
	return s;
}

int DialogueSystem::lineKey(std::uint32_t index)
{
	int& memo = lineKeys_[index];
	if (memo != INT_MIN)
		return memo == INT_MAX ? 0 : memo; // INT_MAX: a cycle, contributes nothing
	memo = INT_MAX;
	const LineDef& line = book_->line(index);
	int best = 0;
	for (const ChildRef& child : book_->children(line.children))
		best = std::max(best, lineKey(child.line));
	memo = score(line.gate, line.speaker, line.interjection, line.gate.locked) + best;
	return memo;
}

int DialogueSystem::rootKey(std::uint32_t dialogue)
{
	const DialogueDef& root = book_->dialogue(dialogue);
	int best = 0;
	for (const ChildRef& child : book_->children(root.firstLines))
		best = std::max(best, lineKey(child.line));
	return score(root.gate, 0, false, root.gate.locked) + best;
}

bool DialogueSystem::gateEvaluable(const Gate& gate) const
{
	for (const CondDef& cond : book_->conds(gate.conds))
	{
		if (cond.op != CondOp::Pass && !dialogueCondImplemented(cond.kind))
			return false;
	}
	for (int list = 0; list < GlCount; ++list)
	{
		if (gate.lists[list].count != 0 && !dialogueListImplemented(list))
			return false;
	}
	return true; // world states are live (worldstate::stateHolds); an unknown record fails closed at the gate
}

// ---------------------------------------------------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------------------------------------------------

struct DialogueSystem::Selection
{
	DialogueSystem& sys;
	const DialogueBook& book;
	const DialogueWorld& world;
	const DialogueEvent& event;
	const DialogueActor& speaker;
	const DialogueActor* target; // the event's target, null = none
	const PackageSet& set;

	struct Entry
	{
		std::uint32_t dialogue;
		std::uint32_t line;
		std::uint64_t voice;
	};
	std::vector<Entry> pool;
	std::deque<DialogueActor> retargeted; // the actors a retarget points `t` at (stable addresses)
	int best = INT_MIN;
	bool forced = false; // the forced root (DialogueSystem::force): its chance rolls pass
	const char* rejected = nullptr; // the gate step that refused last, and its condition (the forced root's diagnostics)
	const CondDef* rejectedCond = nullptr;
	// This selection is an interjection node's subtree (task CA, DIALOGUE.md 3.3 Q10): `speaker` is the interjector, so the
	// T_INTERJECTOR1..3 slots are that mate's own lines ("speak its child as that mate").
	bool interjector = false;

	bool reject(const char* step, const CondDef* cond = nullptr)
	{
		rejected = step;
		rejectedCond = cond;
		return false;
	}

	DialogueCoverage& cov() { return sys.coverage_; }

	double relation(const DialogueActor& from, const DialogueActor& to) const
	{
		if (from.id == to.id)
			return 100.0;
		return world.relation(from.faction, to.faction);
	}

	bool sameFaction(const DialogueActor& a, const DialogueActor& b) const { return !a.faction.empty() && a.faction == b.faction; }

	// A retarget (the lists and DC_IS_FEMALE / DC_HAS_ILLEGAL_ITEM write `t`): the actor `id`, held in the deque so the pointer
	// stays valid for the rest of the gate. nullptr when it is gone.
	const DialogueActor* retarget(std::uint64_t id)
	{
		if (target != nullptr && id == target->id)
			return target;
		if (id == speaker.id)
			return &speaker;
		for (const DialogueActor& held : retargeted)
		{
			if (held.id == id)
				return &held;
		}
		DialogueActor fresh;
		if (!world.actor(id, fresh) || !fresh.exists)
			return nullptr;
		sys.applyInputs(fresh);
		retargeted.push_back(std::move(fresh));
		return &retargeted.back();
	}

	static bool compare(CondOp op, int actual, int value)
	{
		switch (op)
		{
		case CondOp::Pass: return true;
		case CondOp::Bool: return (actual != 0) == (value > 0);
		case CondOp::Eq: return actual == value;
		case CondOp::Lt: return actual < value;
		case CondOp::Gt: return actual > value;
		case CondOp::Ge: return actual >= value;
		}
		return false;
	}

	// One condition of the DC list. `t` is the gate's current target (IS_FEMALE moves it).
	bool condition(const CondDef& cond, const DialogueActor*& t)
	{
		if (cond.op == CondOp::Pass)
			return true;
		const int kind = cond.kind;
		if (kind == 76) // TARGET_CHARACTER_EXISTS sees a missing target (before the fallback)
		{
			++cov().condEvaluated[kind];
			return compare(cond.op, (cond.onTarget ? t != nullptr : true) ? 1 : 0, cond.value);
		}
		if (!dialogueCondImplemented(kind))
		{
			++cov().condFailClosed[static_cast<std::size_t>(kind)];
			return false;
		}
		const DialogueActor& s = speaker;
		const DialogueActor* w = cond.onTarget ? t : &s;
		if (w == nullptr)
		{
			++cov().noTargetFails; // UNSURE: the exe falls back to the nearest player character
			return false;
		}
		++cov().condEvaluated[kind];
		int actual = 0;
		switch (kind)
		{
		case dc::Relations: actual = static_cast<int>(relation(s, *w)); break;
		case dc::PlayerMoney: // the player context's wallet (the exe ignores `who`: it is never the speaker's money)
		{
			DialoguePlayerContext player;
			actual = world.playerContext(target != nullptr ? target->id : 0, player)
				? static_cast<int>(std::clamp<std::int64_t>(player.money, INT_MIN, INT_MAX)) : 0;
			break;
		}
		case dc::IsRecruitable: // the context's roster against the global cap, and the classes the exe refuses (DIALOGUE_CONDITIONS_STATIC 77);
			// UNSURE: the exe's stats test (three maximum stat pairs, averaged >= 25) is not modelled.
		{
			DialoguePlayerContext player;
			const int cls = w->npcClass;
			actual = world.playerContext(target != nullptr ? target->id : 0, player) && player.roster < player.cap
				&& cls != crime::kClassLaw && cls != crime::kClassMilitary && cls != crime::kClassTrader && cls != crime::kClassDiplomat;
			break;
		}
		case dc::IsATrader: actual = w->trader; break;
		case 4: // CARRYING_BOUNTY_ALIVE / 5 _DEAD (DIALOGUE_CONDITIONS_STATIC 4, 5): the person W carries, alive (4) or dead (5), with a
		case 5: // bounty for S's faction, or a diplomat of a faction S's is hostile to. The bounty key is the speaker's faction here
			// (the sim's legal-system faction is the faction itself, crime.cpp).
		{
			DialogueActor carried;
			const bool have = w->carries != 0 && world.actor(w->carries, carried) && carried.exists;
			const bool wanted = kind == 4 ? have && !carried.dead : have && carried.dead;
			actual = wanted && (world.bounty(carried.id, s.faction) > 0
				|| (carried.npcClass == crime::kClassDiplomat && world.relation(s.faction, carried.faction) <= crime::kHostileRelation));
			break;
		}
		case 11: actual = s.houseLocked; break; // BUILDING_IS_CLOSED_AND_SECURED: the speaker's squad AI package +0xe8 "house locked" (who ignored)
		case dc::IsImprisoned: actual = w->imprisoned; break; // IS_IMPRISONED: location state 2, a cage (char+0x2f8 == 2)
		case 7: // IMPRISONED_BY_TARGET / 8 _OTHER (DIALOGUE_CONDITIONS_STATIC 7, 8): W in a cage whose building's faction is (7) or is
		case 8: // not (8) S's — or, when W is S, the dialogue target's (these two are not among the who-redirected ids). The sim's
			// holder is the faction that caged it (Fighter::captorSid, else the cage's town); an unknown holder fails "by target"
			// and leaves "by other" undecided (false, the exe's own answer for a building it cannot find, UNSURE).
		{
			const DialogueActor& holder = w->id == s.id && t != nullptr ? *t : s;
			const bool same = !holder.faction.empty() && w->imprisonedBy == holder.faction;
			actual = w->imprisoned && !w->imprisonedBy.empty() && (kind == dc::ImprisonedByTarget ? same : !same);
			break;
		}
		case 16: actual = w->building == s.building && (w->x - s.x) * (w->x - s.x) + (w->z - s.z) * (w->z - s.z) < kTalkingRangeSq; break;
		case 17:
		{
			const std::uint64_t mine = s.homeBuilding != 0 ? s.homeBuilding : s.building;
			actual = mine != 0 && w->building == mine;
			break;
		}
		case dc::TargetLastSeenXHoursAgo: // (uint) of the game hours since S last saw W; no memory entry reads as never (9999.99)
		{
			int hours = 0;
			actual = world.metHours(s.id, w->id, hours) ? hours : 9999;
			break;
		}
		case dc::IsLeader: actual = w->leader; break;
		case dc::MetTargetBefore: // S's memory has an entry for W (one per character here; the exe's is per squad, UNSURE)
		{
			int hours = 0;
			actual = world.metHours(s.id, w->id, hours) ? 1 : 0;
			break;
		}
		case 21: actual = s.stat > kStrongerRatio * w->stat; break; // WEAKER_THAN_ME
		case 22: actual = w->stat > kStrongerRatio * s.stat; break; // STRONGER_THAN_ME
		case 24: actual = w->id == s.id || sameFaction(s, *w) || relation(s, *w) >= kAllyRelation; break;
		case 25: actual = w->id != s.id && !sameFaction(s, *w) && relation(s, *w) <= kEnemyRelation; break;
		case 27: actual = w->brokenLeg; break;
		case 28: actual = w->brokenArm; break;
		case 29: actual = w->damagedHead; break;
		case 30: actual = w->nearlyKO; break; // NEARLY_KO: the medical min over the organic parts (DIALOGUE_CONDITIONS_LIVE (d))
		case 31: actual = w->inNonPlayerTown; break;
		case 32: actual = w->id == s.id ? 1 : w->running; break; // for W == S the exe's answer is `value > 0`
		case 33: actual = world.copsAround(s.id); break;
		case dc::SquadSize: actual = w->squadSize; break;
		case 36: actual = w->playerFaction; break;
		case dc::NumBackpacks: actual = w->backpacks; break; // NUM_BACKPACKS: CONTAINER bags worn in the BACKPACK slot, squad count
		case 38: actual = w->squadOnlyAnimals; break;
		case 39:
		{
			int enemies = 0, allies = 0;
			world.threat(w->id, enemies, allies);
			actual = static_cast<double>(enemies) > kOutnumberedRatio * static_cast<double>(allies);
			break;
		}
		case 41: actual = w->ko || w->dead || w->carried || w->down; break;
		case 42: actual = w->nearlyKO; break; // IS_NEARLY_KO: the same reading as 30, against the target (or the speaker)
		case 43: actual = w->squadAble < (w->squadSize <= 2 ? 1 : 2); break;
		case 44: actual = w->dead; break;
		case 46: // CARRYING_SOMEONE_TO_ENSLAVE: the person in W's hands, alive and of a class the slave trade takes (OT_CIVILIAN 4,
			// OT_SLAVE 6, OT_BANDIT 8, OT_ADVENTURER 9); value 0 always false. UNSURE: the exe's ignore list (1405a0f10) is not modelled.
		{
			DialogueActor carried;
			const bool have = cond.value != 0 && w->carries != 0 && world.actor(w->carries, carried) && carried.exists;
			actual = have && !carried.dead
				&& (carried.npcClass == 4 || carried.npcClass == crime::kClassSlave || carried.npcClass == crime::kClassBandit
					|| carried.npcClass == crime::kClassAdventurer);
			break;
		}
		case 47: actual = static_cast<int>(world.bounty(w->id, s.faction)); break; // BOUNTY_AMOUNT_ACTUAL: S's faction's bounty on W
		case dc::IsFemale:
			actual = w->female;
			if (actual != 0)
				t = w; // a success retargets the dialogue to W (140671930); local to this gate evaluation (UNSURE)
			break;
		case 48: actual = w->unarmed; break;
		case 52:
		case 53:
		{
			std::uint32_t tags = 0;
			actual = sys.memory(s.id, w->id, tags) && (tags & (kind == 52 ? kLoveTags : kHateTags)) != 0;
			break;
		}
		case 56: actual = w->inCombat; break;
		case dc::WithinTownWalls: actual = w->inWalls; break; // WITHIN_TOWN_WALLS: the gate-region walls (perception.hpp), UNSURE
		case 59: actual = w->slaveState == 1 || w->slaveState == 2; break; // IS_SLAVE: SLAVE or ESCAPING (1405c8aa0)
		case 61: actual = !w->captor.empty() && w->captor == s.faction; break; // TARGET_IS_SLAVE_OF_MY_FACTION: W's captor is S's faction
		case 62: actual = w->slaveState > 1; break; // IS_ESCAPED_SLAVE: ESCAPING or EX-SLAVE
		case 63: actual = w->cageLocked; break; // IS_IN_LOCKED_CAGE (OURS: the warden squad's one flag, not a cage's own lock)
		case 64: actual = w->shackles; break; // WEARING_LOCKED_SHACKLES: a worn ARMOUR piece marked `is locked`
		case 65: actual = !s.race.empty() && s.race == w->race; break;
		case 67: actual = w->sneaking && !w->inCombat; break;
		case 68: actual = w->building != 0; break;
		case dc::HasIllegalItem: // HAS_ILLEGAL_ITEM: W or a squad member S knows holds an item the law faction forbids; a hit
			// retargets the dialogue to that member (DIALOGUE_CONDITIONS_STATIC 69)
		{
			std::uint64_t member = 0;
			actual = world.illegalItem(s.id, w->id, member) ? 1 : 0;
			if (actual != 0 && member != w->id)
			{
				if (const DialogueActor* hit = retarget(member))
					t = hit;
			}
			break;
		}
		case 71: actual = w->starving; break;
		case dc::InAPlayerTown: actual = w->playerTown; break; // IN_A_PLAYER_TOWN: a town of a player faction holds W (PlayerTown too)
		case dc::TownLevelCurrentLocation: // the speaker's own town (S, not W), a player's; false with none (DIALOGUE_CONDITIONS_STATIC 73)
			if (!world.currentTownLevel(s.id, actual))
				return false;
			break;
		case dc::PlayersBestTownLevel: // the highest level over the player context's towns, -1 with none (74)
			if (!world.bestTownLevel(target != nullptr ? target->id : 0, actual))
				return false;
			break;
		case dc::HasRobotLimbs: actual = w->robotLimbs; break;
		default: break;
		}
		return compare(cond.op, actual, cond.value);
	}

	std::uint32_t idOf(const std::string& sid) const { return sid.empty() ? kNoIndex : book.idOf(sid); }

	bool inList(Span list, const std::string& sid) const
	{
		const std::uint32_t id = idOf(sid);
		return id != kNoIndex && has(book.ids(list), id);
	}

	// The exe's one gate (DIALOGUE_CONDITIONS_STATIC 1.3) for a root or a line. `record` is the record's interned sid, the key of
	// the rolls.
	bool gate(const Gate& g, std::uint32_t record)
	{
		const DialogueActor* t = target;
		// 1. personality and memory-tag masks
		if (g.reqPersonality != 0 || g.forbPersonality != 0)
		{
			const std::uint32_t bit = speaker.personality >= 0 && speaker.personality < 32 ? 1u << speaker.personality : 0;
			if (g.reqPersonality != 0 && (g.reqPersonality & bit) == 0)
				return reject("personality");
			if ((g.forbPersonality & bit) != 0)
				return reject("personality");
		}
		if ((g.reqMemory != 0 || g.forbMemory != 0) && t != nullptr)
		{
			std::uint32_t tags = 0;
			if (!sys.memory(speaker.id, t->id, tags))
			{
				if (g.reqMemory != 0)
					return reject("memory");
			} else if ((tags & g.reqMemory) != g.reqMemory || (tags & g.forbMemory) != 0) {
				return reject("memory");
			}
		}
		// 2. one at a time: needs the squad's sessions (step 5), passes
		if (g.oneAtATime)
			++cov().oneAtATimeIgnored;
		// 3. locked: the speaker's lock map entry, else the record's flag
		bool locked = g.locked;
		world.lockState(speaker.id, record, locked);
		if (locked)
		{
			++cov().lockedFails;
			return reject("locked");
		}
		// 5. chance temporary: a roll per gate evaluation, fails when above the chance
		if (g.chanceTemp < 99.0f && !forced)
		{
			const double roll = static_cast<double>(dialogueRoll(sys.seed_, speaker.id, event.tick, record, 1) % 10000) / 100.0;
			if (roll > static_cast<double>(g.chanceTemp))
				return reject("chance temporary");
		}
		// 6-9. the reference lists
		if (g.lists[GlInTownOf].count != 0 && !inList(g.lists[GlInTownOf], speaker.townFaction))
			return reject("in town of");
		if (g.lists[GlMyFaction].count != 0 && !inList(g.lists[GlMyFaction], speaker.faction))
			return reject("my faction");
		if (g.lists[GlTargetFaction].count != 0 && (t == nullptr || !inList(g.lists[GlTargetFaction], t->faction)))
			return reject("target faction");
		for (const WorldReq& req : book.worldStates(g.worldState))
		{
			bool holds = false;
			if (!world.worldState(req.state, holds))
			{
				++cov().worldStateFailClosed; // a state name the data does not define: undecidable
				return reject("world state");
			}
			if (holds != req.want)
				return reject("world state");
		}
		if (g.lists[GlIsCharacter].count != 0 && !inList(g.lists[GlIsCharacter], speaker.character))
			return reject("is character");
		// 7. the target's squad carries a person of the list; a hit on a member other than the target retargets the dialogue.
		if (g.lists[GlTargetCarrying].count != 0)
		{
			std::uint64_t member = 0;
			if (t == nullptr || !world.targetSquadCarries(t->id, speaker.id, book.ids(g.lists[GlTargetCarrying]), member))
				return reject("target carrying character");
			if (member != t->id)
			{
				if (const DialogueActor* hit = retarget(member))
					t = hit;
			}
		}
		// 8. the item type, then the item: the target's squad's inventories, retargeting to the holder.
		if (g.lists[GlTargetHasItemType].count != 0)
		{
			std::uint64_t member = 0;
			if (t == nullptr || !world.targetSquadHasItemType(t->id, book.ids(g.lists[GlTargetHasItemType]), member))
				return reject("target has item type");
			if (member != t->id)
			{
				if (const DialogueActor* hit = retarget(member))
					t = hit;
			}
		}
		if (g.lists[GlTargetHasItem].count != 0)
		{
			std::uint64_t member = 0;
			if (t == nullptr || !world.targetSquadHasItem(t->id, book.ids(g.lists[GlTargetHasItem]), member))
				return reject("target has item");
			if (member != t->id)
			{
				if (const DialogueActor* hit = retarget(member))
					t = hit;
			}
		}
		if (g.lists[GlMySubrace].count != 0)
		{
			++cov().listFailClosed[GlMySubrace];
			return reject("my subrace list");
		}
		if (const auto wanted = book.ids(g.lists[GlHasPackage]); !wanted.empty())
		{
			bool any = false;
			for (const std::uint32_t id : wanted)
				any = any || has(set.sources, id);
			if (!any)
				return reject("has package");
		}
		if (g.lists[GlTargetRace].count != 0 && (t == nullptr || !inList(g.lists[GlTargetRace], t->race)))
			return reject("target race"); // UNSURE: the exe also tries the target's squad mates and retargets
		if (g.lists[GlNoTargetRace].count != 0 && t != nullptr && inList(g.lists[GlNoTargetRace], t->race))
			return reject("no target race");
		if (g.lists[GlMyRace].count != 0 && !inList(g.lists[GlMyRace], speaker.race))
			return reject("my race");
		// 10. target type
		if (g.targetType > 0)
		{
			if (t == nullptr || t->npcClass != g.targetType)
				return reject("target type"); // OT_SLAVE would also take slave states 1 and 2: the sim has none
		}
		// 11. repeat delay: within the limit since the record was last shown (global per record, DIALOGUE_SELECTION_LIVE Q4)
		if (g.repetition != 0)
		{
			std::uint64_t shown = 0;
			const double hours = repetitionHours(g.repetition);
			if (hours > 0.0 && world.lastShown(record, shown)
				&& static_cast<double>(event.tick - std::min(shown, event.tick)) < hours * static_cast<double>(kTicksPerGameHour))
			{
				++cov().repetitionFails;
				return reject("repetition");
			}
		}
		// 12. chance permanent: rolled once per (speaker, record): a hash, so it is stable and needs no cache
		if (g.chancePerm < 99.0f && !forced)
		{
			const double roll = static_cast<double>(dialogueRoll(sys.seed_, speaker.id, 0, record, 2) % 10000) / 100.0;
			if (!(roll < static_cast<double>(g.chancePerm)))
				return reject("chance permanent");
		}
		// 13. the condition list, data order, short circuit; a retarget stays for the rest of the list
		for (const CondDef& cond : book.conds(g.conds))
		{
			if (!condition(cond, t))
				return reject("condition", &cond);
		}
		return true;
	}

	int raceBonus(const Gate& g) const
	{
		return target != nullptr && g.lists[GlTargetRace].count != 0 && inList(g.lists[GlTargetRace], target->race) ? 2 : 0;
	}

	// An interjection node (DIALOGUE.md 3.3, Q10 PROVISIONAL): its own gate is the speaker's; its children are gated and spoken by the
	// interjector, a squad mate, and join the pool at their cumulative score. No interjector: the node is skipped.
	void interject(std::uint32_t dialogue, const LineDef& line, int own, int depth)
	{
		DialogueActor mate;
		const std::uint64_t mateId = world.interjector(speaker.id);
		if (mateId == 0 || !world.actor(mateId, mate) || !mate.exists)
		{
			++cov().interjectionsSkipped;
			return;
		}
		if (!gate(line.gate, line.sid))
			return;
		sys.applyInputs(mate);
		Selection sub{ sys, book, world, event, mate, target, set, {}, {}, INT_MIN };
		sub.interjector = true;
		sub.select(dialogue, book.children(line.children), own, depth + 1);
		if (sub.pool.empty())
			return;
		if (sub.best > best)
		{
			pool.clear();
			best = sub.best;
		}
		if (sub.best == best)
			pool.insert(pool.end(), sub.pool.begin(), sub.pool.end());
	}

	// The selector over a child list (14067a0c0): walks in score order, pools the passing plain lines, recurses into the
	// root-like ones; `base` is the score accumulated on the way down. UNSURE (the doc rates the pool composition M): keys are the
	// cumulative score of the chain, the break test and the pool use them alike.
	void select(std::uint32_t dialogue, std::span<const ChildRef> list, int base, int depth)
	{
		if (depth > kMaxDepth)
			return;
		std::vector<std::uint32_t> order;
		order.reserve(list.size());
		for (const ChildRef& child : list)
		{
			if (std::find(order.begin(), order.end(), child.line) == order.end())
				order.push_back(child.line);
		}
		std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return sys.lineKey(a) > sys.lineKey(b); });
		for (const std::uint32_t index : order)
		{
			const LineDef& line = book.line(index);
			const bool plain = line.text.count != 0 && !line.interjection;
			const int own = base + sys.score(line.gate, line.speaker, line.interjection, line.gate.locked) + raceBonus(line.gate);
			if (own < best && plain)
				break;
			if (line.interjection)
			{
				if (own < best)
					break;
				interject(dialogue, line, own, depth);
				continue;
			}
			// Who says it (140672bf0, DIALOGUE_SELECTION_STATIC Q8 / Q9): T_ME and T_WHOLE_SQUAD the speaker; T_TARGET and
			// T_TARGET_WITH_RACE the target, T_TARGET_IF_PLAYER only a player-faction one (the automatic path: a monologue's target answers
			// by itself, a player character too). T_INTERJECTOR1..3 are the interjector's own lines, and only inside an interjection node's
			// subtree, where `speaker` already is that mate (task CA, Q10). UNSURE: T_TARGET_WITH_RACE is the target itself, not the first
			// squad mate of that race; the target must be able to talk.
			std::uint64_t voice = speaker.id;
			if (line.speaker != 0 && line.speaker != 6)
			{
				const bool interjectorSlot = interjector && line.speaker >= 3 && line.speaker <= 5;
				const bool able = target != nullptr && !target->dead && !target->ko && !target->down && !target->carried;
				if (interjectorSlot)
				{
					voice = speaker.id;
				} else if (!able || line.speaker < 1 || (line.speaker > 2 && line.speaker != 7)
					|| (line.speaker == 2 && !target->playerFaction)) {
					++cov().replyLinesSkipped;
					continue;
				} else {
					voice = target->id;
				}
			}
			if (!gate(line.gate, line.sid))
				continue;
			if (plain)
			{
				if (own > best)
				{
					pool.clear();
					best = own;
				}
				if (own == best)
					pool.push_back({ dialogue, index, voice });
			} else {
				select(dialogue, book.children(line.children), own, depth + 1);
			}
		}
	}
};

const DialogueSystem::SpeakerCache& DialogueSystem::speakerCache(const DialogueActor& speaker)
{
	std::string signature = speaker.character + "|" + speaker.squadTemplate + "|" + speaker.faction + "|" + speaker.race + "|"
		+ (speaker.leader ? 'L' : 'm') + (speaker.animal ? 'A' : 'h') + (speaker.playerFaction ? 'P' : 'n') + (speaker.female ? 'F' : 'M')
		+ (speaker.robotLimbs ? 'R' : 'b') + (speaker.trader ? 'T' : 'c') + std::to_string(speaker.personality);
	SpeakerCache& cache = speakers_[speaker.id];
	if (cache.signature != signature || cache.signature.empty())
	{
		SpeakerKey key;
		key.character = speaker.character;
		key.squadTemplate = speaker.squadTemplate;
		key.faction = speaker.faction;
		key.leader = speaker.leader;
		key.animal = speaker.animal;
		key.playerFaction = speaker.playerFaction;
		cache.set = book_->packageSetFor(key);
		SpeakerFacts facts = book_->characterFacts(speaker.character);
		facts.character = speaker.character;
		facts.faction = speaker.faction;
		if (!speaker.race.empty())
			facts.race = speaker.race;
		facts.personality = speaker.personality;
		facts.female = speaker.female;
		facts.robotLimbs = speaker.robotLimbs;
		facts.trader = speaker.trader || facts.trader;
		facts.leader = speaker.leader;
		cache.pass = book_->staticPass(book_->packageSet(cache.set), facts);
		cache.signature = std::move(signature);
	}
	return cache;
}

DialoguePick DialogueSystem::evaluate(const DialogueWorld& world, const DialogueEvent& event)
{
	DialoguePick pick;
	pick.event = event;
	DialogueActor speaker;
	if (!world.actor(event.speaker, speaker) || !speaker.exists || speaker.dead || speaker.ko || speaker.down || speaker.carried
		|| speaker.playerFaction)
	{
		pick.result = DialoguePick::Result::SpeakerGone;
		return pick;
	}
	const bool forcedSpeaker = forced_ != kNoIndex && speaker.character == forcedCharacter_;
	const auto report = [&](const std::string& what)
	{
		if (forcedLog_)
			forcedLog_("forced " + std::string(book_->sidOf(book_->dialogue(forced_).sid)) + ": speaker " + std::to_string(event.speaker) + " target "
				+ std::to_string(event.target) + " " + dialogueEventName(event.event) + " tick " + std::to_string(event.tick) + ": " + what);
	};
	if (world.busy(event.speaker))
	{
		if (forcedSpeaker)
			report("busy (in a conversation)");
		pick.result = DialoguePick::Result::Busy;
		return pick;
	}
	applyInputs(speaker);
	DialogueActor target;
	bool hasTarget = event.target != 0 && world.actor(event.target, target) && target.exists;
	if (hasTarget)
		applyInputs(target);

	const SpeakerCache& cache = speakerCache(speaker);
	const PackageSet& set = book_->packageSet(cache.set);
	std::vector<std::uint32_t> candidates;
	bool forcedRooted = false;
	for (const std::uint32_t position : set.rootsFor(event.event))
	{
		forcedRooted = forcedRooted || set.roots[position] == forced_;
		if (cache.pass.test(position))
			candidates.push_back(set.roots[position]);
	}
	const bool forced = forcedSpeaker && std::find(candidates.begin(), candidates.end(), forced_) != candidates.end();
	if (forced)
		candidates = { forced_ };
	else if (forcedSpeaker && forcedRooted)
		report("the static pass excludes it (a speaker-fixed condition or an unimplemented kind)");
	pick.candidates = static_cast<std::uint32_t>(candidates.size());
	if (candidates.empty())
	{
		pick.result = DialoguePick::Result::NoCandidates;
		return pick;
	}
	// Ordered by score + best child score, descending, stable (1406ad9d0).
	std::stable_sort(candidates.begin(), candidates.end(), [&](std::uint32_t a, std::uint32_t b) { return rootKey(a) > rootKey(b); });

	Selection selection{ *this, *book_, world, event, speaker, hasTarget ? &target : nullptr, set, {}, {}, INT_MIN };
	selection.forced = forced;
	const auto why = [&]
	{
		std::string out = selection.rejected != nullptr ? selection.rejected : "?";
		if (const CondDef* cond = selection.rejectedCond)
			out += std::string(" ") + dialogueCondName(cond->kind) + " " + condOpName(cond->op) + " " + std::to_string(cond->value)
				+ (cond->onTarget ? " on the target" : " on the speaker");
		return out;
	};
	bool rootPassed = false;
	for (const std::uint32_t index : candidates)
	{
		const DialogueDef& root = book_->dialogue(index);
		if (!selection.gate(root.gate, root.sid))
		{
			if (forced)
				report("the root's gate: " + why());
			continue;
		}
		rootPassed = true;
		const int own = score(root.gate, 0, false, root.gate.locked) + selection.raceBonus(root.gate);
		selection.select(index, book_->children(root.firstLines), own, 0);
	}
	pick.passed = static_cast<std::uint32_t>(selection.pool.size());
	if (selection.pool.empty())
	{
		if (forced && rootPassed)
			report("no line survived (last refusal: " + why() + ")");
		pick.result = DialoguePick::Result::NoneSurvived;
		return pick;
	}
	if (forced)
		report("picked");
	const auto& chosen = selection.pool[selection.pool.size() == 1
		? 0 : dialogueRoll(seed_, event.speaker, event.tick, event.event, 3) % selection.pool.size()];
	pick.result = DialoguePick::Result::Picked;
	pick.dialogue = chosen.dialogue;
	pick.line = chosen.line;
	pick.voice = chosen.voice;
	return pick;
}

DialoguePick DialogueSystem::interrupt(const DialogueWorld& world, const DialogueEvent& event, std::uint32_t dialogue)
{
	DialoguePick pick;
	pick.event = event;
	DialogueActor speaker;
	if (!world.actor(event.speaker, speaker) || !speaker.exists || speaker.dead || speaker.ko || speaker.down || speaker.carried
		|| speaker.playerFaction)
	{
		pick.result = DialoguePick::Result::SpeakerGone;
		return pick;
	}
	applyInputs(speaker);
	DialogueActor target;
	const bool hasTarget = event.target != 0 && world.actor(event.target, target) && target.exists;
	if (hasTarget)
		applyInputs(target);
	const PackageSet& set = book_->packageSet(speakerCache(speaker).set);
	Selection selection{ *this, *book_, world, event, speaker, hasTarget ? &target : nullptr, set, {}, {}, INT_MIN };
	const DialogueDef& root = book_->dialogue(dialogue);
	pick.candidates = 1;
	if (selection.gate(root.gate, root.sid))
		selection.select(dialogue, book_->children(root.firstLines), score(root.gate, 0, false, root.gate.locked) + selection.raceBonus(root.gate), 0);
	pick.passed = static_cast<std::uint32_t>(selection.pool.size());
	if (selection.pool.empty())
	{
		pick.result = DialoguePick::Result::NoneSurvived;
		return pick;
	}
	const auto& chosen = selection.pool[selection.pool.size() == 1
		? 0 : dialogueRoll(seed_, event.speaker, event.tick, event.event, 3) % selection.pool.size()];
	pick.result = DialoguePick::Result::Picked;
	pick.dialogue = dialogue;
	pick.line = chosen.line;
	pick.voice = chosen.voice;
	return pick;
}

void DialogueSystem::applyInputs(DialogueActor& actor) const
{
	const auto found = inputs_.find(actor.id);
	if (found != inputs_.end())
	{
		actor.personality = found->second.personality;
		actor.female = found->second.female;
		actor.robotLimbs = found->second.robotLimbs;
	}
}

DialoguePick DialogueSystem::next(const DialogueWorld& world, std::uint64_t speakerId, std::uint64_t targetId, std::uint32_t dialogue,
	std::uint32_t line, std::uint64_t tick)
{
	DialoguePick pick;
	pick.event.speaker = speakerId;
	pick.event.target = targetId;
	pick.event.tick = tick;
	DialogueActor speaker;
	if (!world.actor(speakerId, speaker) || !speaker.exists || speaker.dead || speaker.ko || speaker.down || speaker.carried)
	{
		pick.result = DialoguePick::Result::SpeakerGone;
		return pick;
	}
	applyInputs(speaker);
	DialogueActor target;
	const bool hasTarget = targetId != 0 && world.actor(targetId, target) && target.exists;
	if (hasTarget)
		applyInputs(target);
	const PackageSet& set = book_->packageSet(speakerCache(speaker).set);
	Selection selection{ *this, *book_, world, pick.event, speaker, hasTarget ? &target : nullptr, set, {}, {}, INT_MIN };
	selection.select(dialogue, book_->children(book_->line(line).children), 0, 0);
	pick.passed = static_cast<std::uint32_t>(selection.pool.size());
	if (selection.pool.empty())
	{
		pick.result = DialoguePick::Result::NoneSurvived;
		return pick;
	}
	const auto& chosen = selection.pool[selection.pool.size() == 1 ? 0 : dialogueRoll(seed_, speakerId, tick, line, 4) % selection.pool.size()];
	pick.result = DialoguePick::Result::Picked;
	pick.dialogue = dialogue;
	pick.line = chosen.line;
	pick.voice = chosen.voice;
	return pick;
}

std::vector<std::uint32_t> DialogueSystem::menu(const DialogueWorld& world, std::uint64_t speakerId, std::uint64_t targetId,
	std::uint32_t line)
{
	std::vector<std::uint32_t> out;
	DialogueEvent event;
	event.speaker = speakerId;
	event.target = targetId;
	DialogueActor speaker;
	DialogueActor target;
	if (!world.actor(speakerId, speaker) || !speaker.exists || !world.actor(targetId, target) || !target.exists)
		return out;
	applyInputs(speaker);
	applyInputs(target);
	const PackageSet& set = book_->packageSet(speakerCache(speaker).set);
	Selection selection{ *this, *book_, world, event, speaker, &target, set, {}, {}, INT_MIN };
	std::vector<std::uint32_t> order;
	for (const ChildRef& child : book_->children(book_->line(line).children))
	{
		if (std::find(order.begin(), order.end(), child.line) == order.end())
			order.push_back(child.line);
	}
	std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return lineKey(a) > lineKey(b); });
	for (const std::uint32_t index : order)
	{
		const LineDef& child = book_->line(index);
		if (child.interjection || !selection.gate(child.gate, child.sid))
			continue;
		if (child.speaker == 0)
		{
			out.push_back(kNoIndex);
			break;
		}
		out.push_back(index);
	}
	return out;
}

double DialogueSystem::repetitionHours(int repetition) noexcept
{
	static constexpr double kHours[] = { 0.0, 2.0, 6.0, 48.0, 168.0, 5000.0, 0.33 };
	return repetition >= 0 && repetition < 7 ? kHours[repetition] : 0.0;
}

DialogueSystem::Speech DialogueSystem::speech(std::uint32_t index, std::uint64_t voice, std::uint64_t tick) const
{
	Speech out;
	const LineDef& line = book_->line(index);
	const auto texts = book_->texts(line.text);
	if (!texts.empty())
	{
		const std::string_view raw = book_->text(texts[texts.size() == 1 ? 0 : dialogueRoll(seed_, voice, tick, index, 5) % texts.size()]);
		// /TOKEN/ substitution: a WORD_SWAPS record named TOKEN gives the alternatives (its lines' texts)
		std::size_t at = 0;
		while (at < raw.size())
		{
			const std::size_t open = raw.find('/', at);
			const std::size_t close = open == std::string_view::npos ? std::string_view::npos : raw.find('/', open + 1);
			if (close == std::string_view::npos)
			{
				out.text.append(raw.substr(at));
				break;
			}
			const std::string_view token = raw.substr(open + 1, close - open - 1);
			const WordSwap* swap = nullptr;
			for (const WordSwap& candidate : book_->wordSwaps())
			{
				if (candidate.token == token)
				{
					swap = &candidate;
					break;
				}
			}
			std::vector<std::string_view> words;
			if (swap != nullptr)
			{
				for (const std::uint32_t swapLine : book_->ids(swap->lines))
				{
					for (const TextRef& ref : book_->texts(book_->line(swapLine).text))
						words.push_back(book_->text(ref));
				}
			}
			out.text.append(raw.substr(at, open - at));
			if (words.empty())
			{
				out.text.append(raw.substr(open, close - open + 1)); // not a swap (a name token the sim does not fill yet): as written
			} else {
				const std::uint64_t roll = dialogueRoll(seed_, voice, swap->persistent ? 0 : tick, static_cast<std::uint32_t>(swap - book_->wordSwaps().data()), 6);
				out.text.append(words[roll % words.size()]);
			}
			at = close + 1;
		}
	}
	// 14067fd80: dlg+0x230 = (length x 0.12 + 2.0) in 1/100 game hour (live: "..." 2.36 -> 0.0236 game h), not seconds.
	out.hours = (2.0 + 0.12 * static_cast<double>(out.text.size())) / 100.0;
	return out;
}

std::vector<DialoguePick> DialogueSystem::step(const DialogueWorld& world, std::uint64_t tick)
{
	std::vector<DialoguePick> picks;
	std::sort(queue_.begin(), queue_.end(), [](const DialogueEvent& a, const DialogueEvent& b)
	{
		if (a.tick != b.tick) return a.tick < b.tick;
		if (a.speaker != b.speaker) return a.speaker < b.speaker;
		if (a.event != b.event) return a.event < b.event;
		if (a.target != b.target) return a.target < b.target;
		return a.seq < b.seq;
	});
	std::vector<DialogueEvent> keep;
	std::size_t used = 0;
	for (const DialogueEvent& event : queue_)
	{
		if (tick > event.tick && tick - event.tick > kMaxAgeTicks)
		{
			++staleDropped_;
			DialoguePick stale;
			stale.event = event;
			stale.result = DialoguePick::Result::Stale;
			log_.push_back(stale);
			continue;
		}
		if (used >= kEvalsPerTick)
		{
			keep.push_back(event);
			continue;
		}
		++used;
		picks.push_back(evaluate(world, event));
		log_.push_back(picks.back());
	}
	queue_ = std::move(keep);
	while (log_.size() > kLogCap)
		log_.pop_front();
	return picks;
}

} // namespace ksim
