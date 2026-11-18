// The bare-world rig of the dt27 goal tests (230 / 135 / 176 / 130 / 53): one hand-made world squad built at Full detail with a
// leader (the follow leader, which never takes a goal) and one member, the package forced as a 24 h contract so the test names the
// goal rows the member scores. No orders are ever given to an NPC: the live selector picks the goal and the executor runs it.
// ksim_rearm's rig, with the package, the member's goal list and the squad's home town as inputs.

#pragma once

#include "ksim/ai/blackboard.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/world.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace dt27
{

using namespace ksim;

// Open ground in the Hub's circle (the animal-feed and rearm fixtures' spot).
constexpr double kX = -50978.8;
constexpr double kZ = 2932.48;
constexpr const char* kSquadTemplate = "42258-rebirth.mod"; // any template: the contract below decides the goals

inline flecs::world& ecsOf(Simulation& sim)
{
	return const_cast<flecs::world&>(sim.world().ecs());
}

inline std::string recordSid(const kdata::GameData& gd, std::string_view type, std::string_view name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName(type)))
		if (record->name == name)
			return record->stringID;
	throw std::runtime_error("no " + std::string(type) + " " + std::string(name));
}

// One goal row of the test's package: the AI_TASK `enum`, its window (hours) and weight.
struct Row
{
	std::uint16_t task = 0;
	int start = 0;
	int end = 24;
	int weight = 0;
	int list = -1; // the MemberRole list this row goes on (0 Squad, 2 Leader, 3 Slave); -1 = the rig's member list
};

inline const kdata::Record* taskRecord(const kdata::GameData& gd, std::uint16_t task)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("AI_TASK")))
		if (record->intOr("enum", -1) == task)
			return record;
	return nullptr;
}

// Whether some AI_PACKAGE of the data carries a goal row of `task` (the data check of each case: the goal is not ours alone).
inline bool dataHasRow(const kdata::GameData& gd, std::uint16_t task)
{
	const kdata::Record* wanted = taskRecord(gd, task);
	if (wanted == nullptr)
		return false;
	for (const kdata::Record* package : gd.byType(kworld::itemTypeByName("AI_PACKAGE")))
		for (const char* category : {"Squad AI Goals", "Squad2 AI Goals", "Leader AI Goals"})
			if (const kdata::RefCategory* refs = package->findRefs(category))
				for (const kdata::RefEntry& entry : refs->entries)
					if (entry.targetID == wanted->stringID)
						return true;
	return false;
}

inline constexpr const char* kTestPackage = "dt27-test-package";

// The merged game data plus one synthetic AI_PACKAGE: only the rows under test, on the member's list, so no other goal competes.
inline std::shared_ptr<const kdata::GameData> dataWithPackage(const std::vector<Row>& rows, std::uint8_t memberList)
{
	auto data = std::make_shared<kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
	kdata::Record package;
	package.type = kworld::itemTypeByName("AI_PACKAGE");
	package.stringID = kTestPackage;
	package.name = kTestPackage;
	// An enum the install has no AI_TASK record for (88 / 93 / 113 / 114 / 124 / 152 / 242: no shipped package uses them) gets a synthetic
	// one, so a test can still hand the squad the goal.
	std::vector<kdata::Record> synthetic;
	for (const Row& row : rows)
		if (taskRecord(*data, row.task) == nullptr)
		{
			kdata::Record task;
			task.type = kworld::itemTypeByName("AI_TASK");
			task.stringID = "dt27-synthetic-task-" + std::to_string(row.task);
			task.name = task.stringID;
			task.ints.push_back({"enum", row.task});
			synthetic.push_back(std::move(task));
		}
	if (!synthetic.empty())
	{
		kdata::File extra;
		extra.version = 16;
		extra.records = synthetic;
		data->apply(extra);
	}
	for (const int listId : {0, 2, 3})
	{
		kdata::RefCategory list;
		list.category = listId == 3 ? "Slave AI Goals" : listId == 2 ? "Leader AI Goals" : "Squad AI Goals";
		for (const Row& row : rows)
		{
			const int rowList = row.list >= 0 ? row.list : memberList;
			if ((rowList == 2 || rowList == 3 ? rowList : 0) != listId)
				continue;
			const kdata::Record* task = taskRecord(*data, row.task);
			test::require(task != nullptr, "no AI_TASK of enum " + std::to_string(row.task));
			kdata::RefEntry entry;
			entry.targetID = task->stringID;
			entry.v0 = row.start;
			entry.v1 = row.end;
			entry.v2 = row.weight;
			entry.hasV1V2 = true;
			list.entries.push_back(std::move(entry));
		}
		if (!list.entries.empty())
			package.references.push_back(std::move(list));
	}
	kdata::File file;
	file.version = 16;
	file.records.push_back(std::move(package));
	data->apply(file);
	return data;
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	Simulation sim;
	std::uint64_t rng = 13;
	std::string body;
	flecs::entity squad;
	std::uint64_t leader = 0;
	std::uint64_t member = 0;
	std::vector<std::string> log;

	// `memberList`: the MemberRole list the member takes goals from (routines::Role: 0 Squad, 1 Squad2, 2 Leader).
	Rig(const std::vector<Row>& rows, std::uint8_t memberList, double dayFraction = 0.5, int limbLoss = 1)
		: gd(dataWithPackage(rows, memberList))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::load(KENSHI_DATA, 91))
	{
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		settings.limbLossLevel = limbLoss; // the game's Dismemberment setting
		sim.enableCombat(data, settings);
		sim.setTaskLog([this](const std::string& line) { log.push_back(line); });
		body = recordSid(*gd, "CHARACTER", "Escaped Servant");
		sim.createPlayerFaction(recordSid(*gd, "FACTION", "Nameless"));
		squad = sim.spawnMover(kX, kZ); // through the region engine, as a load ingests it: the first save / reload keeps the hash
		state::SquadInstance instance;
		instance.templateSid = kSquadTemplate;
		squad.set<state::SquadInstance>(std::move(instance));
		squad.set<Detail>(Detail{DetailLevel::Full});
		leader = spawn(body, kX, kZ);
		member = spawn(body, kX + 6.0, kZ);
		join(leader, 2);
		join(member, memberList);
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * dayFraction)); // noon unless the test names an hour
		for (int i = 0; i < 80 && sim.blackboardOf(squad) == nullptr; ++i)
			sim.tick(); // the selector builds the squad's Blackboard on its first pass (every 0.4 s)
		applyPackage(kTestPackage);
	}

	// The package becomes the applied one for 24 h (Contract_Timed): the test names the goal rows, the template does not matter.
	void applyPackage(const std::string& sid)
	{
		blackboard::Board* board = sim.blackboardOf(squad);
		test::require(board != nullptr, "the squad has no Blackboard after a tick");
		blackboard::Package package;
		test::require(blackboard::readPackage(*gd, sid, 99, package), "no AI_PACKAGE " + sid);
		blackboard::pushContract(*board, std::move(package), 24.0, trader::gameHours(sim.clockTick()));
	}

	std::uint64_t spawn(const std::string& sid, double x, double z)
	{
		state::Fighter f = data->makeFighter(*gd, sid, "", rng);
		f.x = x;
		f.z = z;
		f.y = 1647.0;
		f.team = 0;
		f.hunger = 3.0;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& key)
		{
			if (key.value == id)
				found = e;
		});
		return found;
	}
	state::Fighter& fighterOf(std::uint64_t id) { return *const_cast<CombatArena*>(sim.combat())->find(id); }
	// Whether the character is still in the world (a removed one keeps no Fighter).
	bool exists(std::uint64_t id) { return sim.combat()->find(id) != nullptr; }

	void join(std::uint64_t id, std::uint8_t list)
	{
		flecs::entity entity = entityOf(id);
		entity.add<state::MemberOf>(squad);
		state::MemberRole role;
		role.list = list;
		entity.set<state::MemberRole>(role);
		fighterOf(id).squad = squad.get<state::Id>()->value;
	}

	// The goal the member holds now (0 = none).
	std::uint16_t goalOf(std::uint64_t id)
	{
		const state::Routine* routine = entityOf(id).get<state::Routine>();
		return routine != nullptr ? routine->goal : 0;
	}

	bool logged(const std::string& needle) const
	{
		for (const std::string& line : log)
			if (line.find(needle) != std::string::npos)
				return true;
		return false;
	}

	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim.tick();
	}

	// Ticks until `done` holds or `seconds` of game time pass; false on a timeout.
	template <typename Done>
	bool until(double seconds, Done&& done)
	{
		const int steps = static_cast<int>(seconds / kTickSeconds);
		for (int i = 0; i < steps; ++i)
		{
			if (done())
				return true;
			sim.tick();
		}
		return done();
	}
};


// ---- fixtures shared by the goal tests ----

// A coordinate as the task log prints it.
inline std::string fmt(double value)
{
	std::ostringstream out;
	out << value;
	return out.str();
}

struct Town
{
	flecs::entity entity;
	towncircle::Circle circle;
	int type = 2;
};

// A bare world has no TownState: the fixture makes them from the data's TOWN records (their `type` picks the radius), at the places the
// case needs. `type` 0 is a TOWN_NEST.
inline Town makeTown(Rig& rig, int type, double x, double z)
{
	const kdata::Record* record = nullptr;
	for (const kdata::Record* candidate : rig.gd->byType(kworld::itemTypeByName("TOWN")))
		if (static_cast<int>(candidate->intOr("type", 2)) == type)
		{
			record = candidate;
			break;
		}
	test::require(record != nullptr, "no TOWN record of type " + std::to_string(type));
	state::TownState state;
	state.rootSid = record->stringID;
	state.activeSid = record->stringID;
	state.x = x;
	state.z = z;
	flecs::entity entity = state::createInstance(ecsOf(rig.sim));
	entity.set<state::TownState>(state);
	return {entity, towncircle::of(*rig.gd, state), type};
}

// A building whose interior layout holds a cage piece (the site index's own source), as ksim_raid_goals finds it.
// `nearest`: the one closest to (nearX, nearZ), the one a fighter there can reach; else the first in the data.
inline const BuildingSite* findLayoutBuilding(Rig& rig, routines::SiteKind kind, bool nearest = false, double nearX = kX, double nearZ = kZ)
{
	const BuildingSite* best = nullptr;
	const routines::LayoutLibrary layouts = routines::LayoutLibrary::load(rig.sim.world().dataDir());
	for (const BuildingSite& site : rig.sim.world().buildingSites())
	{
		std::string layoutName = site.interiorLayout;
		if (layoutName.empty() && !site.squadTemplate.empty())
			if (const kdata::Record* squad = rig.gd->find(site.squadTemplate))
				layoutName = squad->stringOr("layout interior", "");
		const routines::Layout* layout = layoutName.empty() ? nullptr : layouts.find(site.buildingSid + "-" + layoutName);
		for (std::size_t i = 0; layout != nullptr && i < layout->pieces.size(); ++i)
			if (const kdata::Record* piece = rig.gd->find(layout->pieces[i].target))
				if (routines::classifyPiece(*rig.gd, *piece) == kind)
				{
					if (!nearest)
						return &site;
					if (best == nullptr || std::hypot(site.cx - nearX, site.cz - nearZ) < std::hypot(best->cx - nearX, best->cz - nearZ))
						best = &site;
					break;
				}
	}
	return best;
}

inline const BuildingSite* findCageBuilding(Rig& rig)
{
	return findLayoutBuilding(rig, routines::SiteKind::Cage);
}

// Arms the member for real: the weapon in the hands slot of its inventory and the fighter's weapon built from it.
inline void armMember(Rig& rig)
{
	const kdata::Record* sword = nullptr;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("WEAPON")))
		if (!record->name.empty() && record->intOr("category", 0) >= 0)
		{
			sword = record;
			break;
		}
	test::require(sword != nullptr, "no WEAPON record");
	state::ItemQuality quality;
	quality.level = 1.0;
	quality.grade = 2;
	state::Inventory pack = *rig.entityOf(rig.member).get<state::Inventory>();
	pack.items.clear();
	state::Inventory::Entry entry;
	entry.itemSid = sword->stringID;
	entry.count = 1;
	entry.slot = state::Inventory::kWeapon;
	entry.section = state::Inventory::Entry::kEquipped;
	entry.quality = quality;
	pack.items.push_back(std::move(entry));
	pack.version = 1;
	rig.entityOf(rig.member).set<state::Inventory>(std::move(pack));
	rig.fighterOf(rig.member).weapon = rig.data->buildWeapon(*rig.gd, *sword, quality);
}

// The occupant's worn armour taken off (the peeler strips it before it touches a limb).
inline void undress(Rig& rig, std::uint64_t id)
{
	rig.fighterOf(id).armour.clear();
	state::Inventory pack = *rig.entityOf(id).get<state::Inventory>();
	std::erase_if(pack.items, [](const state::Inventory::Entry& e) { return e.slot >= state::Inventory::kArmourBase && e.slot != state::Inventory::kBackpackSlot; });
	rig.entityOf(id).set<state::Inventory>(std::move(pack));
}

} // namespace dt27
