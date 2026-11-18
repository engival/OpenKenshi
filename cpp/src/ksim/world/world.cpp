#include "ksim/world/world.hpp"

#include "ksim/economy/producers.hpp"
#include "ksim/ai/routines.hpp"


#include "kdata/format.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ksim {

// The loader builds templates only.
using namespace tmpl;

namespace {

// Record type ids from kdata's itemType table (kdata/format.hpp).
constexpr std::int32_t kBuildingType = 0;
constexpr std::int32_t kCharacterType = 1;
constexpr std::int32_t kRaceType = 7;
constexpr std::int32_t kFactionType = 10;
constexpr std::int32_t kTownType = 13;
constexpr std::int32_t kSquadTemplateType = 52;
constexpr std::int32_t kAiPackageType = 72;
constexpr std::int32_t kHumanCharacterType = 75;
constexpr std::int32_t kAnimalCharacterType = 76;

// Number of (source, target) pair entries for a relation. ecs_count_id cannot
// be used here: it counts source entities that own at least one pair.
template <typename Relation>
std::size_t countPairs(const flecs::world& ecs,
	const std::unordered_map<std::string, flecs::entity>& prefabs)
{
	const flecs::entity_t relation = ecs.component<Relation>().id();
	std::size_t count = 0;
	for (const auto& entry : prefabs)
	{
		const ecs_type_t* type = ecs_get_type(ecs.c_ptr(), entry.second.id());
		if (type == nullptr)
			continue;
		for (std::int32_t i = 0; i < type->count; ++i)
		{
			if (ECS_HAS_RELATION(type->array[i], relation))
				++count;
		}
	}
	return count;
}

// The slab's horizontal normal (unit) as the exe takes it, FUN_14029cc90: a column of the placement's orientation chosen by the
// door's `door navmesh axis`. Measured on the 133 gate placements: for axis 1 (every Defensive Gate and Wall gate: 125) the
// placement's local +X column is parallel to the slab's normal in all of them (the principal axis of the door shape agrees to
// 0.9), and on the United Cities walled town it points away from the town at both gates (north gate +z, south gate -z); the
// Makeshift Gate (axis 0, 8 placements) has a door shape whose axes the column only roughly follows (29 degrees off), so it uses
// the +Z column. UNSURE: which sign counts as outside is the model's authored orientation; this is the one reading that fits the
// two measured gates, and the exe's second cross-product operand (0x142248210) is not decoded.
bool gateNormal(const kworld::PlacedInstance& instance, const kdata::Record* door, double& nx, double& nz)
{
	const double w = instance.qw, x = instance.qx, y = instance.qy, z = instance.qz;
	const int axis = door != nullptr ? door->intOr("door navmesh axis", 0) : 0;
	double cx, cz;
	if (axis == 1)
	{
		cx = 1 - 2 * (y * y + z * z);
		cz = 2 * (x * z - w * y);
	} else {
		cx = 2 * (x * z + w * y);
		cz = 1 - 2 * (x * x + y * y);
	}
	const double length = std::hypot(cx, cz);
	if (length < 1.0e-6)
		return false;
	nx = cx / length;
	nz = cz / length;
	return true;
}

// The gate site of a placement whose first door is `doorKey`: the slab's bounding-box centre from the door shape of the
// placement's collision (the bake's own), nothing when the door has no collision.
void addGateSite(kworld::CollisionLibrary& library, const kdata::Record* door, const kworld::PlacedInstance& instance,
	std::uint64_t doorKey, std::vector<GateSite>& out)
{
	std::vector<kworld::WorldShape> shapes;
	kworld::CollisionCoverage coverage;
	library.addBuilding(instance, shapes, coverage);
	const kworld::WorldShape* slab = nullptr;
	for (const kworld::WorldShape& shape : shapes)
	{
		if (shape.use == kworld::NavUse::Door && shape.door == doorKey && !shape.vertices.empty())
		{
			slab = &shape;
			break;
		}
	}
	GateSite site;
	if (slab == nullptr || !gateNormal(instance, door, site.nx, site.nz))
		return;
	site.key = doorKey;
	site.building = kworld::doorKey(kworld::placementId(instance));
	site.buildingSid = instance.buildingID;
	site.ownerFaction = instance.ownerFaction;
	site.squadTemplate = instance.squadTemplate;
	site.townHand = instance.townHand;
	site.x = 0.5 * (static_cast<double>(slab->min[0]) + static_cast<double>(slab->max[0]));
	site.z = 0.5 * (static_cast<double>(slab->min[2]) + static_cast<double>(slab->max[2]));
	constexpr double kSide = 15.0; // FUN_14029cc90: centre -/+ 15 n
	site.outX = site.x + kSide * site.nx;
	site.outZ = site.z + kSide * site.nz;
	site.inX = site.x - kSide * site.nx;
	site.inZ = site.z - kSide * site.nz;
	out.push_back(std::move(site));
}

// A placed furniture building that is a cage itself (a Peeler Machine: no doors, no collision shapes): a one-point site at its
// placement, so the cage finders see it like any layout cage (the entry is made by the town scorer's index).
void addFurnitureCageSite(const kworld::PlacedInstance& instance, std::vector<BuildingSite>& out)
{
	BuildingSite site;
	site.key = kworld::doorKey(kworld::placementId(instance));
	site.buildingSid = instance.buildingID;
	site.ownerFaction = instance.ownerFaction;
	site.townHand = instance.townHand;
	site.label = instance.label;
	site.furnitureCage = true;
	site.originX = site.cx = instance.x;
	site.originZ = site.cz = instance.z;
	site.originY = site.minY = site.maxY = instance.y;
	site.qw = instance.qw;
	site.qx = instance.qx;
	site.qy = instance.qy;
	site.qz = instance.qz;
	const double w = instance.qw, x = instance.qx, y = instance.qy, z = instance.qz;
	const double ax = 1 - 2 * (y * y + z * z);
	const double az = 2 * (x * z - w * y);
	const double length = std::hypot(ax, az);
	site.axisX = length < 1.0e-6 ? 1.0 : ax / length;
	site.axisZ = length < 1.0e-6 ? 0.0 : az / length;
	out.push_back(std::move(site));
}

// The footprint of a building placement with doors: the oriented box (the placement's local x and z columns on the plane) that
// bounds the xz extent of all its collision, doors excluded, and its height range. Nothing when it has no collision.
// UNSURE: the box is the whole building, walls, porch and roof overhang included, not the interior mask hull (the mask's own
// collision is not read here); a character standing in an L-shaped building's missing corner counts as inside.
void addBuildingSite(kworld::CollisionLibrary& library, const kworld::PlacedInstance& instance, std::vector<BuildingSite>& out)
{
	std::vector<kworld::WorldShape> shapes;
	kworld::CollisionCoverage coverage;
	const std::optional<kworld::BuildingNav> nav = library.addBuilding(instance, shapes, coverage);
	const double w = instance.qw, x = instance.qx, y = instance.qy, z = instance.qz;
	double ax = 1 - 2 * (y * y + z * z);
	double az = 2 * (x * z - w * y);
	const double length = std::hypot(ax, az);
	if (length < 1.0e-6)
		return;
	ax /= length;
	az /= length;
	// The oriented box about the placement's axis that bounds the vertices of `list` (doors excluded); false when it has none.
	struct Bounds
	{
		double minA = 1e30, maxA = -1e30, minB = 1e30, maxB = -1e30, minY = 1e30, maxY = -1e30;
	};
	const auto boundsOf = [&](const std::vector<kworld::WorldShape>& list)
	{
		Bounds bounds;
		for (const kworld::WorldShape& shape : list)
		{
			if (shape.use == kworld::NavUse::Door)
				continue;
			for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
			{
				const double dx = shape.vertices[i] - instance.x;
				const double dz = shape.vertices[i + 2] - instance.z;
				const double a = dx * ax + dz * az;
				const double b = -dx * az + dz * ax;
				bounds.minA = std::min(bounds.minA, a);
				bounds.maxA = std::max(bounds.maxA, a);
				bounds.minB = std::min(bounds.minB, b);
				bounds.maxB = std::max(bounds.maxB, b);
				bounds.minY = std::min(bounds.minY, static_cast<double>(shape.vertices[i + 1]));
				bounds.maxY = std::max(bounds.maxY, static_cast<double>(shape.vertices[i + 1]));
			}
		}
		return bounds;
	};
	const Bounds all = boundsOf(shapes);
	if (all.minA > all.maxA)
		return;
	BuildingSite site;
	site.key = kworld::doorKey(kworld::placementId(instance));
	site.buildingSid = instance.buildingID;
	site.squadTemplate = instance.squadTemplate;
	site.ownerFaction = instance.ownerFaction;
	site.townHand = instance.townHand;
	site.interiorLayout = instance.interiorLayout;
	site.label = instance.label;
	site.originX = instance.x;
	site.originZ = instance.z;
	site.originY = instance.y;
	site.qw = instance.qw;
	site.qx = instance.qx;
	site.qy = instance.qy;
	site.qz = instance.qz;
	site.cx = instance.x + ax * (all.minA + all.maxA) * 0.5 - az * (all.minB + all.maxB) * 0.5;
	site.cz = instance.z + az * (all.minA + all.maxA) * 0.5 + ax * (all.minB + all.maxB) * 0.5;
	site.axisX = ax;
	site.axisZ = az;
	site.halfA = (all.maxA - all.minA) * 0.5;
	site.halfB = (all.maxB - all.minB) * 0.5;
	site.minY = all.minY;
	site.maxY = all.maxY;
	site.groundY = instance.y;
	site.cavernous = nav.has_value() && nav->cavernous;
	site.enforceCeiling = nav.has_value() && nav->enforceCeiling;
	site.shelter = nav.has_value() && nav->shelter;
	// The upper floors (CHAR_DYNAMIC `floor`): the tops of the walkable shapes the bake clipped to the interior mask (the
	// upper floors and their stairs, kworld::WorldShape::keepInside), clustered by height so a storey's slabs and the top of
	// its stairs make one floor. A step between them of less than kFloorGap is the same floor (the Bar's roof slabs differ
	// by 5 units); a storey is ~70 (the Hub's ground 1576, floor 1 1646).
	{
		constexpr double kFloorGap = 30.0;
		std::vector<double> tops;
		for (const kworld::WorldShape& shape : shapes)
		{
			if (shape.use != kworld::NavUse::Walkable || !shape.keepInside || shape.vertices.size() < 3)
				continue;
			double top = -1.0e30;
			for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
				top = std::max(top, static_cast<double>(shape.vertices[i + 1]));
			tops.push_back(top);
		}
		std::sort(tops.begin(), tops.end());
		for (const double top : tops)
		{
			if (!site.floors.empty() && top - site.floors.back() < kFloorGap)
				site.floors.back() = std::max(site.floors.back(), top);
			else
				site.floors.push_back(top);
		}
		// A building whose interior mask has no box (Outpost s-IV's) clips nothing, so no shape is keepInside and the table above is
		// empty: every character read floor 0 (kenshi-k16.6.277). Then the floors are read from the upper parts' own up-facing
		// faces: the heights where they hold kMinFloorArea of horizontal surface (not slopes: normal.y >= 0.8), chained into
		// clusters closer than kFloorGap (a flight's treads join the slab it ends on), a cluster's surface being its heaviest
		// 1-unit bin. Box shapes (walls, blocks) are skipped and a room-sized area is needed. UNSURE: the exe's own rule is unknown
		// (it keeps no height table that we found); this is the height-by-slab rule.
		if (site.floors.empty())
		{
			constexpr double kMinFloorArea = 2000.0;
			std::map<long, double> area;
			for (const kworld::WorldShape& shape : shapes)
			{
				if (shape.use != kworld::NavUse::Walkable || !shape.upperPart || shape.half != std::array<float, 3>{})
					continue; // (a box is a wall or a block, not a floor)
				for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
				{
					const float* a = &shape.vertices[shape.indices[t] * 3];
					const float* b = &shape.vertices[shape.indices[t + 1] * 3];
					const float* c = &shape.vertices[shape.indices[t + 2] * 3];
					const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
					const double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
					const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
					const double twice = std::sqrt(nx * nx + ny * ny + nz * nz);
					if (twice > 1.0e-9 && std::fabs(ny) / twice >= 0.8)
						area[std::lround((a[1] + b[1] + c[1]) / 3.0)] += 0.5 * twice;
				}
			}
			std::vector<std::pair<double, double>> clusters; // surface height, area (ascending height)
			double clusterLow = 0.0, clusterBest = 0.0, clusterWeight = 0.0, clusterArea = 0.0, lastBin = -1.0e30;
			std::vector<double> lows;
			const auto close = [&]
			{
				if (clusterArea >= kMinFloorArea)
				{
					clusters.emplace_back(clusterBest, clusterArea);
					lows.push_back(clusterLow);
				}
				clusterArea = clusterWeight = 0.0;
			};
			for (const auto& [bin, binArea] : area)
			{
				if (clusterArea > 0.0 && bin - lastBin >= kFloorGap)
					close();
				if (clusterArea == 0.0)
					clusterLow = static_cast<double>(bin);
				clusterArea += binArea;
				if (binArea > clusterWeight)
				{
					clusterWeight = binArea;
					clusterBest = static_cast<double>(bin);
				}
				lastBin = static_cast<double>(bin);
			}
			close();
			for (const auto& cluster : clusters)
				site.floors.push_back(cluster.first);
			// The foot of the first flight, when it is well over the placement's own y.
			if (!lows.empty() && lows.front() - instance.y > kFloorGap)
				site.groundY = lows.front();
		}
	}
	// The indoors trigger hull, when the building has an interior mask.
	std::vector<kworld::WorldShape> mask;
	if (library.addInteriorMask(instance, mask))
	{
		const Bounds hull = boundsOf(mask);
		if (hull.minA <= hull.maxA)
		{
			site.hull = true;
			site.hullCx = instance.x + ax * (hull.minA + hull.maxA) * 0.5 - az * (hull.minB + hull.maxB) * 0.5;
			site.hullCz = instance.z + az * (hull.minA + hull.maxA) * 0.5 + ax * (hull.minB + hull.maxB) * 0.5;
			site.hullHalfA = (hull.maxA - hull.minA) * 0.5;
			site.hullHalfB = (hull.maxB - hull.minB) * 0.5;
			site.hullMinY = hull.minY;
			site.hullMaxY = hull.maxY;
		}
	}
	out.push_back(site);
}

Faction readFaction(const kdata::Record& record)
{
	Faction value;
	value.defaultRelation = record.intOr("default relation", value.defaultRelation);
	value.enemyClassification = record.intOr("enemy classification",
		value.enemyClassification);
	value.fundamentalType = record.intOr("fundamental type", value.fundamentalType);
	value.squadFormation = record.intOr("squad formation", value.squadFormation);
	value.roamingPopulation = record.intOr("roaming population",
		value.roamingPopulation);
	value.maxProsperity = record.intOr("max prosperity", value.maxProsperity);
	value.businessRelations = record.intOr("business relations",
		value.businessRelations);
	value.numRanks = record.intOr("num ranks", value.numRanks);
	value.numNegativeRanks = record.intOr("num negative ranks",
		value.numNegativeRanks);
	value.cagesLockLevel = record.intOr("cages lock level", value.cagesLockLevel);
	value.containersLockLevel = record.intOr("containers lock level",
		value.containersLockLevel);
	value.doorsLockLevel = record.intOr("doors lock level", value.doorsLockLevel);
	value.lockLevelRandom = record.intOr("lock level random", value.lockLevelRandom);
	value.roadPreference = record.floatOr("road preference", value.roadPreference);
	value.trustworthy = record.floatOr("trustworthy", value.trustworthy);
	value.effectOfAnger = record.floatOr("effect of anger", value.effectOfAnger);
	value.effectOfHappy = record.floatOr("effect of happy", value.effectOfHappy);
	value.emotionFadeRate = record.floatOr("emotion fade rate",
		value.emotionFadeRate);
	value.runAwayRatioOfSquadSize = record.floatOr("run away ratio of squad size",
		value.runAwayRatioOfSquadSize);
	value.runAwayRatioRelativeToEnemy = record.floatOr(
		"run away ratio relative to enemy", value.runAwayRatioRelativeToEnemy);
	value.antiSlavery = record.boolOr("anti slavery", value.antiSlavery);
	value.notReal = record.boolOr("not real", value.notReal);
	value.offersBounties = record.boolOr("offers bounties", value.offersBounties);
	value.healsStrangers = record.boolOr("heals strangers", value.healsStrangers);
	value.allowSlavesWeapons = record.boolOr("allow slaves weapons",
		value.allowSlavesWeapons);
	return value;
}

Town readTown(const kdata::Record& record)
{
	Town value;
	value.type = record.intOr("type", value.type);
	value.numCentrepoints = record.intOr("num centrepoints", value.numCentrepoints);
	value.nestResidentPopulation = record.intOr("nest resident population",
		value.nestResidentPopulation);
	value.gearArtifactsMinValue = record.intOr("gear artifacts min value",
		value.gearArtifactsMinValue);
	value.gearArtifactsMaxValue = record.intOr("gear artifacts max value",
		value.gearArtifactsMaxValue);
	value.itemArtifactsMinValue = record.intOr("item artifacts min value",
		value.itemArtifactsMinValue);
	value.itemArtifactsMaxValue = record.intOr("item artifacts max value",
		value.itemArtifactsMaxValue);
	value.sizeRadius = record.floatOr("size radius", value.sizeRadius);
	value.townRadiusMult = record.floatOr("town radius mult", value.townRadiusMult);
	value.noFoliageRange = record.floatOr("no-foliage range", value.noFoliageRange);
	value.isPublic = record.boolOr("is public", value.isPublic);
	value.isSecret = record.boolOr("is secret", value.isSecret);
	value.residentsOverride = record.boolOr("residents override",
		value.residentsOverride);
	value.spawnInTownCentre = record.boolOr("spawn in town centre",
		value.spawnInTownCentre);
	value.distantMesh = record.boolOr("distant mesh", value.distantMesh);
	value.unexploredName = record.stringOr("unexplored name");
	return value;
}

SquadTemplate readSquadTemplate(const kdata::Record& record)
{
	SquadTemplate value;
	value.numRandomChars = record.intOr("num random chars", value.numRandomChars);
	value.numRandomCharsMax = record.intOr("num random chars max",
		value.numRandomCharsMax);
	value.forceSpeed = record.intOr("force speed", value.forceSpeed);
	value.buildingDesignation = record.intOr("building designation",
		value.buildingDesignation);
	value.vendorMoney = record.intOr("vendor money", value.vendorMoney);
	value.vendorsFillTotalAmount = record.intOr("vendors fill total amount",
		value.vendorsFillTotalAmount);
	value.bedUsageCost = record.intOr("bed usage cost", value.bedUsageCost);
	value.crossbowLevels = record.intOr("crossbow levels", value.crossbowLevels);
	value.roboticsLevels = record.intOr("robotics levels", value.roboticsLevels);
	value.vendorsRefreshTime = record.floatOr("vendors refresh time",
		value.vendorsRefreshTime);
	value.animalAgeRandom = record.floatOr("animal age random",
		value.animalAgeRandom);
	value.bloodSmellMult = record.floatOr("blood smell mult",
		value.bloodSmellMult);
	value.buyMult = record.floatOr("buy mult", value.buyMult);
	value.isTrader = record.boolOr("is trader", value.isTrader);
	value.regenerates = record.boolOr("regenerates", value.regenerates);
	value.roamingMilitary = record.boolOr("roaming military", value.roamingMilitary);
	value.patrolApproachesTowns = record.boolOr("patrol approaches towns",
		value.patrolApproachesTowns);
	value.dontMultiply = record.boolOr("dont multiply", value.dontMultiply);
	value.buildingRuined = record.boolOr("building ruined", value.buildingRuined);
	value.publicDay = record.boolOr("public day", value.publicDay);
	value.publicNight = record.boolOr("public night", value.publicNight);
	value.publicBeds = record.boolOr("public beds", value.publicBeds);
	value.sellHome = record.boolOr("sell home", value.sellHome);
	value.malnourished = record.boolOr("malnourished", value.malnourished);
	value.buildingName = record.stringOr("building name");
	value.layoutInterior = record.stringOr("layout interior");
	value.layoutExterior = record.stringOr("layout exterior");
	return value;
}

Character readCharacter(const kdata::Record& record, bool animal)
{
	Character value;
	value.animal = animal;
	value.unique = record.boolOr("unique", value.unique);
	value.named = record.boolOr("named", value.named);
	value.isTrader = record.boolOr("is trader", value.isTrader);
	value.wearsUniform = record.boolOr("wears uniform", value.wearsUniform);
	value.dumb = record.boolOr("dumb", value.dumb);
	value.assignsBounties = record.boolOr("assigns bounties", value.assignsBounties);
	value.shaved = record.boolOr("shaved", value.shaved);
	value.npcClass = record.intOr("NPC class", value.npcClass);
	value.armourGrade = record.intOr("armour grade", value.armourGrade);
	value.armourUpgradeChance = record.intOr("armour upgrade chance",
		value.armourUpgradeChance);
	value.femaleChance = record.intOr("female chance", value.femaleChance);
	value.slave = record.intOr("slave", value.slave);
	value.moneyMin = record.intOr("money min", value.moneyMin);
	value.moneyMax = record.intOr("money max", value.moneyMax);
	value.moneyItemMin = record.intOr("money item min", value.moneyItemMin);
	value.moneyItemMax = record.intOr("money item max", value.moneyItemMax);
	value.moneyItemProb = record.floatOr("money item prob", value.moneyItemProb);
	value.wages = record.intOr("wages", value.wages);
	value.bountyAmount = record.intOr("bounty amount", value.bountyAmount);
	value.bountyAmountFuzz = record.intOr("bounty amount fuzz",
		value.bountyAmountFuzz);
	value.bountyChance = record.intOr("bounty chance", value.bountyChance);
	value.factionImportance = record.floatOr("faction importance",
		value.factionImportance);
	value.combatStats = record.intOr("combat stats", value.combatStats);
	value.rangedStats = record.intOr("ranged stats", value.rangedStats);
	value.stealthStats = record.intOr("stealth stats", value.stealthStats);
	value.strength = record.intOr("strength", value.strength);
	value.unarmedStats = record.intOr("unarmed stats", value.unarmedStats);
	value.statsRandomise = record.intOr("stats randomise", value.statsRandomise);
	value.attackType = record.intOr("attack type", value.attackType);
	value.animalStrength = record.intOr("animal strength", value.animalStrength);
	value.inventoryWidth = record.intOr("inventory w", value.inventoryWidth);
	value.inventoryHeight = record.intOr("inventory h", value.inventoryHeight);
	value.lifespan = record.floatOr("lifespan", value.lifespan);
	value.scaleMin = record.floatOr("scale min", value.scaleMin);
	value.scaleMax = record.floatOr("scale max", value.scaleMax);
	value.hpMult = record.floatOr("HP mult", value.hpMult);
	value.turnRate = record.floatOr("turn rate", value.turnRate);
	value.smellBlood = record.floatOr("smell blood", value.smellBlood);
	value.smellEggs = record.floatOr("smell eggs", value.smellEggs);
	// The shopping list (task CH group 3, dt24_shop section 5): (item, want v0, chance v1, priority v2) refs. A dangling target is
	// kept: the item-function lookup drops it when the list is built.
	if (const kdata::RefCategory* refs = record.findRefs("shopping"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (entry.opaqueEntry || entry.targetID.empty())
				continue;
			value.shopping.push_back({entry.targetID, entry.v0, entry.v1, entry.v2});
		}
	}
	return value;
}

Race readRace(const kdata::Record& record)
{
	Race value;
	value.walkSpeed = record.floatOr("walk speed", value.walkSpeed);
	value.hungerRate = record.floatOr("hunger rate", value.hungerRate);
	value.healRate = record.floatOr("heal rate", value.healRate);
	value.bleedRate = record.floatOr("bleed rate", value.bleedRate);
	value.minBlood = record.floatOr("min blood", value.minBlood);
	value.maxBlood = record.floatOr("max blood", value.maxBlood);
	value.speedMinSkill = record.floatOr("speed min skill", value.speedMinSkill);
	value.speedMaxSkill = record.floatOr("speed max skill", value.speedMaxSkill);
	value.pathfindFootprintRadius = record.floatOr("pathfind footprint radius",
		value.pathfindFootprintRadius);
	value.pathfindAcceleration = record.floatOr("pathfind acceleration",
		value.pathfindAcceleration);
	value.visionRangeMult = record.floatOr("vision range mult",
		value.visionRangeMult);
	value.strength = record.floatOr("strength", value.strength);
	value.dexterity = record.floatOr("dexterity", value.dexterity);
	value.swimSpeedMult = record.floatOr("swim speed mult", value.swimSpeedMult);
	value.waterAvoidance = record.floatOr("water avoidance", value.waterAvoidance);
	value.extraAttackSlots = record.intOr("extra attack slots",
		value.extraAttackSlots);
	value.bloodColour = record.intOr("blood colour", value.bloodColour);
	value.healStat = record.intOr("heal stat", value.healStat);
	value.isRobot = record.boolOr("is robot", value.isRobot);
	value.playable = record.boolOr("playable", value.playable);
	value.cantEnterBuildings = record.boolOr("cant enter buildings",
		value.cantEnterBuildings);
	value.gigantic = record.boolOr("gigantic", value.gigantic);
	value.selfHealing = record.boolOr("self healing", value.selfHealing);
	value.vampiric = record.boolOr("vampiric", value.vampiric);
	value.swims = record.boolOr("swims", value.swims);
	value.carriable = record.boolOr("carriable", value.carriable);
	value.singleGender = record.boolOr("single gender", value.singleGender);
	value.noHats = record.boolOr("no hats", value.noHats);
	value.noShirts = record.boolOr("no shirts", value.noShirts);
	value.noShoes = record.boolOr("no shoes", value.noShoes);
	return value;
}

Building readBuilding(const kdata::Record& record)
{
	Building value;
	value.category = record.stringOr("building category", value.category);
	value.group = record.stringOr("building group");
	value.scale = record.floatOr("scale", value.scale);
	value.buildSpeedMult = record.floatOr("build speed mult",
		value.buildSpeedMult);
	value.buildThreshold = record.floatOr("build threshold", value.buildThreshold);
	value.outputRate = record.floatOr("output rate", value.outputRate);
	value.hardness = record.floatOr("hardness", value.hardness);
	value.buildMaterials = record.intOr("build materials", value.buildMaterials);
	value.pathMode = record.intOr("path mode", value.pathMode);
	value.storageSizeWidth = record.intOr("storage size width",
		value.storageSizeWidth);
	value.storageSizeHeight = record.intOr("storage size height",
		value.storageSizeHeight);
	value.maxOperators = record.intOr("max operators", value.maxOperators);
	value.powerOutput = record.intOr("power output", value.powerOutput);
	value.powerCapacity = record.intOr("power capacity", value.powerCapacity);
	value.desirability = record.intOr("desirability", value.desirability);
	value.nodeType = record.intOr("node type", value.nodeType);
	value.itemtypeLimit = record.intOr("itemtype limit", value.itemtypeLimit);
	value.exteriorMaterial = record.intOr("exterior material",
		value.exteriorMaterial);
	value.interiorMaterial = record.intOr("interior material",
		value.interiorMaterial);
	value.hasInventory = record.boolOr("has inventory", value.hasInventory);
	value.hasLock = record.boolOr("has lock", value.hasLock);
	value.isWallGate = record.boolOr("is wall gate", value.isWallGate);
	value.isFeature = record.boolOr("is feature", value.isFeature);
	value.isGateway = record.boolOr("is gateway", value.isGateway);
	value.isShelter = record.boolOr("is shelter", value.isShelter);
	value.alwaysLocks = record.boolOr("always locks", value.alwaysLocks);
	return value;
}

AiPackage readAiPackage(const kdata::Record& record)
{
	AiPackage value;
	value.signalFunc = record.intOr("signal func", value.signalFunc);
	value.unloadedFunc = record.intOr("unloaded func", value.unloadedFunc);
	value.startTime = record.intOr("start time", value.startTime);
	value.endTime = record.intOr("end time", value.endTime);
	value.clearsExistingJobs = record.boolOr("clears existing jobs",
		value.clearsExistingJobs);
	return value;
}

template <typename Component>
flecs::entity addPrefab(flecs::world& ecs,
	std::unordered_map<std::string, flecs::entity>& prefabs,
	const kdata::Record& record, const Component& component)
{
	flecs::entity entity = ecs.prefab();
	entity.set<Source>(
		Source{record.stringID, record.name, kdata::itemTypeName(record.type)});
	entity.set<Component>(component);
	prefabs.emplace(record.stringID, entity);
	return entity;
}

// Iterates one reference category of a record; entries with an opaque pre-v8
// payload have no target string and are skipped (never present in v16/v17).
template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	for (const auto& refs : record.references)
	{
		if (refs.category != category)
			continue;
		for (const auto& entry : refs.entries)
		{
			if (entry.opaqueEntry)
				continue;
			func(entry);
		}
	}
}

template <typename Relation>
void addListPair(flecs::entity source, flecs::entity target,
	const kdata::RefEntry& entry)
{
	Relation value;
	value.v0 = entry.v0;
	value.v1 = entry.v1;
	value.v2 = entry.v2;
	source.set<Relation>(target, value);
}

} // namespace

World World::load(std::string_view dataDir)
{
	const auto start = std::chrono::steady_clock::now();
	const kworld::LoadOrder order = kworld::vanillaLoadOrder(dataDir);
	std::vector<std::string> names;
	for (const kworld::LoadOrderEntry& entry : order.files)
		names.push_back(entry.name);
	World world = build(kworld::loadMergedGameData(order), dataDir, std::move(names));
	world.loadSeconds_ = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - start).count();
	return world;
}

World World::build(const kdata::GameData& gameData, std::string_view dataDir,
	std::vector<std::string> loadOrder)
{
	const auto start = std::chrono::steady_clock::now();
	World world;
	world.dataDir_ = std::string(dataDir);
	world.loadOrder_ = std::move(loadOrder);

	world.buildPrefabs(gameData);
	world.buildRelationships(gameData);
	world.placeBuildings(gameData, dataDir);
	world.finishCounts();

	const auto end = std::chrono::steady_clock::now();
	world.loadSeconds_ = std::chrono::duration<double>(end - start).count();
	return world;
}

World World::empty()
{
	return World();
}

void World::buildPrefabs(const kdata::GameData& gameData)
{
	for (const kdata::Record* record : gameData.byType(kFactionType))
	{
		addPrefab(ecs_, prefabs_, *record, readFaction(*record));
		++counts_.factions;
	}
	for (const kdata::Record* record : gameData.byType(kTownType))
	{
		addPrefab(ecs_, prefabs_, *record, readTown(*record));
		++counts_.towns;
	}
	for (const kdata::Record* record : gameData.byType(kSquadTemplateType))
	{
		addPrefab(ecs_, prefabs_, *record, readSquadTemplate(*record));
		++counts_.squadTemplates;
	}
	for (const kdata::Record* record : gameData.byType(kCharacterType))
	{
		addPrefab(ecs_, prefabs_, *record, readCharacter(*record, false));
		++counts_.characters;
	}
	for (const kdata::Record* record : gameData.byType(kHumanCharacterType))
	{
		addPrefab(ecs_, prefabs_, *record, readCharacter(*record, false));
		++counts_.humanCharacters;
	}
	for (const kdata::Record* record : gameData.byType(kAnimalCharacterType))
	{
		addPrefab(ecs_, prefabs_, *record, readCharacter(*record, true));
		++counts_.animals;
	}
	for (const kdata::Record* record : gameData.byType(kBuildingType))
	{
		addPrefab(ecs_, prefabs_, *record, readBuilding(*record));
		++counts_.buildings;
	}
	for (const kdata::Record* record : gameData.byType(kRaceType))
	{
		addPrefab(ecs_, prefabs_, *record, readRace(*record));
		++counts_.races;
	}
	for (const kdata::Record* record : gameData.byType(kAiPackageType))
	{
		addPrefab(ecs_, prefabs_, *record, readAiPackage(*record));
		++counts_.aiPackages;
	}
	counts_.prefabs = prefabs_.size();
}

void World::buildRelationships(const kdata::GameData& gameData)
{
	const auto findTarget = [this](std::string_view stringID) -> flecs::entity
	{
		const auto found = prefabs_.find(std::string(stringID));
		if (found == prefabs_.end())
			return flecs::entity{};
		return found->second;
	};
	const auto sourceOf = [this](const kdata::Record& record) -> flecs::entity
	{
		const auto found = prefabs_.find(record.stringID);
		if (found == prefabs_.end())
			return flecs::entity{};
		return found->second;
	};

	// FACTION: starting diplomacy and the squad templates it seeds.
	for (const kdata::Record* record : gameData.byType(kFactionType))
	{
		const flecs::entity source = sourceOf(*record);
		if (!source.is_valid())
			continue;
		forEachRef(*record, "relations", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (!target.is_valid())
				return;
			source.set<FactionRelation>(target, FactionRelation{entry.v0});
			factionRelations_.push_back({source.id(), target.id()});
		});
		forEachRef(*record, "residents", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionResidents>(source, target, entry);
		});
		forEachRef(*record, "default resident", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionDefaultResident>(source, target, entry);
		});
		forEachRef(*record, "bar squads", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionBarSquads>(source, target, entry);
		});
		forEachRef(*record, "squads", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionSquads>(source, target, entry);
		});
		forEachRef(*record, "squad default", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionSquadDefault>(source, target, entry);
		});
		forEachRef(*record, "roaming squads", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<FactionRoamingSquads>(source, target, entry);
		});
	}

	// TOWN: owner, population sources and town-version overrides.
	for (const kdata::Record* record : gameData.byType(kTownType))
	{
		const flecs::entity source = sourceOf(*record);
		if (!source.is_valid())
			continue;
		forEachRef(*record, "faction", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.add<OwnedBy>(target);
		});
		forEachRef(*record, "residents", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<TownResidents>(source, target, entry);
		});
		forEachRef(*record, "bar squads", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<TownBarSquads>(source, target, entry);
		});
		forEachRef(*record, "roaming squads", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<TownRoamingSquads>(source, target, entry);
		});
		forEachRef(*record, "default resident", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<TownDefaultResident>(source, target, entry);
		});
		forEachRef(*record, "override town", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.add<OverridesTown>(target);
		});
	}

	// SQUAD_TEMPLATE: faction membership, members and AI packages.
	for (const kdata::Record* record : gameData.byType(kSquadTemplateType))
	{
		const flecs::entity source = sourceOf(*record);
		if (!source.is_valid())
			continue;
		forEachRef(*record, "faction", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.add<MemberOf>(target);
		});
		forEachRef(*record, "squad", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadMember>(source, target, entry);
		});
		forEachRef(*record, "squad2", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadMember2>(source, target, entry);
		});
		forEachRef(*record, "leader", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadLeader>(source, target, entry);
		});
		forEachRef(*record, "choosefrom list", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadChooseFrom>(source, target, entry);
		});
		forEachRef(*record, "animals", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadAnimal>(source, target, entry);
		});
		forEachRef(*record, "animals2", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<SquadAnimal2>(source, target, entry);
		});
		std::int32_t packageOrder = 0;
		forEachRef(*record, "AI packages", [&](const kdata::RefEntry& entry)
		{
			const std::int32_t order = packageOrder++;
			const flecs::entity target = findTarget(entry.targetID);
			if (!target.is_valid())
				return;
			AiPackageLink value;
			value.order = order;
			value.priority = entry.v0;
			value.v1 = entry.v1;
			value.v2 = entry.v2;
			source.set<AiPackageLink>(target, value);
		});
	}

	// CHARACTER / ANIMAL_CHARACTER / HUMAN_CHARACTER: faction and race.
	const auto addCharacterRelations = [&](const kdata::Record& record)
	{
		const flecs::entity source = sourceOf(record);
		if (!source.is_valid())
			return;
		forEachRef(record, "faction", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.add<MemberOf>(target);
		});
		forEachRef(record, "race", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				addListPair<RaceOption>(source, target, entry);
		});
	};
	for (const kdata::Record* record : gameData.byType(kCharacterType))
		addCharacterRelations(*record);
	for (const kdata::Record* record : gameData.byType(kHumanCharacterType))
		addCharacterRelations(*record);
	for (const kdata::Record* record : gameData.byType(kAnimalCharacterType))
		addCharacterRelations(*record);

	// AI_PACKAGE chains (GAMEDATA_SCHEMA.md "AI_PACKAGE").
	for (const kdata::Record* record : gameData.byType(kAiPackageType))
	{
		const flecs::entity source = sourceOf(*record);
		if (!source.is_valid())
			continue;
		forEachRef(*record, "inherits from", [&](const kdata::RefEntry& entry)
		{
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.add<AiInherits>(target);
		});
		std::int32_t leadsOrder = 0;
		forEachRef(*record, "Leads to", [&](const kdata::RefEntry& entry)
		{
			const std::int32_t order = leadsOrder++;
			const flecs::entity target = findTarget(entry.targetID);
			if (target.is_valid())
				source.set<AiLeadsTo>(target, AiLeadsTo{order});
		});
	}
}

void World::placeBuildings(const kdata::GameData& gameData, std::string_view dataDir)
{
	const std::string root(dataDir);
	const kworld::Heightmap heightmap = kworld::Heightmap::open(
		root + "/newland/land/fullmap.tif");
	const kworld::Zones zones = kworld::Zones::load(root);
	placements_ = zones.placeMerged(heightmap, gameData);
	kworld::CollisionLibrary library(dataDir, gameData);
	// The deposit records: MINING foliage whose functionality mines iron (1) or copper (3). Stone (2) is noise, not a placement.
	std::unordered_map<std::string, std::uint8_t> depositResource;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("BUILDING")))
	{
		const kdata::RefCategory* functionality = record->findRefs("functionality");
		if (!record->boolOr("is foliage", false) || record->stringOr("building category") != "MINING"
			|| functionality == nullptr || functionality->entries.empty())
			continue;
		const kdata::Record* function = gameData.find(functionality->entries.front().targetID);
		const std::int32_t mined = function != nullptr ? function->intOr("world resource mining", 0) : 0;
		if (mined == 1 || mined == 3)
			depositResource.emplace(record->stringID, static_cast<std::uint8_t>(mined));
	}

	std::unordered_set<std::string> producerBuildings;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("BUILDING")))
	{
		if (producers::producerFunctionality(gameData, *record) != nullptr)
			producerBuildings.insert(record->stringID);
	}

	// The economy's camps (TOWN_LIFE_SLICE 7.3): every placed production BUILDING of any class, natural deposits excepted (a
	// MINING foliage rock is a producer site, not a camp).
	std::unordered_set<std::string> productionBuildings;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("BUILDING")))
	{
		if (!record->boolOr("is foliage", false) && producers::productionFunctionality(gameData, *record) != nullptr)
			productionBuildings.insert(record->stringID);
	}

	// The disposal machines (137's pot, TOWN_LIFE_SLICE 5.32): every placement whose BUILDING's functionality function is 11. Most
	// are doorless open structures, so BuildingSite does not carry them.
	std::unordered_set<std::string> disposalBuildings;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("BUILDING")))
	{
		const kdata::RefCategory* functionality = record->findRefs("functionality");
		for (std::size_t i = 0; functionality != nullptr && i < functionality->entries.size(); ++i)
		{
			const kdata::Record* function = gameData.find(functionality->entries[i].targetID);
			if (function != nullptr && function->intOr("function", 0) == 11)
			{
				disposalBuildings.insert(record->stringID);
				break;
			}
		}
	}

	for (const kworld::PlacedInstance& instance : placements_)
	{
		if (disposalBuildings.count(instance.buildingID) != 0)
			disposalSites_.push_back(DisposalSite{kworld::doorKey(kworld::placementId(instance)), instance.buildingID,
				instance.ownerFaction, instance.squadTemplate, instance.x, instance.y, instance.z});
		if (producerBuildings.count(instance.buildingID) != 0)
			producerSites_.push_back(ProducerSite{producers::kIdBase + producerSites_.size(), instance.buildingID, instance.ownerFaction, instance.x, instance.y, instance.z});
		if (productionBuildings.count(instance.buildingID) != 0)
			productionSites_.push_back(ProductionSite{instance.buildingID, instance.ownerFaction, instance.x, instance.y, instance.z});
		if (const auto deposit = depositResource.find(instance.buildingID); deposit != depositResource.end())
			depositSites_.push_back(DepositSite{deposit->second, instance.x, instance.z});
		flecs::entity entity = ecs_.entity();
		entity.set<Position>(Position{instance.x, instance.y, instance.z});
		entity.set<Rotation>(Rotation{instance.qw, instance.qx, instance.qy,
			instance.qz});
		entity.set<Scale>(Scale{instance.scale});
		entity.set<PlacementCell>(PlacementCell{instance.cell.x, instance.cell.z});
		entity.set<PlacedBuilding>(PlacedBuilding{instance.instanceID,
			instance.buildingID, instance.layer});
		// Everything starts at the coarsest level; a player vicinity (or a
		// command) promotes entities later.
		entity.set<Detail>(Detail{DetailLevel::Strategic});

		const auto prefab = prefabs_.find(instance.buildingID);
		if (prefab != prefabs_.end() && prefab->second.get<Building>() != nullptr)
		{
			entity.is_a(prefab->second);
			++counts_.placedWithPrefab;
			// Same numbering as the navmesh bake: removed references do not count, a missing
			// child still takes its index.
			const Building& parent = *prefab->second.get<Building>();
			if (parent.hasInventory)
			{
				const kdata::Record* storage = gameData.find(instance.buildingID);
				containerSites_.push_back(ContainerSite{kworld::doorKey(kworld::doorId(instance, 0xC0DE)),
					storage != nullptr ? storage->stringID : std::string(), instance.x, instance.y, instance.z, instance.ownerFaction,
					parent.hasLock});
			}
			const kdata::Record* record = gameData.find(instance.buildingID);
			const kdata::RefCategory* doors = record != nullptr ? record->findRefs("doors") : nullptr;
			// A ruin has its doorways but no doors (kworld::placementRuined): no door or gate sites; still a building.
			const bool ruined = kworld::placementRuined(gameData, instance);
			std::size_t index = 0;
			for (std::size_t i = 0; doors != nullptr && i < doors->entries.size(); ++i)
			{
				if (doors->entries[i].v0 == kdata::kReferenceRemoved)
					continue;
				const kdata::Record* child = gameData.find(doors->entries[i].targetID);
				if (ruined)
				{
					++index;
					continue;
				}
				if (index == 0 && parent.isWallGate)
					addGateSite(library, child, instance, kworld::doorKey(kworld::doorId(instance, 0)), gateSites_);
				doorSites_.push_back(DoorSite{
					kworld::doorKey(kworld::doorId(instance, index++)), instance.x, instance.z,
					parent.isGateway || parent.isWallGate,
					child != nullptr && child->boolOr("always locks", false),
					child != nullptr ? child->stringID : std::string(),
					child != nullptr ? static_cast<double>(child->floatOr("door move speed", 0.5f)) : 0.5,
					kworld::doorKey(kworld::placementId(instance)), instance.ownerFaction});
			}
			if (index == 0 && record != nullptr && routines::classifyPiece(gameData, *record) == routines::SiteKind::Cage)
				addFurnitureCageSite(instance, furnitureCages_);
			// A town gate / gateway is a passage, not a building anyone is inside of.
			if (index > 0 && !parent.isWallGate && !parent.isGateway)
			{
				const std::size_t before = buildingSites_.size();
				addBuildingSite(library, instance, buildingSites_);
				const kdata::RefCategory* interior = record->findRefs("interior");
				if (buildingSites_.size() > before && interior != nullptr)
					buildingSites_.back().hasInterior = std::any_of(interior->entries.begin(), interior->entries.end(),
						[](const kdata::RefEntry& entry) { return entry.v0 != kdata::kReferenceRemoved; });
				if (buildingSites_.size() > before)
					buildingSites_.back().ruined = ruined;
			}
		}
		else
		{
			entity.set<MissingBuildingPrefab>(
				MissingBuildingPrefab{instance.buildingID});
		}
		++counts_.placedBuildings;
	}
}

bool World::addFurnitureCage(const kdata::GameData& gameData, const kworld::PlacedInstance& instance)
{
	const kdata::Record* record = gameData.find(instance.buildingID);
	if (record == nullptr || routines::classifyPiece(gameData, *record) != routines::SiteKind::Cage)
		return false;
	placements_.push_back(instance);
	addFurnitureCageSite(instance, furnitureCages_);
	buildingSiteByKeyValid_ = false;
	return true;
}

void World::finishCounts()
{
	counts_.factionRelations = countPairs<FactionRelation>(ecs_, prefabs_);
	counts_.membership = countPairs<MemberOf>(ecs_, prefabs_);
	counts_.townOwnership = countPairs<OwnedBy>(ecs_, prefabs_);
	counts_.townResidents = countPairs<TownResidents>(ecs_, prefabs_);
	counts_.townBarSquads = countPairs<TownBarSquads>(ecs_, prefabs_);
	counts_.townRoamingSquads = countPairs<TownRoamingSquads>(ecs_, prefabs_);
	counts_.townDefaultResident = countPairs<TownDefaultResident>(ecs_, prefabs_);
	counts_.factionResidents = countPairs<FactionResidents>(ecs_, prefabs_);
	counts_.factionDefaultResident = countPairs<FactionDefaultResident>(ecs_,
		prefabs_);
	counts_.factionBarSquads = countPairs<FactionBarSquads>(ecs_, prefabs_);
	counts_.factionSquads = countPairs<FactionSquads>(ecs_, prefabs_);
	counts_.factionSquadDefault = countPairs<FactionSquadDefault>(ecs_, prefabs_);
	counts_.factionRoamingSquads = countPairs<FactionRoamingSquads>(ecs_, prefabs_);
	counts_.squadMembers = countPairs<SquadMember>(ecs_, prefabs_);
	counts_.squadMembers2 = countPairs<SquadMember2>(ecs_, prefabs_);
	counts_.squadLeaders = countPairs<SquadLeader>(ecs_, prefabs_);
	counts_.squadChooseFrom = countPairs<SquadChooseFrom>(ecs_, prefabs_);
	counts_.squadAnimals = countPairs<SquadAnimal>(ecs_, prefabs_);
	counts_.squadAnimals2 = countPairs<SquadAnimal2>(ecs_, prefabs_);
	counts_.raceOptions = countPairs<RaceOption>(ecs_, prefabs_);
	counts_.aiPackageLinks = countPairs<AiPackageLink>(ecs_, prefabs_);
	counts_.aiInherits = countPairs<AiInherits>(ecs_, prefabs_);
	counts_.aiLeadsTo = countPairs<AiLeadsTo>(ecs_, prefabs_);
	counts_.overridesTown = countPairs<OverridesTown>(ecs_, prefabs_);

	counts_.placedBuildings = static_cast<std::size_t>(
		ecs_.count<PlacedBuilding>());
}

flecs::entity World::find(std::string_view stringID) const
{
	const auto found = prefabs_.find(std::string(stringID));
	if (found == prefabs_.end())
		return flecs::entity{};
	return found->second;
}

const BuildingSite* World::buildingSite(std::uint64_t buildingKey) const
{
	if (!buildingSiteByKeyValid_)
	{
		buildingSiteByKey_.clear();
		for (std::size_t i = 0; i < buildingSites_.size(); ++i)
			buildingSiteByKey_.emplace(buildingSites_[i].key, static_cast<std::uint32_t>(i));
		buildingSiteByKeyValid_ = true;
	}
	const auto found = buildingSiteByKey_.find(buildingKey);
	if (found != buildingSiteByKey_.end())
		return &buildingSites_[found->second];
	const auto furniture = std::find_if(furnitureCages_.begin(), furnitureCages_.end(), [&](const BuildingSite& site) { return site.key == buildingKey; });
	return furniture == furnitureCages_.end() ? nullptr : &*furniture;
}

const DisposalSite* World::disposalSite(std::uint64_t key) const noexcept
{
	for (const DisposalSite& site : disposalSites_)
		if (site.key == key)
			return &site;
	return nullptr;
}

bool World::openToPublic(std::uint64_t buildingKey, bool untemplated) const
{
	const auto site = std::find_if(buildingSites_.begin(), buildingSites_.end(),
		[buildingKey](const BuildingSite& entry) { return entry.key == buildingKey; });
	if (site == buildingSites_.end())
		return false;
	if (site->squadTemplate.empty())
		return untemplated; // no template to say otherwise: `public day` defaults true (SkinHouse's tower, a HUD "open to public", names none)
	const flecs::entity squad = find(site->squadTemplate);
	const SquadTemplate* template_ = squad.is_valid() ? squad.get<SquadTemplate>() : nullptr;
	return template_ != nullptr && template_->publicDay;
}

std::optional<std::int32_t> World::storedRelation(flecs::entity_t from,
	flecs::entity_t to) const
{
	const flecs::entity faction(ecs_.c_ptr(), from);
	const auto* value = faction.get<FactionRelation>(to);
	if (value == nullptr)
		return std::nullopt;
	return value->value;
}

std::array<double, 3> layoutToWorld(const BuildingSite& building, double hand, double x, double y, double z) noexcept
{
	// v + 2 u x (u x v + w v), u = (qx, qy, qz), v = (x, y, hand z) (kworld::placeFurniture)
	const double w = building.qw, ux = building.qx, uy = building.qy, uz = building.qz;
	const double vx = x, vy = y, vz = hand * z;
	const double cx = uy * vz - uz * vy + w * vx;
	const double cy = uz * vx - ux * vz + w * vy;
	const double cz = ux * vy - uy * vx + w * vz;
	return {building.originX + vx + 2 * (uy * cz - uz * cy), building.originY + vy + 2 * (uz * cx - ux * cz),
		building.originZ + vz + 2 * (ux * cy - uy * cx)};
}

std::array<double, 2> layoutToWorld(const BuildingSite& building, double hand, double x, double z) noexcept
{
	const std::array<double, 3> at = layoutToWorld(building, hand, x, 0.0, z);
	return {at[0], at[2]};
}

bool insideFootprint(const BuildingSite& building, const std::array<double, 2>& at) noexcept
{
	const double dx = at[0] - building.cx;
	const double dz = at[1] - building.cz;
	return std::fabs(dx * building.axisX + dz * building.axisZ) <= building.halfA && std::fabs(-dx * building.axisZ + dz * building.axisX) <= building.halfB;
}

double layoutHand(const BuildingSite& building, const std::vector<std::array<double, 2>>& local) noexcept
{
	double bestHand = 1.0;
	std::size_t bestCount = 0;
	for (const double hand : {1.0, -1.0})
	{
		std::size_t count = 0;
		for (const std::array<double, 2>& piece : local)
			count += insideFootprint(building, layoutToWorld(building, hand, piece[0], piece[1])) ? 1 : 0;
		if (count > bestCount)
		{
			bestCount = count;
			bestHand = hand;
		}
	}
	return bestHand;
}

} // namespace ksim
