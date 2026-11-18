#pragma once

// The character layer and the first combat slice (docs/reference/combat/IMPLEMENTATION.md).
//
// CombatRules: the pure rules, each a function of Fighter state and the combat
// data (clip speed, technique choice, block and dodge chances, damage, the
// medical system, XP). Every formula cites its decompile address in the .cpp.
//
// CombatArena: the fighters at full detail. It keeps them in an array sorted by
// state::Id (the canonical hot copy) and steps them serially, one work unit, in Id
// order: ARCHITECTURE.md "Threading" lets the regions of a fight merge into one
// unit, which removes the one-tick delay for hits. Randomness is one stream per
// fighter (Fighter::rng); nothing depends on the worker thread count. Only the
// fighters in the arena cost anything; with none, step() returns at once.
//
// Scope of the first slice: unarmed and one-handed melee 1v1 (the attack-slot,
// engagement, technique, resolution, damage, medical and XP code is general; the
// footwork is a simplified steering model, see IMPLEMENTATION.md).

#include "ksim/combat_data.hpp"
#include "ksim/fighter_state.hpp"
#include "ksim/perception.hpp"
#include "ksim/profile.hpp"
#include "ksim/sensor.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace knav
{
class Walker;
}

namespace ksim {

// A formation slot (ax, az) off the walker's mesh (inside a prop's box or a wall) is moved to the mesh edge nearest it, then `radius` further out from the
// solid, so a follower stops clear of the wall instead of sliding along it at 0 u (k16.6.320). True when it moved; an on-mesh slot, or none within
// reach, is left as it was.
bool clearFormationSlot(const knav::Walker& walker, double y, double radius, double& ax, double& az);

class MeshNavigator;

// Acid rain: 0.08 HP per real second per body part, undecoded from the data because it is hard-coded (kinspect
// weather_acid_rain.md section 4, DAT_1416fbdc8; ~8.8 HP per game hour at speed 1). It goes into the part's wear pool (the blunt
// channel with k = 0), reduced by the worn gear's weather protection and not by the weather's `effect strength`.
inline constexpr double kAcidDamagePerSecond = 0.08;
// Venge's sky beams: 10 HP per real second per body part at full strength (kinspect rwh48_venge_beams_static.md section 5), times the
// beam's strength and (1 - worn protection), into the wear pool like the acid.
inline constexpr double kBeamDamagePerSecond = 10.0;

// The exe's 1405ca9a0(c, 0), the only writer of Character+0xd4 (kinspect STEALTH_CLEAR.md, 71fa402): it clears the sneak
// flag and puts the speed mode back to the mover's default. Every clearing trigger goes through here; `reason` names which
// one (the seven call sites are exhaustive, so this list is the whole set of clears).
enum class SneakClear : std::uint8_t
{
	Targeted, // AI::onTargetedBy 140999f00: any fighter sets its target on X (no hostility / awareness / distance gate)
	Swing, // the engagement notify at an attack start 14043a730 -> 140999d50(attacker, 1): the attacker's own flag
	Hit, // being hit: playHitReaction 140665490 / receiveHit 1404394f0 (no target or awareness needed)
	FollowStop, // Task_FollowLeader stop 140333980
};
void clearSneak(state::Fighter& fighter, SneakClear reason) noexcept;

struct CombatEvent
{
	enum class Kind : std::uint8_t
	{
		TechniqueStart, // who started `technique` against `other`; a = reach d, b = clip speed
		PickDefence, // who chose `technique` (-1 none) against `other`; a = block %, b = dodge % (-1 none)
		Resolve, // who defends against `other`; code = 0 dodge, 5 block, 2 hit; technique = defender's
		Hit, // who was hit by `other`; part; dmg = after armour and toughness; code = impact direction, code2 = the reaction it starts (0 none = KO blow, 1 new heavy, 2 light from t = 0, 3 playing clip kept)
		Xp, // who gained XP of kind `code` against `other`
		Engage, // other asks who; code = result (0 attack, 1 defend, 5 circle)
		SlotAcquire, // other took a slot of who
		SlotRelease, // other released a slot of who
		Stumble, // who entered state 8
		Knockout, // who went unconscious
		Death, // who died
		State, // who changed combat state: code = new, code2 = old
		PositionCorrected, // who was found off the navmesh and snapped back; a = distance moved, -1 = no mesh within reach (not moved)
		DriveBlocked, // who, driven, was stopped by a wall or edge (once per blocked spell)
		Noticed, // who became aware of `other` (perception.hpp): a sensor record went from unaware to aware
		EscapeRoll, // who, staggered, rolled its escape against `other`: code = 1 won, 0 lost
		LimbSevered, // who lost the limb `part` (a hit past -M, or the Dismemberment 2 sink): once per sever
		GiveUpChase, // who gave up chasing `other` (the chase give-up monitor, EV_GIVE_UP_CHASE 60: the dialogues all go home); a = chase seconds, b = seconds unsensed
		Ragdoll, // who started a ragdoll (EVENT 48, task BG): code = cause (0 KO, 1 death, 2 drop/put-down, 3 launch), other = the hit's attacker (0 none), a = strength, seed, part = the hit part
		Shot, // who is the target of a fired ranged shot by `other` (rangedFire, SIMLINK event 49): code = 0 hit, 2 missed (1 blocked is reserved: a held shot sends nothing); part = the gun's sid; turret = the turret's building key (0 = a hand crossbow); dmg[0..2] = muzzle, dmg[3..5] = the target's point
		StealthKo, // who is the victim of a stealth KO attempt by `other` (SIMLINK event 50): code = 0 KO landed, 1 roll failed, 2 aborted (the victim was aware); a = the chance (0..100), 0 for an abort
		Goal, // who picked a target: other = the target, code = the goal (task type) the pick ran (task CF; the keep/join decision)
	};
	Kind kind = Kind::State;
	double time = 0.0; // arena seconds
	std::uint64_t who = 0;
	std::uint64_t other = 0;
	std::int32_t code = 0;
	std::int32_t code2 = 0;
	std::int32_t technique = -1;
	std::uint32_t seed = 0; // Ragdoll: the client's flop seed (the sim's own stream draw, kRagdollStream)
	double a = 0.0;
	double b = 0.0;
	double dmg[6] = {};
	double absorbed[2] = {}; // Hit: what the armour removed, cut (pierce merged in) and blunt, before the toughness factor
	std::string part;
	std::uint64_t turret = 0; // Shot: the manned turret's building key
};

// One candidate of the attack roulette (`1408876d0`).
struct AttackCandidate
{
	std::int32_t technique = -1;
	double weight = 0.0;
};

struct AttackPass
{
	std::vector<AttackCandidate> candidates;
	double total = 0.0;
};

// A defender's choice (`CharStats::pickTechnique`, `140887970`).
struct DefenceChoice
{
	std::int32_t technique = -1; // -1: none found
	double blockPercent = 0.0;
	double dodgePercent = -1.0; // -1: no dodge roll (an armed defender)
	bool dodgeMode = false;
	bool blockFail = false;
};

class CombatRules
{
public:
	CombatRules(std::shared_ptr<const CombatData> data, CombatSettings settings)
		: data_(std::move(data))
		, settings_(settings)
	{
	}

	const CombatData& data() const noexcept { return *data_; }
	const CombatSettings& settings() const noexcept { return settings_; }
	// Live tuning (ADVANCED_OPTIONS_STATIC.md "Ours", SIMLINK DEBUG_SET_OPTION): the world's new-game options (cod, gdm,
	// ht) and the medical clock's dev override. The sim pushes a change; none of it is read from the data or the save here.
	void setTuning(double chanceOfDeath, double damageScale, double hungerTime, double medicalClock) noexcept
	{
		settings_.chanceOfDeath = chanceOfDeath;
		settings_.damageScale = damageScale;
		settings_.hungerTime = hungerTime;
		settings_.medicalClock = medicalClock;
	}

	// `combat.retarget_hold_s`, live (COMBAT_OPTIONS.md 6): a scenario or the server flag sets it between runs.
	void setRetargetHold(double seconds) noexcept { settings_.retargetHold = seconds; }

	// The forced KO a part collapse starts (task BL, kinspect down_ko_trigger.md / fb0248d, live): the KO timer (med+0xa0)
	// is exactly 3.0 medical timer units (~2.1 s live at ~1.4 units/s), so it scales with the medical clock and game speed;
	// a sever calls the same writer (arg 0.7529, a 3.0 floor). It is not startKnockoutTimer: when it ends the down flag stays
	// and the body crawls (posture 2). playHitReaction's 25 % forced-KO roll is NOT modelled: kinspect found its gate returning
	// 0 for every item class in this build, so it is unreachable.
	static constexpr double kCollapseKoTime = 3.0;

	// ---- CharStats ----
	// Stat ids of getEffectiveStat (EFFECTIVE_STAT_LIVE.md; the exe's stat-name switch).
	static constexpr int kStatStrength = 1;
	static constexpr int kStatAttack = 2;
	static constexpr int kStatAthletics = 0x11;
	static constexpr int kStatDexterity = 0x12;
	static constexpr int kStatDefence = 0x13;
	static constexpr int kStatThievery = 10;
	static constexpr int kStatAssassination = 0x16;
	static constexpr int kStatMartialArts = 0x1e;
	// `StatsBlock::getEffectiveStat` with the combat callers' flags (110101: hunger, limbs,
	// prosthetics; p5 and p7 off): the multiplier of a stat for a hungry or hurt character.
	// Implemented for the combat stats above; any other id is 1.0. Not modelled: the prosthetic
	// limb item mults (P(id)), the med+0x1a8 block (1.0 live). Limb inputs come from the parts.
	static double effectiveStat(const state::Fighter& f, int stat, bool hunger = true) noexcept;
	// The skill float of the wielded weapon's category (`CharStats+0xf8..0x118`).
	static double weaponSkill(const state::Fighter& f) noexcept;
	// `140884ee0` / `140885e40`: the attack value (unarmed: the martial-arts one).
	static double attackValue(const state::Fighter& f) noexcept;
	// Defence value used by blockChance for an armed defender (`+0x124`, int and weapon mods).
	static double defenceValue(const state::Fighter& f) noexcept;
	// `140885190`: the dodge skill after encumbrance, health and armour.
	static double dodgeSkillEff(const state::Fighter& f) noexcept;
	// `CharStats::vf4` (`1408866b0`): CharStats+0x188 and +0x18c.
	double attackClipSpeed(const state::Fighter& f) const noexcept;
	double reactClipSpeed(const state::Fighter& f) const noexcept;
	double maxBlood(const state::Fighter& f) const noexcept;
	// `140883ff0`: 0 -> 0.6, 1 -> 0.3, otherwise 0.2.
	static double attackRecoveryTime(int kind) noexcept;

	// ---- chances (`1408862d0`, `140885d80`) ----
	double blockChance(const state::Fighter& defender, double attackerSkill) const noexcept;
	double dodgeChance(const state::Fighter& defender, double attackerSkill) const noexcept;
	// `140665370`: true when A (attacker, skill p2) is sent to circle; draws two uniforms.
	bool attackChanceRoll(double p1, double p2, std::uint64_t& rng) const;

	// ---- technique choice ----
	// Character geometry (MEASURED.md items 17, 18): CharStats+0x200 and cc+0x1a0.
	static constexpr double kCharD = 18.0;
	static constexpr double kCharWidth = 4.0;
	// CharStats+0x200 = the wielded weapon's `length` (18 for fists and the club, 21 for the Katana: the
	// e1 pick records show half 10.5 = 21 / 2). A real pick passes D/2 as its half body; the
	// weapon-change pick passes 18 for every weapon measured so far, taken here as D (UNSURE: all
	// measured weapons but the Katana have length 18, and the Katana's change pick was not logged).
	static double weaponLength(const state::Fighter& f) noexcept { return f.weapon.length > 0.0 ? f.weapon.length : kCharD; }
	static constexpr double kWeaponChangeDistance = 199.0;

	// `140888030` pass: candidates and weights at pick distance d (admitted when d <= L + half).
	// pass 0: the weapon type; 1 and 2: the unarmed fallback (relaxed limbs). Writes the reach
	// (CharStats+0x150): max(L + half) over the techniques that pass the filters, before the d test.
	AttackPass attackPass(const state::Fighter& f, double d, double half, std::int32_t previous,
		bool targetStumbling, int pass, double& reach) const;
	// The whole pick: passes until one has candidates, then the roulette. -1: none. `reach` is the
	// max over the passes run, which the caller caches as CharStats+0x150 (Fighter::cachedReach).
	std::int32_t pickAttack(const state::Fighter& f, double d, double half, std::int32_t previous,
		bool targetStumbling, double& reach, std::uint64_t& rng) const;
	// CharStats::onWeaponChanged: the pick with d 199 and half 18 (nothing is admitted, no roll is
	// drawn); returns the cached reach it leaves (43 + ... : unarmed 25 + 18).
	double weaponChangedReach(const state::Fighter& f) const;
	// `140887970`.
	DefenceChoice pickDefence(const state::Fighter& defender, std::int32_t incomingDir,
		double attackerSkill, bool attackerIsFists, bool behind, bool stumbling,
		std::uint64_t& rng) const;

	// ---- damage (`1408868b0`) ----
	// dmg[6] = {cut, blunt, pierce, stun, bleedMult, armourPenetration}, before power.
	void computeDamage(const state::Fighter& attacker, const state::Fighter& target,
		double dmg[6]) const;
	// MedicalSystem::receiveHit's armour step (RESOLUTION.md 4.2) on the part `part`: dmg[0..3] in place,
	// before toughness. `covers(piece index, coverage 0..1)` decides, per piece listing the part, whether
	// it covers this hit (the game rolls rand01() < coverage; a test can force the outcome).
	void applyArmour(const state::Fighter& f, std::string_view part, double dmg[6],
		const std::function<bool(std::size_t, double)>& covers) const;
	// `MedicalSystem::getDamageResistance`: the factor (1 - resistance) applied to a component.
	double toughnessFactor(const state::Fighter& f) const noexcept;

	// ---- XP (`1408c5df0`, `1408c6980`, ...) ----
	void addXp(double& skill, double amount) const noexcept;
	// `me` supplies the crowd factor (engaged attackers + overflow, MEASURED.md item 15).
	double skillDiffXpMult(const state::Fighter& me, double mine, double theirs) const noexcept;
	double xpUnit() const noexcept; // X = exp gain * 0.25 * 0.1

	// ---- MedicalSystem ----
	// `1406447d0`.
	double knockoutTime(const state::Fighter& f) const noexcept;
	double koThreshold(const state::Fighter& f) const noexcept; // the vital-part hit gate (KoGate setting)
	double koPoint(const state::Fighter& f) const noexcept; // `getKnockoutThreshold`: -lerp(toughness, min, max)
	static double partEffective(const state::FighterPart& p) noexcept
	{
		return p.health - p.stun + p.bonus;
	}
	// Per-second update: bleeding, clotting, stun decay, heat decay, KO timer, collapse flags, death.
	// Returns true when the fighter just died.
	bool updateMedical(state::Fighter& f, double dt) const;
	void checkCollapse(state::Fighter& f) const;
	// severPart (`14064edc0`, BQ11): for a humanoid's intact non-vital limb whose H - bonus is below -(max - wear), when the
	// Dismemberment setting allows `viaHit` (>= 1) or the passive path (>= 2). Forces the limb's max to 5, costs U[5, 30] blood
	// once and adds a wound of 200 x bleed rate (0.3). Returns whether it severed.
	bool trySever(state::Fighter& f, state::FighterPart& p, bool viaHit) const;
	// The sever itself, without the Dismemberment setting or the threshold (the eat application severs a part that reached it on its own
	// terms): false for a part that is no limb, already severed, robotic, or on an animal.
	bool severNow(state::Fighter& f, state::FighterPart& p) const;
	// Hunger per game hour is M / T, T = `starvation time` x the Hunger time option (MEDICAL_HUNGER_LIVE.md a).
	// M: dead or KO'd R x bed rate (no beds yet: a downed body takes the bed rate, UNSURE), moving or fighting
	// R x (1 + (1 - encumbrance) x 0.7), else R x 1 (the idle flag, M = 0.8, has no producer: UNSURE).
	double hungerUse(const state::Fighter& f) const noexcept;
	// ---- first aid (Task_FirstAid; MEDICAL_HUNGER_LIVE.md c) ----
	// The medic's First Aid skill: raw, x0.8 when treating himself.
	static double aidSkill(const state::Fighter& medic, bool self) noexcept { return medic.skills.medic * (self ? 0.8 : 1.0); }
	// Pool per second: lerp(min(skill, quality) / 100, 1, 15) x medic speed (robot kits: the robot medic speed).
	double bandageRate(double skill, double kitQuality, bool robotKit) const noexcept;
	// Kit charges per second: lerp((100 - skill) / 100, drain 99, drain 1).
	double kitDrain(double skill) const noexcept;
	// One tick of the bandage step: fills the pools of the parts the kit can treat (organic parts for an organic kit,
	// robotic for a robotic one) in the order head, torso, arms, legs, each before the next, up to max - wear - health.
	// Returns the pool added; `more` is set when some part still has room.
	double bandage(state::Fighter& patient, double rate, bool robotKit, double dt, bool& more) const;
	// The wound step: the NEWEST wound (back of Fighter::wounds) loses lerp(f, 0.0005, 0.03) x 2 x medic speed x dt,
	// f = min(skill, quality) / 100; leftover dt does not carry to the next wound (UNSURE). The bandage step runs only
	// once the list is empty (Simulation::treat).
	// UNSURE: f (not measured live).
	void clotWounds(state::Fighter& patient, double skill, double kitQuality, bool robotKit, double dt) const;
	// First Aid XP per second of treatment: 1 / lerp(skill / 100, XP rate medic 1, 99) (added directly, no addXP curve).
	double aidXpPerSecond(const state::Fighter& medic) const noexcept;
	// Blood loss per second right now (the bleed rate of the medical update).
	double bleedRate(const state::Fighter& f) const noexcept;

private:
	std::shared_ptr<const CombatData> data_;
	CombatSettings settings_;
};

class CombatArena
{
public:
	struct Member
	{
		std::uint64_t id = 0;
		state::Fighter fighter;
		// The defence-pending flag (+0x1f4) and the state-6 steer mode (+0x378) live in Fighter::holdSteer, the swing's
		// hit list (+0x1d8) in Fighter::hitList: the save carries both (v66), so a load mid-hold or mid-swing takes the
		// same decisions.
		// This step's depenetration move (xz, away from the neighbour; 0 when not pushed). CombatArena::step: set by the push at the head of the step,
		// read by the walk in the same step. Transient: rebuilt every step from saved positions.
		double pushX = 0.0;
		double pushZ = 0.0;
		// A landed hit on a player's move walk (k16.6.343, COMBAT_MOVE_LIVE.md 2): the seconds the walk still stands at zero velocity. Transient.
		double walkHold = 0.0;
		// The body's actual motion over the whole step, in the character's frame (fwd = along the facing, lat = to the right),
		// units/s: what the wire's CHAR_DYNAMIC fwdSpeed/latSpeed report (SIMLINK "CHAR_DYNAMIC"). It is a displacement, so it
		// carries everything that moves the body in the step, not only the walker's own move: the separation and reposition
		// pushes, a snap, a start that jumps to speed. Transient: recomputed every step.
		double preX = 0.0; // the position at the step's start (CombatArena::step)
		double preZ = 0.0;
		double stepFwd = 0.0;
		double stepLat = 0.0;
		// BQ13 / BQ14 join rules (join_rules.hpp): `joinDriven` = the character has an AI package, so it picks fights only through the attack
		// goals of its role list (`joinGoals`, joins::k* bits) and not on sight. Derived from the squad's package by the Simulation
		// (setJoinGoals); never saved. A plain fighter (a duel, a test) keeps the sight rule.
		bool joinDriven = false;
		std::uint16_t joinGoals = 0;
		// WaitingStateAI's far flag (CombatSteer hysteresis: set above a gap of 160, cleared below 70; BQ12 b). Transient.
		bool waitFar = false;
		// rwh.108 (FUN_1402ae0e0, steer+0x4c): the seconds the chase target has run freely (its own locomotion, > 9 u/s, not stumbling, not mid-action);
		// the steer aims at the target's predicted point once it passes 1.5 s. Transient.
		double leadHold = 0.0;
		// rwh.116: the far flag measured to the predicted point (90 / 160 / 70), and the steer's speed accumulator (steer+0x12c; per character, never
		// reset, 0 after a load). Transient.
		bool leadFar = false;
		double steerSpeed = 0.0;
		// SELF_PRESERVATION (goal 32, kinspect f5784bb + c1a305f): the attackers whose landed hits this member has
		// taken. A player character answers such an attacker at any range, past the kSelfDefenceRange gate (the
		// original's ranged victim turned on its shooter at once). Transient, like hitList: never saved.
		std::vector<std::uint64_t> hitBy;
		// The last fighter whose blow landed on this one (task AN's death attribution: a death by bleeding or hunger counts as the
		// last attacker's). Transient: a save mid-fight loses it and a later death reads 0.
		std::uint64_t lastAttacker = 0;
		// The crowd penalty's input (crowd_melee.md §10, task AU): how many attackers this fighter refused as a TARGET (its engaged
		// list was full) in the current and the previous step. Writers bump `refused[refusePhase]`; readers (the target score, the
		// wait ring) read the other phase, i.e. the previous step's count. No decay, no accumulation. The arena's double buffer is
		// rebuilt from the saved Fighter::overflow at add(), and step() writes it back, so a load resumes the same penalty.
		std::uint32_t refused[2] = {0, 0};
	};

	// The melee avoidance constants of one character (the exe's CombatSteer S+0xfc / +0x100 / +0x104 / +0x108), rolled at every melee task start (kinspect BQ12 a: a new
	// target, a mode switch or a restart; the same target and type does not re-roll): a pure function of the arena seed, the fighter's dice
	// key (Fighter::diceSeed, else the Id; task CC) and its task-start count (Fighter::comfortRolls, saved), so deterministic and identical
	// after a save and load.
	struct Comfort
	{
		double hostileRadius = 0.0; // +0xfc, [15, 45): added to Rsum for hostile neighbours (weight 1)
		double friendlyRadius = 0.0; // +0x100, [3.9, 33.9): for non-hostile neighbours (weight 2)
		double deadMoving = 0.0; // +0x104, [3, 6): the repulsion is dropped below this while moving
		double deadStanding = 0.0; // +0x108, [39, 66): and below this while standing
	};
	Comfort comfort(std::uint64_t id, std::uint32_t rolls = 0) const;

	// The attackers that count as threats to `id` right now (the exe's +0x228 list, buildIncomingThreatList): mid-swing (progress in (0, 1)) with
	// `id` as their target or in their hit list. For the tests and the debug views.
	std::vector<std::uint64_t> threatsOf(std::uint64_t id);

	CombatArena(std::shared_ptr<const CombatData> data, CombatSettings settings, std::uint64_t seed);

	const CombatRules& rules() const noexcept { return rules_; }
	const CombatData& data() const noexcept { return rules_.data(); }
	// The world's new-game options and the medical clock's dev override, on the arena's own copy (CombatRules::setTuning).
	void setTuning(double chanceOfDeath, double damageScale, double hungerTime, double medicalClock) noexcept
	{
		rules_.setTuning(chanceOfDeath, damageScale, hungerTime, medicalClock);
	}
	void setRetargetHold(double seconds) noexcept { rules_.setRetargetHold(seconds); }
	// The run speed of the fighter's gait (u/s): as it moves now, or with `laden` false without encumbrance and sneak.
	double runSpeed(const state::Fighter& f, bool laden = true) const;

	// Adds a fighter (id above 0, unique). An rng of 0 is seeded from the arena seed and the fighter's dice key (Fighter::diceSeed,
	// else the id; task CC).
	void add(std::uint64_t id, state::Fighter fighter);
	bool remove(std::uint64_t id);
	// Sets the join rules of one member (Member::joinDriven / joinGoals); false when there is no such member.
	bool setJoinGoals(std::uint64_t id, bool driven, std::uint16_t goals);
	// The key a fighter's dice streams hang on (task CC): its Fighter::diceSeed, the character's stable identity, when the spawn path set
	// one; else the Id (a bare arena, an older save).
	std::uint64_t diceKey(std::uint64_t id) const noexcept;
	// The goal selector's fight query (task fight-goals; kinspect patrol_aggro.md c332e51): the goal and target the member's package
	// goals admit right now - the same scan pickTarget commits, run as a pure query. Empty when the member is not package-driven or
	// no goal admits anyone. The target is the person the goal hands over (the attacker of a fighting squadmate for 143 / 15).
	struct FightGoal
	{
		std::uint64_t target = 0;
		std::uint16_t task = 0;
	};
	FightGoal fightGoalOf(std::uint64_t id);
	// Commits a fight goal the selector chose: the personal hostile entry for a target that is no enemy, the engagement and the Goal
	// event, exactly as pickTarget's tail does. False when either id is unknown.
	bool orderFight(std::uint64_t id, std::uint64_t target, std::uint16_t task);
	// Lets go of the member's target (a task that takes no fight: the individual run-away goals; a walking NPC never re-picks). False for an unknown id.
	bool dropTarget(std::uint64_t id);
	// A blow a task lands outside the fight (Task_KillPrisoner's "chop down static", dt24_killprisoner section 5, task dt27): CharStats::
	// computeDamage of the attacker on the defender, then MedicalSystem::receiveHit - no technique, no swing, no defence roll. A KO or a
	// death it causes is reported like any other (the Knockout / Death events and takeDeaths). False for an unknown id.
	bool strikeFromTask(std::uint64_t attacker, std::uint64_t defender);
	// Ranged fire (task ranged, dt41_A): one step of the attacker's crossbow at the defender. The range test (the height factor), the
	// 0.3 s retry / reload timer, the ammo, the blockers on the line (a walled-off target, an ally in the way: no shot) and the shot itself, an
	// instant line query with the skill's deviation; a hit lands through the same medical path as strikeFromTask.
	enum class Shot { NoWeapon, NoTarget, OutOfRange, OutOfArc, Waiting, NoAmmo, Blocked, Missed, Hit };
	Shot rangedFire(std::uint64_t attacker, std::uint64_t defender, double dt);
	// The range test alone: dist^2 < (range x height factor)^2 (dt41_A section 1).
	static bool inRangedReach(const state::Fighter& shooter, const state::Fighter& target) noexcept;
	// A mounted gunner's firing arc: the target's bearing within kTurretArcHalf of the turret's facing (OURS, UNSURE: the decode has no arc).
	static bool inTurretArc(const state::Fighter& shooter, const state::Fighter& target) noexcept;
	// Man a turret (task ds/turret, dt41_B): the gunner stands at the post and shoots with the turret's gun (`rangedFire` reads it). False for
	// an unknown, dead or already mounted fighter. dismountTurret is the task's stop (the gun is released; the next task plans as usual).
	bool mountTurret(std::uint64_t id, std::uint64_t building, double x, double z, double yaw, const state::FighterRanged& gun);
	void dismountTurret(std::uint64_t id);

	// ---- the stealth KO (task ds/stealth-ko S4; stealth_ko.hpp, docs/reference/combat/STEALTH_KO.md) ----
	// Row 228's order: `assassin` walks up behind `victim`, plays the knockout clip and rolls (Fighter::stealthKo; the arena's own step runs
	// it). Answers the reasons an order 60 would: "" (started; any other walk, route or fight order of the assassin is replaced),
	// "bad_target" (no such victim, or itself), "not_allowed" (the assassin is down, KO'd, carried, mounted or has no working arm; the
	// victim is dead, KO'd or down). cancelStealthKo is the task's stop.
	std::string orderStealthKo(std::uint64_t assassin, std::uint64_t victim);
	// The same answers without starting anything (the wire's pre-check, order 60 action 14).
	std::string checkStealthKo(std::uint64_t assassin, std::uint64_t victim) const;
	void cancelStealthKo(std::uint64_t assassin);
	// How one stealth KO ended, taken by the Simulation (assault crime and EV 0x38 ASSASSINATION_FAILED are its). `result`: 0 the victim
	// is KO'd, 1 the roll failed, 2 aborted before the roll (the victim knew the assassin). Transient like attacks_.
	struct StealthKoNote
	{
		std::uint64_t assassin = 0;
		std::uint64_t victim = 0;
		std::uint8_t result = 0;
		double chance = 0.0;
		double roll = 0.0;
	};
	std::vector<StealthKoNote> takeStealthKos() { return std::exchange(stealthKos_, {}); }
	state::Fighter* find(std::uint64_t id) noexcept;
	const state::Fighter* find(std::uint64_t id) const noexcept;
	const std::vector<Member>& members() const noexcept { return members_; }
	std::vector<Member>& membersMutable() noexcept { return members_; }
	bool empty() const noexcept { return members_.empty(); }
	// The previous step's refused-attacker count of `id` (the crowd penalty's n, crowd_melee.md §10); 0 for an unknown id.
	std::uint32_t lastRefusals(std::uint64_t id) const noexcept;
	// The state-6 follow ring (inner, outer) the arena would steer `waiterId` on: its target's engaged list size plus the target's
	// last step's refusals (crowd_melee.md §10.4). Empty when the waiter has no target.
	std::optional<std::pair<double, double>> waitRingOf(std::uint64_t waiterId) const noexcept;

	// A ragdoll start (task BG): rolls the lying yaw (state::Fighter::restYaw) and returns the client's flop seed, both from
	// the fighter's own stream, streamSeed(streamSeed(streamSeed(seed, kRagdollStream), dice key), restRolls++). No combat dice move.
	// Where it comes to rest (task body-rest, PHYSICS.md 3.3-3.4; OURS): a lying body on a navmesh first takes the knockback
	// (`push` units along (pushX, pushZ), a unit vector; 0 = none), then slides downhill off a steep slope, both through the Walker
	// so it stays on the mesh. Decided here, at the start, from the saved state: nothing new is saved and a load mid-fall agrees.
	std::uint32_t rollRagdoll(state::Fighter& f, std::uint64_t id, double pushX = 0.0, double pushZ = 0.0, double push = 0.0);
	// The seeded distance a body released by a walking carrier lands from it (PHYSICS.md 3.2), kDropMin..kDropMax.
	double rollDropDistance(const state::Fighter& f, std::uint64_t id) const;

	// ---- the eat application (140650370, kinspect dt39_eaten_alive) ----
	// Per eaten victim, derived and unsaved: the applications past the last part (+0x15c) and the drain branch's recompute field (+0x60;
	// UNSURE: its start value is taken as 0).
	struct EatState
	{
		std::uint8_t count = 0;
		double drain = 0.0;
	};
	enum class EatResult : std::uint8_t
	{
		Inedible, // the victim's race is robotic (+0x7c): refused, nothing changed
		Applied,
		Eaten, // the counter ran past kEatCounterMax: the victim is to be removed as "eaten"
	};
	struct EatOutcome
	{
		EatResult result = EatResult::Applied;
		bool screamed = false; // a conscious live victim: the caller fires SCREAMING_TORTURE (0x0b); the toughness XP is already given
	};
	static constexpr std::uint8_t kEatCounterMax = 5;
	// Toughness XP per XP unit of the exe's award (1408c6430 / severPart's 300): live raw xp 4.5 for the 300 -> 0.015 (UNSURE).
	static constexpr double kEatXpScale = 0.015;
	// One application of `dt` seconds on `victimId`. drainPath = the EATER race's +0x7d is set (blood drain), else the limb loop (one
	// part per application, reverse part order, a part past its threshold severs and the loop goes on). Dead victims take x5, live ones
	// x 2(1 - toughness x size x 0.01) (size = 1: the CharStats+0x34 field is not decoded, UNSURE). The ms+0xa0 = -1 KO countdown is not modelled.
	EatOutcome eat(std::uint64_t victimId, bool drainPath, double dt, EatState& state);

	// ---- the peeler's occupant torture (TortureBuilding::vf28 14029af90, kinspect dt39 section 6) ----
	// One tick on a living occupant: unless it carries bandage-type items (`bandaged`), every limb (Left Arm, Right Arm, Left Leg, Right Leg: the exe's vector order, rwh42 5)
	// is damaged by dt through the part helper 140644a70 (not the combat damage path), or severed (CombatRules::severNow, the item drop, the 300 sever XP) once at its threshold.
	// Once no limb has health above 0 the torso parts (Head, Stomach, Chest) are worked the same way, never severed; `finished` (when given) is set when every part is
	// at its line or severed: the caller releases the occupant (rwh42 5 steps 5-6). The armour strip comes before this call (Simulation::stepTorture).
	// Returns whether the occupant screams (SCREAMING_TORTURE 0x0b, every tick, the caller fires it); false for a dead or unknown one.
	// No per-tick toughness XP. UNSURE: the damage is the helper's first field = dt (its fifth, dt x 2, is read as nothing), the part
	// order is the vector's, a KO'd occupant still screams, and the sever XP firing here is untested live.
	bool torture(std::uint64_t occupantId, bool bandaged, double dt, bool* finished = nullptr);

	// One combat update of dt seconds for every fighter, in Id order.
	void step(double dt);
#if KSIM_PROFILE
	// The tick's profiler, for the sub marks inside step (docs/PROFILING.md); null = none.
	void setProfiler(profile::Profiler* profiler) noexcept { profiler_ = profiler; }
#endif
	double time() const noexcept { return time_; }
	void setTime(double time) noexcept { time_ = time; }

	void setObserver(std::function<void(const CombatEvent&)> observer) { observer_ = std::move(observer); }
	// The decision trace's fight hook (kenshi-k16.6.59, decision_trace.hpp Source::Combat): `wants` (null = every member) is asked before any
	// candidate list is built; `sink` gets one FightPick when a member's combat goal or target CHANGES (a held target adds nothing, so a
	// long fight does not flood the ring). Observation only: no RNG draw, no state change, nothing saved.
	struct FightCandidate
	{
		std::uint64_t id = 0;
		std::uint16_t goal = 0; // the combat goal that admitted it
		double utility = 0.0; // the distance term, (1 - d / 200) x 2 (d centre to centre)
		double crowd = 0.0; // its engaged count plus refusals (less one for the current target)
		double score = 0.0; // the picker's score
	};
	struct FightPick
	{
		std::uint64_t member = 0;
		std::uint64_t previousTarget = 0;
		std::uint16_t previousGoal = 0;
		std::uint64_t target = 0; // 0 = it let go (nothing admitted)
		std::uint16_t goal = 0;
		double score = 0.0;
		bool order = false; // a player's attack order (no scan ran: no candidates)
		std::vector<FightCandidate> candidates; // every admitted enemy in arena order; empty for an order
	};
	void setFightTrace(std::function<bool(std::uint64_t)> wants, std::function<void(const FightPick&)> sink)
	{
		fightWants_ = std::move(wants);
		fightSink_ = std::move(sink);
		fightLast_.clear();
	}

	// Attacks the crime step reads (crime.hpp): every technique start against a target and every landed hit, whether or not an
	// observer is set. Transient: the Simulation takes them every tick, so a save between ticks never holds any.
	struct AttackNote
	{
		std::uint64_t attacker = 0;
		std::uint64_t victim = 0;
		bool landed = false; // a landed hit (resolveIncomingHit's tail), not a technique start
	};
	std::vector<AttackNote> takeAttacks() { return std::exchange(attacks_, {}); }

	// Limbs severed since the last take (a hit or the Dismemberment 2 sink): the Simulation drops the severed limb item and the boots.
	// Transient like attacks_.
	struct SeverNote
	{
		std::uint64_t who = 0;
		std::string part;
		bool item = true; // severPart param_3: the eat-alive path severs with 0 and leaves no limb item (rwh42 1.4)
	};
	std::vector<SeverNote> takeSevers() { return std::exchange(severs_, {}); }

	// Deaths since the last take (task AN): the victim and the fighter whose blow killed it (0 = no blow: bleeding, hunger, a
	// script). Transient like attacks_; the Simulation records the world event's dead set and the player's involvement.
	struct DeathNote
	{
		std::uint64_t who = 0;
		std::uint64_t killer = 0;
	};
	std::vector<DeathNote> takeDeaths() { return std::exchange(deaths_, {}); }

	// What perception reads of the world (perception.hpp): the time of day (the Simulation sets it every tick from the
	// clock), the weather, line of sight and lamp hooks. Configuration, never saved.
	perception::Environment& environmentMutable() noexcept { return environment_; }
	const perception::Environment& environment() const noexcept { return environment_; }

	// MQ14 path 2 (ARCHITECTURE.md "Aggressive squads"): the faction pairs that are NOT neutral to each other, as bondKey(a, b) of the
	// Fighter::faction ids: the same faction, or `a` allied to `b` (relation at or above 50 in a's view). A fighter with attacksNeutrals
	// treats every other fighter whose pair is not bonded as an enemy. Configuration, never saved (the Simulation rebuilds it every 10 s
	// of sim time and at the first step after a load).
	static std::uint64_t bondKey(std::uint64_t a, std::uint64_t b) noexcept { return (a << 32) ^ (b & 0xffffffffULL); }
	void setBonded(std::unordered_set<std::uint64_t> bonded, std::unordered_set<std::uint64_t> coexist = {}, std::unordered_set<std::uint64_t> hostile = {})
	{
		bonded_ = std::move(bonded);
		coexist_ = std::move(coexist);
		hostile_ = std::move(hostile);
	}
	// The relation the enemy test reads (sensor::enemy), from Simulation::sensorContext: the observer's faction view, the same
	// refresh as the sets.
	// The relation is memoised for the length of one step() (nothing writes the factions' relations inside it); outside a step it
	// reads through.
	void setSensorContext(sensor::Context context);
	const sensor::Context& sensorContext() const { return sensor_; }
	// Test hook: the steer's speed accumulator of a character that has steered before (rwh.116: it is warm after the first chase, never reset).
	void warmSteer(std::uint64_t id);
	// The characters the depenetration leaves alone, as pusher and as neighbour (BQ16: in a bed or cage = the exe's char+0x2f8, operating a machine = +0xb8;
	// our stand-ins are a seated patron and an operator). Rebuilt by the Simulation before every step; never saved.
	void setPushExempt(std::unordered_set<std::uint64_t> ids) { pushExempt_ = std::move(ids); }
	// Faction pairs hostile by relation (at or below -30 in either view): the BQ14 goal 16 takes them on sight. Same refresh as the bond set.
	bool hostileFactions(std::uint64_t a, std::uint64_t b) const { return a != 0 && b != 0 && a != b && hostile_.count(bondKey(a, b)) != 0; }
	// The FACTION `coexistence` relation flag (kinspect bq10_goal21.md K2, RelationData byte 3): goal 21 skips such a target faction.
	bool coexists(std::uint64_t a, std::uint64_t b) const { return a != 0 && b != 0 && coexist_.count(bondKey(a, b)) != 0; }
	// Two factionless fighters (id 0) are bonded: wild squads share one team and never fight each other.
	bool bonded(std::uint64_t a, std::uint64_t b) const { return a == b || (a != 0 && b != 0 && bonded_.count(bondKey(a, b)) != 0); }
	// Task BP: whether `captorFaction` (the id Fighter::captorFaction holds; the Simulation resolves the slave's captorSid into it) is
	// `faction` or one of its allies. A captor's squad and its allies do not treat its captive as an enemy (the original's "is my
	// captive", CRIME_NOTICE C27; that the enemy test uses it is OURS/UNSURE). The caller checks its personal hostile list first.
	bool captiveOf(std::uint64_t captorFaction, std::uint64_t faction) const { return captorFaction != 0 && bonded(captorFaction, faction); }

	// The navmesh the fighters live on (SIMLINK.md section 12). With one, every
	// move goes through the Walker (slides on edges, sets y), an off-mesh fighter is snapped back at
	// the start of each step with a PositionCorrected event, and move orders follow their routes.
	// Without one the arena is a bare combat harness: footwork moves freely on the plane and move
	// orders have no routes (the sim refuses them), which is only for tests that never order a walk.
	void setNavigation(std::shared_ptr<const MeshNavigator> navigation) { navigation_ = std::move(navigation); }
	const MeshNavigator* navigation() const noexcept { return navigation_.get(); }
	// The body layer (task AW; mechanism OURS, numbers matched to live -- kinspect findings/bar_pile_throwout.md). The original
	// resolves ragdolls with physics; ours gives a KO'd or dead body (not carried, not in a bed or cage) its own collision layer:
	// an upright walks over it (the player crossed the live pile, the bodies moving <= 0.1 u), so uprights and bodies never pair,
	// and bodies push only each other apart, so a pile spreads instead of stacking. Ragdolls rest on and against each other: the
	// live bar pile settles 2.3-16.3 u apart in a ~15 x 8 patch, so the pair distance is 2.3 (matched), a small circle (OURS).
	// A conscious down crawler is NOT a body: it is a normal depenetration neighbour (see Ctx::pushes).
	static constexpr double kBodySpacing = 2.3;
	static constexpr double kBodyRadius = 0.5 * kBodySpacing;
	// A conscious down crawler's speed (task BF; kinspect bf_crawl_down.md 9a539ac): CharStats::getSpeed's `if (down)
	// speed = 5.0` overrides athletics, legs, blood and encumbrance entirely - it is a constant, not a formula (live: the
	// player and an NPC both 4.97-5.0 u/s). A down character crawls on a move order, a drive or a player's attack order
	// (crawling to the target, never landing a blow), pathing as usual with `down` still set; a KO'd body never moves.
	static constexpr double kCrawlSpeed = 5.0;
	// The launch strength an EVENT 48 carries for a hit (task BG, PHYSICS.md 3.3): the velocity change at the hit part, u/s.
	// OURS/UNSURE - the exe's impulse is not decoded; the client uses it only to size the flop. A collapse (a KO or a death
	// with no blow, a put-down) sends 0.
	static constexpr double kRagdollHitStrength = 3.0;
	// Where a body comes to rest (task body-rest, PHYSICS.md 3; OURS, UNSURE). The release lands 13-20 u from the carrier (the live
	// drop: 13.2 from the carry point, 15.3-19.5 from the carrier). A heavy hit moves the body kKnockbackPerStrength u per unit of
	// strength (the finding has no knockback numbers). A slope past kSlideMinSlope (tan 10 deg) slides it kSlideGain x tan, up to
	// kSlideCap (the live get-up drift: 4.1 u on a 14 deg dune), downhill, sampled kSlideProbe u out.
	static constexpr double kDropMin = 13.0;
	static constexpr double kDropMax = 20.0;
	static constexpr double kKnockbackPerStrength = 1.0;
	static constexpr double kSlideMinSlope = 0.176;
	static constexpr double kSlideGain = 16.4;
	static constexpr double kSlideCap = 8.0;
	static constexpr double kSlideProbe = 2.0;
	// The follow task (SQUAD_FOLLOW.md): ends it for f (what `Task_Follow` stop does: the sneak flag is reset)
	// and leaves the rest of f alone. Nothing when f is not following.
	static void endFollow(state::Fighter& f) noexcept;
	// True if making `subject` follow `leader` would close a loop of followers.
	bool followLoops(std::uint64_t subject, std::uint64_t leader) const noexcept;
	// The squad memory's TEMP_ENEMY tag (1406744a0 / 140674300 tag 4): every member of `id`'s squad lists `other`'s squad as hostile (the
	// personal type-4 list, Fighter::hostileSquads). forgiveHostile takes it off again (TEMP_ALLY, UNSURE: the sim has no ally tag).
	void markHostile(std::uint64_t id, std::uint64_t other);
	void forgiveHostile(std::uint64_t id, std::uint64_t other);
	// The follow slot offset of member `id` in the squad led by `top` (local frame, -X forward: +x is behind
	// the leader, +z to its right), for the current followers; the layout laws are kinspect 0789d56
	// (SQUAD_FOLLOW.md s3): random = sqrt(n x 100) disc + push-out, caravan = rand(10, 20) + 2r gaps.
	// Exposed for the tests.
	void formationOffset(std::uint64_t id, std::uint64_t top, double& ox, double& oz) const;
	// PositionCorrected events so far (soaks report it; it should stay 0), and those with no mesh in reach.
	std::uint64_t positionCorrections() const noexcept { return corrections_; }
	std::uint64_t uncorrectable() const noexcept { return uncorrectable_; }
	// Move routes dropped because the walk was blocked.
	std::uint64_t routesBlocked() const noexcept { return routesBlocked_; }
	// Reroutes done after a blocked walk or chase (bounded per walk: kMaxReroutes).
	std::uint64_t reroutes() const noexcept { return reroutes_; }
	// Re-requests after 5 s off the route (the exe's timeOffPath).
	std::uint64_t offPathRepaths() const noexcept { return offPathRepaths_; }
	// The melee gates' path queries so far and cache hits (task AQ's cost counters; ksim_fight prints the queries).
	std::uint64_t pathQueries() const noexcept { return pathQueries_; }
	std::uint64_t pathCacheHits() const noexcept { return pathCacheHits_; }

private:
#if KSIM_PROFILE
	profile::Profiler* profiler_ = nullptr;
#endif
	struct Ctx;
	friend struct Ctx;
	Member* memberOf(std::uint64_t id) noexcept;
	// The medical hit of a blow no technique made (strikeFromTask, rangedFire): hit-by tags, the receiveHit, the KO and death events.
	void landBlow(Ctx& ctx, Member& att, Member& def, double dmg[6]);
	// A pair's distance cache behind the melee gates (task AQ; ENGAGEMENT.md "Fights across floors"): the route length
	// plus the gap from its end to the target, and where both stood when sampled. Transient, never saved.
	struct PathSample
	{
		double length = 0.0; // infinity = no route
		double ax = 0.0;
		double ay = 0.0;
		double az = 0.0;
		double bx = 0.0;
		double by = 0.0;
		double bz = 0.0;
		double time = 0.0;
	};

	CombatRules rules_;
	std::uint64_t seed_;
	std::vector<Member> members_;
	double time_ = 0.0;
	// The crowd penalty's double buffer (Member::refused): the phase this step's refusals go to; readers use the other.
	std::uint8_t refusePhase_ = 0;
	std::function<void(const CombatEvent&)> observer_;
	std::function<bool(std::uint64_t)> fightWants_; // setFightTrace
	std::function<void(const FightPick&)> fightSink_;
	std::unordered_map<std::uint64_t, std::pair<std::uint16_t, std::uint64_t>> fightLast_; // member -> the (goal, target) last reported
	std::vector<AttackNote> attacks_;
	std::vector<SeverNote> severs_;
	std::vector<StealthKoNote> stealthKos_;
	std::vector<DeathNote> deaths_;
	perception::Environment environment_;
	std::unordered_set<std::uint64_t> bonded_;
	std::unordered_set<std::uint64_t> coexist_;
	std::unordered_set<std::uint64_t> hostile_;
	// The G5 sensor view (task BV/BU): the faction relation the enemy test reads, supplied by Simulation::sensorContext with the same
	// refresh as the sets above. Empty until the Simulation sets it (a bare arena keeps the team test only).
	sensor::Context sensor_;
	struct RelationMemo
	{
		bool on = false;
		std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, double>> byFrom;
	};
	mutable std::map<std::array<std::uint64_t, 3>, std::vector<std::pair<double, double>>> formationCache_; // formationOffset's random layouts
	std::shared_ptr<RelationMemo> relationMemo_ = std::make_shared<RelationMemo>();
	std::unordered_set<std::uint64_t> pushExempt_;
	std::shared_ptr<const MeshNavigator> navigation_;
	std::map<std::pair<std::uint64_t, std::uint64_t>, PathSample> pathCache_;
	// A walk's failed re-request (member -> the goal asked and when it may be asked again): the original blocks a goal its path request
	// failed on for 20 s (addBlockedTarget) and re-requests a changed goal only past 2 u (rwh.144 sections 1e, 5; kenshi-k16.6.262).
	// Transient, not saved: a load asks once more.
	struct ReplanFail
	{
		double x = 0.0;
		double z = 0.0;
		double until = 0.0;
	};
	std::unordered_map<std::uint64_t, ReplanFail> replanFail_;
	std::uint64_t pathQueries_ = 0;
	std::uint64_t pathCacheHits_ = 0;
	std::uint64_t corrections_ = 0;
	std::uint64_t uncorrectable_ = 0;
	std::uint64_t routesBlocked_ = 0;
	std::uint64_t reroutes_ = 0;
	std::uint64_t offPathRepaths_ = 0;
};

} // namespace ksim
