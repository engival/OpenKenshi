#include "ksim/world/wilderness.hpp"

#include "ksim/core/rng.hpp"
#include "ksim/save/save.hpp"
#include "ksim/world/spawn.hpp"
#include "ksim/world/squad_alive.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace ksim {

namespace {

using state::SpawnSource;

// A nest TOWN's squad lists, as the default-world spawner reads them.
constexpr const char* kNestLists[] = {"residents", "default resident", "bar squads",
	"roaming squads"};

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

std::string firstRef(const kdata::Record& record, std::string_view category)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr || refs->entries.empty())
		return {};
	return refs->entries.front().targetID;
}

struct HomelessEntry
{
	const kdata::Record* squad = nullptr;
	std::int64_t weight = 0;
};

struct NestEntry
{
	std::vector<const kdata::Record*> squads;
	std::int64_t weight = 0;
};

struct SlotDef
{
	std::string group;
	std::uint32_t colour = 0;
	double zones = 0.0;
	double amount = 1.0;
	std::int32_t numNests = 0;
	std::vector<HomelessEntry> homeless;
	std::vector<NestEntry> nests;
	std::int64_t homelessWeight = 0;
	std::int64_t nestWeight = 0;
};

std::int64_t roundQuota(double value)
{
	return value <= 0.0 ? 0 : static_cast<std::int64_t>(std::floor(value + 0.5));
}

// Ascending ticks.
void sortDue(std::vector<std::uint64_t>& due)
{
	std::sort(due.begin(), due.end());
}

} // namespace

struct WildernessSpawner::Impl
{
	Impl(const kdata::GameData& gameData, std::string_view dataDir)
		: data(gameData)
		, ground(dataDir)
	{
		const std::int32_t squadType = kworld::itemTypeByName("SQUAD_TEMPLATE");
		const std::int32_t townType = kworld::itemTypeByName("TOWN");
		// A scratch builder only for its world-state test.
		flecs::world scratch;
		RngStream unused(1);
		const SquadBuilder judge(scratch, gameData, unused);
		const auto usable = [&](const std::string& sid) -> const kdata::Record*
		{
			const kdata::Record* record = data.find(sid);
			if (record == nullptr || record->type != squadType || !judge.worldStateHolds(*record))
				return nullptr;
			return record;
		};
		for (const kdata::Record* group : data.byType(kworld::itemTypeByName("BIOME_GROUP")))
		{
			SlotDef def;
			def.group = group->stringID;
			def.colour = static_cast<std::uint32_t>(group->intOr("index", 0));
			def.zones = ground.zones(def.colour);
			if (def.zones <= 0.0)
				continue;
			def.amount = group->floatOr("homeless spawn amount", 1.0f);
			def.numNests = group->intOr("num nests", 6);
			forEachRef(*group, "homeless spawns", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* squad = usable(entry.targetID);
				if (squad != nullptr && entry.v0 > 0)
				{
					def.homeless.push_back(HomelessEntry{squad, entry.v0});
					def.homelessWeight += entry.v0;
				}
			});
			forEachRef(*group, "nests", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* nest = data.find(entry.targetID);
				// A nest TOWN's own `world state` gates it as a squad's does ("Fog Deathyard Invasion" holds only once the west hive queen is
				// not okay): a nest whose state fails is no source of this group.
				if (nest == nullptr || nest->type != townType || entry.v0 <= 0 || !judge.worldStateHolds(*nest))
					return;
				NestEntry nestEntry;
				nestEntry.weight = entry.v0;
				for (const char* list : kNestLists)
				{
					forEachRef(*nest, list, [&](const kdata::RefEntry& member)
					{
						if (const kdata::Record* squad = usable(member.targetID))
							nestEntry.squads.push_back(squad);
					});
				}
				if (!nestEntry.squads.empty())
				{
					def.nests.push_back(std::move(nestEntry));
					def.nestWeight += entry.v0;
				}
			});
			if (def.homeless.empty() && def.nests.empty())
				continue;
			slots.push_back(std::move(def));
		}
	}

	std::int64_t homelessQuota(const SlotDef& slot, double population) const
	{
		return slot.homeless.empty() ? 0 : roundQuota(slot.zones * slot.amount * population);
	}

	std::int64_t nestQuota(const SlotDef& slot, double nests) const
	{
		return slot.nests.empty() ? 0 : roundQuota(slot.numNests * nests);
	}

	struct Live
	{
		std::vector<std::int64_t> homeless; // all homeless squads of the slot
		std::vector<std::int64_t> homelessIntact; // the exe sector's S2 (squad_alive.hpp); the rest is S3
		std::vector<std::int64_t> nests;
	};

	Live live(const flecs::world& ecs) const
	{
		Live live;
		live.homeless.assign(slots.size(), 0);
		live.homelessIntact.assign(slots.size(), 0);
		live.nests.assign(slots.size(), 0);
		std::vector<std::pair<std::int32_t, std::uint32_t>> nestKeys;
		ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
		{
			if (squad.slot < 0 || static_cast<std::size_t>(squad.slot) >= slots.size())
				return;
			if (squad.nest == 0)
			{
				++live.homeless[static_cast<std::size_t>(squad.slot)];
				live.homelessIntact[static_cast<std::size_t>(squad.slot)] += squadVitals(ecs, entity, &data).intact ? 1 : 0;
			} else
				nestKeys.emplace_back(squad.slot, squad.nest);
		});
		std::sort(nestKeys.begin(), nestKeys.end());
		nestKeys.erase(std::unique(nestKeys.begin(), nestKeys.end()), nestKeys.end());
		for (const auto& [slot, nest] : nestKeys)
			++live.nests[static_cast<std::size_t>(slot)];
		return live;
	}

	std::uint64_t delay(RngStream& rng, std::uint64_t minHours, std::uint64_t maxHours) const
	{
		return static_cast<std::uint64_t>(rng.between(static_cast<std::int64_t>(minHours),
			static_cast<std::int64_t>(maxHours))) * kTicksPerGameHour;
	}

	template <typename Entry>
	const Entry& draw(const std::vector<Entry>& list, std::int64_t total, RngStream& rng) const
	{
		std::int64_t pick = rng.between(0, total - 1);
		for (const Entry& entry : list)
		{
			if (pick < entry.weight)
				return entry;
			pick -= entry.weight;
		}
		return list.back();
	}

	struct Context
	{
		flecs::world& ecs;
		SquadBuilder& builder;
		RngStream& rng;
		std::unordered_map<std::string, flecs::entity_t> factions;
		state::WildernessState& wild;
		double population = 1.0;
		// Per slot: live squads (homeless and nest) by template sid, for the weight rule.
		std::vector<std::unordered_map<std::string, std::int64_t>> copies;
		WildernessReport report;
	};

	flecs::entity_t factionOf(Context& context, const kdata::Record& squad) const
	{
		const auto found = context.factions.find(firstRef(squad, "faction"));
		return found == context.factions.end() ? 0 : found->second;
	}

	flecs::entity spawnOne(Context& context, std::int32_t slotIndex, const kdata::Record& squad,
		const state::GroundPoint& den, std::uint32_t nest)
	{
		const bool isNest = nest != 0;
		flecs::entity entity = context.builder.build(squad, factionOf(context, squad), 0,
			isNest ? SpawnSource::BiomeNest : SpawnSource::BiomeHomeless, true, den.x, den.z);
		state::SquadInstance* instance = entity.get_mut<state::SquadInstance>();
		instance->hasDen = true;
		instance->denX = den.x;
		instance->denZ = den.z;
		instance->slot = slotIndex;
		instance->nest = nest;
		++context.copies[static_cast<std::size_t>(slotIndex)][squad.stringID];
		++context.report.squads;
		return entity;
	}

	// The exe's candidate weights of a slot (kinspect s71_static.md 3.4,
	// ccf1940): w - liveCopies x totalW / (zones x pop x amount), at least 0.
	// A template with w below that density is drawn only while none is live.
	std::vector<double> candidateWeights(const SlotDef& slot,
		const std::unordered_map<std::string, std::int64_t>& copies, double population) const
	{
		const double density = static_cast<double>(slot.homelessWeight)
			/ (slot.zones * population * slot.amount);
		std::vector<double> weights;
		for (const HomelessEntry& entry : slot.homeless)
		{
			const auto found = copies.find(entry.squad->stringID);
			const double live = found == copies.end() ? 0.0 : static_cast<double>(found->second);
			weights.push_back(std::max(0.0, static_cast<double>(entry.weight) - live * density));
		}
		return weights;
	}

	// False when every candidate is capped (the exe then spawns nothing this
	// pass: the hole stays and is retried on the next step).
	bool spawnHomeless(Context& context, std::int32_t slotIndex)
	{
		const SlotDef& slot = slots[static_cast<std::size_t>(slotIndex)];
		const std::vector<double> weights = candidateWeights(slot,
			context.copies[static_cast<std::size_t>(slotIndex)], context.population);
		double total = 0.0;
		for (const double weight : weights)
			total += weight;
		if (total <= 0.0)
			return false;
		double pick = context.rng.uniform01() * total;
		std::size_t chosen = weights.size() - 1;
		for (std::size_t i = 0; i < weights.size(); ++i)
		{
			if (weights[i] <= 0.0)
				continue;
			chosen = i;
			pick -= weights[i];
			if (pick < 0.0)
				break;
		}
		spawnOne(context, slotIndex, *slot.homeless[chosen].squad,
			ground.pick(slot.colour, context.rng), 0);
		++context.report.homelessSquads;
		return true;
	}

	bool spawnNest(Context& context, std::int32_t slotIndex)
	{
		const SlotDef& slot = slots[static_cast<std::size_t>(slotIndex)];
		const NestEntry& entry = draw(slot.nests, slot.nestWeight, context.rng);
		const state::GroundPoint den = ground.pick(slot.colour, context.rng);
		const std::uint32_t nest = context.wild.nextNest++;
		for (const kdata::Record* squad : entry.squads)
			spawnOne(context, slotIndex, *squad, den, nest);
		++context.report.nests;
		return true;
	}

	// One kind of one slot: queue the holes (or, at the start, fill them at once), then spawn what is due. The count is the exe's
	// sector arithmetic, in single-precision float (bq15_corpse_cleanup.md d, s79 section 1 of the quota): cur = |S2| + 0.5 |S3|
	// (intact squads and non-intact ones, squad_alive.hpp); a spawn happens only while target > cur, and the sector cap skips it when
	// target <= |S2|. `quota` is our rounded target (roundQuota), the nests pass every nest as S2.
	template <typename Spawn>
	void topUp(Context& context, std::vector<std::uint64_t>& due, std::int64_t quota,
		std::int64_t intact, std::int64_t nonIntact, std::uint64_t now, bool initial, std::uint64_t minHours,
		std::uint64_t maxHours, Spawn&& spawn)
	{
		const float target = static_cast<float>(quota);
		float cur = static_cast<float>(intact) + 0.5f * static_cast<float>(nonIntact);
		const float capIntact = static_cast<float>(intact);
		if (initial)
		{
			for (; target > cur && target > capIntact; cur += 1.0f)
			{
				if (!spawn())
					break;
			}
			return;
		}
		// One due entry per hole: ceil(target - cur) (a non-intact squad leaves half a hole, which still needs one spawn).
		const std::size_t holes = target > cur ? static_cast<std::size_t>(std::ceil(target - cur)) : 0;
		while (due.size() > holes)
			due.pop_back();
		while (due.size() < holes)
		{
			due.push_back(now + delay(context.rng, minHours, maxHours));
			++context.report.queued;
		}
		sortDue(due);
		std::size_t spawned = 0;
		while (spawned < due.size() && due[spawned] <= now && target > cur && target > capIntact)
		{
			if (!spawn())
				break;
			cur += 1.0f;
			++spawned;
		}
		due.erase(due.begin(), due.begin() + static_cast<std::ptrdiff_t>(spawned));
	}

	WildernessReport run(flecs::world& ecs, std::uint64_t now, bool initial)
	{
		state::WildernessState* saved = ecs.get_mut<state::WildernessState>();
		if (saved == nullptr || saved->slots.size() != slots.size())
			throw SaveError("the wilderness state does not match this game data ("
				+ std::to_string(saved == nullptr ? 0 : saved->slots.size()) + " slots saved, "
				+ std::to_string(slots.size()) + " in the data)");
		RngStream rng(saved->rng);
		Live counts = live(ecs);
		SquadBuilder builder(ecs, data, rng);
		Context context{ecs, builder, rng, {}, *saved, saved->population, {}, {}};
		context.copies.resize(slots.size());
		// The per-template density cap counts intact squads only (UNSURE: the exe's "loaded squad of that template" is S2 or S2 + S3): a
		// wiped squad whose corpses still lie must not block the replacement the float quota asks for (a slot whose templates are all at
		// their cap would keep its hole for ever).
		ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
		{
			if (squad.slot >= 0 && static_cast<std::size_t>(squad.slot) < slots.size() && squadVitals(ecs, entity, &data).intact)
				++context.copies[static_cast<std::size_t>(squad.slot)][squad.templateSid];
		});
		ecs.each([&](flecs::entity entity, const state::FactionState& faction)
		{
			context.factions.emplace(faction.templateSid, entity.id());
		});
		for (std::size_t i = 0; i < slots.size(); ++i)
		{
			const std::int32_t index = static_cast<std::int32_t>(i);
			state::WildSlotState& slotState = saved->slots[i];
			topUp(context, slotState.homelessDue, homelessQuota(slots[i], saved->population),
				counts.homelessIntact[i], counts.homeless[i] - counts.homelessIntact[i], now, initial, kHomelessRespawnMinHours,
				kHomelessRespawnMaxHours, [&] { return spawnHomeless(context, index); });
			topUp(context, slotState.nestDue, nestQuota(slots[i], saved->nests),
				counts.nests[i], 0, now, initial, kNestRespawnMinHours, kNestRespawnMaxHours,
				[&] { return spawnNest(context, index); });
		}
		saved->rng = rng.state();
		context.report.characters = builder.characters();
		return context.report;
	}

	const kdata::GameData& data;
	WildGround ground;
	std::vector<SlotDef> slots;
};

WildernessSpawner::WildernessSpawner(const kdata::GameData& gameData, std::string_view dataDir)
	: impl_(std::make_unique<Impl>(gameData, dataDir))
{
}

WildernessSpawner::~WildernessSpawner() = default;

std::size_t WildernessSpawner::slotCount() const noexcept
{
	return impl_->slots.size();
}

WildernessReport WildernessSpawner::seed(flecs::world& ecs, std::uint64_t worldSeed,
	double population, double nestMultiplier)
{
	state::WildernessState fresh;
	fresh.population = population;
	fresh.nests = nestMultiplier;
	fresh.rng = RngStream(streamSeed(worldSeed, kWildernessStream)).state();
	fresh.slots.resize(impl_->slots.size());
	ecs.set<state::WildernessState>(std::move(fresh));
	return impl_->run(ecs, ecs.get<state::Clock>()->tick, true);
}

WildernessReport WildernessSpawner::step(flecs::world& ecs, std::uint64_t now)
{
	return impl_->run(ecs, now, false);
}

std::vector<WildernessSlotCount> WildernessSpawner::counts(const flecs::world& ecs) const
{
	const state::WildernessState* saved = ecs.get<state::WildernessState>();
	const Impl::Live live = impl_->live(ecs);
	std::vector<WildernessSlotCount> result;
	// Homeless squads that are not intact (squad_alive.hpp), per slot: they count 0.5 in the quota (topUp).
	std::vector<std::int64_t> nonIntact(impl_->slots.size(), 0);
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		if (instance.slot >= 0 && static_cast<std::size_t>(instance.slot) < nonIntact.size() && instance.nest == 0
			&& !squadVitals(ecs, squad, &impl_->data).intact)
			++nonIntact[static_cast<std::size_t>(instance.slot)];
	});
	for (std::size_t i = 0; i < impl_->slots.size(); ++i)
	{
		WildernessSlotCount count;
		count.homelessNonIntact = nonIntact[i];
		count.group = impl_->slots[i].group;
		count.homelessQuota = impl_->homelessQuota(impl_->slots[i],
			saved != nullptr ? saved->population : 1.0);
		count.nestQuota = impl_->nestQuota(impl_->slots[i], saved != nullptr ? saved->nests : 1.0);
		count.homeless = live.homeless[i];
		count.nests = live.nests[i];
		if (saved != nullptr && i < saved->slots.size())
		{
			count.homelessPending = static_cast<std::int64_t>(saved->slots[i].homelessDue.size());
			count.nestsPending = static_cast<std::int64_t>(saved->slots[i].nestDue.size());
		}
		result.push_back(std::move(count));
	}
	return result;
}

flecs::entity WildernessSpawner::placeHomeless(flecs::world& ecs, std::string_view templateSid,
	double x, double z, std::size_t members) const
{
	state::WildernessState* saved = ecs.get_mut<state::WildernessState>();
	if (saved == nullptr || saved->slots.size() != impl_->slots.size())
		throw SaveError("placeHomeless needs the wilderness state of this game data");
	const kdata::Record* squad = impl_->data.find(std::string(templateSid));
	if (squad == nullptr || squad->type != kworld::itemTypeByName("SQUAD_TEMPLATE"))
		throw SaveError("placeHomeless: not a squad template: " + std::string(templateSid));
	const std::uint32_t colour = impl_->ground.colourAt(x, z);
	std::int32_t slotIndex = -1;
	for (std::size_t i = 0; i < impl_->slots.size(); ++i)
	{
		if (impl_->slots[i].colour == colour)
			slotIndex = static_cast<std::int32_t>(i);
	}
	if (slotIndex < 0)
		throw SaveError("placeHomeless: the point is in no wilderness slot");
	RngStream rng(saved->rng);
	SquadBuilder builder(ecs, impl_->data, rng);
	Impl::Context context{ecs, builder, rng, {}, *saved, saved->population, {}, {}};
	context.copies.resize(impl_->slots.size());
	ecs.each([&](flecs::entity entity, const state::FactionState& faction)
	{
		context.factions.emplace(faction.templateSid, entity.id());
	});
	for (int roll = 0; roll < 2000; ++roll)
	{
		const flecs::entity placed = impl_->spawnOne(context, slotIndex, *squad, state::GroundPoint{x, z}, 0);
		std::size_t size = 0;
		ecs.query_builder<const state::CharacterInstance>().with<state::MemberOf>(placed).build()
			.each([&](const state::CharacterInstance&) { ++size; });
		if (members == 0 || size == members)
		{
			saved->rng = rng.state();
			return placed;
		}
		// Not the size asked for: take it back out (and out of the copy count).
		ecs.delete_with<state::MemberOf>(placed.id());
		placed.destruct();
		--context.copies[static_cast<std::size_t>(slotIndex)][squad->stringID];
		--context.report.squads;
	}
	throw SaveError("placeHomeless: no " + std::string(templateSid) + " squad of " + std::to_string(members) + " members in 2000 rolls");
}

std::vector<WildernessSlotCensus> WildernessSpawner::census(const flecs::world& ecs) const
{
	const state::WildernessState* saved = ecs.get<state::WildernessState>();
	const double population = saved != nullptr ? saved->population : 1.0;
	std::vector<std::unordered_map<std::string, std::int64_t>> homeless(impl_->slots.size());
	ecs.each([&](const state::SquadInstance& squad)
	{
		if (squad.nest == 0 && squad.slot >= 0
			&& static_cast<std::size_t>(squad.slot) < impl_->slots.size())
			++homeless[static_cast<std::size_t>(squad.slot)][squad.templateSid];
	});
	std::vector<WildernessSlotCensus> result;
	for (std::size_t i = 0; i < impl_->slots.size(); ++i)
	{
		const SlotDef& slot = impl_->slots[i];
		WildernessSlotCensus census;
		census.group = slot.group;
		census.colour = slot.colour;
		census.zones = slot.zones;
		census.amount = slot.amount;
		census.totalWeight = slot.homelessWeight;
		census.quota = impl_->homelessQuota(slot, population);
		for (const HomelessEntry& entry : slot.homeless)
		{
			const auto found = homeless[i].find(entry.squad->stringID);
			census.templates.push_back(WildernessTemplateCount{entry.squad->stringID, entry.weight,
				found == homeless[i].end() ? 0 : found->second});
		}
		result.push_back(std::move(census));
	}
	return result;
}

} // namespace ksim
