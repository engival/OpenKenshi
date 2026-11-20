// kenshi-k16.6.403: the occupancy map of the 1 Hz slicing (k16.6.320) is shared derived state, built at each second boundary. A save taken mid-second with a
// squad's OccupyTown package just applied holds the map the last boundary built (the town is not occupied yet); a load must answer 'the town is ours'
// (Simulation::townOursForTest) as the unbroken run does, tick by tick, and end on the same state hash. Before save v117 the load rebuilt the map from the
// boards as they stood, so the loaded run saw the town occupied up to a second early. The dt27 world has no towns: one is made at the rig's spot.

#include "dt27_rig.hpp"

#include "ksim/save/save.hpp"
#include "ksim/world/towncircle.hpp"

#include <cstdio>
#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::int32_t kOccupyTownSignal = 0x1e;
constexpr std::uint64_t kTownId = 0x7f00000001ULL;

// A town record with a real radius, placed over the rig's spot and owned by nobody; the occupancy map is the only way it is 'ours'.
void makeTown(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("TOWN")))
	{
		state::TownState town;
		town.rootSid = record->stringID;
		town.activeSid = record->stringID;
		town.x = kX;
		town.z = kZ;
		if (towncircle::of(*rig.gd, town).radius < 500.0)
			continue;
		ecsOf(rig.sim).entity().set<state::TownState>(std::move(town)).set<state::Id>(state::Id{kTownId});
		return;
	}
	test::require(false, "no TOWN record with a radius");
}

std::uint64_t otherFaction(Rig& rig)
{
	std::uint64_t faction = 0;
	ecsOf(rig.sim).each([&](flecs::entity entity, const state::FactionState&) { if (faction == 0) faction = entity.id(); });
	test::require(faction != 0, "no faction in the world");
	return faction;
}

void parity()
{
	Rig rig({}, 0);
	makeTown(rig);
	const std::uint64_t faction = otherFaction(rig);
	blackboard::Board* live = rig.sim.blackboardOf(rig.squad);
	test::require(live != nullptr, "the rig squad has no board");
	std::string sid;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("AI_PACKAGE")))
		if (record->intOr("signal func", 0) == kOccupyTownSignal)
		{
			sid = record->stringID;
			break;
		}
	test::require(!sid.empty(), "no OccupyTown AI package");
	blackboard::Package occupy;
	test::require(blackboard::readPackage(*rig.gd, sid, 0, occupy), "no AI package " + sid);
	live->contract = blackboard::Package{};
	live->contractExpiry = 0.0;
	blackboard::changeAI(*live, std::move(occupy));
	live->current.clear(); // nothing applied: the boundary's map is empty
	CombatArena* arena = const_cast<CombatArena*>(rig.sim.combat());
	arena->find(rig.leader)->faction = faction;
	arena->find(rig.member)->faction = faction;

	while (rig.sim.clockTick() % Simulation::kSquadIntervalTicks != 0)
		rig.sim.tick();
	rig.tick(6); // inside the second: the next boundary is 14 ticks away
	test::require(!rig.sim.townOursForTest(kX, kZ, faction), "the town is ours before any occupation");
	live->current = sid; // the package starts between two boundaries: the map still lacks it
	live->currentPriority = 0;
	test::require(!rig.sim.townOursForTest(kX, kZ, faction), "the map changed between boundaries");

	std::stringstream stream;
	rig.sim.save(stream);
	Rig other({}, 0); // the same deterministic world, loaded in place from the save (ksim_save_mid_order's Reload)
	other.sim.reload(stream);
	other.squad = other.entityOf(other.member).target<state::MemberOf>();
	other.squad.set<Detail>(Detail{DetailLevel::Full}); // the load puts the bare squad back at token detail; the rig keeps it on the mesh
	test::require(other.sim.stateHash() == rig.sim.stateHash(), "the state hash differs at load");

	bool occupiedSeen = false;
	for (int i = 0; i < 2 * static_cast<int>(Simulation::kSquadIntervalTicks); ++i)
	{
		rig.sim.tick();
		other.sim.tick();
		const bool want = rig.sim.townOursForTest(kX, kZ, faction);
		const bool got = other.sim.townOursForTest(kX, kZ, faction);
		occupiedSeen = occupiedSeen || want;
		test::require(want == got, "tick " + std::to_string(i) + ": the loaded run answers 'town is ours' " + (got ? "yes" : "no") + ", the straight run " + (want ? "yes" : "no"));
	}
	std::cout << "  the straight run held the town at some tick: " << (occupiedSeen ? "yes" : "no") << "\n";
	test::require(rig.sim.stateHash() == other.sim.stateHash(), "the loaded run's state hash differs from the straight run's");
}

} // namespace

int main()
{
	try
	{
		parity();
		std::cout << "ksim_slice_load_parity: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_slice_load_parity: FAILED: " << e.what() << "\n";
		return 1;
	}
}
