#pragma once

// Overlay loader. MOD_FORMAT.md "How the flags combine across files
// (load semantics)": each file is an overlay; records merge by stringID,
// fields key-by-key, references by (category, targetID), instances by
// instance ID.

#include "kdata/format.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {

class GameData
{
public:
	// Apply one file over the records loaded so far, in file order.
	void apply(const File& file);
	// Same result; takes the records instead of copying them.
	void apply(File&& file);

	// nullptr when absent (including when a later overlay removed it).
	const Record* find(std::string_view stringID) const;

	// Records of one itemType, in the stable merge order.
	std::vector<const Record*> byType(std::int32_t type) const;

	// Stable merge order: first definition wins its position; later
	// overlays modify in place and new records are appended.
	const std::vector<Record>& records() const noexcept { return records_; }
	std::size_t size() const noexcept { return records_.size(); }

private:
	std::vector<Record> records_;
	struct StringHash
	{
		using is_transparent = void;
		std::size_t operator()(std::string_view text) const noexcept { return std::hash<std::string_view>()(text); }
	};
	std::unordered_map<std::string, std::size_t, StringHash, std::equal_to<>> index_;

	template <typename F>
	void applyImpl(F& file);

	void compact(const std::vector<char>& dead);
};

} // namespace kdata
