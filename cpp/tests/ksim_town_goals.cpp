// The in-town routines, pure part (routines.hpp, TOWN_LIFE_SLICE.md section 5, T3a): the package compile on the real merged data and on
// synthetic records, the hour windows, the 24 h no-goal-less sweep over every package, the site index on a real interior layout and on a
// bare building, and the reservation book.

#include "ksim/ai/routines.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ksim::routines;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

constexpr std::int32_t kAiTask = 22;
constexpr std::int32_t kAiPackage = 72;

const kdata::Record* byName(const kdata::GameData& data, std::int32_t type, std::string_view name)
{
	for (const kdata::Record* record : data.byType(type))
		if (record->name == name)
			return record;
	return nullptr;
}

const CompiledPackage& package(const PackageTable& table, const kdata::GameData& data, std::string_view name)
{
	const kdata::Record* record = byName(data, kAiPackage, name);
	test::require(record != nullptr, "package " + std::string(name));
	const CompiledPackage* found = table.find(record->stringID);
	test::require(found != nullptr, "compiled " + std::string(name));
	return *found;
}

// "task:start-end" for the rows of `role` whose task is in `tasks`, in list order.
std::vector<std::string> rows(const CompiledPackage& pkg, Role role, const std::set<int>& tasks)
{
	std::vector<std::string> out;
	for (const GoalRow& row : pkg.list(role))
		if (tasks.contains(row.task))
			out.push_back(std::to_string(row.task) + ":" + std::to_string(row.start) + "-" + std::to_string(row.end));
	return out;
}

using Rows = std::vector<std::string>;

// ---- synthetic game data ----

kdata::Record task(const std::string& sid, int taskEnum, int classification)
{
	kdata::Record record;
	record.type = kAiTask;
	record.stringID = sid;
	record.name = sid;
	record.ints.push_back({"enum", taskEnum});
	record.ints.push_back({"classification", classification});
	return record;
}

struct Ref
{
	const char* target;
	int start;
	int end;
	int weight;
};

void list(kdata::Record& record, const char* category, const std::vector<Ref>& refs)
{
	kdata::RefCategory entries;
	entries.category = category;
	for (const Ref& ref : refs)
	{
		kdata::RefEntry entry;
		entry.targetID = ref.target;
		entry.v0 = ref.start;
		entry.v1 = ref.end;
		entry.v2 = ref.weight;
		entry.hasV1V2 = true;
		entries.entries.push_back(entry);
	}
	record.references.push_back(std::move(entries));
}

kdata::Record pkg(const std::string& sid)
{
	kdata::Record record;
	record.type = kAiPackage;
	record.stringID = sid;
	record.name = sid;
	return record;
}

kdata::GameData synthetic(std::vector<kdata::Record> records)
{
	kdata::File file;
	file.version = 16;
	file.records = std::move(records);
	kdata::GameData data;
	data.apply(file);
	return data;
}

void windows()
{
	check(windowHolds(8, 20, 8.0) && windowHolds(8, 20, 19.99) && !windowHolds(8, 20, 7.99) && !windowHolds(8, 20, 20.0), "window [8, 20): start in, end out");
	check(windowHolds(23, 6, 23.0) && windowHolds(23, 6, 0.0) && windowHolds(23, 6, 5.99) && !windowHolds(23, 6, 6.0) && !windowHolds(23, 6, 12.0)
		&& !windowHolds(23, 6, 22.99), "window 23-6 wraps midnight");
	check(windowHolds(0, 100, 0.0) && windowHolds(0, 100, 23.99) && windowHolds(12, 280, 3.0), "end > 24 is always on");
	check(windowHolds(0, 0, 0.0) && windowHolds(0, 0, 13.5) && windowHolds(0, 24, 23.99), "0-0 is always on (TQ19 default), 0-24 covers the day");
	GoalRow row;
	check(effectiveWeight(row) == 1.0, "weight 0 is 1.0 (item 57)");
	row.weight = 80;
	check(std::abs(effectiveWeight(row) - 0.8) < 1e-12, "weight val2 80 is 0.8");
	check(holdOf(task::GoHomeToBed).base == 2.0 && holdOf(task::GoHomeToBedSecure).span == 8.0 && holdOf(task::Shopping).span == 1.5
		&& holdOf(task::StayInHome).base == 0.2 && holdOf(task::LockAllMyDoors).span == 0.0, "holds: bed 2+8, shopping 0.25+1.5, STAY_IN_HOME 0.2+0.8, doors none");
	// Task CH group 3 (dt24_38 section 1): the guard-node goals' duration stores, and 268 the same triple as 38.
	check(holdOf(task::StaticGuardBuilding).base == 0.1 && holdOf(task::StaticGuardBuilding).span == 0.5
		&& holdOf(task::StaticGuardBuildingIndoors).base == 0.1 && holdOf(task::StaticGuardBuildingIndoors).span == 0.5
		&& holdOf(task::StaticGuardTown).base == 0.1 && holdOf(task::StaticGuardTown).span == 1.0,
		"holds: node 0.1+0.5 (38 and 268), post 0.1+1.0 (157)");
	// Task CH turrets: the turret rows' duration stores were not decoded, so they take the node hold (UNSURE, dt24_turret_finder).
	check(holdOf(task::ManTurrets).base == 0.1 && holdOf(task::ManTurrets).span == 0.5
		&& holdOf(task::TurretOnBuilding).base == 0.1 && holdOf(task::TurretOnBuilding).span == 0.5, "holds: turrets 0.1+0.5 (UNSURE)");
}

void taskTableChecks()
{
	const std::vector<TaskInfo>& table = taskTable();
	bool sorted = true;
	for (std::size_t i = 1; i < table.size(); ++i)
		sorted = sorted && table[i - 1].task < table[i].task;
	check(sorted, "the goal table is sorted and unique by task (taskInfo binary-searches it)");
	check(taskInfo(task::StayInHome) != nullptr && taskInfo(task::StayInHome)->use == TaskUse::Modelled, "STAY_IN_HOME 43 is modelled");
	check(taskInfo(task::PickupIntruders)->use == TaskUse::Adopted && runsGoal(task::PickupIntruders), "223 adopts stepIntruders");
	check(taskInfo(task::PickupIntrudersTown)->use == TaskUse::Adopted && runsGoal(task::PickupIntrudersTown)
		&& taskInfo(task::TakeIntruderOutsideTown)->use == TaskUse::Adopted, "273/274 are the town scope of the same executor");
	check(taskInfo(task::StaticGuardBuildingIndoors)->use == TaskUse::Modelled && runsGoal(task::StaticGuardBuildingIndoors)
		&& phaseOfTask(task::StaticGuardBuildingIndoors) == TownPhase::Post, "268 is 38's indoors-only twin, modelled, at the post phase");
	// Task CH turrets (dt24_turret_finder): 149 and 222 run the finder; 234 is the player job (no AI task row, no scorer entry).
	check(taskInfo(task::ManTurrets)->use == TaskUse::Modelled && runsGoal(task::ManTurrets)
		&& phaseOfTask(task::ManTurrets) == TownPhase::Post, "149 runs the turret finder at the post phase");
	check(taskInfo(task::TurretOnBuilding)->use == TaskUse::Modelled && runsGoal(task::TurretOnBuilding)
		&& phaseOfTask(task::TurretOnBuilding) == TownPhase::Post, "222 runs the 234 finder body at the post phase");
	check(taskInfo(234) == nullptr && !runsGoal(234), "234 is a player job: no AI task row");
	check(taskInfo(154)->use == TaskUse::Never && taskInfo(117)->use == TaskUse::Never && taskInfo(193)->use == TaskUse::Never
		&& taskInfo(24)->use == TaskUse::Never, "the four dead values are Never (37 is modelled since kenshi-k16.6.12.8)");
	check(taskInfo(37)->use == TaskUse::Modelled && taskInfo(47)->use == TaskUse::Modelled && taskInfo(51)->use == TaskUse::Modelled,
		"37 / 47 / 51 are modelled: the node DB's general nodes");
	check(taskInfo(999) == nullptr && !runsGoal(999), "an unlisted task is unmodelled, not an error");
}

void compileReal(const kdata::GameData& data, const PackageTable& table)
{
	check(table.packages().size() == data.byType(kAiPackage).size() && table.packages().size() >= 219, "one compiled package per AI_PACKAGE record");
	check(table.missingTasks() == 0 && table.inheritCycles() == 0, "the merged data has no dangling AI_TASK ref and no inherit cycle");

	// The 5.5 table, on the goals it lists.
	const Rows shopLeaderGoals = {"20:6-24", "155:0-6", "86:6-23", "239:23-6"};
	const std::set<int> shopTasks = {20, 86, 155, 239};
	const CompiledPackage& shop = package(table, data, "Shop");
	check(rows(shop, Role::Leader, shopTasks) == shopLeaderGoals, "Shop leader: keeper 6-24, OPEN 6-23, LOCK 0-6, bed (secure) 23-6");
	check(rows(shop, Role::Squad, shopTasks) == Rows({"239:0-6", "155:0-6"}), "Shop squad: LOCK 0-6, bed 0-6");
	check(rows(shop, Role::Squad2, shopTasks) == rows(shop, Role::Squad, shopTasks) && shop.readsFrom[1] == Role::Squad, "Shop squad2 reads the squad list (fallback)");

	const CompiledPackage& civilian = package(table, data, "Civilian");
	const std::set<int> civilianTasks = {43, 86, 118, 155, 239};
	check(rows(civilian, Role::Leader, civilianTasks) == Rows({"155:20-8", "86:8-20", "43:0-24", "239:21-8"}), "Civilian leader: STAY_IN_HOME, OPEN 8-20, LOCK 20-8, secure bed 21-8");
	check(rows(civilian, Role::Squad, civilianTasks) == Rows({"118:8-17", "43:0-24", "239:21-8"}), "Civilian squad: SHOPPING 8-17, STAY_IN_HOME, secure bed 21-8");
	check(rows(civilian, Role::Squad2, civilianTasks) == rows(civilian, Role::Squad, civilianTasks), "Civilian squad2: via fallback");

	const std::set<int> guardTasks = {36, 38, 149, 153, 157};
	const CompiledPackage& town = package(table, data, "Guard town");
	check(rows(town, Role::Leader, guardTasks) == Rows({"38:8-20", "153:20-8"}), "Guard town leader: node 38 8-20, bed 20-8");
	check(rows(town, Role::Squad, guardTasks) == Rows({"157:12-22", "153:23-11", "36:12-22", "149:12-22"}), "Guard town squad: post 12-22, patrol 12-22, turrets 12-22, bed 23-11");
	check(rows(town, Role::Squad2, guardTasks) == Rows({"36:12-22", "153:12-22", "157:23-11", "149:23-11"}), "Guard town squad2: post 23-11, turrets 23-11, patrol 12-22, bed 12-22");
	for (const char* name : {"Guard town - slave catching", "guard town - lawful arrests"})
		check(rows(package(table, data, name), Role::Leader, {38, 153}) == Rows({"38:8-20", "153:20-8"}), std::string(name) + " keeps the Guard town leader shifts");

	const CompiledPackage& patrol = package(table, data, "Guard military patrol base");
	check(rows(patrol, Role::Leader, guardTasks) == Rows({"38:8-20", "153:20-8"}), "military patrol base leader: node 8-20, bed 20-8");
	check(rows(patrol, Role::Squad, guardTasks) == Rows({"157:11-23", "36:0-100", "149:0-24"}), "military patrol base squad: post 11-23, patrol always, no bed");
	check(rows(patrol, Role::Squad2, guardTasks) == Rows({"36:0-100", "153:11-23", "157:23-11", "149:0-24"}), "military patrol base squad2: post 23-11, patrol, bed 11-23");

	const std::set<int> wardenTasks = {20, 38, 153, 195};
	const CompiledPackage& warden = package(table, data, "Cage Warden");
	check(rows(warden, Role::Leader, wardenTasks) == Rows({"38:0-24", "153:23-5", "195:22-24", "20:0-24"}), "Cage Warden leader: node, bed 23-5, LOCK_CAGES 22-24, Shopkeeper");
	check(rows(warden, Role::Squad, wardenTasks) == Rows({"153:10-19", "38:0-24", "195:19-21"}), "Cage Warden squad: node, LOCK_CAGES 19-21, bed 10-19");
	check(rows(warden, Role::Squad2, wardenTasks) == rows(warden, Role::Squad, wardenTasks), "Cage Warden squad2: via fallback");

	// The merge of `inherits from`: the night shifts addon is part of Guard building, residents, its rows first.
	const CompiledPackage& residents = package(table, data, "Guard building, residents");
	const CompiledPackage& night = package(table, data, "addon- night shifts");
	check(rows(night, Role::Squad, {153}) == Rows({"153:22-10"}) && rows(night, Role::Squad2, {153}) == Rows({"153:10-22"}) && rows(night, Role::Leader, {153}) == Rows({"153:23-8"}), "night shifts: bed 22-10 / 10-22 / 23-8");
	check(rows(residents, Role::Squad, {153}) == Rows({"153:22-10"}) && rows(residents, Role::Squad2, {153}) == Rows({"153:10-22"})
		&& rows(residents, Role::Leader, {153}) == Rows({"153:23-8"}), "Guard building, residents: the night shifts beds merged in");
	check(residents.list(Role::Squad).front().task == task::GoHomeToBed && residents.list(Role::Squad).front().start == 22, "the base (addon) rows come first");
	check(rows(residents, Role::Squad, {38, 43, 36}) == Rows({"38:0-100", "36:0-24", "43:0-24"}), "Guard building, residents squad: node 38, patrol, STAY_IN_HOME");

	// A leader-only addon reaches every role through the fallback: the real lockup addon has only a Leader list.
	const CompiledPackage& lockup = package(table, data, "addon- lockup and sleep at night");
	check(lockup.readsFrom[0] == Role::Leader && lockup.readsFrom[1] == Role::Leader && lockup.list(Role::Squad2).size() == lockup.list(Role::Leader).size()
		&& rows(lockup, Role::Squad, {155, 239, 86}) == Rows({"155:22-6", "239:21-7", "86:7-20"}), "lockup addon: LOCK 22-6, secure bed 21-7, OPEN 7-20 reach every role");

	const CompiledPackage& squatters = package(table, data, "Squatters");
	check(squatters.readsFrom[0] == Role::Leader && squatters.readsFrom[1] == Role::Leader, "Squatters is leader-only: squad and squad2 read the leader list");
	check(rows(squatters, Role::Squad2, {43, 36}) == Rows({"36:0-100", "43:0-24"}), "Squatters squad2: PATROL_TOWN and STAY_IN_HOME via the fallback");

	// Slaves never fall back.
	check(shop.list(Role::Slave).size() == 1 && civilian.list(Role::Slave).empty(), "the slave list is its own; it has no fallback");
}

void compileSynthetic()
{
	// Fallback: Squad <- Leader; Squad2 <- Squad; and an addon that has only a Leader list reaches every member.
	{
		kdata::Record a = task("t_a", 43, 2);
		kdata::Record b = task("t_b", 38, 2);
		kdata::Record addon = pkg("addon");
		list(addon, "Leader AI Goals", {{"t_a", 0, 24, 0}});
		kdata::Record base = pkg("base");
		list(base, "inherits from", {{"addon", 0, 0, 0}});
		kdata::Record own = pkg("own");
		list(own, "inherits from", {{"addon", 0, 0, 0}});
		list(own, "Squad AI Goals", {{"t_b", 8, 20, 5}});
		list(own, "Leader AI Goals", {{"t_b", 0, 24, 0}});
		kdata::Record none = pkg("none");
		const kdata::GameData data = synthetic({a, b, addon, base, own, none});
		const PackageTable table(data);
		const CompiledPackage& viaAddon = *table.find("base");
		check(viaAddon.list(Role::Leader).size() == 1 && viaAddon.list(Role::Squad).size() == 1 && viaAddon.list(Role::Squad2).size() == 1 && viaAddon.list(Role::Slave).empty(),
			"a leader-only addon reaches Leader, Squad and Squad2 (Squad <- Leader, Squad2 <- Squad)");
		check(viaAddon.readsFrom[0] == Role::Leader && viaAddon.readsFrom[1] == Role::Leader, "and the report says where they read from");
		const CompiledPackage& mixed = *table.find("own");
		check(mixed.list(Role::Leader).size() == 2 && mixed.list(Role::Leader)[0].task == 43 && mixed.list(Role::Leader)[1].task == 38, "the addon row comes first, then the package's own rows");
		check(mixed.list(Role::Squad).size() == 1 && mixed.list(Role::Squad)[0].task == 38 && mixed.readsFrom[0] == Role::Squad, "a non-empty Squad list does not take the addon's Leader row (the fallback is for empty lists)");
		check(mixed.list(Role::Squad2).size() == 1 && mixed.readsFrom[1] == Role::Squad, "Squad2 <- Squad after the merge");
		check(mixed.list(Role::Squad)[0].weight == 5 && mixed.list(Role::Squad)[0].start == 8 && mixed.list(Role::Squad)[0].end == 20, "start, end and weight come from the ref's v0, v1, v2");
		check(table.find("none")->empty() && std::string(routineStatus(*table.find("none"))) == "none", "a package with no list has status none");
	}
	// Append, dedupe on the task record (dt19 section 4, task BY): a child's row for a task the base already lists is skipped
	// and the base's window stands; the child's other rows append after it. Tiers come from the task record.
	{
		kdata::Record urgent = task("t_u", 143, 3);
		kdata::Record fluff = task("t_f", 36, 1);
		kdata::Record other = task("t_o", 43, 2);
		kdata::Record parent = pkg("parent");
		list(parent, "Leader AI Goals", {{"t_u", 0, 24, 0}, {"t_f", 12, 22, 0}});
		kdata::Record child = pkg("child");
		list(child, "inherits from", {{"parent", 0, 0, 0}});
		list(child, "Leader AI Goals", {{"t_f", 6, 10, 0}, {"t_o", 14, 18, 0}});
		const kdata::GameData data = synthetic({urgent, fluff, other, parent, child});
		const PackageTable table(data);
		const std::vector<GoalRow>& leader = table.find("child")->list(Role::Leader);
		check(leader.size() == 3 && leader[0].taskSid == "t_u" && leader[1].start == 12 && leader[2].taskSid == "t_o",
			"a child's row for the base's task is skipped (the base's window stands); its new row appends");
		check(leader[0].tier == Tier::Urgent && leader[1].tier == Tier::Fluff && leader[2].tier == Tier::NonUrgent, "the tier is the AI_TASK classification");
	}
	// Cycle guard, and a dangling task ref is skipped and counted.
	{
		kdata::Record t = task("t_c", 43, 2);
		kdata::Record a = pkg("pa");
		list(a, "inherits from", {{"pb", 0, 0, 0}});
		list(a, "Leader AI Goals", {{"t_c", 0, 24, 0}, {"missing", 0, 24, 0}});
		kdata::Record b = pkg("pb");
		list(b, "inherits from", {{"pa", 0, 0, 0}});
		list(b, "Squad AI Goals", {{"t_c", 0, 24, 0}});
		const kdata::GameData data = synthetic({t, a, b});
		const PackageTable table(data);
		check(table.inheritCycles() >= 1, "an inherit cycle is cut and counted");
		check(table.missingTasks() >= 1 && table.find("pa")->list(Role::Leader).size() == 1, "a dangling AI_TASK ref is skipped and counted");
	}
}

void sweep(const kdata::GameData& data, const PackageTable& table)
{
	// No terminal state: every package x role x half hour has a candidate, the last is the fallback, every other one is in its window.
	std::size_t checked = 0;
	std::size_t fallbackOnly = 0;
	bool ok = true;
	for (const CompiledPackage& pkg : table.packages())
		for (int r = 0; r < kRoleCount; ++r)
			for (int half = 0; half < 48; ++half)
			{
				const double hour = half / 2.0;
				std::size_t unmodelled = 0;
				const std::vector<Candidate> candidates = candidatesAt(pkg, static_cast<Role>(r), hour, &unmodelled);
				++checked;
				ok = ok && !candidates.empty() && candidates.back().fallback && candidates.back().task == task::StayInHome;
				for (std::size_t i = 0; i + 1 < candidates.size(); ++i)
					ok = ok && !candidates[i].fallback && candidates[i].row != nullptr && windowHolds(*candidates[i].row, hour) && runsGoal(candidates[i].task);
				if (candidates.size() == 1)
					++fallbackOnly;
			}
	std::printf("swept %zu (package, role, half hour); fallback only in %zu\n", checked, fallbackOnly);
	check(ok && checked == table.packages().size() * kRoleCount * 48, "24 h sweep: every package, role and half hour has a goal, the fallback last, no row out of its window");

	// The Guard town gaps at hours 11 and 22 fall to the fallback, not to a stall. 208 (the Urgent get-out-of-bed event goal, live only
	// for a sleeper in a bed) is in every window and does not fill a gap; neither do the crime goals (209 / 210 / 211 / 212 / 204, live
	// only while a finder names an alarm, a prisoner or a bounty).
	const auto withoutEvents = [](std::vector<Candidate> list)
	{
		list.erase(std::remove_if(list.begin(), list.end(), [](const Candidate& c)
		{
			return c.task == task::GetOutOfBedEmergency || c.task == task::InvestigateAlarms || c.task == task::InvestigateAlarmsAlliesOnly
				|| c.task == task::PoliceFreePrisoners || c.task == task::LootStolenGoods || c.task == task::HuntBounties;
		}), list.end());
		return list;
	};
	const std::vector<Candidate> gap = withoutEvents(candidatesAt(*table.find("3110-gamedata.base"), Role::Squad, 11.0));
	check(gap.size() == 1 && gap[0].fallback, "Guard town squad at 11:00 is fallback only (a real gap)");
	check(withoutEvents(candidatesAt(*table.find("3110-gamedata.base"), Role::Squad, 22.5)).size() == 1, "Guard town squad at 22:30 is fallback only");
	const std::vector<Candidate> day = withoutEvents(candidatesAt(*table.find("3110-gamedata.base"), Role::Squad, 13.0));
	// Task CH turrets (dt24_turret_finder): the turret rows are modelled now, so the day list carries them after post and patrol.
	const auto carries = [&](std::uint16_t task)
	{
		return std::any_of(day.begin(), day.end(), [&](const Candidate& candidate) { return candidate.task == task; });
	};
	check(day.size() == 4 && day[0].task == task::StaticGuardTown && day[1].task == task::PatrolTown && carries(task::ManTurrets)
		&& day.back().fallback, "Guard town squad at 13:00: post, patrol, the turret row, fallback");
	std::size_t skipped = 0;
	candidatesAt(*table.find("3110-gamedata.base"), Role::Squad, 13.0, &skipped);
	check(skipped >= 4, "the unmodelled / owned rows are counted, not an error");
	const std::vector<Candidate> shopNight = withoutEvents(candidatesAt(*table.find("2824-gamedata.base"), Role::Leader, 2.0));
	check(shopNight.size() >= 3 && shopNight[shopNight.size() - 3].task == task::LockAllMyDoors, "Shop leader at 02:00 has LOCK and the secure-bed rows ahead of the fallback");

	std::size_t modelled = 0;
	std::size_t fallback = 0;
	std::size_t none = 0;
	for (const CompiledPackage& pkg : table.packages())
	{
		const std::string status = routineStatus(pkg);
		(status == "modelled" ? modelled : status == "fallback" ? fallback : none)++;
	}
	std::printf("routine status: modelled %zu, fallback %zu, none %zu\n", modelled, fallback, none);
	check(modelled + fallback + none == table.packages().size() && modelled > 100, "the coverage column splits the packages into modelled / fallback / none");

	// T3f: the unloaded phase at every package and half hour. It is Home exactly when the leader's list has nothing the phase can be
	// (the STAY_IN_HOME fallback, or only the event goals), and its unmodelled count is candidatesAt's.
	std::size_t phased = 0;
	std::size_t home = 0;
	bool phasesOk = true;
	for (const CompiledPackage& pkg : table.packages())
		for (int half = 0; half < 48; ++half)
		{
			const double hour = half / 2.0;
			std::size_t phaseUnmodelled = 0;
			std::size_t candidateUnmodelled = 0;
			const TownPhase phase = townPhase(pkg, hour, &phaseUnmodelled);
			const std::vector<Candidate> candidates = candidatesAt(pkg, Role::Leader, hour, &candidateUnmodelled);
			// Nothing the leader could plan beyond the fallback and the event goals: Home. Otherwise the phase is the one some
			// window-valid, non-event row of that list leads to (an explicit STAY_IN_HOME row is Home too).
			bool plannable = false;
			bool reachable = false;
			for (std::size_t i = 0; i + 1 < candidates.size(); ++i)
			{
				if (phaseSkipsTask(candidates[i].task))
					continue;
				plannable = true;
				reachable = reachable || phaseOfTask(candidates[i].task) == phase;
			}
			phasesOk = phasesOk && (!plannable ? phase == TownPhase::Home : reachable) && phaseUnmodelled == candidateUnmodelled;
			++phased;
			home += phase == TownPhase::Home ? 1 : 0;
		}
	check(phasesOk && phased == table.packages().size() * 48, "the unloaded phase is Home exactly when the leader has no routine plan, at every package and half hour");
	std::printf("unloaded phase: %zu (package, half hour) swept, Home in %zu\n", phased, home);
	// The real windows of the data: a guard at its post by day and in bed at night, a shop working and then asleep.
	const CompiledPackage& guard = package(table, data, "Guard town");
	check(townPhase(guard, 10.0) == TownPhase::Post, "Guard town at 10:00 is at its post");
	check(townPhase(guard, 2.0) == TownPhase::Sleep, "Guard town at 02:00 is asleep");
	const CompiledPackage& shop = *table.find("2824-gamedata.base");
	check(townPhase(shop, 10.0) == TownPhase::Work, "the Shop's keeper is at work at 10:00");
	check(townPhase(shop, 2.0) == TownPhase::Sleep, "the Shop's keeper is asleep at 02:00");
}

void dataIds(const kdata::GameData& data)
{
	// TQ23, on the merged BUILDING records.
	const auto kind = [&](const char* name)
	{
		const kdata::Record* record = byName(data, 0, name);
		test::require(record != nullptr, std::string("BUILDING ") + name);
		return classifyPiece(data, *record);
	};
	check(kind("Round table") == SiteKind::Table && kind("barrelTable") == SiteKind::Table, "function 16 is a table (Round table, barrelTable)");
	check(kind("Shop Counter Large old") == SiteKind::Counter && kind("Shop Counter Old") == SiteKind::Counter, "function 9 is a shop counter");
	check(kind("Node Guardpost") == SiteKind::GuardNode, "node type 2 (Node Guardpost) is a guard node");
	check(kind("Node Shopkeeper") == SiteKind::None && kind("Node Turret") == SiteKind::None && kind("Node Use") == SiteKind::None,
		"the Shopkeeper, Defensive / Turret and Use nodes are not guard nodes");
	check(kind("Node Generaal ") == SiteKind::GeneralNode, "the General node (type 1) is the GeneralNode kind, not a guard node (kenshi-k16.6.12.8)");
	check(kind("Old Stool") == SiteKind::None, "a stool (function 17) is not a site");
	std::size_t beds = 0;
	for (const kdata::Record* record : data.byType(0))
		beds += classifyPiece(data, *record) == SiteKind::Bed;
	check(beds >= 4, "BUILDING records with a function 6 functionality are beds (found " + std::to_string(beds) + ")");
	// Task CH group 3 (dt24_38 section 3): the guard-post test reads a BUILDING's functionality ids; 8 is the cage in that same id space.
	const auto functions = [&](const char* name)
	{
		const kdata::Record* record = byName(data, 0, name);
		test::require(record != nullptr, std::string("BUILDING ") + name);
		return buildingFunctions(data, *record);
	};
	const std::vector<std::int32_t> cage = functions("Prisoner Cage");
	check(std::find(cage.begin(), cage.end(), kFunctionCage) != cage.end() && classifyPiece(data, *byName(data, 0, "Prisoner Cage")) == SiteKind::Cage,
		"Prisoner Cage: the guard-post id 8 is the cage function");
	const std::vector<std::int32_t> table = functions("Round table");
	check(std::find(table.begin(), table.end(), kFunctionTable) != table.end(), "Round table: function 16 in the same list");
	const std::vector<std::int32_t> guardPost = functions("Node Guardpost");
	check(guardPost.empty() && classifyPiece(data, *byName(data, 0, "Node Guardpost")) == SiteKind::GuardNode,
		"a node record has no functionality: its guard-node kind is the node type, not a function id");
	// Task CH turrets (dt24_turret_finder section 2): a turret is the functionality function 12, not a layout node and not the peeler
	// machine's function 8. Every turret building also carries a `gun turret` GUN_DATA ref (the constructor throws without one).
	const auto turret = [&](const char* name)
	{
		const kdata::Record* record = byName(data, 0, name);
		test::require(record != nullptr, std::string("BUILDING ") + name);
		return isTurretBuilding(data, *record);
	};
	check(turret("Harpoon Turret") && turret("Harpoon Turret MkII") && turret("Mounted Crossbow"),
		"the harpoon turrets and the mounted crossbow are turret buildings (function 12)");
	// `n-turret node` is renamed "Node Turret" in the merged four files (an override), and it is a placement marker, not a turret: the
	// node type is what marks a mounting point, and no turret check reads it (dt24_turret_finder section 2).
	check(!turret("Training Turret") && !turret("Prisoner Cage") && !turret("Node Guardpost") && !turret("Node Turret"),
		"training turrets (function 7), the peeler machine (8), guard nodes and the turret node marker are not turret buildings");
	std::size_t turrets = 0;
	std::size_t withoutGun = 0;
	for (const kdata::Record* record : data.byType(0))
		if (isTurretBuilding(data, *record))
		{
			++turrets;
			withoutGun += record->findRefs("gun turret") == nullptr ? 1 : 0;
		}
	check(turrets >= 10 && withoutGun == 0, "the merged data's turret buildings (found " + std::to_string(turrets) + ") all carry a gun turret ref");
}

void realSites(const kdata::GameData& data)
{
	const LayoutLibrary library = LayoutLibrary::load(KENSHI_DATA);
	check(library.size() >= 400, "the four interiors.level files hold the layouts (" + std::to_string(library.size()) + ")");
	const Layout* bar = library.find("611-gamedata.base-bar2");
	test::require(bar != nullptr && bar->building == "611-gamedata.base", "the Saloon's bar2 layout");
	const std::vector<Piece> pieces = classifyLayout(data, *bar);
	const auto count = [](const std::vector<Piece>& list, SiteKind wanted)
	{
		return static_cast<std::size_t>(std::count_if(list.begin(), list.end(), [wanted](const Piece& p) { return p.kind == wanted; }));
	};
	check(count(pieces, SiteKind::GuardNode) == 7 && count(pieces, SiteKind::Table) == 3 && count(pieces, SiteKind::Counter) == 1 && count(pieces, SiteKind::Bed) == 0,
		"bar2: 7 guard nodes, 3 tables, 1 counter, no bed (INTERIORS.md)");

	BuildingSpec spec;
	spec.key = 4001;
	spec.buildingSid = bar->building;
	spec.designation = 3;
	spec.cx = 1000.0;
	spec.cz = 2000.0;
	spec.doorX = 1010.0;
	spec.doorZ = 2000.0;
	spec.memberCount = 4;
	spec.guardCount = 9;
	spec.pieces = pieces;
	SiteIndex index;
	index.add(spec);
	check(index.sitesOf(4001, SiteKind::GuardNode).size() == 7 && !index.sitesOf(4001, SiteKind::GuardNode)[0]->synthetic, "real guard nodes win (the template's guard count 9 is not used)");
	check(index.sitesOf(4001, SiteKind::Table).size() == 3 && index.sitesOf(4001, SiteKind::Counter).size() == 1, "real tables and counter");
	check(index.sitesOf(4001, SiteKind::Bed).size() == 4 && index.sitesOf(4001, SiteKind::Bed)[0]->synthetic, "no real bed: synthetic beds, one per home member");
	check(index.sitesOf(4001, SiteKind::HomePoint).size() == 1 && index.sitesOf(4001, SiteKind::HomePoint)[0]->x == 1010.0, "a home point at the door");
	const Site third{SiteKind::GuardNode, 4001, 2};
	const SiteEntry* found = index.siteAt(third);
	check(found != nullptr && found->site == third, "siteAt finds (kind, building, slot)");
	check(index.siteAt({SiteKind::GuardNode, 4001, 7}) == nullptr && index.siteAt({SiteKind::Bed, 9999, 0}) == nullptr, "an unknown slot or building is null");

	// A layout with a real bed: the bed is found by function 6 and no synthetic bed is added.
	const Layout* withBed = nullptr;
	for (const kdata::Record* building : data.byType(0))
		for (const Layout* layout : library.forBuilding(building->stringID))
			if (withBed == nullptr && count(classifyLayout(data, *layout), SiteKind::Bed) > 0)
				withBed = layout;
	test::require(withBed != nullptr, "some interior layout holds a bed");
	const std::vector<Piece> bedPieces = classifyLayout(data, *withBed);
	BuildingSpec home;
	home.key = 4002;
	home.buildingSid = withBed->building;
	home.memberCount = 1;
	home.pieces = bedPieces;
	index.add(home);
	const std::vector<const SiteEntry*> beds = index.sitesOf(4002, SiteKind::Bed);
	check(beds.size() == count(bedPieces, SiteKind::Bed) && !beds.empty() && !beds[0]->synthetic, "a real bed (function 6) is a Bed site and no synthetic bed is added: " + withBed->sid);
	check(index.buildings() == 2, "two buildings indexed");

	// The Hub bar's upstairs beds at their use nodes (two per Bed, 11 u either side, x the Bed's scale 1.1). Layout frame; mapped onto
	// the Hub bar these are within 0.1 u of the nodes kinspect measured (findings/bar_beds.md): one node blocked by the next bed, one on the wall side.
	const Layout* hubBar = library.find("3384-TwoStorey.mod-Bar");
	test::require(hubBar != nullptr, "the TwoStorey Bar layout");
	const kdata::Record* bed = byName(data, 0, "Bed");
	test::require(bed != nullptr, "BUILDING Bed");
	check(useNodes(data, *bed).size() == 2, "the Bed has two use nodes");
	const std::vector<Piece> hubBeds = ksim::routines::bedPieces(data, *hubBar);
	const std::array<std::array<double, 2>, 3> nodes{{{-37.2, 20.6}, {-36.6, -22.7}, {-26.4, -43.1}}};
	test::require(hubBeds.size() == nodes.size(), "the Hub bar has three beds (" + std::to_string(hubBeds.size()) + ")");
	for (std::size_t i = 0; i < nodes.size(); ++i)
		check(hubBeds[i].kind == SiteKind::Bed && std::hypot(hubBeds[i].x - nodes[i][0], hubBeds[i].z - nodes[i][1]) < 0.5,
			"Hub bar bed " + std::to_string(i) + " sits at its free use node (" + std::to_string(hubBeds[i].x) + ", " + std::to_string(hubBeds[i].z) + ")");
}

void syntheticSites()
{
	BuildingSpec spec;
	spec.key = 7;
	spec.cx = 100.0;
	spec.cz = 200.0;
	spec.doorX = 110.0;
	spec.doorZ = 200.0;
	spec.memberCount = 3;
	spec.guardCount = 0;
	SiteIndex index;
	index.add(spec);
	const std::vector<const SiteEntry*> guards = index.sitesOf(7, SiteKind::GuardNode);
	check(guards.size() == static_cast<std::size_t>(kMinSyntheticGuardNodes), "a bare building offers at least kMinSyntheticGuardNodes guard nodes");
	bool ring = true;
	for (const SiteEntry* node : guards)
		ring = ring && node->synthetic && std::abs(std::hypot(node->x - 110.0, node->z - 200.0) - kRingRadius) < 1e-9;
	check(ring, "synthetic guard nodes sit on a ring of radius 12 round the door");
	check(index.sitesOf(7, SiteKind::Table).size() == 1 && index.sitesOf(7, SiteKind::Counter).size() == 1 && index.sitesOf(7, SiteKind::HomePoint).size() == 1, "one table, one counter, one home point");
	check(index.sitesOf(7, SiteKind::Bed).size() == 3 && index.sitesOf(7, SiteKind::TownNode).empty(), "beds = members; no synthetic town node");
	check(index.sitesOf(7, SiteKind::Table)[0]->x == 100.0 && index.sitesOf(7, SiteKind::Counter)[0]->x > 100.0 && index.sitesOf(7, SiteKind::Counter)[0]->x < 110.0, "the table at the centre, the counter on the door side");
	spec.guardCount = 5;
	spec.memberCount = 0;
	index.add(spec);
	check(index.sitesOf(7, SiteKind::GuardNode).size() == 5 && index.sitesOf(7, SiteKind::Bed).size() == 1 && index.buildings() == 1, "re-adding replaces: guard count 5, a house is never bedless");

	// A rotated layout: world = centre + R * local.
	BuildingSpec turned;
	turned.key = 8;
	turned.cx = 10.0;
	turned.cz = 20.0;
	turned.yawCos = 0.0;
	turned.yawSin = 1.0;
	turned.pieces = {{SiteKind::Bed, 3.0, 0.0}};
	index.add(turned);
	check(std::abs(index.sitesOf(8, SiteKind::Bed)[0]->x - 10.0) < 1e-9 && std::abs(index.sitesOf(8, SiteKind::Bed)[0]->z - 17.0) < 1e-9, "the layout rotation applies to real pieces");
	check(index.find(8)->sites.front().site.kind == SiteKind::HomePoint || index.find(8)->sites.front().site.kind == SiteKind::GuardNode, "sites are sorted by kind");
}

void reservations()
{
	Reservations book;
	const Site bed0{SiteKind::Bed, 1, 0};
	const Site bed1{SiteKind::Bed, 1, 1};
	const Site table{SiteKind::Table, 2, 0};
	check(book.empty() && book.free(bed0) && book.held(10).kind == SiteKind::None, "an empty book");
	check(book.take(bed0, 10, 100) && book.size() == 1 && book.holder(bed0)->owner == 10 && book.holder(bed0)->since == 100, "take");
	check(!book.take(bed0, 11, 120) && book.holder(bed0)->owner == 10 && book.held(11).kind == SiteKind::None, "another owner is refused and nothing changes");
	check(book.take(bed0, 10, 130) && book.holder(bed0)->since == 100, "the same owner keeps its claim and its since");
	check(book.take(bed1, 10, 140) && book.free(bed0) && book.size() == 1 && book.held(10) == bed1, "an owner holds one site: a new take releases the old");
	check(!book.take(bed1, 11, 150) && book.held(10) == bed1, "a failed take keeps the old claim");
	check(book.take(table, 99, 150) && book.size() == 2, "a squad claims a table beside the member's bed");
	check(!book.release(11) && !book.release(bed1, 11) && book.size() == 2, "releasing what is not held does nothing");
	check(book.release(bed1, 10) && book.free(bed1) && book.size() == 1, "release by site and owner");
	check(book.release(99) && book.empty(), "release by owner: the book is empty (the demotion sweep leaves nothing)");

	// Safety release, death release.
	book.take(bed0, 1, 100);
	book.take(bed1, 2, 5000);
	book.take(table, 3, 9000);
	check(book.expire(10000, 6000) == 1 && book.free(bed0) && !book.free(bed1) && !book.free(table), "kReservationMaxHours: only the claim older than the limit goes");
	check(book.releaseDead([](std::uint64_t owner) { return owner != 3; }) == 1 && book.free(table) && book.size() == 1, "a dead owner's claim is released");
	book.clear();
	check(book.empty(), "clear");

	// The load-time rebuild: the lower Id keeps a contested site and the loser is told.
	check(book.adopt(bed0, 20, 10) && !book.adopt(bed0, 30, 11) && book.holder(bed0)->owner == 20, "adopt: the first owner keeps it, a higher Id loses");
	check(book.adopt(bed0, 5, 12) && book.holder(bed0)->owner == 5 && book.held(20).kind == SiteKind::None && book.size() == 1, "adopt: a lower Id takes it over; the former holder has none left");
	check(book.adopt(bed0, 5, 13) && book.holder(bed0)->since == 12, "adopt: the same owner again is a no-op");
}

} // namespace

int main()
{
	try
	{
		windows();
		taskTableChecks();
		const kdata::GameData data = kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA));
		const PackageTable table(data);
		compileReal(data, table);
		compileSynthetic();
		sweep(data, table);
		dataIds(data);
		realSites(data);
		syntheticSites();
		reservations();
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
	std::puts("ksim_town_goals: all passed");
	return 0;
}
