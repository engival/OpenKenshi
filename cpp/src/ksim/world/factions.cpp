#include "ksim/world/factions.hpp"

#include "world/factions_script.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/world/spawn.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace ksim {

namespace {

constexpr double kTicksPerHour = static_cast<double>(kTicksPerGameHour);
constexpr double kHoursPerDay = 24.0;

// Temperament decays by this factor each game-hour (half-lives of about a day).
constexpr double kAlarmDecay = 0.97;
constexpr double kGriefDecay = 0.98;
constexpr double kConfidenceDecay = 0.97;
// What an event adds (UNSURE: guesses).
constexpr double kSquadLostGrief = 0.12;
constexpr double kTownLostAlarm = 0.6;
constexpr double kTownLostGrief = 0.25;
constexpr double kTownGainedConfidence = 0.4;
// Utility numbers (UNSURE).
constexpr double kConsolidateBase = 0.35;
constexpr double kConsolidateConfidence = 0.5;
constexpr double kDefendBase = 0.2;
constexpr double kDefendAlarm = 1.3;
constexpr double kMourn = 1.2;
constexpr double kRaidScale = 0.9;
constexpr double kIntentBonus = 0.1;
constexpr double kIntentBonusHours = 6.0;
// Aggression = kAggressionBase + kAggressionSpread x (share of the towns'
// owners that are hostile to this faction).
constexpr double kAggressionBase = 0.4;
constexpr double kAggressionSpread = 0.6;
// A raid takes at most this many times the walk there and back, plus the stay
// and a day of slack, before the squads give up.
constexpr double kDeadlineSlack = 1.5;
constexpr double kDeadlineExtraHours = 24.0;
// Candidates tried in one step before it gives up for the hour.
constexpr int kTries = 3;
// A queued follow-up that could not start (no target, a campaign running) is
// dropped after this many game-hours past its due time (the exe retries a
// queued request about ten times in later 6-hour passes).
constexpr double kTriggerPatienceHours = 10.0 * kRollHours;

double unit(std::uint64_t bits)
{
	return static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
}

// FNV-1a: a stable hash of a stringID, the same on every platform.
std::uint64_t hashOf(std::string_view text)
{
	std::uint64_t hash = 0xcbf29ce484222325ULL;
	for (const char c : text)
		hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
	return hash;
}

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

double distance(double ax, double az, double bx, double bz)
{
	return std::hypot(ax - bx, az - bz);
}

// The record and its `inherits from` ancestors, nearest first (the order of
// tools/world_model.py campaign_ancestors: depth first, stack order).
std::vector<const kdata::Record*> lineage(const kdata::GameData& data,
	const kdata::Record& record, std::int32_t campaignType)
{
	std::vector<const kdata::Record*> out{&record};
	std::vector<const kdata::Record*> stack{&record};
	while (!stack.empty())
	{
		const kdata::Record* current = stack.back();
		stack.pop_back();
		forEachRef(*current, "inherits from", [&](const kdata::RefEntry& entry)
		{
			const kdata::Record* parent = data.find(entry.targetID);
			if (parent == nullptr || parent->type != campaignType
				|| std::find(out.begin(), out.end(), parent) != out.end())
				return;
			out.push_back(parent);
			stack.push_back(parent);
		});
	}
	return out;
}

void removeSquadEntity(flecs::world& ecs, flecs::entity squad)
{
	ecs.delete_with<state::MemberOf>(squad.id());
	squad.destruct();
}

// Per reason: the delta factor, and the floor (negative steps) or ceiling
// (positive steps). FACTIONS.md 3.1.
struct ReasonRule
{
	double factor = 0.0;
	double bound = 0.0;
	bool known = false;
};

ReasonRule ruleFor(RelationReason reason) noexcept
{
	switch (reason)
	{
	case RelationReason::Aggression:
		return {-0.05, -20.0, true};
	case RelationReason::Hit:
		return {-2.5, -50.0, true};
	case RelationReason::HeavyHit:
		return {-10.0, -50.0, true};
	case RelationReason::Unprovoked:
		return {-20.0, -100.0, true};
	case RelationReason::Kill:
		return {-20.0, -75.0, true};
	case RelationReason::Treatment:
		return {0.15, 5.0, true};
	case RelationReason::Capture:
		return {-10.0, -60.0, true};
	case RelationReason::Betrayal:
		return {-20.0, -100.0, true}; // DAT_1416b1860 with multiplier DAT_14167c308 = 1.0; vf4 clamps to [-100, 100]
	}
	return {};
}

} // namespace

double onSiteHours(std::int32_t key) noexcept
{
	switch (key)
	{
	case 2:
		return 12.0;
	case 3:
		return 1.0;
	default:
		return 4.0;
	}
}

double relationAfter(double relation, RelationReason reason, double amount, double dt) noexcept
{
	const ReasonRule rule = ruleFor(reason);
	if (!rule.known)
		return relation;
	const double delta = rule.factor * amount * (reason == RelationReason::Treatment ? dt : 1.0);
	if (delta < 0.0 ? relation > rule.bound : relation < rule.bound)
		relation += delta;
	return std::clamp(relation, -100.0, 100.0);
}

double factionMaxProsperity(const kdata::GameData& data, std::string_view factionSid)
{
	const kdata::Record* record = data.find(std::string(factionSid));
	return static_cast<double>(record != nullptr ? record->intOr("max prosperity", 1000) : 1000);
}

double factionProsperity(const kdata::GameData& data, flecs::entity faction)
{
	const state::FactionState* state = faction.get<state::FactionState>();
	return state != nullptr ? factionMaxProsperity(data, state->templateSid) - state->prosperityLost : 0.0;
}

double factionRelation(const kdata::GameData& data, flecs::entity from, flecs::entity to)
{
	if (from.id() == to.id())
		return 100.0;
	if (const state::Relation* relation = from.get<state::Relation>(to))
		return relation->value;
	const state::FactionState* state = from.get<state::FactionState>();
	const kdata::Record* record = state != nullptr ? data.find(state->templateSid) : nullptr;
	return record != nullptr ? record->intOr("default relation", 0) : 0.0;
}

bool factionsHostile(const kdata::GameData& data, flecs::entity from, flecs::entity to)
{
	// Task BV: see factions.hpp. An NPC reads its own faction's view of the target; the player faction does not read its own map, the
	// lookup goes to the target faction's relations (kinspect dt21, PlayerFactionRelations), so a player <-> NPC pair reads the NPC's
	// record toward the player in both orders.
	if (from.get<state::PlayerFaction>() != nullptr && to.get<state::PlayerFaction>() == nullptr)
		return factionRelation(data, to, from) <= FactionBook::kHostile;
	return factionRelation(data, from, to) <= FactionBook::kHostile;
}

double playerStanding(const kdata::GameData& data, flecs::entity player, flecs::entity other)
{
	// dt21: the NPC faction's own record toward the player; the player's map is never consulted.
	return factionRelation(data, other, player);
}

double applyRelationReason(const kdata::GameData& data, flecs::entity victim,
	flecs::entity aggressor, RelationReason reason, double amount, double dt)
{
	if (!victim.is_valid() || !aggressor.is_valid())
		return 0.0;
	const double before = factionRelation(data, victim, aggressor);
	if (victim.id() == aggressor.id())
		return before;
	const double after = relationAfter(before, reason, amount, dt);
	if (after != before)
	{
		victim.set<state::Relation>(aggressor, state::Relation{after});
		flecs::world world = victim.world();
		noteRelationChange(world, victim, aggressor, before, after);
	}
	return after;
}

void noteRelationChange(flecs::world& ecs, flecs::entity victim, flecs::entity other,
	double before, double after)
{
	// A relation change involving the player faction, in either direction, makes the other faction known to it (kinspect 49; UNSURE).
	const auto learn = [](flecs::entity player, flecs::entity other)
	{
		const state::PlayerFaction* held = player.get<state::PlayerFaction>();
		const state::FactionState* faction = other.get<state::FactionState>();
		if (held == nullptr || faction == nullptr || player.id() == other.id()
			|| std::find(held->known.begin(), held->known.end(), faction->templateSid) != held->known.end())
			return;
		state::PlayerFaction copy = *held;
		copy.known.push_back(faction->templateSid);
		player.set<state::PlayerFaction>(std::move(copy));
	};
	learn(victim, other);
	learn(other, victim);
	const state::Clock* clock = ecs.get<state::Clock>();
	const std::uint64_t tick = clock != nullptr ? clock->tick : 0;
	const auto push = [&](state::RelationCrossing crossing)
	{
		pushFactionEvent(ecs, state::FactionEvent{state::EventKind::RelationCrossed,
			state::EntityRef{victim.id()}, {}, tick, state::EntityRef{other.id()},
			static_cast<std::int32_t>(crossing)});
	};
	const bool wasHostile = before <= FactionBook::kHostile;
	const bool isHostile = after <= FactionBook::kHostile;
	if (isHostile != wasHostile)
		push(isHostile ? state::RelationCrossing::BecameHostile : state::RelationCrossing::NoLongerHostile);
	const bool wasAlly = before >= FactionBook::kAlly;
	const bool isAlly = after >= FactionBook::kAlly;
	if (isAlly != wasAlly)
		push(isAlly ? state::RelationCrossing::BecameAlly : state::RelationCrossing::NoLongerAlly);
}

// ---------------------------------------------------------------------------
// FactionBook
// ---------------------------------------------------------------------------

std::shared_ptr<const FactionBook> FactionBook::build(
	std::shared_ptr<const kdata::GameData> gameData)
{
	auto book = std::shared_ptr<FactionBook>(new FactionBook);
	book->data_ = std::move(gameData);
	const kdata::GameData& data = *book->data_;
	const std::int32_t campaignType = kworld::itemTypeByName("FACTION_CAMPAIGN");
	const std::int32_t squadType = kworld::itemTypeByName("SQUAD_TEMPLATE");

	// A scratch builder only for its world-state test.
	flecs::world scratch;
	RngStream unused(1);
	const SquadBuilder judge(scratch, data, unused);

	for (const kdata::Record* record : data.byType(campaignType))
	{
		CampaignInfo info;
		info.sid = record->stringID;
		info.name = record->name;
		info.key = record->intOr("key", 0);
		info.hostile = record->boolOr("is hostile", true);
		info.territorial = record->boolOr("territorial triggers", false);
		info.targetCharacters = record->boolOr("target characters", false);
		info.ignoresNoGo = record->boolOr("ignores nogo zones", false);
		info.ignoresChanceMults = record->boolOr("ignores chance mults", false);
		info.eligible = judge.worldStateHolds(*record);
		info.numForces = record->intOr("num forces", 20);
		info.chancePerDay = record->floatOr("chance per day", 0.0f);
		info.rangeNear = record->floatOr("range near", 0.0f);
		info.rangeFar = record->floatOr("range far", 0.0f);
		info.targetPopulationMin = record->intOr("target population min", 0);
		info.targetTechLevel = record->intOr("target tech level >=", 0);
		info.repeatDays = record->floatOr("repeat limit", 4.0f);
		const std::vector<const kdata::Record*> ancestry = lineage(data, *record, campaignType);
		// `squads to use`: the record's own, else the nearest ancestor's.
		for (const kdata::Record* source : ancestry)
		{
			forEachRef(*source, "squads to use", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* squad = data.find(entry.targetID);
				if (squad != nullptr && squad->type == squadType)
					info.squads.emplace_back(entry.targetID, std::max(1, entry.v0));
			});
			if (!info.squads.empty())
				break;
		}
		const auto follow = [&](std::string_view category, std::vector<CampaignInfo::Follow>& out)
		{
			forEachRef(*record, category, [&](const kdata::RefEntry& entry)
			{
				out.push_back({entry.targetID, static_cast<double>(entry.v0),
					static_cast<double>(std::max(entry.v0, entry.v1)),
					static_cast<double>(entry.v2)});
			});
		};
		// The phase packages: per key the record's own, else the nearest ancestor's; the first ref of a list names the package.
		const auto packageOf = [&](std::string_view category) -> std::string
		{
			for (const kdata::Record* source : ancestry)
			{
				std::string found;
				forEachRef(*source, category, [&](const kdata::RefEntry& entry)
				{
					if (found.empty())
						found = entry.targetID;
				});
				if (!found.empty())
					return found;
			}
			return {};
		};
		const char* const kPhaseKeys[3][2] = {{"pt1 AI leader", "pt1 AI others"}, {"pt2 AI leader", "pt2 AI others"},
			{"retreat AI leader", "retreat AI others"}};
		for (std::size_t phase = 0; phase < info.phases.size(); ++phase)
		{
			info.phases[phase].leader = packageOf(kPhaseKeys[phase][0]);
			info.phases[phase].others = packageOf(kPhaseKeys[phase][1]);
			if (info.phases[phase].others.empty())
				info.phases[phase].others = info.phases[phase].leader;
		}
		info.victory.leader = packageOf("victory AI leader");
		info.victory.others = packageOf("victory AI others");
		if (info.victory.others.empty())
			info.victory.others = info.victory.leader;
		follow("victory trigger", info.onVictory);
		follow("loss trigger", info.onLoss);
		book->campaignIndex_.emplace(info.sid, book->campaigns_.size());
		book->campaigns_.push_back(std::move(info));
	}

	const std::int32_t townType = kworld::itemTypeByName("TOWN");
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("FACTION")))
	{
		FactionInfo info;
		info.sid = record->stringID;
		info.name = record->name;
		info.defaultRelation = record->intOr("default relation", 0);
		const auto colours = [&](std::string_view category, std::vector<std::uint32_t>& out)
		{
			forEachRef(*record, category, [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* group = data.find(entry.targetID);
				if (group != nullptr)
					out.push_back(static_cast<std::uint32_t>(group->intOr("index", 0)));
			});
			std::sort(out.begin(), out.end());
		};
		colours("biomes", info.biomes);
		colours("no-go zones", info.noGo);
		forEachRef(*record, "campaigns", [&](const kdata::RefEntry& entry)
		{
			const auto found = book->campaignIndex_.find(entry.targetID);
			if (found != book->campaignIndex_.end())
				info.campaigns.push_back(found->second);
		});
		// A campaign with no force list uses the owner's roaming squads: the
		// FACTION `roaming squads` and those of the TOWNs it owns (the field
		// text: "if nothing is listed it will try to use TOWN [roaming
		// spawns]"). The first faction that lists the campaign supplies them.
		bool needed = false;
		for (const std::size_t index : info.campaigns)
			needed = needed || book->campaigns_[index].squads.empty();
		if (needed)
		{
			std::vector<std::pair<std::string, std::int32_t>> roaming;
			const auto add = [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* squad = data.find(entry.targetID);
				if (squad != nullptr && squad->type == squadType
					&& std::none_of(roaming.begin(), roaming.end(), [&](const auto& have)
						{ return have.first == entry.targetID; }))
					roaming.emplace_back(entry.targetID, 100);
			};
			forEachRef(*record, "roaming squads", add);
			for (const kdata::Record* town : data.byType(townType))
			{
				const kdata::RefCategory* owner = town->findRefs("faction");
				if (owner != nullptr && !owner->entries.empty()
					&& owner->entries.front().targetID == record->stringID)
					forEachRef(*town, "roaming squads", add);
			}
			for (const std::size_t index : info.campaigns)
			{
				CampaignInfo& campaign = book->campaigns_[index];
				if (campaign.squads.empty() && !roaming.empty())
				{
					campaign.squads = roaming;
					campaign.squadsFromFallback = true;
				}
			}
		}
		book->factionIndex_.emplace(info.sid, book->factions_.size());
		book->factions_.push_back(std::move(info));
	}
	return book;
}

const FactionInfo* FactionBook::faction(std::string_view sid) const
{
	const auto found = factionIndex_.find(std::string(sid));
	return found == factionIndex_.end() ? nullptr : &factions_[found->second];
}

const CampaignInfo* FactionBook::campaign(std::string_view sid) const
{
	const auto found = campaignIndex_.find(std::string(sid));
	return found == campaignIndex_.end() ? nullptr : &campaigns_[found->second];
}

std::size_t FactionBook::runnable() const
{
	std::set<std::size_t> owned;
	for (const FactionInfo& info : factions_)
		owned.insert(info.campaigns.begin(), info.campaigns.end());
	std::size_t count = 0;
	for (const std::size_t index : owned)
	{
		const CampaignInfo& campaign = campaigns_[index];
		count += campaign.eligible && campaign.territorial && campaign.rangeFar > 0.0
			&& campaign.chancePerDay > 0.0 && !campaign.squads.empty() ? 1 : 0;
	}
	return count;
}

void pushFactionEvent(flecs::world& ecs, const state::FactionEvent& event)
{
	if (ecs.get<state::FactionEvents>() == nullptr)
		ecs.set<state::FactionEvents>(state::FactionEvents{});
	ecs.get_mut<state::FactionEvents>()->pending.push_back(event);
}

// ---------------------------------------------------------------------------
// UtilityFactionLayer
// ---------------------------------------------------------------------------

struct UtilityFactionLayer::Candidate
{
	const CampaignInfo* campaign = nullptr;
	flecs::entity_t target = 0;
	flecs::entity_t origin = 0;
	double tx = 0.0;
	double tz = 0.0;
	double ox = 0.0;
	double oz = 0.0;
	double distance = 0.0;
	double chance = 0.0; // per game-hour; 1 for a follow-up
	bool triggered = false;
};

UtilityFactionLayer::UtilityFactionLayer(std::shared_ptr<const FactionBook> book,
	const Navigator* navigator, const std::vector<TownSite>& towns, std::uint64_t seed,
	FactionTuning tuning, std::shared_ptr<const AreaLookup> areas, FactionScriptSetup script)
	: book_(std::move(book))
	, navigator_(navigator)
	, towns_(towns)
	, seed_(seed)
	, tuning_(tuning)
	, areas_(std::move(areas))
{
	if (!script.source.empty())
	{
		script_ = std::make_unique<FactionScript>(std::move(script), seed_);
		stats_.scriptLoaded = script_->loaded();
	}
}

UtilityFactionLayer::~UtilityFactionLayer() = default;

// A target town must lie in the faction's `biomes` (when it lists any) and
// outside its `no-go zones` (unless the campaign ignores them). FACTIONS.md 2.1.
bool UtilityFactionLayer::inTerritory(const FactionInfo& info, const CampaignInfo& campaign,
	double x, double z) const
{
	if (areas_ == nullptr || (info.biomes.empty() && (info.noGo.empty() || campaign.ignoresNoGo)))
		return true;
	const std::uint32_t colour = areas_->colourAt(x, z);
	const auto has = [&](const std::vector<std::uint32_t>& list)
	{
		return std::binary_search(list.begin(), list.end(), colour);
	};
	if (!info.biomes.empty() && !has(info.biomes))
		return false;
	return campaign.ignoresNoGo || !has(info.noGo);
}

double UtilityFactionLayer::roll(const FactionContext& context, std::uint64_t salt) const
{
	return unit(splitMix64(seed_ ^ splitMix64(context.id)
		^ splitMix64(context.tick * 0x9E3779B97F4A7C15ULL + salt)));
}

namespace {

struct TownView
{
	const TownSite* site = nullptr;
	flecs::entity_t owner = 0;
};

// The faction's view of `other`: the live relation pair, else its default.
double relationOf(flecs::entity faction, flecs::entity other, double fallback)
{
	if (const state::Relation* relation = faction.get<state::Relation>(other))
		return relation->value;
	return fallback;
}

void forget(std::vector<state::Cooldown>& list, std::uint64_t now)
{
	list.erase(std::remove_if(list.begin(), list.end(),
		[&](const state::Cooldown& entry) { return entry.until <= now; }), list.end());
}

bool cooling(const std::vector<state::Cooldown>& list, std::string_view campaign)
{
	return std::any_of(list.begin(), list.end(),
		[&](const state::Cooldown& entry) { return entry.campaign == campaign; });
}

bool cooling(const std::vector<state::Cooldown>& list, flecs::entity_t town)
{
	return std::any_of(list.begin(), list.end(),
		[&](const state::Cooldown& entry) { return entry.town.id == town; });
}

void cool(std::vector<state::Cooldown>& list, const std::string& campaign,
	flecs::entity_t town, std::uint64_t until)
{
	const auto same = std::find_if(list.begin(), list.end(), [&](const state::Cooldown& entry)
	{
		return entry.campaign == campaign && entry.town.id == town;
	});
	if (same != list.end())
		same->until = std::max(same->until, until);
	else
		list.push_back(state::Cooldown{campaign, state::EntityRef{town}, until});
}

} // namespace

// Faction state on first sight, and the part of the temperament that is a
// function of the live relations (aggression), refreshed each step.
void UtilityFactionLayer::ensure(FactionContext& context, state::FactionAI& ai) const
{
	const FactionInfo* info = book_ != nullptr ? book_->faction(context.faction.templateSid)
		: nullptr;
	if (info == nullptr || info->campaigns.empty())
	{
		ai.aggression = 0.0;
		return;
	}
	std::set<flecs::entity_t> owners;
	for (const TownSite& town : towns_)
	{
		const state::TownState* state = flecs::entity(context.ecs.c_ptr(), town.entity)
			.get<state::TownState>();
		if (state != nullptr && state->owner.id != 0 && state->owner.id != context.entity.id())
			owners.insert(state->owner.id);
	}
	std::size_t hostile = 0;
	for (const flecs::entity_t owner : owners)
	{
		hostile += relationOf(context.entity, flecs::entity(context.ecs.c_ptr(), owner),
			info->defaultRelation) <= FactionBook::kHostile ? 1 : 0;
	}
	ai.aggression = kAggressionBase + kAggressionSpread
		* (owners.empty() ? 0.0 : static_cast<double>(hostile) / owners.size());
}

// Information up: take this faction's events into the counters. What they mean for
// the temperament is decide's.
void UtilityFactionLayer::absorb(FactionContext& context, state::FactionAI& ai)
{
	for (const state::FactionEvent& event : context.events)
	{
		if (event.faction.id != context.entity.id())
			continue;
		++ai.events;
		++stats_.eventsConsumed;
		if (event.kind == state::EventKind::SquadLost)
			++ai.squadsLost;
	}
}

// The native temperament and intent scoring: decay, the events, then utility over
// the small goal set. The script (data/behaviour/default/faction_intent.lua) is a
// transcription of this, in the same order of arithmetic; this is the engine's
// fallback and the reference the equivalence test compares it to.
void UtilityFactionLayer::nativeDecide(FactionContext& context, state::FactionAI& ai, bool canRaid)
{
	ai.alarm *= kAlarmDecay;
	ai.grief *= kGriefDecay;
	ai.confidence *= kConfidenceDecay;
	for (const state::FactionEvent& event : context.events)
	{
		if (event.faction.id != context.entity.id())
			continue;
		switch (event.kind)
		{
		case state::EventKind::SquadLost:
			ai.grief += kSquadLostGrief;
			break;
		case state::EventKind::TownLost:
			ai.alarm += kTownLostAlarm;
			ai.grief += kTownLostGrief;
			break;
		case state::EventKind::TownGained:
			ai.confidence += kTownGainedConfidence;
			break;
		case state::EventKind::RaidOutcome:
		case state::EventKind::RelationCrossed:
			break; // the native scoring does not use them
		}
	}
	ai.alarm = std::min(1.0, ai.alarm);
	ai.grief = std::min(1.0, ai.grief);
	ai.confidence = std::min(1.0, ai.confidence);

	double score[state::kIntentCount];
	score[static_cast<int>(state::Intent::Consolidate)] =
		kConsolidateBase + kConsolidateConfidence * ai.confidence;
	score[static_cast<int>(state::Intent::Defend)] = kDefendBase + kDefendAlarm * ai.alarm;
	score[static_cast<int>(state::Intent::Mourn)] = kMourn * ai.grief;
	score[static_cast<int>(state::Intent::Raid)] = canRaid
		? ai.aggression * (1.0 - ai.grief) * kRaidScale : 0.0;
	const double age = gameHoursIn(ticksSince(context.tick, ai.since));
	score[static_cast<int>(ai.intent)] += kIntentBonus * std::max(0.0, 1.0 - age / kIntentBonusHours);
	int best = 0;
	for (int i = 1; i < state::kIntentCount; ++i)
	{
		if (score[i] > score[best])
			best = i;
	}
	if (static_cast<state::Intent>(best) != ai.intent)
	{
		ai.intent = static_cast<state::Intent>(best);
		ai.since = context.tick;
	}
}

void UtilityFactionLayer::decide(FactionContext& context, state::FactionAI& ai, bool canRaid)
{
	if (script_ != nullptr)
	{
		const FactionInfo* info = book_ != nullptr ? book_->faction(context.faction.templateSid)
			: nullptr;
		switch (script_->decide(context, info, ai, canRaid, stats_))
		{
		case FactionScript::Result::Applied:
		case FactionScript::Result::Sleeping:
			return;
		case FactionScript::Result::Fallback:
			break;
		}
	}
	nativeDecide(context, ai, canRaid);
}

// The outcome of a raid whose squads are all gone.
void UtilityFactionLayer::conclude(FactionContext& context, state::FactionAI& ai,
	const state::Raid& raid, bool defeated)
{
	// A defeated force ("has been defeated") is a loss whatever it reached.
	const bool arrived = raid.arrived != 0 && !defeated;
	if (arrived)
		++ai.completed;
	else
		++ai.failed;
	pushFactionEvent(context.ecs, state::FactionEvent{state::EventKind::RaidOutcome,
		state::EntityRef{context.entity.id()}, raid.target, context.tick, {},
		static_cast<std::int32_t>(arrived ? state::RaidResult::Victory
			: defeated ? state::RaidResult::Defeated : state::RaidResult::Loss)});
	if (!arrived)
	{
		cool(ai.targetCooldowns, std::string(), raid.target.id,
			context.tick + static_cast<std::uint64_t>(tuning_.failedTargetHours * kTicksPerHour));
		cool(ai.campaignCooldowns, raid.campaign, 0,
			context.tick + static_cast<std::uint64_t>(tuning_.failedCampaignHours * kTicksPerHour));
	}
	// Follow-ups: `victory trigger` / `loss trigger` are absolute chances in
	// sequence (a total under 100 leaves a chance of nothing), a delay of
	// min..max game-hours.
	const CampaignInfo* campaign = book_ != nullptr ? book_->campaign(raid.campaign) : nullptr;
	if (campaign == nullptr)
		return;
	const std::vector<CampaignInfo::Follow>& follows = arrived ? campaign->onVictory
		: campaign->onLoss;
	const double pick = roll(context, hashOf(raid.campaign) + 0x7721) * 100.0;
	double sum = 0.0;
	for (const CampaignInfo::Follow& follow : follows)
	{
		sum += follow.percent;
		if (pick >= sum)
			continue;
		const double hours = follow.minHours + (follow.maxHours - follow.minHours)
			* roll(context, hashOf(follow.campaign) + 0x7722);
		ai.triggers.push_back(state::PendingTrigger{follow.campaign,
			context.tick + static_cast<std::uint64_t>(hours * kTicksPerHour)});
		break;
	}
}

// Information up from the squads: follow each raid; retire finished squads.
bool UtilityFactionLayer::follow(FactionContext& context, state::FactionAI& ai)
{
	bool changed = false;
	std::vector<state::Raid> keep;
	for (state::Raid& raid : ai.raids)
	{
		std::vector<std::uint64_t> alive;
		for (const std::uint64_t id : raid.squads)
		{
			const auto found = std::lower_bound(context.squads.begin(), context.squads.end(),
				std::pair<std::uint64_t, flecs::entity_t>{id, 0});
			if (found == context.squads.end() || found->first != id)
			{
				++raid.squadsLost; // gone (removed or killed: the producer pushed the event)
				continue;
			}
			const flecs::entity squad(context.ecs.c_ptr(), found->second);
			const state::GroupAI* group = squad.get<state::GroupAI>();
			const state::OrderStatus status = group != nullptr ? group->order.status
				: state::OrderStatus::Issued;
			if (status != state::OrderStatus::Issued && !raid.started)
			{
				raid.started = true;
				++ai.started;
			}
			if ((status == state::OrderStatus::Staying || status == state::OrderStatus::Retreating
				|| status == state::OrderStatus::Returning
				|| status == state::OrderStatus::Done) && raid.arrived == 0)
			{
				raid.arrived = context.tick;
				++ai.arrived;
			}
			if (status == state::OrderStatus::Done || status == state::OrderStatus::Failed)
			{
				// Campaign squads exist for the raid only.
				removeSquadEntity(context.ecs, squad);
				++stats_.squadsRetired;
				changed = true;
				continue;
			}
			alive.push_back(id);
		}
		raid.squads = std::move(alive);
		// Defeated: the alive fraction (squads not killed, whatever they are
		// doing) is under 0.2. Members are not tracked one by one: a squad is
		// lost whole (only RemoveSquad kills, there is no combat yet).
		const bool defeated = raid.squadsLost > 0 && raid.squadsAtStart > 0
			&& 1.0 - static_cast<double>(raid.squadsLost) / raid.squadsAtStart < kDefeatedFraction;
		if (defeated && !raid.squads.empty())
		{
			for (const std::uint64_t id : raid.squads)
			{
				const auto found = std::lower_bound(context.squads.begin(), context.squads.end(),
					std::pair<std::uint64_t, flecs::entity_t>{id, 0});
				removeSquadEntity(context.ecs, flecs::entity(context.ecs.c_ptr(), found->second));
				++stats_.squadsRetired;
			}
			raid.squads.clear();
			changed = true;
		}
		if (defeated)
			++stats_.defeated;
		if (raid.squads.empty())
			conclude(context, ai, raid, defeated);
		else
			keep.push_back(std::move(raid));
	}
	ai.raids = std::move(keep);
	return changed;
}

std::vector<UtilityFactionLayer::Candidate> UtilityFactionLayer::candidates(
	FactionContext& context, const state::FactionAI& ai, bool rolling)
{
	std::vector<Candidate> out;
	const FactionInfo* info = book_ != nullptr ? book_->faction(context.faction.templateSid)
		: nullptr;
	if (info == nullptr || info->campaigns.empty() || navigator_ == nullptr)
		return out;

	std::vector<TownView> views;
	std::vector<const TownSite*> mine;
	for (const TownSite& site : towns_)
	{
		const state::TownState* state = flecs::entity(context.ecs.c_ptr(), site.entity)
			.get<state::TownState>();
		if (state == nullptr)
			continue;
		views.push_back({&site, state->owner.id});
		if (state->owner.id == context.entity.id() && navigator_->covers(site.x, site.z))
			mine.push_back(&site);
	}
	if (mine.empty())
		return out;

	std::map<flecs::entity_t, std::int32_t> relations;
	const auto relation = [&](flecs::entity_t owner)
	{
		const auto found = relations.find(owner);
		if (found != relations.end())
			return found->second;
		return relations.emplace(owner, relationOf(context.entity,
			flecs::entity(context.ecs.c_ptr(), owner), info->defaultRelation)).first->second;
	};

	// A player town: a TownState whose owner is a player faction (an owned record town, `scenarioTownToPlayer`, a conquest). The player's own
	// outposts (state::PlayerTown) are not record towns and not raid sites yet. The player's population: members over its squads.
	std::map<flecs::entity_t, bool> playerOwned;
	const auto isPlayer = [&](flecs::entity_t owner)
	{
		const auto found = playerOwned.find(owner);
		if (found != playerOwned.end())
			return found->second;
		return playerOwned.emplace(owner, flecs::entity(context.ecs.c_ptr(), owner).has<state::PlayerFaction>()).first->second;
	};
	std::int64_t population = -1;
	const auto playerPopulation = [&]()
	{
		if (population < 0)
		{
			population = 0;
			context.ecs.each([&](const state::PlayerSquad& squad) { population += static_cast<std::int64_t>(squad.members.size()); });
		}
		return population;
	};

	// A player town's size level (FACTIONS.md 4): the owner's finished buildings inside the town's circle (OURS for "parented to the town": a record town
	// owns no PlayerBuilding, so the circle stands in). Computed on the first campaign that gates on it, once per town per call.
	std::map<flecs::entity_t, int> levels;
	const auto townLevel = [&](const TownView& view)
	{
		const auto found = levels.find(view.site->entity);
		if (found != levels.end())
			return found->second;
		const state::TownState* town = flecs::entity(context.ecs.c_ptr(), view.site->entity).get<state::TownState>();
		const towncircle::Circle circle = towncircle::of(book_->data(), *town);
		const std::uint64_t owner = flecs::entity(context.ecs.c_ptr(), view.owner).get<state::Id>()->value;
		return levels.emplace(view.site->entity, towncircle::levelOf(context.ecs, book_->data(), owner, circle)).first->second;
	};

	const auto consider = [&](const CampaignInfo& campaign, bool triggered)
	{
		if (cooling(ai.campaignCooldowns, campaign.sid) || campaign.squads.empty()
			|| std::any_of(ai.raids.begin(), ai.raids.end(),
				[&](const state::Raid& raid) { return raid.campaign == campaign.sid; }))
			return;
		Candidate best;
		best.campaign = &campaign;
		best.triggered = triggered;
		double bestFall = -1.0;
		std::uint64_t bestId = 0;
		for (const TownView& view : views)
		{
			if (view.owner == 0 || view.owner == context.entity.id()
				|| cooling(ai.targetCooldowns, view.site->entity)
				|| !navigator_->covers(view.site->x, view.site->z))
				continue;
			// The original raids the player's towns only; other factions' towns behind world.npc_raids.
			if (isPlayer(view.owner) ? playerPopulation() < campaign.targetPopulationMin : !tuning_.npcRaids)
				continue;
			// The town gate (1409c5740, a town target only): a player town below `target tech level >=` has chance 0.
			if (campaign.targetTechLevel > 0 && isPlayer(view.owner) && townLevel(view) < campaign.targetTechLevel)
				continue;
			// Hostile campaigns go for the hostile; visits and tributes for the rest.
			const double feeling = relation(view.owner);
			if (campaign.hostile ? feeling > FactionBook::kHostile : feeling <= FactionBook::kHostile)
				continue;
			if (!inTerritory(*info, campaign, view.site->x, view.site->z))
			{
				++stats_.territoryRefused;
				continue;
			}
			const TownSite* from = nullptr;
			double nearest = 1.0e18;
			for (const TownSite* origin : mine)
			{
				const double d = distance(origin->x, origin->z, view.site->x, view.site->z);
				if (d < nearest)
				{
					nearest = d;
					from = origin;
				}
			}
			// Falloff: 1 inside `range near`, linear to 0 at `range far`; with no
			// `range far` there is no distance gate. The data's ranges are metres.
			double fall = 1.0;
			if (!triggered && campaign.rangeFar > 0.0)
			{
				const double near = campaign.rangeNear * kUnitsPerMetre;
				const double far = campaign.rangeFar * kUnitsPerMetre;
				if (nearest > far)
					continue;
				if (far > near && nearest > near)
					fall = (far - nearest) / (far - near);
			}
			if (fall < bestFall || (fall == bestFall && (nearest > best.distance
				|| (nearest == best.distance && view.site->id > bestId))))
				continue;
			bestFall = fall;
			bestId = view.site->id;
			best.target = view.site->entity;
			best.origin = from->entity;
			best.tx = view.site->x;
			best.tz = view.site->z;
			best.ox = from->x;
			best.oz = from->z;
			best.distance = nearest;
			// The roll chance: per day x the 6-hour share x falloff x the option.
			best.chance = triggered ? 1.0 : campaign.chancePerDay * kRollShare * fall
				* (campaign.ignoresChanceMults ? 1.0 : tuning_.raidFrequency);
		}
		if (best.target != 0 && (triggered || best.chance > 0.0))
			out.push_back(best);
	};

	for (const state::PendingTrigger& trigger : ai.triggers)
	{
		const CampaignInfo* campaign = trigger.due <= context.tick
			? book_->campaign(trigger.campaign) : nullptr;
		if (campaign != nullptr)
			consider(*campaign, true);
	}
	// The random campaigns only on a roll pass; a campaign without
	// `territorial triggers` is never rolled (chains and requests only).
	for (const std::size_t index : info->campaigns)
	{
		const CampaignInfo& campaign = book_->campaigns()[index];
		if (rolling && campaign.eligible && campaign.territorial && campaign.chancePerDay > 0.0)
			consider(campaign, false);
	}
	std::sort(out.begin(), out.end(), [](const Candidate& left, const Candidate& right)
	{
		return std::tuple(!left.triggered, -left.chance, left.campaign->sid)
			< std::tuple(!right.triggered, -right.chance, right.campaign->sid);
	});
	return out;
}

bool issueRaid(state::FactionAI& ai, const RaidOrder& order, const std::vector<flecs::entity>& squads)
{
	for (const flecs::entity squad : squads)
	{
		if (squad.get<state::Position>() == nullptr || squad.get<state::Id>() == nullptr)
			return false;
	}
	state::Raid raid;
	raid.campaign = order.campaign;
	raid.target = order.target;
	raid.origin = order.origin;
	raid.planned = order.now;
	raid.squadsAtStart = static_cast<std::uint32_t>(squads.size());
	for (const flecs::entity squad : squads)
	{
		const state::Position at = order.originAt.value_or(*squad.get<state::Position>());
		state::GroupAI group;
		group.order.status = state::OrderStatus::Issued;
		group.order.target = order.target;
		group.order.originX = at.x;
		group.order.originZ = at.z;
		group.order.stay = order.stay;
		group.order.retreat = order.retreat;
		group.order.deadline = order.deadline;
		group.order.issued = order.now;
		group.order.campaign = order.campaign;
		squad.set<state::GroupAI>(std::move(group));
		raid.squads.push_back(squad.get<state::Id>()->value);
	}
	ai.raids.push_back(std::move(raid));
	++ai.planned;
	return true;
}

bool UtilityFactionLayer::commission(FactionContext& context, state::FactionAI& ai,
	const Candidate& candidate)
{
	const CampaignInfo& campaign = *candidate.campaign;
	const Route route = navigator_->route(candidate.ox, candidate.oz, candidate.tx, candidate.tz);
	if (!route.found || route.points.size() < 2)
		return false;
	double length = 0.0;
	for (std::size_t i = 1; i < route.points.size(); ++i)
		length += distance(route.points[i - 1].x, route.points[i - 1].z, route.points[i].x,
			route.points[i].z);
	const double walkTicks = length / UtilityGroupLayer::kWalkSpeed / kTickSeconds;
	// On site by `key`, then the 3-hour retreat state (FACTIONS.md 2.1).
	const std::uint64_t stay = static_cast<std::uint64_t>(onSiteHours(campaign.key) * kTicksPerHour);
	const std::uint64_t retreat = static_cast<std::uint64_t>(kRetreatHours * kTicksPerHour);
	const std::uint64_t deadline = context.tick + static_cast<std::uint64_t>(
		2.0 * walkTicks * kDeadlineSlack + kDeadlineExtraHours * kTicksPerHour) + stay + retreat;

	// The force: weighted draws from `squads to use` until int(num forces x
	// raidSizeMult) men.
	const std::int32_t wanted = static_cast<std::int32_t>(campaign.numForces * tuning_.raidSizeMult);
	RngStream rng(splitMix64(seed_ ^ splitMix64(context.id)
		^ splitMix64(context.tick ^ hashOf(campaign.sid))));
	SquadBuilder builder(context.ecs, book_->data(), rng);
	std::int64_t total = 0;
	for (const auto& [sid, weight] : campaign.squads)
		total += weight;
	std::vector<flecs::entity> raiders;
	std::int32_t forces = 0;
	while (forces < wanted && raiders.size() < tuning_.maxSquads)
	{
		std::int64_t pick = rng.between(0, total - 1);
		const std::string* chosen = &campaign.squads.back().first;
		for (const auto& [sid, weight] : campaign.squads)
		{
			if (pick < weight)
			{
				chosen = &sid;
				break;
			}
			pick -= weight;
		}
		const kdata::Record* record = book_->data().find(*chosen);
		if (record == nullptr)
			break;
		const std::size_t before = builder.characters();
		raiders.push_back(builder.build(*record, context.entity.id(), 0,
			state::SpawnSource::Campaign, true, candidate.ox, candidate.oz));
		forces += static_cast<std::int32_t>(std::max<std::size_t>(1, builder.characters() - before));
	}
	if (raiders.empty())
		return false;

	RaidOrder order;
	order.campaign = campaign.sid;
	order.target = state::EntityRef{candidate.target};
	order.origin = state::EntityRef{candidate.origin};
	order.stay = stay;
	order.retreat = retreat;
	order.deadline = deadline;
	order.now = context.tick;
	order.originAt = state::Position{candidate.ox, candidate.oz};
	if (!issueRaid(ai, order, raiders))
		return false;
	stats_.squadsSpawned += raiders.size();
	++stats_.raidsPlanned;
	// `repeat limit`: days before this campaign may start again.
	cool(ai.campaignCooldowns, campaign.sid, 0, context.tick
		+ static_cast<std::uint64_t>(std::min(campaign.repeatDays, 3650.0) * kHoursPerDay * kTicksPerHour));
	return true;
}

FactionResult UtilityFactionLayer::update(FactionContext& context)
{
	FactionResult result;
	++stats_.steps;
	state::FactionAI* ai = context.entity.get_mut<state::FactionAI>();
	if (ai == nullptr)
		return result;
	const bool first = ai->lastStep == 0;
	ai->lastStep = context.tick;
	if (first)
		ai->since = context.tick;

	// Prosperity regenerates a little with every update below the cap (kinspect item 53).
	if (state::FactionState* state = context.entity.get_mut<state::FactionState>(); state != nullptr && state->prosperityLost > 0.0)
		state->prosperityLost = std::max(0.0, state->prosperityLost - trader::kProsperityRegen);

	forget(ai->campaignCooldowns, context.tick);
	forget(ai->targetCooldowns, context.tick);
	absorb(context, *ai);
	ensure(context, *ai);
	result.instancesChanged = follow(context, *ai);

	// The roll clock (the exe, FACTIONS.md 2.1): set at the first step to now +
	// 6 game-hours, reset to now + 6 on every pass that finds the time reached.
	const std::uint64_t rollTicks = static_cast<std::uint64_t>(kRollHours * kTicksPerHour);
	if (ai->nextRoll == 0)
		ai->nextRoll = context.tick + rollTicks;
	const bool pass = context.tick >= ai->nextRoll;
	if (pass)
		ai->nextRoll = context.tick + rollTicks;
	// At most `maxRaids` (one) running campaign; a queued follow-up (due or
	// not) is a queued request and holds the roll back.
	const bool room = ai->raids.size() < tuning_.maxRaids;
	const bool rolling = pass && room && ai->triggers.empty();
	const bool dueTrigger = std::any_of(ai->triggers.begin(), ai->triggers.end(),
		[&](const state::PendingTrigger& trigger) { return trigger.due <= context.tick; });

	// Temperament and intent. Raid is on offer to a faction that has campaigns
	// (aggression is 0 otherwise) and a navigator; whether a roll finds a target is
	// decided on the roll pass.
	decide(context, *ai, navigator_ != nullptr && ai->aggression > 0.0);
	++stats_.intentSteps[static_cast<std::size_t>(ai->intent)];

	// Commission: due follow-ups first, whatever the intent; then, on a roll
	// pass of a faction whose intent is Raid, each campaign draws against its
	// chance and one of those that passed is picked in proportion to it. One
	// new campaign per step.
	const bool roll6 = rolling && ai->intent == state::Intent::Raid;
	std::vector<Candidate> options;
	if (room && (roll6 || dueTrigger))
		options = candidates(context, *ai, roll6);
	std::vector<std::string> spent;
	int tries = 0;
	bool commissioned = false;
	const auto reject = [&](const Candidate& candidate)
	{
		// Could not be raised (no route, no force): cool the target and the campaign.
		++ai->rejected;
		++stats_.raidsRejected;
		cool(ai->targetCooldowns, std::string(), candidate.target,
			context.tick + static_cast<std::uint64_t>(tuning_.failedTargetHours * kTicksPerHour / 2));
		cool(ai->campaignCooldowns, candidate.campaign->sid, 0,
			context.tick + static_cast<std::uint64_t>(tuning_.rejectedHours * kTicksPerHour));
	};
	for (const Candidate& candidate : options)
	{
		if (!candidate.triggered || tries >= kTries || commissioned)
			continue;
		++tries;
		++stats_.triggersFired;
		spent.push_back(candidate.campaign->sid);
		if (commission(context, *ai, candidate))
			commissioned = true;
		else
			reject(candidate);
	}
	if (roll6 && !commissioned)
	{
		++stats_.rolls;
		std::vector<Candidate> passed;
		for (const Candidate& candidate : options)
		{
			if (candidate.triggered)
				continue;
			++stats_.rollCandidates;
			if (roll(context, hashOf(candidate.campaign->sid)) < candidate.chance)
			{
				passed.push_back(candidate);
				++stats_.rollPassed;
			}
		}
		while (!passed.empty() && tries < kTries)
		{
			std::size_t index = 0;
			if (passed.size() > 1)
			{
				double sum = 0.0;
				for (const Candidate& candidate : passed)
					sum += candidate.chance;
				double pick = roll(context, 0x9171 + static_cast<std::uint64_t>(tries)) * sum;
				for (; index + 1 < passed.size() && pick >= passed[index].chance; ++index)
					pick -= passed[index].chance;
			}
			++tries;
			const Candidate chosen = passed[index];
			if (commission(context, *ai, chosen))
			{
				commissioned = true;
				break;
			}
			reject(chosen);
			passed.erase(passed.begin() + static_cast<std::ptrdiff_t>(index));
		}
	}
	result.instancesChanged = result.instancesChanged || commissioned;
	// A due follow-up is used up whether or not it could be raised; one that
	// found no way to start (no target, a campaign running) is dropped after
	// its patience, like the exe's request retries.
	const std::uint64_t patience = static_cast<std::uint64_t>(kTriggerPatienceHours * kTicksPerHour);
	if (!ai->triggers.empty())
	{
		ai->triggers.erase(std::remove_if(ai->triggers.begin(), ai->triggers.end(),
			[&](const state::PendingTrigger& trigger)
			{
				return trigger.due <= context.tick && (context.tick - trigger.due >= patience
					|| std::find(spent.begin(), spent.end(), trigger.campaign) != spent.end());
			}), ai->triggers.end());
	}
	return result;
}

} // namespace ksim
