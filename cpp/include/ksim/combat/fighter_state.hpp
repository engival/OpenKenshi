#pragma once

// The character layer's saved state (ARCHITECTURE.md "Runtime state"): one
// state::Fighter per character at full detail. Included by state.hpp, which
// lists it in SavedComponents.
//
// A Fighter holds a CharStats-like skill block, a MedicalSystem-like body, the
// CombatClass fields the fight logic needs (docs/reference/combat/ENGAGEMENT.md
// has the original offsets) and the kinematics of the character. The combat
// arena (combat.hpp) keeps these in a sorted array as the canonical hot copy and
// mirrors them one way into flecs at the tick boundary, like the region engine.
//
// All numbers are doubles (the save codec has no float); nothing derived is
// stored: clip speeds, block chances and the like are recomputed from here.
// Cross references are state::Id values (0 = none), never flecs ids.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ksim::state {

// CombatClass+0x1f0 (MEASURED.md "Combat state enum", ENGAGEMENT.md).
enum class CombatState : std::int32_t
{
	Attack = 0,
	Defend = 1, // reactive technique (block, dodge)
	Between = 3,
	Hub = 4,
	Circle = 5,
	Wait = 6,
	Stumble = 8,
	Done = 9,
	Approach = 11, // 10 (entry, one frame) is folded into 11
};

// CharStats skills (the save-writer names).
struct FighterSkills
{
	bool operator==(const FighterSkills&) const = default;

	double strength = 1.0;
	double dexterity = 1.0;
	double toughness = 1.0;
	double athletics = 1.0;
	double dodge = 1.0;
	double attack = 1.0; // melee attack
	double defence = 1.0; // melee defence
	double katana = 1.0;
	double sabres = 1.0;
	double hackers = 1.0;
	double blunt = 1.0;
	double heavy = 1.0;
	double unarmed = 1.0; // martial arts
	double poles = 1.0;
	double medic = 1.0; // First Aid (STATS `medic`, CharStats+0x98)
	double lockpicking = 1.0; // STATS `lockpicking` (CharStats stat 0x25; interact.cpp lockpicking)
	double stealth = 1.0; // STATS `stealth` (CharStats+0xa4, stat 0x10, 0..100): the stealth fraction of perception.hpp
	double science = 1.0; // STATS `science` (CharStats+0xe0, stat 4): prospecting's area and map size (prospect.hpp)
	double labouring = 1.0; // STATS `labouring` (stat 3): the skill of mines and manual refineries (producers.hpp, MQ2 pending)
	double engineer = 1.0; // STATS `engineer` (stat 5, save v88): the Build and Dismantle tasks' skill (b50c_static Q6; M that stat 5 is it)
	double crossbows = 1.0; // STATS `bow` (save v96): the Crossbows skill (dt41_A: the shot's accuracy and the reload read it)
	double turrets = 1.0; // STATS `turrets` (save v97): the skill id 0xb a turret's gunner shoots with (dt41_B section 3)
	double thievery = 1.0; // STATS `thievery` (save v98, stat 10): a term of the victim's defence against a stealth KO (dt41_C)
	double perception = 1.0; // STATS `perception` (save v113, stat 0x18): trained by the ranged aim (combat.cpp rangedFire); nothing else reads it yet
	double assassination = 1.0; // stat 0x16 (CharStats+0xb8, save v98): the stealth KO's attack and its KO time. No STATS key: UNSURE where it starts

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.strength);
		ar(self.dexterity);
		ar(self.toughness);
		ar(self.athletics);
		ar(self.dodge);
		ar(self.attack);
		ar(self.defence);
		ar(self.katana);
		ar(self.sabres);
		ar(self.hackers);
		ar(self.blunt);
		ar(self.heavy);
		ar(self.unarmed);
		ar(self.poles);
		ar(self.medic);
		ar(self.lockpicking);
		ar(self.stealth);
		ar(self.science);
		ar(self.labouring);
		if (ar.version >= 88)
			ar(self.engineer);
		if (ar.version >= 96)
			ar(self.crossbows);
		if (ar.version >= 97)
			ar(self.turrets);
		if (ar.version >= 98)
		{
			ar(self.thievery);
			ar(self.assassination);
		}
		if (ar.version >= 113)
			ar(self.perception);
	}
};

// One body part (MedicalSystem's HealthPartStatus).
struct FighterPart
{
	bool operator==(const FighterPart&) const = default;

	std::string name; // LOCATIONAL_DAMAGE name
	std::int32_t type = 0; // body part type: 0 torso, 1 leg, 2 arm, 3 head
	double chance = 100.0; // +0x38 hit weight (RACE combat anatomy v0)
	double heat = 1.0; // +0x3c
	double maxHealth = 100.0; // +0x54 * +0x58 * +0x5c
	double wear = 0.0; // +0x50
	double health = 100.0; // +0x40
	double stun = 0.0; // +0x44
	double bonus = 0.0; // +0x4c
	double koMult = 1.0; // LOCATIONAL_DAMAGE `KO mult`
	std::int32_t collapseBits = 0; // `collapse part`
	bool vital = false; // `death`
	bool collapses = false;
	bool severable = false;
	bool collapsed = false;
	double pool = 0.0; // bandage pool: health a medic has put on this part that has not converted yet
	bool robotic = false; // a robot race's part (or a prosthetic): needs a robotic kit, heals x250

	// A severed limb (limb state 1, severPart 14064edc0, BQ11): the exe forces its max to 5 every tick. The max is the persisted
	// marker (no extra save field): no race has a limb whose own max is exactly 5, and we have no prosthetics (state 2, which
	// also sets 5). It leaks, sinks only to -3 x 5, and stays severed.
	static constexpr double kSeveredMax = 5.0;
	bool limb() const noexcept { return !vital && (type == 1 || type == 2); }
	bool severed() const noexcept { return limb() && maxHealth == kSeveredMax; }

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.type);
		ar(self.chance);
		ar(self.heat);
		ar(self.maxHealth);
		ar(self.wear);
		ar(self.health);
		ar(self.stun);
		ar(self.bonus);
		ar(self.koMult);
		ar(self.collapseBits);
		ar(self.vital);
		ar(self.collapses);
		ar(self.severable);
		ar(self.collapsed);
		ar(self.pool);
		ar(self.robotic);
	}
};

// Part coverage of an armour piece (ARMOUR `part coverage` v0 / 100).
struct FighterCoverage
{
	bool operator==(const FighterCoverage&) const = default;

	std::string part;
	double coverage = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.part);
		ar(self.coverage);
	}
};

// The generated quality of a weapon or armour item instance (SIMLINK.md INVENTORY, RUNTIME_SAVE.md). A weapon: the
// WEAPON_MANUFACTURER and the MATERIAL_SPECS_WEAPON model that were rolled for it, and the model's item level in 0..1. An armour
// piece: the item level (0.2 x grade for a piece with part coverage, 1 without). `level` < 0 = no quality (an item that is not
// a weapon or armour, or one nobody generated). `grade` is the display grade 0..5 (prototype .. masterwork), kNoGrade for none.
struct ItemQuality
{
	static constexpr std::uint8_t kNoGrade = 255;
	std::string maker; // WEAPON_MANUFACTURER record ("" for armour)
	std::string model; // MATERIAL_SPECS_WEAPON record ("" for armour)
	double level = -1.0;
	std::uint8_t grade = kNoGrade;
	// A faction-uniform armour piece (item+0x1e8, kinspect s72 section 4): the FACTION whose uniform it is; "" = none. Set at spawn on
	// the armour of a CHARACTER with `wears uniform`; it stays with the piece when looted, dropped or sold. Prices x0.3 (trade.cpp).
	std::string uniform;

	bool set() const noexcept { return level >= 0.0; }
	bool operator==(const ItemQuality&) const = default;

	// Grade name band of an item level, the exe's (FUN_140796300, GRADES_GATESQUADS_KO_LIVE.md a) on the int g = round(level x 100):
	// < 11 prototype, <= 20 shoddy, < 41 standard, < 61 high, < 81 specialist, else masterwork. The game shows the word only for
	// 0 < g < 100; the colour uses other bands, derived from the level by the client.
	static std::uint8_t gradeOf(double level) noexcept
	{
		const long g = std::lround(level * 100.0);
		return static_cast<std::uint8_t>(g < 11 ? 0 : g <= 20 ? 1 : g < 41 ? 2 : g < 61 ? 3 : g < 81 ? 4 : 5);
	}

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.maker);
		ar(self.model);
		ar(self.level);
		ar(self.grade);
		ar(self.uniform);
	}
};

// An armour piece with its generated numbers (RESOLUTION.md 4.3).
struct FighterArmour
{
	bool operator==(const FighterArmour&) const = default;

	ItemQuality quality; // what the piece was built from (the inventory instance carries the same)
	std::string name;
	std::string sid; // the ARMOUR record's stringID (the item the inventory holds)
	std::int32_t slot = 7; // ARMOUR `slot` (AttachSlot)
	double cut = 0.0; // +0x228
	double blunt = 0.0; // +0x22c
	double pierce = 0.0; // +0x230
	double cutIntoStun = 0.5; // +0x238
	double combatSpeedMult = 1.0;
	double dodgeMult = 1.0;
	double damageOutputMult = 1.0;
	double attackBonus = 0.0; // `combat attk bonus`
	double defenceBonus = 0.0; // `combat def bonus`
	double unarmedBonus = 0.0;
	std::vector<FighterCoverage> coverage;
	// Weather protection (acid rain: kinspect weather_acid_rain.md section 3). `weather types`: bit n set when the piece lists a
	// `weather protectionN` entry for affect type n (the field's value is the type; type 2 is rain/acid). `weather protection
	// amount` is the piece's share, summed over the worn pieces that protect the type (unclamped: 1.3 is total protection).
	std::uint32_t weatherTypes = 0;
	double weatherProtection = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.sid);
		ar(self.slot);
		ar(self.cut);
		ar(self.blunt);
		ar(self.pierce);
		ar(self.cutIntoStun);
		ar(self.combatSpeedMult);
		ar(self.dodgeMult);
		ar(self.damageOutputMult);
		ar(self.attackBonus);
		ar(self.defenceBonus);
		ar(self.unarmedBonus);
		ar(self.coverage);
		ar(self.quality);
		// Save v85: weather protection (task indoors). An older save loads none, i.e. no acid-rain protection until re-equipped.
		if (ar.version >= 85)
		{
			ar(self.weatherTypes);
			ar(self.weatherProtection);
		}
	}
};

// The wielded weapon as CharStats copies it (WeaponData::loadDamage, RESOLUTION.md 1.3).
// weaponType: 0 katana, 1 sabre, 2 blunt, 3 heavy, 4 hackers, 5 unarmed, 8 polearm.
struct FighterWeapon
{
	bool operator==(const FighterWeapon&) const = default;

	std::string name;
	std::string sid; // the WEAPON record's stringID (the item the inventory holds)
	std::int32_t weaponType = 5;
	double cutMult = 0.0; // +0x19c
	double bluntMult = 0.0; // +0x1a0
	double bleedMult = 0.0; // +0x1a4
	double pierceMult = 0.0; // +0x1a8
	double cut1 = 0.0; // +0x1ac
	double blunt1 = 0.0; // +0x1b0
	double humanMult = 1.0; // +0x1b8
	double animalMult = 1.0; // +0x1bc
	double robotMult = 1.0; // +0x1b4
	double armourPenetration = 0.0; // +0x1c0
	double length = 18.0; // +0x200
	double weight = 0.0; // +0x228
	double attackMod = 0.0; // weapon `attack mod` (+ material spec)
	double defenceMod = 0.0;
	// Weapon `indoors mod` (an int in the data: rebirth.mod Polearm -8 ... Wakizashi +4; indoors_state.md section 2). Added to
	// the equipment bonus of attack and defence while the character is indoors outside a `cavernous` building (CombatRules).
	double indoorsMod = 0.0;
	ItemQuality quality; // the manufacturer, model and level the numbers above were built from (unset for fists)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.sid);
		ar(self.weaponType);
		ar(self.cutMult);
		ar(self.bluntMult);
		ar(self.bleedMult);
		ar(self.pierceMult);
		ar(self.cut1);
		ar(self.blunt1);
		ar(self.humanMult);
		ar(self.animalMult);
		ar(self.robotMult);
		ar(self.armourPenetration);
		ar(self.length);
		ar(self.weight);
		ar(self.attackMod);
		ar(self.defenceMod);
		// Save v85: the weapon's indoors mod (task indoors). An older save loads 0, i.e. no mod until the weapon is re-equipped.
		if (ar.version >= 85)
			ar(self.indoorsMod);
		ar(self.quality);
	}
};

// A carried crossbow's numbers (CROSSBOW record at item level L, dt41_A `GunClass::loadRangedStats`) and its firing state. `sid` empty =
// no ranged weapon. Save v96, additive: an older save loads nobody armed with one.
struct FighterRanged
{
	std::string sid; // the CROSSBOW record's stringID
	double range = 0.0; // `range` (stats +0x08): the shot's reach before the height factor
	double deviation = 0.0; // `accuracy deviation at 0 skill` (+0x10), degrees of random offset at skill 0
	double perfectSkill = 100.0; // `accuracy perfect skill` (+0x0c): the Crossbows skill with no deviation
	double aimSpeed = 1.0; // `aim speed` (+0x14)
	double shotSpeed = 1.0; // `shot speed` (+0x18; the shot is instant, kept for the client's later bolt)
	std::int32_t numShots = 1; // `num shots` (+0x1c): bolts per shot
	double reloadMin = 1.0; // `reload time min` (+0x24): seconds at skill 100
	double reloadMax = 5.0; // `reload time max` (+0x28): seconds at skill 0
	double pierceMin = 0.0; // `pierce damage min` at the item level
	double pierceMax = 0.0;
	double bleedMult = 1.0;
	double humanMult = 1.0;
	double robotMult = 1.0;
	std::int32_t ammo = 0; // bolts carried (UNSURE source: the decode has no count, FEATURES "Crossbows")
	double timer = 0.0; // gun+0x70: seconds until the next shot (a reload, or the 0.3 s retry); fires at <= 0

	bool armed() const noexcept { return !sid.empty(); }

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.sid);
		ar(self.range);
		ar(self.deviation);
		ar(self.perfectSkill);
		ar(self.aimSpeed);
		ar(self.shotSpeed);
		ar(self.numShots);
		ar(self.reloadMin);
		ar(self.reloadMax);
		ar(self.pierceMin);
		ar(self.pierceMax);
		ar(self.bleedMult);
		ar(self.humanMult);
		ar(self.robotMult);
		ar(self.ammo);
		ar(self.timer);
	}
};

// A gunner at a turret (task ds/turret S3, dt41_B): the building it mans, the turret's post and facing, and the turret's own gun
// (its GUN_DATA numbers; the gunner's shot reads these, not `Fighter::ranged`). `building` 0 = not mounted. Save v97, additive.
struct FighterMount
{
	std::uint64_t building = 0; // the turret building's key
	double x = 0.0; // the operator's post (the turret's use node)
	double z = 0.0;
	double yaw = 0.0; // the turret's facing: the middle of its firing arc (OURS, UNSURE)
	FighterRanged gun; // `armed()` once mounted; its timer is the turret's cycle

	bool mounted() const noexcept { return building != 0; }

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.building);
		ar(self.x);
		ar(self.z);
		ar(self.yaw);
		ar(self.gun);
	}
};

// A stealth KO under way (task ds/stealth-ko S4, dt41_C Task_StealthKO): the target, and the task's state (`task+0x78`): 0 approach, 1 the
// "knockout" clip plays, 2 the clip's tail holds. `target` 0 = none. Save v98, additive.
struct FighterStealthKo
{
	std::uint64_t target = 0;
	std::uint8_t phase = 0;
	double elapsed = 0.0; // seconds in the current phase

	bool active() const noexcept { return target != 0; }

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.phase);
		ar(self.elapsed);
	}
};

// One attack-slot entry, in the table of the victim (the owner of the Fighter).
struct FighterSlot
{
	std::uint64_t holder = 0; // state::Id of the attacker; 0 = free
	double timer = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.holder);
		ar(self.timer);
	}
};

// One waypoint of a route (the navmesh surface point; y is the surface height there).
struct RoutePoint
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.x);
		ar(self.y);
		ar(self.z);
	}
};

// An individual goal (DIALOGUE.md 5, step 6b): a dialogue effect's order that the routines and the squad AI leave alone until it ends
// (Simulation::stepIndividualGoals). Each one ends by itself (target gone, arrived, timer), so it is never a terminal state.
enum class IndividualKind : std::uint8_t
{
	None = 0,
	Flee = 1, // task 280 RUN_AWAY_FORCED (DA_RUN_AWAY): runs from `target`, takes no fight
	GoHome = 2, // task 19 GO_HOMEBUILDING (DA_GO_HOME): walks to the squad's home building, else its home town
	Summoned = 3, // task 67 MOVE_ON_FREE_WILL_FAST (DA_SUMMON_MY_SQUAD): runs to `target`, the summoner
	RunAway = 4, // goal 32 SELF_PRESERVATION / task 35 RUN_AWAY: an attacked non-fighter runs from `target` until nobody is aggressing against it
};

struct IndividualGoal
{
	IndividualKind kind = IndividualKind::None;
	std::uint64_t target = 0; // state::Id: the one fled from, or the summoner
	std::uint64_t until = 0; // tick the goal ends at the latest
	double x = 0.0; // GoHome: where home is
	double z = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.kind);
		ar(self.target);
		ar(self.until);
		ar(self.x);
		ar(self.z);
	}
};

// Direct drive (SIMLINK.md section 12), Fighter::drive.
enum class DriveMode : std::uint8_t
{
	Stop = 0,
	Forward = 1,
	Backward = 2,
};

// One observer's record of one other character (perception.hpp, PERCEPTION_LIVE.md items 17 and 18): the original's sensor
// record (known / sighted flags and the memory timer) with the awareness map's spot (sneak meter) folded in.
struct SightRecord
{
	std::uint64_t target = 0; // state::Id
	bool seen = false; // known or sighted within the memory (30 s, 60 in combat): the observer is aware of the target
	double sinceSeen = 0.0; // seconds since the last scan that knew or sighted it
	// Seconds since a scan itself sensed it (known or sighted). Unlike sinceSeen, an attacker's onTargetedBy `notice` never
	// resets it: the chase give-up monitor (s73 section 3) asks whether the target is sensed, not whether it is remembered.
	double sinceSensed = 1.0e9;
	bool sighted = false; // the last noise roll's verdict (cached between the 2 s throttled rolls)
	double sinceRoll = 1.0e9; // seconds since the last noise roll
	std::uint32_t rolls = 0; // noise rolls made (the deterministic dice index)
	double meter = 0.0; // the sneak meter (Spot value); notice at > 1
	double fill = 0.0; // Spot fillTime of the last scan with the accumulator active
	bool active = false; // Spot active: the meter fills; clear: it drains by dt / 260
	std::uint32_t state = 0; // 0 none, 2 being noticed (the meter fills), 1 noticed (aware of it)
	// Seconds since perception::notice (the original's force-notice, 140858f40) wrote this record. Derived, never saved: the wire's
	// DETECTED_BY reads it to tell a told-about friendly from a sighted one.
	double sinceNoticed = 1.0e9;
	// The exe's SensorRecord seen_now (+0x38): the last scan KNEW the target (sight, or the sneak meter past 1.0) or perception::notice
	// forced it; a noise-roll "heard" (sighted) never sets it, and the first scan with neither clears it. Derived, not saved (one scan
	// after a load it reads false). The stealth KO's abort and SEEN_BY read it (rwh.152).
	bool knownNow = false;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.seen);
		ar(self.sinceSeen);
		ar(self.sinceSensed);
		ar(self.sighted);
		ar(self.sinceRoll);
		ar(self.rolls);
		ar(self.meter);
		ar(self.fill);
		ar(self.active);
		ar(self.state);
	}
};

inline constexpr std::size_t kMetCap = 32; // dialogue-memory entries kept per character; the oldest sighting is evicted first

// One entry of a character's dialogue memory (the exe's squad memory, DIALOGUE_CONDITIONS_STATIC 18 and 20): a character it has seen
// and the sim tick it last saw it. DC_TARGET_LAST_SEEN_X_HOURS_AGO reads the age, DC_MET_TARGET_BEFORE the presence.
struct MetRecord
{
	std::uint64_t target = 0; // state::Id
	std::uint64_t tick = 0; // the sim tick of the last sighting

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.target);
		ar(self.tick);
	}
};

// One character-creator slider value (an editor Range of the race's XML, by name).
struct LookSlider
{
	std::string name;
	double value = 0.0;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.name);
		ar(self.value);
	}
};

// The look a character creator gave a new-game character (NEW_GAME n > 0, SIMLINK.md; save v42). `custom` false = the character has no
// creator look (every spawned or template character): CHAR_APPEARANCE keeps its scenario look. The name is Fighter::name, the race
// Fighter::raceSid.
struct FighterLook
{
	bool custom = false;
	std::uint8_t female = 0;
	std::uint16_t headIndex = 0; // into the RACE's `heads male` / `heads female` (CHAR_APPEARANCE headIndex)
	std::string hair; // ATTACHMENT sid, "" = none
	std::string beard;
	std::string hairColour; // COLOR_DATA sid
	double skin = 0.0; // 0..1
	std::vector<LookSlider> sliders; // as the creator sent them (the sliders it left out are at the race's mids)
	std::int32_t stance = 0; // the Idle stance slider, 1..N (the editor's idle ANIMATION list)

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.custom);
		ar(self.female);
		ar(self.headIndex);
		ar(self.hair);
		ar(self.beard);
		ar(self.hairColour);
		ar(self.skin);
		ar(self.sliders);
		ar(self.stance);
	}
};

// The recovery hold's steer bookkeeping (CombatClass +0x1f4 and CharMovement +0x378; CombatArena::Member's hold
// before save v66): the opponent's pending flag, whether either side was swinging last tick, whether the hold is
// running, the steer mode, and the last of Attack/Defend seen before the hold (its entry rule).
struct HoldSteer
{
	bool pending = false;
	bool oppSwing = false;
	bool ownSwing = false;
	bool inHold = false;
	bool steerOn = true;
	CombatState came = CombatState::Hub;

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.pending);
		ar(self.oppSwing);
		ar(self.ownSwing);
		ar(self.inHold);
		ar(self.steerOn);
		ar(self.came);
	}
};

// The character layer's per-character state.
struct Fighter
{
	static constexpr std::string_view kName = "Fighter";

	// Identity and sides.
	std::string raceSid; // RACE record
	std::int32_t team = 0; // characters of different teams are enemies
	bool blockMode = false; // CharStats+0x128 (the player's "defensive" mode)
	bool passive = false; // CharStats+0x12c: PROTECT_ALLIES is utility 0 (rwh.148 3a); the rest of the AI runs. Saved, v112
	bool holdPosition = false; // the HOLD POSITION toggle: joins an ally's fight but never leaves the anchor: crossbow in reach, melee in close range only (rwh.148 3b, rwh.179; UNSURE, not measured as the toggle). Saved, v112
	bool fought = false; // had a target at least once (Simulation::stepRearm): only such a member stands down when the squad is quiet (k16.6.361). Saved, v118

	// Kinematics (the hot part). yaw in radians, 0 = +z, increasing towards +x.
	double x = 0.0;
	double z = 0.0;
	double y = 0.0; // the navmesh surface height; meaningful only with a navmesh (the sim sets it)
	double yaw = 0.0;
	double aimYaw = 0.0; // the technique's aim (the look-at, mover+0x250), set at its start; the body turns onto it at kTurnRateCombat (save v67)
	double forwardSpeed = 0.0; // units/s along the heading
	double lateralSpeed = 0.0; // units/s to the right
	// walkOrder's commanded heading and the yaw it wrote with it (saved from v109): the walk's velocity memory stays on the commanded heading while the
	// body faces the net displacement; any other turn of the body (yaw != walkFacing) hands the memory back to the yaw.
	double walkHeading = 0.0;
	double walkFacing = 0.0;
	// The actual velocity of the last step (position change / dt, world x and z): what the follow task reads
	// of a mover (SQUAD_FOLLOW.md s4), unlike forwardSpeed it follows a slide, a block or a turn.
	double velX = 0.0;
	double velZ = 0.0;

	// CharStats.
	FighterSkills skills;
	double raceStrength = 1.0;
	double raceDexterity = 1.0;
	double attackInt = 0.0; // CharStats+0x20: integer bonus to the melee attack value
	double defenceInt = 0.0; // +0x24
	double unarmedInt = 0.0; // +0x2c
	double healFactor = 1.0; // StatsBlock+0x58 (UNSURE source)
	double bloodFactor = 1.0; // StatsBlock+0x5c
	double encumbrance = 1.0; // CharStats+0x190, 1 = unencumbered
	FighterWeapon weapon;
	FighterRanged ranged; // the carried crossbow (save v96); `weapon` stays the melee one
	FighterStealthKo stealthKo; // the stealth KO it is carrying out (save v98)
	FighterMount mount; // the turret this fighter mans (save v97); while mounted its gun replaces `ranged`
	// The gun a shot reads: the turret's while mounted, else the carried crossbow.
	FighterRanged& gun() noexcept { return mount.mounted() ? mount.gun : ranged; }
	const FighterRanged& gun() const noexcept { return mount.mounted() ? mount.gun : ranged; }
	std::vector<FighterArmour> armour;
	// The CHARACTER's `inventory` list (item sid, count) a spawn carries: adoptFighter moves it into the new pack and clears it. Not
	// saved: a loaded fighter's pack is the saved one (k16.6.287).
	std::vector<std::pair<std::string, std::int32_t>> spawnPack;
	// The CONTAINER sid of the backpack the spawn wears (the CHARACTER's `backpack` list); adoptFighter puts it in the BACKPACK slot and clears it.
	std::string spawnBackpack;

	// MedicalSystem.
	// The arena's pair-distance cache entries this fighter is the lower id of (CombatArena::PathSample, saved v114, k16.8.50): written into the
	// save by the mirror and read back by CombatArena::add, empty in a running arena. A cache a load dropped made the decisions that read it
	// (unreachableNear, the Hub's floor test) differ from the unbroken run; `ageTicks` is arena steps since the sample (whole steps: a seconds age rebased on a new clock rounds differently from the unbroken run's).
	struct PathSample
	{
		std::uint64_t other = 0;
		double length = 0.0;
		double ax = 0.0;
		double ay = 0.0;
		double az = 0.0;
		double bx = 0.0;
		double by = 0.0;
		double bz = 0.0;
		std::uint64_t ageTicks = 0;

		template <typename Archive, typename Self>
		static void fields(Archive& ar, Self& self)
		{
			ar(self.other);
			ar(self.length);
			ar(self.ax);
			ar(self.ay);
			ar(self.az);
			ar(self.bx);
			ar(self.by);
			ar(self.bz);
			ar(self.ageTicks);
		}
	};
	std::vector<PathSample> pathSamples;
	std::vector<FighterPart> parts;
	double blood = 150.0;
	double minBlood = 75.0;
	double maxBloodRace = 150.0;
	std::vector<double> wounds; // bleeding wound rates
	double koTimer = 0.0; // +0xa0
	bool unconscious = false; // +0x161
	bool koLatch = false; // +0x162: a vital part went below the KO point
	bool dead = false; // +0x164
	bool rightArmOk = true; // +0x165
	bool leftArmOk = true; // +0x166
	bool down = false; // a leg or torso part collapsed (ragdoll)
	// The ragdoll's rest pose (task BG, PHYSICS.md 2-3; save v76): while the body is prone (CHAR_DYNAMIC flag bit 5: down,
	// unconscious or dead) it lies at `restYaw`, the head-to-feet direction in the character yaw convention (toward
	// (sin yaw, cos yaw), 0 = +Z). Rolled once per ragdoll start from the fighter's own stream (CombatArena::rollRagdoll,
	// kRagdollStream); `restRolls` is the draw count, so a launch or a save/load continues the same sequence. False once the
	// body is up again; the next prone start rolls a new yaw.
	double restYaw = 0.0;
	std::uint32_t restRolls = 0;
	bool restYawValid = false;
	// med+0x60: 3.0 full (UI x100); the stat penalty starts below 2.0 (getEffectiveStat). Constant for
	// now: the decay over time is not measured (STATUS TODO). Scenarios seed it.
	double hunger = 3.0;
	double hungerRate = 1.0; // RACE `hunger rate` (R)
	bool hungerKo = false; // the forced KO below 0.1 has been started (MEDICAL_HUNGER_LIVE.md a)
	double bleeding = 0.0; // blood lost per second, as of the last medical update (event 2 bleed drips)
	double foodCharges = 0.0; // charges left of the food item being eaten
	double eatTimer = 0.0; // seconds to the next bite
	std::string kitSid; // the first aid kit in use (ITEM record), "" none
	double kitCharges = 0.0; // charges left of that kit

	// CombatClass.
	CombatState state = CombatState::Hub;
	CombatState pending = CombatState::Hub; // +0x1f4
	std::int32_t technique = -1; // +0x150: index into CombatData::techniques, -1 none
	double clipTime = 0.0; // clip time of the running technique, seconds
	double clipSpeed = 1.0; // play speed it was started at
	bool techniqueFinished = false; // +0x158
	std::int32_t impactIndex = 0; // +0x268
	std::int32_t lastImpact = 0; // +0x26c
	bool hitListBuilt = false; // +0x1d4
	// The swing's hit list (+0x1d8: the fighters the impacts already passed have hit) and the recovery hold's steer
	// flags. Cleared at every swing start / re-entered at every hold, but per-character state all the same: the save
	// carries them (v66), so a load mid-swing or mid-hold takes the same decisions (ksim_combat_sim).
	std::vector<std::uint64_t> hitList;
	HoldSteer holdSteer;
	bool recovering = false; // +0x144
	double recoverTimer = 0.0; // +0x148
	double actionTimer = 0.0; // +0x14c
	double swingProgress = 0.0; // +0x140
	double reactTimer = 0.0; // +0x274
	double stateTime = 0.0; // seconds in the current state
	double stumbleTimer = 0.0; // seconds left of the playing hit-reaction clip (state 8 ends at 0)
	double escapeTimer = 0.0; // +0x124: 1.2 s from the entry into state 8; the escape roll fires once it is < 0
	bool escapeAsked = false; // +0x120: set by decideEngagement while in state 8, cleared at the entry
	std::int32_t reactClip = -1; // CombatData clip index of the playing hit reaction (root motion pushes the defender), -1 none
	double reactTime = 0.0; // seconds into reactClip
	double recoilX = 0.0; // unit vector the hit reaction pushes along (away from the attacker); (0,0) = none
	double recoilZ = 0.0;
	double circleTimer = 0.0; // Circle/Waiting `timer`
	double circleOffset = 0.0; // Circle/Waiting `offset`
	std::int32_t previousTechnique = -1;
	// CharStats+0x150: the longest reach among the techniques the last pick admitted, L + half. -1 =
	// not computed yet; computed lazily by the weapon-change pick (CombatRules::weaponChangedReach).
	double cachedReach = -1.0;
	std::uint64_t target = 0; // state::Id; 0 = none
	bool strolling = false; // on a non-urgent routine goal (Simulation::step sets it every tick from the Routine): the gait walks, whatever the speed mode; transient, never saved
	std::uint16_t rangedTask = 0; // the ranged task (262 / 263 / 266) the last combat step ran, 0 = melee; transient, never saved
	double targetAge = 0.0; // +0x2f0
	// The AI_TASK goal that started the current fight (task AT, save v74; the Goal row shows it): 5 a player's attack order, 32 self
	// preservation, 16 / 15 / 143 / 34 / 21 the join goals; 0 = unknown (a load, a target set outside a goal). Set where the target is
	// picked, cleared with it.
	std::uint16_t combatGoal = 0;
	std::vector<FighterSlot> slots; // attack slots owned by this fighter (as a victim)
	std::vector<std::uint64_t> engaged; // +0x1f8 engaged attackers
	std::uint32_t overflow = 0; // +0x210 / +0x214 folded: the refusals of the previous step (crowd_melee.md §10), written by the arena's step
	std::uint64_t rng = 0;
	// The character's stable dice identity (task CC, save v81): the key every combat stream of this fighter hangs on (the combat rng, the
	// comfort radii, the ragdoll roll, the follow formation), derived from what the character is and where it spawned, never from its
	// global Id. 0 = none (a bare arena, an older save, a test that adds fighters by hand): the arena then keys on the Id, as before.
	std::uint64_t diceSeed = 0;
	// Melee task starts so far (a count kept in the old reserved double, so the save layout is unchanged): the comfort radii re-roll at every
	// start of a melee task, i.e. whenever the target changes (CombatArena::comfort(id, rolls); kinspect BQ12 a).
	double comfortRolls = 0.0;

	// Player orders (SIMLINK.md, save v10). Set only by commands (command.hpp).
	bool playerControlled = false; // flags bit 6 on the wire; no behavioural effect itself
	// A move order's route on the navmesh (SIMLINK.md section 12: the server routes, never a
	// straight line). Walking = routeNext < route.size(). The first point is where the order was
	// given. routeVersion bumps whenever the route is set or cleared (the wire's CHAR_ROUTE).
	std::vector<RoutePoint> route;
	std::uint32_t routeNext = 0;
	std::uint32_t routeVersion = 0;
	std::uint32_t routeSeq = 0; // clientSeq of the order that made the route
	bool routeApproach = false; // the route ends at an approach point, not the clicked spot (CHAR_ROUTE flags bit 1)
	// Direct drive: sticky state, heading (radians, 0 = +z, towards +x), clientSeq of the last
	// applied drive order. A driven character is in the AI-off hold (`hold`).
	DriveMode drive = DriveMode::Stop;
	double driveHeading = 0.0;
	std::uint32_t driveSeq = 0;
	std::int8_t driveStrafe = 0; // -1 left, 0 none, +1 right of the heading (order 20's trailing strafe i8; saved, v111)
	bool driveBlocked = false; // the last driven step was stopped by a wall or edge
	// Turn in place (SIMLINK.md section 12): a drive 0 in the drive hold turns the body towards driveHeading with no translation,
	// until it faces it.
	bool driveTurn = false;
	// Seconds a walking character has barely moved (a move order or a route): 5 s re-paths (the original, s70 section 4).
	double walkStuck = 0.0;
	// Seconds the walk has been off its route (the exe's timeOffPath); 5 s re-requests the path. Not saved: a loaded walk starts counting again.
	double offPath = 0.0;
	std::uint64_t orderTarget = 0; // attack order: pickTarget keeps this enemy until it is down
	// Task dt25 (45 BODYGUARD): the protected hand, the original's AITaskSystem+0x10 - the character a bodyguard order (a
	// player's 45 job, or a package's bodyguard row on a squad) put under this character's guard. The combat picker adds +10
	// toward a candidate that is fighting it (combat.cpp scanCandidates). Derived, never saved: the job or the package's row
	// sets it again on the next pass, as the original re-syncs the hand on every goal selection.
	std::uint64_t protectTarget = 0;
	bool hold = false; // stand still: no chasing or footwork; fights what comes
	bool orderStop = false; // one-shot: drop the current target at the next update
	// One action at a time (kenshi-k16.6.335, rwh.165). Derived each tick by Simulation::stepOrderCombat, never saved: a player's interact / lock / lift
	// order is running (the combat tick then picks no target: the original runs the combat AI only inside the melee tasks); `orderInterrupt` is set when
	// the body is targeted by a new attacker or hit while not in combat (onTargetedBy -> requestReplan) and consumed by the same step, which stops the
	// task for `orderPause` ticks and lets the order start again from its beginning.
	bool orderBusy = false;
	bool orderInterrupt = false;
	std::uint8_t orderPause = 0;

	// Reroutes spent on the current walk (a blocked move order, or consecutive blocked steps of a chase).
	std::uint32_t routeTries = 0;
	// Attack chase (SIMLINK.md section 12): the route above was made by the chase, not an order
	// (routeSeq 0, routeApproach set), and is followed from the combat steering, not walkOrder.
	bool routeChase = false;
	double chaseTx = 0.0; // where the target was when the chase route was planned
	double chaseTz = 0.0;
	double chasePlanAge = 0.0; // seconds since that plan
	double chaseCooldown = 0.0; // stands still this long after the retries ran out
	bool chaseDirect = true; // hysteresis flag: steering straight at the target (below 50, until 100); starts set like the follow task

	// Who set the current plan (task fight-goals): a squad plan (the world carry's route and follow, a world task's walk) or an order (a
	// command). Only a Plan may be preempted by a package fight goal in the goal selector; an order outscores the package tiers (dt14's
	// persistent-order pass). Transient: the writers mark it, a clear resets it, and a route loaded from a save reads None until its
	// writer runs again.
	enum class PlanSource : std::uint8_t
	{
		None,
		Plan,
		Order,
	};
	PlanSource routeSource = PlanSource::None;
	PlanSource followSource = PlanSource::None;
	// Seconds to the next fight-goal re-selection (task fight-goals; kinspect patrol_aggro_town.md a8a41b7 section 3: the exe's per-AI
	// throttle, a random phase U(1,40)/60 s re-rolled at every full update). Transient: a load starts it at 0 (select on the next tick).
	double fightGoalClock = 0.0;

	// Sneak mode (Character+0xd4, SQUAD_FOLLOW.md s9): a sneaking character does not join an ally's fight, a follower
	// copies its leader's, and a hostile that targets it clears it (`clearSneak`, the exe's onTargetedBy).
	bool sneak = false;
	// Speed mode (order 10 `which` 5, SIMLINK.md; the exe's CharMovement cap +0xbc): 0 Walk (the race's walk speed), 1 Jog
	// (55), 2 Run (none, the default), 3 Group (every member capped at the slowest live, conscious member's own run speed).
	// The order never replaces a move, a chase or a follow; it caps their speed. `which` 2 (the old run toggle) is an alias:
	// 1 = Run, 0 = Walk.
	std::uint8_t moveMode = 2;
	// The Group mode's group key (the lowest state::Id of the order's ids; 0 = no group). Another mode leaves the group.
	std::uint64_t speedGroup = 0;
	// The display name: the CHARACTER record's name (a unique's own, e.g. "Beep"), set by makeFighter. The sim has no random
	// name generator yet: a random spawn carries its record's name (OPEN). Scenarios may overwrite it.
	std::string name;
	// The follow task (order 3, SQUAD_FOLLOW.md s4): 0 = none. The route of a follower in path mode rides
	// `route` with routeChase, like a chase. followHx/Hz is the task's smoothed heading (+0xa0; (0,0) = fresh
	// task), followDirect the +0xac flag (starts set), followAge the seconds since the task started and
	// followUnseen how long the leader has been out of sight, with the last place it was seen (monitor).
	std::uint64_t followTarget = 0;
	double followHx = 0.0;
	double followHz = 0.0;
	bool followDirect = true;
	double followAge = 0.0;
	double followUnseen = 0.0;
	double followSeenX = 0.0;
	double followSeenZ = 0.0;

	// Perception (perception.hpp). The inputs the sensor reads of a character: whether it is an animal (immune to the weather
	// of its sight range: only CharacterHuman writes the weather state), the building it is inside (0 outdoors; set at
	// spawn from its position, Simulation::buildingAt, then by crossing a building's door, Simulation::stepBuildings; a town
	// gate is never a building), crouched stance (separate from `sneak`; no order sets it yet), asleep (cone 0; no
	// sleep yet), and the observer's attention (the squad AI package float, 0.5 when it has none; the data has no field
	// for it, UNSURE).
	bool animal = false;
	// An animal's CHARACTER `attack type` (CombatData::makeFighter; task BQ, save v79): the WeaponCategory that selects its
	// COMBAT_TECHNIQUE family (the beak's is 20 under rebirth.mod, matching its two `giraffe type attack` records). 0 for a
	// human, and for an animal loaded from a v78 save (it picks no animal technique until it is respawned).
	std::int32_t attackType = 0;
	std::uint64_t building = 0;
	// The floor of `building` the character stands on (CHAR_DYNAMIC `floor`, SIMLINK.md section 6): 0 on the ground, 1 the
	// first upper floor. Derived from the position and the building's floor surfaces (Simulation::stepBuildings,
	// BuildingSite::floorAt), not saved: the first step after a load fills it again.
	std::int16_t floor = 0;
	// Under a ceiling (the exe's char+0xd5, indoors_state.md section 1; CHAR_DYNAMIC flag 19): in a building's interior AND a
	// surface above this floor (BuildingSite::indoorsAt). A roof / top deck inside the footprint is outdoors. Set with `building`
	// and `floor` by Simulation::setLocation; saved (v85).
	bool indoors = false;
	// The building is `cavernous` (its BUILDING record): the weapon `indoors mod` is skipped there (indoors_state.md section 2).
	// Derived with `indoors`, not saved: the next placement step fills it again.
	bool cavernous = false;
	// Under the roof of an `is shelter` building while exposed (the exe's second acid-rain gate: a down ray finds a shelter
	// building's roof more than 10 u overhead, weather_acid_rain.md section 5). Derived with `indoors`, not saved.
	bool sheltered = false;
	// Carrying (carry.hpp): the character this one carries (state::CarryStep has the phase) and the one carrying this one. A
	// carried character moves with its carrier, is not a target and is in no building.
	std::uint64_t carrying = 0;
	std::uint64_t carriedBy = 0;
	bool crouched = false;
	bool asleep = false;
	double attention = 0.5;
	// Crime (crime.hpp). `npcClass` is the CHARACTER's `NPC class` (CharacterTypeEnum, AI+0xDC: 1 law enforcement, 2 military, 3 trader,
	// 4 civilian, 5 diplomat, 6 slave, 7 slaver, 8 bandit, 9 adventurer; 0 = none); a player-controlled character counts as 9 whatever it
	// says (crime::effectiveClass). `homeBuilding` is the building (BuildingSite::key) the resident's squad calls home, 0 = none
	// (ownerships+0x38; set for a non-player character that starts inside a building, Simulation::spawnFighter / placeInBuildings; UNSURE: the squad's real home is not on the Fighter). `houseLocked` is the squad
	// package byte +0xe8 of the trespass sensor (TRESPASS_LIVE.md): "we closed the house up ourselves". Derived state on the squad's
	// home, to be set by the residents' own close/lock door tasks (only if the home is a building and none of its doors is open or
	// opening) and cleared by any open / unlock / pick-lock of a home door, or when no closed-and-unlocked home door is left. The
	// sim has no resident close/lock tasks yet, so it is an explicit input that defaults to off. No time-of-day rule.
	std::uint8_t npcClass = 0;
	std::uint64_t homeBuilding = 0;
	bool houseLocked = false;
	// The portrait camera serial (SIMLINK.md CHAR_APPEARANCE portraitSerial): rolled once at spawn, uniform in [0, 4e9) like the
	// original's rand x 4e9 (1406223f0), from the character's own pure stream (kPortraitStream). Part of the appearance; never changes.
	std::uint32_t portraitSerial = 0;
	// The creator's look (NEW_GAME with Specs); `look.custom` false for everyone else.
	FighterLook look;
	// The sensor: seconds since its last scan (negative = not staggered yet), and its records sorted by target.
	double senseClock = -1.0;
	std::vector<SightRecord> sight;
	// The chase give-up monitor (s73_static.md section 3; Task_Move chaseGiveUpMonitor 14033a800): the target it watches (0 = none),
	// sim seconds since this chase began, sim seconds the target has gone unsensed, and the sight record's position of it (it follows
	// the target's true position while the record is younger than 25 U outdoors / 8 U in a town, then stays).
	std::uint64_t chaseId = 0;
	double chaseAge = 0.0;
	double chaseUnseen = 0.0;
	double chaseX = 0.0;
	double chaseZ = 0.0;
	// MQ14 path 2: the squad goal ATTACK_ENEMIES_AND_NEUTRALS (routines::task 21). `faction` is the owning FACTION's state::Id (0 = none),
	// the key of the neutral test (CombatArena::bonded); `attacksNeutrals` makes pickTarget take every non-bonded fighter as a target. When
	// the chase monitor dropped a target after 66 game minutes (aggroBlockId) that target is skipped for aggroBlock more sim seconds, so a target it cannot reach is not an
	// endless chase.
	std::uint64_t faction = 0;
	bool attacksNeutrals = false;
	std::uint64_t aggroBlockId = 0;
	double aggroBlock = 0.0;
	// BQ10 (kinspect bq10_goal21.md, save v53). `attacksNeutrals` is per character, from its own role list (leader / squad / squad2 / slave) of
	// the squad's one active package; the goal's hour window [aggroStart, aggroEnd) is checked on every evaluation (start == end: always open).
	// `squad` is the squad's key (the SquadInstance's state::Id; a squadless fighter gets 1<<62 | its id in CombatArena::add), `uniqueSquad`
	// marks a UNIQUE_SQUAD_TEMPLATE squad (goal 21 never targets it). `hostileSquads` is the squad's personal type-4 list (sorted squad keys):
	// onTargetedBy adds the attacker's squad to every member of the victim's squad, and the list is read before the faction relation.
	std::int32_t aggroStart = 0;
	std::int32_t aggroEnd = 24;
	std::uint64_t squad = 0;
	bool uniqueSquad = false;
	std::vector<std::uint64_t> hostileSquads;
	// Task BZ (dt23, save v78): the same personal map's AGGRESSOR bit (2) - the squads whose members attacked mine. onTargetedBy runs on the
	// victim, so the attack writes the ATTACKER's squad into the victim's squad table; goal 34 reads it as "already fighting me". Sorted squad
	// keys, same lifetime as hostileSquads (dt23: the stamp is saved and never read, so no expiry). Our separate list stands for the exe's one
	// map with bits 1..7 (the type-4 list above is the other bit we model).
	std::vector<std::uint64_t> aggressorSquads;
	// DIALOGUE.md 5 step 6b (save v61): the running individual goal, kind None = none.
	IndividualGoal goal;
	// Captivity (TOWN_LIFE_SLICE.md 5.19, save v68). `slaveState` is the original's slave-state enum (CRIME_NOTICE.md): 0 not a slave,
	// 1 a slave, 2 an escaping slave, 3 an ex-slave. `captorSid` is the faction that chained it, written only when empty (TQ22's
	// default: a recaptured escapee keeps its first owner). `cageBuilding`/`cageSlot` place it in a cage (the original's location type
	// 2: the cage site of that building, slot order), 0 = not caged. Written by the slave chain (carry.cpp stepSlaveTrade): the catcher
	// sets state 1 and the owner at the cuff, then the cage. Freed by DA_REMOVE_SLAVE_STATUS (Simulation::removeSlaveStatus, reached
	// through Grey's line 59278 since task AY); nothing sets states 2 or 3 yet.
	std::uint8_t slaveState = 0;
	std::string captorSid;
	// Save v107 (rwh.106): the world-clock stamp of the last change into state 2 or 3, in game hours (the exe's CharStatus+0x08, the save's `ssct`).
	// 2 -> 3 fires 12.0 h after it (and re-stamps), 3 -> 0 96.0 h after it. -1 = not stamped yet (a state set around the setter, a pre-v107
	// save): the slave step stamps it at the first look.
	double slaveStamp = -1.0;
	// Task BP (derived, never saved): `captorSid` as a faction entity id, resolved by the Simulation every tick, so combat's enemy test
	// can see whose captive a shackled slave is without the ecs (its captor and its captor's allies do not attack it).
	std::uint64_t captorFaction = 0;
	// kenshi-k16.6.116 (derived, never saved): the slave state is 1 and the character carries its own job row 187 Obedient Slave (rwh.99 section 5),
	// kept by the Simulation (jobs.cpp syncObedient). The row gates PROTECT_ALLIES (utility 0) and the same-owner shield among slaves.
	bool obedient = false;
	std::uint64_t cageBuilding = 0;
	std::uint16_t cageSlot = 0;
	// The captive-in-transit flag (the exe's status +0xE9, save v91): set when a non-slave is lifted, cleared when it is released. The carry
	// inspection (crime.cpp) reads it, and the put-down writes crime 15 for it (CARRY_KIDNAP_LIVE.md). The exe sets it only in
	// Task_Kidnapping (246); ours sets it on every lift of a non-slave (the user's fix: a plain carry is observable).
	bool captive = false;

	// The dialogue memory (save v75, DIALOGUE_CONDITIONS_STATIC 18 / 20): who this character has seen and when, for
	// DC_TARGET_LAST_SEEN_X_HOURS_AGO and DC_MET_TARGET_BEFORE. Per character, not per squad as the exe's (its memories hang off the
	// squad, UNSURE: a recruit shares nothing here); bounded to the kMetCap most recent sightings, oldest evicted first, so a save
	// stays small and the conditions, which ask about hours and weeks, keep the targets that matter. Written by the perception scan
	// (perception::scan) and DA_REMEMBER_CHARACTER. Ascending by tick.
	std::vector<MetRecord> met;

	// The tick `target` was last seen, false when this character has no entry for it (never seen, or evicted).
	bool metBefore(std::uint64_t target, std::uint64_t& tick) const noexcept
	{
		const auto at = std::find_if(met.begin(), met.end(), [target](const MetRecord& entry) { return entry.target == target; });
		if (at == met.end())
			return false;
		tick = at->tick;
		return true;
	}
	// Records a sighting of `target` at `tick` (kept ascending: the erase and the append both move it to the newest place).
	void rememberMet(std::uint64_t target, std::uint64_t tick)
	{
		const auto at = std::find_if(met.begin(), met.end(), [target](const MetRecord& entry) { return entry.target == target; });
		if (at != met.end())
		{
			if (at->tick == tick)
				return;
			met.erase(at);
		}
		if (met.size() >= kMetCap)
			met.erase(met.begin());
		met.push_back(MetRecord{target, tick});
	}

	// A running player task that blocks PROTECT_ALLIES (measured: following or a move order).
	// Direct drive under way: a drive state, or a pure sidestep (drive 0 with a strafe).
	bool driving() const noexcept { return drive != DriveMode::Stop || driveStrafe != 0; }
	bool taskRunning() const noexcept { return followTarget != 0 || routeNext < route.size() || driving(); }

	// A move order walks its route; a chase route is followed by the steering.
	bool walking() const noexcept { return routeNext < route.size() && !routeChase; }
	// Why the route was last cleared or replanned (CHAR_ROUTE diag tail `clearedWhy`, SIMLINK.md section 6). Transient, not saved. 0 = none yet.
	// 16..26 are route-lifecycle events; a failure's own code (AiReason numbers 4..11) may also land here. Held until the next event.
	enum RouteWhy : std::uint8_t
	{
		kReplanOffPath = 16,
		kReplanBlocked = 17,
		kReplanChase = 18,
		kClearArrived = 19,
		kClearWayShut = 20,
		kClearGiveUp = 21,
		kClearNewOrder = 22,
		kClearDoorStep = 23,
		kClearTaskEnded = 24, // the default for a clearRoute() with no reason
		kClearIncapacitated = 25,
		kClearWentDirect = 26,
	};
	std::uint8_t clearWhy = 0;
	// Drops the route, bumping routeVersion only if there was one (the wire sends the clear). `why` is kept only for a real clear (an
	// empty route being cleared again must not overwrite the event that emptied it).
	void clearRoute(std::uint8_t why = kClearTaskEnded) noexcept
	{
		if (!route.empty())
		{
			++routeVersion;
			clearWhy = why;
		}
		route.clear();
		routeNext = 0;
		offPath = 0.0;
		routeApproach = false;
		routeChase = false;
		routeSource = PlanSource::None;
	}

	template <typename Archive, typename Self>
	static void fields(Archive& ar, Self& self)
	{
		ar(self.raceSid);
		ar(self.team);
		ar(self.blockMode);
		ar(self.x);
		ar(self.z);
		ar(self.y);
		ar(self.yaw);
		ar(self.forwardSpeed);
		ar(self.lateralSpeed);
		ar(self.skills);
		ar(self.raceStrength);
		ar(self.raceDexterity);
		ar(self.attackInt);
		ar(self.defenceInt);
		ar(self.unarmedInt);
		ar(self.healFactor);
		ar(self.bloodFactor);
		ar(self.encumbrance);
		ar(self.weapon);
		ar(self.armour);
		ar(self.parts);
		ar(self.blood);
		ar(self.minBlood);
		ar(self.maxBloodRace);
		ar(self.wounds);
		ar(self.koTimer);
		ar(self.unconscious);
		ar(self.koLatch);
		ar(self.dead);
		ar(self.rightArmOk);
		ar(self.leftArmOk);
		ar(self.down);
		ar(self.hunger);
		ar(self.hungerRate);
		ar(self.hungerKo);
		ar(self.bleeding);
		ar(self.foodCharges);
		ar(self.eatTimer);
		ar(self.kitSid);
		ar(self.kitCharges);
		ar(self.state);
		ar(self.pending);
		ar(self.technique);
		ar(self.clipTime);
		ar(self.clipSpeed);
		ar(self.techniqueFinished);
		ar(self.impactIndex);
		ar(self.lastImpact);
		ar(self.hitListBuilt);
		// Save v66: the transient-looking combat bookkeeping that decides the next engagement (see above).
		if (ar.version >= 66)
		{
			ar(self.hitList);
			ar(self.holdSteer);
		}
		// Save v67: the technique's aim (the look hand). The swing's impact box and root motion read it, so a load mid-swing
		// resumes with the same one (ENGAGEMENT.md 9.4).
		if (ar.version >= 67)
			ar(self.aimYaw);
		else if constexpr (!std::is_const_v<Self>)
			self.aimYaw = self.yaw; // a v66 save has no aim: the body's facing stands in for a swing already in flight
		ar(self.recovering);
		ar(self.recoverTimer);
		ar(self.actionTimer);
		ar(self.swingProgress);
		ar(self.reactTimer);
		ar(self.stateTime);
		ar(self.stumbleTimer);
		ar(self.escapeTimer);
		ar(self.escapeAsked);
		ar(self.reactClip);
		ar(self.reactTime);
		ar(self.recoilX);
		ar(self.recoilZ);
		ar(self.circleTimer);
		ar(self.circleOffset);
		ar(self.previousTechnique);
		ar(self.cachedReach);
		ar(self.target);
		ar(self.targetAge);
		// Save v74: the goal that started the fight (the Goal row; task AT). A v73 save loads with none: the row falls back to the plain
		// attack goal until the next pick.
		if (ar.version >= 74)
			ar(self.combatGoal);
		ar(self.slots);
		ar(self.engaged);
		ar(self.overflow);
		ar(self.rng);
		ar(self.comfortRolls);
		ar(self.playerControlled);
		ar(self.route);
		ar(self.routeNext);
		ar(self.routeVersion);
		ar(self.routeSeq);
		ar(self.routeApproach);
		ar(self.routeTries);
		ar(self.routeChase);
		ar(self.chaseTx);
		ar(self.chaseTz);
		ar(self.chasePlanAge);
		ar(self.chaseCooldown);
		ar(self.chaseDirect);
		ar(self.drive);
		ar(self.driveHeading);
		ar(self.driveSeq);
		ar(self.driveBlocked);
		ar(self.driveTurn);
		if (ar.version >= 111)
			ar(self.driveStrafe);
		if (ar.version >= 112)
		{
			ar(self.passive);
			ar(self.holdPosition);
		}
		if (ar.version >= 118)
			ar(self.fought);
		ar(self.walkStuck);
		ar(self.orderTarget);
		ar(self.hold);
		ar(self.orderStop);
		ar(self.sneak);
		// Save v65: moveMode u8 + speedGroup u64 replace the run bool; before it, run = 0 loads as Walk (0), 1 as Run (2).
		if (ar.version >= 65)
		{
			ar(self.moveMode);
			ar(self.speedGroup);
		} else {
			bool run = self.moveMode != 0; // a v64 write keeps the mode's run bit
			ar(run);
			if constexpr (!std::is_const_v<Self>)
			{
				self.moveMode = run ? 2 : 0;
				self.speedGroup = 0;
			}
		}
		ar(self.name);
		ar(self.followTarget);
		ar(self.followHx);
		ar(self.followHz);
		ar(self.followDirect);
		ar(self.followAge);
		ar(self.followUnseen);
		ar(self.followSeenX);
		ar(self.followSeenZ);
		ar(self.velX);
		ar(self.velZ);
		ar(self.animal);
		ar(self.building);
		ar(self.crouched);
		ar(self.asleep);
		ar(self.attention);
		ar(self.senseClock);
		ar(self.sight);
		ar(self.carrying);
		ar(self.carriedBy);
		ar(self.npcClass);
		ar(self.homeBuilding);
		ar(self.houseLocked);
		ar(self.portraitSerial);
		ar(self.look);
		ar(self.chaseId);
		ar(self.chaseAge);
		ar(self.chaseUnseen);
		ar(self.chaseX);
		ar(self.chaseZ);
		ar(self.faction);
		ar(self.attacksNeutrals);
		ar(self.aggroBlockId);
		ar(self.aggroBlock);
		ar(self.aggroStart);
		ar(self.aggroEnd);
		ar(self.squad);
		ar(self.uniqueSquad);
		ar(self.hostileSquads);
		// Save v78: the aggressor tags (task BZ). An older save loads no squad tagged.
		if (ar.version >= 78)
			ar(self.aggressorSquads);
		ar(self.goal);
		// Save v68: captivity. A v67 save loads free (state 0, no owner, no cage).
		if (ar.version >= 68)
		{
			ar(self.slaveState);
			ar(self.captorSid);
			ar(self.cageBuilding);
			ar(self.cageSlot);
		}
		if (ar.version >= 107)
			ar(self.slaveStamp);
		// Save v109: the walk's velocity memory (k16.6.200). An older save loads both 0: the walk reads them as unset.
		if (ar.version >= 109)
		{
			ar(self.walkHeading);
			ar(self.walkFacing);
		}
		// Save v91: the captive-in-transit flag. An older save loads nobody captive.
		if (ar.version >= 91)
			ar(self.captive);
		// Save v75: the dialogue memory. An older save loads every character having met nobody.
		if (ar.version >= 75)
			ar(self.met);
		// Save v76: the ragdoll's rest yaw and its draw count (task BG). An older save loads a body with no roll: the next
		// prone step seeds one.
		if (ar.version >= 76)
		{
			ar(self.restYaw);
			ar(self.restRolls);
			ar(self.restYawValid);
		}
		// Save v79: the animal's technique family (task BQ). An older save loads 0 on every character: an animal picks no
		// animal technique until it is respawned (the humans, whose techniques read 0, are unaffected).
		if (ar.version >= 79)
			ar(self.attackType);
		// Save v81: the stable dice identity (task CC; v80 is task CB's goap fields). An older save loads 0: the fighter's saved rng
		// already started from its Id, so the arena keeps keying on the Id for it.
		if (ar.version >= 81)
			ar(self.diceSeed);
		// Save v85: indoors (task indoors). An older save loads outdoors; the next placement step sets it from the position. `cavernous`
		// is derived and not saved: it is back on that same step.
		if (ar.version >= 85)
			ar(self.indoors);
		// Save v96: the carried crossbow (task ranged). An older save loads nobody armed with one.
		if (ar.version >= 96)
			ar(self.ranged);
		// Save v97: the turret it mans (task ds/turret S3). An older save loads nobody mounted (the next routine look mounts it again).
		if (ar.version >= 97)
			ar(self.mount);
		// Save v98: the stealth KO it is carrying out (task ds/stealth-ko S4). An older save loads nobody mid-KO.
		if (ar.version >= 98)
			ar(self.stealthKo);
		// Save v114: the pair-distance cache entries (k16.8.50). An older save loads none: the first looks re-measure.
		if (ar.version >= 114)
			ar(self.pathSamples);
	}
};

} // namespace ksim::state
