#include "ksim/world/buildings.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/world/prospect.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

namespace ksim::buildings {

namespace {

constexpr double kEps = 1e-9;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) noexcept
{
	h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
	return h;
}

bool hasRefs(const kdata::Record& record, std::string_view category) noexcept
{
	const kdata::RefCategory* refs = record.findRefs(category);
	return refs != nullptr && !refs->entries.empty();
}

} // namespace

// ---- the records ----

std::vector<state::BuildMaterial> buildingCost(const kdata::Record& building)
{
	std::vector<state::BuildMaterial> mats;
	if (const kdata::RefCategory* construction = building.findRefs("construction"))
	{
		for (const kdata::RefEntry& entry : construction->entries)
		{
			if (!entry.opaqueEntry && entry.v0 > 0 && !entry.targetID.empty())
				mats.push_back({entry.targetID, static_cast<std::uint32_t>(entry.v0), 0});
		}
	}
	if (mats.empty())
	{
		const std::int32_t legacy = building.intOr("build materials", 1);
		if (legacy > 0)
			mats.push_back({kBuildingMaterialItem, static_cast<std::uint32_t>(legacy), 0});
	}
	return mats;
}

std::uint32_t totalMaterials(const std::vector<state::BuildMaterial>& mats) noexcept
{
	std::uint32_t total = 0;
	for (const state::BuildMaterial& row : mats)
		total += row.need;
	return total;
}

double maxHp(const kdata::Record& building) noexcept
{
	return doorMaxHealth(building);
}

double buildHours(std::uint32_t total, double buildSpeedMult, double ka, double kb) noexcept
{
	const double speed = buildSpeedMult * ka * kb;
	return total == 0 || speed <= 0.0 ? 0.0 : static_cast<double>(total) / speed;
}

int estimatedHours(std::uint32_t total, double buildSpeedMult, double ka, double kb) noexcept
{
	return static_cast<int>(std::floor(buildHours(total, buildSpeedMult, ka, kb) + 0.5));
}

const char* siteSupport(const kdata::Record& building, const kdata::Record* functionality) noexcept
{
	// Each of these has a placement test the slice has not built (BUILDINGS_SLICE.md section 3, the "status" column): no silent pass.
	if (functionality != nullptr && functionality->intOr("world resource mining", 0) != 0)
		return "unsupported_site"; // a mine: no_resource
	if (hasRefs(building, "farm data"))
		return "unsupported_site"; // a farm: wont_grow
	if (hasRefs(building, "snaps to") || hasRefs(building, "wall subsections") || building.boolOr("links together", false)
		|| building.boolOr("is wall gate", false) || building.boolOr("is gateway", false) || building.boolOr("is door", false))
		return "unsupported_site"; // walls, gates, snapping pieces: needs_node
	if (building.boolOr("is interior furniture", false) || building.boolOr("is exterior furniture", false) || building.boolOr("ceiling placement", false))
		return "unsupported_site"; // furniture: must_be_inside / must_be_outside / incomplete_building / bad_floor
	if (building.boolOr("is node", false) || building.boolOr("is feature", false) || building.boolOr("is foliage", false))
		return "unsupported_site"; // world objects, not buildable pieces
	return "";
}

// ---- construction ----

double progressOf(const state::PlayerBuilding& b) noexcept
{
	if (b.built)
		return 1.0;
	return b.maxHp > 0.0 ? std::clamp(b.hp / b.maxHp, 0.0, 1.0) : 1.0;
}

double materialCap(const state::PlayerBuilding& b) noexcept
{
	std::uint64_t need = 0;
	std::uint64_t have = 0;
	for (const state::BuildMaterial& row : b.mats)
	{
		need += row.need;
		have += std::min(row.have, row.need);
	}
	if (need == 0)
		return b.maxHp;
	return std::min(b.maxHp, static_cast<double>(have)); // Q8: HP may reach Σ min(required, delivered); units are fungible across items
}

double hpPerTick(double maxHp, double buildHours, double builders) noexcept
{
	if (buildHours <= 0.0 || builders <= 0.0)
		return 0.0;
	return maxHp / buildHours / static_cast<double>(kTicksPerGameHour) * builders;
}

bool working(const state::PlayerBuilding& b) noexcept
{
	return !b.built && !b.paused && !b.broken && b.workers > 0 && b.hp < materialCap(b) - kEps;
}

double rateOf(const state::PlayerBuilding& b, double hours) noexcept
{
	if (!working(b) || b.maxHp <= 0.0)
		return 0.0;
	return hpPerTick(b.maxHp, hours, builderFactor(b.workers)) / b.maxHp / kTickSeconds;
}

bool buildable(const state::PlayerBuilding& b) noexcept
{
	if (b.dismantled)
		return false;
	if (b.ruin)
		return true;
	if (b.built && !b.broken)
		return false;
	return materialCap(b) > b.hp + kEps || b.hp >= b.maxHp - kEps; // at max HP only the completion is left
}

bool whole(const state::PlayerBuilding& b) noexcept
{
	return b.built && !b.broken && !b.ruin && !b.dismantled;
}

std::vector<std::size_t> missingRows(const state::PlayerBuilding& b)
{
	std::vector<std::size_t> rows;
	for (std::size_t i = 0; i < b.mats.size(); ++i)
		if (b.mats[i].have < b.mats[i].need)
			rows.push_back(i);
	return rows;
}

bool removeHp(state::PlayerBuilding& b, double damage, double rate) noexcept
{
	if (b.dismantled)
		return false;
	if (b.built && !b.ruin)
	{
		if (b.hp >= b.maxHp)
			b.hp = b.maxHp - 1.0;
		b.built = false;
		b.broken = true;
	}
	b.hp -= damage * rate;
	if (b.hp > 0.0)
		return false;
	b.hp = 0.0;
	b.built = false;
	b.dismantled = true;
	return true;
}

bool stepSite(state::PlayerBuilding& b, double hours, double builders) noexcept
{
	if (b.built || b.paused || b.ruin || b.dismantled)
		return false;
	bool changed = false;
	const double cap = materialCap(b);
	if (builders > 0.0 && b.hp < cap - kEps)
	{
		b.hp = std::min({b.hp + hpPerTick(b.maxHp, hours, builders), cap, b.maxHp});
		changed = true;
	}
	if (b.hp >= b.maxHp - kEps && cap >= b.maxHp - kEps)
	{
		for (const state::BuildMaterial& row : b.mats)
		{
			if (row.have < row.need)
				return changed;
		}
		b.built = true;
		b.broken = false;
		b.hp = b.maxHp;
		b.paused = false;
		changed = true;
	}
	return changed;
}

std::uint32_t accepts(const state::PlayerBuilding& b, const std::string& item) noexcept
{
	if (b.built)
		return 0;
	for (const state::BuildMaterial& row : b.mats)
	{
		if (row.itemSid == item)
			return row.need > row.have ? row.need - row.have : 0;
	}
	return 0;
}

std::uint8_t statusOf(const state::PlayerBuilding& b) noexcept
{
	if (b.broken)
		return kBroken;
	if (b.built)
		return kWorking;
	// The doc's table lists Paused after UnderConstruction, which would make it unreachable: a paused site shows Paused first.
	if (b.paused)
		return kPaused;
	return b.hp >= materialCap(b) - kEps ? kNeedsMaterials : kUnderConstruction;
}

std::uint32_t flagsOf(const state::PlayerBuilding& b) noexcept
{
	return (b.built ? 1u : 0u) | (b.paused ? 2u : 0u) | (b.broken ? 4u : 0u) | (b.switchedOn ? 8u : 0u) | (b.runOffBattery ? 16u : 0u)
		| (b.repairWanted ? 32u : 0u) | (b.dismantling ? 64u : 0u);
}

std::uint64_t signatureOf(const state::PlayerBuilding& b) noexcept
{
	std::uint64_t h = mix(0, b.status);
	h = mix(h, flagsOf(b));
	h = mix(h, b.workers);
	for (const state::BuildMaterial& row : b.mats)
		h = mix(h, row.have);
	return h;
}

bool navActive(double progress, double buildThresholdPercent) noexcept
{
	return progress * 100.0 >= buildThresholdPercent;
}

// ---- the town and the ground ----

double townBaseRadius(int type) noexcept
{
	switch (type)
	{
	case 2: return 2500.0; // TOWN
	case 3: return 937.5; // VILLAGE
	case 4: return 875.0; // RUINS
	case 9: return 437.5; // POI
	case 8: // NEST_MARKER
	case 10: return 0.0; // NULL
	default: return 1250.0; // NEST, OUTPOST, SLAVE_CAMP, MILITARY, PRISON
	}
}

double townRadius(int type, double radiusMult) noexcept
{
	if (radiusMult < 0.1)
		radiusMult = 1.0;
	return townBaseRadius(type) * std::clamp(radiusMult, 0.1, 5.0);
}

bool townBlocks(int type, double radius, double distance, bool createsPlayerTown) noexcept
{
	if (type == kTownTypeMarker)
		return false;
	const double k = type == kTownTypeRuins || type == kTownTypePoi ? 1.0 : kTownK;
	if (!(distance < radius * k || distance < kTownFloor))
		return false;
	return createsPlayerTown || distance < radius;
}

TownShape playerTownShape(const std::vector<std::pair<double, double>>& buildings) noexcept
{
	TownShape shape;
	if (buildings.empty())
		return shape;
	for (const auto& [x, z] : buildings)
	{
		shape.x += x;
		shape.z += z;
	}
	shape.x /= static_cast<double>(buildings.size());
	shape.z /= static_cast<double>(buildings.size());
	double radius = 0.0;
	for (const auto& [x, z] : buildings)
		radius = std::max(radius, std::hypot(x - shape.x, z - shape.z) + kPlayerTownMargin);
	shape.radius = std::clamp(radius, kPlayerTownMin, kPlayerTownMax);
	return shape;
}

void countTownBuilding(const kdata::GameData& data, const kdata::Record& record, TownCounters& counters)
{
	bool gate = false;
	if (const kdata::RefCategory* refs = record.findRefs("functionality"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* functionality = entry.opaqueEntry ? nullptr : data.find(entry.targetID);
			if (functionality == nullptr)
				continue;
			switch (functionality->intOr("function", 0))
			{
			case kFunctionResearch: counters.research = std::max<int>(counters.research, static_cast<int>(functionality->intOr("tech level", 0))); break;
			case kFunctionTurret: ++counters.turrets; break;
			case kFunctionGate: gate = true; break;
			default: break;
			}
		}
	}
	// G: a gate is `function` 21 or, as no shipped BUILDING carries that function, a record with `is wall gate` (UNSURE: the exe's id comes from the building's
	// class, +0x158; `is gateway` ramps are shells-excluded but not counted).
	if (gate || record.boolOr("is wall gate", false))
	{
		++counters.gates;
		gate = true;
	}
	// The shell test: an interior, and neither a WallBuilding nor a GatewayBuilding (UNSURE which BUILDING flags make those classes: `links together`
	// for walls; `is wall gate` / `is gateway` for gates).
	const kdata::RefCategory* interior = record.findRefs("interior");
	if (interior != nullptr && !interior->entries.empty() && !gate && !record.boolOr("links together", false)
		&& !record.boolOr("is wall gate", false) && !record.boolOr("is gateway", false))
		++counters.shells;
}

int townLevelOf(const TownCounters& c) noexcept
{
	int level = 0;
	if (c.research >= 2 || c.shells >= 2 || c.gates >= 1 || c.turrets >= 4)
		level = 1;
	if (c.gates >= 1)
	{
		if (c.research >= 4 || c.shells >= 5 || c.turrets >= 5)
			level = 2;
		if (c.research >= 4 && c.shells >= 5 && c.turrets >= 7)
			level = 3;
	}
	return level;
}

int townLevel(const kdata::GameData& data, const std::vector<const state::PlayerBuilding*>& members)
{
	TownCounters counters;
	for (const state::PlayerBuilding* member : members)
	{
		const kdata::Record* record = member->built && !member->ruin && !member->dismantled ? data.find(member->recordSid) : nullptr;
		if (record != nullptr)
			countTownBuilding(data, *record, counters);
	}
	return townLevelOf(counters);
}

bool groundBlocked(const std::vector<int>& partGroundTypes, double centreY) noexcept
{
	for (const int type : partGroundTypes)
	{
		if (type == kGroundTypeLand && centreY < kLandMinY)
			return true;
	}
	return false;
}

bool slopeBlocked(const double h[4], double halfA, double halfB, bool matchSlope, double maxSlopeDeg) noexcept
{
	// Corners (-a, -b), (+a, -b), (+a, +b), (-a, +b); the diagonals are corner 2 - corner 0 and corner 3 - corner 1.
	const double d1[3] = {2.0 * halfA, h[2] - h[0], 2.0 * halfB};
	const double d2[3] = {-2.0 * halfA, h[3] - h[1], 2.0 * halfB};
	double n[3] = {d1[1] * d2[2] - d1[2] * d2[1], d1[2] * d2[0] - d1[0] * d2[2], d1[0] * d2[1] - d1[1] * d2[0]};
	const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (length < 1e-9)
		return false; // a point footprint is level
	const double ny = std::fabs(n[1]) / length;
	if (ny < kMinNormalY)
		return true;
	return matchSlope && std::acos(std::min(ny, 1.0)) * 180.0 / 3.14159265358979323846 > maxSlopeDeg;
}

double builderFactor(unsigned builders) noexcept
{
	double sum = 0.0;
	double term = 1.0;
	for (unsigned k = 0; k < std::min(builders, kBuilderMax); ++k)
	{
		sum += term;
		term *= kBuilderDamping;
	}
	return sum;
}

double skillMult(double stat) noexcept
{
	return 0.3 + 1.7 * std::clamp(stat, 0.0, 100.0) / 100.0;
}

double builderSum(const std::vector<double>& stats) noexcept
{
	double sum = 0.0;
	double damping = 1.0;
	for (std::size_t k = 0; k < std::min<std::size_t>(stats.size(), kBuilderMax); ++k)
	{
		sum += skillMult(stats[k]) * damping;
		damping *= kBuilderDamping;
	}
	return sum;
}

// ---- placement geometry ----

void yawAxis(double qw, double qx, double qy, double qz, double& ax, double& az) noexcept
{
	const double x = 1.0 - 2.0 * (qy * qy + qz * qz);
	const double z = 2.0 * (qx * qz - qw * qy);
	const double length = std::hypot(x, z);
	if (length < 1e-9)
	{
		ax = 1.0;
		az = 0.0;
		return;
	}
	ax = x / length;
	az = z / length;
}

Box placeBox(const Footprint& footprint, double x, double y, double z, double qw, double qx, double qy, double qz) noexcept
{
	Box box;
	yawAxis(qw, qx, qy, qz, box.ax, box.az);
	const double bx = -box.az;
	const double bz = box.ax;
	if (footprint.valid)
	{
		const double lx = (footprint.minX + footprint.maxX) / 2.0;
		const double lz = (footprint.minZ + footprint.maxZ) / 2.0;
		box.cx = x + lx * box.ax + lz * bx;
		box.cz = z + lx * box.az + lz * bz;
		box.halfA = (footprint.maxX - footprint.minX) / 2.0;
		box.halfB = (footprint.maxZ - footprint.minZ) / 2.0;
		box.minY = y + footprint.minY;
		box.maxY = y + footprint.maxY;
	} else {
		box.cx = x;
		box.cz = z;
		box.halfA = kNoFootprintHalf;
		box.halfB = kNoFootprintHalf;
		box.minY = y - kNoFootprintHalf;
		box.maxY = y + 2.0 * kNoFootprintHalf;
	}
	return box;
}

bool boxesOverlap(const Box& a, const Box& b) noexcept
{
	if (a.maxY <= b.minY + kEps || b.maxY <= a.minY + kEps)
		return false;
	const double dx = b.cx - a.cx;
	const double dz = b.cz - a.cz;
	const double axes[4][2] = {{a.ax, a.az}, {-a.az, a.ax}, {b.ax, b.az}, {-b.az, b.ax}};
	for (const auto& axis : axes)
	{
		const double distance = std::fabs(dx * axis[0] + dz * axis[1]);
		const double reachA = a.halfA * std::fabs(a.ax * axis[0] + a.az * axis[1]) + a.halfB * std::fabs(-a.az * axis[0] + a.ax * axis[1]);
		const double reachB = b.halfA * std::fabs(b.ax * axis[0] + b.az * axis[1]) + b.halfB * std::fabs(-b.az * axis[0] + b.ax * axis[1]);
		if (distance >= reachA + reachB - kEps)
			return false;
	}
	return true;
}

bool pointInBox(const Box& box, double x, double y, double z) noexcept
{
	if (y < box.minY || y > box.maxY)
		return false;
	const double dx = x - box.cx;
	const double dz = z - box.cz;
	return std::fabs(dx * box.ax + dz * box.az) <= box.halfA && std::fabs(-dx * box.az + dz * box.ax) <= box.halfB;
}

} // namespace ksim::buildings

// =====================================================================================================================
// The Simulation side
// =====================================================================================================================

namespace ksim {

using namespace buildings;

// The collision library and the footprints derived from it (never saved).
struct BuildingGeometry
{
	BuildingGeometry(const std::string& dataDir, std::shared_ptr<const kdata::GameData> data)
		: gameData(std::move(data))
		, library(dataDir, *gameData)
	{
	}
	std::shared_ptr<const kdata::GameData> gameData;
	kworld::CollisionLibrary library;
	std::map<std::string, Footprint> footprints;

	// The union box of the record's collision shapes at the origin, yaw 0. The instance id feeds the part roll (select_parts), so it is
	// the record's own sid: the same footprint every time, from the record alone.
	const Footprint& of(const kdata::Record& record)
	{
		const auto found = footprints.find(record.stringID);
		if (found != footprints.end())
			return found->second;
		Footprint footprint;
		kworld::PlacedInstance placed;
		placed.instanceID = "player/" + record.stringID;
		placed.buildingID = record.stringID;
		placed.scale = record.floatOr("scale", 1.0f);
		placed.hasScale = true;
		std::vector<kworld::WorldShape> shapes;
		kworld::CollisionCoverage coverage;
		library.addBuilding(placed, shapes, coverage);
		for (const kworld::WorldShape& shape : shapes)
		{
			if (shape.use == kworld::NavUse::Door)
				continue;
			if (!footprint.valid)
			{
				footprint = {true, shape.min[0], shape.max[0], shape.min[2], shape.max[2], shape.min[1], shape.max[1]};
				continue;
			}
			footprint.minX = std::min<double>(footprint.minX, shape.min[0]);
			footprint.maxX = std::max<double>(footprint.maxX, shape.max[0]);
			footprint.minZ = std::min<double>(footprint.minZ, shape.min[2]);
			footprint.maxZ = std::max<double>(footprint.maxZ, shape.max[2]);
			footprint.minY = std::min<double>(footprint.minY, shape.min[1]);
			footprint.maxY = std::max<double>(footprint.maxY, shape.max[1]);
		}
		return footprints.emplace(record.stringID, footprint).first->second;
	}
};

namespace {

// The first (lowest Id) player faction's Id; 0 when there is none.
std::uint64_t playerFactionId(const flecs::world& ecs)
{
	std::uint64_t lowest = 0;
	ecs.each([&](const state::Id& id, const state::PlayerFaction&)
	{
		if (lowest == 0 || id.value < lowest)
			lowest = id.value;
	});
	return lowest;
}

std::uint64_t playerFactionId(const World& world)
{
	std::uint64_t lowest = 0;
	world.each([&](const state::Id& id, const state::PlayerFaction&)
	{
		if (lowest == 0 || id.value < lowest)
			lowest = id.value;
	});
	return lowest;
}

// The flecs id of the first player faction's entity (0 when none); TownState::owner holds one.
flecs::entity_t playerFactionEntity(const flecs::world& ecs)
{
	std::uint64_t lowest = 0;
	flecs::entity_t entity = 0;
	ecs.each([&](flecs::entity e, const state::Id& id, const state::PlayerFaction&)
	{
		if (lowest == 0 || id.value < lowest)
		{
			lowest = id.value;
			entity = e.id();
		}
	});
	return entity;
}

flecs::entity findBuilding(const flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&](flecs::entity entity, const state::Id& i, const state::PlayerBuilding&)
	{
		if (i.value == id)
			found = entity;
	});
	return found;
}

const kdata::Record* functionalityOf(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* refs = building.findRefs("functionality");
	if (refs == nullptr || refs->entries.empty() || refs->entries.front().opaqueEntry)
		return nullptr;
	return data.find(refs->entries.front().targetID);
}

} // namespace

// What a judged placement found.
struct BuildJudge
{
	const kdata::Record* record = nullptr;
	Box box;
	std::uint64_t faction = 0;
};

std::shared_ptr<BuildingGeometry> Simulation::buildingGeometry() const
{
	if (buildingGeometry_ == nullptr && gameData_ != nullptr)
		buildingGeometry_ = std::make_shared<BuildingGeometry>(world_.dataDir(), gameData_);
	return buildingGeometry_;
}

void Simulation::setGameSpeed(double speed) noexcept
{
	gameSpeed_ = speed;
	cadence_ = speed >= 3.0 ? 3u : speed >= 2.0 ? 2u : 1u;
}

void Simulation::setBuildSpeed(double ka) noexcept
{
	buildSpeed_ = std::clamp(ka, kKaMin, kKaMax);
}

std::string Simulation::townBlocker(double x, double z, bool createsPlayerTown, std::uint64_t faction) const
{
	// T is the nearest town by centre distance (every TOWN record but NEST_MARKER, and every player town); the faction's own never blocks.
	double best = 0.0;
	std::string blocker;
	bool found = false;
	bool blocks = false;
	world_.ecs().each([&](const state::TownState& town)
	{
		const kdata::Record* record = gameData_ != nullptr ? gameData_->find(town.activeSid) : nullptr;
		if (record == nullptr)
			return;
		const int type = record->intOr("type", 2);
		if (type == kTownTypeMarker)
			return;
		const double d = std::hypot(town.x - x, town.z - z);
		if (found && d >= best)
			return;
		found = true;
		best = d;
		blocker = town.activeSid;
		const double radius = townRadius(type, record->floatOr("town radius mult", 1.0f));
		blocks = townBlocks(type, radius, d, createsPlayerTown) && !(faction != 0 && town.owner.id != 0 && playerFactionEntity(world_.ecs()) == town.owner.id);
	});
	world_.ecs().each([&](const state::Id& id, const state::PlayerTown& town)
	{
		const double d = std::hypot(town.x - x, town.z - z);
		if (found && d >= best)
			return;
		found = true;
		best = d;
		blocker = "player:" + std::to_string(id.value);
		blocks = town.owner != faction && townBlocks(2, town.radius > 1.0 ? town.radius : kPlayerTownDefault, d, createsPlayerTown);
	});
	return found && blocks ? blocker : std::string();
}

std::string Simulation::judgeBuild(const BuildOrder& order, BuildJudge& judge) const
{
	if (gameData_ == nullptr)
		return "unsupported";
	judge.faction = playerFactionId(world_.ecs());
	if (judge.faction == 0)
		return "no_faction"; // checked before `locked`: without a faction the unlocked set is empty, which would hide it
	const std::vector<std::string> unlocked = unlockedBuildings(completedResearch());
	if (!order.scenario && std::find(unlocked.begin(), unlocked.end(), order.record) == unlocked.end())
		return "locked";
	const double qNorm = std::sqrt(order.qw * order.qw + order.qx * order.qx + order.qy * order.qy + order.qz * order.qz);
	if (!std::isfinite(order.x + order.y + order.z + qNorm) || order.floor > kMaxFloor || std::fabs(qNorm - 1.0) > kQuatTolerance
		|| std::fabs(order.x) > kworld::kWorldExtent || std::fabs(order.z) > kworld::kWorldExtent)
		return "bad_value";
	judge.record = gameData_->find(order.record);
	if (judge.record == nullptr)
		return "locked";
	const kdata::Record& record = *judge.record;
	if (*siteSupport(record, functionalityOf(*gameData_, record)) != '\0')
		return "unsupported_site";
	// The town test (Q2, Q3): the nearest town, unless it is the faction's own.
	if (!order.scenario && !townBlocker(order.x, order.z, record.boolOr("creates player town", true), judge.faction).empty())
		return "too_close_town";
	const std::optional<double> ground = groundHeight(order.x, order.z);
	const std::shared_ptr<BuildingGeometry> geometry = buildingGeometry();
	if (!ground || geometry == nullptr)
		return "unsupported"; // no heightmap
	if (std::fabs(order.y - *ground) > kHeightTolerance)
		return "bad_value";
	judge.box = placeBox(geometry->of(record), order.x, order.y, order.z, order.qw, order.qx, order.qy, order.qz);
	const Box& box = judge.box;
	// characters_in_way (11): any living fighter in the footprint.
	if (combat_ != nullptr && !order.scenario)
	{
		Box reach = box;
		reach.minY -= 5.0;
		bool inWay = false;
		world_.ecs().each([&](const state::Id& id, const state::Fighter&)
		{
			const state::Fighter* f = combat_->find(id.value);
			inWay = inWay || (f != nullptr && !f->dead && pointInBox(reach, f->x, f->y, f->z));
		});
		if (inWay)
			return "characters_in_way";
	}
	// invalid_ground (Q4): a LAND part needs the footprint centre's Y at 98 or more; ANY and WATER parts are not tested.
	std::vector<int> groundTypes;
	if (const kdata::RefCategory* parts = record.findRefs("parts"))
	{
		for (const kdata::RefEntry& entry : parts->entries)
		{
			const kdata::Record* part = entry.opaqueEntry ? nullptr : gameData_->find(entry.targetID);
			if (part != nullptr)
				groundTypes.push_back(part->intOr("ground type", 0));
		}
	}
	if (groundBlocked(groundTypes, order.y))
		return "invalid_ground";
	// not_level (Q5): the terrain normal from the footprint's four corner heights; the slope factor is the exe's default 1.0.
	double corners[4];
	const double cu[4] = {-1.0, 1.0, 1.0, -1.0};
	const double cv[4] = {-1.0, -1.0, 1.0, 1.0};
	for (int i = 0; i < 4; ++i)
	{
		const double u = cu[i] * box.halfA;
		const double v = cv[i] * box.halfB;
		const std::optional<double> h = groundHeight(box.cx + u * box.ax - v * box.az, box.cz + u * box.az + v * box.ax);
		if (!h)
			return "bad_value";
		corners[i] = *h;
	}
	if (slopeBlocked(corners, box.halfA, box.halfB, record.boolOr("match slope", false), record.floatOr("max slope", 45.0f)))
		return "not_level";
	// overlap: the footprint boxes of other player buildings and of the placed buildings that have doors. UNSURE: the world's other
	// static shapes (foliage, doorless buildings) are not kept by the sim, so a site on them passes.
	bool overlaps = false;
	world_.ecs().each([&](const state::PlayerBuilding& other)
	{
		if (overlaps)
			return;
		const kdata::Record* otherRecord = gameData_->find(other.recordSid);
		if (otherRecord == nullptr)
			return;
		overlaps = boxesOverlap(box, placeBox(geometry->of(*otherRecord), other.x, other.y, other.z, other.qw, other.qx, other.qy, other.qz));
	});
	for (const BuildingSite& site : world_.buildingSites())
	{
		if (!overlaps)
			overlaps = boxesOverlap(box, Box{site.cx, site.cz, site.axisX, site.axisZ, site.halfA, site.halfB, site.minY, site.maxY});
	}
	if (overlaps && !order.scenario)
		return "overlap";
	return "";
}

std::string Simulation::checkBuildPlace(const BuildOrder& order) const
{
	BuildJudge judge;
	return judgeBuild(order, judge);
}

std::string Simulation::runBuildPlace(const BuildOrder& order, bool commit)
{
	BuildJudge judge;
	std::string reason = judgeBuild(order, judge);
	if (!reason.empty() || !commit)
		return reason;
	flecs::world& ecs = ecsMutable();
	const kdata::Record& record = *judge.record;
	state::PlayerBuilding building;
	building.recordSid = record.stringID;
	building.ownerFaction = judge.faction;
	building.x = order.x;
	building.y = order.y;
	building.z = order.z;
	building.qw = order.qw;
	building.qx = order.qx;
	building.qy = order.qy;
	building.qz = order.qz;
	building.floor = order.floor;
	building.mats = buildingCost(record);
	building.maxHp = maxHp(record);
	building.placedTick = clockTick();
	if (building.mats.empty())
	{
		building.built = true; // no cost: born complete at maxHp (the door code's rule)
		building.hp = building.maxHp;
	}
	building.status = statusOf(building);

	// The town: the faction's own within reach, else a new one when the record `creates player town`.
	flecs::entity town{};
	double nearest = kOwnTownReach;
	ecs.each([&](flecs::entity entity, const state::PlayerTown& t)
	{
		const double d = std::hypot(t.x - order.x, t.z - order.z);
		if (t.owner == judge.faction && d < nearest)
		{
			nearest = d;
			town = entity;
		}
	});
	if (!town.is_valid() && record.boolOr("creates player town", true))
	{
		town = state::createInstance(ecs);
		town.set<state::PlayerTown>(state::PlayerTown{kDefaultTownName, judge.faction, order.x, order.z, kPlayerTownDefault, {}});
	}
	flecs::entity entity = state::createInstance(ecs);
	const std::uint64_t id = entity.get<state::Id>()->value;
	if (town.is_valid())
	{
		building.town = town.get<state::Id>()->value;
		state::PlayerTown* t = town.get_mut<state::PlayerTown>();
		t->members.push_back(id);
		// Re-centre on the members and grow the radius (Town::vf75, Q3).
		std::vector<std::pair<double, double>> at{{order.x, order.z}};
		ecs.each([&](const state::PlayerBuilding& other)
		{
			if (other.town == building.town)
				at.emplace_back(other.x, other.z);
		});
		const TownShape shape = playerTownShape(at);
		t->x = shape.x;
		t->z = shape.z;
		t->radius = shape.radius;
	}
	entity.set<state::PlayerBuilding>(std::move(building));
	if (!order.scenario)
	{
		kworld::PlacedInstance placed;
		placed.instanceID = "player/" + std::to_string(id);
		placed.buildingID = record.stringID;
		placed.x = order.x;
		placed.y = order.y;
		placed.z = order.z;
		placed.qw = order.qw;
		placed.qx = order.qx;
		placed.qy = order.qy;
		placed.qz = order.qz;
		standBuilding(std::move(placed));
	}
	return "";
}

std::uint64_t Simulation::placeScenarioBuilding(const std::string& recordSid, double x, double z, double yaw, bool built, const std::string& faction, std::string& reason)
{
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(recordSid) : nullptr;
	const std::optional<double> ground = groundHeight(x, z);
	if (record == nullptr || !ground)
	{
		reason = record == nullptr ? "no such record" : "no ground height";
		return 0;
	}
	const std::shared_ptr<const producers::Book> book = producerBook();
	if (const producers::Spec* spec = built && book != nullptr ? book->find(record->stringID) : nullptr; spec != nullptr)
	{
		// A finished producer (a mine, a machine) is the producers.hpp entity, a site of its own beside the placements and the deposits.
		static constexpr std::uint64_t kScenarioIdBase = 1ull << 41;
		ProducerSite site;
		site.id = producers::kIdBase + kScenarioIdBase + scenarioProducers_++;
		site.recordSid = record->stringID;
		site.ownerFaction = faction;
		site.x = x;
		site.y = *ground;
		site.z = z;
		double useX = x, useY = *ground, useZ = z;
		if (navigation_ != nullptr)
		{
			const auto spot = navigation_->walker().snap({x, *ground, z}, producers::kUseSnap);
			if (spot)
			{
				useX = spot->x;
				useY = spot->y;
				useZ = spot->z;
			}
		}
		spawnProducer(site, useX, useY, useZ);
		return site.id;
	}
	BuildOrder order;
	order.record = record->stringID;
	order.x = x;
	order.y = *ground;
	order.z = z;
	order.qw = std::cos(yaw * 0.5);
	order.qy = std::sin(yaw * 0.5);
	order.scenario = true;
	std::uint64_t before = 0;
	ecsMutable().each([&](const state::Id& i, const state::PlayerBuilding&) { before = std::max(before, i.value); });
	reason = runBuildPlace(order, true);
	if (!reason.empty())
		return 0;
	std::uint64_t placed = 0;
	ecsMutable().each([&](const state::Id& i, const state::PlayerBuilding&) { placed = std::max(placed, i.value); });
	if (placed <= before)
	{
		reason = "no building made";
		return 0;
	}
	flecs::entity entity = findBuilding(world_.ecs(), placed);
	if (!entity.is_valid())
		return 0;
	state::PlayerBuilding* b = entity.get_mut<state::PlayerBuilding>();
	for (state::BuildMaterial& row : b->mats)
		row.have = row.need; // the materials are already on site: the builders only build
	if (built)
	{
		b->built = true;
		b->hp = b->maxHp;
	}
	b->status = statusOf(*b);
	++b->version;
	return placed;
}

std::string Simulation::scenarioTask(std::uint64_t actor, const std::string& task, std::uint64_t target)
{
	if (task == "shackle")
	{
		shackleTarget(actor);
		return shacklesLocked(actor) ? "" : "no_shackles";
	}
	if (task == "pick_cage" || task == "cut_cage")
		return orderLockTask(actor, task == "pick_cage" ? routines::task::PickLock : routines::task::CutLock, actor) ? "" : "refused";
	if (task == "pick_shackles" || task == "cut_shackles")
		return orderShackleAction(actor, target, task == "pick_shackles" ? kPickShacklesAction : kCutShacklesAction);
	if (task == "operate")
	{
		bool found = false;
		world_.ecs().each([&](const state::Id& i, const state::Producer&) { found = found || i.value == target; });
		if (!found)
			return "bad_target";
		cancelOrders(actor);
		beginInteract(actor, target, static_cast<std::uint8_t>(InteractAction::Use), 0, {}, false);
		return "";
	}
	if (task == "build")
		return orderConstructionTask(actor, routines::task::Build, target) ? "" : "refused";
	return "bad_value";
}

std::string Simulation::checkBuildPause(std::uint64_t building) const
{
	const flecs::entity entity = findBuilding(world_.ecs(), building);
	if (!entity.is_valid())
		return "bad_target";
	const state::PlayerBuilding* b = entity.get<state::PlayerBuilding>();
	return b->ownerFaction != playerFactionId(world_.ecs()) || b->built ? "bad_target" : "";
}

std::string Simulation::runBuildPause(std::uint64_t building, bool paused, bool commit)
{
	std::string reason = checkBuildPause(building);
	if (!reason.empty() || !commit)
		return reason;
	state::PlayerBuilding* b = findBuilding(world_.ecs(), building).get_mut<state::PlayerBuilding>();
	const std::uint64_t before = signatureOf(*b);
	b->paused = paused;
	b->status = statusOf(*b);
	if (signatureOf(*b) != before)
		++b->version;
	return "";
}

std::uint32_t Simulation::addMaterials(std::uint64_t building, const std::string& item, std::uint32_t count)
{
	const flecs::entity entity = findBuilding(world_.ecs(), building);
	if (!entity.is_valid())
		return 0;
	state::PlayerBuilding* b = entity.get_mut<state::PlayerBuilding>();
	const std::uint32_t taken = std::min(count, accepts(*b, item));
	if (taken == 0)
		return 0;
	const std::uint64_t before = signatureOf(*b);
	for (state::BuildMaterial& row : b->mats)
	{
		if (row.itemSid == item)
			row.have += taken;
	}
	b->status = statusOf(*b);
	if (signatureOf(*b) != before)
		++b->version;
	return taken;
}

bool Simulation::setBuilders(std::uint64_t building, std::uint16_t count)
{
	const flecs::entity entity = findBuilding(world_.ecs(), building);
	if (!entity.is_valid())
		return false;
	state::PlayerBuilding* b = entity.get_mut<state::PlayerBuilding>();
	const std::uint64_t before = signatureOf(*b);
	b->workers = count;
	b->status = statusOf(*b);
	if (signatureOf(*b) != before)
		++b->version;
	return true;
}

double Simulation::buildHoursOf(std::uint64_t building) const
{
	const flecs::entity entity = findBuilding(world_.ecs(), building);
	if (!entity.is_valid() || gameData_ == nullptr)
		return 0.0;
	const state::PlayerBuilding* b = entity.get<state::PlayerBuilding>();
	const kdata::Record* record = gameData_->find(b->recordSid);
	return buildHours(totalMaterials(b->mats), record != nullptr ? record->floatOr("build speed mult", 1.0f) : 1.0, buildSpeed_);
}

void Simulation::stepConstruction()
{
	flecs::world& ecs = ecsMutable();
	// world.each() builds an uncached query that walks every table; most worlds have no player buildings.
	if (ecs.count<state::PlayerBuilding>() == 0)
		return;
	ecs.each([&](const state::Id& id, state::PlayerBuilding& b)
	{
		if (b.built)
			return;
		const std::uint64_t before = signatureOf(b);
		const bool wasBuilt = b.built;
		const kdata::Record* record = gameData_ != nullptr ? gameData_->find(b.recordSid) : nullptr;
		const double hours = buildHours(totalMaterials(b.mats), record != nullptr ? record->floatOr("build speed mult", 1.0f) : 1.0, buildSpeed_);
		// The Build tasks' power (Sum skillMult x 0.75^k over the builders of this tick) when any worked, else the scripted worker count.
		const auto power = buildPower_.find(id.value);
		stepSite(b, hours, power != buildPower_.end() ? power->second : builderFactor(b.workers));
		b.status = statusOf(b);
		if (signatureOf(b) != before)
			++b.version;
		if (b.built && !wasBuilt)
		{
			std::ostringstream line;
			line << "construction complete building " << id.value << " " << b.recordSid;
			logTask(line.str());
		}
	});
	buildPower_.clear();
	buildersOn_.clear();
}

std::vector<Simulation::BuildingView> Simulation::playerBuildings() const
{
	std::vector<BuildingView> views;
	const std::uint64_t faction = playerFactionId(world_);
	if (faction == 0)
		return views;
	world_.each([&](const state::Id& id, const state::PlayerBuilding& b)
	{
		if (b.ownerFaction != faction)
			return;
		BuildingView v;
		v.id = id.value;
		v.record = b.recordSid;
		v.owner = b.ownerFaction;
		v.town = b.town;
		v.x = b.x;
		v.y = b.y;
		v.z = b.z;
		v.qw = b.qw;
		v.qx = b.qx;
		v.qy = b.qy;
		v.qz = b.qz;
		v.floor = b.floor;
		v.status = b.status;
		v.flags = flagsOf(b);
		v.progress = progressOf(b);
		v.hp = b.hp;
		v.maxHp = b.maxHp;
		v.version = b.version;
		if (!b.built)
		{
			v.mats = b.mats;
			const kdata::Record* record = gameData_ != nullptr ? gameData_->find(b.recordSid) : nullptr;
			v.rate = rateOf(b, buildHours(totalMaterials(b.mats), record != nullptr ? record->floatOr("build speed mult", 1.0f) : 1.0, buildSpeed_));
		}
		views.push_back(std::move(v));
	});
	std::sort(views.begin(), views.end(), [](const BuildingView& a, const BuildingView& b) { return a.id < b.id; });
	return views;
}

namespace {

// Reach of the build tasks: Task_Build has no distance test (a separate MOVE step brings the character, b50c Q6); a building has no
// footprint here, so the actor works from within this of its centre. UNSURE: ours.
constexpr double kConstructionReach = 40.0;
// 156 / 200: how near the interior point (156) or the exit point (200) the walk must end. UNSURE: ours (the exe's point is the interior
// object's position / the exit object's extrapolated point, dt24_enterleave section 2).
constexpr double kEnterReach = 8.0;
constexpr double kConstructionGiveUpSeconds = 30.0;
// Where a dismantle's refund lies: scattered within this of the centre (the exe picks points inside the AABB, FUN_14029dc10; UNSURE).
constexpr double kRefundScatter = 10.0;
// 97 trains the skill at the producers' operator rate (producers.hpp kXpPerSecond) up to the building's `output rate` level (the "Training
// max: lvl 6" of build21 section 5). UNSURE: Task_UseTrainingDummy's body is unread; the skill is melee attack for every dummy.
constexpr double kTrainingXpPerSecond = 0.02 * 0.75 * 0.5;

// A deterministic U(0, 1) from a seed (splitmix64).
double unit(std::uint64_t seed)
{
	seed += 0x9E3779B97F4A7C15ull;
	seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
	seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
	seed ^= seed >> 31;
	return static_cast<double>(seed >> 11) / 9007199254740992.0;
}

} // namespace

bool Simulation::orderConstructionTask(std::uint64_t id, std::uint16_t task, std::uint64_t building)
{
	if (!routines::task::isConstructionTask(task) || combat_ == nullptr || combat_->find(id) == nullptr || gameData_ == nullptr)
		return false;
	const flecs::entity site = findBuilding(world_.ecs(), building);
	const std::uint64_t player = playerFactionId(world_.ecs());
	if (!site.is_valid() || player == 0)
		return false;
	const state::PlayerBuilding* b = site.get<state::PlayerBuilding>();
	const state::Allegiance* own = fighterEntity(id).get<state::Allegiance>();
	const state::FactionState* faction = world_.ecs().entity(playerFactionEntity(world_.ecs())).get<state::FactionState>();
	if (b->ownerFaction != player || b->dismantled || own == nullptr || faction == nullptr || own->factionSid != faction->templateSid)
		return false;
	if (task == routines::task::UseTrainingDummy)
	{
		const kdata::Record* record = gameData_->find(b->recordSid);
		bool dummy = false;
		if (record != nullptr)
			for (const std::int32_t function : routines::buildingFunctions(*gameData_, *record))
				dummy = dummy || function == routines::kFunctionTraining;
		if (!dummy || !b->built)
			return false;
	}
	buildOrders_[id] = ConstructionOrder{task, building, clockTick(), 0.0};
	return true;
}

// 125 JOB_BUILDER (dt24_101 section 3): the resolver is BuildingFinderForEngineers - a construction site near the town point - and the goal's
// isBuildingComplete is the finish test. The finder checks nothing about materials; the task on top is OURS (the coordinator's design,
// "the way the Medic job picks a patient"): the nearest player building with work left within kJobEngineerRange; materials missing and one
// of them in the pack -> 71, a site (or ruin) the delivered materials still let it build -> 2, a built-but-damaged one -> 95. A site with
// nothing in the pack and nothing buildable is skipped (the hauling that fetches materials is not modelled, UNSURE).
std::pair<std::uint16_t, std::uint64_t> Simulation::planEngineer(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	const std::uint64_t player = playerFactionId(world_.ecs());
	if (f == nullptr || player == 0)
		return {};
	const state::Inventory* pack = fighterEntity(id).get<state::Inventory>();
	std::pair<std::uint16_t, std::uint64_t> best{};
	double bestD = 0.0;
	world_.ecs().each([&](const state::Id& buildingId, const state::PlayerBuilding& b)
	{
		if (b.ownerFaction != player || b.dismantled || whole(b))
			return;
		const double d = std::hypot(f->x - b.x, f->z - b.z);
		if (d > kJobEngineerRange || (best.second != 0 && (d > bestD || (d == bestD && buildingId.value > best.second))))
			return;
		std::uint16_t task = 0;
		bool carried = false;
		if (pack != nullptr)
			for (const std::size_t row : missingRows(b))
				for (const state::Inventory::Entry& entry : pack->items)
					carried = carried || (entry.slot == state::Inventory::kCarried && entry.count > 0 && entry.itemSid == b.mats[row].itemSid);
		if (carried)
			task = routines::task::AddMaterials;
		else if (buildable(b))
			task = b.broken && !b.ruin ? routines::task::Repair : routines::task::Build; // `broken` = a damaged building that was built
		if (task == 0)
			return;
		best = {task, buildingId.value};
		bestD = d;
	});
	return best;
}

void Simulation::dropRefund(const state::PlayerBuilding& b, std::uint64_t buildingId)
{
	const std::uint64_t now = clockTick();
	std::uint64_t salt = 0;
	const auto draw = [&]() { return unit(buildingId * 0x1000193ull + now * 31 + ++salt); };
	for (const state::BuildMaterial& row : refundOf(b, kMinDismantleFraction, draw))
	{
		state::GroundItem item;
		item.itemSid = row.itemSid;
		item.count = row.have;
		item.x = b.x + (draw() * 2.0 - 1.0) * kRefundScatter;
		item.z = b.z + (draw() * 2.0 - 1.0) * kRefundScatter;
		item.y = b.y;
		item.useX = item.x;
		item.useY = item.y;
		item.useZ = item.z;
		spawnGroundItem(std::move(item));
		std::ostringstream line;
		line << "dismantle refund building " << buildingId << ": " << row.have << " x " << row.itemSid;
		logTask(line.str());
	}
}

Simulation::ConstructionStep Simulation::stepConstructionTask(std::uint64_t id, ConstructionOrder& order, const std::function<bool(double, double)>& walk)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0)
		return ConstructionStep::Refused;
	const flecs::entity site = findBuilding(world_.ecs(), order.building);
	if (!site.is_valid())
		return ConstructionStep::Refused;
	state::PlayerBuilding* b = site.get_mut<state::PlayerBuilding>();
	if (b->dismantled)
		return ConstructionStep::Refused;
	const std::uint16_t task = order.task;
	const bool doors = task == routines::task::EnterBuilding || task == routines::task::LeaveBuilding;
	const auto logLine = [&](const std::string& what)
	{
		std::ostringstream line;
		line << "build " << task << " member " << id << " building " << order.building << " " << what;
		logTask(line.str());
	};
	// The goals that are already met end the task before any walk (156 / 200: the interior state; 2 / 95: nothing to build).
	if (task == routines::task::EnterBuilding && f->building == order.building)
		return ConstructionStep::Done;
	if (task == routines::task::LeaveBuilding && f->building != order.building)
		return ConstructionStep::Done;
	if ((task == routines::task::Build || task == routines::task::Repair) && whole(*b))
		return ConstructionStep::Done;
	if (task == routines::task::AddMaterials && missingRows(*b).empty() && !b->ruin)
		return ConstructionStep::Done;
	if (task == routines::task::Dismantle)
	{
		// Task_Dismantle refuses over furniture or mounted buildings (b50c Q18: the barks "I have to dismantle the furniture first." / "I have to
		// unmount the buildings first.").
		if (b->mounted != 0)
		{
			logLine("refused: unmount the buildings first");
			return ConstructionStep::Refused;
		}
		if (b->furniture != 0)
		{
			logLine("refused: dismantle the furniture first");
			return ConstructionStep::Refused;
		}
	}
	const double reach = doors ? kEnterReach : kConstructionReach;
	if (std::hypot(f->x - b->x, f->z - b->z) > reach)
	{
		if (!f->walking())
		{
			if (static_cast<double>(ticksSince(clockTick(), order.since)) * kTickSeconds > kConstructionGiveUpSeconds)
				return ConstructionStep::Refused;
			if (clockTick() % kSquadIntervalTicks == 0)
				walk(b->x, b->z);
		}
		return ConstructionStep::Working;
	}
	f->clearRoute();
	f->forwardSpeed = 0.0;
	f->lateralSpeed = 0.0;
	const std::uint64_t before = signatureOf(*b);
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(b->recordSid) : nullptr;
	const double mult = record != nullptr ? record->floatOr("build speed mult", 1.0f) : 1.0;
	ConstructionStep result = ConstructionStep::Working;
	const auto revive = [&]()
	{
		std::uint64_t salt = 0;
		buildings::revive(*b, [&]() { return unit(order.building * 7919ull + clockTick() + ++salt); });
		logLine("revived the ruin");
	};

	if (task == routines::task::EnterBuilding)
	{
		f->building = order.building;
		logLine("entered");
		result = ConstructionStep::Done;
	} else if (task == routines::task::LeaveBuilding) {
		f->building = 0;
		logLine("left");
		result = ConstructionStep::Done;
	} else if (task == routines::task::UseTrainingDummy) {
		const double cap = record != nullptr ? record->floatOr("output rate", 0.0f) : 0.0;
		if (cap > 0.0 && f->skills.attack >= cap)
			return ConstructionStep::Done;
		combat_->rules().addXp(f->skills.attack, kTrainingXpPerSecond * kTickSeconds);
	} else if (task == routines::task::AddMaterials) {
		order.elapsed += kTickSeconds * kAddMaterialsWarmup;
		if (order.elapsed < 1.0)
			return ConstructionStep::Working;
		const std::vector<std::size_t> missing = missingRows(*b);
		flecs::entity actor = fighterEntity(id);
		state::Inventory mine;
		if (const state::Inventory* packed = actor.get<state::Inventory>())
			mine = *packed;
		const grid::Rules rules(*gameData_);
		std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, id);
		std::uint32_t put = 0;
		for (const std::size_t row : missing)
		{
			// One unit per missing key per tick (Q7); the actor's pack first (the building-material container fallback is not modelled).
			for (std::size_t at = 0; at < mine.items.size(); ++at)
			{
				state::Inventory::Entry& entry = mine.items[at];
				if (entry.slot != state::Inventory::kCarried || entry.count == 0 || entry.itemSid != b->mats[row].itemSid)
					continue;
				if (--entry.count == 0)
					mine.items.erase(mine.items.begin() + static_cast<std::ptrdiff_t>(at));
				++b->mats[row].have;
				++put;
				break;
			}
		}
		if (put == 0)
		{
			logLine("refused: no material of a missing kind in the pack");
			return ConstructionStep::Refused; // CharBody::vf5 "impossible"
		}
		++mine.version;
		storeInventory(actor, id, std::move(mine), std::move(overflow));
		if (b->ruin)
			revive();
		logLine("added " + std::to_string(put) + " units");
		if (missingRows(*b).empty())
			result = ConstructionStep::Done;
	} else if (task == routines::task::Dismantle) {
		// Task_Dismantle (Q18): HP falls by dt x lerp(stat / 100, 0.05, 4.0) x 4.0 x rate, no Ka / Kb.
		const double stat = std::clamp(f->skills.engineer, 0.0, 100.0) * 0.01;
		const double work = kTickSeconds * (kDismantleLow + (kDismantleHigh - kDismantleLow) * stat) * kDismantleScale;
		b->dismantling = true;
		combat_->rules().addXp(f->skills.engineer, producers::kXpPerSecond * kTickSeconds);
		if (removeHp(*b, work, mult / kRateDivisor))
		{
			b->dismantling = false;
			dropRefund(*b, order.building);
			logLine("dismantled");
			result = ConstructionStep::Done;
		}
	} else {
		// 2 BUILD / 95 REPAIR: Task_Build. A ruin revives on its first call; the materials delivered cap the HP (Q8: the task is impossible
		// once HP reaches the cap and the AI fetches more).
		if (b->ruin)
			revive();
		if (!buildable(*b))
		{
			logLine("refused: the delivered materials are all built in");
			return ConstructionStep::Refused;
		}
		const unsigned k = buildersOn_[order.building]++;
		buildPower_[order.building] += skillMult(f->skills.engineer) * std::pow(kBuilderDamping, std::min(k, kBuilderMax - 1));
		combat_->rules().addXp(f->skills.engineer, producers::kXpPerSecond * kTickSeconds);
	}
	b->status = statusOf(*b);
	if (signatureOf(*b) != before)
		++b->version;
	return result;
}

bool Simulation::constructionProgress(std::uint64_t id, TaskProgressRow& row) const
{
	const auto order = buildOrders_.find(id);
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (order == buildOrders_.end() || f == nullptr)
		return false;
	const flecs::entity site = findBuilding(world_.ecs(), order->second.building);
	const state::PlayerBuilding* b = site.is_valid() ? site.get<state::PlayerBuilding>() : nullptr;
	if (b == nullptr || b->dismantled || std::hypot(f->x - b->x, f->z - b->z) > kConstructionReach)
		return false; // still walking: the bar starts at the site
	row.id = id;
	row.target = order->second.building;
	const float hp = static_cast<float>(std::clamp(b->hp / b->maxHp, 0.0, 1.0));
	switch (order->second.task)
	{
	case routines::task::AddMaterials:
		row.kind = 5;
		row.progress = static_cast<float>(std::min(order->second.elapsed, 1.0));
		row.rate = order->second.elapsed < 1.0 ? static_cast<float>(kAddMaterialsWarmup) : 0.0f; // the warm-up runs 0.2 per game second
		row.flags = kProgressOriginal;
		return true;
	case routines::task::Build:
	case routines::task::Repair:
		row.kind = order->second.task == routines::task::Build ? 11 : 12;
		row.progress = hp; // the rate is not constant (builders' damping, the delivered materials), so the session's tolerance paces the frames
		return !whole(*b);
	case routines::task::Dismantle:
		row.kind = 13;
		row.progress = 1.0f - hp;
		return b->dismantling;
	default:
		return false;
	}
}

void Simulation::stepConstructionOrders(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	for (auto it = buildOrders_.begin(); it != buildOrders_.end();)
	{
		const std::uint64_t actor = it->first;
		const ConstructionStep step = stepConstructionTask(actor, it->second, [&](double x, double z) { return walk(actor, x, z); });
		it = step == ConstructionStep::Working ? std::next(it) : buildOrders_.erase(it);
	}
}

} // namespace ksim
