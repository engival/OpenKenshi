// Containers (interact.hpp, SIMLINK.md section 13; task AL): the interior-layout storage of a real town building is a box,
// the lock a `has lock` piece starts with, and `itemtype limit` on a stand.
//
//  1. the Hub Bar's layout storage (placement 4933, 3384-TwoStorey "Bar"): a Weapon Cabinet and a Safe, both `has lock`, exist
//     as containers inside the building's footprint, keyed so a second furnishing makes no new box;
//  2. a locked box refuses loot and transfers; the player picks it (order action 3, TASK_PROGRESS 39 kind 1 while picking),
//     the box unlocks (its version bumps) and the loot goes through;
//  3. an Armour Stand (`itemtype limit` 3) refuses a weapon and takes an armour piece;
//  4. a save keeps the boxes, their keys, their locks and their contents (the loaded state hash of the straight run).
// argv[1]: the Hub navmesh cache (the ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/ai/routines.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr double kBarX = -51181.0; // the Hub Bar (3384-TwoStorey, placement 4933; ksim_building_membership, ksim_native_nav)
constexpr double kBarZ = 2639.0;
constexpr std::uint8_t kLoot = 5;
constexpr std::uint8_t kPick = 3;

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::uint32_t seq = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const kworld::LoadOrder& order)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		// Orders only for the player: nobody else walks into a box or picks it.
		sim->removeFighter(duel.enemy);
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	state::Fighter& player() { return *const_cast<CombatArena*>(sim->combat())->find(duel.player); }
	flecs::entity playerEntity()
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == duel.player) found = e; });
		return found;
	}
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	void standAt(const state::Container& box)
	{
		state::Fighter& p = player();
		p.x = box.useX;
		p.y = box.useY;
		p.z = box.useZ;
		tick(2);
	}
	flecs::entity boxEntity(std::uint64_t id)
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::Id& i, const state::Container&) { if (i.value == id) found = e; });
		return found;
	}
	std::string recordNamed(const char* type, const char* name) const
	{
		for (const kdata::Record& record : gd->records())
			if (record.type == kworld::itemTypeByName(type) && record.name == name)
				return record.stringID;
		throw std::runtime_error(std::string("no ") + type + " '" + name + "'");
	}
	const kdata::Record* houseContents() const
	{
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("VENDOR_LIST")))
			if (record->name == "house contents")
				return record;
		return nullptr;
	}
	std::size_t containerCount()
	{
		std::size_t n = 0;
		ecs().each([&](const state::Container&) { ++n; });
		return n;
	}
};

// The order as a session makes it, standing at the use point (every order below is given from there).
void order(Rig& rig, std::uint64_t target, std::uint8_t action)
{
	rig.sim->enqueue(Command::fighterInteract(state::Id{rig.duel.player}, target, action, ++rig.seq, {}, {}, false));
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_containers HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order_ = kworld::vanillaLoadOrder(KENSHI_DATA);

		Rig rig(gd, data, nav, order_);
		test::require(rig.sim->interiorLayouts() != nullptr, "the simulation has no interior layouts");

		const BuildingSite* bar = nullptr;
		for (const BuildingSite& site : rig.sim->world().buildingSites())
			if (site.interiorLayout == "Bar" && std::hypot(site.originX - kBarX, site.originZ - kBarZ) < 30.0)
				bar = &site;
		test::require(bar != nullptr, "no building site at the Hub Bar");
		const std::string cabinetRecord = rig.recordNamed("BUILDING", "Weapon Cabinet");
		const std::string safeRecord = rig.recordNamed("BUILDING", "Safe");

		// ---- 1. the Bar's layout storage, at its pieces ----
		std::vector<std::pair<std::uint64_t, const state::Container*>> barBoxes;
		rig.ecs().each([&](const state::Id& i, const state::Container& box)
		{
			if (insideFootprint(*bar, {box.x, box.z}))
				barBoxes.push_back({i.value, &box});
		});
		test::require(barBoxes.size() >= 8, "the Hub Bar has " + std::to_string(barBoxes.size()) + " boxes, not its layout's storage");
		std::uint64_t cabinet = 0;
		std::uint64_t safe = 0;
		for (const auto& [id, box] : barBoxes)
		{
			test::require(box->key != 0, "a layout box has no key");
			// The building's two floors (the layout's y 4.38 and 70.16 over the placement's 1576.1, on its tilt).
			test::require(box->y > 1570.0 && box->y < 1660.0, "a Bar box is at y " + std::to_string(box->y));
			if (box->recordSid == cabinetRecord && box->locked && cabinet == 0)
			{
				cabinet = id;
				test::require(box->y < 1600.0, "the Weapon Cabinet is not on the Bar's ground floor (y " + std::to_string(box->y) + ")");
			}
			if (box->recordSid == safeRecord && box->locked && safe == 0)
			{
				safe = id;
				test::require(box->y < 1600.0, "the Safe is not on the Bar's ground floor (y " + std::to_string(box->y) + ")");
			}
		}
		test::require(cabinet != 0, "the Bar's locked Weapon Cabinet is not a locked box");
		test::require(safe != 0, "the Bar's locked Safe is not a locked box");
		std::cout << "layout boxes: " << barBoxes.size() << " in the Hub Bar, the cabinet " << cabinet << " and the safe " << safe << " locked\n";

		// ---- the original's container and seeding rule (rwh.146, k16.6.256): every Hub box is a functionality-class container; a function 2
		// box holds only its `limit inventory`; only functions 2 / 9 / 13 are seeded; weapon cabinets / armour stands stay empty of
		// house goods; a water tank (limit Water) holds nothing ----
		{
			std::size_t all = 0;
			std::size_t water = 0;
			rig.ecs().each([&](const state::Container& box, const state::Inventory& held)
			{
				const kdata::Record* record = gd->find(box.recordSid);
				test::require(record != nullptr, "a box with no record");
				std::int32_t function = -1;
				if (const kdata::RefCategory* functions = record->findRefs("functionality"))
					for (const kdata::RefEntry& entry : functions->entries)
						if (const kdata::Record* f = entry.v0 == kdata::kReferenceRemoved ? nullptr : gd->find(entry.targetID))
						{
							function = f->intOr("function", 0);
							break;
						}
				test::require(function >= 0, record->name + " is a container with no functionality record");
				++all;
				if (function != 2 && function != 9 && function != 13)
					test::require(held.items.empty(), record->name + " (function " + std::to_string(function) + ") was seeded");
				std::vector<std::string> only;
				if (const kdata::RefCategory* refs = record->findRefs("limit inventory"))
					for (const kdata::RefEntry& entry : refs->entries)
						if (entry.v0 != kdata::kReferenceRemoved)
							only.push_back(entry.targetID);
				for (const state::InventoryEntry& item : held.items)
				{
					if (function == 2 && !only.empty())
						test::require(std::find(only.begin(), only.end(), item.itemSid) != only.end(), record->name + " holds " + item.itemSid + " outside its limit inventory (" + (only.empty() ? std::string("none") : only.front() + "...") + ", " + std::to_string(only.size()) + " listed)");
					const std::int32_t limit = record->intOr("itemtype limit", 0);
					const kdata::Record* itemRecord = gd->find(item.itemSid);
					if (limit == 2 || limit == 3)
						test::require(itemRecord != nullptr && itemRecord->type == kworld::itemTypeByName(limit == 2 ? "WEAPON" : "ARMOUR"), record->name + " holds the wrong item type");
				}
				if (record->name.find("Water Tank") != std::string::npos || record->name.find("Water Tower") != std::string::npos)
				{
					++water;
					test::require(held.items.empty(), record->name + " carries seeded items");
				}
			});
			test::require(all != 0, "no Hub boxes");
			std::cout << "container rule: " << all << " Hub boxes follow it, " << water << " water tanks empty\n";
		}

		// ---- the wire tail (k16.7.29): a layout box names its building and the piece's index in the DATA's list, every entry counted ----
		{
			// The layouts as the data reads them (the level files, parsed here, not through the library that keeps the entries it can use).
			kdata::GameData levels;
			for (const std::string& path : routines::LayoutLibrary::levelFiles(KENSHI_DATA))
			{
				std::ifstream in(path, std::ios::binary);
				const std::vector<char> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				std::vector<std::byte> bytes(raw.size());
				std::memcpy(bytes.data(), raw.data(), raw.size());
				levels.apply(kdata::parse(bytes, std::filesystem::path(path).filename().string()));
			}
			std::size_t checked = 0;
			rig.ecs().each([&](const state::Container& box)
			{
				if (box.piece == state::Container::kNoPiece || !insideFootprint(*bar, {box.x, box.z}))
					return;
				test::require(box.building == bar->key, "a Bar box names another building");
				const routines::Layout* layout = rig.sim->interiorLayouts()->find(bar->buildingSid + "-" + bar->interiorLayout);
				const kdata::Record* collection = levels.find(layout->sid);
				test::require(collection != nullptr && box.piece < collection->instances.size(), "the piece index is not in the data's list");
				test::require(collection->instances[box.piece].targetStringID == box.recordSid, "the data's entry " + std::to_string(box.piece) + " is not the box's record");
				++checked;
			});
			test::require(checked >= 8, "only " + std::to_string(checked) + " Bar boxes carry a piece index");
			// Every layout in the library: the kept pieces' data index points at their own record, and none is shifted by a dropped entry.
			std::size_t shifted = 0;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				const routines::Layout* layout = site.interiorLayout.empty() ? nullptr : rig.sim->interiorLayouts()->find(site.buildingSid + "-" + site.interiorLayout);
				const kdata::Record* collection = layout != nullptr ? levels.find(layout->sid) : nullptr;
				if (collection == nullptr)
					continue;
				for (const routines::LayoutPiece& piece : layout->pieces)
				{
					test::require(piece.index < collection->instances.size() && collection->instances[piece.index].targetStringID == piece.target,
						"a kept piece's data index is not its entry");
					if (&piece - layout->pieces.data() != piece.index)
						++shifted;
				}
			}
			std::cout << "pieces whose data index differs from their kept position: " << shifted << "\n";
		}

		// A second furnishing of the same zones makes no new box (the key finds the one there is).
		const std::size_t before = rig.containerCount();
		spawnMeshContainers(*rig.sim, *nav, rig.houseContents());
		test::require(rig.containerCount() == before, "a second furnishing made " + std::to_string(rig.containerCount() - before) + " new boxes");

		// ---- 2. the lock: refused loot and transfer, the pick, the loot ----
		{
			const state::Container* box = rig.boxEntity(cabinet).get<state::Container>();
			test::require(box->lockLevel >= 1 && box->lockLevel <= 100, "a lock level out of 1..100: " + std::to_string(box->lockLevel));
			test::require(box->lockInterval == kPickFirstInterval && box->lockAttempts == 0,
				"a fresh lock does not start its first attempt at 5 s");
			const state::Inventory* contents = rig.boxEntity(cabinet).get<state::Inventory>();
			const std::string item = contents != nullptr && !contents->items.empty() ? contents->items.front().itemSid : std::string();
			const InteractTarget target = findInteractTarget(rig.ecs(), rig.sim->combat(), cabinet, rig.sim.get());
			test::require(checkInteract(rig.ecs(), *rig.sim->combat(), rig.duel.player, target, kLoot, 0, gd.get(), rig.sim.get()) == "locked",
				"a locked box allows loot");
			test::require(checkInteract(rig.ecs(), *rig.sim->combat(), rig.duel.player, target, kPick, 0, gd.get(), rig.sim.get()).empty(),
				"a locked box refuses the pick");
			rig.standAt(*box);
			// In reach of the box, the lock is the answer (out of reach, the range would come first).
			if (!item.empty())
				test::require(rig.sim->checkTransfer(rig.duel.player, cabinet, item, 1, true) == "locked",
					"a transfer takes from a locked box");
			const std::uint32_t version0 = box->version;
			state::Fighter& p = rig.player();
			p.skills.lockpicking = 100.0; // chance 0.9 whatever the level (door_plan.hpp pickChance)
			order(rig, cabinet, kPick);
			int ticks = 0;
			bool sawProgress = false;
			while (box->locked && ticks++ < 6000)
			{
				rig.tick();
				if (!sawProgress)
				{
					for (const Simulation::TaskProgressRow& row : rig.sim->taskProgress())
						sawProgress = row.kind == 1 && row.id == rig.duel.player;
				}
			}
			test::require(!box->locked, "the pick never opened the box");
			test::require(sawProgress, "the pick sent no TASK_PROGRESS 39 kind 1");
			test::require(box->version > version0, "unlocking the box bumped no version");
			std::cout << "pick: the cabinet opened after " << ticks * kTickSeconds << " s, " << box->lockAttempts << " attempts, key " << box->key << "\n";

			// The lock is open now: the loot goes through.
			std::size_t kinds = 0;
			std::uint32_t units = 0;
			if (const state::Inventory* held = rig.boxEntity(cabinet).get<state::Inventory>())
			{
				kinds = held->items.size();
				for (const state::Inventory::Entry& e : held->items)
					units += e.count;
			}
			order(rig, cabinet, kLoot);
			for (int waited = 0; rig.boxEntity(cabinet).get<state::Inventory>()->items.size() != 0 && waited < 800; ++waited)
				rig.tick();
			test::require(rig.boxEntity(cabinet).get<state::Inventory>()->items.empty(), "the loot left items in the unlocked box");
			std::cout << "loot: " << kinds << " stacks (" << units << " units) out of the cabinet\n";
		}

		// ---- 3. `itemtype limit`: an Armour Stand refuses a weapon ----
		{
			const state::Fighter& at = rig.player();
			state::Container stand;
			stand.recordSid = rig.recordNamed("BUILDING", "Armour Stand"); // `itemtype limit` 3
			stand.x = stand.useX = at.x;
			stand.y = stand.useY = at.y;
			stand.z = stand.useZ = at.z;
			const std::uint64_t standId = rig.sim->spawnContainer(std::move(stand), state::Inventory{}).get<state::Id>()->value;
			rig.tick(2);
			const std::string katana = rig.recordNamed("WEAPON", "Katana");
			const std::string armour = [&]()
			{
				for (const kdata::Record& record : gd->records())
					if (record.type == kworld::itemTypeByName("ARMOUR"))
						return record.stringID;
				throw std::runtime_error("no ARMOUR record");
			}();
			flecs::entity player = rig.playerEntity();
			state::Inventory pack = *player.get<state::Inventory>();
			pack.add(katana, 1, state::Inventory::kCarried);
			pack.add(armour, 1, state::Inventory::kCarried);
			++pack.version;
			player.set<state::Inventory>(std::move(pack));
			test::require(rig.sim->checkTransfer(rig.duel.player, standId, katana, 1, false) == "item_type",
				"an Armour Stand takes a weapon");
			test::require(rig.sim->checkTransfer(rig.duel.player, standId, armour, 1, false).empty(),
				"an Armour Stand refuses an armour piece");
			rig.sim->enqueue(Command::containerTransfer(state::Id{rig.duel.player}, standId, armour, 1, false, ++rig.seq));
			rig.tick(4);
			const state::Inventory* held = rig.boxEntity(standId).get<state::Inventory>();
			test::require(held != nullptr && held->carried(armour) == 1, "the armour piece did not go onto the stand");
			test::require(player.get<state::Inventory>()->carried(katana) == 1, "the Katana left the pack for the stand");
			std::cout << "itemtype limit: the Armour Stand refused the Katana and took " << armour << "\n";

			// ---- 3b. the reasons a container order can name (task AX): locked, out_of_reach, full, not_allowed ----
			// `item_type` answered above, `locked` in section 2; these are the rest, each once.
			state::Fighter& actor = rig.player();
			const double keepX = actor.x;
			actor.x += 1000.0;
			test::require(rig.sim->checkTransfer(rig.duel.player, standId, armour, 1, false) == "out_of_reach",
				"a stand 1000 u away answers not out_of_reach");
			actor.x = keepX;
			// A box by the actor, locked first and then with no room for the stack (1 x 1, the Katana longer).
			state::Container tiny;
			tiny.recordSid = cabinetRecord;
			tiny.locked = true;
			tiny.x = tiny.useX = actor.x;
			tiny.y = tiny.useY = actor.y;
			tiny.z = tiny.useZ = actor.z;
			const std::uint64_t tinyId = rig.sim->spawnContainer(std::move(tiny), state::Inventory{}).get<state::Id>()->value;
			rig.tick(2);
			test::require(rig.sim->checkTransfer(rig.duel.player, tinyId, katana, 1, false) == "locked",
				"a locked box answers not locked");
			flecs::entity tinyBox = rig.boxEntity(tinyId);
			state::Container& tinyLock = *const_cast<state::Container*>(tinyBox.get<state::Container>());
			tinyLock.locked = false;
			state::Inventory oneCell = *tinyBox.get<state::Inventory>();
			oneCell.gridW = oneCell.gridH = 1;
			tinyBox.set<state::Inventory>(std::move(oneCell));
			test::require(rig.sim->checkTransfer(rig.duel.player, tinyId, katana, 1, false) == "full",
				"a box with no room answers not full");
			// The actor's own state stays `not_allowed`: a downed character moves nothing.
			actor.unconscious = true;
			test::require(rig.sim->checkTransfer(rig.duel.player, tinyId, katana, 1, false) == "not_allowed",
				"a downed actor answers not not_allowed");
			actor.unconscious = false;
			std::cout << "refusals: locked, item_type, out_of_reach, full, not_allowed\n";
		}

		// ---- 4. save and load keep the boxes, their locks and their contents ----
		{
			const state::Container* open = rig.boxEntity(cabinet).get<state::Container>();
			const std::uint64_t cabinetKey = open->key;
			test::require(!open->locked, "the cabinet was not left open for the save");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_containers.save";
			rig.sim->saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order_, path);
			CombatSettings settings;
			settings.substeps = 2;
			loaded.enableCombat(data, settings);
			loaded.enableNavigation(nav);
			std::size_t boxes = 0;
			bool cabinetOpen = false;
			bool safeLocked = false;
			loaded.world().ecs().each([&](const state::Container& box)
			{
				++boxes;
				if (box.key == cabinetKey)
					cabinetOpen = !box.locked;
				if (box.recordSid == safeRecord && box.locked)
					safeLocked = true;
			});
			test::require(boxes == rig.containerCount(), "the save lost containers");
			test::require(cabinetOpen, "the picked cabinet came back locked");
			test::require(safeLocked, "the Safe came back unlocked");
			test::require(loaded.stateHash() == rig.sim->stateHash(), "the loaded boxes differ from the saved ones");
			std::remove(path.c_str());
			std::cout << "save: " << boxes << " boxes back, the cabinet open, the Safe still locked, hash " << loaded.stateHash() << "\n";
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_containers: " << e.what() << "\n";
		return 1;
	}
}
