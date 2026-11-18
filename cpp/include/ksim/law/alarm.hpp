#pragma once

// The town's AlarmManager (kinspect dt41_E sections 1-4; docs/reference/CRIME_STATIC.md "Memory/tags and alarm"). One registry per
// town: the exe's std::map<kind, list<Alarm*>> is a flat list here (the kind is a field; a 32-bit kind mask selects). The raise writer
// is 1409335e0, the expiry 14092bfe0, the selection 14092a810; the consumers are the 209 / 210 tick (Task_InvestigateAlarms, 140354260)
// and the "any alarm near" tests (AI::vf28 1405a0b50, 140599060; the goal state ALARMS_IN_THE_VICINITY of 209 / 210 / 283 / USE_BED).
//
// Saved from v110 (state::RunStates, OURS): the list per town, each alarm's remaining life and the id counter, so a load mid-alarm continues as the
// unbroken run (before v110 an alarm was dropped and an adopted 209 / 210 whose alarm was gone ended its goal).
// OURS where marked: the Alarm's raiser hand (+0x00 / +0x60 floor id) is the raiser's character id and faction; the +0x60 equality test
// (a floor / level id the sim has no model of) and building+0xc2 are not kept (UNSURE: see the selection's note).

#include <cstdint>
#include <map>
#include <vector>

namespace ksim::alarm
{

inline constexpr double kLifetime = 8.0; // Alarm+0x24 on create; a re-raise does not reset it
inline constexpr double kSearchRadius = 375.0; // DAT_1416c1590: the selection's radius, both AI callers and the 209 / 210 tick
inline constexpr double kUnusableFactor = 0.5; // DAT_14167e8d8: the radius factor when the record's building is unusable or unknown
inline constexpr double kCloseEnough = 100.0; // _DAT_14168dae4 = 10000.0, a squared distance: the 209 / 210 tick's "arrived"
inline constexpr double kStandOff = 30.0; // _DAT_141693f4c: the walk point's distance from the alarm
inline constexpr double kDwellSeconds = 10.0; // this+0x7c: the wait after arriving, stepped by the frame delta

// kind is the exe's CrimeEnum (0xf kidnapping; 0xb trespassing; the dialogue path writes the target's crime, char+0x148).
inline constexpr std::uint8_t kKindKidnapping = 15;

// What the selection asks of the record's building (the exe resolves the hand: 0 = no such object).
enum class BuildingState : std::uint8_t { Unknown, Usable, Unusable };

struct Alarm
{
	std::uint64_t id = 0; // OURS: the name a goal's chainTarget holds
	std::uint8_t kind = 0;
	std::uint64_t owner = 0; // +0x28 (the raiser's faction); selection passes 0 = any at every call site
	std::uint64_t raiser = 0; // +0x00 / +0x60: the raiser's character id
	std::uint64_t building = 0; // +0x40: the raiser's current building (0 = none)
	double x = 0.0; // +0x30
	double z = 0.0;
	double life = kLifetime; // +0x24
};

class Board
{
public:
	// 1409335e0: an existing alarm of this kind and owner is refreshed in place (building, raiser and position; NOT the lifetime),
	// else a new one is made with the full lifetime. Returns the alarm.
	const Alarm& raise(int town, std::uint8_t kind, double x, double z, std::uint64_t building, std::uint64_t owner, std::uint64_t raiser)
	{
		std::vector<Alarm>& list = towns_[town];
		for (Alarm& held : list)
		{
			if (held.kind != kind || held.owner != owner)
				continue;
			held.building = building;
			held.raiser = raiser;
			held.x = x;
			held.z = z;
			return held;
		}
		Alarm made;
		made.id = ++last_;
		made.kind = kind;
		made.owner = owner;
		made.raiser = raiser;
		made.building = building;
		made.x = x;
		made.z = z;
		list.push_back(made);
		return list.back();
	}

	// 14092bfe0: every record loses `dt`; those below 0 are removed.
	void tick(double dt)
	{
		for (auto it = towns_.begin(); it != towns_.end();)
		{
			std::vector<Alarm>& list = it->second;
			for (std::size_t i = 0; i < list.size();)
			{
				list[i].life -= dt;
				if (list[i].life < 0.0)
					list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
				else
					++i;
			}
			it = list.empty() ? towns_.erase(it) : std::next(it);
		}
	}

	// The save's two halves: every alarm with its town (in list order), and the way back (the counter included).
	template <typename Visit>
	void each(Visit&& visit) const
	{
		for (const auto& [town, list] : towns_)
			for (const Alarm& held : list)
				visit(town, held);
	}
	std::uint64_t last() const noexcept { return last_; }
	void clear() noexcept
	{
		towns_.clear();
		last_ = 0;
	}
	void restore(int town, const Alarm& held)
	{
		towns_[town].push_back(held);
	}
	void resume(std::uint64_t last) noexcept { last_ = last; } // the id counter, saved even with no alarm left

	const Alarm* find(std::uint64_t id) const noexcept
	{
		for (const auto& [town, list] : towns_)
			for (const Alarm& held : list)
				if (held.id == id)
					return &held;
		return nullptr;
	}

	// 14092c300: the investigator deletes the alarm after its dwell.
	bool erase(std::uint64_t id)
	{
		for (auto& [town, list] : towns_)
			for (std::size_t i = 0; i < list.size(); ++i)
				if (list[i].id == id)
				{
					list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
					return true;
				}
		return false;
	}

	bool empty() const noexcept { return towns_.empty(); }
	std::size_t size() const noexcept
	{
		std::size_t total = 0;
		for (const auto& [town, list] : towns_)
			total += list.size();
		return total;
	}

	// 14092a810: the nearest alarm of the town accepted for a searcher at (sx, sz) standing in `myBuilding`. `kinds` is the 32-bit
	// kind mask (0 = all), `owner` 0 = any. `state(building)` resolves a record's building hand. The record is skipped when its building
	// is not usable and the searcher is not in it ("I am not the one who raised it"); the radius is halved when the building is unusable
	// or unknown; accepted when the squared distance is below the radius squared; the nearest wins (the first of equals).
	// `accept(alarm)` is the caller's own filter (OURS: 210's allies-only).
	template <typename State, typename Accept>
	const Alarm* nearest(int town, std::uint32_t kinds, double radius, double sx, double sz, std::uint64_t owner, std::uint64_t myBuilding,
		State&& state, Accept&& accept) const
	{
		const auto at = towns_.find(town);
		if (at == towns_.end())
			return nullptr;
		const Alarm* best = nullptr;
		double bestSq = 0.0;
		for (const Alarm& held : at->second)
		{
			if (kinds != 0 && held.kind < 32 && (kinds & (1u << held.kind)) == 0)
				continue;
			if ((owner != 0 && held.owner != owner) || !accept(held))
				continue;
			const BuildingState building = held.building != 0 ? state(held.building) : BuildingState::Unknown;
			if (building == BuildingState::Unusable && held.building != myBuilding)
				continue;
			const double reach = building == BuildingState::Usable ? radius : radius * kUnusableFactor;
			const double dx = held.x - sx;
			const double dz = held.z - sz;
			const double sq = dx * dx + dz * dz;
			if (sq < reach * reach && (best == nullptr || sq < bestSq))
			{
				best = &held;
				bestSq = sq;
			}
		}
		return best;
	}

private:
	std::map<int, std::vector<Alarm>> towns_;
	std::uint64_t last_ = 0;
};

} // namespace ksim::alarm
