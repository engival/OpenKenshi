#include "ksim/nav/doors.hpp"

#include "ksim/ai/ai.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/world/interact.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>

namespace ksim {

namespace
{

// UNSURE: gate hp is "per gate from data" in the original; no hp field was found in the data. The measured house door had 16.
constexpr double kGateHp = 16.0;

double distance(double ax, double az, double bx, double bz)
{
	return std::hypot(ax - bx, az - bz);
}

} // namespace

std::shared_ptr<const DoorData> DoorData::fromWorld(const World& world)
{
	auto data = std::make_shared<DoorData>();
	data->sites = world.doorSites();
	for (const auto& [sid, prefab] : world.prefabs())
	{
		const tmpl::Town* town = prefab.get<tmpl::Town>();
		if (town != nullptr && !town->isPublic)
			data->privateTowns.insert(sid);
	}
	return data;
}

DoorSystem::DoorSystem(std::shared_ptr<const DoorData> data,
	std::shared_ptr<const FactionBook> factions, DoorTuning tuning)
	: data_(std::move(data))
	, factions_(std::move(factions))
	, tuning_(tuning)
{
}

void DoorSystem::bind(flecs::world& ecs, const std::vector<TownSite>& towns)
{
	ecs_ = &ecs;
	if (boundTowns_ == towns.size())
		return;
	boundTowns_ = towns.size();
	gateTowns_.clear();
	gates_.clear();
	applied_ = false;
	// Each gate goes to the nearest town centre, no cutoff (gatebind.hpp, shared with the spawn rule); ties keep the first (towns is in Id order).
	std::vector<std::pair<std::uint64_t, std::size_t>> owned; // (door key, index in towns)
	std::vector<bool> used(towns.size(), false);
	std::vector<const DoorSite*> ofGate;
	for (const DoorSite& site : data_->sites)
	{
		if (!site.gate)
			continue;
		const std::size_t best = nearestTownIndex(site.x, site.z, towns.size(),
			[&](std::size_t i) { return std::pair<double, double>(towns[i].x, towns[i].z); });
		if (best == towns.size())
			continue;
		used[best] = true;
		owned.emplace_back(site.key, best);
		ofGate.push_back(&site);
	}
	std::vector<std::size_t> compact(towns.size(), 0);
	for (std::size_t i = 0; i < towns.size(); ++i)
	{
		if (!used[i])
			continue;
		compact[i] = gateTowns_.size();
		gateTowns_.push_back(GateTown{towns[i].entity, towns[i].x, towns[i].z});
	}
	for (std::size_t i = 0; i < owned.size(); ++i)
		gates_.push_back(Gate{owned[i].first, compact[owned[i].second], ofGate[i]->alwaysLocks, ofGate[i]->moveSpeed,
			ofGate[i]->ownerFaction});
	std::sort(gates_.begin(), gates_.end(),
		[](const Gate& a, const Gate& b) { return a.key < b.key; });
	stats_.gates = gates_.size();
	stats_.gateTowns = gateTowns_.size();
}

const DoorSystem::GateTown* DoorSystem::nearest(double x, double z, double reach) const
{
	const GateTown* best = nullptr;
	double bestDistance = reach;
	for (const GateTown& town : gateTowns_)
	{
		const double far = distance(x, z, town.x, town.z);
		if (far < bestDistance)
		{
			bestDistance = far;
			best = &town;
		}
	}
	return best;
}

flecs::entity_t DoorSystem::townOf(std::uint64_t doorKey) const
{
	const auto found = std::lower_bound(gates_.begin(), gates_.end(), doorKey,
		[](const Gate& gate, std::uint64_t key) { return gate.key < key; });
	return found != gates_.end() && found->key == doorKey ? gateTowns_[found->town].entity : 0;
}

DoorClass DoorSystem::classOf(flecs::entity_t faction, flecs::entity_t owner) const
{
	if (owner == 0 || ecs_ == nullptr)
		return DoorClass::Neutral;
	if (faction == 0)
		return DoorClass::Hostile;
	if (faction == owner)
		return DoorClass::Resident;
	const flecs::entity keeper(ecs_->c_ptr(), owner);
	double feeling = 0.0;
	if (const state::Relation* relation = keeper.get<state::Relation>(faction))
		feeling = relation->value;
	else if (factions_ != nullptr)
	{
		if (const state::FactionState* state = keeper.get<state::FactionState>())
			if (const FactionInfo* info = factions_->faction(state->templateSid))
			{
				feeling = info->defaultRelation;
				if (const state::FactionState* other = flecs::entity(ecs_->c_ptr(), faction).get<state::FactionState>())
					if (const FactionInfo* theirs = factions_->faction(other->templateSid))
						feeling = std::min(feeling, static_cast<double>(theirs->defaultRelation));
			}
	}
	if (feeling >= tuning_.allyRelation)
		return DoorClass::Ally;
	if (feeling <= tuning_.hostileRelation)
		return DoorClass::Hostile;
	return DoorClass::Neutral;
}

GateBash DoorSystem::bashing(flecs::entity_t faction, double toX, double toZ) const
{
	(void)faction; // a gate refuses nobody by who they are (doors.hpp)
	if (!tuning_.enabled || ecs_ == nullptr)
		return {};
	const GateTown* target = nearest(toX, toZ, tuning_.townReach);
	if (target == nullptr)
		return {};
	// The first gate (lowest key) of the town that is locked.
	for (const Gate& gate : gates_)
	{
		if (gateTowns_[gate.town].entity != target->entity)
			continue;
		const knav::DoorSetting setting = book_.get(gate.key);
		if (setting.state == knav::DoorState::Locked)
		{
			const DoorSite* site = nullptr;
			for (const DoorSite& candidate : data_->sites)
				if (candidate.key == gate.key)
					site = &candidate;
			GateBash bash;
			bash.key = gate.key;
			// One basher, unarmed: 0.3 hp a hit, one hit per 2.7 s. UNSURE: the squad's size and weapons are not read here, and
			// gate hp is not in the data (the 16 hp of the measured house door is used).
			bash.seconds = std::ceil(kGateHp / bashDamage(0.0, 0.0)) * kBashInterval;
			bash.x = site != nullptr ? site->x : target->x;
			bash.z = site != nullptr ? site->z : target->z;
			return bash;
		}
	}
	return {};
}

GateBash DoorSystem::bashFor(flecs::entity_t faction, std::uint64_t key) const
{
	(void)faction;
	if (!tuning_.enabled || ecs_ == nullptr)
		return {};
	const auto found = std::lower_bound(gates_.begin(), gates_.end(), key, [](const Gate& gate, std::uint64_t k) { return gate.key < k; });
	if (found == gates_.end() || found->key != key)
		return {};
	const GateTown& gateTown = gateTowns_[found->town];
	if (book_.get(key).state != knav::DoorState::Locked)
		return {}; // open (or shut but passable): nothing to break here
	const DoorSite* site = nullptr;
	for (const DoorSite& candidate : data_->sites)
		if (candidate.key == key)
			site = &candidate;
	GateBash bash;
	bash.key = key;
	// One basher, unarmed: 0.3 hp a hit, one hit per 2.7 s. UNSURE: the squad's size and weapons are not read here, and gate hp is not
	// in the data (the 16 hp of the measured house door is used).
	bash.seconds = std::ceil(kGateHp / bashDamage(0.0, 0.0)) * kBashInterval;
	bash.x = site != nullptr ? site->x : gateTown.x;
	bash.z = site != nullptr ? site->z : gateTown.z;
	return bash;
}

void DoorSystem::breakGate(flecs::world& ecs, std::uint64_t key)
{
	state::DoorOverrides overrides;
	if (const state::DoorOverrides* current = ecs.get<state::DoorOverrides>())
		overrides = *current;
	auto& entries = overrides.entries;
	const auto at = std::lower_bound(entries.begin(), entries.end(), key,
		[](const state::DoorOverride& entry, std::uint64_t k) { return entry.key < k; });
	if (at != entries.end() && at->key == key)
		at->state = 3;
	else
		entries.insert(at, state::DoorOverride{key, 3});
	ecs.set<state::DoorOverrides>(std::move(overrides));
	++stats_.gatesBroken;
}

knav::DoorBook DoorSystem::build(flecs::world& ecs) const
{
	knav::DoorBook book;
	for (const Gate& gate : gates_)
	{
		const flecs::entity town(ecs.c_ptr(), gateTowns_[gate.town].entity);
		const state::TownState* state = town.get<state::TownState>();
		if (state == nullptr || state->owner.id == 0)
			continue;
	}
	if (const state::DoorOverrides* overrides = ecs.get<state::DoorOverrides>())
		for (const state::DoorOverride& entry : overrides->entries)
		{
			if (entry.state == 3)
			{
				// Broken: open for ever, the lock is gone.
				book.setState(entry.key, knav::DoorState::Open);
			} else {
				book.setState(entry.key, static_cast<knav::DoorState>(entry.state));
			}
		}
	return book;
}

void DoorSystem::publish(knav::DoorBook&& book, const Navigator* navigator)
{
	if (applied_ && book == book_)
		return;
	book_ = std::move(book);
	applied_ = true;
	stats_.doorsRestricted = book_.settings().size();
	++stats_.booksApplied;
	if (navigator != nullptr)
		navigator->applyDoors(book_);
}

void DoorSystem::update(flecs::world& ecs, std::uint64_t tick, const std::vector<TownSite>& towns,
	const Navigator* navigator)
{
	(void)tick;
	bind(ecs, towns);
	if (!tuning_.enabled)
		return;
	// Towns a hostile raid is under way to, and by whom.
	std::vector<std::pair<flecs::entity_t, flecs::entity_t>> targeted;
	ecs.each([&](flecs::entity raider, const state::FactionAI& ai)
	{
		for (const state::Raid& raid : ai.raids)
		{
			if (!raid.started)
				continue;
			if (factions_ != nullptr)
			{
				const CampaignInfo* campaign = factions_->campaign(raid.campaign);
				if (campaign != nullptr && !campaign->hostile)
					continue;
			}
			targeted.emplace_back(raid.target.id, raider.id());
		}
	});
	stats_.raidHours += targeted.size();
	for (const auto& [target, raider] : targeted)
		for (const GateTown& gateTown : gateTowns_)
			stats_.raidHoursAtGates += gateTown.entity == target ? 1 : 0;
	overrides_.clear();
	if (const state::DoorOverrides* overrides = ecs.get<state::DoorOverrides>())
		overrides_ = overrides->entries;
	publish(build(ecs), navigator);
}

void DoorSystem::crewsReopen(flecs::world& ecs, std::uint64_t tick)
{
	if (ecs.count<state::GateCrew>() == 0) // what the full pass leaves with no crew, without the uncached ecs.each()
	{
		stats_.gatesCrewed = 0;
		shutSince_.clear();
		return;
	}
	const state::DoorOverrides* current = ecs.get<state::DoorOverrides>();
	// The gates with a crew, and a faction of one of them (the lowest squad Id: the squads are walked in Id order).
	std::vector<std::tuple<std::uint64_t, std::uint64_t, flecs::entity_t>> crews; // (squad Id, door key, the squad's faction)
	ecs.each([&](const state::Id& id, const state::SquadInstance& squad, const state::GateCrew& crew)
	{
		crews.emplace_back(id.value, crew.gate, squad.faction.id);
	});
	std::sort(crews.begin(), crews.end());
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> byKey; // (door key, a crew faction), ascending by key
	for (const auto& [id, key, faction] : crews)
		byKey.emplace_back(key, faction);
	std::stable_sort(byKey.begin(), byKey.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	stats_.gatesCrewed = 0;
	for (std::size_t i = 0; i < byKey.size(); ++i)
		if (i == 0 || byKey[i].first != byKey[i - 1].first)
			++stats_.gatesCrewed;
	if (current == nullptr || byKey.empty())
	{
		shutSince_.clear();
		return;
	}
	std::vector<std::uint64_t> reopened;
	std::unordered_map<std::uint64_t, std::uint64_t> still;
	for (const state::DoorOverride& entry : current->entries)
	{
		// Open (0) is already what the crew wants; broken (3) is for ever.
		if (entry.state == 0 || entry.state > 2)
			continue;
		const auto crew = std::lower_bound(byKey.begin(), byKey.end(), entry.key,
			[](const auto& pair, std::uint64_t key) { return pair.first < key; });
		const auto gate = std::lower_bound(gates_.begin(), gates_.end(), entry.key,
			[](const Gate& g, std::uint64_t key) { return g.key < key; });
		if (crew == byKey.end() || crew->first != entry.key || gate == gates_.end() || gate->key != entry.key)
			continue;
		const auto started = shutSince_.find(entry.key);
		const std::uint64_t since = started != shutSince_.end() ? started->second : tick;
		still.emplace(entry.key, since);
		// A locked door needs UNLOCK first, which only a member of the door's faction plans (isObjectMine); the others wait.
		double seconds = tuning_.crewOpenFraction / std::max(gate->moveSpeed, 1.0e-6);
		if (entry.state == 2)
		{
			const flecs::entity owner(ecs.c_ptr(), gateTowns_[gate->town].entity);
			const state::TownState* town = owner.get<state::TownState>();
			bool mine = false;
			if (crew->second != 0)
			{
				const flecs::entity crewFaction(ecs.c_ptr(), crew->second);
				if (!gate->ownerFaction.empty())
				{
					const state::FactionState* faction = crewFaction.get<state::FactionState>();
					mine = faction != nullptr && faction->templateSid == gate->ownerFaction;
				} else {
					mine = town != nullptr && town->owner.id == crew->second;
				}
			}
			if (!mine)
				continue;
			seconds += tuning_.crewUnlockSeconds;
		}
		if (ticksSince(tick, since) >= static_cast<std::uint64_t>(std::ceil(seconds / kTickSeconds)))
			reopened.push_back(entry.key);
	}
	shutSince_ = std::move(still);
	if (reopened.empty())
		return;
	state::DoorOverrides overrides = *current;
	std::erase_if(overrides.entries, [&](const state::DoorOverride& entry)
	{
		return std::find(reopened.begin(), reopened.end(), entry.key) != reopened.end();
	});
	for (const std::uint64_t key : reopened)
		shutSince_.erase(key);
	stats_.crewReopens += reopened.size();
	if (overrides.entries.empty())
		ecs.remove<state::DoorOverrides>();
	else
		ecs.set<state::DoorOverrides>(std::move(overrides));
}

void DoorSystem::sync(flecs::world& ecs, std::uint64_t tick, const std::vector<TownSite>& towns,
	const Navigator* navigator)
{
	bind(ecs, towns);
	if (!tuning_.enabled)
		return;
	crewsReopen(ecs, tick);
	static const std::vector<state::DoorOverride> none;
	const state::DoorOverrides* overrides = ecs.get<state::DoorOverrides>();
	const std::vector<state::DoorOverride>& wanted = overrides != nullptr ? overrides->entries : none;
	if (applied_ && wanted == overrides_)
		return;
	overrides_ = wanted;
	publish(build(ecs), navigator);
}

} // namespace ksim
