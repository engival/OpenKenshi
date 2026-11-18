#pragma once

// Research state from the FCS data (kenshi-h2v.17.7, CRAFTING_SLICE 2.3, 3, 9.2; kinspect research25 1.3, craft27 3.2): from a faction's completed
// RESEARCH sids, derived and never saved, the global tech level, every record's state (completed / listable / locked, with the reason) and the
// unlocked set of each `enable *` kind. Only the records' own fields are read (`enable *`, `requirements`, `level`, `blueprint only`,
// `is level upgrade`); no record name is in the code. The bench level is the caller's input (no bench exists yet): `Row::level` against it
// says whether a listable row can be queued.

#include "kdata/gamedata.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ksim::research {

// The RESEARCH `enable *` ref lists, in the order the snapshot keeps its sets.
enum class Kind : std::uint8_t
{
	Item,
	WeaponType,
	WeaponModel,
	Armour,
	Crossbow,
	Backpack,
	Robotics,
	Buildings,
};
inline constexpr std::size_t kKindCount = 8;

// The RESEARCH field of a kind ("enable item", ...).
std::string_view kindField(Kind kind) noexcept;

enum class State : std::uint8_t
{
	Completed, // finished, or silently finished: everything it enables is already unlocked (9.2)
	Listable, // listed on the research page
	Locked, // not listed; Row::reason says why
};

enum class Reason : std::uint8_t
{
	None,
	Requirement, // an unfinished requirement is not itself directly researchable
	TechLevel, // level above the faction's tech level
	BlueprintOnly, // the record, or an unfinished requirement, is `blueprint only`
};

struct Row
{
	std::string sid;
	State state = State::Locked;
	Reason reason = Reason::None;
	int level = 0; // RESEARCH `level`: a listable row is queueable when it is <= the best research bench's level
};

struct Snapshot
{
	int techLevel = 1; // 1, or `level + 1` of the highest finished `is level upgrade` record
	std::vector<std::string> completed; // the faction's list, then the silently finished records (record order)
	std::vector<Row> rows; // every live RESEARCH record, in record order
	// Per kind, the unlocked refs in the order the finished records enable them (the faction's completion order).
	std::array<std::vector<std::string>, kKindCount> unlocked;

	bool isUnlocked(Kind kind, const std::string& sid) const;
	const Row* row(const std::string& sid) const;

private:
	friend class Book;
	std::array<std::unordered_set<std::string>, kKindCount> unlockedSet_;
	std::unordered_map<std::string, std::size_t> rowOf_;
};

// The RESEARCH records of a game data, read once. Holds the data by reference: it must outlive the book.
class Book
{
public:
	explicit Book(const kdata::GameData& data);

	// The state for a faction whose completed research is `completed` (sids in completion order; unknown ones are kept as given and enable
	// nothing). Recompute when the list changes.
	Snapshot evaluate(const std::vector<std::string>& completed) const;

private:
	struct Entry
	{
		std::string sid;
		int level = 0;
		bool blueprintOnly = false;
		bool levelUpgrade = false;
		std::vector<std::string> requirements;
		std::array<std::vector<std::string>, kKindCount> enables;
	};
	const kdata::GameData& data_;
	std::vector<Entry> entries_;
	std::unordered_map<std::string, std::size_t> index_;
};

} // namespace ksim::research
