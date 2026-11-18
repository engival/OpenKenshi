#include "ksim/blackboard.hpp"
#include "ksim/clock.hpp"

#include <algorithm>
#include <string_view>

namespace ksim::blackboard {

namespace {

// kdata's itemType table (kdata/format.hpp), as routines.cpp uses it.
constexpr std::int32_t kAiPackageType = 72;

// 1402721a0: the package becomes current; `reapply` marks that its role lists are rebuilt (ours are compiled per record).
void apply(Board& board, const Package& package, double nowHours)
{
	if (board.current != package.sid)
	{
		board.traderState = 0;
		board.occupyHeld = 0.0;
		board.occupyTown = 0;
		board.assaultEngaged = false;
		board.startedAt = nowHours; // the class's start hook stamps now (a re-apply of the running package does not restart it)
	}
	board.current = package.sid;
	board.currentPriority = package.priority;
	board.reapply = true;
}

} // namespace

bool timedSignal(std::int32_t signalFunc) noexcept
{
	return signalFunc == kContractTimed || signalFunc == kContractHiredAlly;
}

bool finished(const Board& board, const Package& package, double nowHours, const Situation& situation) noexcept
{
	switch (package.signalFunc)
	{
		case kContractTimed:
		case kContractHiredAlly:
			// Contract_HiredAlly's "ended" flag (AIPackage +0xa8) is not modelled: it starts unset and nothing in this slice sets it.
			return nowHours > board.contractExpiry;
		case kGoOutOnPatrol: case kGoOutOnPatrolRunning: case kCannibalGoOutOnPatrol: // 1402870a0
			return !situation.platoonReady || nowHours - board.startedAt > kPatrolHours;
		case kRetreatAndRefresh: // 1402871e0: finished as soon as it would not start
			return !shouldStart(board, package, nowHours, situation);
		case kPassingByTownAssault: // 14028eca0
			if (!situation.platoonReady)
				return true;
			if (nowHours - board.startedAt < kAssaultMinHours || !situation.targetsGone)
				return false;
			return board.assaultEngaged || situation.engagedMembers >= kAssaultEngaged;
		case kOccupyTown: // 140289290
			return !situation.leaderAlive || board.occupyHeld >= kOccupyHours - 0.5 / kTicksPerGameHour; // half a tick of slack: kTicksPerGameHour is rounded, so 4.0 s sums just short
		case kWanderingTrader: // 140286a50
			return board.traderState == 2;
		case kSlaverCamps: case kSlaverAnywhere: // 140288af0: the ownership set is empty (rwh.8; no member test)
			return situation.owned == 0;
		case kPrisonerGathering: case kPrisonerGatheringUnholy: // 140288a00
			return situation.missionCount <= situation.members * kGatherPerMember && !situation.gatherTarget;
		case kWarBase: case kWarAssault: case kWarAssaultCannibal: // 14028fdb0
			return !situation.warInstance;
		default:
			return false;
	}
}

namespace {

// 1402868c0: whether a trader keeps wandering at this hour.
bool traderKeepsWandering(double nowHours, const Situation& situation) noexcept
{
	const double hour = nowHours - 24.0 * static_cast<long long>(nowHours / 24.0);
	return situation.townOwned || (situation.atTown && hour > kDaylightFrom && hour < kDaylightTo);
}

} // namespace

bool shouldStart(const Board& board, const Package& package, double nowHours, const Situation& situation) noexcept
{
	switch (package.signalFunc)
	{
		case kSlaverCamps: case kSlaverAnywhere: // 140288aa0: a member list and at least one owned character (rwh.8)
			return situation.members > 0 && situation.owned >= 1;
		case kOccupyTown: // 140286cb0: only ever reached as a `Leads to` successor
			return false;
		case kRetreatAndRefresh: // 1402871c0
			return !situation.platoonReady;
		case kPassingByTownAssault: // 140290450
			return situation.platoonReady && situation.assaultTown;
		case kWanderingTrader: // 1402869a0
			return situation.townOwned || board.traderState != 2 || traderKeepsWandering(nowHours, situation);
		default:
			return true; // 140285610, the base class (and every class that does not override vf12)
	}
}

bool canLead(const Package& package, const Situation& situation) noexcept
{
	if (package.signalFunc == kPassingByTownAssault) // 14028ea90
		return situation.platoonReady;
	return true; // 140285660
}

bool readPackage(const kdata::GameData& data, std::string_view sid, std::int32_t priority, Package& out)
{
	const kdata::Record* record = data.find(std::string(sid));
	if (record == nullptr || record->type != kAiPackageType)
		return false;
	out.sid = std::string(sid);
	out.priority = priority;
	out.signalFunc = record->intOr("signal func", 0);
	out.leadsTo.clear();
	if (const kdata::RefCategory* leads = record->findRefs("Leads to"); leads != nullptr && !leads->entries.empty())
		out.leadsTo = leads->entries.front().targetID;
	return true;
}

void registerPackage(Board& board, Package package)
{
	std::vector<Package>& bucket = board.table[package.priority];
	if (std::any_of(bucket.begin(), bucket.end(), [&package](const Package& other) { return other.sid == package.sid; }))
		return;
	if (package.priority > board.maxPriority)
		board.maxPriority = package.priority;
	bucket.push_back(std::move(package));
}

void resolveLeads(Board& board)
{
	for (auto& [priority, bucket] : board.table)
		for (Package& package : bucket)
			if (!package.leadsTo.empty() && find(board, package.leadsTo) == nullptr)
				package.leadsTo.clear();
}

void pushContract(Board& board, Package package, double hours, double nowHours)
{
	if (!board.contract.sid.empty() && board.contract.sid == package.sid)
		return; // the same contract is already running
	package.priority = 0; // contracts register at priority 0
	if (package.signalFunc == 0)
		package.signalFunc = kContractTimed; // the contract default when `signal func` is 0
	package.leadsTo.clear();
	board.contract = std::move(package);
	board.contractExpiry = nowHours + (hours >= 1.0 ? hours : kShortContractHours);
	apply(board, board.contract, nowHours);
}

void changeAI(Board& board, Package package)
{
	board.table.clear();
	board.maxPriority = -1;
	board.current.clear();
	board.reapply = true;
	package.priority = 0;
	package.leadsTo.clear();
	board.replacement = package.sid;
	registerPackage(board, std::move(package));
	apply(board, board.table[0].front(), 0.0);
}

state::BoardStates::Entry capture(const Board& board, std::uint64_t squadId)
{
	state::BoardStates::Entry out;
	out.squad = squadId;
	out.replacement = board.replacement;
	out.contract = board.contract.sid;
	out.contractSignal = board.contract.signalFunc;
	out.contractExpiry = board.contractExpiry;
	out.current = board.current;
	out.currentPriority = board.currentPriority;
	out.reapply = board.reapply;
	out.runningAway = board.runningAway;
	out.stickyCageBuilding = board.stickyCageBuilding;
	out.stickyCageSlot = board.stickyCageSlot;
	out.hasSmell = board.hasSmell;
	out.smellX = board.smellX;
	out.smellZ = board.smellZ;
	out.startedAt = board.startedAt;
	out.traderState = board.traderState;
	out.occupyHeld = board.occupyHeld;
	out.owned = board.owned;
	out.depot = board.depot;
	return out;
}

void restore(Board& board, const state::BoardStates::Entry& saved, const kdata::GameData& data)
{
	Package replacement;
	if (!saved.replacement.empty() && readPackage(data, saved.replacement, 0, replacement))
	{
		board.table.clear();
		board.maxPriority = -1;
		board.replacement = saved.replacement;
		registerPackage(board, std::move(replacement));
		resolveLeads(board);
	}
	if (!saved.contract.empty())
	{
		board.contract.sid = saved.contract;
		board.contract.priority = 0;
		board.contract.signalFunc = saved.contractSignal;
		board.contract.leadsTo.clear();
		board.contractExpiry = saved.contractExpiry;
	}
	board.current = saved.current;
	board.currentPriority = saved.currentPriority;
	board.reapply = saved.reapply;
	board.runningAway = saved.runningAway;
	board.stickyCageBuilding = saved.stickyCageBuilding;
	board.stickyCageSlot = saved.stickyCageSlot;
	board.hasSmell = saved.hasSmell;
	board.smellX = saved.smellX;
	board.smellZ = saved.smellZ;
	board.startedAt = saved.startedAt;
	board.traderState = saved.traderState;
	board.occupyHeld = saved.occupyHeld;
	board.owned = saved.owned;
	board.depot = saved.depot;
	board.depotStamp = saved.depot != 0 ? saved.startedAt : -1.0; // a found depot stays; a package that found none runs its finder again
}

void tick(Board& board, double nowHours, const Campaign& campaign, const Situation& situation)
{
	if (!board.runningAway)
		for (std::int32_t priority = board.maxPriority; priority >= 0; --priority)
			if (consider(board, priority, nowHours, campaign, situation))
				break;
	// The applied package's own tick (vf9): only the F classes with state do anything.
	if (!situation.advance)
		return;
	const Package* running = board.current.empty() ? nullptr : find(board, board.current);
	if (running == nullptr)
		return;
	if (running->signalFunc == kWanderingTrader && situation.atTown && board.traderState < 2) // 140288c30: 0 -> 1 -> 2
		++board.traderState;
	else if (running->signalFunc == kPassingByTownAssault && situation.engagedMembers >= kAssaultEngaged)
		board.assaultEngaged = true;
	else if (running->signalFunc == kOccupyTown)
		board.occupyHeld = situation.occupying ? 0.0 : board.occupyHeld + situation.stepHours; // 140289290's accumulator: time with nobody there
}

bool consider(Board& board, std::int32_t priority, double nowHours, const Campaign& campaign, const Situation& situation)
{
	// 1. Campaign: the phase package is pushed as a 24 h contract before any priority is looked at.
	if (campaign.present)
		pushContract(board, campaign.package, kCampaignHours, nowHours);
	// 2. Contract override: while it is unfinished the table is not consulted.
	if (!board.contract.sid.empty())
	{
		if (!finished(board, board.contract, nowHours, situation))
		{
			if (board.current != board.contract.sid) // the decompiled re-apply condition (dt19 section 11, UNSURE)
				apply(board, board.contract, nowHours);
			return true;
		}
		// 14026a110: the "contract end talk passive" dialogue op fires here in the original (the dialogue layer's; not modelled).
		if (board.current == board.contract.sid)
			board.current.clear();
		board.contract = Package{};
	}
	// 3. The priority's bucket.
	const auto bucket = board.table.find(priority);
	if (bucket == board.table.end())
		return false;
	for (const Package& package : bucket->second)
	{
		if (package.sid == board.current)
		{
			if (!finished(board, package, nowHours, situation))
			{
				if (board.reapply)
					apply(board, package, nowHours);
				board.reapply = false;
				return true;
			}
			if (package.leadsTo.empty() || !canLead(package, situation))
			{
				// F s12: a finished lone priority-0 package is applied again, so its start hook re-runs (GoOutOnPatrol re-stamps).
				if (priority == 0 && bucket->second.size() == 1)
					board.startedAt = nowHours;
				return false;
			}
			const Package* next = find(board, package.leadsTo);
			if (next == nullptr) // dropped at registration; the level is not handled
				return false;
			apply(board, *next, nowHours);
			return true;
		}
		// should-start (vf12): the first non-current package that answers yes takes over; one that answers no is skipped (UNSURE: the
		// loop's continue is read from dt19, not from the class bodies).
		if (!shouldStart(board, package, nowHours, situation))
			continue;
		apply(board, package, nowHours);
		return true;
	}
	if (bucket->second.size() == 1 && priority == 0)
	{
		// the lone package runs, but the level still reports "not handled"; a finished GoOutOnPatrol runs its start hook again (F s12)
		if (board.current == bucket->second.front().sid)
			board.startedAt = nowHours;
		apply(board, bucket->second.front(), nowHours);
	}
	return false;
}

const Package* find(const Board& board, std::string_view sid) noexcept
{
	if (!board.contract.sid.empty() && board.contract.sid == sid)
		return &board.contract;
	for (const auto& [priority, bucket] : board.table)
		for (const Package& package : bucket)
			if (package.sid == sid)
				return &package;
	return nullptr;
}

} // namespace ksim::blackboard
