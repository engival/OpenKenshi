#include "ksim/crime.hpp"

#include "ksim/clock.hpp"
#include "ksim/factions.hpp"
#include "ksim/perception.hpp"
#include "ksim/placement.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace ksim::crime
{

// ---------------------------------------------------------------------------------------------------------------------
// Pure rules
// ---------------------------------------------------------------------------------------------------------------------

std::int32_t bountyAmount(std::uint8_t crime) noexcept
{
	switch (crime)
	{
	case kEnslaving:
	case kTrespassing:
		return 100;
	case kBurglary:
	case kFencing:
		return 1000;
	case kTheft:
	case kEscapingPrison:
	case kUniformTheft:
		return 2500;
	case kMurder:
	case kSlaveFreeing:
	case kSmuggling:
	case kTerrorism:
	case kKidnapping:
		return 5000;
	case kTreason:
		return 50000;
	default:
		return 500; // assault, looting, crop theft, and the default
	}
}

double expiryHours(std::int32_t amount) noexcept
{
	if (amount >= kNeverExpires)
		return -1.0;
	return static_cast<double>(amount) * kExpiryHoursPerUnit;
}

double sentenceHours(std::int32_t amount, double prisonTime) noexcept
{
	if (amount <= 0)
		return 16.0; // 140854950: bounty 0 -> 16 h (unreachable on the caging path)
	if (amount >= kForeverBelow)
		return kForeverHours;
	return std::floor(static_cast<double>(amount) * kSentenceHoursPerUnit * prisonTime);
}

int effectiveClass(std::uint8_t npcClass, bool playerControlled) noexcept
{
	return playerControlled ? kClassAdventurer : static_cast<int>(npcClass);
}

bool isLaw(int cls, bool inOwnTown) noexcept
{
	return cls == kClassLaw || (cls == kClassMilitary && inOwnTown);
}

std::int32_t wantedThreshold(int cls, bool inOwnTown) noexcept
{
	switch (cls)
	{
	case kClassLaw:
		return 1;
	case kClassMilitary:
		return inOwnTown ? 2000 : 4000;
	case kClassAdventurer:
		return 1000;
	case kClassSlave:
		return std::numeric_limits<std::int32_t>::max();
	default:
		return 10000;
	}
}

int witnessEvent(std::uint8_t crime, bool victimIsWitness) noexcept
{
	switch (crime)
	{
	case kEnslaving:
	case kKidnapping:
		return 0x3a; // EV_KIDNAPPING_MY_ALLY (the witness must be a friend of the victim; not checked here)
	case kBurglary:
	case kSlaveFreeing:
		return kEvWitnessThiefOrLockpick;
	case kTheft:
	case kFencing:
	case kLooting:
		return victimIsWitness ? kEvThiefCaughtStealingFromMe : kEvWitnessLootingAlly;
	case kTrespassing:
		return kEvIntruderFound;
	case kCropTheft:
		return 0x39; // EV_EATING_MY_CROPS
	case kUniformTheft:
		return 0x22; // EV_I_SEE_UNIFORM_IMPOSTER
	case kNone:
		return 0;
	default:
		return kEvWitnessGenericAssault; // 4, 5, 6, 8, 9, 12
	}
}

bool witnessBetrayal(std::uint8_t crime) noexcept
{
	switch (crime)
	{
	case kTheft:
	case kLooting:
	case kFencing: // caseD_3 falls through to caseD_4 (rwh138 1)
	case kMurder:
	case kAssault:
	case kTreason:
	case kSmuggling:
	case kTerrorism:
	case kEscapingPrison:
		return true;
	default: // 0, kidnapping / enslaving, burglary / slave freeing, trespass, crop and uniform theft have their own branches
		return false;
	}
}

const state::Bounty* findBounty(const state::CrimeRecord& record, std::string_view factionSid) noexcept
{
	for (const state::Bounty& bounty : record.bounties)
	{
		if (bounty.factionSid == factionSid)
			return &bounty;
	}
	return nullptr;
}

double stealChance(const StealInput& in) noexcept
{
	const double skill = in.thievery * 0.01;
	if (in.character)
	{
		if (in.disabled)
			return 1.0;
		double chance = 1.1 * skill;
		if (in.worn)
		{
			chance *= 0.75;
			if (in.itemIsContainer)
				chance *= 0.75;
		}
		return chance;
	}
	double reach = 0.5 + 0.5 * std::min(in.distance / 200.0, 1.0);
	if (in.ownPlatoon)
		reach *= 0.5;
	double chance = 0.3 + (2.25 - 0.3) * (skill * reach);
	if (in.posture3)
		chance *= 2.0;
	if (in.floorMismatch)
		chance *= 1.5;
	return chance;
}

std::string legalFaction(const kdata::GameData* data, std::string_view factionSid)
{
	const kdata::Record* record = data != nullptr && !factionSid.empty() ? data->find(factionSid) : nullptr;
	if (record == nullptr)
		return {};
	if (record->boolOr("offers bounties", false))
		return std::string(factionSid);
	const kdata::RefCategory* legal = record->findRefs("legal system");
	return legal != nullptr && !legal->entries.empty() ? legal->entries.front().targetID : std::string();
}

std::int32_t bountyOf(const state::CrimeRecord& record, std::string_view factionSid) noexcept
{
	const state::Bounty* found = findBounty(record, factionSid);
	return found != nullptr ? found->amount : 0;
}

namespace
{

state::Bounty& getOrCreate(state::CrimeRecord& record, std::string_view holder)
{
	auto at = std::lower_bound(record.bounties.begin(), record.bounties.end(), holder,
		[](const state::Bounty& bounty, std::string_view key) { return bounty.factionSid < key; });
	if (at == record.bounties.end() || at->factionSid != holder)
	{
		state::Bounty fresh;
		fresh.factionSid = std::string(holder);
		at = record.bounties.insert(at, std::move(fresh));
	}
	return *at;
}

} // namespace

bool setCrime(state::CrimeRecord& record, std::uint8_t crime, std::string_view victimFaction, std::uint64_t victimChar,
	bool diplomatInTerritory)
{
	const bool terrorism = crime == kAssault && diplomatInTerritory;
	if (terrorism)
		crime = kTerrorism;
	if (!terrorism && record.crime != kNone && record.crime != crime)
	{
		if (!(crime == kAssault && !record.committed))
		{
			record.timer = std::max(record.timer, kBumpTimer);
			return false;
		}
	}
	if (record.crime != crime)
		record.committed = false;
	record.crime = crime;
	record.victimFaction = std::string(victimFaction);
	record.victimChar = victimChar;
	record.timer = std::max(record.timer, kMinTimer);
	return true;
}

bool reportCrime(state::CrimeRecord& record, std::uint8_t crime, std::string_view victimFaction, std::uint64_t victimChar,
	double timerSeconds, bool diplomatInTerritory)
{
	if (!setCrime(record, crime, victimFaction, victimChar, diplomatInTerritory))
		return false;
	record.timer = timerSeconds;
	return true;
}

void refreshTimer(state::CrimeRecord& record, double seconds) noexcept
{
	record.timer = std::max(record.timer, seconds);
}

bool commit(state::CrimeRecord& record, std::string_view holder, int criminalClass, double nowHours)
{
	if (record.crime == kNone || record.committed || record.crime == kEnslaving)
		return false;
	if (criminalClass == kClassBandit || criminalClass == kClassDiplomat || criminalClass == kClassLaw)
		return false;
	state::Bounty& bounty = getOrCreate(record, holder);
	bounty.amount += bountyAmount(record.crime);
	bounty.crimes |= 1u << record.crime;
	bounty.stamp = nowHours;
	record.committed = true;
	return true;
}

void addFlat(state::CrimeRecord& record, std::string_view holder, std::int32_t amount, double nowHours)
{
	state::Bounty& bounty = getOrCreate(record, holder);
	bounty.amount += amount;
	bounty.stamp = nowHours;
	record.committed = true;
}

bool erase(state::CrimeRecord& record, std::string_view holder)
{
	for (auto at = record.bounties.begin(); at != record.bounties.end(); ++at)
	{
		if (at->factionSid == holder)
		{
			record.bounties.erase(at);
			return true;
		}
	}
	return false;
}

std::int32_t claimReward(state::CrimeRecord& record, std::string_view holder, bool dead) noexcept
{
	for (state::Bounty& bounty : record.bounties)
	{
		if (bounty.factionSid != holder)
			continue;
		if (bounty.claim)
			return 0;
		bounty.claim = true;
		return dead ? bounty.amount / 2 : bounty.amount;
	}
	return 0;
}

bool forgive(state::CrimeRecord& record, std::string_view holder) noexcept
{
	const bool had = erase(record, holder);
	record.crime = kNone;
	record.victimFaction.clear();
	record.victimChar = 0;
	record.timer = -1.0;
	record.committed = false;
	return had;
}

void startSentence(state::CrimeRecord& record, std::int32_t amount, double nowHours, double prisonTime)
{
	record.sentenceStart = nowHours;
	record.sentenceHours = sentenceHours(amount, prisonTime);
}

bool empty(const state::CrimeRecord& record) noexcept
{
	return record.crime == kNone && record.bounties.empty() && record.sentenceHours == 0.0;
}

bool tick(state::CrimeRecord& record, double dt, double nowHours, bool inCage)
{
	if (record.crime != kNone)
	{
		if (record.timer <= 0.0)
		{
			record.crime = kNone;
			record.victimFaction.clear();
			record.victimChar = 0;
			record.timer = 0.0;
		} else {
			record.timer -= dt;
		}
	}
	if (!inCage)
	{
		for (auto at = record.bounties.begin(); at != record.bounties.end(); ++at)
		{
			if (at->amount <= 0)
				continue;
			const double hours = expiryHours(at->amount);
			const double age = nowHours - at->stamp;
			if (age < 0.0 || (hours >= 0.0 && age > hours))
			{
				record.bounties.erase(at); // one expiry per tick
				break;
			}
		}
	}
	return empty(record);
}

} // namespace ksim::crime

// ---------------------------------------------------------------------------------------------------------------------
// The world step
// ---------------------------------------------------------------------------------------------------------------------

namespace ksim
{

namespace
{

constexpr std::uint64_t kCrimeStepTicks = static_cast<std::uint64_t>(crime::kStepSeconds / kTickSeconds + 0.5);

// Faction facts for one step: the FACTION record's flags and the relation of two factions by sid.
class FactionView
{
public:
	FactionView(const flecs::world& ecs, const kdata::GameData* data)
		: ecs_(ecs)
		, data_(data)
	{
	}

	bool offersBounties(const std::string& sid) const
	{
		const kdata::Record* record = find(sid);
		return record != nullptr && record->boolOr("offers bounties", false);
	}

	std::string legal(const std::string& sid) const { return crime::legalFaction(data_, sid); }

	// FACTION `not real`: crimes against it are ignored, and so are those against a `fundamental type` 8 (bandit) faction (setCrime
	// refuses +0x34 == 8; CARRY_KIDNAP_LIVE.md break 2, which read +0x34 as the data field).
	bool notReal(const std::string& sid) const
	{
		const kdata::Record* record = find(sid);
		return record != nullptr && (record->boolOr("not real", false) || record->intOr("fundamental type", 0) == 8);
	}

	// factionRelation by sid: 100 for the same faction, the live relation when both have a FactionState, else the record's
	// `default relation`; 0 without game data or a record.
	double relation(const std::string& from, const std::string& to)
	{
		if (from.empty() || to.empty())
			return 0.0;
		if (from == to)
			return 100.0;
		if (data_ == nullptr)
			return 0.0;
		build();
		const auto a = entities_.find(from);
		const auto b = entities_.find(to);
		if (a != entities_.end() && b != entities_.end())
			return factionRelation(*data_, flecs::entity(ecs_.c_ptr(), a->second), flecs::entity(ecs_.c_ptr(), b->second));
		const kdata::Record* record = find(from);
		return record != nullptr ? record->intOr("default relation", 0) : 0.0;
	}

	// FactionRelations vf4 (1406b2ea0): `from`'s relation toward `to` takes one step of `reason`. False when a side has no faction
	// entity, when they are the same faction, or for the player faction's own map (PlayerFactionRelations stubs vf4, like the
	// dialogue's DA_AFFECT_RELATIONS).
	bool applyReason(const std::string& from, const std::string& to, RelationReason reason, double amount)
	{
		if (data_ == nullptr || from.empty() || to.empty() || from == to)
			return false;
		build();
		const auto a = entities_.find(from);
		const auto b = entities_.find(to);
		if (a == entities_.end() || b == entities_.end())
			return false;
		const flecs::entity owner(ecs_.c_ptr(), a->second);
		if (owner.has<state::PlayerFaction>())
			return false;
		applyRelationReason(*data_, owner, flecs::entity(ecs_.c_ptr(), b->second), reason, amount);
		return true;
	}

	// Whether the point's CURRENT town is one the faction owns. DECODED by address: the exe's current town is Character vf+0x78 =
	// getTown (1405c89b0 / GOG 1405c8350, CharStatus+0x88), the circle test of towncircle.hpp (the Town radius vf+0x2a0, base by
	// type x mult; the nearest centre when circles overlap) - the same call as goal 34 and the dialogue's IN_A_NON_PLAYER_TOWN.
	// The crime functions using it read "in my own town"; FUN_140598fa0's `vf+0x78 == 0` (CRIME_STATIC 0xa4) is "has no town".
	bool inOwnTown(const std::string& sid, double x, double z)
	{
		if (data_ == nullptr || sid.empty())
			return false;
		build();
		const auto owner = entities_.find(sid);
		if (owner == entities_.end())
			return false;
		std::vector<towncircle::Circle> circles;
		std::vector<flecs::entity_t> towns;
		ecs_.each([&](const state::TownState& town)
		{
			circles.push_back(towncircle::of(*data_, town));
			towns.push_back(town.owner.id);
		});
		const int current = towncircle::at(circles, x, z);
		return current >= 0 && towns[static_cast<std::size_t>(current)] == owner->second;
	}

private:
	const kdata::Record* find(const std::string& sid) const
	{
		return data_ != nullptr && !sid.empty() ? data_->find(sid) : nullptr;
	}

	void build()
	{
		if (built_)
			return;
		built_ = true;
		ecs_.each([&](flecs::entity entity, const state::FactionState& faction) { entities_[faction.templateSid] = entity.id(); });
	}

	const flecs::world& ecs_;
	const kdata::GameData* data_;
	std::unordered_map<std::string, flecs::entity_t> entities_;
	bool built_ = false;
};

} // namespace

std::uint64_t Simulation::dialogueEventCount(int event) const
{
	const auto found = dialogueCounts_.find(event);
	return found != dialogueCounts_.end() ? found->second : 0;
}

double Simulation::factionRelationBetween(const std::string& from, const std::string& to)
{
	return FactionView(ecsMutable(), gameData_.get()).relation(from, to);
}

const state::CrimeRecord* Simulation::crimeRecord(std::uint64_t id) const
{
	if (combat_ == nullptr || combat_->find(id) == nullptr)
		return nullptr;
	return fighterEntity(id).get<state::CrimeRecord>();
}

std::int32_t Simulation::bountyOn(std::uint64_t id, const std::string& factionSid) const
{
	const state::CrimeRecord* record = crimeRecord(id);
	return record != nullptr ? crime::bountyOf(*record, factionSid) : 0;
}

void Simulation::fireDialogue(std::uint64_t speaker, std::uint64_t subject, int event, int reason)
{
	// The dialogue engine (docs/DIALOGUE.md): with enableDialogue the event is queued and stepDialogue says which root and line the
	// speaker would use (DIALOGUE.md 8 step 3). HOOK: starting the conversation and running the lines' actions (DA_ARREST_TARGET,
	// DA_CRIME_ALARM, DA_ATTACK_*) is step 5 on. An event is also a counter and an observer call.
	++dialogueCounts_[event];
	if (dialogue_ != nullptr)
	{
		DialogueEvent queued;
		queued.event = static_cast<std::uint8_t>(event);
		queued.speaker = speaker;
		queued.target = subject;
		queued.reason = static_cast<std::uint8_t>(reason);
		queued.tick = clockTick();
		dialogue_->push(queued);
	}
	if (dialogueObserver_)
	{
		crime::DialogueEvent e;
		e.tick = clockTick();
		e.speaker = speaker;
		e.subject = subject;
		e.event = event;
		dialogueObserver_(e);
	}
}

bool Simulation::diplomatInTerritory(std::uint64_t victimChar, const std::string& victimFaction)
{
	if (victimChar == 0 || gameData_ == nullptr || combat_ == nullptr)
		return false;
	const state::Fighter* victim = combat_->find(victimChar);
	if (victim == nullptr || victim->npcClass != crime::kClassDiplomat) // the raw unit-data class (state+0xdc), not the player-adjusted one
		return false;
	if (!crimeAreasTried_)
	{
		crimeAreasTried_ = true;
		try
		{
			crimeAreas_ = std::make_shared<AreaLookup>(world_.dataDir());
		} catch (const std::exception&) {
			// No areas map: no territory.
		}
	}
	if (crimeAreas_ == nullptr)
		return false;
	auto listed = factionBiomes_.find(victimFaction);
	if (listed == factionBiomes_.end())
	{
		std::vector<std::uint32_t> colours;
		if (const kdata::Record* record = gameData_->find(victimFaction))
			if (const kdata::RefCategory* biomes = record->findRefs("biomes"))
				for (const kdata::RefEntry& entry : biomes->entries)
					if (const kdata::Record* group = gameData_->find(entry.targetID))
						colours.push_back(static_cast<std::uint32_t>(group->intOr("index", 0)));
		std::sort(colours.begin(), colours.end());
		listed = factionBiomes_.emplace(victimFaction, std::move(colours)).first;
	}
	return std::binary_search(listed->second.begin(), listed->second.end(), crimeAreas_->colourAt(victim->x, victim->z));
}

bool Simulation::reportCrime(std::uint64_t who, std::uint8_t crimeId, const std::string& victimFaction, std::uint64_t victimChar,
	double timerSeconds)
{
	if (combat_ == nullptr)
		return false;
	const state::Fighter* f = combat_->find(who);
	if (f == nullptr || f->dead || f->unconscious) // reportCrime is ignored if the owner is dead or KO'd
		return false;
	flecs::entity entity = fighterEntity(who);
	const state::Allegiance* own = entity.get<state::Allegiance>();
	const std::string ownFaction = own != nullptr ? own->factionSid : std::string();
	if (victimFaction.empty() || victimFaction == ownFaction)
		return false;
	FactionView factions(world_.ecs(), gameData_.get());
	if (factions.notReal(victimFaction))
		return false;
	// A law character cannot be a criminal (setCrime refuses it).
	if (crime::isLaw(crime::effectiveClass(f->npcClass, f->playerControlled), factions.inOwnTown(ownFaction, f->x, f->z)))
		return false;
	// CRIME_STATIC.md setCrime also refuses a criminal in location state 2 (a cage occupant: CRIME_NOTICE C29, which is why the lift's
	// ESCAPE_PRISON on the carried never lands) and a victim faction the criminal is disguised as (no disguise yet).
	if (f->cageBuilding != 0)
		return false;
	state::CrimeRecord record;
	if (const state::CrimeRecord* held = entity.get<state::CrimeRecord>())
		record = *held;
	const bool terror = crimeId == crime::kAssault && diplomatInTerritory(victimChar, victimFaction);
	const bool accepted = timerSeconds > 0.0 ? crime::reportCrime(record, crimeId, victimFaction, victimChar, timerSeconds, terror)
	                                         : crime::setCrime(record, crimeId, victimFaction, victimChar, terror);
	if (!crime::empty(record))
		entity.set<state::CrimeRecord>(std::move(record));
	return accepted;
}

void Simulation::addBounty(std::uint64_t id, const std::string& factionSid, std::int32_t amount)
{
	if (combat_ == nullptr || combat_->find(id) == nullptr)
		return;
	flecs::entity entity = fighterEntity(id);
	state::CrimeRecord record;
	if (const state::CrimeRecord* held = entity.get<state::CrimeRecord>())
		record = *held;
	crime::addFlat(record, factionSid, amount, gameHoursIn(clockTick()));
	entity.set<state::CrimeRecord>(std::move(record));
}

bool Simulation::clearBounty(std::uint64_t id, const std::string& factionSid)
{
	const state::CrimeRecord* held = crimeRecord(id);
	if (held == nullptr)
		return false;
	state::CrimeRecord record = *held;
	const bool had = crime::erase(record, factionSid);
	flecs::entity entity = fighterEntity(id);
	if (crime::empty(record))
		entity.remove<state::CrimeRecord>();
	else
		entity.set<state::CrimeRecord>(std::move(record));
	return had;
}

std::int32_t Simulation::claimBounty(std::uint64_t id, const std::string& factionSid, bool dead)
{
	const state::CrimeRecord* held = crimeRecord(id);
	if (held == nullptr)
		return 0;
	state::CrimeRecord record = *held;
	const std::int32_t amount = crime::claimReward(record, factionSid, dead);
	fighterEntity(id).set<state::CrimeRecord>(std::move(record));
	return amount;
}

bool Simulation::forgiveCrime(std::uint64_t id, const std::string& factionSid)
{
	const state::CrimeRecord* held = crimeRecord(id);
	if (held == nullptr)
		return false;
	state::CrimeRecord record = *held;
	const bool had = crime::forgive(record, factionSid);
	flecs::entity entity = fighterEntity(id);
	if (crime::empty(record))
		entity.remove<state::CrimeRecord>();
	else
		entity.set<state::CrimeRecord>(std::move(record));
	return had;
}

void Simulation::reportTheft(std::uint64_t who, std::uint64_t container, const std::string& ownerSid)
{
	// Task_Pickup::vf2 (14034f340 L100): setCrime(3, the owner faction), no reportCrime, so the timer is only topped up to 3 s.
	// A 3 s theft needs a fresh sighting inside its 3 s to be witnessed (CRIME_LIVE.md b).
	// A body (a character's inventory) is the victim character as well as its faction: the witness rule below reads it.
	// From a character (inventory / trading "take from character", 140712490 L93): theft while the victim is up, looting (10) from a KO'd or dead one.
	const state::Fighter* body = combat_ != nullptr ? combat_->find(container) : nullptr;
	const bool victimUp = body != nullptr && !body->dead && !body->unconscious;
	reportCrime(who, body != nullptr && !victimUp ? crime::kLooting : crime::kTheft, ownerSid, body != nullptr ? container : 0);
	noteCaughtAct(who, 1, container);
	if (interactObserver_)
	{
		InteractEvent stolen;
		stolen.kind = InteractEvent::Kind::Theft;
		stolen.who = who;
		stolen.target = container;
		stolen.item = ownerSid;
		interactObserver_(stolen);
	}
}

double Simulation::stealChance(std::uint64_t who, std::uint64_t container, const std::string& item) const
{
	const state::Fighter* thief = combat_ != nullptr ? combat_->find(who) : nullptr;
	const Storage storage = findStorage(container);
	if (thief == nullptr || storage.entity == 0 || !isForeignOwner(world_.ecs(), who, storage.ownerSid))
		return 1.0;
	crime::StealInput in;
	in.thievery = thief->skills.thievery;
	if (storage.body)
	{
		const state::Fighter* victim = combat_->find(container);
		in.character = true;
		in.disabled = victim == nullptr || victim->dead || victim->unconscious || victim->carriedBy != 0 || victim->cageBuilding != 0;
		if (const state::Inventory* held = flecs::entity(world_.ecs().c_ptr(), storage.entity).get<state::Inventory>())
		{
			if (held->carried(item) == 0)
			{
				for (const state::Inventory::Entry& entry : held->items)
				{
					if (entry.slot != state::Inventory::kCarried && entry.itemSid == item)
					{
						in.worn = true;
						in.itemIsContainer = !entry.contents.empty();
						break;
					}
				}
			}
		}
	} else {
		in.distance = std::hypot(storage.x - thief->x, storage.z - thief->z);
		// UNSURE (deferred: playtesting): the ownership test behind ownPlatoon and FUN_1405499a0, and the victim's posture / floor: ours has
		// no posture or floor model, so they stay false.
	}
	return crime::stealChance(in);
}

void Simulation::recordCaughtStealing(std::uint64_t thief, std::uint64_t victim, const std::string& ownerSid)
{
	reportCrime(thief, crime::kTheft, ownerSid, victim, crime::kTimerTheftReport);
	if (victim != 0)
		fireDialogue(victim, thief, crime::kEvThiefCaughtStealingFromMe);
}

bool Simulation::stealRollCaught(std::uint64_t who, std::uint64_t container, const std::string& item, const Storage& storage)
{
	const state::Fighter* thief = combat_ != nullptr ? combat_->find(who) : nullptr;
	if (thief == nullptr || !thief->playerControlled || !isForeignOwner(world_.ecs(), who, storage.ownerSid))
		return false;
	const double chance = stealChance(who, container, item);
	if (!crime::stealCaught(chance, rng_[static_cast<std::size_t>(Subsystem::FullDetail)].uniform01()))
		return false;
	std::uint64_t victim = storage.body ? container : 0;
	if (!storage.body)
	{
		// The container's owner notices: the nearest living character of the owner faction.
		double best = 1.0e18;
		world_.ecs().each([&](const state::Id& id, const state::Allegiance& side)
		{
			const state::Fighter* other = side.factionSid == storage.ownerSid ? combat_->find(id.value) : nullptr;
			if (other == nullptr || other->dead || other->unconscious)
				return;
			const double d = std::hypot(other->x - storage.x, other->z - storage.z);
			if (d < best)
			{
				best = d;
				victim = id.value;
			}
		});
	}
	recordCaughtStealing(who, victim, storage.ownerSid);
	return true;
}

// SIMLINK EVENT 51 bookkeeping: the first sighting of a theft / lockpick record names the act once (the record's life is the act's).
// The note is made on every tick of the crime (the lock tasks report each tick), so it only starts an entry when none is held and the
// character carries a pending crime of that kind (setCrime may have refused it).
void Simulation::noteCaughtAct(std::uint64_t who, std::uint8_t cause, std::uint64_t container)
{
	if (!crimeObserver_ || caughtActs_.count(who) != 0)
		return;
	const state::CrimeRecord* held = crimeRecord(who);
	if (held != nullptr && (held->crime == crime::kTheft || held->crime == crime::kLooting || held->crime == crime::kBurglary))
		caughtActs_[who] = {container, cause, false};
}

void Simulation::stepCrime(std::uint64_t now)
{
	if (combat_ == nullptr)
		return;
	// ---- the attacks of this tick (assault) ----
	// 140334070 (an attack starts; Task_RangedAttack / Task_FocusedMeleeAttack) and resolveIncomingHit 140665fa0 (a landed hit)
	// record Assault with a 30 s timer. Not set if the victim has a bounty for the attacker's faction, is a bandit (class 8), or
	// already has a pending crime (UNSURE: the static note does not say whose pending crime; the victim's is read).
	// UNSURE: for the hit path the static note says "when the defender is hit by an attacker (so retaliation is also recorded)";
	// which character owns the record is not stated. The attacker owns it here.
	for (const CombatArena::AttackNote& note : combat_->takeAttacks())
	{
		const state::Fighter* attacker = combat_->find(note.attacker);
		const state::Fighter* victim = combat_->find(note.victim);
		if (attacker == nullptr || victim == nullptr)
			continue;
		const state::Allegiance* attackerSide = fighterEntity(note.attacker).get<state::Allegiance>();
		const state::Allegiance* victimSide = fighterEntity(note.victim).get<state::Allegiance>();
		if (attackerSide == nullptr || victimSide == nullptr)
			continue;
		const int victimClass = crime::effectiveClass(victim->npcClass, victim->playerControlled);
		if (note.landed)
		{
			// The law branch of the hit-path writer (resolveIncomingHit 140665fa0 tail, CRIME_STATIC.md rwh138 4.2; rwh139 3): a landed hit
			// between a law and a non-law character charges the NON-law one with Assault (5) and the bounty with the law side's faction
			// (500, 140851e40), whatever its goal (15 PROTECT_ALLIES and 32 SELF_PRESERVATION included: a squadmate joining the guards'
			// fight is charged on its own first landed hit; one that never hits stays crime 0). No faction-relation write, no in-combat
			// gate. Law = crime::isLaw (law class, or military in its own town); the exe's own test is not decoded. Timer: the decoded
			// tail passes 30 s (rwh138); rwh139 read 20.0 s on a live record (UNSURE: it may have been aged or a different writer).
			// The same entry writes tag 4 (temp enemy, 1406744a0) for the attacker into the DEFENDER's platoon store, so the defender's
			// squad treats the hitter as hostile at once (rwh139: Engival hostile, Hamut who never hit neutral): markHostile below.
			FactionView hitFactions(ecsMutable(), gameData_.get());
			const bool attackerLaw = crime::isLaw(crime::effectiveClass(attacker->npcClass, attacker->playerControlled),
				hitFactions.inOwnTown(attackerSide->factionSid, attacker->x, attacker->z));
			const bool victimLaw = crime::isLaw(victimClass, hitFactions.inOwnTown(victimSide->factionSid, victim->x, victim->z));
			if (attackerLaw != victimLaw)
			{
				combat_->markHostile(note.victim, note.attacker);
				const std::uint64_t culprit = victimLaw ? note.attacker : note.victim;
				const std::string lawSide = victimLaw ? victimSide->factionSid : attackerSide->factionSid;
				if (reportCrime(culprit, crime::kAssault, lawSide, victimLaw ? note.victim : note.attacker, crime::kTimerAssault))
				{
					state::CrimeRecord record = *fighterEntity(culprit).get<state::CrimeRecord>();
					crime::commit(record, hitFactions.legal(lawSide), crime::effectiveClass(victimLaw ? attacker->npcClass : victim->npcClass,
						victimLaw ? attacker->playerControlled : victim->playerControlled), gameHoursIn(now));
					fighterEntity(culprit).set<state::CrimeRecord>(std::move(record));
				}
				continue;
			}
		}
		if (victimClass == crime::kClassBandit)
			continue;
		// OURS (k16.6.85): a defender answering its attacker (32 SELF_PRESERVATION, 143 PROTECT_OWN_SQUAD, 15 PROTECT_ALLIES) commits no assault; the
		// live original's bar crew never answered the victim squad's hits on the player (rwh.82), and the exe's own test for that is not decoded.
		if (attacker->combatGoal == routines::task::SelfPreservation || attacker->combatGoal == routines::task::ProtectOwnSquad
			|| attacker->combatGoal == routines::task::ProtectAllies)
			continue;
		const state::CrimeRecord* victimRecord = fighterEntity(note.victim).get<state::CrimeRecord>();
		if (victimRecord != nullptr && (victimRecord->crime != crime::kNone || crime::bountyOf(*victimRecord, attackerSide->factionSid) > 0))
			continue;
		reportCrime(note.attacker, crime::kAssault, victimSide->factionSid, note.victim, crime::kTimerAssault);
	}
	if (now % kCrimeStepTicks != 0)
		return;

	// ---- once a second ----
	flecs::world& ecs = ecsMutable();
	const double nowHours = gameHoursIn(now);
	FactionView factions(ecs, gameData_.get());
	const auto& members = combat_->members();

	struct Held
	{
		std::uint64_t id;
		flecs::entity_t entity;
	};
	const auto holders = [&]
	{
		std::vector<Held> found;
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::CrimeRecord&) { found.push_back({id.value, entity.id()}); });
		std::sort(found.begin(), found.end(), [](const Held& l, const Held& r) { return l.id < r.id; });
		return found;
	};
	const auto sideOf = [&](std::uint64_t id) -> std::string
	{
		const state::Allegiance* side = fighterEntity(id).get<state::Allegiance>();
		return side != nullptr ? side->factionSid : std::string();
	};
	// An observer that can witness: awake, not down, not carried.
	const auto usable = [](const state::Fighter& f) { return !f.dead && !f.unconscious && !f.down && !f.asleep && f.carriedBy == 0; };

	// 1. The record timers and the bounty expiry (1405cbb80, every character tick).
	for (const Held& held : holders())
	{
		flecs::entity entity(ecs.c_ptr(), held.entity);
		state::CrimeRecord record = *entity.get<state::CrimeRecord>();
		// A character in a cage does not expire (inCage, kenshi-k16.6.36). Expiry also erases the bounty of the faction that owns
		// the character's current town (1408539f0); there is no per-character town yet, so only the entry itself goes.
		const state::Fighter* holder = combat_ != nullptr ? combat_->find(held.id) : nullptr;
		if (crime::tick(record, crime::kStepSeconds, nowHours, holder != nullptr && holder->cageBuilding != 0))
			entity.remove<state::CrimeRecord>();
		else
			entity.set<state::CrimeRecord>(std::move(record));
	}
	for (auto act = caughtActs_.begin(); act != caughtActs_.end();) // the record is gone or no longer a theft: the act ended
	{
		const state::CrimeRecord* held = crimeRecord(act->first);
		if (held == nullptr || (held->crime != crime::kTheft && held->crime != crime::kLooting && held->crime != crime::kBurglary))
			act = caughtActs_.erase(act);
		else
			++act;
	}

	// 2. The trespass sensor (140852350, called from 140859680 for each seen character), the "house locked" case only
	// (TRESPASS_LIVE.md): the observer's squad home is a building and X stands in it, the squad flag is on (state::Fighter::
	// houseLocked), X is a player character and not friendly (G2) or hostile (gate 15) to the observer. Branch 2 (a law
	// observer and a foreign non-public building) and the shoo (EV 9, flag off: Get out of here -> Go -> I said get out -> alarm)
	// are TODO: the gating facts are not clear enough (TRESPASS_LIVE.md runs 5-9: what sets the flag is derived state, "we
	// closed the house up ourselves"). G4 (X+0x138/+0x140) and B1 (X is O's captive) are not modelled.
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& observer = members[i].fighter;
		if (!observer.houseLocked || observer.homeBuilding == 0 || !usable(observer))
			continue;
		const std::string observerSide = sideOf(members[i].id);
		for (const state::SightRecord& sighting : observer.sight)
		{
			if (!sighting.seen || sighting.sinceSeen >= crime::kStepSeconds || sighting.target == members[i].id)
				continue;
			const state::Fighter* intruder = combat_->find(sighting.target);
			if (intruder == nullptr || !intruder->playerControlled || intruder->building != observer.homeBuilding || !usable(*intruder))
				continue;
			const std::string intruderSide = sideOf(sighting.target);
			const double relation = factions.relation(observerSide, intruderSide);
			if (relation >= crime::kFriendlyRelation || relation <= crime::kHostileRelation)
				continue;
			const state::CrimeRecord* pending = fighterEntity(sighting.target).get<state::CrimeRecord>();
			if (pending != nullptr && pending->crime != crime::kNone)
				continue; // already pending: the sensor answers false (run 6)
			if (reportCrime(sighting.target, crime::kTrespassing, observerSide, members[i].id, crime::kTimerTrespassSensor))
			{
				fireDialogue(members[i].id, sighting.target, crime::kEvIntruderFound);
				// ST_INTRUDER tag on X in O's squad memory: no perception tags yet.
			}
		}
	}

	// 2b. The carry inspection (1408549f0, CRIME_NOTICE C21-C28, CARRY_KIDNAP_LIVE.md): an observer that sees a carrier hauling someone
	// writes a crime on the carrier. The exe's filters: the observer is not a friend of the carrier nor of its faction (C18, C20), has no
	// pending crime of its own, the carrier is no animal and not incapacitated, a carried non-slave needs the captive flag (C82) and
	// no bounty with the observer's legal faction (C24, C25), an enemy of the carried does nothing, a slave is the observer's captive
	// (C27: SLAVE_FREEING, victim the observer) or kidnapping only for a friend of the carried; setCrime refuses an unreal victim
	// faction (CRIME_STATIC) and, with reportCrime, the carrier's own (C28).
	// OUR FIX (ARCHITECTURE "Guards notice what you carry"; the prisoner flag decides, not the carrier's faction): a carried character
	// whose captor is the observer's faction (a held prisoner, slave or not) is SLAVE_FREEING against that faction whoever carries it
	// but the holder, which closes the same-faction hole (C28). Law observers do not write the stranger case: a stranger carried in
	// is free, a wanted one is allowed in (the carry-out check needs the town direction: not modelled, UNSURE).
	for (const auto& member : members)
	{
		const state::Fighter& carrier = member.fighter;
		const state::Fighter* carried = carrier.carrying != 0 ? combat_->find(carrier.carrying) : nullptr;
		if (carried == nullptr || carried->carriedBy != member.id || carrier.dead || carrier.unconscious || carrier.animal)
			continue;
		const std::string carrierSide = sideOf(member.id);
		const std::string carriedSide = sideOf(carrier.carrying);
		const state::CrimeRecord* carriedRecord = fighterEntity(carrier.carrying).get<state::CrimeRecord>();
		for (const auto& watcher : members)
		{
			const state::Fighter& observer = watcher.fighter;
			if (watcher.id == member.id || watcher.id == carrier.carrying || !usable(observer))
				continue;
			const state::SightRecord* sighting = perception::find(observer, member.id);
			if (sighting == nullptr || !sighting->seen || sighting->sinceSeen >= crime::kStepSeconds)
				continue;
			const std::string observerSide = sideOf(watcher.id);
			if (observerSide.empty() || observerSide == carrierSide || factions.relation(observerSide, carrierSide) >= crime::kFriendlyRelation)
				continue;
			if (const state::CrimeRecord* own = fighterEntity(watcher.id).get<state::CrimeRecord>(); own != nullptr && own->crime != crime::kNone)
				continue;
			const bool law = crime::isLaw(crime::effectiveClass(observer.npcClass, observer.playerControlled),
				factions.inOwnTown(observerSide, observer.x, observer.z));
			const double towardCarried = factions.relation(observerSide, carriedSide);
			if (carried->captorSid == observerSide)
			{
				reportCrime(member.id, crime::kSlaveFreeing, observerSide, carrier.carrying);
			} else if (carried->slaveState == 1 || carried->slaveState == 2) {
				if (towardCarried >= crime::kFriendlyRelation)
					reportCrime(member.id, crime::kKidnapping, carriedSide, carrier.carrying);
			} else if (carried->captive && !law && towardCarried > crime::kHostileRelation
				&& (carriedRecord == nullptr || crime::bountyOf(*carriedRecord, factions.legal(observerSide)) <= 0)) {
				reportCrime(member.id, crime::kKidnapping, carriedSide, carrier.carrying);
			}
		}
	}

	// 3. Witnesses (140854d10) and 4. the wanted reaction (140856f00).
	for (const Held& held : holders())
	{
		flecs::entity entity(ecs.c_ptr(), held.entity);
		const state::Fighter* criminal = combat_->find(held.id);
		if (criminal == nullptr)
			continue;
		state::CrimeRecord record = *entity.get<state::CrimeRecord>();
		const std::string criminalSide = sideOf(held.id);
		const int criminalClass = crime::effectiveClass(criminal->npcClass, criminal->playerControlled);
		const bool criminalLaw = crime::isLaw(criminalClass, factions.inOwnTown(criminalSide, criminal->x, criminal->z));
		bool changed = false;
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const std::uint64_t observerId = members[i].id;
			if (observerId == held.id)
				continue;
			const state::Fighter& observer = members[i].fighter;
			if (!usable(observer))
				continue;
			const state::SightRecord* sighting = perception::find(observer, held.id);
			if (sighting == nullptr || !sighting->seen)
				continue;
			const bool fresh = sighting->sinceSeen < crime::kStepSeconds;
			const std::string observerSide = sideOf(observerId);
			if (!observerSide.empty() && observerSide == criminalSide)
				continue; // same faction as the criminal
			const double fromCriminal = factions.relation(observerSide, criminalSide);

			// 4. Wanted reaction: a player character with a bounty held by the observer's faction, within 400 units. The exe
			// boosts the observer's detection first (player criminals, threshold met, not disguised: no disguise yet) and
			// then fires EV_BOUNTY_SPOTTED when a bounty of at least 1 exists for the legal faction; the threshold gate here is
			// the task's reading (UNSURE: the exe fires it for any bounty >= 1 once the boost applies).
			if (fresh && criminal->playerControlled && !observerSide.empty())
			{
				const std::int32_t held_ = crime::bountyOf(record, factions.legal(observerSide));
				if (held_ >= 1 && std::hypot(observer.x - criminal->x, observer.z - criminal->z) <= crime::kBountySpottedRange)
				{
					const int cls = crime::effectiveClass(observer.npcClass, observer.playerControlled);
					const bool townOf = cls == crime::kClassMilitary && factions.inOwnTown(observerSide, observer.x, observer.z);
					if (held_ >= crime::wantedThreshold(cls, townOf))
					{
						fireDialogue(observerId, held.id, crime::kEvBountySpotted);
						// HOOK: the attack / arrest is dialogue-defined (DA_ATTACK_CHASE_FOREVER, DA_ARREST_TARGET, DA_CRIME_ALARM).
					}
				}
			}

			// 3. Witness of the pending crime.
			if (record.crime == crime::kNone || criminalLaw || criminal->carriedBy != 0 || criminal->unconscious || criminal->dead)
				continue;
			if (!fresh && !(record.timer > crime::kWitnessAgain))
				continue; // runs on a fresh sighting, or while the timer is above 5 s
			const state::CrimeRecord* own = fighterEntity(observerId).get<state::CrimeRecord>();
			if (own != nullptr && own->crime != crime::kNone)
				continue; // an observer with its own pending crime does not witness
			const bool victimIsOwn = !observerSide.empty() && observerSide == record.victimFaction;
			if (fromCriminal >= crime::kFriendlyRelation && !victimIsOwn)
				continue; // a friend of the criminal, unless the victim is its own faction
			if (!record.victimFaction.empty() && factions.relation(observerSide, record.victimFaction) <= crime::kHostileRelation)
				continue; // ignores crimes against a faction it is hostile to
			// MINE/UNSURE (the exe's looting exemption is not decoded; CRIME_STATIC "10 Looting"): nor against a character it is
			// fighting, i.e. on another combat team. Hub guards that KO'd a Dust Bandit ignore the player looting him, though
			// the two factions sit at the default -10 both ways (the guards fight bandits by team, not by relation). Looting a
			// town citizen's body (the observer's own team) is still witnessed.
			if (record.victimChar != 0)
			{
				const state::Fighter* victim = combat_->find(record.victimChar);
				if (victim != nullptr && victim->team != observer.team)
					continue;
			}
			// The case-4 block (140854d10 L348-L368, rwh138 "Betrayal case", rwh148 row 2): a witness whose faction is friendly to
			// the victim's and that considers the criminal an ally betrays it - FactionRelations::vf4(observer, criminal, -20, 1),
			// DAT_1416b1860 x DAT_14167c308. It also drops the ally tag 3 and sets temp-enemy 4 on the criminal; no tags are modelled.
			// The next firing reads the written relation back, so 70 -> 50 -> 30 takes two firings while 60 -> 40 stops after one
			// (live: 60 -> 40 at 2.7 s, 70 -> 30; rwh148). "friendly"/"ally" here are the exe's relation >= 50 tests (1406b2630 /
			// 1407923d0, DAT_141683170); the same-faction and temp-ally-tag-3 cases of isFriendly are covered by the earlier skips.
			if (crime::witnessBetrayal(record.crime) && fromCriminal >= crime::kFriendlyRelation
				&& factions.relation(observerSide, record.victimFaction) >= crime::kFriendlyRelation)
				factions.applyReason(observerSide, criminalSide, RelationReason::Betrayal, 1.0);
			// The witness reacts: tag ST_CRIMINAL (1406744a0; no perception tags yet), refresh the timer to 20 s, fire the event.
			crime::refreshTimer(record);
			changed = true;
			if (auto act = caughtActs_.find(held.id); act != caughtActs_.end() && !act->second.sent && crimeObserver_)
			{
				act->second.sent = true;
				CrimeEvent caught;
				caught.kind = CrimeEvent::Kind::CaughtStealing;
				caught.who = held.id;
				caught.other = observerId;
				caught.cause = act->second.cause;
				caught.container = act->second.container;
				crimeObserver_(caught);
			}
			perception::notice(*combat_->find(observerId), held.id); // 140854d10 force-notices the criminal (the arrow's record)
			// HOOK: when the observer is the victim (record.victimChar == observerId) it retaliates (1405cb0a0).
			const int event = crime::witnessEvent(record.crime, record.victimChar == observerId);
			// EV_KIDNAPPING_MY_ALLY needs a friend of the victim (CRIME_STATIC 1 / 15, witnessEvent's note).
			const bool allyEvent = event == 0x3a && !victimIsOwn && factions.relation(observerSide, record.victimFaction) < crime::kFriendlyRelation;
			if (event != 0 && !allyEvent)
				fireDialogue(observerId, held.id, event, record.crime);
			// Only law enforcement, or military in its own town, of a faction that offers bounties commits the bounty on its own.
			// Everyone else gets one only through a dialogue effect (DA_ASSIGN_BOUNTY with `assigns bounties` speakers).
			const bool law = crime::isLaw(crime::effectiveClass(observer.npcClass, observer.playerControlled),
				factions.inOwnTown(observerSide, observer.x, observer.z));
			if (law && factions.offersBounties(observerSide))
				crime::commit(record, factions.legal(observerSide), criminalClass, nowHours);
		}
		if (changed)
			entity.set<state::CrimeRecord>(std::move(record));
	}
}

} // namespace ksim
