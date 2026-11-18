#pragma once

// Combat data: everything the combat layer reads from the merged game data,
// resolved once (docs/reference/combat/DATA.md). Constants come from the
// GLOBAL CONSTANTS record of the merged load order (rebirth.mod included), with
// the exe's load-time scaling applied (RESOLUTION.md 0.1); the animation clip
// lengths and root motion come from the shared Ogre skeleton reader.
//
// Nothing here is hardcoded that the data has. What the data lacks and the
// exe supplies is marked EXE: the armour resistance tables (RESOLUTION.md 4.3).

#include "kdata/gamedata.hpp"
#include "ksim/body.hpp"
#include "ksim/fighter_state.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ksim {

// GLOBAL CONSTANTS after the loader's scaling (`14086b2b0`).
struct CombatConstants
{
	std::int32_t maxNumAttackSlots = 1;
	double attackChanceFactor = 0.05;
	double baseBlockChance = 70.0;
	double blockReductionPerLevel = 1.5; // `block chance reduction per 10levels` * 0.1
	double blockIncreasePerLevel = 1.2; // `... increase per 10levels` * 0.1
	double cutDamage1 = 0.0; // `cut damage 1` * `damage multiplier`
	double cutDamage99 = 0.0;
	double bluntDamage1 = 0.0;
	double bluntDamage99 = 0.0;
	double pierceDamageMultiplier = 0.0; // * `damage multiplier`
	double unarmedDamageMult = 1.0;
	double resistanceMin = -0.5;
	double resistanceMax = 0.5;
	double stumbleDamageMax = 0.0; // `stumble damage max` * `damage multiplier`
	double immediateBloodLoss = 0.2;
	double bleedRate = 0.0; // * 0.1
	double bleedClotRate = 0.0; // * 0.1
	double bloodRecoveryRate = 0.0; // * 0.1
	double extraBloodLoss = 1.0;
	double bluntPermanentOrganDamage = 0.0;
	double minToughnessKoPoint = 10.0;
	double maxToughnessKoPoint = 99.0;
	double knockoutTimeBase = 30.0;
	double knockoutMult1 = 4.0;
	double knockoutMult99 = 1.0;
	double stunRecoveryRate = 1.0;
	double healRateMult = 0.25;
	double restingHealRateMult = 2.0;
	double xpRateStrength = 0.5;
	double xpRateToughness = 1.33;
	double expGainMultiplier = 3.0;
	double skillDiffXp0xPenalty = 25.0;
	double skillDiffXp2xBonus = 10.0;
	double minStrengthXpMult = 0.1;
	double weightStrengthDiff1x = 20.0;
	double weightStrengthDiffMax = 1.0;
	double robotWearRate = 1.0;
	// Medicine and hunger (MEDICAL_HUNGER_LIVE.md). Names are the CONSTANTS record's; the merged load order gives the
	// live numbers (rebirth: medic speed 3, starvation time 60 h, clot rate 0.00085).
	double medicSpeed = 3.0; // `medic speed mult`
	double robotMedicSpeed = 1.0; // `robot medic speed mult`
	double medkitDrain1 = 2.0; // `medkit drain 1` x 0.1: charges per second at skill 1
	double medkitDrain99 = 0.05; // `medkit drain 99` x 0.1
	double degenerationMult1 = 1.7; // `degeneration mult 1` (low toughness)
	double degenerationMult99 = 0.0; // `degeneration mult 99`
	double bodypartDegenerationRate = 1.0;
	double starvationHours = 24.0; // `starvation time`: game hours from fed to starved at Hunger time 1
	double bedHungerRate = 0.4; // `bed hunger rate`
	double encumbranceHungerRate = 0.7; // `encumbrance hunger rate`
	// Carrying (GATES_CARRY_STATIC.md section 5; GLOBAL CONSTANTS, merged data: 1.2 / 15 / 30): capacity = carryWeightMult x
	// strength x vf10 + encumbranceBase; a carried person weighs carryPersonWeight, whoever it is.
	double carryWeightMult = 1.2; // `carry weight mult`
	double encumbranceBase = 15.0; // `encumbrance base`
	double carryPersonWeight = 30.0; // `carry person weight`
	double xpMedic1 = 15.0; // `XP rate medic 1`
	double xpMedic99 = 150.0; // `XP rate medic 99`
};

// COMBAT_TECHNIQUE, one ImpactPoint (`attack direction N`, `power N`, `anim blocked frame N`,
// `anim stop frame N`, `limb N`). Fractions of the clip, power as a multiplier.
struct ImpactPoint
{
	std::int32_t dir = 0;
	double power = 1.0;
	double hitFrac = 0.8;
	double stopFrac = 1.0;
	std::int32_t limb = 4;
};

struct Technique
{
	std::string name;
	std::string animName;
	double speedMult = 1.0; // +0x28
	bool isBlock = false;
	bool isDodge = false;
	bool isStumbleDodge = false;
	bool gainsGround = false;
	bool isProne = false;
	bool lowStrike = false;
	double hesitate = 0.2;
	double attackDistance = 0.0;
	double attackDistanceMinVsStatic = 0.0;
	double acceptableEndTime = 0.95;
	double minSkill = -100.0;
	double maxSkill = 999.0;
	double maxEncumbrance = 999.0;
	double chance = 1.0;
	std::int32_t maxHits = 99;
	std::array<bool, 9> weapon{}; // by weapon type: 0 katana 1 sabre 2 blunt 3 heavy 4 hackers 5 unarmed 8 polearm
	std::int32_t animal = 0; // WeaponCategory; 0 and 5 are the human set
	bool disabled = false;
	std::vector<ImpactPoint> impacts;
	std::int32_t clip = -1; // index into CombatData::clips, -1 when the clip is missing
	// Task BQ: an animal technique's playable length, in seconds. It has no clip in the human skeleton (the animal clips live in
	// `data/animal/meshes/*.skeleton`, which is not loaded), so it is scaled to put its first impact at kAnimalAttackSeconds;
	// 0 = the clip's own length (CombatArena::clipLength).
	double length = 0.0;
};

// Task BQ: seconds from an animal technique's start to its first impact. Measured live on the beak's `attack1` (kinspect
// findings/beak_zigzag.md 6edd18a: 47-54 frames, mean 48, of a 60 fps run). The 5.0 s skeleton clip has no root motion and cannot
// give it (the exe's animal clip playback speed is not decoded, so an animal swing is started at clip speed 1). UNSURE: one
// species measured, the rest follow the same rule.
inline constexpr double kAnimalAttackSeconds = 0.8;

// Root motion of a clip sampled at a fixed step: the cumulative displacement of
// the root bone (skeleton frame: +z forward, +x lateral) and its yaw (radians).
struct Clip
{
	std::string name;
	double length = 0.0;
	double step = 0.0; // seconds between samples
	struct Sample
	{
		float x = 0.0f;
		float z = 0.0f;
		float yaw = 0.0f;
	};
	std::vector<Sample> root; // cumulative; root[0] is zero

	// Cumulative root displacement at clip time t (clamped to [0, length]).
	Sample at(double t) const noexcept;
};

// LOCATIONAL_DAMAGE.
struct AnatomyPart
{
	std::string sid;
	std::string name;
	std::int32_t type = 0;
	double koMult = 1.0;
	std::int32_t collapseBits = 0;
	bool collapses = false;
	bool death = false;
	bool severance = false;
	double affectsSkills = 0.0;
	double affectsMoveSpeed = 0.0;
};

struct RaceAnatomyEntry
{
	std::int32_t part = -1; // index into CombatData::anatomy
	double chance = 100.0; // `combat anatomy` v0
	double health = 100.0; // v1
};

struct RaceCombat
{
	std::string sid;
	std::string name;
	double strength = 1.0;
	double dexterity = 1.0;
	double combatMoveSpeedMult = 1.0;
	double walkSpeed = 15.0;
	double speedMinSkill = 70.0;
	double speedMaxSkill = 120.0;
	std::int32_t bloodColour = 0x800000; // RACE `blood colour`, 0xRRGGBB (task limb-tint)
	double minBlood = 75.0;
	double maxBlood = 150.0;
	double bleedRate = 1.0;
	double healRate = 1.0;
	double hungerRate = 1.0; // RACE `hunger rate`: R of the hunger formula
	double visionRangeMult = 1.0; // RACE `vision range mult`: x 1500 sight range (perception.hpp)
	double pathfindAcceleration = 20.0; // RACE `pathfind acceleration` (Havok units: world u/s^2 = x10): the walker's ramp (knav/walk.hpp)
	double waterCost = 1.0; // 1 + RACE `water avoidance` (human 7.0, Fishman 0.25): the routing cost scale of water (Body::water)
	double footprintRadius = 2.6; // RACE `pathfind footprint radius` (human 2.6): the R of the interaction reach (interact.hpp)
	// Task BQ: the body width cc+0x1a0 the impact box is built from (its lateral half is this + 0.5), from RACE `hull size X`
	// / 2 + 1.5: a humanoid (hull X 5) keeps the 4.0 the box always used; the beak (18) gets 10.5, which reproduces the live
	// box's 22 u width (kinspect findings/beak_zigzag.md). OURS/UNSURE: the pad is a two-point fit; the exe's own formula and
	// an animal's size-by-age scale are not decoded.
	bool carriable = true; // RACE `carriable` (+0x7a; false only on Big-Bones and Leviathan): the lift gate 14034f990 refuses a false one
	double bodyWidth = 4.0;
	bool hasFemale = false; // RACE lists `heads female`: a female body exists (the creator's sex(true)); else a spawn stays male
	bool isRobot = false; // RACE `is robot` = +0x7c: an inedible victim (dt39 Q8)
	bool vampiric = false; // RACE `vampiric` = +0x7d: the eat application drains blood instead of eating limbs
	std::int32_t extraAttackSlots = 0;
	// RACE `weather immunity0..N` (RaceData::load, kinspect note restaged c29899b): the field VALUE is an affect type and a 0 is
	// skipped, so bit n = immune to weather n. Skeletons and robots list 1..4; hive drones, fishmen, cannibals and goats list 2
	// (acid). An immune character takes no weather damage of that type (the same pattern as the armour weather protection).
	std::uint32_t weatherImmunity = 0;
	std::vector<RaceAnatomyEntry> anatomy;
};

// Combat options where we may differ from the original (docs/COMBAT_OPTIONS.md) and the
// exe's hidden settings globals (RESOLUTION.md 0.2). Configuration, not state.
struct CombatSettings
{
	// `combat.fix_extra_attack_slots`: off reproduces the original's slot-loop bug exactly.
	bool fixExtraAttackSlots = true;
	// `xp.no_runaway_skills`: addXP gives nothing at skill >= 101 (authored values untouched).
	bool noRunawaySkills = true;
	// `combat.retarget_hold_s`: seconds a normal NPC keeps its target before pickTarget may re-pick (`+0x2f0 < hold`). 1.0 = the exe
	// (rwh113: 1.0 s, 0.3 s with CharStats+0x12b, not modelled); 0 re-picks every tick. COMBAT_OPTIONS.md 6.
	double retargetHold = 1.0;
	// `squad.follow_heading_fix`: off reproduces the original's follow-slot heading exactly,
	// including the 180-degree near-fixed point (SQUAD_FOLLOW.md s9); on snaps the smoothed heading to the
	// leader's direction when they differ by more than 150 degrees.
	bool squadFollowHeadingFix = false;
	// FACTION `squad formation` of the player faction (SQUAD_FOLLOW.md s3): 0 random, 1 caravan, 2 military.
	// UNSURE: the player faction's value is not read from the data yet; 0 is what 23 of 30 factions have.
	int squadFormation = 0;
	// DAT_142133580: multiplies every damage component, every XP award and the unarmed
	// self-injury. 1.0 in the reference save (proved by the exact toughness ratio); the new-game option gdm
	// (state::WorldOptions::damageScale) overrides it in a world that has one.
	double damageScale = 1.0;
	// DAT_142133578: multiplies bleeding. UNSURE default.
	double bleedScale = 1.0;
	// Medical time per sim second (everything in MedicalSystem::update except hunger: bleed, clotting, stun decay, the KO
	// timer, healing). The original feeds the update Character+0xc8 = the sim frame dt x game speed plus a clamped wall-clock
	// delta, so the rate is the frame cadence, not a constant. Measured: 2.00 (Escaped Servant, watched) / 1.5 (Engival) in
	// the katana duels (clog_duel_katana_r1/r12/r2, MEDICAL_HUNGER_LIVE.md), 1.0 far from the camera, ~1.4 in the dt2 collapse
	// traces; against the live KO timer T values it is 1 unit ~= 1.04 real s at speed 1 (KO_DEATH_LIVE.md), i.e. 0.96 units
	// per sim second, and medicalClock 2.0 came out as 2.08x the original's medical units (task BA, STATE 2026-11-06).
	// Default 1 / 1.04 = the original's nominal rate (user, 2026-11-07: "sync it with original kenshi"); ~1.0 = sim time. It is
	// the dev override DEBUG_SET_OPTION changes live (COMBAT_OPTIONS.md 4), never saved. UNSURE: the frame-rate term.
	double medicalClock = 1.0 / 1.04;
	// DAT_142133504, settings.cfg `Dismemberment` (BQ11 s76): 0 Never, 1 Rare (the exe's default when the key is absent), 2 Frequent.
	// >= 1 severs a humanoid's limb on a hit that takes H - D below -M; >= 2 also by the passive sink. Animals never sever.
	int limbLossLevel = 1;
	// New-game option "Hunger time" (GOG exe+0x21324a8 index 7): multiplies `starvation time`. 4.0 on the measured
	// save (T = 60 x 4 = 240 game hours), the default here (MEDICAL_HUNGER_LIVE.md); state::WorldOptions::hungerTime (ht)
	// overrides it in a world that has one.
	double hungerTime = 4.0;
	// New-game option "Chance of death" (1.0 measured): multiplies the bleed rate and the degeneration;
	// state::WorldOptions::chanceOfDeath (cod) overrides it in a world that has one.
	double chanceOfDeath = 1.0;
	// Which gate decides the unconscious flag of a VITAL part (RESOLUTION.md 5.2; the hit path is VERIFIED at 0).
	// Zero: effective health < 0. KoPoint: below -koPoint(toughness). Limbs do not use this switch: their collapse
	// gate is always the KO point (MEDICAL_HUNGER_LIVE.md e, kinspect 8de1eab).
	enum class KoGate : std::uint8_t
	{
		Zero = 0,
		KoPoint = 1,
	};
	KoGate koGate = KoGate::Zero;
	// Footwork and hit geometry, fitted to the reference fights (UNSURE, IMPLEMENTATION.md):
	// the circling band as centre distances, and the reach of an impact:
	// hitReachBase + hitReachPerDistance x technique attack distance.
	double ringInner = 16.0;
	double ringOuter = 26.0;
	double hitReachBase = 18.0;
	double hitReachPerDistance = 0.28;
	// Combat updates per Simulation tick (the arena step is tick / substeps).
	int substeps = 1;
	// Testing only (ksim_server --stealth N): >= 0 sets the Stealth skill of every NEW_GAME character (the sneak run check).
	double newGameStealth = -1.0;
};

// EXE tables (ArmourStats constructor `1408a8f90`, RESOLUTION.md 4.3), by material then class.
struct ArmourTables
{
	// material 0 cloth, 1 leather, 2 chain, 3 plate: LO and HI of cut, blunt, pierce.
	static constexpr double kCut[4][2] = {{0.00, 0.10}, {0.00, 0.55}, {0.05, 0.65}, {0.10, 0.85}};
	static constexpr double kBlunt[4][2] = {{0.00, 0.30}, {0.00, 0.40}, {0.00, 0.30}, {0.00, 0.60}};
	static constexpr double kPierce[4][2] = {{0.0, 0.0}, {0.0, 35.0}, {0.0, 60.0}, {0.0, 90.0}};
	// class 0 cloth, 1 light, 2 medium, 3 heavy.
	static constexpr double kClassCut[4] = {0.4, 0.8, 0.9, 1.0};
	static constexpr double kClassBlunt[4] = {0.1, 0.7, 0.9, 1.0};
};

// ITEM `item function` values the medical slice uses (MEDICAL_HUNGER_LIVE.md). Food is the pair the eat searches take (dt24_eat
// section 9: 0xf FOOD_RESTRICTED is tried first, then 3 FOOD).
inline constexpr std::int32_t kItemFunctionMedkit = 1;
inline constexpr std::int32_t kItemFunctionFood = 3;
inline constexpr std::int32_t kItemFunctionLimb = 0x11; // a severed limb item (1409993a0's o+0x124 test)
inline constexpr std::int32_t kItemFunctionRobotKit = 12;
inline constexpr std::int32_t kItemFunctionFoodRestricted = 15;
// Food charges per unit of an ITEM's `charges`: what a member starts a meal with (medical.cpp bites it down; crops.cpp's bite starts one).
inline constexpr double kFoodChargesPerUnit = 100.0;

// An ITEM record, as far as eating and first aid need it.
struct ItemInfo
{
	std::int32_t function = 0;
	double charges = 1.0; // `charges`
	double quality = 1.0; // `quality`
};

class CombatData
{
public:
	// Throws on a missing record or key (no silent fallbacks). dataDir is the game `data/`.
	static std::shared_ptr<const CombatData> build(const kdata::GameData& gameData,
		std::string_view dataDir);

	CombatConstants k;
	std::vector<Technique> techniques;
	std::vector<Clip> clips;
	std::vector<AnatomyPart> anatomy;
	std::vector<RaceCombat> races;
	// race() is asked per fighter pair per tick: sid and name -> index into races, first race wins (as the linear scan did); empty for a
	// hand-built CombatData, which race() then scans.
	struct RaceKeyHash
	{
		using is_transparent = void;
		std::size_t operator()(std::string_view key) const noexcept { return std::hash<std::string_view>{}(key); }
	};
	std::unordered_map<std::string, std::size_t, RaceKeyHash, std::equal_to<>> raceIndex;
	std::unordered_map<std::string, ItemInfo> items; // ITEM records by stringID

	const ItemInfo* item(const std::string& sid) const noexcept
	{
		const auto found = items.find(sid);
		return found == items.end() ? nullptr : &found->second;
	}

	const Technique* technique(std::string_view name) const noexcept;
	std::int32_t techniqueIndex(std::string_view name) const noexcept;
	const Clip* clip(std::string_view name) const noexcept;
	const RaceCombat* race(std::string_view sidOrName) const noexcept;
	// The routing clearance radius of a race: RACE `pathfind footprint radius` (docs/formats/NAVMESH_CACHE.md
	// "Clearance", TravelFilter::radius). The merged data puts the Greenlander at 4.0, the exe's CharMovement+0x364;
	// a race not in the data gets RaceCombat's own default. Every character route asks for it, never for 0.
	double pathRadius(std::string_view raceSid) const noexcept
	{
		const RaceCombat* found = race(raceSid);
		return found != nullptr ? found->footprintRadius : RaceCombat{}.footprintRadius;
	}
	// The traveller of a race's routes: pathRadius and the water cost scale. A race not in the data gets no water scale (the filter's default).
	Body body(std::string_view raceSid) const noexcept
	{
		const RaceCombat* found = race(raceSid);
		return found != nullptr ? Body{found->footprintRadius, static_cast<float>(found->waterCost)} : Body{pathRadius(raceSid)};
	}
	std::int32_t anatomyIndex(std::string_view name) const noexcept;

	// Clip of the hit reaction played on a landed hit ("mid blow light"), -1 when absent.
	std::int32_t stumbleClip = -1;
	// The heavy hit reaction ("mid blow"), -1 when absent.
	std::int32_t stumbleHeavyClip = -1;

	// The FISTS weapon: what a character fights with once its weapon is taken (looting).
	state::FighterWeapon unarmed;

	// Weapon (WEAPON record) at item level L in 0..1 (RESOLUTION.md 1.3) as CharStats copies it.
	// manufacturerCut / Blunt / Weight: WEAPON_MANUFACTURER `cut damage mod` / `blunt damage mod` / `weight mod`.
	state::FighterWeapon makeWeapon(const kdata::Record& weapon, double level,
		double manufacturerCut = 1.0, double manufacturerBlunt = 1.0, double manufacturerWeight = 1.0) const;

	// The crossbow (CROSSBOW record) at item level L in 0..1 as GunClass::loadRangedStats reads it (dt41_A), full of kStartAmmo bolts.
	static constexpr std::int32_t kStartAmmo = 30; // UNSURE: the decode has no ammo count (FEATURES "Crossbows")
	state::FighterRanged makeRanged(const kdata::Record& crossbow, double level) const;
	// A turret's gun (GUN_DATA row `gunData`, task ds/turret): its own range / accuracy / reload, the damage of the data's first CROSSBOW record.
	state::FighterRanged makeTurretGun(const kdata::GameData& gameData, const kdata::Record& gunData) const;

	// Armour piece (ARMOUR record) at item level L in 0..1 (RESOLUTION.md 4.3).
	state::FighterArmour makeArmour(const kdata::Record& armour, double level) const;

	// The weapon / armour piece an item instance's quality describes: makeWeapon with the quality's manufacturer mods and model
	// attack / defence mods, makeArmour at its level; the quality is kept on the result. This is what makeFighter spawns with
	// and what equipping rebuilds from, so unequip + equip gives exactly the same numbers. Throws on an unset quality.
	state::FighterWeapon buildWeapon(const kdata::GameData& gameData, const kdata::Record& weapon, const state::ItemQuality& quality) const;
	state::FighterArmour buildArmour(const kdata::Record& armour, const state::ItemQuality& quality) const;

	// A generated quality for an item that is stocked rather than carried (container contents; the vendor rule of
	// LOCKS_EQUIP_STOCK_LIVE.md e: grade by the generic weights 50 / 100 / 50 / 2 / 0 / 0, quality 5 / 20 / 40 / 60 / 80 / 95).
	// rollGrade and rollPick are uniform in [0, 1). A weapon takes a manufacturer that lists it in `weapon types` and its model
	// nearest the quality; unset for an item that is neither weapon nor armour. UNSURE: the stock path's real weapon roll.
	// A merchant (trader.hpp) passes its faction's `armors 0..5` as `gradeWeights` (six weights; null = the generic ones above) and its
	// VENDOR_LIST `weapon manufacturers` as `makers` (null = every maker; a maker outside the list is skipped, one that lists none of
	// the weapon's makers falls back to all; UNSURE: the manufacturer is not drawn by the list's v0, the nearest model decides).
	// Static: it reads no member.
	static state::ItemQuality rollQuality(const kdata::GameData& gameData, const kdata::Record& item, double rollGrade, double rollPick,
		const double* gradeWeights = nullptr, const std::vector<std::string>* makers = nullptr);

	// The unarmed "weapon": the FISTS record.
	state::FighterWeapon makeFists(const kdata::GameData& gameData) const;

	// A fighter from a CHARACTER record: skills from its STATS record (else `combat stats`,
	// `unarmed stats`, `strength` with `stats randomise` drawn from rng), a race (raceSid when given;
	// else the squad template's `race override`, the character's `race` list or its faction's `races`,
	// rolled by weight), the carried weapon (first `weapons` entry with quantity >= 1) with a manufacturer
	// and model rolled from `weapon level`, and one `clothing` piece per slot at a level from `armour grade`
	// (see the .cpp for the rules, all checked against the e1 and Bad Teeth join records). The caller sets
	// position, team and Id. squadTemplateSid is optional context ("" = none): a bandit record has no race
	// of its own and needs its squad's faction.
	state::Fighter makeFighter(const kdata::GameData& gameData, std::string_view characterSid,
		std::string_view raceSid, std::uint64_t& rng, std::string_view squadTemplateSid = {}) const;

	// The sex a spawned CHARACTER rolls: `female chance` percent (the exe's FUN_1402e2b50 draws randint(0,100); STARTS_CAGES_STATIC.md),
	// from the character's own dice state at spawn (a pure hash, no draw: a re-run gives the same sex and no other roll moves). 0 for an
	// animal, a race with no female body, or a chance of 0. makeFighter sets Fighter::look.female with it; newgame reads it back.
	static std::uint8_t rollFemale(const kdata::Record& character, const RaceCombat& race, std::uint64_t diceState);

	// Body parts for a race at full health.
	void initParts(state::Fighter& fighter, const RaceCombat& race) const;
};

} // namespace ksim
