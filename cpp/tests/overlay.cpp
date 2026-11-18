// Hand-built overlays covering each GameData merge rule:
// field override, reference removal, instance replacement/deletion, REMOVED.

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "test_util.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

kdata::File makeFile(std::int32_t version)
{
	kdata::File file;
	file.version = version;
	file.hasHeader = version >= 16;
	file.header.version = 1;
	file.lastID = 100;
	return file;
}

kdata::Instance makeInstance(std::string id, std::string target)
{
	kdata::Instance instance;
	instance.id = std::move(id);
	instance.idIsString = true;
	instance.targetStringID = std::move(target);
	return instance;
}

kdata::RefEntry makeRef(std::string target, std::int32_t v0)
{
	kdata::RefEntry entry;
	entry.targetID = std::move(target);
	entry.v0 = v0;
	entry.hasV1V2 = true;
	return entry;
}

kdata::RefEntry makeRemovedRef(std::string target)
{
	auto entry = makeRef(std::move(target), kdata::kReferenceRemoved);
	entry.v1 = kdata::kReferenceRemoved;
	entry.v2 = kdata::kReferenceRemoved;
	return entry;
}

kdata::File serialized(const kdata::File& file)
{
	auto bytes = kdata::write(file);
	return kdata::parse(bytes, "test.mod");
}

const kdata::FieldInt* findInt(const kdata::Record& record, std::string_view key)
{
	for (const auto& field : record.ints)
		if (field.key == key)
			return &field;
	return nullptr;
}

const kdata::RefCategory* findCategory(const kdata::Record& record, std::string_view name)
{
	for (const auto& category : record.references)
		if (category.category == name)
			return &category;
	return nullptr;
}

const kdata::Instance* findInstance(const kdata::Record& record, std::string_view id)
{
	for (const auto& instance : record.instances)
		if (instance.id == id)
			return &instance;
	return nullptr;
}

void run()
{
	// Base file (v16, so records carry the flags word).
	auto base = makeFile(16);
	{
		kdata::Record record;
		record.type = 4; // ITEM
		record.name = "Base Name";
		record.stringID = "100-base";
		record.hasFlags = true;
		record.flags = 0x80000002; // "new" from the v16-era writer
		record.bools = { { "flag", true } };
		record.ints = { { "a", 1 }, { "b", 2 } };
		record.strings = { { "present", "base" } };
		record.references = {
			{ "links", { makeRef("T", 1), makeRef("U", 2) } },
		};
		record.instances = { makeInstance("i1", "X"), makeInstance("i2", "Y") };
		base.records.push_back(record);
	}
	{
		kdata::Record record;
		record.type = 4;
		record.name = "Keep Me";
		record.stringID = "300-keep";
		record.hasFlags = true;
		record.flags = 0x80000002;
		base.records.push_back(record);
	}

	// Overlay: field override, reference removal, instance replacement and
	// deletion, a new instance, a new record, and a REMOVED record.
	auto mod = makeFile(16);
	{
		kdata::Record record;
		record.type = 4;
		record.name = "Mod Name"; // flags=1: not MODIFIED|RENAMED, must be ignored
		record.stringID = "100-base";
		record.hasFlags = true;
		record.flags = 1;
		record.ints = { { "a", 9 }, { "c", 3 } };
		record.strings = { { "new", "value" } };
		record.references = {
			{ "links", { makeRemovedRef("T"), makeRef("U", 5), makeRef("W", 7) } },
			// Removal of an absent category must not create it.
			{ "ghost", { makeRemovedRef("nope") } },
		};
		record.instances = {
			makeInstance("i1", "Z"),
			makeInstance("i2", ""), // empty target = delete
			makeInstance("i3", "Q"),
		};
		mod.records.push_back(record);
	}
	{
		kdata::Record record;
		record.type = 4;
		record.name = "New Item";
		record.stringID = "200-new";
		record.hasFlags = true;
		record.flags = 2;
		record.ints = { { "n", 1 } };
		mod.records.push_back(record);
	}
	{
		kdata::Record record;
		record.type = 4;
		record.stringID = "300-keep";
		record.hasFlags = true;
		record.flags = 1;
		record.bools = { { "REMOVED", true } };
		mod.records.push_back(record);
	}

	// The hand-built files must themselves survive parse+write.
	auto baseRoundTrip = serialized(base);
	auto modRoundTrip = serialized(mod);
	base = baseRoundTrip;
	mod = modRoundTrip;

	kdata::GameData data;
	data.apply(base);
	data.apply(mod);

	test::require(data.size() == 2, "expected 2 merged records");
	test::require(data.find("300-keep") == nullptr, "REMOVED record still present");
	test::require(data.find("100-base") != nullptr, "merged record missing");
	test::require(data.find("200-new") != nullptr, "new record missing");
	auto items = data.byType(4);
	test::require(items.size() == 2, "byType(ITEM) count");
	test::require(items[0]->stringID == "100-base" && items[1]->stringID == "200-new",
		"byType order not stable");

	const auto* record = data.find("100-base");
	test::require(record->name == "Base Name",
		"name changed without MODIFIED|RENAMED");

	auto* a = findInt(*record, "a");
	auto* b = findInt(*record, "b");
	auto* c = findInt(*record, "c");
	test::require(a != nullptr && a->value == 9, "int override a");
	test::require(b != nullptr && b->value == 2, "int b preserved");
	test::require(c != nullptr && c->value == 3, "int c added");
	test::require(record->ints.size() == 3, "int field count");
	test::require(record->ints[0].key == "a" && record->ints[1].key == "b"
		&& record->ints[2].key == "c", "int field order not stable");
	test::require(record->bools.size() == 1 && record->bools[0].key == "flag"
		&& record->bools[0].value, "bool field preserved");
	test::require(record->strings.size() == 2
		&& record->strings[0].key == "present"
		&& record->strings[1].key == "new", "string fields");

	const auto* links = findCategory(*record, "links");
	test::require(links != nullptr, "links category missing");
	test::require(links->entries.size() == 2, "reference removal/merge count");
	test::require(links->entries[0].targetID == "U" && links->entries[0].v0 == 5,
		"reference override U");
	test::require(links->entries[1].targetID == "W" && links->entries[1].v0 == 7,
		"reference add W");
	test::require(findCategory(*record, "ghost") == nullptr,
		"removal-only category was created");
	test::require(record->references.size() == 1, "reference category count");

	test::require(record->instances.size() == 2, "instance count after merge");
	test::require(record->instances[0].id == "i1"
		&& record->instances[0].targetStringID == "Z", "instance replacement");
	test::require(findInstance(*record, "i2") == nullptr,
		"instance deletion via empty target");
	test::require(record->instances[1].id == "i3"
		&& record->instances[1].targetStringID == "Q", "instance addition");

	// MODIFIED|RENAMED (3) is the only combination that may change a name.
	auto rename = makeFile(16);
	{
		kdata::Record update;
		update.type = 4;
		update.name = "Renamed";
		update.stringID = "100-base";
		update.hasFlags = true;
		update.flags = 3;
		rename.records.push_back(update);
	}
	data.apply(rename);
	test::require(data.find("100-base")->name == "Renamed", "RENAMED ignored");

	// v8/v9 have only v0; a lone 0x7FFFFFFF is the removal form there.
	kdata::File oldBase = makeFile(16);
	{
		kdata::Record record;
		record.type = 4;
		record.stringID = "old";
		record.references = { { "c", { makeRef("t", 1) } } };
		oldBase.records.push_back(record);
	}
	kdata::File oldOverlay = makeFile(9);
	{
		kdata::Record record;
		record.type = 4;
		record.stringID = "old";
		kdata::RefEntry entry;
		entry.targetID = "t";
		entry.v0 = kdata::kReferenceRemoved;
		entry.hasV1V2 = false;
		record.references = { { "c", { entry } } };
		oldOverlay.records.push_back(record);
	}
	kdata::GameData oldData;
	oldData.apply(oldBase);
	oldData.apply(oldOverlay);
	test::require(oldData.find("old")->references[0].entries.empty(),
		"v9 removed reference not deleted");
}

} // namespace

int main()
{
	try
	{
		run();
	}
	catch (const std::exception& error)
	{
		std::cerr << "overlay: " << error.what() << "\n";
		return 1;
	}
	std::cout << "overlay: all rules ok\n";
	return 0;
}
