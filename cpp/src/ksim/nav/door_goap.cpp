#include "ksim/nav/door_plan.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>

namespace ksim {

namespace
{

// The plan step that needs DOOR_IS_OPEN (the exe's move order through a closed door, GATES_CARRY_STATIC a2). OURS: no registry row.
constexpr std::uint16_t kCrossDoor = 0x7fe0;

// The facts at one door, as the evaluators the plan reads. The actor stands at the door when the plan runs (the walker stopped in front of it).
struct DoorWorld final : goap::World
{
	const DoorFacts& facts;
	explicit DoorWorld(const DoorFacts& facts_) : facts(facts_) {}

	// The row's precondition is the exe's alone (dt43 G section 1): not mine, not an ally's (the weapon is its own symbol).
	bool bashable() const noexcept { return !facts.mine && !facts.allied; }

	// The measured score by owner (DOORS_LIVE section 16): a hostile owner 1.0, else only the law / military / slave / slaver / bandit kinds; the
	// rest score 0 and drop the plan. OURS, UNSURE: dt43 G has no owner input to the bash utility (the finding is silent on the utility).
	double bashScore() const noexcept
	{
		return facts.hostile || (facts.hasOwner && kindBashesDoors(facts.ownerKind)) ? kBashUtility : 0.0;
	}

	bool holds(goap::Sym sym, std::uint64_t, double, double) const override
	{
		switch (sym)
		{
		case goap::Sym::AtLocation:
		case goap::Sym::AtBuildingDoor:
		case goap::Sym::AtCurrentDoor:
		case goap::Sym::AtDoorInside:
		case goap::Sym::AtDoorOutside: return true;
		case goap::Sym::IsDoorLocked: return facts.locked;
		case goap::Sym::IsObjectMine: return facts.mine;
		case goap::Sym::DoorBashable: return bashable();
		case goap::Sym::GateBashOpen: return facts.gate && facts.mates < kGateCrewCap && !facts.underAttack;
		case goap::Sym::HasWeaponEquipped: return facts.weaponInHand;
		case goap::Sym::HasWeaponCarried: return !facts.weaponInHand && facts.weaponCarried;
		case goap::Sym::HasOneWorkingArm: return facts.workingArm;
		case goap::Sym::LockSkillOk: return facts.lockLevel - facts.lockpicking < kPickMaxDifficulty;
		default: return false; // DoorIsOpen, DoorIsBroken, HasTools (a door cut needs the tool item: not modelled)
		}
	}

	// The measured scores (DOORS_LIVE section 16): the pick 0.1, the bash 1.0 by the weapon, the gate bash 1.0 shared with its mates.
	double utility(std::uint16_t task, std::uint64_t) const override
	{
		switch (task)
		{
		case goap::kPickLock: return kPickUtility;
		case goap::kBashDoor:
		case goap::kBashDoorHere:
		case goap::kBashDoorOrder: return bashScore();
		case goap::kBashGate: return bashScore() / static_cast<double>(facts.mates + 1);
		default: return -1.0;
		}
	}
};

// The registry plus the two rows only a door plan needs: the crossing step, and 6 EQUIP_WEAPON (the exe inserts it in front of a bash with
// a carried weapon; it is local here so other goals do not start equipping, UNSURE).
const std::vector<goap::TaskRow>& doorTable()
{
	static const std::vector<goap::TaskRow> table = []
	{
		std::vector<goap::TaskRow> out = goap::rows();
		// 7 UNEQUIP_WEAPON stays: the pick wants empty hands and the planner sheathes in front of it. The pick has no owner gate (dt43 G section 1:
		// any AI picks any door it can reach, an ally's included), so an armed NPC that cannot bash sheathes and picks.
		// No 264 EQUIP_CROSSBOW either: its HasWeaponEquipped effect would stand in for the carried-weapon equip step below.
		out.erase(std::remove_if(out.begin(), out.end(), [](const goap::TaskRow& row) { return row.task == goap::kEquipCrossbow; }), out.end());
		goap::TaskRow cross;
		cross.task = kCrossDoor;
		cross.name = "CROSS_DOOR";
		cross.pre[cross.preCount++] = {goap::Sym::DoorIsOpen, true};
		out.push_back(cross);
		goap::TaskRow equip;
		equip.task = goap::kEquipWeapon;
		equip.name = "EQUIP_WEAPON";
		equip.pre[equip.preCount++] = {goap::Sym::HasWeaponCarried, true};
		equip.eff[equip.effCount++] = {goap::Sym::HasWeaponEquipped, true};
		out.push_back(equip);
		return out;
	}();
	return table;
}

} // namespace

DoorDecision decideDoor(const DoorFacts& facts)
{
	const DoorWorld world(facts);
	const goap::Plan plan = goap::planBestWith(world, kCrossDoor, 0, 0.0, 0.0, doorTable());
	DoorDecision out;
	if (!plan.found())
		return out;
	out.utility = plan.score;
	bool opens = false;
	for (const goap::Step& step : plan.steps)
	{
		switch (step.task)
		{
		case goap::kEquipWeapon:
			out.equipFirst = true;
			break;
		case routines::task::UnequipWeapon:
			out.unequipFirst = true;
			break;
		case goap::kUnlockDoor:
		case goap::kUnlockDoorHere:
			out.plan = DoorPlan::Unlock;
			break;
		case goap::kPickLock:
			out.plan = DoorPlan::Pick;
			break;
		case goap::kBashDoor:
		case goap::kBashDoorHere:
		case goap::kBashGate:
			out.plan = DoorPlan::Bash;
			break;
		case goap::kOpenDoor:
		case goap::kOpenDoorHere:
			opens = true;
			break;
		default:
			break;
		}
	}
	if (out.plan == DoorPlan::Wait && opens)
		out.plan = DoorPlan::Open;
	return out;
}

} // namespace ksim
