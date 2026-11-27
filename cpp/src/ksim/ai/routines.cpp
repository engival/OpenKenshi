#include "ksim/ai/routines.hpp"

#include "kdata/format.hpp"

#include <limits>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>

namespace ksim::routines {

namespace {

// Record type ids from kdata's itemType table (kdata/format.hpp).
constexpr std::int32_t kAiTaskType = 22;
constexpr std::int32_t kInstanceCollectionType = 30;
constexpr std::int32_t kAiPackageType = 72;

// Sorted by task (taskInfo does a binary search).
const std::vector<TaskInfo> kTasks = {
	{7, "UNEQUIP_WEAPON", TaskUse::Modelled, "native"}, // the planner's sheathe before a no-weapon goal (GATES_CARRY_STATIC, dt24_275)
	{15, "Protect allies", TaskUse::Owned, "combat"},
	{16, "Attack enemies", TaskUse::Owned, "combat"},
	{20, "Shopkeeper", TaskUse::Modelled, "T3b"},
	{21, "Attack enemies and neutrals", TaskUse::Owned, "combat"}, // MQ14 path 2: combat's attacksNeutrals flag (ARCHITECTURE.md)
	{22, "PATROL", TaskUse::Modelled, "patrol"}, // task patrol: a goal-less walk at the member's own point (dt24_36 section 4)
	{24, "WANDERER", TaskUse::Never, "never"},
	{28, "STAND_UP", TaskUse::Modelled, "native"}, // ds/small-tasks-2: Task_UnCrouch clears the crouched stance
	{32, "Self preservation", TaskUse::Owned, "combat"},
	{34, "Law enforcer", TaskUse::Owned, "crime"},
	{36, "PATROL_TOWN", TaskUse::Modelled, "T3b"},
	{37, "WANDER_TOWN", TaskUse::Modelled, "native"}, // kenshi-k16.6.12.8: a general node of the home building (rwh.55)
	{38, "STAND_AT_GUARD_NODE_HOMEBUILDING", TaskUse::Modelled, "T3b"},
	{41, "ATTACK_ENEMIES_OF_MY_SLAVEMASTER", TaskUse::Owned, "combat"}, // task raid-goals: combat's join scan, dt24_41
	{43, "STAY_IN_HOME", TaskUse::Modelled, "T3b"},
	{47, "STAND_AT_GENERAL_NODE", TaskUse::Modelled, "native"}, // kenshi-k16.6.12.8: finder 14099f280
	{51, "STAND_AT_NODE", TaskUse::Modelled, "native"}, // kenshi-k16.6.12.8: Task_StandAtNode (reserve, face, release)
	{53, "TRAVEL_TO_TARGET_TOWN", TaskUse::Modelled, "native"}, // dt27 section 5
	{58, "Medic", TaskUse::Owned, "jobs"},
	{86, "OPEN_UP_SHOP_DOORS", TaskUse::Modelled, "T3c"},
	{88, "DELIVER_RESOURCES", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{89, "JOB_KEEP_EVERYTHING_RUNNING", TaskUse::Modelled, "native"}, // ds/machines-2: dt24_113 / dt24_cage_loot_play
	{90, "UNJAM_ALL_MACHINES", TaskUse::Modelled, "native"}, // ds/machines-2: dt24_113 / dt24_cage_loot_play
	{91, "UNJAM_MACHINE", TaskUse::Modelled, "native"}, // ds/machines-2: dt24_113 / dt24_cage_loot_play
	{93, "FILL_MACHINE", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{102, "SEEK_AND_TALK_AND_SEND_SIGNAL", TaskUse::Modelled, "native"}, // ds/small-tasks-2: Task_TalktoPlayer (item28b section 1.3)
	{105, "FIND_AND_RESCUE", TaskUse::Modelled, "native"}, // port-talk: 148's finder and the shared rescue executor (dt24_142)
	{106, "FIND_BED_AND_PUT_IN", TaskUse::Modelled, "T3e2"}, // ds/bed-audit: the world selector's caging twin; the town scorer never plans it
	{107, "USE_CAGE", TaskUse::Modelled, "native"}, // task ds/slave-goals: 172's plan step (I_AM_IMPRISONED's writer, dt24_plantasks)
	{112, "FIND_CAGE_AND_PUT_IN", TaskUse::Modelled, "T3e2"},
	{113, "EMPTY_MACHINE_OUTPUTS", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{114, "GET_RID_OF_RESOURCES_IN_MY_INVENTORY", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{117, "FIND_A_SHOP", TaskUse::Never, "never"},
	{118, "SHOPPING", TaskUse::Modelled, "T3d"},
	{119, "BUY_SHIT", TaskUse::Modelled, "T3d"},
	{124, "OPERATE_STORAGE", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{127, "RUN_AWAY_HOMETOWN", TaskUse::Modelled, "native"}, // dt24_128: reduced port
	{128, "RETREAT_HOMETOWN", TaskUse::Modelled, "native"}, // dt24_128: reduced port
	{130, "TRAVEL_TO_TARGET_TOWN_FAST", TaskUse::Modelled, "native"}, // dt27 section 5
	{131, "LOOT_FOOD_AND_STUFF", TaskUse::Modelled, "native"}, // ds/slave-loot: dt24_131, 243's twin over 284's pull
	{132, "FIND_AND_KIDNAP", TaskUse::Modelled, "native"}, // task ds/slave-goals: 254's lift row, planner-only (dt26 sections 1-2)
	{135, "KILL_A_RANDOM_CAGE_OCCUPANT", TaskUse::Modelled, "native"}, // dt27 section 3
	{137, "DEAD_GUYS_GO_IN_THE_POT", TaskUse::Modelled, "T3g"}, // task CH town-residents-3: the corpse pot (TOWN_LIFE_SLICE 5.32)
	{138, "FIND_A_DEAD_GUY", TaskUse::Modelled, "native"}, // ds/dead-guy: the corpse lift as a goal of its own (dt30)
	{139, "EAT_A_RANDOM_CAGE_OCCUPANT", TaskUse::Modelled, "native"}, // dt29_fog_kidnap sections 6, 8
	{142, "FIND_AND_RESCUE_LEADER", TaskUse::Modelled, "native"}, // dt24_142
	{143, "Protect squad", TaskUse::Owned, "combat"},
	{144, "Attack enemies, don't leave town", TaskUse::Owned, "combat"},
	{148, "FIND_AND_RESCUE_IF_THERES_BEDS", TaskUse::Modelled, "native"}, // dt24_142
	{149, "MAN_TURRET", TaskUse::Modelled, "T3b"}, // task CH turrets: the finder, the walk stands at it (no mounting yet)
	{152, "OPERATE_AUTOMATIC_MACHINERY", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{153, "GO_HOME_AND_GO_TO_BED", TaskUse::Modelled, "T3c"},
	{154, "GO_TO_THE_BAR_AND_DRINK", TaskUse::Never, "never"},
	{155, "LOCK_ALL_MY_DOORS", TaskUse::Modelled, "T3c"},
	{157, "STAND_AT_GUARD_NODE_HOMETOWN_OUTSIDE", TaskUse::Modelled, "T3b"},
	{158, "SHOO_STRANGERS", TaskUse::Modelled, "T3c"},
	{159, "SEND_DIALOGUE_SIGNAL", TaskUse::Modelled, "native"}, // ds/small-tasks-2: the same class as 102
	{164, "FOLLOW_WHILE_TALKING", TaskUse::Modelled, "native"}, // ds/small-tasks-2: Task_FollowAndTalk (dt24_stalk_turret section 6)
	{165, "TOWN_STALKER", TaskUse::Modelled, "native"}, // ds/small-tasks-2: Task_TownStalker, the out-of-town branch (dt24_stalk_turret section 5)
	{167, "CAPTURE_NEW_SLAVES", TaskUse::Modelled, "T3e2"},
	{168, "CARRY_WOUNDED_SLAVES", TaskUse::Modelled, "T3e2"},
	{172, "SLAVE_GET_IN_MY_MASTERS_CAGE", TaskUse::Modelled, "native"}, // task ds/slave-goals (dt24_172 section 1)
	{175, "SLEEP_ON_FLOOR", TaskUse::Modelled, "native"}, // dt24_175: the animals' floor sleep (the world selector's terminal rest row)
	{176, "HUNTING_BLOODSMELL", TaskUse::Modelled, "native"}, // dt27 section 4
	{182, "STRIP_SLAVES", TaskUse::Modelled, "T3e2"},
	{183, "SLAVE_WATCHING", TaskUse::Modelled, "T3e2"},
	{184, "PUT_LOOT_IN_STORAGE", TaskUse::Modelled, "T3e2"},
	{188, "WORK_THE_SLAVES", TaskUse::Modelled, "native"}, // ds/slave-loot: dt24_131 section 4
	{189, "Pretend labour (auto worker)", TaskUse::Modelled, "T3b"},
	{190, "Pretend labour (pretend)", TaskUse::Modelled, "T3b"},
	{192, "GO_TO_SOMEWHERE_FOR_DELIVERING_SLAVES", TaskUse::Modelled, "native"}, // dt27 section 6.1
	{193, "CAPTURE_ESCAPING_SLAVES", TaskUse::Never, "never"},
	{194, "GIVE_ALL_MY_SLAVES_TO", TaskUse::Modelled, "native"}, // dt27 section 6.3
	{195, "LOCK_ALL_THE_CAGES", TaskUse::Modelled, "T3e2"},
	{196, "BEAT_CAGE_OCCUPANT", TaskUse::Modelled, "native"}, // ds/cannibal-pot: 135's blows (dt24_killprisoner, dt39 section 4)
	{197, "LOCK_ALL_MY_DOORS_STAY_OUTSIDE", TaskUse::Modelled, "T3c"},
	{202, "TOTAL_ESCAPE", TaskUse::Ignored, "never"}, // ds/escape: a data-only row (dt24_207), no effect and no executor
	{204, "HUNT_BOUNTIES", TaskUse::Adopted, "crime-rows"}, // dt24 204 / dt41_E section 7: the scorer picks the target, the arrest order (arrest.cpp) is the chain
	{207, "GET_OUT_OF_CAGE_ESCAPE", TaskUse::Modelled, "native"}, // ds/escape: the shared release action (dt24_207)
	{208, "GET_OUT_OF_BED_IF_ITS_EMERGENCY", TaskUse::Modelled, "T3c"}, // task ds/follow-bed: the scorer's Urgent wake (dt24_208 / s63), the shared get-out action
	{209, "INVESTIGATE_ALARMS", TaskUse::Adopted, "crime-rows"}, // dt41_E: the town alarm manager, stepCrimeGoals runs the dwell
	{210, "INVESTIGATE_ALARMS_ALLIES_ONLY", TaskUse::Adopted, "crime-rows"},
	{211, "POLICE_FREE_PRISONERS_WHEN_DONE", TaskUse::Adopted, "crime-rows"}, // finder 1408cf3c0; the release itself is arrest.cpp / lock_tasks.cpp (110)
	{212, "LOOT_STOLEN_GOODS", TaskUse::Adopted, "crime-rows"}, // finder 1408cf6a0; stepCrimeGoals takes the items
	{214, "RELAX_IN_TOWN_PACKAGE", TaskUse::Modelled, "T3d"},
	{215, "TRAVEL_TO_TARGET_PACKAGE", TaskUse::Modelled, "native"}, // task army-travel: dt24_215, the walk to the package target point
	{216, "RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE", TaskUse::Modelled, "patrol"}, // task patrol: I_AM_INSIDE_TARGET_BUILDING (dt24_36 section 3)
	{217, "GATHER_SLAVES_FROM_CAGES_IF_ITS_AN_EXPORT_TOWN", TaskUse::Modelled, "native"}, // dt27 section 6.2
	{218, "GIVE_ALL_MY_SLAVES_TO_IF_ITS_AN_IMPORT_TOWN", TaskUse::Modelled, "native"}, // dt27 section 6.3
	{221, "PRETEND_TO_OPERATE", TaskUse::Modelled, "T3b"},
	{222, "Turret gunner homebuilding", TaskUse::Modelled, "T3b"}, // task CH turrets: the 234 finder body (dt24_turret_finder)
	{223, "PICKUP_INTRUDERS", TaskUse::Adopted, "T3e1"},
	{224, "TAKE_INTRUDER_OUTSIDE", TaskUse::Adopted, "T3e1"},
	{230, "EAT_A_RANDOM_DEAD_BODY", TaskUse::Modelled, "native"}, // dt27 section 2
	{233, "EAT_A_RANDOM_KO_BODY", TaskUse::Modelled, "native"}, // dt39 section 5
	{237, "FOGMAN_WORSHIP_VICTIM", TaskUse::Modelled, "native"}, // dt24_237
	{239, "GO_HOME_AND_GO_TO_BED_SECURE", TaskUse::Modelled, "T3c"},
	{241, "LOOT_RESOURCE_ITEMS_WE_HAVE_STORAGE_FOR", TaskUse::Modelled, "native"}, // ds/machines-2: dt24_113 / dt24_cage_loot_play
	{242, "DITCH_ALL_RESOURCES", TaskUse::Modelled, "native"}, // ds/machines: dt24_113 / dt24_plantasks section 4
	{243, "AQUIRE_FOOD_AT_HOMEBASE", TaskUse::Modelled, "native"}, // dt24_243: the occupied town's food
	{244, "GRAB_ONE_FOOD", TaskUse::Modelled, "native"}, // dt24_243: 243's plan step (code-only: no package names 244)
	{245, "GATHER_SLAVES_FROM_CAGES_IF_FEMALE_OR_BEAST", TaskUse::Modelled, "native"}, // ds/slave-loot: dt24_131 section 7
	{253, "FOLLOW_URGENT_ESCAPE", TaskUse::Modelled, "native"}, // ds/escape: 141's follow while the leader is not down (dt24_207)
	{254, "FINAL_KIDNAPPER_CAGE_JOB", TaskUse::Modelled, "native"}, // task ds/slave-goals (dt26): the kidnapper package goal
	{255, "SIT_ON_THRONE", TaskUse::Modelled, "native"}, // dt24_43 section 2c: 43 on the throne slot
	{256, "GET_OUT_OF_CAGE_OPPORTUNISTIC", TaskUse::Modelled, "native"}, // ds/escape: Task_GetOutOfCage mode 1 (dt24_207)
	{268, "STAND_AT_GUARD_NODE_HOMEBUILDING_INDOORS_ONLY", TaskUse::Modelled, "T3b"}, // 38's twin, one node-query flag (task CH group 3)
	{269, "Heal my legs when crippled", TaskUse::Owned, "medical"},
	{270, "ASSAULT_FORTIFICATIONS_PREFER_GATES", TaskUse::Modelled, "raid"}, // task raid-goals (dt24_270)
	{273, "PICKUP_INTRUDERS_TOWN", TaskUse::Adopted, "T3e1"}, // the town scope of 223 (dt24_273; task CH group 3)
	{274, "TAKE_INTRUDER_OUTSIDE_TOWN", TaskUse::Adopted, "T3e1"},
	{275, "SIT_AROUND", TaskUse::Modelled, "native"}, // dt24_275: the occupied town's sit
	{276, "LIBERATE_ALL_THE_PRISONERS", TaskUse::Modelled, "raid"}, // task raid-goals (dt24_276)
	{281, "FIND_CAGE_AND_PUT_DEADGUY_IN", TaskUse::Modelled, "native"}, // dt26 / dt24_172: 112 with a corpse allowed
	{282, "EAT_A_RANDOM_CAGE_OCCUPANT_MEASURED_RATE", TaskUse::Modelled, "native"}, // dt24_killprisoner: 139
	{283, "SHOO_STRANGERS_IF_PRIVATE", TaskUse::Modelled, "T3c"},
	{288, "Protect allies, don't leave town", TaskUse::Owned, "combat"},
};

float readF32(const std::byte* bytes) noexcept
{
	float value;
	std::memcpy(&value, bytes, sizeof value);
	return value;
}

std::vector<std::byte> readFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("routines: cannot open " + path);
	in.seekg(0, std::ios::end);
	const auto size = in.tellg();
	in.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty())
		in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	if (!in)
		throw std::runtime_error("routines: cannot read " + path);
	return bytes;
}

using Lists = std::array<std::vector<GoalRow>, kRoleCount>;

constexpr const char* kListKeys[kRoleCount] = {"Squad AI Goals", "Squad2 AI Goals", "Leader AI Goals", "Slave AI Goals"};

class Compiler
{
public:
	Compiler(const kdata::GameData& data, std::size_t& missing, std::size_t& cycles)
		: data_(data), missing_(missing), cycles_(cycles)
	{
	}

	// The merged, pre-fallback lists of a package (parents first, then its own).
	const Lists& merged(const std::string& sid)
	{
		if (const auto found = done_.find(sid); found != done_.end())
			return found->second;
		static const Lists none;
		if (!active_.insert(sid).second)
		{
			// The original has no cycle guard (dt19 section 4); a cyclic mod would recurse until the stack dies. Ours drops the
			// back edge and says so loudly, once per package met in the cycle.
			++cycles_;
			std::fprintf(stderr, "routines: AI package `inherits from` cycle at '%s' (the back edge is ignored)\n", sid.c_str());
			return none;
		}
		Lists lists;
		if (const kdata::Record* record = data_.find(sid))
		{
			if (const kdata::RefCategory* parents = record->findRefs("inherits from"))
				for (const kdata::RefEntry& parent : parents->entries)
				{
					const Lists inherited = merged(parent.targetID);
					for (int role = 0; role < kRoleCount; ++role)
						mergeRows(lists[static_cast<std::size_t>(role)], inherited[static_cast<std::size_t>(role)]);
				}
			for (int role = 0; role < kRoleCount; ++role)
				mergeRows(lists[static_cast<std::size_t>(role)], own(*record, kListKeys[role]));
		}
		active_.erase(sid);
		return done_.emplace(sid, std::move(lists)).first->second;
	}

	std::vector<GoalRow> own(const kdata::Record& record, const char* key)
	{
		std::vector<GoalRow> rows;
		const kdata::RefCategory* category = record.findRefs(key);
		if (category == nullptr)
			return rows;
		for (const kdata::RefEntry& entry : category->entries)
		{
			const kdata::Record* task = data_.find(entry.targetID);
			if (task == nullptr || task->type != kAiTaskType)
			{
				++missing_;
				continue;
			}
			GoalRow row;
			row.taskSid = task->stringID;
			row.task = static_cast<std::uint16_t>(task->intOr("enum", 0));
			const std::int32_t tier = task->intOr("classification", 2);
			row.tier = tier >= 3 ? Tier::Urgent : tier == 1 ? Tier::Fluff : Tier::NonUrgent;
			row.start = entry.v0;
			row.end = entry.v1;
			row.weight = entry.v2;
			rows.push_back(std::move(row));
		}
		return rows;
	}

private:
	const kdata::GameData& data_;
	std::size_t& missing_;
	std::size_t& cycles_;
	std::unordered_map<std::string, Lists> done_;
	std::set<std::string> active_;
};

} // namespace

// dt19 section 4: append, skip a row the list already holds for the same task record. The original's insert (14027b3a0)
// compares the task type and the row's target hand; the hand is the AI_TASK record here (the data carries no separate one).
void mergeRows(std::vector<GoalRow>& into, const std::vector<GoalRow>& rows)
{
	for (const GoalRow& row : rows)
	{
		const bool held = std::any_of(into.begin(), into.end(), [&row](const GoalRow& other)
		{
			return other.taskSid == row.taskSid;
		});
		if (!held)
			into.push_back(row);
	}
}

// ---- goal table ----

const std::vector<TaskInfo>& taskTable() noexcept
{
	return kTasks;
}

const TaskInfo* taskInfo(std::uint16_t task) noexcept
{
	const auto found = std::lower_bound(kTasks.begin(), kTasks.end(), task,
		[](const TaskInfo& info, std::uint16_t value) { return info.task < value; });
	return found != kTasks.end() && found->task == task ? &*found : nullptr;
}

bool runsGoal(std::uint16_t task) noexcept
{
	const TaskInfo* info = taskInfo(task);
	return info != nullptr && (info->use == TaskUse::Modelled || info->use == TaskUse::Adopted);
}

// ---- windows and candidates ----

Hold holdOf(std::uint16_t goal) noexcept
{
	switch (goal)
	{
	case task::StaticGuardBuilding:
	case task::StaticGuardBuildingIndoors:
		return kHoldNode;
	case task::StaticGuardTown:
		return kHoldPost;
	case task::ManTurrets:
	case task::TurretOnBuilding:
		return kHoldNode; // UNSURE: the turret rows' desc stores were not decoded (dt24_turret_finder section 1); the node hold is taken
	case task::StayInHome:
	case task::SitOnThrone: // dt24_43 section 2c: the same duration triple as 43
		return kHoldStayInHome;
	case task::GoHomeToBed:
	case task::GoHomeToBedSecure:
		return kHoldBed;
	case task::Shopping:
		return kHoldShopping;
	case task::PretendLabourPretend:
		return kHoldLabour;
	case task::SitAround:
		return kHoldStayInHome; // dt24_275: 0.2 + rand x 0.8 h (the row's +0x18 / +0x1c fields)
	case task::SleepOnFloor:
	case task::SleepOnFloorFakeAmbush: // 261 is the same class (Task_SleepOnFloor)
		return kHoldBed; // UNSURE (dt24_175): the exe's floor-sleep bounds (DAT_141696c70 / DAT_14169bb40) are unread; the bed hold stands in
	default:
		return {};
	}
}

Commit commitOf(std::uint16_t goal) noexcept
{
	switch (goal)
	{
	case task::RunAwayHometown:
		return {0.05, 0.15}; // 127 (dt33); 128's pair is 0 / 0
	case task::DefeatSquadLimitChase:
		return {0.25, 2.0}; // 248 (dt24_101: +0x18 / +0x1c; 101 has none)
	case 35:
		return {0.05, 0.1}; // dt33: the second row with a nonzero pair
	default:
		return {}; // UNSURE: the other rows' +0x18 / +0x1c were not read
	}
}

bool windowHolds(std::int32_t start, std::int32_t end, double hour) noexcept
{
	if (end > 24 || (start == end && kEqualWindowAlwaysOn))
		return true;
	if (start < end)
		return hour >= start && hour < end;
	if (start == end)
		return false;
	return hour >= start || hour < end;
}

bool windowHolds(const GoalRow& row, double hour) noexcept
{
	return windowHolds(row.start, row.end, hour);
}

double effectiveWeight(const GoalRow& row) noexcept
{
	return row.weight == 0 ? 1.0 : row.weight * 0.01;
}

goalsel::Offer makeOffer(std::uint16_t task, Tier tier, double weight, double utility, std::uint64_t target, int others, std::int32_t start, std::int32_t end) noexcept
{
	goalsel::Offer offer;
	offer.task = task;
	offer.tier = static_cast<int>(tier);
	offer.utility = utility;
	offer.weight = weight;
	offer.crowdDecay = kCrowdDecay;
	offer.others = others;
	offer.start = static_cast<std::uint16_t>(start);
	offer.end = static_cast<std::uint16_t>(end);
	offer.target = target;
	const Hold hold = holdOf(task);
	offer.holds = hold.base > 0.0 || hold.span > 0.0;
	offer.hold = {hold.base, hold.span};
	return offer;
}

bool CompiledPackage::empty() const noexcept
{
	return std::all_of(lists.begin(), lists.end(), [](const std::vector<GoalRow>& list) { return list.empty(); });
}

std::vector<Candidate> candidatesAt(const CompiledPackage& package, Role role, double hour, std::size_t* unmodelled)
{
	std::vector<Candidate> out;
	std::size_t skipped = 0;
	for (const GoalRow& row : package.list(role))
	{
		if (!windowHolds(row, hour))
			continue;
		if (!runsGoal(row.task))
		{
			++skipped;
			continue;
		}
		out.push_back({&row, row.task, row.tier, effectiveWeight(row), false});
	}
	Candidate fallback;
	fallback.task = task::StayInHome;
	fallback.tier = Tier::NonUrgent;
	fallback.weight = kFallbackUtility;
	fallback.fallback = true;
	out.push_back(fallback);
	if (unmodelled != nullptr)
		*unmodelled = skipped;
	return out;
}

const char* townPhaseName(TownPhase phase) noexcept
{
	switch (phase)
	{
	case TownPhase::Home: return "home";
	case TownPhase::Post: return "post";
	case TownPhase::Work: return "work";
	case TownPhase::Relax: return "relax";
	case TownPhase::Patrol: return "patrol";
	case TownPhase::Sleep: return "sleep";
	}
	return "home";
}

TownPhase phaseOfTask(std::uint16_t task) noexcept
{
	switch (task)
	{
	case task::GoHomeToBed:
	case task::GoHomeToBedSecure:
		return TownPhase::Sleep;
	case task::StaticGuardBuilding:
	case task::StaticGuardBuildingIndoors:
	case task::StaticGuardTown:
	case task::ManTurrets:
	case task::TurretOnBuilding:
		return TownPhase::Post;
	case task::Patrol:
	case task::PatrolTown:
	case task::RunAroundTown:
		return TownPhase::Patrol;
	case task::RelaxInTownPackage:
		return TownPhase::Relax;
	case task::StayInHome:
		return TownPhase::Home;
	default:
		return runsGoal(task) ? TownPhase::Work : TownPhase::Home;
	}
}

bool phaseSkipsTask(std::uint16_t task) noexcept
{
	// Every one of these is a scorer candidate only while live state says so (an intruder home, a stranger near, a downed body, a
	// corpse, loot, a slave errand); with no world loaded the finder cannot run, so the row says nothing about the routine.
	switch (task)
	{
	case task::GetOutOfBedEmergency: // 208: only for a sleeper in a bed under attack (a bed is not a phase)
	case task::FindCageAndPutIn:
	case task::FindBedAndPutIn: // 106: only while a person is carried (the world selector's)
	case task::FindCageAndPutDeadGuyIn:
	case task::FinalKidnapperCageJob: // only while a downed non-ally is in sight (132's finder, dt26 section 4)
	case task::DeadGuysInThePot:
	case task::FindCorpses: // 138: the world selector's corpse lift; the town scorer reaches it only inside 137's chain
	case task::ShooStrangers:
	case task::CaptureSlaves:
	case task::CarryWoundedSlaves:
	case task::SlaveGetInCage:
	case task::GetOutOfCageEscape: // 207 / 256 / 253: the world selector's (a caged slave, a follow); the town scorer never plans them
	case task::GetOutOfCageOpportunistic:
	case task::FollowUrgentEscape:
	case task::StripSlaves:
	case task::SlaveWatching:
	case task::PutLootInStorage:
	case task::PickupIntruders:
	case task::TakeIntruderOutside:
	case task::PickupIntrudersTown:
	case task::TakeIntruderOutsideTown:
	case task::HuntBounties: // crime-rows: live only while a finder names something (alarm, prisoner, bounty)
	case task::InvestigateAlarms:
	case task::InvestigateAlarmsAlliesOnly:
	case task::PoliceFreePrisoners:
	case task::LootStolenGoods:
	case task::ShooStrangersPrivate:
		return true;
	default:
		return false;
	}
}

TownPhase townPhase(const CompiledPackage& package, double hour, std::size_t* unmodelled) noexcept
{
	std::size_t skipped = 0;
	const std::vector<Candidate> candidates = candidatesAt(package, Role::Leader, hour, &skipped);
	const Candidate* best = nullptr;
	for (const Candidate& candidate : candidates)
	{
		// The fallback is what happens when nothing else can plan; the scorer's own list decides the phase. The event goals are not
		// in the routine either (phaseSkipsTask).
		if (candidate.fallback || phaseSkipsTask(candidate.task))
			continue;
		if (best == nullptr || candidate.tier > best->tier || (candidate.tier == best->tier && candidate.weight >= best->weight))
			best = &candidate;
	}
	if (unmodelled != nullptr)
		*unmodelled = skipped;
	return best != nullptr ? phaseOfTask(best->task) : TownPhase::Home;
}

// ---- package table ----

PackageTable::PackageTable(const kdata::GameData& data)
	: data_(&data)
{
	Compiler compiler(data, missingTasks_, cycles_);
	for (const kdata::Record* record : data.byType(kAiPackageType))
	{
		CompiledPackage package;
		package.sid = record->stringID;
		package.name = record->name;
		package.lists = compiler.merged(record->stringID);

		// The empty-list fallback runs after the merge (5.5): Squad <- Leader; Squad2 <- Squad, else Leader. The slave list has none.
		auto& squad = package.lists[static_cast<std::size_t>(Role::Squad)];
		auto& squad2 = package.lists[static_cast<std::size_t>(Role::Squad2)];
		const auto& leader = package.lists[static_cast<std::size_t>(Role::Leader)];
		if (squad.empty() && !leader.empty())
		{
			squad = leader;
			package.readsFrom[static_cast<std::size_t>(Role::Squad)] = Role::Leader;
		}
		if (squad2.empty() && !squad.empty()) // Squad was already filled from Leader when it was empty, so "else Leader" is the same list
		{
			squad2 = squad;
			package.readsFrom[static_cast<std::size_t>(Role::Squad2)] = package.readsFrom[static_cast<std::size_t>(Role::Squad)];
		}
		index_.emplace(package.sid, packages_.size());
		packages_.push_back(std::move(package));
	}
}

// RACE `AI Goals` (dt34 section 2.5): the same (start, end, weight) rows as a package's lists; the data carries (0, 100, 0), always
// on at weight 1.0. Built on first use per race and kept.
const std::vector<GoalRow>& PackageTable::raceRows(const std::string& raceSid) const
{
	if (const auto found = raceRows_.find(raceSid); found != raceRows_.end())
		return found->second;
	std::size_t missing = 0, cycles = 0;
	Compiler compiler(*data_, missing, cycles);
	std::vector<GoalRow> rows;
	if (const kdata::Record* race = data_->find(raceSid))
		rows = compiler.own(*race, "AI Goals");
	return raceRows_.emplace(raceSid, std::move(rows)).first->second;
}

std::vector<GoalRow> PackageTable::rowsWithRace(const CompiledPackage* package, Role role, const std::string& raceSid) const
{
	std::vector<GoalRow> rows;
	static const std::vector<GoalRow> none;
	const std::vector<GoalRow>& own = package != nullptr ? package->list(role) : none;
	for (const GoalRow& row : raceRows(raceSid))
		if (std::none_of(own.begin(), own.end(), [&row](const GoalRow& mine) { return mine.task == row.task; }))
			rows.push_back(row);
	rows.insert(rows.end(), own.begin(), own.end());
	return rows;
}

const CompiledPackage* PackageTable::find(std::string_view packageSid) const noexcept
{
	const auto found = index_.find(std::string(packageSid));
	return found == index_.end() ? nullptr : &packages_[found->second];
}

const char* routineStatus(const CompiledPackage& package) noexcept
{
	if (package.empty())
		return "none";
	for (const auto& list : package.lists)
		for (const GoalRow& row : list)
			if (runsGoal(row.task))
				return "modelled";
	return "fallback";
}

bool carriesGoal(const CompiledPackage& package, std::uint16_t task) noexcept
{
	for (const auto& list : package.lists)
		for (const GoalRow& row : list)
			if (row.task == task)
				return true;
	return false;
}

bool goalHolds(const CompiledPackage& package, Role role, std::uint16_t task, double hour) noexcept
{
	for (const GoalRow& row : package.list(role))
		if (row.task == task && windowHolds(row, hour))
			return true;
	return false;
}

// ---- sites ----

SiteKind classifyPiece(const kdata::GameData& data, const kdata::Record& building)
{
	if (building.boolOr("is node", false) && building.intOr("node type", kNodeGeneral) == kNodeGuardpost)
		return SiteKind::GuardNode;
	if (building.boolOr("is node", false) && building.intOr("node type", kNodeGeneral) == kNodeGeneral && building.findRefs("functionality") == nullptr)
		return SiteKind::GeneralNode; // rwh.55 section 2: the factory makes a LocationNode of any `is node` record that is not type 4
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return SiteKind::None;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		const kdata::Record* record = data.find(entry.targetID);
		if (record == nullptr)
			continue;
		switch (record->intOr("function", 0))
		{
		case kFunctionBed:
			return SiteKind::Bed;
		case kFunctionCage:
			return SiteKind::Cage;
		case kFunctionCounter:
			return SiteKind::Counter;
		case kFunctionTable:
			return SiteKind::Table;
		case kFunctionThrone:
			if (functionInMask(kMaskGoal255, kFunctionThrone))
				return SiteKind::Throne;
			break;
		default:
			break;
		}
	}
	return SiteKind::None;
}

std::int32_t machineFunction(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return 0;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		const kdata::Record* record = data.find(entry.targetID);
		const std::int32_t function = record != nullptr ? record->intOr("function", 0) : 0;
		if (function > 0 && functionInMask(kMaskMachine, function))
			return function;
	}
	return 0;
}

bool isShopkeeperNode(const kdata::Record& building)
{
	return building.boolOr("is node", false) && building.intOr("node type", kNodeGeneral) == kNodeShopkeeper;
}

std::vector<std::int32_t> buildingFunctions(const kdata::GameData& data, const kdata::Record& building)
{
	std::vector<std::int32_t> functions;
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return functions;
	for (const kdata::RefEntry& entry : functionality->entries)
		if (const kdata::Record* record = data.find(entry.targetID))
			functions.push_back(record->intOr("function", 0));
	return functions;
}

bool isTurretBuilding(const kdata::GameData& data, const kdata::Record& building)
{
	for (const std::int32_t function : buildingFunctions(data, building))
		if (function == kFunctionGunTurret)
			return true;
	return false;
}

bool isTortureFunctionality(const kdata::Record& functionality)
{
	if (functionality.intOr("function", 0) != kFunctionCage)
		return false;
	const kdata::RefCategory* produces = functionality.findRefs("produces");
	return (produces != nullptr && !produces->entries.empty()) || functionality.intOr("flags", 0) != 0;
}

std::vector<std::string> LayoutLibrary::levelFiles(std::string_view dataDir)
{
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	const fs::path root = fs::path(dataDir) / "newland" / "leveldata";
	for (const char* layer : {"", "Newwworld", "Dialogue", "rebirth"})
	{
		const fs::path path = root / layer / "interiors.level";
		if (fs::exists(path))
			files.push_back(path.string());
	}
	return files;
}

LayoutLibrary LayoutLibrary::load(const std::vector<std::string>& files)
{
	kdata::GameData merged;
	for (const std::string& path : files)
	{
		const std::vector<std::byte> bytes = readFile(path);
		merged.apply(kdata::parse(bytes, std::filesystem::path(path).filename().string()));
	}
	LayoutLibrary library;
	for (const kdata::Record* record : merged.byType(kInstanceCollectionType))
	{
		const std::string building = record->stringOr("associated building");
		if (building.empty())
			continue;
		Layout layout;
		layout.sid = record->stringID;
		layout.building = building;
		for (std::size_t n = 0; n < record->instances.size(); ++n)
		{
			const kdata::Instance& instance = record->instances[n];
			if (instance.targetStringID.empty())
				continue;
			const std::byte* t = instance.transform.data();
			layout.pieces.push_back({instance.targetStringID, readF32(t), readF32(t + 4), readF32(t + 8), readF32(t + 12), readF32(t + 16),
				readF32(t + 20), readF32(t + 24), static_cast<std::uint16_t>(std::min<std::size_t>(n, 0xFFFE))});
		}
		library.index_.emplace(layout.sid, library.layouts_.size());
		library.layouts_.push_back(std::move(layout));
	}
	return library;
}

const Layout* LayoutLibrary::find(std::string_view layoutSid) const noexcept
{
	const auto found = index_.find(std::string(layoutSid));
	return found == index_.end() ? nullptr : &layouts_[found->second];
}

std::vector<const Layout*> LayoutLibrary::forBuilding(std::string_view buildingSid) const
{
	std::vector<const Layout*> out;
	for (const Layout& layout : layouts_)
		if (layout.building == buildingSid)
			out.push_back(&layout);
	std::sort(out.begin(), out.end(), [](const Layout* a, const Layout* b) { return a->sid < b->sid; });
	return out;
}

std::vector<Piece> classifyLayout(const kdata::GameData& data, const Layout& layout)
{
	std::vector<Piece> out;
	for (const LayoutPiece& piece : layout.pieces)
	{
		const kdata::Record* record = data.find(piece.target);
		if (record == nullptr)
			continue;
		const SiteKind kind = isShopkeeperNode(*record) ? SiteKind::ShopkeeperNode : classifyPiece(data, *record);
		if (kind == SiteKind::GeneralNode)
		{
			// The node faces its orientation x NEGATIVE_UNIT_X (rwh.55 section 3, +0xc0), in the layout frame.
			const std::array<double, 3> face = rotate({piece.qw, piece.qx, piece.qy, piece.qz}, {-1.0, 0.0, 0.0});
			const double length = std::hypot(face[0], face[2]);
			out.push_back({kind, piece.x, piece.z, piece.y, length > 0.0 ? face[0] / length : 0.0, length > 0.0 ? face[2] / length : 1.0});
			out.back().piece = piece.index;
		} else if (kind != SiteKind::None) {
			out.push_back({kind, piece.x, piece.z, piece.y});
			out.back().piece = piece.index;
		}
	}
	return out;
}

std::vector<UseNode> useNodePoses(const kdata::GameData& data, const kdata::Record& building)
{
	std::vector<UseNode> out;
	const double scale = building.floatOr("scale", 1.0f);
	for (const kdata::Instance& instance : building.instances)
	{
		const kdata::Record* node = instance.targetStringID.empty() ? nullptr : data.find(instance.targetStringID);
		if (node == nullptr || !node->boolOr("is node", false) || node->intOr("node type", kNodeGeneral) != kNodeUse)
			continue;
		const std::byte* t = instance.transform.data();
		out.push_back({{scale * readF32(t), scale * readF32(t + 4), scale * readF32(t + 8)},
			{readF32(t + 12), readF32(t + 16), readF32(t + 20), readF32(t + 24)}});
	}
	return out;
}

std::vector<std::array<double, 3>> useNodes(const kdata::GameData& data, const kdata::Record& building)
{
	std::vector<std::array<double, 3>> out;
	for (const UseNode& node : useNodePoses(data, building))
		out.push_back(node.at);
	return out;
}

std::array<double, 3> rotate(const std::array<double, 4>& q, const std::array<double, 3>& v) noexcept
{
	// v + 2 u x (u x v + w v), u = (x, y, z)
	const double w = q[0], x = q[1], y = q[2], z = q[3];
	const double cx = y * v[2] - z * v[1] + w * v[0];
	const double cy = z * v[0] - x * v[2] + w * v[1];
	const double cz = x * v[1] - y * v[0] + w * v[2];
	return {v[0] + 2 * (y * cz - z * cy), v[1] + 2 * (z * cx - x * cz), v[2] + 2 * (x * cy - y * cx)};
}

std::string functionClip(const kdata::GameData& data, const kdata::Record& building, std::string_view fallback)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	for (std::size_t i = 0; functionality != nullptr && i < functionality->entries.size(); ++i)
	{
		const kdata::Record* function = data.find(functionality->entries[i].targetID);
		const kdata::RefCategory* clip = function != nullptr ? function->findRefs("animation") : nullptr;
		for (std::size_t k = 0; clip != nullptr && k < clip->entries.size(); ++k)
		{
			if (clip->entries[k].v0 == kdata::kReferenceRemoved)
				continue;
			const kdata::Record* animation = data.find(clip->entries[k].targetID);
			if (animation != nullptr && !animation->name.empty())
				return animation->name;
		}
	}
	return std::string(fallback);
}

std::array<double, 2> pieceToLayout(const LayoutPiece& piece, const std::array<double, 3>& local) noexcept
{
	// v + 2 u x (u x v + w v), u = (qx, qy, qz) (kworld::placeFurniture)
	const double w = piece.qw, x = piece.qx, y = piece.qy, z = piece.qz;
	const double cx = y * local[2] - z * local[1] + w * local[0];
	const double cy = z * local[0] - x * local[2] + w * local[1];
	const double cz = x * local[1] - y * local[0] + w * local[2];
	return {piece.x + local[0] + 2 * (y * cz - z * cy), piece.z + local[2] + 2 * (x * cy - y * cx)};
}

std::vector<Piece> bedPieces(const kdata::GameData& data, const Layout& layout)
{
	std::vector<std::pair<const LayoutPiece*, const kdata::Record*>> beds;
	for (const LayoutPiece& piece : layout.pieces)
		if (const kdata::Record* record = data.find(piece.target); record != nullptr && classifyPiece(data, *record) == SiteKind::Bed)
			beds.emplace_back(&piece, record);
	// A spot one bed took is not another's: two beds side by side share the node between them (equally near both, so the nearer-bed
	// test passes it), and both sleepers lay on one point (client live Heng check 2026-11-04, the Slaver Guards).
	constexpr double kSpotGap = 8.0;
	std::vector<std::array<double, 3>> taken; // x, z, the floor (a bed upstairs may lie over one below)
	std::vector<Piece> out;
	for (const auto& [piece, record] : beds)
	{
		std::array<double, 2> at{piece->x, piece->z};
		double nodeY = piece->y; // a bed without a free node keeps the floor
		const std::array<double, 4> pieceQ{piece->qw, piece->qx, piece->qy, piece->qz};
		// The sleeper faces the node's -X (orientation node.q x RotY(-90 deg)); a bed without a free node faces its own -X.
		std::array<double, 3> face = rotate(pieceQ, {-1.0, 0.0, 0.0});
		struct Spot
		{
			std::array<double, 2> at;
			std::array<double, 4> q;
			double nodeY; // the node's own height in the layout frame (the body lies on it, above the floor)
		};
		std::vector<Spot> spots;
		std::vector<BedNode> nodes;
		for (const UseNode& node : useNodePoses(data, *record))
		{
			spots.push_back({pieceToLayout(*piece, node.at), node.q, piece->y + rotate(pieceQ, node.at)[1]});
			const std::array<double, 3> nodeFace = rotate(pieceQ, rotate(node.q, {-1.0, 0.0, 0.0}));
			const double nodeLength = std::hypot(nodeFace[0], nodeFace[2]);
			nodes.push_back({spots.back().at[0], spots.back().at[1], spots.back().nodeY, nodeLength > 0.0 ? nodeFace[0] / nodeLength : 0.0,
				nodeLength > 0.0 ? nodeFace[2] / nodeLength : 1.0});
		}
		std::stable_sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b)
		{
			return std::hypot(a.at[0], a.at[1]) < std::hypot(b.at[0], b.at[1]);
		});
		bool placed = false;
		for (const Spot& spot : spots)
		{
			const double own = std::hypot(spot.at[0] - piece->x, spot.at[1] - piece->z);
			const bool blocked = std::any_of(beds.begin(), beds.end(), [&](const auto& other)
			{
				return other.first != piece && std::hypot(spot.at[0] - other.first->x, spot.at[1] - other.first->z) < own;
			}) || std::any_of(taken.begin(), taken.end(), [&](const std::array<double, 3>& t)
			{
				return std::hypot(spot.at[0] - t[0], spot.at[1] - t[1]) < kSpotGap && std::fabs(piece->y - t[2]) < kSpotGap;
			});
			if (!blocked)
			{
				at = spot.at;
				nodeY = spot.nodeY;
				face = rotate(pieceQ, rotate(spot.q, {-1.0, 0.0, 0.0}));
				placed = true;
				break;
			}
		}
		if (!placed && !spots.empty())
		{
			// Every node is a neighbour's or taken: the nearest one all the same, never the origin (a body there lies across the furniture).
			at = spots.front().at;
			nodeY = spots.front().nodeY;
			face = rotate(pieceQ, rotate(spots.front().q, {-1.0, 0.0, 0.0}));
		}
		taken.push_back({at[0], at[1], piece->y});
		const double length = std::hypot(face[0], face[2]);
		out.push_back({SiteKind::Bed, at[0], at[1], piece->y, length > 0.0 ? face[0] / length : 0.0, length > 0.0 ? face[2] / length : 1.0,
			functionClip(data, *record, kClipSleepInBed), record->stringID, nodeY, std::move(nodes)});
		out.back().piece = piece->index;
	}
	return out;
}

BedNode bedNodeNear(const SiteEntry& bed, double x, double z) noexcept
{
	// The exe's rule (object_node_rundown.md 3): the node nearest the character, no hysteresis; the site's own point only for a bed with no node list.
	BedNode best{bed.x, bed.z, bed.hasNodeY ? bed.nodeY : bed.y, bed.faceX, bed.faceZ};
	double nearest = bed.nodes.empty() ? 0.0 : std::numeric_limits<double>::infinity();
	for (const BedNode& node : bed.nodes)
		if (const double d = std::hypot(node.x - x, node.z - z); d < nearest)
		{
			nearest = d;
			best = node;
		}
	return best;
}

std::uint64_t SiteIndex::nextRevision() noexcept
{
	static std::atomic<std::uint64_t> counter{0};
	return ++counter;
}

void SiteIndex::add(const BuildingSpec& spec)
{
	revision_ = nextRevision();
	BuildingSites entry;
	entry.key = spec.key;
	entry.buildingSid = spec.buildingSid;
	entry.designation = spec.designation;
	entry.publicBuilding = spec.publicBuilding;
	entry.publicBeds = spec.publicBeds;
	entry.ownerFaction = spec.ownerFaction;
	entry.cx = spec.cx;
	entry.cz = spec.cz;

	std::array<std::uint16_t, kSiteKindCount> count{};
	const auto push = [&](SiteKind kind, double x, double z, bool synthetic, std::optional<double> y = std::nullopt, const Piece* piece = nullptr)
	{
		const std::size_t k = static_cast<std::size_t>(kind);
		SiteEntry site{{kind, spec.key, count[k]++}, x, z, synthetic, y.has_value(), y.value_or(0.0)};
		if (piece != nullptr)
		{
			site.faceX = spec.yawCos * piece->faceX + spec.yawSin * piece->faceZ;
			site.faceZ = -spec.yawSin * piece->faceX + spec.yawCos * piece->faceZ;
			site.clip = piece->clip;
			site.record = piece->record;
			site.hasIn = piece->hasIn;
			site.inX = spec.cx + spec.yawCos * piece->inX + spec.yawSin * piece->inZ;
			site.inZ = spec.cz - spec.yawSin * piece->inX + spec.yawCos * piece->inZ;
			site.hasNodeY = kind == SiteKind::Bed && spec.hasHeight;
			site.nodeY = spec.originY + piece->nodeY;
			site.piece = piece->piece;
			for (const BedNode& node : piece->nodes)
				site.nodes.push_back({spec.cx + spec.yawCos * node.x + spec.yawSin * node.z, spec.cz - spec.yawSin * node.x + spec.yawCos * node.z,
					spec.originY + node.y, spec.yawCos * node.faceX + spec.yawSin * node.faceZ, -spec.yawSin * node.faceX + spec.yawCos * node.faceZ});
			for (BedNode& node : site.nodes)
				if (std::hypot(node.x - x, node.z - z) < 0.01)
					node = {x, z, site.nodeY, site.faceX, site.faceZ}; // the site's own point is one of them, bit for bit
		}
		entry.sites.push_back(std::move(site));
	};

	// Real pieces first: they win their kind. Cages are real only: a building with no cage layout has none (T3e2).
	for (const SiteKind kind : {SiteKind::GuardNode, SiteKind::TownNode, SiteKind::Bed, SiteKind::Table, SiteKind::Counter, SiteKind::Throne, SiteKind::Cage, SiteKind::Seat, SiteKind::Machine, SiteKind::ShopkeeperNode, SiteKind::GeneralNode})
		for (const Piece& piece : spec.pieces)
			if (piece.kind == kind)
				push(kind, spec.cx + spec.yawCos * piece.x + spec.yawSin * piece.z, spec.cz - spec.yawSin * piece.x + spec.yawCos * piece.z, false,
					spec.hasHeight ? std::optional<double>(spec.originY + piece.y) : std::nullopt, &piece);

	push(SiteKind::HomePoint, spec.doorX, spec.doorZ, true);
	const auto none = [&](SiteKind kind) { return count[static_cast<std::size_t>(kind)] == 0; };
	if (none(SiteKind::GuardNode))
	{
		const int nodes = std::max(spec.guardCount, kMinSyntheticGuardNodes);
		for (int i = 0; i < nodes; ++i)
		{
			const double angle = 6.283185307179586 * i / nodes;
			push(SiteKind::GuardNode, spec.doorX + kRingRadius * std::cos(angle), spec.doorZ + kRingRadius * std::sin(angle), true);
		}
	}
	if (none(SiteKind::Table))
		push(SiteKind::Table, spec.cx, spec.cz, true);
	if (none(SiteKind::Counter))
		push(SiteKind::Counter, spec.cx + 0.75 * (spec.doorX - spec.cx), spec.cz + 0.75 * (spec.doorZ - spec.cz), true);
	// Synthetic beds lie in rows of kBedRow, kBedSpacing apart round the centre (OURS: one point stacked every sleeper of a homeless squad on
	// the town point; the walk ends at the nearest reachable point when a spot is off the mesh).
	if (kSyntheticBeds && none(SiteKind::Bed))
	{
		const int beds = std::max(spec.memberCount, 1);
		const int rows = (beds + kBedRow - 1) / kBedRow;
		for (int i = 0; i < beds; ++i)
			push(SiteKind::Bed, spec.cx + ((i % kBedRow) - 0.5 * (std::min(beds, kBedRow) - 1)) * kBedSpacing,
				spec.cz + ((i / kBedRow) - 0.5 * (rows - 1)) * kBedSpacing, true);
	}

	std::stable_sort(entry.sites.begin(), entry.sites.end(),
		[](const SiteEntry& a, const SiteEntry& b) { return a.site < b.site; });
	buildings_[spec.key] = std::move(entry);
}

void SiteIndex::add(const std::vector<BuildingSpec>& specs)
{
	for (const BuildingSpec& spec : specs)
		add(spec);
}

const BuildingSites* SiteIndex::find(std::uint64_t buildingKey) const noexcept
{
	const auto found = buildings_.find(buildingKey);
	return found == buildings_.end() ? nullptr : &found->second;
}

std::vector<const SiteEntry*> SiteIndex::sitesOf(std::uint64_t buildingKey, SiteKind kind) const
{
	std::vector<const SiteEntry*> out;
	if (const BuildingSites* building = find(buildingKey))
		for (const SiteEntry& entry : building->sites)
			if (entry.site.kind == kind)
				out.push_back(&entry);
	return out;
}

std::vector<std::uint64_t> SiteIndex::keys() const
{
	std::vector<std::uint64_t> out;
	out.reserve(buildings_.size());
	for (const auto& [key, building] : buildings_)
		out.push_back(key);
	return out;
}

std::uint64_t siteId(const Site& site) noexcept
{
	std::uint64_t hash = 14695981039346656037ull;
	const auto mix = [&hash](std::uint64_t value)
	{
		hash ^= value;
		hash *= 1099511628211ull;
	};
	mix(site.object);
	mix(site.slot);
	return kSiteIdFlag | (hash & (kSiteIdFlag - 1));
}

const SiteEntry* SiteIndex::siteAt(const Site& site) const noexcept
{
	const BuildingSites* building = find(site.object);
	if (building == nullptr)
		return nullptr;
	const auto found = std::lower_bound(building->sites.begin(), building->sites.end(), site,
		[](const SiteEntry& entry, const Site& value) { return entry.site < value; });
	return found != building->sites.end() && found->site == site ? &*found : nullptr;
}

// ---- reservations ----

bool Reservations::take(const Site& site, std::uint64_t owner, std::uint64_t tick)
{
	if (const auto found = bySite_.find(site); found != bySite_.end())
		return found->second.owner == owner;
	release(owner);
	bySite_.emplace(site, Claim{owner, tick});
	byOwner_[owner] = site;
	return true;
}

bool Reservations::release(std::uint64_t owner)
{
	const auto found = byOwner_.find(owner);
	if (found == byOwner_.end())
		return false;
	bySite_.erase(found->second);
	byOwner_.erase(found);
	return true;
}

bool Reservations::release(const Site& site, std::uint64_t owner)
{
	const auto found = bySite_.find(site);
	if (found == bySite_.end() || found->second.owner != owner)
		return false;
	return release(owner);
}

const Claim* Reservations::holder(const Site& site) const noexcept
{
	const auto found = bySite_.find(site);
	return found == bySite_.end() ? nullptr : &found->second;
}

Site Reservations::held(std::uint64_t owner) const noexcept
{
	const auto found = byOwner_.find(owner);
	return found == byOwner_.end() ? Site{} : found->second;
}

std::size_t Reservations::expire(std::uint64_t now, std::uint64_t maxTicks)
{
	std::vector<std::uint64_t> stale;
	for (const auto& [site, claim] : bySite_)
		if (now > claim.since && now - claim.since >= maxTicks)
			stale.push_back(claim.owner);
	for (const std::uint64_t owner : stale)
		release(owner);
	return stale.size();
}

bool Reservations::adopt(const Site& site, std::uint64_t owner, std::uint64_t since)
{
	const auto found = bySite_.find(site);
	if (found == bySite_.end())
	{
		release(owner);
		bySite_.emplace(site, Claim{owner, since});
		byOwner_[owner] = site;
		return true;
	}
	if (found->second.owner == owner)
		return true;
	if (owner > found->second.owner)
		return false;
	release(found->second.owner); // the lower Id wins; the former holder is re-scored by the caller
	release(owner);
	bySite_.emplace(site, Claim{owner, since});
	byOwner_[owner] = site;
	return true;
}

void Reservations::clear()
{
	bySite_.clear();
	byOwner_.clear();
}

} // namespace ksim::routines
