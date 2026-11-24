#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace kdata {

namespace {

bool recordIsRemoved(const Record& record)
{
	// MOD_FORMAT.md "Record": a bool REMOVED = true marks a deleted record.
	for (const auto& field : record.bools)
		if (field.key == "REMOVED" && field.value)
			return true;
	return false;
}

// Overlay identity of one entry: a domain tag plus either a number or bytes. No allocation, so large lists
// can be indexed by hash (kenshi-k16.15.46); `sv` points into the entry, so the target is reserved before indexing.
struct MergeKey
{
	std::uint8_t kind = 0;
	std::int32_t num = 0;
	std::string_view sv;
	bool operator==(const MergeKey& other) const noexcept
	{
		return kind == other.kind && num == other.num && sv == other.sv;
	}
};

struct MergeKeyHash
{
	std::size_t operator()(const MergeKey& key) const noexcept
	{
		return std::hash<std::string_view>()(key.sv) * 31u + key.kind * 7u + static_cast<std::size_t>(key.num);
	}
};

constexpr std::size_t kIndexedMergeMin = 24;

// Merge `source` into `target` by key: replace the first entry with the same key, else append; `removal` entries
// delete the first match. Same result as a linear first-match scan per incoming entry; a hash index replaces the scan
// on long lists (unique target keys only, else the scan runs).
template <typename T, typename KeyOf, typename Removal>
void mergeKeyed(std::vector<T>& target, const std::vector<T>& source, KeyOf keyOf, Removal removal)
{
	if (source.empty())
		return;
	bool indexed = target.size() >= kIndexedMergeMin;
	std::unordered_map<MergeKey, std::size_t, MergeKeyHash> index;
	if (indexed)
	{
		target.reserve(target.size() + source.size());
		index.reserve((target.size() + source.size()) * 2);
		for (std::size_t i = 0; i < target.size() && indexed; ++i)
			indexed = index.emplace(keyOf(target[i]), i).second;
	}
	if (!indexed)
	{
		for (const auto& incoming : source)
		{
			const MergeKey key = keyOf(incoming);
			auto existing = std::find_if(target.begin(), target.end(), [&](const T& entry) { return keyOf(entry) == key; });
			if (removal(incoming))
			{
				if (existing != target.end())
					target.erase(existing);
			}
			else if (existing != target.end())
				*existing = incoming;
			else
				target.push_back(incoming);
		}
		return;
	}
	std::vector<char> dead(target.size(), 0);
	bool anyDead = false;
	for (const auto& incoming : source)
	{
		auto found = index.find(keyOf(incoming));
		if (removal(incoming))
		{
			if (found != index.end())
			{
				dead[found->second] = 1;
				anyDead = true;
				index.erase(found);
			}
		}
		else if (found != index.end())
		{
			const std::size_t at = found->second;
			index.erase(found);
			target[at] = incoming;
			index.emplace(keyOf(target[at]), at);
		} else {
			target.push_back(incoming);
			dead.push_back(0);
			index.emplace(keyOf(target.back()), target.size() - 1);
		}
	}
	if (anyDead)
	{
		std::size_t kept = 0;
		for (std::size_t i = 0; i < target.size(); ++i)
			if (!dead[i])
			{
				if (kept != i)
					target[kept] = std::move(target[i]);
				++kept;
			}
		target.erase(target.begin() + static_cast<std::ptrdiff_t>(kept), target.end());
	}
}

template <typename Field>
void mergeFields(std::vector<Field>& target, const std::vector<Field>& source)
{
	mergeKeyed(target, source, [](const Field& field) { return MergeKey{ 0, 0, field.key }; },
		[](const Field&) { return false; });
}

MergeKey referenceKey(const RefEntry& entry)
{
	// v8+ identity is the target stringID. Pre-v8 entries are opaque; two
	// entries are only equal if all eight bytes match.
	// UNSURE: MOD_FORMAT.md does not define overlay identity for the
	// opaque pre-v8 form; raw bytes are the simplest choice.
	if (entry.opaqueEntry)
		return MergeKey{ 1, 0, std::string_view(reinterpret_cast<const char*>(entry.opaque.data()), entry.opaque.size()) };
	return MergeKey{ 0, 0, entry.targetID };
}

bool referenceIsRemoval(const RefEntry& entry)
{
	// MOD_FORMAT.md "Record": v0=v1=v2=0x7FFFFFFF means the reference is
	// removed. v8/v9 only carry v0, so there it must stand alone.
	// UNSURE: the spec only states the three-value form; treating a lone
	// 0x7FFFFFFF in v8/v9 as a removal is the simplest extension.
	if (entry.v0 != kReferenceRemoved)
		return false;
	if (!entry.hasV1V2)
		return true;
	return entry.v1 == kReferenceRemoved && entry.v2 == kReferenceRemoved;
}

RefCategory* findCategory(std::vector<RefCategory>& categories, std::string_view name)
{
	for (auto& category : categories)
		if (category.category == name)
			return &category;
	return nullptr;
}

void mergeReferences(std::vector<RefCategory>& target,
	const std::vector<RefCategory>& source)
{
	for (const auto& incomingCategory : source)
	{
		auto category = findCategory(target, incomingCategory.category);
		if (category == nullptr)
		{
			// A removal of something absent is a no-op and must not
			// create an empty category.
			if (std::all_of(incomingCategory.entries.begin(), incomingCategory.entries.end(), referenceIsRemoval))
				continue;
			target.push_back(RefCategory{ incomingCategory.category, {} });
			category = &target.back();
		}
		mergeKeyed(category->entries, incomingCategory.entries, referenceKey, referenceIsRemoval);
	}
}

MergeKey instanceKey(const Instance& instance)
{
	// UNSURE: the spec keys instances by "instance ID" but does not say how
	// to compare the v15+ string form with the older i32 form. Keeping the
	// two domains apart (a record overlay normally uses one form).
	return instance.idIsString ? MergeKey{ 0, 0, instance.id } : MergeKey{ 1, instance.idInt, {} };
}

void mergeInstances(std::vector<Instance>& target, const std::vector<Instance>& source,
	std::int32_t fileVersion)
{
	// MOD_FORMAT.md "Record": an empty targetStringID deletes the
	// instance. Before v8 there is no target field, so no deletion form.
	// UNSURE: deletion is only meaningful for v8+; assumed here.
	mergeKeyed(target, source, instanceKey,
		[fileVersion](const Instance& incoming) { return fileVersion >= 8 && incoming.targetStringID.empty(); });
}

void mergeRecord(Record& target, const Record& source, std::int32_t fileVersion)
{
	// MOD_FORMAT.md "Record flags": a modifying record renames only when
	// RENAMED is set; bit 1 only counts when bit 0 is set.
	// UNSURE: files before v15 have no flags word. There the simplest rule
	// is to take a non-empty incoming name.
	bool renames = source.hasFlags ? (source.flags & 3) == 3 : !source.name.empty();
	if (renames && !source.name.empty())
		target.name = source.name;
	// UNSURE: the merged record's flags are not defined by the load section;
	// keep the newest word when one is present.
	if (source.hasFlags)
		target.flags = source.flags;
	// UNSURE: identity is the stringID, so the type/id of an overlay record
	// are taken as already established by the first definition; they are
	// left untouched here.

	// UNSURE: each section is merged independently. The spec's single
	// modData dictionary means a key could in principle collide across
	// sections ("a string value does not overwrite a key already holding a
	// non-string"); no shipped record has such a collision.
	mergeFields(target.bools, source.bools);
	mergeFields(target.floats, source.floats);
	mergeFields(target.ints, source.ints);
	mergeFields(target.vec3s, source.vec3s);
	mergeFields(target.vec4s, source.vec4s);
	mergeFields(target.strings, source.strings);
	mergeFields(target.files, source.files);
	mergeReferences(target.references, source.references);
	mergeInstances(target.instances, source.instances, fileVersion);

	// UNSURE: v11..v14 tag tables are per-record application filters, not
	// data; the load rule for them is not part of the required overlay set.
	// The target keeps its existing table.
}

} // namespace

void GameData::apply(const File& file)
{
	applyImpl(file);
}

void GameData::apply(File&& file)
{
	applyImpl(file);
}

template <typename F>
void GameData::applyImpl(F& file)
{
	std::vector<char> dead;
	// UNSURE: the v17 header `deletes` block is not mentioned by the load
	// semantics section; it is ignored. No shipped file exercises it here.
	for (auto& record : file.records)
	{
		if (recordIsRemoved(record))
		{
			auto existing = index_.find(record.stringID);
			if (existing != index_.end())
			{
				// Removed in place and compacted once after the file: a per-removal erase + index rebuild is O(n) each.
				dead.resize(records_.size(), 0);
				dead[existing->second] = 1;
				index_.erase(existing);
			}
			continue;
		}

		auto existing = index_.find(record.stringID);
		if (existing == index_.end())
		{
			// UNSURE: the editor states for MODIFIED-on-missing
			// (ItemAlreadyDefined/MISSING) are not part of the required
			// overlay rules; an unknown stringID is always inserted.
			index_.emplace(record.stringID, records_.size());
			if constexpr (std::is_const_v<F>)
				records_.push_back(record);
			else
				records_.push_back(std::move(record));
		}
		else
		{
			mergeRecord(records_[existing->second], record, file.version);
		}
	}
	if (std::find(dead.begin(), dead.end(), 1) != dead.end())
		compact(dead);
}

const Record* GameData::find(std::string_view stringID) const
{
	auto existing = index_.find(stringID);
	if (existing == index_.end())
		return nullptr;
	return &records_[existing->second];
}

std::vector<const Record*> GameData::byType(std::int32_t type) const
{
	std::vector<const Record*> matches;
	for (const auto& record : records_)
		if (record.type == type)
			matches.push_back(&record);
	return matches;
}

void GameData::compact(const std::vector<char>& dead)
{
	std::size_t kept = 0;
	for (std::size_t i = 0; i < records_.size(); ++i)
		if (i >= dead.size() || !dead[i])
		{
			if (kept != i)
				records_[kept] = std::move(records_[i]);
			++kept;
		}
	records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(kept), records_.end());
	index_.clear();
	for (std::size_t i = 0; i < records_.size(); ++i)
		index_.emplace(records_[i].stringID, i);
}

} // namespace kdata
