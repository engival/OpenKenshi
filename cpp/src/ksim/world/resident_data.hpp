#pragma once

// The residents' derived, never-saved state (residents.cpp promotion, routine_scorer.cpp scorer; TOWN_LIFE_SLICE 5.3 / 5.4):
// the compiled packages, the members that could not be built, the promoted world squads, and the site index the scorer's finders
// read. Everything here is rebuilt on demand from the world data and the saved components, so a load needs nothing from it.

#include "ksim/ai/routines.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/world.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace ksim {

struct ResidentData
{
	explicit ResidentData(const kdata::GameData& data)
		: packages(data)
	{
	}
	routines::PackageTable packages;
	// Members that could not be built or placed (makeFighter threw, no ground under them): not retried until a restart, so a bad record
	// costs one throw, not one per second.
	std::set<std::uint64_t> skipped;
	// The world squads (state::Id) whose members are built (5.14). Derived, never saved: rebuilt from the arena at the first look after
	// a load (a squad whose alive, non-player member Fighters carry no Routine).
	std::set<std::uint64_t> world;
	// The world squads the Fighters carry this second (5.14 follow): squad Id, its entity, its leader's Id. Refilled by prepareWorldCarry.
	struct Carried
	{
		std::uint64_t squad = 0;
		flecs::entity entity;
		std::uint64_t leader = 0;
		bool holding = false;
	};
	std::vector<Carried> carried;
	std::map<std::uint64_t, std::uint32_t> carriedNoRoute; // leader -> consecutive ticks without a route to its carrot (not saved)
	bool worldKnown = false;
	// Characters a scenario file placed on their squad (Simulation::scenarioMember): built, at their saved spot, no Routine yet. A resident squad's
	// promotion gives them theirs without moving them (promoteResidents); knowWorldSquads does not count them as a built world squad. Derived, never saved.
	std::set<std::uint64_t> adopted;
	// Squads a scenario file placed (scenarioMember): they run the world squads' goal selector with no player faction too (promoteResidents eligibility; spectate_fight, kenshi-k16.6.124). Derived, never saved.
	std::set<std::uint64_t> fileSquads;

	// ---- the scorer's sites (T3b2) ----
	// Pseudo-building keys: a placed building's key is the hash of its placement; a squad's yard (members with no home building) and a
	// town's outside nodes get a key of their own, with a high bit set (a hash reaching one is a 2^-60 event).
	static constexpr std::uint64_t kYardBit = std::uint64_t(1) << 63;
	static constexpr std::uint64_t kTownBit = std::uint64_t(1) << 62;

	routines::SiteIndex sites;
	// Yards by position key (ensureYard): a squad that moves maps to another key; the old one stays indexed (a function of the live
	// state, so a load and the uninterrupted run index the same sites).
	struct YardInfo
	{
		double x = 0.0;
		double z = 0.0;
	};
	std::map<std::uint64_t, YardInfo> yards;
	std::set<std::uint64_t> towns; // town keys indexed
	// BuildingSite by key, built at the first use (World::buildingSites is fixed after load).
	std::unordered_map<std::uint64_t, std::size_t> buildingSiteByKey;
	bool buildingSitesKnown = false;
	// Turret buildings of the loaded world (`functionality` function 12, routines::isTurretBuilding), built at the first turret goal
	// (task CH turrets): the exe's turret database is its building registry; ours is World::buildingSites filtered to turrets. Key, and
	// the placement's x, z (the walk's spot; OURS, the exe walks to the turret's own use position).
	struct TurretInfo
	{
		std::uint64_t key = 0;
		double x = 0.0;
		double z = 0.0;
		double y = 0.0; // the manning spot's height (the turret's use node)
		double yaw = 0.0; // the placement's facing (+z through its quaternion): the middle of the firing arc (task ds/turret, OURS)
		int town = -2; // the town circle it stands in (Environment::townOf), -2 = not asked yet
		std::string gunSid; // the building's `gun turret` GUN_DATA row (dt41_B: the gun's stats come from the building)
	};
	std::vector<TurretInfo> turretSites;
	bool turretsKnown = false;
	// The buildings spawned as ruins (no interior furniture): their layout beds are not there, so bedViews never lists one.
	std::set<std::uint64_t> ruinedBuildings;
	// Simulation::primeSiteIndex indexed every placed building with a layout (a function of the data, so a load repeats it identically).
	bool allBuildingsIndexed = false;
	// The first door (lowest key) of each building: x, z. Rebuilt when the number of Door entities changes (zones furnish doors as they load).
	std::unordered_map<std::uint64_t, std::array<double, 2>> doorOfBuilding;
	std::size_t doorsSeen = ~std::size_t(0);
	// Every Door entity of a building, ascending by door key (T3c, the resident door routine). Rebuilt with doorOfBuilding.
	std::unordered_map<std::uint64_t, std::vector<flecs::entity_t>> doorsOfBuilding;
	// ProducerSite by its entity Id (index into World::producerSites), for the Work sites.
	std::unordered_map<std::uint64_t, std::size_t> producerById;

	// ---- the bar hangout (T3d, 5.17) ----
	// One town's buildings as the bar finder sees them, built at the first request and again when the town's active record changes (a raid,
	// a world state: the lists are a function of it, so a loaded game sees the same); derived, never saved. Building keys are BuildingSite::key,
	// ascending.
	struct TownBars
	{
		std::string activeSid; // the TownState::activeSid these were built from
		std::vector<std::uint64_t> bars; // the BAR (designation 3) buildings
		std::vector<std::uint64_t> publicBuildings; // every public building with a table (bars included)
		std::vector<std::uint64_t> shops; // buildings with a counter worth a pretend visit: SHOP (1) and BAR (3) designations
		std::map<std::string, std::uint64_t> homes; // a placed squad template of the town -> its building (BuildingSite::key)
	};
	std::unordered_map<std::uint64_t, TownBars> townBars; // by the town's state::Id
	// A claimed table by its key (Simulation::claimBarTable): the building and the slot within it. Refilled whenever a town's tables are listed.
	struct TableInfo
	{
		std::uint64_t building = 0;
		std::uint16_t slot = 0;
		double x = 0.0;
		double z = 0.0;
		bool hasY = false; // a real layout piece: y is its floor
		double y = 0.0;
	};
	std::unordered_map<std::uint64_t, TableInfo> tableByKey;
	// A building's loose chairs (serving no table) as one more seat group: its TableInfo (slot routines::kLooseSlot, x, z the chairs' centre).
	std::unordered_map<std::uint64_t, TableInfo> looseOfBuilding;
	// A table's seats (the layout's chairs and stools near it), world x, z, nearest first; by the table key. Empty / absent: no seat in the data.
	std::unordered_map<std::uint64_t, std::vector<std::array<double, 2>>> seatsOfTable;
	// The pose of each seat, by the table key, in seatsOfTable's order (SIMLINK CHAR_DYNAMIC pose tail, kinspect bed_seat_pose.md 3).
	struct SeatPose
	{
		double y = 0.0; // the furniture origin's height
		double faceX = 0.0; // the chair's +Z, world
		double faceZ = 1.0;
		bool faceTable = false; // functionality `flags` 1 (`chair` / `sit on floor`: stools, pillows): faces its table instead
		std::string clip;
	};
	std::unordered_map<std::uint64_t, std::vector<SeatPose>> seatPoses;
	bool barsPrimed = false; // Simulation::knowBarSeats indexed every town (once per load: this object is rebuilt with it)
	std::set<std::uint64_t> tabled; // buildings whose layout tables were added to the site index
	std::shared_ptr<const routines::LayoutLibrary> layouts; // interiors.level, loaded at the first bar town
	bool layoutsTried = false;
};

} // namespace ksim
