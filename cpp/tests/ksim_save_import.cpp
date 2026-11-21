// Original-save import, stage 2 (kenshi-cym.2; docs/reference/SAVE_IMPORT.md): a small save written by hand with kdata::write (never cut from a real one) imports
// into a new game's world. Asserted: every PLATOON record is accounted for (player, full, token, never activated, skipped with a reason) and the stale file no
// record names is listed, not read; a token member's stats, wounds, bandage, gear, pack and sex are kept (MemberDelta + Inventory) and come back when the squad
// is promoted; a slave keeps its slave state and its captor's faction through the `slaver` handle; a dead member is a body where it lay; the player's member is
// a controlled Fighter, the CAMERA's selected handle resolves to it, and the handle map holds every imported squad and member; the hair mod sid is counted,
// any other unknown sid fails loudly; the imported world runs. Needs the game data (KENSHI_DATA), no navmesh.
// Stage 3a (kenshi-cym.3): the same hand-written save carries the world-wide records (clock, money, finished research, two factions with relations and prosperity,
// towns with an owner and a `replacementTown`, one weather slot, a bounty on the player's member), each checked where our save keeps it. The real save (the
// SkinHouse folder, never in the repo; KSIM_SKINHOUSE) is checked against values read by tools/ksave.py when it is given.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/save.hpp"
#include "ksim/save/save_import.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"

#include "kdata/format.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"


#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


namespace
{
// A fresh unique directory under the temp path (mkdtemp does not exist on MinGW).
std::filesystem::path makeTempDir(const std::string& prefix)
{
	static int counter = 0;
	const std::filesystem::path dir = std::filesystem::temp_directory_path()
		/ (prefix + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())) + "_" + std::to_string(counter++));
	std::filesystem::create_directories(dir);
	return dir;
}
} // namespace

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;
namespace fs = std::filesystem;

namespace {

constexpr const char* kSquadTemplate = "1302-gamedata.base"; // "Bandit squad": leader 1301 Dust bandit boss, members 510
constexpr const char* kBoss = "1301-gamedata.base";
constexpr const char* kNameless = "204-gamedata.base"; // the player's faction
constexpr const char* kBandits = "200-gamedata.base"; // Dust Bandits
constexpr const char* kHairMod = "99999-AF Hair.mod";
constexpr std::int32_t kPoolPlayer = 900;
constexpr std::int32_t kPoolFar = 901;
constexpr std::int32_t kPoolNear = 902;
constexpr std::int32_t kPoolNever = 903;
constexpr std::int32_t kPoolMissing = 904;

std::array<std::byte, 4> floatBytes(float value)
{
	std::array<std::byte, 4> bytes{};
	std::memcpy(bytes.data(), &value, 4);
	return bytes;
}

// One record, by field.
struct Rec
{
	kdata::Record record;

	Rec(const char* type, std::string sid, std::string name)
	{
		record.type = kworld::itemTypeByName(type);
		record.stringID = std::move(sid);
		record.name = std::move(name);
		record.hasFlags = true;
	}
	Rec& i(const std::string& key, std::int32_t value) { record.ints.push_back({key, value}); return *this; }
	Rec& b(const std::string& key, bool value) { record.bools.push_back({key, value}); return *this; }
	Rec& f(const std::string& key, double value) { record.floats.push_back({key, floatBytes(static_cast<float>(value))}); return *this; }
	Rec& s(const std::string& key, const std::string& value) { record.strings.push_back({key, value}); return *this; }
	Rec& file(const std::string& key, const std::string& value) { record.files.push_back({key, value}); return *this; }
	Rec& vec3(const std::string& key, float x, float y, float z)
	{
		kdata::FieldVec3 field;
		field.key = key;
		const float values[3] = {x, y, z};
		std::memcpy(field.value.data(), values, sizeof values);
		record.vec3s.push_back(field);
		return *this;
	}
	Rec& vec4(const std::string& key, float w, float x, float y, float z)
	{
		kdata::FieldVec4 field;
		field.key = key;
		const float values[4] = {w, x, y, z};
		std::memcpy(field.value.data(), values, sizeof values);
		record.vec4s.push_back(field);
		return *this;
	}
	Rec& handle(const std::string& prefix, std::int32_t c, std::int32_t index, std::int32_t type)
	{
		return i(prefix + "C", c).i(prefix + "I", index).i(prefix + "CS", 0).i(prefix + "S", 0).i(prefix + "TYPE", type);
	}
};

void writeFile(const fs::path& path, std::vector<Rec> records)
{
	kdata::File file;
	file.version = 15;
	file.hasHeader = false;
	for (Rec& rec : records)
		file.records.push_back(std::move(rec.record));
	file.lastID = 1000;
	const std::vector<std::byte> bytes = kdata::write(file);
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

struct Limb
{
	std::string sid;
	double flesh = 100.0;
	double bandage = 0.0;
};

struct Item
{
	std::string sid;
	std::string section;
	int quantity = 1;
};

struct Who
{
	std::string name;
	int serial = 1;
	double x = 0.0;
	double z = 0.0;
	bool leader = false;
	bool female = false;
	bool dead = false;
	double bleeding = 0.0;
	double strength = 40.0;
	int slaveState = 0;
	double ssct = 0.0; // the slave-state stamp in game hours (rwh.106), written when set
	bool sentenced = false; // `sentence` 3350 h, `psts` 48 h (a jail term still running at the saved 3353.5 h, no bounty)
	bool bounty = false; // a bounty row of 8000 on kBandits (claimed, crimes 0b10100100, stamp 3266 h)
	std::int32_t slaverPool = 0;
	std::int32_t slaverSerial = 0;
	std::vector<Limb> limbs;
	std::vector<Item> items;
	// GAMESTATE_AI: persistent jobs `pjob<i>` (null hand) and one-shot orders `tjob<i>` (null hand, `tjobP<i>` the point).
	std::vector<int> pjobs;
	std::vector<std::pair<int, std::array<float, 3>>> tjobs;
	double y = 0.0; // last: the members above are brace-initialised by position
};

// A platoon file: INSTANCE_COLLECTION with one instance per member, each with its state records (the layout of tools/ksave.py's reader).
void writePlatoon(const fs::path& path, const std::string& stem, std::int32_t pool, const std::vector<Who>& members)
{
	std::vector<Rec> records;
	Rec collection("INSTANCE_COLLECTION", "0-collection", stem);
	for (std::size_t n = 0; n < members.size(); ++n)
	{
		const Who& w = members[n];
		const std::string tag = stem + std::to_string(n);
		std::vector<std::string> states;
		Rec ch("GAMESTATE_CHARACTER", "ch" + tag, "state");
		ch.s("name", w.name).s("owner faction ID", kBandits).b("is leader", w.leader).i("speed mode", 2).b("chase mode", true).b("ranged mode", true)
			.i("slavestate", w.slaveState).handle("handle", pool, w.serial, 1);
		if (w.ssct != 0.0)
			ch.f("ssct", w.ssct);
		if (w.bounty)
			ch.i("amount0", 8000).s("bountyfac0", kBandits).i("bountyexp0", 3266).i("crimes0", 164).b("claim0", true);
		if (w.sentenced)
			ch.f("sentence", 3350.0).f("psts", 48.0);
		if (w.slaverPool != 0)
			ch.handle("slaver", w.slaverPool, w.slaverSerial, 1);
		else
			ch.handle("slaver", 0, 0, 11);
		Rec med("MEDICAL_STATE", "md" + tag, "med");
		med.f("blood", 100.0).f("hung", 3.0).f("bleeding", w.bleeding).f("KO", 0.0).f("fed", 0.0).b("dead", w.dead).b("unconcious", false).b("coma", false).b("incapacitated", false);
		for (std::size_t k = 0; k < w.limbs.size(); ++k)
		{
			const std::string index = std::to_string(k);
			med.s("sid" + index, w.limbs[k].sid).f("flesh" + index, w.limbs[k].flesh).f("hit" + index, 0.0).f("hitmult" + index, 1.0).f("wear" + index, 0.0)
				.f("bandage" + index, w.limbs[k].bandage).f("stun" + index, 0.0).f("rig" + index, 0.0);
		}
		Rec stats("STATS", "st" + tag, "stats");
		stats.f("strength", w.strength).f("katana", 55.0).f("xp", 12.0).f("farming", 30.0);
		Rec look("CHARACTER_APPEARANCE", "ap" + tag, "look");
		look.b("sex female", w.female).s("head", kHairMod);
		Rec inventory("INVENTORY_STATE", "iv" + tag, "inv");
		std::vector<Rec> itemRecords;
		for (std::size_t m = 0; m < w.items.size(); ++m)
		{
			const std::string sid = "it" + tag + "_" + std::to_string(m);
			Rec item("INVENTORY_ITEM_STATE", sid, "item");
			item.s("base data sid", w.items[m].sid).s("section", w.items[m].section).i("quantity", w.items[m].quantity).i("level", 100).f("quality", 100.0)
				.s("material sid", "").s("company sid", "").s("color sid", "").i("inventory x", 0).i("inventory y", 0).f("charges", 0.0);
			kdata::Instance entry;
			entry.id = sid;
			entry.targetStringID = sid;
			inventory.record.instances.push_back(entry);
			itemRecords.push_back(std::move(item));
		}
		kdata::Instance instance;
		instance.id = "m" + tag;
		instance.targetStringID = kBoss;
		const float xform[7] = {static_cast<float>(w.x), static_cast<float>(w.y), static_cast<float>(w.z), 1.0f, 0.0f, 0.0f, 0.0f};
		std::memcpy(instance.transform.data(), xform, sizeof xform);
		instance.states = {ch.record.stringID, med.record.stringID, stats.record.stringID, look.record.stringID, inventory.record.stringID};
		std::optional<Rec> ai;
		if (!w.pjobs.empty() || !w.tjobs.empty())
		{
			// A null hand is TYPE 11; its other four fields are stale garbage in a real save (rwh92), so they are non-zero here.
			const auto nullHand = [](Rec& rec, const std::string& prefix)
			{
				rec.i(prefix + "TYPE", 11).i(prefix + "CS", 1120403456).i(prefix + "C", 5).i(prefix + "I", 9).i(prefix + "S", 3);
			};
			ai.emplace("GAMESTATE_AI", "ai" + tag, "ai");
			ai->b("jobs", true);
			for (std::size_t k = 0; k < w.pjobs.size(); ++k)
			{
				ai->i("pjob" + std::to_string(k), w.pjobs[k]);
				nullHand(*ai, "pjobT" + std::to_string(k));
			}
			for (std::size_t k = 0; k < w.tjobs.size(); ++k)
			{
				ai->i("tjob" + std::to_string(k), w.tjobs[k].first);
				nullHand(*ai, "tjobT" + std::to_string(k));
				ai->vec3("tjobP" + std::to_string(k), w.tjobs[k].second[0], w.tjobs[k].second[1], w.tjobs[k].second[2]);
			}
			instance.states.push_back(ai->record.stringID);
		}
		collection.record.instances.push_back(instance);
		records.push_back(std::move(ch));
		records.push_back(std::move(med));
		records.push_back(std::move(stats));
		records.push_back(std::move(look));
		records.push_back(std::move(inventory));
		if (ai)
			records.push_back(std::move(*ai));
		for (Rec& item : itemRecords)
			records.push_back(std::move(item));
	}
	records.insert(records.begin(), std::move(collection));
	writeFile(path, std::move(records));
}

Rec platoon(const std::string& name, const std::string& faction, const std::string& factionName, const std::string& file, std::int32_t pool, float x, float z, int count, int money)
{
	Rec p("PLATOON", name, name);
	p.s("platoon stringID", name).s("faction stringID", faction).s("faction name", factionName).s("squad template", kSquadTemplate).file("content file", file)
		.handle("handle", pool, 0, 34).i("char count", count).i("money", money).b("is resident", false).b("imprisoned", false).b("dead", false)
		.b("never been activated", file == "platoon/").vec3("position", x, 0.0f, z);
	return p;
}

struct Names
{
	std::string head, torso;
	std::string weapon, armour, item;
};

// Limb sids by the part names the template's Fighter has, and a weapon, an armour piece and a plain item of the game data.
Names namesFor(const kdata::GameData& gd, const CombatData& data, Simulation&)
{
	std::uint64_t rng = 5;
	const state::Fighter f = data.makeFighter(gd, kBoss, "", rng, kSquadTemplate);
	const auto limbSid = [&](const std::string& part)
	{
		for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("LOCATIONAL_DAMAGE")))
		{
			if (record->name == part)
				return record->stringID;
		}
		throw test::Failure("no LOCATIONAL_DAMAGE named " + part);
	};
	Names names;
	names.head = limbSid(f.parts.front().name);
	names.torso = limbSid(f.parts.back().name);
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("WEAPON")))
	{
		if (record->name == "Katana")
			names.weapon = record->stringID;
	}
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("ARMOUR")))
	{
		if (names.armour.empty() && record->findRefs("part coverage") != nullptr && record->intOr("slot", -1) != 12)
			names.armour = record->stringID;
	}
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("ITEM")))
	{
		if (names.item.empty())
			names.item = record->stringID;
	}
	test::require(!names.weapon.empty() && !names.armour.empty() && !names.item.empty(), "the game data has a Katana, an armour piece and an item");
	return names;
}

// The entity of a state::Id.
flecs::entity entityOf(const Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	sim.world().ecs().each([&](flecs::entity entity, const state::Id& value)
	{
		if (value.value == id)
			found = entity;
	});
	test::require(found.is_valid(), "an entity with Id " + std::to_string(id));
	return found;
}

// kenshi-cym.14: a saved one-shot move (task 29) starts at load and the member walks to its point. A save of its own (the hero alone) on the Hub mesh,
// since the main save's positions are off the mesh. `knav`: the Hub navmesh cache (the ctest fixture knav_cache); without it the case is skipped.
void walkAfterImport(const kworld::LoadOrder& order, const char* knav)
{
	const std::shared_ptr<MeshNavigator> nav = knav != nullptr ? MeshNavigator::load(knav) : nullptr;
	if (nav == nullptr)
		return;
	const auto from = nav->walker().snap({-50978.8, 1535.0, 2932.48}, 200.0);
	const auto to = from ? nav->walker().snap({from->x + 50.0, from->y, from->z}, 40.0) : std::nullopt;
	test::require(from && to, "the Hub mesh has ground for the walk case");
	SpawnOptions spawn;
	spawn.seed = 7;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
	const auto gd = sim.gameData();
	const auto data = CombatData::build(*gd, KENSHI_DATA);
	sim.enableCombat(data, CombatSettings{});
	sim.enableNavigation(nav);
	const fs::path dir = makeTempDir("ksim_save_import_walk_");
	fs::create_directories(dir / "platoon");
	Who hero{"Hero", 7, from->x, from->z, true};
	hero.y = from->y;
	hero.tjobs = {{29, {static_cast<float>(to->x), static_cast<float>(to->y), static_cast<float>(to->z)}}};
	writePlatoon(dir / "platoon" / "Expedition.platoon", "pl", kPoolPlayer, {hero});
	Rec cam("CAMERA", "cam", "camera");
	cam.s("pfaction name", "Nameless").vec3("pos", 0.0f, 0.0f, 0.0f).vec4("rot", 1.0f, 0.0f, 0.0f, 0.0f).f("zoom", -300.0).i("time day", 140).i("time hour", 17).i("time minute", 32)
		.i("player money", 0);
	writeFile(dir / "quick.save", {cam, platoon("Expedition", kNameless, "Nameless", "platoon/Expedition.platoon", kPoolPlayer, static_cast<float>(from->x), static_cast<float>(from->z), 1, 0)});
	const saveimport::Report report = saveimport::importSave(sim, *gd, *data, dir.string(), saveimport::Options{});
	fs::remove_all(dir);
	const std::uint64_t id = report.memberOfHandle.at({kPoolPlayer, 7});
	test::require(report.movesImported == 1 && entityOf(sim, id).get<state::OrderQueue>() != nullptr, "the walk case: the move is queued at load");
	const auto left = [&] { return std::hypot(sim.combat()->find(id)->x - to->x, sim.combat()->find(id)->z - to->z); };
	for (int t = 0; t < 4000 && left() > 8.0; ++t)
		sim.run(1);
	test::require(left() <= 8.0 && entityOf(sim, id).get<state::OrderQueue>() == nullptr, "the saved move order started at load and the member walked to its point");
}

// kenshi-cym.25: members saved 50 u below the ground (a player's, and a near squad's promoted to Full) end on the navmesh surface; Options::snapFull off
// leaves them where the save put them. The Hub mesh as in walkAfterImport.
void snapAfterImport(const kworld::LoadOrder& order, const char* knav)
{
	const std::shared_ptr<MeshNavigator> nav = knav != nullptr ? MeshNavigator::load(knav) : nullptr;
	if (nav == nullptr)
		return;
	const auto from = nav->walker().snap({-50978.8, 1535.0, 2932.48}, 200.0);
	const auto beside = from ? nav->walker().snap({from->x + 30.0, from->y, from->z}, 40.0) : std::nullopt;
	test::require(from && beside, "the Hub mesh has ground for the snap case");
	for (const bool snap : {true, false})
	{
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gd = sim.gameData();
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		const fs::path dir = makeTempDir("ksim_save_import_snap_");
		fs::create_directories(dir / "platoon");
		Who hero{"Hero", 7, from->x, from->z, true};
		hero.y = from->y - 50.0;
		writePlatoon(dir / "platoon" / "Expedition.platoon", "pl", kPoolPlayer, {hero});
		Who near{"Near", 3, beside->x, beside->z, true};
		near.y = beside->y - 50.0;
		writePlatoon(dir / "platoon" / "fixb.platoon", "fb", kPoolNear, {near});
		Rec cam("CAMERA", "cam", "camera");
		cam.s("pfaction name", "Nameless").vec3("pos", 0.0f, 0.0f, 0.0f).vec4("rot", 1.0f, 0.0f, 0.0f, 0.0f).f("zoom", -300.0).i("time day", 140).i("time hour", 17).i("time minute", 32)
			.i("player money", 0);
		writeFile(dir / "quick.save", {cam, platoon("Expedition", kNameless, "Nameless", "platoon/Expedition.platoon", kPoolPlayer, static_cast<float>(from->x), static_cast<float>(from->z), 1, 0),
			platoon("fixb", kBandits, "Bandits", "platoon/fixb.platoon", kPoolNear, static_cast<float>(beside->x), static_cast<float>(beside->z), 1, 0)});
		saveimport::Options options;
		options.snapFull = snap;
		const saveimport::Report report = saveimport::importSave(sim, *gd, *data, dir.string(), options);
		fs::remove_all(dir);
		test::require(report.membersFull >= 1, "the snap case: the near squad is Full");
		const auto heroAt = sim.combat()->find(report.memberOfHandle.at({kPoolPlayer, 7}));
		const auto nearAt = sim.combat()->find(report.memberOfHandle.at({kPoolNear, 3}));
		test::require(heroAt != nullptr && nearAt != nullptr, "the snap case: both members exist");
		if (snap)
		{
			test::require(std::abs(heroAt->y - from->y) < 1.0 && std::abs(nearAt->y - beside->y) < 1.0, "a member saved 50 u below the ground ends on the navmesh surface");
			test::require(report.unplaced == 0, "the snap case: nothing unplaced");
		} else {
			test::require(std::abs(heroAt->y - (from->y - 50.0)) < 1.0 && std::abs(nearAt->y - (beside->y - 50.0)) < 1.0, "snapFull off leaves the saved height");
		}
	}
}

} // namespace

// `ksim_save_import_test --write-fixture DIR`: the smallest save the server's --import-save test (ksim_server_import) starts from: the hero (a player Fighter,
// selected in CAMERA) and one more member of the player squad. Written to DIR (quick.save, platoon/); nothing is imported.
int writeFixture(const fs::path& dir)
{
	fs::create_directories(dir / "platoon");
	Who hero{"Hero", 7, 1000.0, 1000.0, true};
	Who mate{"Mate", 8, 1010.0, 1000.0};
	writePlatoon(dir / "platoon" / "Expedition.platoon", "pl", kPoolPlayer, {hero, mate});
	Rec cam("CAMERA", "cam", "camera");
	cam.s("pfaction name", "Nameless").vec3("pos", 1000.0f, 20.0f, 1000.0f).vec4("rot", 1.0f, 0.0f, 0.0f, 0.0f).f("zoom", -300.0).handle("selected_character", kPoolPlayer, 7, 1)
		.i("time day", 140).i("time hour", 17).i("time minute", 32).i("player money", 0);
	writeFile(dir / "quick.save", {cam, platoon("Expedition", kNameless, "Nameless", "platoon/Expedition.platoon", kPoolPlayer, 1000.0f, 1000.0f, 2, 0)});
	return 0;
}

int main(int argc, char** argv)
{
	if (argc == 3 && std::string(argv[1]) == "--write-fixture")
		return writeFixture(argv[2]);
	fs::path dir;
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gd = sim.gameData();
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		sim.enableCombat(data, CombatSettings{});
		const Names names = namesFor(*gd, *data, sim);

		dir = makeTempDir("ksim_save_import_");
		fs::create_directories(dir / "platoon");
		// The player's squad (CAMERA's selected character is its member 7), a far token squad, a near squad that becomes Full, a never-activated one and one
		// whose file is gone.
		Who hero{"Hero", 7, 1000.0, 1000.0, true};
		hero.bounty = true;
		hero.items = {{names.weapon, "weapon", 1}, {names.item, "main", 3}};
		// The hero's saved orders (kenshi-cym.14): the persistent Medic / Find and rescue / Find and put in bed rows (148 brings 106 after it, the saved 106 is
		// its duplicate), one unknown type, a one-shot move (task 29) and a one-shot attack (16) with no hand, which the sim cannot issue from a save.
		hero.pjobs = {58, 148, 106, 9999};
		hero.tjobs = {{29, {1100.0f, 0.0f, 1000.0f}}, {16, {0.0f, 0.0f, 0.0f}}};
		writePlatoon(dir / "platoon" / "Expedition.platoon", "pl", kPoolPlayer, {hero});
		Who wounded{"Wounded", 1, 50000.0, 50000.0, true, true};
		wounded.bleeding = 0.5;
		wounded.strength = 61.0;
		wounded.slaveState = 2; // an escaped slave of a token squad: its ssct stamp rides in the MemberDelta (rwh.106)
		wounded.ssct = 3345.25;
		wounded.limbs = {{names.head, 40.0, 2.0}, {names.torso, 75.0, 0.0}};
		wounded.items = {{names.weapon, "weapon", 1}, {names.armour, "armour", 1}, {names.item, "main", 4}};
		Who slave{"Slave", 2, 50010.0, 50020.0};
		slave.slaveState = 1;
		slave.slaverPool = kPoolNear;
		slave.slaverSerial = 3;
		writePlatoon(dir / "platoon" / "fixa.platoon", "fa", kPoolFar, {wounded, slave});
		Who captor{"Captor", 3, 1500.0, 1000.0, true};
		captor.sentenced = true;
		captor.slaveState = 3; // an ex-slave of a Full squad: its ssct stamp is on the Fighter
		captor.ssct = 3340.5;
		captor.limbs = {{names.head, 80.0, 0.0}};
		Who body{"Body", 4, 1510.0, 1030.0};
		body.dead = true;
		writePlatoon(dir / "platoon" / "fixb.platoon", "fb", kPoolNear, {captor, body});
		writeFile(dir / "platoon" / "stale.platoon", {});
		Rec cam("CAMERA", "cam", "camera");
		cam.s("pfaction name", "Nameless").vec3("pos", 1000.0f, 20.0f, 1000.0f).vec4("rot", 1.0f, 0.0f, 0.0f, 0.0f).f("zoom", -300.0).handle("selected_character", kPoolPlayer, 7, 1);
		cam.i("num usedUniques", 2);
		cam.i("time day", 140).i("time hour", 17).i("time minute", 32).i("player money", 228952);
		// The world-wide records: finished research (a level suffix and a TECH suffix fold to the sid), two factions, two towns, a weather slot.
		std::vector<std::string> researchSids;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("RESEARCH")))
		{
			if (researchSids.size() < 2 && !record->boolOr("REMOVED", false))
				researchSids.push_back(record->stringID);
		}
		test::require(researchSids.size() == 2, "the game data has two RESEARCH records");
		Rec research("RESEARCH", "res", "research");
		research.s("finished0", researchSids[0]).s("finished1", researchSids[0] + ".2").s("finished2", researchSids[1] + ".TECH.1");
		Rec bandits("GAMESTATE_FACTION", "fb", "Dust Bandits");
		bandits.s("gamedata stringID", kBandits).f("prosperity", 600.0).s("relationSID0", kNameless).f("relation0", -42.5).s("relationSID1", kBandits).f("relation1", 100.0)
			.i("rank", 2).f("global trust", 4.5).f("trust0", 12.5).f("trustNeg0", 3.0).f("trust1", 0.0);
		Rec nameless("GAMESTATE_FACTION", "fn", "Nameless");
		nameless.s("gamedata stringID", kNameless).f("prosperity", 1000.0).s("relationSID0", kBandits).f("relation0", 7.0);
		const std::map<std::string, state::EntityRef> placed = sim.importTownIndex();
		test::require(placed.size() >= 2, "the world has towns on placements");
		const auto townA = placed.begin();
		const auto townB = std::next(placed.begin());
		std::string replacement;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("TOWN")))
		{
			if (replacement.empty() && !record->boolOr("REMOVED", false))
				replacement = record->stringID;
		}
		cam.s("usedUniques0", replacement).i("usedUniquesState0", 1).b("usedUniquesPlayer0", true).handle("usedUniques0handle", kPoolPlayer, 7, 1);
		cam.s("usedUniques1", researchSids[0]).i("usedUniquesState1", 2).b("usedUniquesPlayer1", false).handle("usedUniques1handle", 0, 0, 11);
		Rec stateA("GAMESTATE_TOWN", "ta", "Town state A");
		stateA.b("public", true).i("plevel", 3).b("discovered", true).f("pop", 80.0).s("factions here0", kBandits);
		stateA.i("handTYPE", 13).i("handC", 1).s("instance", townA->first).s("faction", kBandits).s("replacementTown", "");
		Rec stateB("GAMESTATE_TOWN", "tb", "Town state B");
		stateB.b("public", false).i("plevel", 1).b("explored", true);
		stateB.i("handTYPE", 13).i("handC", 2).s("instance", townB->first).s("faction", kNameless).s("replacementTown", replacement);
		Rec stateC("GAMESTATE_TOWN", "tc", "Town state C");
		stateC.b("public", true).i("plevel", 2).b("discovered", true).b("ded", true);
		stateC.i("handTYPE", 13).i("handC", 3).s("instance", "0-nowhere-INGAME").s("faction", kBandits).s("replacementTown", "");
		// A season only one group of ours holds, and a slot with no season.
		const WeatherModel& model = sim.weather()->model();
		std::string uniqueSeason, uniqueGroup, seasonWeather;
		for (const WeatherGroupDef& group : model.groups())
		{
			for (const SeasonDef& season : group.seasons)
			{
				std::size_t holders = 0;
				for (const WeatherGroupDef& other : model.groups())
				{
					for (const SeasonDef& theirs : other.seasons)
						holders += theirs.sid == season.sid ? 1 : 0;
				}
				if (uniqueSeason.empty() && holders == 1 && !season.weathers.empty())
				{
					uniqueSeason = season.sid;
					uniqueGroup = group.sid;
					seasonWeather = season.weathers.front().weather;
				}
			}
		}
		test::require(!uniqueSeason.empty(), "some season belongs to one group");
		Rec biomes("BIOMES", "bio", "biomes");
		biomes.s("0_w_s_id", uniqueSeason).s("0_w_w_id", seasonWeather).i("0_w_wi_start", 202629).i("0_w_wi_end", 202667).f("0_w_wi_str", 0.75).f("0_w_wi_ef_str", 0.5).i("0_w_s_end", 240)
			.s("1_w_s_id", "").s("1_w_w_id", seasonWeather);
		writeFile(dir / "quick.save", {cam, research, bandits, nameless, stateA, stateB, stateC, biomes, platoon("Expedition", kNameless, "Nameless", "platoon/Expedition.platoon", kPoolPlayer, 1000.0f, 1000.0f, 1, 50),
			platoon("Far", kBandits, "Dust Bandits", "platoon/fixa.platoon", kPoolFar, 50000.0f, 50000.0f, 2, 0),
			platoon("Near", kBandits, "Dust Bandits", "platoon/fixb.platoon", kPoolNear, 1500.0f, 1000.0f, 2, 10),
			platoon("Never", kBandits, "Dust Bandits", "platoon/", kPoolNever, 30000.0f, 30000.0f, 3, 0),
			platoon("Gone", kBandits, "Dust Bandits", "platoon/gone.platoon", kPoolMissing, 40000.0f, 40000.0f, 1, 0)});

		const saveimport::Report report = saveimport::importSave(sim, *gd, *data, dir.string());
		const auto outcome = [&](const std::string& key) { return report.byOutcome.count(key) != 0 ? report.byOutcome.at(key) : std::size_t{0}; };
		test::require(report.platoons.size() == 5, "every PLATOON record is accounted for");
		test::require(outcome("player") == 1 && outcome("token") == 1 && outcome("full") == 1 && outcome("token (never activated)") == 1
			&& outcome("skipped: content file missing") == 1, "the platoons by outcome: 1 player, 1 token, 1 full, 1 never activated, 1 skipped for its missing file");
		test::require(report.staleFiles == std::vector<std::string>{"stale.platoon"}, "the file no record names is listed as stale");
		test::require(report.membersPlayer == 1 && report.membersToken >= 2 && report.membersFull == 2 && report.dead == 1, "members: 1 player, the far squad and the never-activated ones as tokens, 2 full");
		test::require(report.appearanceSids.size() == 1 && report.appearanceSids.count(kHairMod) == 1 && report.appearanceOccurrences == 5, "the hair mod sid is counted, not an error");
		test::require(report.squadOfPool.size() == 4 && report.memberOfHandle.size() == 5, "the handle map holds the 4 imported squads (the skipped one has none) and the 5 members");
		test::require(report.gaps.count("squad purse (money; PLATOON `money`)") != 0 && report.statsUnmapped.count("farming") != 0 && report.statsUnmapped.count("xp") != 0,
			"the gaps are counted: the purse, the STATS with no field");

		// The token member keeps its record: stats, wounds, bandage, sex, gear and pack.
		const std::uint64_t woundedId = report.memberOfHandle.at({kPoolFar, 1});
		const flecs::entity token = entityOf(sim, woundedId);
		const state::MemberDelta* delta = token.get<state::MemberDelta>();
		test::require(delta != nullptr && token.get<state::Fighter>() == nullptr, "a token member has a MemberDelta and no Fighter");
		test::require((delta->mask & (state::MemberDelta::kBody | state::MemberDelta::kSkills | state::MemberDelta::kWeapon | state::MemberDelta::kArmour | state::MemberDelta::kLook))
			== (state::MemberDelta::kBody | state::MemberDelta::kSkills | state::MemberDelta::kWeapon | state::MemberDelta::kArmour | state::MemberDelta::kLook),
			"the delta holds body, skills, weapon, armour and look");
		test::require(delta->skills.strength == 61.0 && delta->skills.katana == 55.0, "the stats came across (strength 61, katana 55)");
		test::require(delta->name == "Wounded" && delta->female == 1, "the name and the sex came across");
		test::require(!delta->wounds.empty() && delta->wounds.front() == 0.5, "the bleeding is a wound of that rate");
		bool head = false;
		for (const state::FighterPart& part : delta->parts)
			head = head || (part.health == 40.0 && part.pool == 2.0);
		test::require(head, "a limb at 40 flesh with a bandage of 2 is stored");
		test::require(delta->weapon.sid == names.weapon && delta->armour.size() == 1 && delta->armour.front().sid == names.armour, "the worn weapon and armour are stored");
		const state::Inventory* pack = token.get<state::Inventory>();
		test::require(pack != nullptr, "a token member has its Inventory");
		std::uint32_t carried = 0;
		bool wielded = false;
		for (const state::InventoryEntry& entry : pack->items)
		{
			carried += entry.itemSid == names.item && entry.slot == state::Inventory::kCarried ? entry.count : 0;
			wielded = wielded || (entry.itemSid == names.weapon && entry.slot == state::Inventory::kWeapon);
		}
		test::require(carried == 4 && wielded, "the pack holds the 4 carried items and the wielded weapon");

		// The slave: slave state and the captor's faction through the handle.
		const flecs::entity slaveEntity = entityOf(sim, report.memberOfHandle.at({kPoolFar, 2}));
		const state::MemberDelta* slaveDelta = slaveEntity.get<state::MemberDelta>();
		test::require(slaveDelta != nullptr && (slaveDelta->mask & state::MemberDelta::kSlave) != 0 && slaveDelta->slaveState == 1 && slaveDelta->captorSid == kBandits,
			"the slave is a slave of the captor's faction (slaver handle -> member -> platoon)");
		test::require(report.slaves == 3 && report.captors == 1, "three slave states (state 1, 2, 3), one captor resolved");
		const flecs::entity escapee = entityOf(sim, report.memberOfHandle.at({kPoolFar, 1}));
		const state::MemberDelta* escapeeDelta = escapee.get<state::MemberDelta>();
		test::require(escapeeDelta != nullptr && escapeeDelta->slaveState == 2 && escapeeDelta->slaveStamp == 3345.25, "ssct: the escaped slave of a token squad keeps its stamp (3345.25 h)");
		test::require(slaveDelta->slaveStamp == -1.0, "ssct: a slave (state 1) carries no stamp");

		// The Full squad: Fighters at their saved spots, the wounded one's limb applied, the body where it lay with its decay clock.
		const std::uint64_t captorId = report.memberOfHandle.at({kPoolNear, 3});
		const state::Fighter* captorFighter = sim.combat()->find(captorId);
		test::require(captorFighter != nullptr && !captorFighter->dead && captorFighter->x == 1500.0 && captorFighter->z == 1000.0, "a Full member stands at its saved spot");
		test::require(captorFighter->name == "Captor" && captorFighter->parts.front().health == 80.0, "its name and its limb came across");
		test::require(captorFighter->slaveState == 3 && captorFighter->slaveStamp == 3340.5, "ssct: the ex-slave of a Full squad keeps its stamp (3340.5 h)");
		const std::uint64_t bodyId = report.memberOfHandle.at({kPoolNear, 4});
		const state::Fighter* bodyFighter = sim.combat()->find(bodyId);
		test::require(bodyFighter != nullptr && bodyFighter->dead && bodyFighter->x == 1510.0 && bodyFighter->z == 1030.0, "the dead member is a body where it lay");
		test::require(entityOf(sim, bodyId).get<state::CorpseDecay>() != nullptr, "the body has its decay clock");

		// The player's member and the CAMERA.
		const std::uint64_t heroId = report.memberOfHandle.at({kPoolPlayer, 7});
		const state::Fighter* heroFighter = sim.combat()->find(heroId);
		test::require(heroFighter != nullptr && heroFighter->playerControlled && heroFighter->name == "Hero", "the player's member is a controlled Fighter");
		test::require(report.selected == heroId && report.hasCamera && report.cameraPos[0] == 1000.0 && report.cameraZoom == -300.0, "CAMERA: the selected handle resolves to the member, the camera is read");

		// Saved orders: the persistent rows as the jobs list holds them, the move queued, the unissuable ones counted.
		{
			const state::Jobs* jobs = entityOf(sim, heroId).get<state::Jobs>();
			test::require(jobs != nullptr && jobs->rows.size() == 3 && jobs->rows[0].type == 58 && jobs->rows[1].type == 148 && jobs->rows[2].type == 106,
				"the saved pjobs are the jobs list: Medic, Find and rescue, Find and put in bed (the duplicate 106 dropped)");
			test::require(report.jobsImported == 3 && report.movesImported == 1 && report.ordersSkipped.size() == 2 && report.ordersSkipped.at(9999) == 1 && report.ordersSkipped.at(16) == 1,
				"the order counts: 3 rows, 1 move, skipped task 9999 (not a job) and 16 (a one-shot attack)");
			const state::OrderQueue* queue = entityOf(sim, heroId).get<state::OrderQueue>();
			test::require(queue != nullptr && queue->orders.size() == 1 && queue->orders[0].kind == 1 && queue->orders[0].x == 1100.0 && queue->orders[0].z == 1000.0,
				"the saved tjob is one queued move order to its point");
		}

		// The squad promoted later has the member as saved (the delta comes back).
		sim.run(40);
		walkAfterImport(order, argc > 1 ? argv[1] : nullptr);
		snapAfterImport(order, argc > 1 ? argv[1] : nullptr);
		test::require(sim.combat()->find(heroId) != nullptr, "the imported world runs");

		// Stage 3a: each group where our save keeps it.
		const saveimport::WorldReport& world = report.world;
		test::require(world.day == 140 && world.hour == 17 && world.minute == 32, "the clock is read");
		{
			const GameTime now = gameTimeAt(sim.clockTick());
			test::require(now.day == 140 && now.hour == 17 && (now.minute == 32 || now.minute == 33), "the sim's clock stands at the saved time (40 ticks run, under two game minutes)");
		}
		test::require(world.research == 2 && world.playerFaction && world.money == 228952, "research (the level and TECH suffixes folded) and money landed on the player faction");
		const std::vector<std::string> completed = sim.completedResearch();
		test::require(completed == researchSids, "the completed research is the two sids, in order");
		{
			std::int64_t money = -1;
			sim.world().ecs().each([&](const state::PlayerFaction& faction) { money = faction.money; });
			test::require(money == 228952, "PlayerFaction.money is the player money");
		}
		const auto factionEntity = [&](const std::string& sid)
		{
			flecs::entity found;
			sim.world().ecs().each([&](flecs::entity entity, const state::FactionState& faction) { found = faction.templateSid == sid ? entity : found; });
			return found;
		};
		const flecs::entity banditsFaction = factionEntity(kBandits);
		const flecs::entity namelessFaction = factionEntity(kNameless);
		test::require(banditsFaction.is_valid() && namelessFaction.is_valid(), "both factions exist");
		test::require(world.relations == 2 && world.relationsNonZero == 2 && world.factions == 2 && world.prosperity == 2, "relations (the self row skipped) and prosperity counted");
		test::require(std::abs(factionRelation(*gd, banditsFaction, namelessFaction) + 42.5) < 1e-6 && factionRelation(*gd, namelessFaction, banditsFaction) == 7.0, "each direction of the relation is its own record's value");
		test::require(std::abs(factionProsperity(*gd, banditsFaction) - 600.0) < 1e-6 && std::abs(factionProsperity(*gd, namelessFaction) - 1000.0) < 1e-6, "prosperity (the original's, 1000 = full) lands as the loss below the cap");
		test::require(world.towns == 3 && world.owners == 2 && world.townsWithoutPlacement == 1, "two towns found their placement, the third names none");
		test::require(sim.world().ecs().entity(townA->second.id).get<state::TownState>()->owner.id == banditsFaction.id()
			&& sim.world().ecs().entity(townB->second.id).get<state::TownState>()->owner.id == namelessFaction.id(), "the town owners are the saved factions");
		test::require(world.replacements.size() == 1 && world.replacements.count(townB->first) != 0 && world.replacements.at(townB->first) == replacement,
			"replacementTown is parsed (instance -> TOWN sid)");
		test::require(world.replacementsApplied == 1 && world.replacementsRefused == 0 && sim.world().ecs().entity(townB->second.id).get<state::TownState>()->activeSid == replacement,
			"the replacement town is applied: TownState.activeSid");
		// Stage 3b: town flags, the town with no TownState, faction standing, used uniques, a jail sentence.
		{
			const state::TownInfo& a = sim.world().ecs().entity(townA->second.id).get<state::TownState>()->info;
			const state::TownInfo& b = sim.world().ecs().entity(townB->second.id).get<state::TownState>()->info;
			test::require(world.townsInfo == 3 && world.townsPublic == 2 && world.townsDiscovered == 2 && world.townsExplored == 1, "the town flags are counted over every town row (public, discovered, explored)");
			test::require(a.known && a.isPublic && a.level == 3 && a.discovered && a.pop == 80.0 && a.factionsHere == std::vector<std::string>{kBandits}, "town A: public, level 3, discovered, pop 80, a faction here");
			test::require(b.known && !b.isPublic && b.level == 1 && b.explored && !b.discovered, "town B: private, level 1, explored");
			std::vector<state::ExtraTown> extras;
			sim.world().ecs().each([&](const state::ExtraTown& extra) { extras.push_back(extra); });
			test::require(world.extraTowns == 1 && world.extraTownsPlaced == 0 && world.extraTownsOwned == 1 && extras.size() == 1, "the town with no TownState is one ExtraTown");
			test::require(extras.front().instance == "0-nowhere-INGAME" && !extras.front().placed && extras.front().owner.id == banditsFaction.id() && extras.front().info.isPublic
				&& extras.front().info.level == 2 && extras.front().info.discovered && extras.front().info.ded, "the ExtraTown keeps instance, owner and the flags");
			const state::FactionState* fb = banditsFaction.get<state::FactionState>();
			test::require(world.ranked == 1 && world.trustRows == 1 && fb->rank == 2 && fb->globalTrust == 4.5 && fb->trust.size() == 1 && fb->trust.front().other == kNameless
				&& fb->trust.front().trust == 12.5 && fb->trust.front().trustNeg == 3.0, "faction rank, global trust and the non-zero trust# rows");
			const state::WorldEventState* events = sim.world().ecs().get<state::WorldEventState>();
			test::require(world.uniques == 2 && world.uniquesResolved == 1 && events != nullptr && events->used.size() == 2, "both used uniques are kept, one resolved to a member");
			test::require(events->used.front().sid == replacement && events->used.front().state == 1 && events->used.front().player && events->used.front().member == heroId
				&& events->used.back().state == 2 && !events->used.back().player && events->used.back().member == 0, "a used unique keeps sid, state, player flag and its member");
			const state::CrimeRecord* jailed = nullptr;
			sim.world().ecs().each([&](const state::Id& id, const state::CrimeRecord& record) { jailed = id.value == captorId ? &record : jailed; });
			test::require(world.sentences == 1 && jailed != nullptr && jailed->sentenceStart == 3350.0 && jailed->sentenceHours == 48.0 && jailed->bounties.empty(), "a sentence without a bounty is a CrimeRecord");
		}
		const state::CrimeRecord* crime = sim.crimeRecord(heroId);
		test::require(world.bounties == 1 && world.bountyCharacters == 2 && crime != nullptr && crime->bounties.size() == 1, "the bounty is a CrimeRecord on the member (two records: the bounty holder and the sentenced one)");
		test::require(crime->bounties.front().factionSid == kBandits && crime->bounties.front().amount == 8000 && crime->bounties.front().crimes == 164 && crime->bounties.front().claim
			&& crime->bounties.front().stamp == 3266.0, "the bounty's holder, amount, crimes, claim and stamp are the saved ones");
		test::require(world.weatherSlots == 2 && world.weatherSet.size() == 1 && world.weatherSet.at(0) == uniqueGroup && world.weatherLeft.count("no season") == 1, "one slot found its group, the empty one is left");
		for (const state::WeatherRegion& region : sim.world().ecs().get<state::WeatherRegions>()->regions)
		{
			if (region.group != uniqueGroup)
				continue;
			test::require(region.weather == seasonWeather && region.startMinute == 202629 && region.endMinute == 202667 && region.strength == 0.75 && region.effectStrength == 0.5
				&& region.seasonEndDay == 240 && region.started == 1, "the region has the saved weather and times");
		}

		// An unknown sid fails loudly, listed; nothing is substituted.
		{
			const fs::path bad = dir / "bad";
			fs::create_directories(bad / "platoon");
			Who odd{"Odd", 1, 100.0, 100.0, true};
			odd.items = {{"1-nonexistent.mod", "main", 1}};
			writePlatoon(bad / "platoon" / "odd.platoon", "od", 950, {odd});
			writeFile(bad / "quick.save", {cam, platoon("Odd", kBandits, "Dust Bandits", "platoon/odd.platoon", 950, 100.0f, 100.0f, 1, 0)});
			bool refused = false;
			try
			{
				saveimport::importSave(sim, *gd, *data, bad.string());
			} catch (const std::exception& e) {
				refused = std::string(e.what()).find("1-nonexistent.mod") != std::string::npos;
			}
			test::require(refused, "an unknown item sid is refused, named");
		}
		fs::remove_all(dir);
		dir.clear();
		// The real save, when given: values read from its records by tools/ksave.py (dump, 2026-11-11).
		if (const char* skin = std::getenv("KSIM_SKINHOUSE"); skin != nullptr && *skin != 0)
		{
			Simulation real = Simulation::newGame(KENSHI_DATA, order, spawn);
			const auto realGd = real.gameData();
			const auto realData = CombatData::build(*realGd, KENSHI_DATA);
			real.enableCombat(realData, CombatSettings{});
			const saveimport::Report skinReport = saveimport::importSave(real, *realGd, *realData, skin);
			const saveimport::WorldReport& w = skinReport.world;
			{
				// The save's platoons are the whole population: the new game's own squads (and the wilderness spawner) are gone.
				std::size_t squads = 0;
				real.world().ecs().each([&](const state::SquadInstance&) { ++squads; });
				std::cout << "ksim_save_import: SkinHouse squads " << squads << " (" << skinReport.generatedRemoved << " generated removed)\n";
				const auto count = [&](const char* outcome)
				{
					const auto found = skinReport.byOutcome.find(outcome);
					return found == skinReport.byOutcome.end() ? std::size_t{0} : found->second;
				};
				test::require(skinReport.generatedRemoved > 1000, "SkinHouse: the new game's generated squads were removed");
				test::require(squads + count("player") + count("skipped") == skinReport.platoons.size(), "SkinHouse: the world holds exactly the imported platoons (the player's 3 are not SquadInstances)");
				test::require(squads == 1162, "SkinHouse: 1,162 squads (1,165 platoons, 3 of them the player's)");
				test::require(real.wilderness() == nullptr, "SkinHouse: no wilderness spawner after an import");
			}
			test::require(w.day == 140 && w.hour == 17 && w.minute == 32, "SkinHouse: the clock is day 140 17:32");
			test::require(w.money == 228952 && w.playerFaction, "SkinHouse: money 228952");
			test::require(w.research == 71 && w.researchDropped == 7, "SkinHouse: 86 finished entries fold to 78 sids, 71 live RESEARCH records and 7 dropped");
			test::require(w.relations == 10404 && w.relationsNonZero == 4028 && w.factions == 103 && w.prosperity == 103, "SkinHouse: 10,404 relation rows (4,028 non-zero), 103 factions");
			const auto factionOf = [&](const std::string& sid)
			{
				flecs::entity found;
				real.world().ecs().each([&](flecs::entity entity, const state::FactionState& faction) { found = faction.templateSid == sid ? entity : found; });
				return found;
			};
			const flecs::entity dust = factionOf(kBandits);
			test::require(std::abs(factionRelation(*realGd, dust, factionOf(kNameless)) + 11.0225849) < 1e-3, "SkinHouse: Dust Bandits toward Nameless -11.0226");
			test::require(std::abs(factionProsperity(*realGd, dust) - 332.913) < 1e-2, "SkinHouse: Dust Bandits prosperity 332.913");
			test::require(w.towns == 306 && w.owners == 169 && w.townsWithoutPlacement == 137 && w.replacements.empty(), "SkinHouse: 306 towns, 169 on a placement of ours, no replacementTown set");
			test::require(w.bounties == 66 && w.bountyCharacters == 62 && w.bountiesLost == 0, "SkinHouse: 66 bounty rows; 62 characters hold a CrimeRecord (55 with a bounty, 7 more with only a sentence)");
			{
				// kenshi-k16.6.126 (rwh.106): `ssct`, the stamp of the last change into slave state 2 or 3 (22 characters of the save: 3 dead bodies and 3 of the Drifters / Vagrants squads, whose platoons are
				// not imported, leave 16 live ones; 4 of them state 3 hungry bandits at 3352.09 h), reaches the same stamp the timers read; the Tuner Outlaw (escaped, platoon Outlaw_424) is at 3377.0126953125 h.
				int stamped = 0;
				int stampedAtTuner = 0;
				const auto note = [&](int state, double stamp)
				{
					stamped += state > 1 && stamp >= 0.0 ? 1 : 0;
					stampedAtTuner += state == 2 && stamp == 3377.0126953125 ? 1 : 0;
				};
				real.world().ecs().each([&](const state::MemberDelta& delta) { note(delta.slaveState, delta.slaveStamp); });
				for (const CombatArena::Member& member : real.combat()->members())
					note(member.fighter.slaveState, member.fighter.slaveStamp);
				test::require(stamped == 16 && stampedAtTuner == 3, "SkinHouse: 16 imported characters in slave state 2 or 3 carry their ssct stamp (" + std::to_string(stamped) + "), three escaped ones at 3377.0127 h (" + std::to_string(stampedAtTuner) + ")");
			}
			const state::CrimeRecord* engival = real.crimeRecord(skinReport.selected);
			test::require(engival != nullptr && engival->bounties.size() == 1 && engival->bounties.front().amount == 8000 && engival->bounties.front().crimes == 164
				&& engival->bounties.front().claim && engival->bounties.front().stamp == 3266.0, "SkinHouse: Engival's bounty 8000, crimes 164, claimed, stamp 3266 h");
			{
				// kenshi-cym.14 (rwh92): Engival's tjob0 is task 29 to a point inside the Skin Bandits tower, his pjobs are 58 / 148 / 106
				const flecs::entity engivalEntity = entityOf(real, skinReport.selected);
				const state::OrderQueue* orders = engivalEntity.get<state::OrderQueue>();
				test::require(orders != nullptr && orders->orders.size() == 1 && orders->orders[0].kind == 1 && std::abs(orders->orders[0].x - 72672.914) < 0.01
					&& std::abs(orders->orders[0].y - 690.737) < 0.01 && std::abs(orders->orders[0].z - 94252.805) < 0.01, "SkinHouse: Engival has the saved move order to (72672.9, 690.7, 94252.8)");
				const state::Jobs* jobs = engivalEntity.get<state::Jobs>();
				test::require(jobs != nullptr && jobs->rows.size() == 3 && jobs->rows[0].type == 58 && jobs->rows[1].type == 148 && jobs->rows[2].type == 106, "SkinHouse: Engival's pjobs are 58, 148, 106");
				test::require(skinReport.movesImported == 1 && skinReport.jobsImported == 13 && skinReport.ordersSkipped.size() == 2 && skinReport.ordersSkipped.at(87) == 6 && skinReport.ordersSkipped.at(124) == 1,
					"SkinHouse: one move order, 13 job rows over the 4 members; skipped: 87 x6 and 124 x1 (a machine / building target, not a member)");
			}
			test::require(w.weatherSlots == 71 && w.weatherSet.size() == 65 && w.weatherSet.count(67) != 0 && w.weatherSet.at(67) == "55939-rebirth.mod", "SkinHouse: 65 of 71 weather slots found their group (the season pairing), slot 67 is Sonorous Dark");
			for (const state::WeatherRegion& region : real.world().ecs().get<state::WeatherRegions>()->regions)
			{
				if (region.group == "55939-rebirth.mod")
					test::require(region.weather == "97895-rebirth.mod" && region.startMinute == 202629 && region.endMinute == 202667 && region.strength == 1.0 && region.seasonEndDay == 200,
						"SkinHouse: Sonorous Dark has weather 97895-rebirth.mod, minutes 202629..202667, strength 1");
			}
			test::require(w.extraTowns == 137 && w.extraTownsPlaced == 135 && w.extraTownsOwned == 136, "SkinHouse: the 137 town records with no TownState are ExtraTowns (135 placed, 136 owned)");
			test::require(w.townsInfo == 306 && w.townsPublic == 253 && w.townsDiscovered == 78 && w.townsExplored == 35, "SkinHouse: 306 town flag sets, 253 public, 78 discovered, 35 explored");
			test::require(w.replacementsApplied == 0 && w.replacementsRefused == 0, "SkinHouse: no replacementTown set, none applied");
			test::require(w.ranked == 1 && w.trustRows == 0, "SkinHouse: one ranked faction, no non-zero trust rows");
			test::require(w.uniques == 75 && w.uniquesResolved == 75, "SkinHouse: 75 used uniques, every handle resolved");
			test::require(w.sentences == 10, "SkinHouse: 10 characters serve a sentence");
			// Round trip through our save: the imported world keeps the new state byte for byte.
			std::vector<std::string> names;
			for (const kworld::LoadOrderEntry& entry : order.files)
				names.push_back(entry.name);
			std::ostringstream out;
			writeSave(out, real.world().ecs(), names);
			flecs::world loaded;
			state::registerComponents(loaded);
			std::istringstream in(out.str());
			readSave(in, loaded, names);
			test::require(stateHash(loaded) == stateHash(real.world().ecs()), "SkinHouse: the imported world saves and loads to the same hash");
			std::size_t extras = 0;
			loaded.each([&](const state::ExtraTown&) { ++extras; });
			test::require(extras == 137 && loaded.get<state::WorldEventState>() != nullptr && loaded.get<state::WorldEventState>()->used.size() == 75, "SkinHouse: ExtraTowns and used uniques survive the round trip");
			// ksim_soak --import-save: the imported world runs one game-hour under the squad AI (no navigator: the generic fallbacks) and writes the CSVs.
			AiSystem::Config ai;
			ai.registry = std::make_shared<const BehaviourRegistry>(real.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			real.enableAi(std::move(ai));
			std::ostringstream csv;
			std::ostringstream factionCsv;
			SoakOptions soak;
			soak.days = 1.0 / 24.0;
			soak.csv = &csv;
			soak.factionCsv = &factionCsv;
			const SoakReport soaked = runSoak(real, soak);
			test::require(soaked.samples == 1 && soaked.last.squads > 1000, "SkinHouse: one game-hour soaks over the imported squads");
			test::require(csv.str().find("\n1,0,") != std::string::npos, "SkinHouse: the soak CSV has its first hourly row");
			// The imported player faction is a living world (kenshi-cym.21): the full-detail area follows Engival, so the squad in his tower (template
			// 66541-rebirth.mod, ~40 u from his saved move order) is built after the hour. Before, no area was set and it stayed a token for all 48 h.
			// The original has no radius (findings/rwh202_save_wire_validity_static.md 3): a platoon is active when its position is in a loaded zone, the 3x3
			// zones round the camera focus plus the zones a +-340 u box round each player character overlaps. The tower is a player spot, so the zone it is in
			// and its eight neighbours are loaded whatever the distance in metres.
			const RegionEngine& zones = real.regions();
			const std::int32_t axis = zones.regionsPerAxis();
			const std::int32_t towerZone = zones.regionOf(72673.0, 94252.0);
			std::size_t loadedSquads = 0;
			std::size_t loadedBuilt = 0;
			std::size_t farBuilt = 0;
			real.world().ecs().each([&](flecs::entity e, const state::SquadInstance&, const state::Position& at)
			{
				const std::int32_t zone = zones.regionOf(at.x, at.z);
				if (std::abs(zone % axis - towerZone % axis) > 1 || std::abs(zone / axis - towerZone / axis) > 1)
					return;
				const bool built = e.get<Detail>() != nullptr && e.get<Detail>()->level == DetailLevel::Full;
				++loadedSquads;
				loadedBuilt += built;
				farBuilt += built && std::hypot(at.x - 72673.0, at.z - 94252.0) > 1000.0;
			});
			test::require(loadedSquads >= 1 && loadedBuilt == loadedSquads, "SkinHouse: every squad in a zone loaded round the tower is Full detail after an hour");
			test::require(farBuilt >= 1, "SkinHouse: loaded squads beyond 1000 u are Full too (no radius)");
			std::cout << "ksim_save_import: SkinHouse world state OK\n";
		} else {
			std::cout << "ksim_save_import: KSIM_SKINHOUSE not set, the SkinHouse section is skipped\n";
		}
		std::cout << "ksim_save_import: OK\n";
		return 0;
	} catch (const std::exception& e) {
		if (!dir.empty())
			fs::remove_all(dir);
		std::cerr << "ksim_save_import: FAILED: " << e.what() << "\n";
		return 1;
	}
}
