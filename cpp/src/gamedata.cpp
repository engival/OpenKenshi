#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

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

template <typename Field>
void mergeFields(std::vector<Field>& target, const std::vector<Field>& source)
{
	for (const auto& incoming : source)
	{
		auto existing = std::find_if(target.begin(), target.end(),
			[&](const Field& field) { return field.key == incoming.key; });
		if (existing == target.end())
			target.push_back(incoming);
		else
			*existing = incoming;
	}
}

std::string referenceKey(const RefEntry& entry)
{
	// v8+ identity is the target stringID. Pre-v8 entries are opaque; two
	// entries are only equal if all eight bytes match.
	// UNSURE: MOD_FORMAT.md does not define overlay identity for the
	// opaque pre-v8 form; raw bytes are the simplest choice.
	if (entry.opaqueEntry)
	{
		std::string key;
		key.reserve(9);
		key.push_back('\0');
		for (auto byte : entry.opaque)
			key.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		return key;
	}
	return entry.targetID;
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
		for (const auto& incoming : incomingCategory.entries)
		{
			auto key = referenceKey(incoming);
			bool removal = referenceIsRemoval(incoming);
			if (category == nullptr)
			{
				// A removal of something absent is a no-op and must not
				// create an empty category.
				if (removal)
					continue;
				target.push_back(RefCategory{ incomingCategory.category, {} });
				category = &target.back();
			}
			auto existing = std::find_if(category->entries.begin(),
				category->entries.end(),
				[&](const RefEntry& entry) { return referenceKey(entry) == key; });
			if (removal)
			{
				if (existing != category->entries.end())
					category->entries.erase(existing);
			}
			else if (existing != category->entries.end())
			{
				*existing = incoming;
			}
			else
			{
				category->entries.push_back(incoming);
			}
		}
	}
}

std::string instanceKey(const Instance& instance)
{
	// UNSURE: the spec keys instances by "instance ID" but does not say how
	// to compare the v15+ string form with the older i32 form. Prefixing
	// keeps the two domains apart (a record overlay normally uses one form).
	return instance.idIsString
		? "s:" + instance.id
		: "i:" + std::to_string(instance.idInt);
}

void mergeInstances(std::vector<Instance>& target, const std::vector<Instance>& source,
	std::int32_t fileVersion)
{
	for (const auto& incoming : source)
	{
		auto key = instanceKey(incoming);
		auto existing = std::find_if(target.begin(), target.end(),
			[&](const Instance& instance) { return instanceKey(instance) == key; });
		// MOD_FORMAT.md "Record": an empty targetStringID deletes the
		// instance. Before v8 there is no target field, so no deletion form.
		// UNSURE: deletion is only meaningful for v8+; assumed here.
		bool deletion = fileVersion >= 8 && incoming.targetStringID.empty();
		if (deletion)
		{
			if (existing != target.end())
				target.erase(existing);
		}
		else if (existing != target.end())
		{
			*existing = incoming;
		}
		else
		{
			target.push_back(incoming);
		}
	}
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
	// UNSURE: the v17 header `deletes` block is not mentioned by the load
	// semantics section; it is ignored. No shipped file exercises it here.
	for (const auto& record : file.records)
	{
		if (recordIsRemoved(record))
		{
			auto existing = index_.find(record.stringID);
			if (existing != index_.end())
				removeAt(existing->second);
			continue;
		}

		auto existing = index_.find(record.stringID);
		if (existing == index_.end())
		{
			// UNSURE: the editor states for MODIFIED-on-missing
			// (ItemAlreadyDefined/MISSING) are not part of the required
			// overlay rules; an unknown stringID is always inserted.
			index_.emplace(record.stringID, records_.size());
			records_.push_back(record);
		}
		else
		{
			mergeRecord(records_[existing->second], record, file.version);
		}
	}
}

const Record* GameData::find(std::string_view stringID) const
{
	auto existing = index_.find(std::string(stringID));
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

void GameData::removeAt(std::size_t position)
{
	records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(position));
	// Rebuild the index: later positions shift down. Removals are rare
	// (at most a few hundred in the shipped data).
	index_.clear();
	for (std::size_t i = 0; i < records_.size(); ++i)
		index_.emplace(records_[i].stringID, i);
}

} // namespace kdata
