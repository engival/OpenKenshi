#pragma once

// Template components: read-only data loaded from game-data records and zone
// files (ARCHITECTURE.md "Runtime state": templates vs instances). Live state
// is in state.hpp (namespace ksim::state) and refers to a template by string
// ID only.
//
// One flecs prefab per game-data record; the typed components below hold the
// fields docs/reference/GAMEDATA_SCHEMA.md marks as relevant to the
// simulation. Fields not modelled are listed in NOTES.md ("ksim" section).
//
// Relationships are flecs pairs. Lists whose slots carry meaning keep the
// record's (v0, v1, v2) in a small value component on the pair. Faction
// relations carry the record's starting value in FactionRelation; the live
// value is state::Relation.

#include <cstdint>
#include <string>
#include <vector>

namespace ksim::tmpl {

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

// Present on every prefab: the source record's identity, kept verbatim.
struct Source
{
	std::string stringID;
	std::string name;
	std::string type;
};

// A slot triplet for a reference list (v0, v1, v2 as stored in the record).
struct ListEntry
{
	std::int32_t v0 = 0;
	std::int32_t v1 = 0;
	std::int32_t v2 = 0;
};

// ---------------------------------------------------------------------------
// Prefab components (one per record type)
// ---------------------------------------------------------------------------

struct Faction
{
	std::int32_t defaultRelation = 0;
	std::int32_t enemyClassification = -10;
	std::int32_t fundamentalType = 4; // CharacterTypeEnum.OT_CIVILIAN
	std::int32_t squadFormation = 0; // SquadFormation.RANDOM
	std::int32_t roamingPopulation = 50;
	std::int32_t maxProsperity = 1000;
	std::int32_t businessRelations = -5;
	std::int32_t numRanks = 1;
	std::int32_t numNegativeRanks = 1;
	std::int32_t cagesLockLevel = 40;
	std::int32_t containersLockLevel = 40;
	std::int32_t doorsLockLevel = 40;
	std::int32_t lockLevelRandom = 10;
	float roadPreference = 0.4f;
	float trustworthy = 0.0f;
	float effectOfAnger = 0.2f;
	float effectOfHappy = 0.2f;
	float emotionFadeRate = 1.0f;
	float runAwayRatioOfSquadSize = 0.0f;
	float runAwayRatioRelativeToEnemy = 0.0f;
	bool antiSlavery = false;
	bool notReal = false;
	bool offersBounties = false;
	bool healsStrangers = false;
	bool allowSlavesWeapons = false;
};

struct Town
{
	std::int32_t type = 1; // TownType.TOWN_OUTPOST
	std::int32_t numCentrepoints = 4;
	std::int32_t nestResidentPopulation = 15;
	std::int32_t gearArtifactsMinValue = 0;
	std::int32_t gearArtifactsMaxValue = 0;
	std::int32_t itemArtifactsMinValue = 0;
	std::int32_t itemArtifactsMaxValue = 0;
	float sizeRadius = 350.0f;
	float townRadiusMult = 1.0f;
	float noFoliageRange = 2000.0f;
	bool isPublic = true;
	bool isSecret = false;
	bool residentsOverride = false;
	bool spawnInTownCentre = false;
	bool distantMesh = false;
	std::string unexploredName;
};

struct SquadTemplate
{
	std::int32_t numRandomChars = 0;
	std::int32_t numRandomCharsMax = 0;
	std::int32_t forceSpeed = 0; // MoveSpeed.NO_SPEED_CHANGE
	std::int32_t buildingDesignation = 0;
	std::int32_t vendorMoney = 0;
	std::int32_t vendorsFillTotalAmount = 15;
	std::int32_t bedUsageCost = 100;
	std::int32_t crossbowLevels = 1;
	std::int32_t roboticsLevels = 1;
	float vendorsRefreshTime = 24.0f;
	float animalAgeRandom = 0.1f;
	float bloodSmellMult = 0.0f;
	float buyMult = 1.0f;
	bool isTrader = false;
	bool regenerates = false;
	bool roamingMilitary = false;
	bool patrolApproachesTowns = true;
	bool dontMultiply = false;
	bool buildingRuined = false;
	bool publicDay = true;
	bool publicNight = false;
	bool publicBeds = false;
	bool sellHome = false;
	bool malnourished = false;
	std::string buildingName;
	std::string layoutInterior;
	std::string layoutExterior;
};

// One CHARACTER `shopping` reference (task CH group 3, dt24_shop / dt24_118): the item the character wants, how many
// (`want`, the ref's v0), the day's chance in percent (`chance`, v1) and the order (`priority`, v2: 0 first). The town's
// shopping list is compiled from its residents' lists; the exe stores the compiled one on the town (town vf+0x268,
// 140998e40) and the character's copy (AI+0x2b0) carries its own bought counts.
struct ShoppingRef
{
	std::string itemSid; // ITEM record
	std::int32_t want = 1;
	std::int32_t chance = 100;
	std::int32_t priority = 0;
};

// Shared by CHARACTER, HUMAN_CHARACTER and ANIMAL_CHARACTER.
struct Character
{
	bool animal = false;
	bool unique = false;
	bool named = false;
	bool isTrader = false;
	bool wearsUniform = false;
	bool dumb = false;
	bool assignsBounties = false;
	bool shaved = false;
	std::int32_t npcClass = 2; // CharacterTypeEnum.OT_MILITARY
	std::int32_t armourGrade = 1; // ArmourRarity.GEAR_CHEAP
	std::int32_t armourUpgradeChance = 5;
	std::int32_t femaleChance = 0;
	std::int32_t slave = 0;
	std::int32_t moneyMin = 0;
	std::int32_t moneyMax = 200;
	std::int32_t moneyItemMin = 0;
	std::int32_t moneyItemMax = 0;
	float moneyItemProb = 0.0f;
	std::int32_t wages = 0;
	std::int32_t bountyAmount = 0;
	std::int32_t bountyAmountFuzz = 0;
	std::int32_t bountyChance = 0;
	float factionImportance = 1.0f;
	std::int32_t combatStats = 0;
	std::int32_t rangedStats = 0;
	std::int32_t stealthStats = 0;
	std::int32_t strength = 0;
	std::int32_t unarmedStats = 0;
	std::int32_t statsRandomise = 3;
	// Animal-only fields; harmless defaults on humans.
	std::int32_t attackType = 20; // WeaponCategory.ATTACK_GIRAFFE
	std::int32_t animalStrength = 20;
	std::int32_t inventoryWidth = 4;
	std::int32_t inventoryHeight = 8;
	float lifespan = 20.0f;
	float scaleMin = 0.3f;
	float scaleMax = 1.0f;
	float hpMult = 1.0f;
	float turnRate = 1.0f;
	float smellBlood = 0.0f;
	float smellEggs = 0.0f;
	// The `shopping` list (only 8 CHARACTER records carry one: Citizen, Drifter, Adventurer, the caravan crews, the bounty
	// hunter). A character with no list never takes the SHOPPING goal (118): the list gates it.
	std::vector<ShoppingRef> shopping;
};

struct Race
{
	float walkSpeed = 15.0f;
	float hungerRate = 1.0f;
	float healRate = 1.0f;
	float bleedRate = 1.0f;
	float minBlood = 75.0f;
	float maxBlood = 150.0f;
	float speedMinSkill = 70.0f;
	float speedMaxSkill = 120.0f;
	float pathfindFootprintRadius = 2.5f;
	float pathfindAcceleration = 15.0f;
	float visionRangeMult = 1.0f;
	float strength = 1.0f;
	float dexterity = 1.0f;
	float swimSpeedMult = 1.0f;
	float waterAvoidance = 0.0f;
	std::int32_t extraAttackSlots = 0;
	std::int32_t bloodColour = 0x800000;
	std::int32_t healStat = 6; // StatsEnumerated.STAT_MEDIC
	bool isRobot = false;
	bool playable = true;
	bool cantEnterBuildings = false;
	bool gigantic = false;
	bool selfHealing = false;
	bool vampiric = false;
	bool swims = true;
	bool carriable = true;
	bool singleGender = false;
	bool noHats = false;
	bool noShirts = false;
	bool noShoes = false;
};

struct Building
{
	std::string category = "misc";
	std::string group;
	float scale = 1.0f;
	float buildSpeedMult = 1.0f;
	float buildThreshold = 0.0f;
	float outputRate = 1.0f;
	float hardness = 30.0f;
	std::int32_t buildMaterials = 1;
	std::int32_t pathMode = 0; // PathMode.NAVMESH_OBSTACLE
	std::int32_t storageSizeWidth = 18;
	std::int32_t storageSizeHeight = 18;
	std::int32_t maxOperators = -1;
	std::int32_t powerOutput = 0;
	std::int32_t powerCapacity = 0;
	std::int32_t desirability = 0;
	std::int32_t nodeType = 0;
	std::int32_t itemtypeLimit = 0;
	std::int32_t exteriorMaterial = 2; // GroundType.GROUND_CONCRETE
	std::int32_t interiorMaterial = 6; // GroundType.GROUND_WOOD
	bool hasInventory = false;
	bool hasLock = false;
	bool isWallGate = false;
	bool isFeature = false;
	bool isGateway = false;
	bool isShelter = false;
	bool alwaysLocks = false;
};

struct AiPackage
{
	std::int32_t signalFunc = 0;
	std::int32_t unloadedFunc = 0;
	std::int32_t startTime = 0;
	std::int32_t endTime = 24;
	bool clearsExistingJobs = false;
};

// ---------------------------------------------------------------------------
// Placed world objects (not prefabs; static content from the zone files)
// ---------------------------------------------------------------------------

struct Position
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

// Quaternion, W-first, matching the 28-byte zone transform.
struct Rotation
{
	double w = 1.0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct Scale
{
	float value = 1.0f;
};

// The zone cell named by the zone file (kworld::PlacedInstance::cell).
struct PlacementCell
{
	std::int32_t x = 0;
	std::int32_t z = 0;
};

struct PlacedBuilding
{
	std::string instanceID;
	std::string buildingID;
	std::string layer;
};

// Set instead of IsA when the placement names a BUILDING absent from the
// merged data (14 placements / 3 ids in the shipped files).
struct MissingBuildingPrefab
{
	std::string buildingID;
};

// ---------------------------------------------------------------------------
// Relationship types (flecs pairs)
// ---------------------------------------------------------------------------

// Membership and ownership (ARCHITECTURE.md "Language and stack").
struct MemberOf
{
};

struct OwnedBy
{
};

// Value-carrying list relations. Each relationship type is its own struct
// because flecs stores the pair value under the relationship's type.
struct TownResidents : ListEntry
{
};

struct TownBarSquads : ListEntry
{
};

struct TownRoamingSquads : ListEntry
{
};

struct TownDefaultResident : ListEntry
{
};

struct FactionResidents : ListEntry
{
};

struct FactionDefaultResident : ListEntry
{
};

struct FactionBarSquads : ListEntry
{
};

struct FactionSquads : ListEntry
{
};

struct FactionSquadDefault : ListEntry
{
};

struct FactionRoamingSquads : ListEntry
{
};

struct SquadMember : ListEntry
{
};

struct SquadMember2 : ListEntry
{
};

struct SquadLeader : ListEntry
{
};

struct SquadChooseFrom : ListEntry
{
};

struct SquadAnimal : ListEntry
{
};

struct SquadAnimal2 : ListEntry
{
};

struct RaceOption : ListEntry
{
};

struct AiPackageLink
{
	std::int32_t priority = 0;
	std::int32_t v1 = 0;
	std::int32_t v2 = 0;
	std::int32_t order = 0; // the entry's index in the record's `AI packages` (data order: the board's tie-break, behaviours.cpp)
};

struct AiInherits
{
};

struct AiLeadsTo
{
	std::int32_t order = 0; // the entry's index in the package's `Leads to` (blackboard.cpp readPackage takes the first)
};

struct OverridesTown
{
};

// Starting faction relation value, -100..100 (GAMEDATA_SCHEMA.md "FACTION",
// `relations` v0). Unlisted pairs fall back to Faction::defaultRelation.
struct FactionRelation
{
	std::int32_t value = 0;
};

} // namespace ksim::tmpl
