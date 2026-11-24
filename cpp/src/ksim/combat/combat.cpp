#include "ksim/combat/combat.hpp"

#include "ksim/ai/ai.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/world/join_rules.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/combat/stealth_ko.hpp"

#include "world/world_resolvers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#if defined(KSIM_PROFILE) && KSIM_PROFILE
#include "knav/route_measure.hpp"
#if KSIM_PROFILE
#include "ksim/dev/island_probe.hpp"
#endif
#endif

// The combat arena: the state machine of ENGAGEMENT.md around the rules of combat_rules.cpp.
// UNSURE parts (the footwork constants, the hit-reach box) are the kTune* values below; each is
// listed in IMPLEMENTATION.md.

namespace ksim {

namespace {

using state::CombatState;
using state::Fighter;

// The combat goals' live world for the planner (task CF): one closure, so Ctx::combatHolds can answer for the goal's hand.
struct CombatPlanWorld final : goap::World
{
	std::function<bool(goap::Sym, std::uint64_t, double, double)> eval;
	bool holds(goap::Sym sym, std::uint64_t target, double x, double z) const override { return eval(sym, target, x, z); }
};

// Footwork and geometry: fitted to the reference fights, not decoded (ENGAGEMENT.md 9, 11).
constexpr double kTuneWalkCap = 18.0; // `1402af1e0`: speed cap inside the near band
constexpr double kTuneBackCap = 16.0; // backing-off cap
// The state-6 hold's push (kinspect 48 (a), findings/item48_push_profile.md): a per-frame lag toward a fixed speed, v += 0.24 (16 - v), the same for
// both fighters. The 16 is not pinned to a source (not the run / walk gait, not the steer's maxSpeed 200); keep it a named constant.
// The ramp of the combat footwork (approach, circling, back-off) of a steer that aims at the body: fitted to the server's logged restarts (ENGAGEMENT.md 9,
// UNSURE; ksim_crawl, ksim_crowd_ring, ksim_combat_armed's radial speed and ksim_fight_levels are fitted to it). The chase that leads its target
// (rwh.116) uses the steer accumulator and the movement's acceleration instead (k16.6.172).
constexpr double kFootworkAcceleration = 15.0;
constexpr double kHoldPushSpeed = 16.0;
constexpr double kHoldPushLag = 0.24; // per exe frame
// UNSURE: our tick (20 Hz) is not an exe frame (9-19 ms). The rule is applied in sub-steps of one nominal exe frame, 11.1 ms (the measured
// median; 1/60 s would reach 16 u/s in 0.15 s, the exe takes 9 frames = 0.11 s), with the lag fraction scaled for a partial last sub-step.
constexpr double kExeFrame = 0.0111;
// The drive cuts to a target of 0 when the centre reaches the raw inner arg + 0.16 (15.41 against 15.25), not the ring edge 17.65; the same lag
// then decays the speed (x0.744 a frame, +0.45 u).
constexpr double kHoldPushCutExtra = 0.16;
constexpr double kHoldBrake = 200.0; // u/s^2 (per second) when the steer turns off mid-push
constexpr double kTuneApproachDistance = 150.0; // beyond this the target is "not directly reachable"
constexpr double kImpactW = CombatRules::kCharWidth; // cc+0x1a0, the character width
constexpr double kCapsuleRadius = 3.45; // target capsule radius at the impact test
// The impact box's vertical test (kenshi-rwh.214, findings/rwh214_melee_box_axes_static.md): the box's local Y is vertical
// (the rotation is a yaw) with half-extent 25.0, centred ~6 u above the attacker's feet (clamped ground + 9, pulled back);
// an impact whose attack direction is LEFT(2)/RIGHT(3) replaces the half-extents with (13, 8, D/2): 8 u vertical. The target
// is an axis-aligned box, feet to feet + 20 (s = 1; prone 5).
constexpr double kBoxHalfUp = 25.0;
constexpr double kBoxHalfUpSide = 8.0;
constexpr double kBoxCentreUp = 6.0;
constexpr double kTargetHeight = 20.0;
constexpr double kTargetHeightProne = 5.0;
// Melee avoidance (kinspect s74_static.md 1.4-1.6 and bq9_crowd_spacing.md): CombatSteer's two avoidance calls. Footprint radius of a human 2.6
// (race 'pathfind footprint radius', UNSURE for other races: all take 2.6 here); neighbours inside r_self + r_other + 40 horizontally and 20
// vertically; a repulsion that grows linearly to 100 at distance 0 within r_nb + Rsum + comfort radius; gain 0.105; hostile neighbours weigh 1,
// others 2; the current target is skipped; at most 26 neighbours; dead zones from the character's Comfort.
constexpr double kAvoidFootprint = 2.6;
constexpr double kAvoidMargin = 40.0;
constexpr double kAvoidVertical = 20.0;
constexpr double kAvoidScale = 100.0;
constexpr double kAvoidGain = 0.105;
constexpr double kAvoidFriendlyWeight = 2.0;
constexpr int kAvoidMaxNeighbours = 26;
constexpr double kAvoidSpeedCap = 18.0; // near the target the steer caps its speed at 18
constexpr double kComfortFootprintBase = 3.9; // cc+0x278 after FUN_14060ce30: footprint x 1.5 (of the human 2.6; other races scale it)
// WaitingStateAI's steer (s77_static.md b): it runs straight at the target, no pathing. Above a gap (centre distance - Rsum) of 160 (cleared below 70) the
// speed cap is min(200, run speed), else 18.
constexpr double kWaitFarOn = 160.0;
constexpr double kWaitFarOff = 70.0;
constexpr double kWaitFarCap = 200.0;
// rwh.116 section 5 (`1402af1e0`): the far flag of the combat steer is set above a gap of 90 (160 in Waiting) and cleared below 70, measured to the aim
// point; the steer's speed accumulator (steer+0x12c = CharMovement+0x244) gains 15 u/s^2 per character, is never reset, and limits the direct steer's
// speed. The body then follows it at the movement acceleration (rwh.116 section 2), not at 15.
constexpr double kSteerFarOn = 90.0;
constexpr double kSteerAccumulate = 15.0;
constexpr double kSteerHoldSpeed = 55.0; // the accumulator's cap while the hold point (steer+0x50) is active
// rwh.108 section 1 / 6 (the predictor FUN_1404354d0 and the gate FUN_1402ae0e0): the chase aims at pos + facing x speed x lead for a target that has run
// freely for 1.5 s above 9 u/s. lead = 1.2 + (1 - attack animation speed mult) x 2 s in the steer, a flat 1.2 s in the swing tests.
constexpr double kLeadBase = 1.2;
constexpr double kLeadAnimWeight = 2.0;
constexpr double kLeadSwing = 1.2;
constexpr double kLeadMinSpeed = 9.0;
constexpr double kLeadHold = 1.5;
// Depenetration (s77_static.md c): the push per frame, no dt, is (R - |d|) x (0.5 + (r_n - r_s) / R) against the largest neighbour, capped at 1.0 physics
// = 10 world units.
constexpr double kDepenetrateCap = 10.0;
// A pusher with posture != 0 (sneaking, crouched: char+0xe0) pushes at x0.02 (s80_static.md Q1.2). R is the footprints' sum unscaled (human 4 + 4 = 8);
// the old 7.07 / 8 scale (kDepenetrateScale) is retired (retired/combat_depenetrate_scale.txt).
constexpr double kDepenetratePosture = 0.02;
// The contact lag (rwh.188 live, sections 3 / 8): the push runs against the body's own contact set, which lags a passer, so a fast pass misses most bodies
// (124 u/s: 1-6 of 9-10 overlapped bodies pushed, mean 2.8; 20 u/s: all). The arena step is 0.025 s (two per 0.05 s tick, substeps 2): a runner steps
// 3.1 u and spends 3-5 steps inside R = 8 (chord 10.6-16 u), a walker 10+ per 0.05 s. A neighbour pushes only once it has been inside R for kContactTicks
// consecutive earlier steps (0.1 s): a run pass then catches the long chords only, about the live share; a walk catches every body.
constexpr double kContactTime = 0.1; // the lag as a duration (4 steps of 0.025 s); separate() counts steps, so it converts at the arena's step
constexpr int kSettlePasses = 8; // settleSelf: the halving overlap is under 0.4 % of itself after 8 (live rest spacing 7.98-7.99 of R = 8)
constexpr double kDepenetrateMin = 0.02; // the push is dropped below this overlap (raw 0.001 physics units)
// The body layer (task AW, OURS: the original resolves bodies with ragdoll physics). A lying body gives way a fraction of its own
// overlap against the largest one per step (the upright pass's rule, so each body of a pair gives way once and nothing overshoots:
// the pair takes about 0.28 of the overlap per step at this rate), capped to kBodySpreadCap units per step: slow, never a shove.
// The radius and the pair distance are CombatArena::kBodyRadius / kBodySpacing (combat.hpp). The floor is the upright push's: below
// it the overlap is not worth a move, so a pile comes to rest (about 0.13 short of the spacing).
constexpr double kBodySpreadRate = 0.15;
constexpr double kBodySpreadCap = 1.0;
constexpr double kBodySpreadMin = kDepenetrateMin;
// How far a rest point that a blocked step left off the mesh may be snapped back (CombatArena::rollRagdoll); farther keeps the old point.
constexpr double kRestSnapReach = 6.0;
// The hit reaction (state 8), DUEL_CADENCE_LIVE.md (d): the clip ignores the attacker's technique. A HEAVY hit (damage >
// toughness x 0.52) with no reaction playing starts `mid blow` (4.63 s), which the escape timer CombatClass+0x124 = 1.2 s cuts
// (19 of 51 chop left-3 reactions end at 1.20-1.21 s); any other reaction is `mid blow light` (1.133 s clip, ends at 1.11 s: 58 of
// 106 combo reactions, Engival's 66 of 66), and a heavy hit during a reaction restarts it with that light clip (the combo's second
// impact). The re-roll against nearby attackers (1.6-3.0 s, `stumble()`) is retired.
constexpr double kStumbleTime = 1.11; // the light reaction: `mid blow light` ends at 1.11 s
constexpr double kStumbleHeavyTime = 1.2;
// A player's move walk under attack (COMBAT_MOVE_LIVE.md, kenshi-rwh.172 live): a light hit zeroes the velocity for 1-3 frames and the walk
// ramps back up; a heavy hit (state 8) holds it ~0.45 s. The crowd does not block, nothing pushes the runner, no re-plan.
constexpr double kMoveHitHold = 0.1;
constexpr double kMoveStumbleTime = 0.45; // the heavy `mid blow` cut by the escape timer
// No recovery hold after the stagger (the recovery flag +0x144 stays 0 in the logs: the timer reads 0.2 but nothing waits on it, and
// the defender polls and picks in the very frame state 8 ends). The earlier 0.2 s hold (`kStaggerRecovery`) delayed those picks.
// After a swing that was dodged the next swing follows 0.7 s after the swing end, not 0.2: the 0.2 timer runs out, a
// one-frame state 0 blip follows (exe+0x60a15a) and the hub's 0.5 s timer runs once more (clog_1v1_a2: gaps 0.41..0.50 after
// an unresolved swing, 0.70 after each of 8 dodged ones). The katana duel's 4 dodged swings show 0.19..0.70. UNSURE why only
// dodges re-arm it; hits (0.60, 58 of 58) and blocks (0.29, 9 of 9) do not. Replaces the fitted swing-end pad.
constexpr double kDodgedHold = 0.5;
// The hit reaction pushes the defender away from the attacker by the playing clip's root motion (DUEL_RECOVERY_RECOIL_LIVE.md b):
// `mid blow` moves it 4.2 u back by 0.5 s and 14 u by the 1.2 s cut, `mid blow light` 10.6 u by 1.0 s. A heavy hit during a reaction
// restarts the (light) clip, so a combo's two landed hits push 4 + 10.6 u. The earlier fitted curve is retired
// (retired/combat_recoil_path.txt).
constexpr double kPi = 3.14159265358979323846;
// The AI_TASK a player's attack order runs (task AT, the Goal row: n_ATTACK 5, "Attacking target"); the goal the fight records.
constexpr std::uint16_t kAttackOrderGoal = 5;
constexpr double kPostSlack = 40.0; // a mounted gunner farther than this from its post has been moved off it: it lets go of the turret
constexpr std::uint16_t kUnprovokedFocusedGoal = 61; // UNPROVOKED_FOCUSED_MELEE_ATTACK: its remap is 266
// How far from the navmesh a fighter may be and still be snapped back onto it.
constexpr double kCorrectionReach = 256.0;
// correct() looks for the mesh point on the fighter's own level first (this far up or down, a stair's rise or two), so a fighter beside a ledge over a stair flight
// or a ramp is brought back to its own floor, not snapped to the nearest point of the other level (ENGAGEMENT.md 15, kenshi-h2v.30).
constexpr double kCorrectionLevel = 4.0;
// ... and only that near: a character set down at a bogus height (a placement's y 29 u under the floor it stands on) goes straight up to the floor above, not to
// some floor of its own height tens of units away.
constexpr double kCorrectionOwnReach = 16.0;
// Attack chases (docs/reference/SQUAD_FOLLOW.md s4, the follow task's rule): steer straight at the target
// below 50 units, follow a navmesh route beyond 100, and in between keep whichever the character was
// doing (hysteresis). UNSURE for attacks: borrowed from the follow task, not measured on a chase.
constexpr double kChaseDirectBelow = 50.0;
constexpr double kChaseRouteAbove = 100.0;
// A target on another floor (task AS): when the two stand more than this apart in height, no straight walk can
// reach, so the chase plans a route to the target's 3D point (its click height) instead of steering at its xz,
// and re-plans when the gap crosses it (a target that took a stair). OURS: a storey is 50-70 u, so 20 clears
// slopes and stair steps; the Watchtower's floors are 127 apart and the Hub Storm House is a storey too.
constexpr double kFloorApart = 20.0;
// A chaser whose height is more than this off its route's surface (nearest point on the current leg) has left the route's level: a hit's recoil carried it off a
// stair's edge onto the floor beneath. It re-plans from where it stands (OURS, UNSURE: the original's rule is not decoded; the rwh.295 re-plan interval is 2.6 s median, PATH_WALK.md 3d).
constexpr double kOffSurface = 8.0;
// ...unless the gap is a hillside's: under kFloorSlope x the xz distance. On the runtime mesh the shipped surface sits up to 30 u off the
// heightmap, so two bodies on one slope differ by 20+ u (kenshi-k16.6.271). OURS, tuned: the mesh's own 40 degrees (0.84) merged the tower's
// floors (128 u at ~150 u xz, ksim_melee_floors); see NAVMESH_CACHE.md "Native source".
constexpr double kFloorSlope = 0.4;
// A chase route is re-planned when the target is this far from where it was when planned (UNSURE; about
// half a second of a running target), but not more often than kChaseMinPlan seconds.
constexpr double kChaseReplan = 30.0;
constexpr double kChaseMinPlan = 0.5;
// A follower's route to its slot is re-planned when the slot moved this far from the plan: the original re-requests a path once the goal
// moved more than 2 u from the stored one (PATHFINDING_STATIC.md s1 "Dedup", rwh.144, H), still not more often than kChaseMinPlan.
constexpr double kFollowReplan = 2.0;
// A walk (move order or chase) blocked mid-way re-routes at most this many times, then is abandoned
// (move order: cleared; chase: stands for kChaseCooldown seconds, then plans again).
constexpr std::uint32_t kMaxReroutes = 3;
constexpr double kStuckRepath = 5.0; // seconds of barely moving on a walk before it re-paths (s70 section 4)
// timeOffPath (HAVOKCHAR_EXE.md 3 step 6, C100-C102): a walk that stays off its route this long (5.0 s) re-requests the path to the same goal.
// The exe is off the route when the walker's face is outside the faces of route nodes [nextEdge-1 .. nextEdge+4]; ours has no faces, so it is
// off when it is more than kOffPathReach (a multiple of the body's footprint) from the polyline through route points [routeNext-1 .. routeNext+4],
// or off the mesh (LOCOMOTION.md 9, D7).
constexpr double kOffPathRepath = 5.0;
constexpr double kOffPathReach = 3.0;
// The corridor walk (PATH_WALK.md 3b step 5): a position this close behind a portal's line counts as on it, passed (ours, not the exe's).
constexpr double kOnPortalLine = 0.05;
constexpr double kFirstPointDrop = 4.0; // a route's start this near the walker, with a clear way to the next point, is dropped (k16.8.8)
constexpr double kArriveReach = 1.0; // a walk within this (xz) of its end has arrived (bq16_deadlocks.md Q2: 0.01 physics^2)
constexpr double kFaceByVelocity = 4.0; // above this speed (u/s) the body faces the velocity, below it the way to the next corner (s70 section 3)
constexpr double kChaseCooldown = 2.0;

// The follow task (docs/reference/SQUAD_FOLLOW.md s4 and s9, measured unless marked).
constexpr double kFollowHeadingRate = 0.5; // heading += (h - heading) * rate * dt
constexpr double kFollowSlotLead = 1.0; // the slot leads the leader by this many seconds of its speed
constexpr double kFollowFixCos = -0.8660254037844386; // option squad.follow_heading_fix: snap beyond 150 degrees
constexpr double kFollowStandoff = 40.0; // Task_FollowLeader's follow distance, for a leader outside the squad
constexpr double kSlotStandOff = 1.0; // a slot on the mesh stands this far from its edge: the prop ring (0.75 u) alone left a follower there under a body's margin from the prop (kenshi-k16.6.548)
constexpr double kFollowStandOffRate = 2.0; // u/s a halted follower is eased off a mesh edge
constexpr double kSlotSnapReach = 64.0; // how far from a formation slot off the mesh the nearest mesh edge is looked for (clearSlot)
constexpr double kFollowDirectionFloor = 0.1; // the direction factor never goes below this
// The give-up monitor (s4 step 2): "perceived" is the follower's sensor having seen the leader on its last scan
// (perception.hpp; slack of a quarter scan interval), and the original's time units are taken as seconds (UNSURE).
constexpr double kPerceivedSlack = 1.25;
constexpr double kFollowMonitorStart = 5.0;
constexpr double kFollowGiveUpAge = 12.0;
constexpr double kFollowGiveUpRadius = 60.0;
// A player character's own SELF_PRESERVATION (goal 32; kinspect f5784bb + c1a305f): it answers an attacker that
// targets it once that attacker is within this centre distance. 150 is a placeholder (UNSURE): live, bandits picked
// a standing player at 374-462 u and the player's goal flipped 5.24 s later with the nearest attacker at 146 u and
// closing at about 60 u/s, so the range and a reaction time are not separated yet (engagement Q3). An attacker
// that has already hit it is answered at any range (a faction-mate archer's shot at 200 u notified the victim but
// never flipped it: that attacker was no enemy, not a miss).
constexpr double kSelfDefenceRange = 150.0;
// HOLD POSITION (rwh.148 3b; the tutorial string: "only attack any enemy that comes into close range"): a holder picks or keeps a target only
// within kHoldRange (centre to centre) and lets go past kHoldKeep, so it never chases (an armed crossbow in reach keeps a farther one, rwh.179). OURS / UNSURE: the original's "close range" is not
// measured; 40 u is about twice the melee ring (the circling band) so a swing's reach and a ring-mate count, and the 60 u keep stops a
// target at the edge flickering the pick.
constexpr double kHoldRange = 40.0;
// Task 13 ATTACK_ATTACKERS_OF: the ally's attacker is taken only within this distance of the ally (rwh.189 s3, d^2 <= 22500).
constexpr double kAttackerOfRadius = 150.0;
constexpr double kHoldKeep = 60.0;
// The player speed modes (SIMLINK.md order 10 `which` 5; kinspect 0789d56, the exe's setSpeedMode 140665150): Jog's cap
// in u/s. Walk is the race walk speed through the run flag (gaitOf), Run has no cap, Group the slowest member's run speed.
// They cap move orders and drives; a chase or a follow task always runs (Ctx::runSpeed).
constexpr double kSpeedModeJog = 55.0;
// The chase give-up monitor (s73_static.md sections 1, 3 and 5; Task_Move 14033a800). Game time to sim seconds: a game day is
// kGameDaySeconds (33 game seconds per sim second). 1 U of the exe's clock is 66 game seconds, so 12 U = 13.2 game minutes.
constexpr double kGameMinute = kGameDaySeconds / 1440.0;
constexpr double kChaseMonitorStart = 5.0 * kGameMinute; // the chase must have run this long before the monitor may fire
constexpr double kChaseGiveUpAge = 13.2 * kGameMinute; // unsensed for longer than 12 U
constexpr double kChaseGiveUpRadius = 60.0; // or within 60 u of the record's position
constexpr double kGoalRange = 6517.0; // 140599f30: goal 21's target range (d^2 = 42467328)
constexpr double kChaseClearOrders = 66.0 * kGameMinute; // 60 U: the monitor clears the chaser's orders (s73_static.md section 3)
constexpr double kRequestDedup = 2.0; // vf18 treats a goal within 2 u of the stored one as the same request (rwh.144 section 5)
constexpr double kBlockedTarget = 20.0; // addBlockedTarget: a dropped target is skipped this many sim seconds (s73_static.md section 3)
constexpr double kChaseTrackOpen =27.5 * kGameMinute; // 25 U: the unsensed record still follows the target's true position
constexpr double kChaseTrackTown = 8.8 * kGameMinute; // 8 U when the target is in a town
// Formation (SQUAD_FOLLOW.md s3). The member radius = body radius x 1.5 (the push-out and the caravan
// gaps; kinspect 0789d56), one constant for every fighter (a body radius of 4, UNSURE per race).
constexpr double kFormationRadius = 6.0;
// The push-out sets the slot, not the disc (kinspect f834513, 2026-11-05): leader-member |o| = 1.5 x s_member
// + s_leader + 15 (decoded 1.5 s + s_leader + 5; the +10 is UNSURE); member-member = 1.5 x s_i + 1.5 x s_j + 5
// (UNSURE: decoded, not measured live). At s = 4 every member sits on the leader ring at N <= 6; the disc
// radius only binds at N >= 7.
constexpr double kLeaderRing = kFormationRadius + 4.0 + 15.0; // 25
constexpr double kMemberGap = 2.0 * kFormationRadius + 5.0; // 17

double draw(std::uint64_t& rng)
{
	RngStream stream(rng);
	const double value = stream.uniform01();
	rng = stream.state();
	return value;
}

double wrapAngle(double a) noexcept
{
	while (a > kPi)
		a -= 2.0 * kPi;
	while (a < -kPi)
		a += 2.0 * kPi;
	return a;
}

// The gait of a move order or a drive (knav::Gait: the one definition the client predicts with too).
knav::Gait gaitOf(const CombatData& data, const Fighter& f)
{
	knav::Gait gait;
	if (const RaceCombat* race = data.race(f.raceSid))
	{
		gait.speedMinSkill = race->speedMinSkill;
		gait.speedMaxSkill = race->speedMaxSkill;
		gait.walkSpeed = race->walkSpeed;
		gait.acceleration = race->pathfindAcceleration * knav::kAccelerationScale;
		gait.footprint = race->footprintRadius;
	}
	gait.athletics = f.skills.athletics * CombatRules::effectiveStat(f, CombatRules::kStatAthletics);
	// The legs (task BH, kinspect `140884970`): min(left, right leg effective health / max health) clamped to 0..1, on the
	// excess over the 11 u/s floor. A leg at 0 stands (0 is above the negative KO point) and leaves exactly 11 u/s - the BF
	// live side fact; a collapsed leg is negative and clamps to the same floor. A race without named legs (animals) keeps 1
	// (UNSURE: the exe reads its own limb slots).
	{
		double worst = 1.0;
		bool any = false;
		for (const state::FighterPart& p : f.parts)
		{
			if (p.name != "Left Leg" && p.name != "Right Leg")
				continue;
			any = true;
			worst = std::min(worst, p.maxHealth > 0.0 ? CombatRules::partEffective(p) / p.maxHealth : 1.0);
		}
		if (any)
			gait.legs = std::clamp(worst, 0.0, 1.0);
	}
	gait.run = f.moveMode != 0 && !f.strolling; // speed mode 0 (Walk) clears the run flag; so does a non-urgent routine goal (kenshi-h2v.31)
	if (f.moveMode == 1)
		gait.cap = kSpeedModeJog;
	gait.encumbrance = f.encumbrance;
	// UNSURE: the mods leave out Medical+0xb4, CharStats+0x3c and the leg-slot item's Stealth modifier.
	if (f.sneak && !perception::inCombat(f))
		gait.sneak = perception::stealthFraction(f.skills.stealth, f.crouched, f.carrying != 0, f.encumbrance);
	// An NPC carrier asked to walk goes at move mode 1 instead, capped at 55 (GATES_CARRY_STATIC.md section 5).
	if (f.carrying != 0 && !f.playerControlled)
		gait.walkSpeed = std::max(gait.walkSpeed, kCarryWalkCap);
	return gait;
}

} // namespace

void CombatArena::warmSteer(std::uint64_t id)
{
	if (Member* m = memberOf(id))
		m->steerSpeed = 1.0e9;
}

bool clearFormationSlot(const knav::Walker& walker, double y, double radius, double& ax, double& az)
{
	if (walker.onMesh({ax, y, az}))
		return false;
	const std::optional<knav::Position> edge = walker.snap({ax, y, az}, kSlotSnapReach);
	const double gap = edge ? std::hypot(edge->x - ax, edge->z - az) : 0.0; // in the plane: a slot above or below the mesh is not beside a wall
	if (gap < 1.0e-6)
		return false;
	const double ox = (edge->x - ax) / gap;
	const double oz = (edge->z - az) / gap;
	if (walker.onMesh({edge->x + ox * radius, edge->y, edge->z + oz * radius}))
	{
		ax = edge->x + ox * radius;
		az = edge->z + oz * radius;
	} else {
		ax = edge->x;
		az = edge->z;
	}
	return true;
}

// The push that takes (x, z) on the mesh back from an edge it is within kSlotStandOff of: a follower that stops stands there for good, and the
// edge of a prop's clearance ring is only 0.75 u off the prop (a body's margin is 1.1 u, kenshi-k16.6.548). (0, 0) when clear of every edge.
static void standOffPush(const knav::Walker& walker, double y, double x, double z, double& px, double& pz)
{
	px = 0.0;
	pz = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const double ux = std::sin(i * (std::numbers::pi / 4.0));
		const double uz = std::cos(i * (std::numbers::pi / 4.0));
		const knav::StepResult r = walker.step({x, y, z}, ux * kSlotStandOff, uz * kSlotStandOff);
		if (r.onMesh && r.blocked)
		{
			px -= ux * (kSlotStandOff - r.moved);
			pz -= uz * (kSlotStandOff - r.moved);
		}
	}
}

struct CombatArena::Ctx
{
	CombatArena& arena;
	const CombatRules& rules;
	const CombatData& data;
	double dt = 0.0;
#if KSIM_PROFILE
	// kenshi-k16.6.305: think (the pick) vs act inside the update loop; counted only while `on`.
	struct ThinkProbe
	{
		bool on = false;
		int depth = 0;
		std::uint64_t thinkNs = 0, thinkRouteNs = 0, thinkCalls = 0, scanNs = 0, scanCalls = 0;
		std::array<std::uint64_t, 9> byState{}; // outer pickTarget calls by the caller's state (kenshi-k16.6.631): Attack Defend Between Hub Circle Wait Stumble Done Approach
	} think;
	// kenshi-k16.6.631: the member being updated, its distance band, and the branch it took (Dead Down Walk Follow Drive Idle Fight Other).
	enum Branch : int { BrDead, BrDown, BrWalk, BrFollow, BrDrive, BrIdle, BrFight, BrOther };
	int branch = BrOther;
	int band = 2;
	std::array<std::uint64_t, 24> branchN{}; // [branch * 3 + band]
	struct ThinkTimer
	{
		ThinkProbe& p;
		std::uint64_t began = 0, routeBegan = 0;
		bool outer;
		explicit ThinkTimer(ThinkProbe& probe) : p(probe), outer(probe.on && probe.depth == 0)
		{
			if (!outer) return;
			++p.depth;
			began = knav::measure::now();
			routeBegan = knav::measure::counters.routeNs.load(std::memory_order_relaxed);
		}
		~ThinkTimer()
		{
			if (!outer) return;
			--p.depth;
			++p.thinkCalls;
			p.thinkNs += knav::measure::now() - began;
			p.thinkRouteNs += knav::measure::counters.routeNs.load(std::memory_order_relaxed) - routeBegan;
		}
	};
#endif
	// Steering request of the fighter being updated.
	struct Steer
	{
		bool active = false;
		double inner = 0.0;
		double outer = 0.0;
		double cap = kTuneWalkCap;
		bool run = false;
		bool backOff = false; // moves out when inside `inner`: the state-1 steer (Item 34 (e)) and the state-6 hold (Item 46); the ring of 3 stays at rest inside (d)
		bool straight = false; // no path chase beyond the ring: steers straight at the target (WaitingStateAI)
	} steer;
	struct WalkKeyHash
	{
		std::size_t operator()(const std::array<double, 6>& k) const
		{
			std::size_t h = 0;
			for (const double v : k)
				h = h * 1000003u ^ std::hash<double>()(v);
			return h;
		}
	};
	mutable std::unordered_map<std::array<double, 6>, bool, WalkKeyHash> walkMemo; // straightWalkClear, per Ctx (one step)
	std::uint64_t ringRouted = 0; // the fighter whose circle() walked a route round an obstacle this tick: its integrate()'s direct steer must not undo it
	double clipT0 = 0.0;
	double clipT1 = 0.0;
	bool clipAdvanced = false;
	// The Ids a squadmate follows this step (Fighter::followTarget, saved), sorted: a leader that is followed walks at the
	// group's pace. Built once per step from the saved follow orders, so a save and the straight run agree.
	std::vector<std::uint64_t> followed_;

	explicit Ctx(CombatArena& a)
		: arena(a)
		, rules(a.rules_)
		, data(a.rules_.data())
	{
	}

	// The measured steering radii (CADENCE_LIVE.md M2): `setCombatFollow(inner, outer)` args of the states that steer. State 1 (defend) and
	// state 3 (between, pending defend) pass CombatClass +0x284 / +0x288 = 11.5 / 23; WaitingStateAI (state 6, the recovery hold) passes
	// n x 0.25 + 15 and n x 3 + 25, n = the TARGET's engaged list size + the refusals it made last step (crowd_melee.md §10.4; our M2
	// capture read the waiter's own count, which the asm corrects). States 0, 4 and 8 make
	// no steer call. The follow compares the CENTRE distance with args - footprint 5.6 + own radius + target radius (CADENCE_LIVE.md
	// Item 34 (d): 11.5 / 23 act as 13.9 / 25.4, Waiting 15.25 / 28 as 17.65 / 30.4; live radial speed inside the ring 0.00 / 0.01 / 0.09).
	// Retired: the 5.6 offset on the hold only and plain args in state 3 (CADENCE_SLICE.md "Footwork", UNSURE then).
	static constexpr double kFootprint = 5.6;
	static constexpr double kFollowOffset = 2.0 * CombatRules::kCharWidth - kFootprint; // = +2.4 on the args
	static constexpr double kDefendInner = 11.5;
	static constexpr double kDefendOuter = 23.0;
	// AttackState::enter's failed-start walk (MEASURED.md item 16, kinspect 2dea8f9): when tryStartAttack starts nothing,
	// `enter` sets the ring [6, 35.5] and halves it in the same call to [3, 17.75]; after the follow's clamps the steer radii
	// act as (1, 12.15) in centre distance. The fighter closes to it and re-picks every frame. That is how a low-skill unarmed
	// fighter reaches its one admissible technique: `ma badpunch` (skill <= 20) is admitted at d <= half + attackDistance = 5,
	// i.e. a centre distance of 15 (task BI: while this walk held at the old ringOuter - 1 = 25 the imported unarmed brawlers
	// never got there and never swung again).
	static constexpr double kAttackApproachInner = 1.0;
	static constexpr double kAttackApproachOuter = 12.15;
	Steer defendRing(bool backOff = false) const { return {true, kDefendInner + kFollowOffset, kDefendOuter + kFollowOffset, kTuneWalkCap, false, backOff}; }
	// The state-6 hold (and the circle's Wait) pushes out when inside the ring's inner edge while the steer is on (CharMovement +0x378,
	// kinspect 48 (c)); `Member::hold` tracks the mode. Retired exception (`pending == Defend && target attacking`, stale `pending`):
	// retired/combat_waiting_ring_pending_exception.txt.
	Steer waitingRing(const Member& m)
	{
		// crowd_melee.md §10.4: n is the TARGET's engaged list size plus the refusals it made last step (the crowd the waiter is
		// queued behind), never the waiter's own slots. A waiter is only here after a full-list refusal, so a vanilla ring has n >= 5.
		double n = 0.0;
		if (const Member* target = m.fighter.target != 0 ? member(m.fighter.target) : nullptr)
			n = static_cast<double>(target->fighter.engaged.size()) + static_cast<double>(refusalsOf(*target));
		return {true, n * 0.25 + 15.0 + kFollowOffset, n * 3.0 + 25.0 + kFollowOffset, kTuneWalkCap, false, m.fighter.holdSteer.steerOn};
	}
	// Kinspect 48 (c): the pending flag is set on the frame the opponent starts a swing (whatever the holder is doing), cleared when the
	// holder starts its own swing or the opponent's swing ends. Set inside a hold it turns the steer off (1-2 exe frames later, less than
	// one of our ticks, so at once). Outside a hold the steer is on exactly when nothing is pending.
	void trackPending(Member& m)
	{
		Fighter& f = m.fighter;
		const Member* t = f.target != 0 ? member(f.target) : nullptr;
		const bool opp = t != nullptr && t->fighter.state == CombatState::Attack;
		const bool own = f.state == CombatState::Attack;
		if (opp && !m.fighter.holdSteer.oppSwing)
		{
			m.fighter.holdSteer.pending = true;
			if (m.fighter.holdSteer.inHold)
				m.fighter.holdSteer.steerOn = false;
		}
		if ((own && !m.fighter.holdSteer.ownSwing) || !opp)
			m.fighter.holdSteer.pending = false;
		if (own || f.state == CombatState::Defend)
			m.fighter.holdSteer.came = f.state;
		m.fighter.holdSteer.oppSwing = opp;
		m.fighter.holdSteer.ownSwing = own;
		if (!f.recovering)
			m.fighter.holdSteer.inHold = false;
		if (!m.fighter.holdSteer.inHold)
			m.fighter.holdSteer.steerOn = !m.fighter.holdSteer.pending;
	}
	double ringInner() const { return arena.rules_.settings().ringInner; }
	double ringOuter() const { return arena.rules_.settings().ringOuter; }
	double hitReachBase() const { return arena.rules_.settings().hitReachBase; }
	double hitReachPerDistance() const { return arena.rules_.settings().hitReachPerDistance; }

	// ---- helpers ----
	void indexFollows()
	{
		followed_.clear();
		for (const Member& m : arena.members_)
			if (m.fighter.followTarget != 0 && m.fighter.followTarget != m.id)
				followed_.push_back(m.fighter.followTarget);
		std::sort(followed_.begin(), followed_.end());
	}

	bool isFollowed(std::uint64_t id) const
	{
		return std::binary_search(followed_.begin(), followed_.end(), id);
	}

	Member* member(std::uint64_t id)
	{
		auto it = std::lower_bound(arena.members_.begin(), arena.members_.end(), id,
			[](const Member& m, std::uint64_t v) { return m.id < v; });
		if (it == arena.members_.end() || it->id != id)
			return nullptr;
		it->dirty = true; // a mutable handle into another member: a possible write, so the mirror copies it
		return &*it;
	}

	void emit(CombatEvent::Kind kind, std::uint64_t who, std::uint64_t other, std::int32_t code = 0,
		std::int32_t code2 = 0, std::int32_t technique = -1, double a = 0.0, double b = 0.0, const std::string& part = std::string())
	{
		if (kind == CombatEvent::Kind::LimbSevered)
			arena.severs_.push_back({who, part});
		if (!arena.observer_)
			return;
		CombatEvent e;
		e.part = part;
		e.kind = kind;
		e.time = arena.time_;
		e.who = who;
		e.other = other;
		e.code = code;
		e.code2 = code2;
		e.technique = technique;
		e.a = a;
		e.b = b;
		arena.observer_(e);
	}

	// EVENT 48 (task BG, PHYSICS.md 2-3): a ragdoll start. Cause 0 a KO, 1 a death, 2 a drop/put-down (emitted by the carry
	// step), 3 a launch (a hit that moves an already-down body). `attacker` is the hit's attacker (0 = a collapse), `strength`
	// the velocity change at the part (0 for a collapse). The lying yaw and the seed come from the fighter's own ragdoll stream;
	// the event goes out right after the KO / death event of the same tick.
	void emitRagdoll(Fighter& victim, std::uint64_t id, std::int32_t cause, std::uint64_t attacker, const std::string& part,
		double strength, double pushX = 0.0, double pushZ = 0.0, double push = 0.0)
	{
		const std::uint32_t seed = arena.rollRagdoll(victim, id, pushX, pushZ, push);
		if (!arena.observer_)
			return;
		CombatEvent e;
		e.kind = CombatEvent::Kind::Ragdoll;
		e.time = arena.time_;
		e.who = id;
		e.other = attacker;
		e.code = cause;
		e.a = strength;
		e.seed = seed;
		e.part = part;
		arena.observer_(e);
	}

	// Different teams, or MQ14 path 2: `a` runs ATTACK_ENEMIES_AND_NEUTRALS and `b` is not bonded to it (same faction or allied: the
	// arena's bonded set), or the other way round while `b` is engaged (it has a target): the attacked, and whoever sees an aggressor
	// at work, answer it. An aggressor that has not picked anyone yet is not an enemy of the neutrals it has not attacked.
	bool enemy(const Fighter& a, const Fighter& b) const
	{
		if (b.dead)
			return false;
		// Task BP: a shackled slave is not its captor's enemy, nor its captor's allies': the captive walks along with the squad that
		// chained it instead of being re-engaged. A squad the slave attacked still answers (the personal list, checked first), and a
		// fighter with an explicit attack order (a player's rebellion, or a slave owner's discipline) is never spared it.
		// rwh.99 section 5 (FUN_14059e300 + 14032e2a0): a candidate that is a slave with its 187 row, of the same owner as the picker's own slave
		// state, is not scored: the master's slaves are not each other's targets (OURS: the owner is the captor faction, the chain byte is the state).
		// The deference below is the row's too (k16.6.116 (b)): without the 187 row (order 77) the slave is an ordinary fighter again.
		if (a.obedient && b.obedient && a.captorFaction != 0 && a.captorFaction == b.captorFaction && a.orderTarget == 0)
			return false;
		if (a.orderTarget == 0)
		{
			if (a.obedient && !hostileTo(b, a.squad) && arena.captiveOf(a.captorFaction, b.faction))
				return false;
			if (b.obedient && !hostileTo(a, b.squad) && arena.captiveOf(b.captorFaction, a.faction))
				return false;
		}
		// The team partition: a faction hostile to the player (worldTeam) and the factionless wild squads (kWildTeam) answer here.
		if (a.team != b.team)
			return true;
		// Task BV: the original's enemy test (sensor::enemy, FUN_14079c950): the observer's faction view of the target, plus the personal
		// type-4 list (the same list combat grew up with), for pairs the team partition shares - every faction neutral to the player
		// is team 0. The relation comes from Simulation::sensorContext (task BU).
		if (sensor::enemy(arena.sensorContext(), a, b))
			return true;
		return goalTarget(a, b) && goalInRange(a, b);
	}

	// BQ10 (kinspect bq10_goal21.md): the squad's personal type-4 list comes before the faction relation.
	static bool hostileTo(const Fighter& a, std::uint64_t squad)
	{
		return std::binary_search(a.hostileSquads.begin(), a.hostileSquads.end(), squad);
	}

	// The goal's hour window, checked on every evaluation (start == end: always open; start > end wraps midnight).
	bool goalActive(const Fighter& a) const
	{
		if (!a.attacksNeutrals)
			return false;
		const double hour = arena.environment_.dayFraction * 24.0;
		if (a.aggroStart == a.aggroEnd)
			return true;
		return a.aggroStart < a.aggroEnd ? hour >= a.aggroStart && hour < a.aggroEnd : hour >= a.aggroStart || hour < a.aggroEnd;
	}

	// Goal 21's target filter (finder 14059f720): not own squad, not a unique squad, not an ally (same faction, relation >= 50) and not a
	// coexistence faction. (Dead, KO'd or carried, unsensed: usable() / aware at the call sites.)
	bool goalTarget(const Fighter& a, const Fighter& b) const
	{
		return goalActive(a) && a.squad != b.squad && !b.uniqueSquad && !arena.bonded(a.faction, b.faction) && !arena.coexists(a.faction, b.faction);
	}

	// Range (140599f30): within ~6517 u (d^2 42467328), or in the aggressor's home building. ("Already fighting me" is the personal list.)
	static bool goalInRange(const Fighter& a, const Fighter& b)
	{
		return centre(a, b) <= kGoalRange || (a.homeBuilding != 0 && b.building == a.homeBuilding);
	}

	// onTargetedBy (140999f00): the whole squad of `side` takes the other fighter's squad as a personal hostile.
	void markHostile(const Member& side, const Fighter& other)
	{
		for (Member& mate : arena.members_)
		{
			if (mate.fighter.squad != side.fighter.squad)
				continue;
			mate.dirty = true;
			auto& list = mate.fighter.hostileSquads;
			const auto at = std::lower_bound(list.begin(), list.end(), other.squad);
			if (at == list.end() || *at != other.squad)
				list.insert(at, other.squad);
		}
	}

	// The same squad map's AGGRESSOR bit (2, task BZ; dt23): the whole squad of `side` records the other fighter's squad as having attacked
	// it. Written where an attack task starts (setTarget's victim flip, the same place as the type-4 entry), read by goal 34 as "already
	// fighting me". A squad's own attack on a troublemaker writes it on the troublemaker's table too - from the victim's side the joiner is
	// the aggressor, which is what the map records.
	void markAggressor(const Member& side, const Fighter& other)
	{
		for (Member& mate : arena.members_)
		{
			if (mate.fighter.squad != side.fighter.squad)
				continue;
			mate.dirty = true;
			auto& list = mate.fighter.aggressorSquads;
			const auto at = std::lower_bound(list.begin(), list.end(), other.squad);
			if (at == list.end() || *at != other.squad)
				list.insert(at, other.squad);
		}
	}
	// A carried character is nobody's target and acts for nobody (carry.hpp).
	static bool usable(const Fighter& f) { return !f.dead && !f.unconscious && f.carriedBy == 0; }
	// A down (conscious, limb-collapsed) or KO'd body is not a target (task BK; kinspect 84a2ab0, findings/downed_target_combat.md):
	// the original's attack task effect is isTargetDown and CombatClassAI::update invalidates a down / KO target (a down candidate
	// scores -10 at the candidate stage, which we never reach). A down fighter still crawls and still swings at others; it is only
	// never picked.
	// A body in a cage or prison is nobody's victim (kenshi-k16.6.29; the original's prisoner targeting is not decoded, UNSURE, deferred: playtesting):
	// this one test gates every pick, keep, chase and shot (targetable(), setTarget, rangedFire), so a held target on a caged body is dropped.
	static bool caged(const Fighter& f) { return f.cageBuilding != 0; }
	static bool targetable(const Fighter& f) { return usable(f) && !f.down && !caged(f); }

	// The race's 'pathfind footprint radius' (human 2.6, Goat 4, Garru / Bull 7, Dog / Spider 10, Turtle 13, Beak Thing 20, Cage Beast 24,
	// Leviathan 40: read from the RACE data, BQ12 e). UNSURE: animals scale it by a size lerp (char+0x6f0 / 0x6f4 / 0x700) we do not model.
	double footprintOf(const Fighter& f) const
	{
		const RaceCombat* race = data.race(f.raceSid);
		return race != nullptr ? race->footprintRadius : kAvoidFootprint;
	}

	// The traveller of f's routes: its footprint and its race's water cost scale (PATHFINDING_STATIC.md item 4).
	Body bodyOf(const Fighter& f) const { return data.body(f.raceSid); }

	static double centre(const Fighter& a, const Fighter& b)
	{
		return std::hypot(a.x - b.x, a.z - b.z);
	}

	// tryStartAttack's pick distance (MEASURED.md item 18; CADENCE_LIVE.md Item 34 (c), Item 36 (b)): 3D distance - cc+0x278 (1.5 w =
	// 6.0) - k * the target's cc+0x1a0, floored at 0. The live d is the centre distance - 9.5 (the gate's d equals the combatlog d to
	// 0.001), so the target term is 3.5, not w = 4: k = lerp(0.45, 1.0, vf+0x390) and the scale s is not identified (UNSURE), so 3.5 is the measured constant.
	// NOT adopted: with 3.5 (centre - 9.5) the 40-seed clock-2 bleed median is 0.145, over the hard 0.141 edge (chaos, CADENCE_SLICE.md
	// "Follow-up 2"); the term stays w = 4 (centre - 10) until the bleed row is understood. One-constant switch.
	static constexpr double kPickTargetTerm = CombatRules::kCharWidth;
	// pickTarget's distance range (crowd_melee.md §10): (1 - d/range) x 2, i.e. 0.01 per unit, so one crowd member costs 100 u.
	static constexpr double kPickRange = 200.0;
	static constexpr double kMeleeReach = 8.0; // OURS (UNSURE): where a target counts as "melee attacking me" for iAmBeingMeleeAttacked
	static double pickDistance(const Fighter& a, double bx, double by, double bz)
	{
		const double dy = a.y - by;
		const double d3 = std::sqrt((a.x - bx) * (a.x - bx) + dy * dy + (a.z - bz) * (a.z - bz));
		return std::max(0.0, d3 - 1.5 * CombatRules::kCharWidth - kPickTargetTerm);
	}
	static double pickDistance(const Fighter& a, const Fighter& b) { return pickDistance(a, b.x, b.y, b.z); }

	// ---- the distance between a pair: the walk, not the crow (task AQ) ----
	// The user's rule (2026-11-06, the Hub Storm House playtest): a fight measures the way you must walk, not how
	// tall the building is. A guard on a roof and someone on the floor below are far apart when the only way
	// between them is round and up the stairs, and a roof no walk reaches is out of range. The straight line is
	// only the cheap first reject (a walk is never shorter than the crow); the answer is the route length plus
	// the gap from its end to the target, so a target the route only gets near still counts its distance.
	// OURS (ENGAGEMENT.md "Fights across floors"): a pair's length is cached while neither end moves more than
	// kPathCacheMove and the sample is younger than kPathCacheAge (a door opening is seen within that); the hit
	// gates allow kPathSlack past their box or threat reach; a route ending more than kPathEndHeight from the
	// target's own height is on another floor, i.e. no route.
	static constexpr double kPathCacheMove = 4.0;
	static constexpr double kPathCacheAge = 2.0;
	static constexpr double kPathEndHeight = 10.0;
	static constexpr double kPathSlack = 4.0;

	static double straight(const Fighter& a, const Fighter& b)
	{
		const double dy = a.y - b.y;
		return std::sqrt((a.x - b.x) * (a.x - b.x) + dy * dy + (a.z - b.z) * (a.z - b.z));
	}

	// The body's clearance first, then the bare mesh when clearance finds none (task BN). A bake can leave a passage
	// narrower than the body (BJ: an ~8 u `Area::Edge` band) where the original's own navtiles have open land; the
	// clearance-free route exists and is walkable, so the fighter closes on a reachable enemy instead of giving it up.
	// Only a pair with no route at either clearance is genuinely unreachable (a roof island, a locked door).
	static Route routeForBody(const MeshNavigator& n, const knav::Position& from, double tx, double tz,
		std::optional<double> ty, Body body)
	{
		Route route = n.routeFrom(from, tx, tz, ty, false, body);
		if (!route.found && body.radius > 0.0 && !route.plainFailed)
		{
			n.noteRadiusZeroRetry();
			route = n.routeFrom(from, tx, tz, ty, false, Body{0.0, body.water});
			route.radiusFallback = true;
		}
		return route;
	}

	double pathLength(const Member& a, const Member& b)
	{
		const Fighter& fa = a.fighter;
		const Fighter& fb = b.fighter;
		const MeshNavigator* n = nav();
		if (n == nullptr)
			return straight(fa, fb); // a bare harness: the straight line is the path
		const auto key = a.id < b.id ? std::make_pair(a.id, b.id) : std::make_pair(b.id, a.id);
		if (const auto it = arena.pathCache_.find(key); it != arena.pathCache_.end())
		{
			const CombatArena::PathSample& s = it->second;
			if (static_cast<double>(arena.stepCount_ - s.tick) * dt <= kPathCacheAge
				&& std::hypot(std::hypot(fa.x - s.ax, fa.y - s.ay), fa.z - s.az) <= kPathCacheMove
				&& std::hypot(std::hypot(fb.x - s.bx, fb.y - s.by), fb.z - s.bz) <= kPathCacheMove)
			{
				++arena.pathCacheHits_;
				return s.length;
			}
		}
		++arena.pathQueries_;
#if KSIM_PROFILE
		const knav::measure::WhoScope whoScope(a.id, 1);
#endif
		const Route route = routeForBody(*n, {fa.x, fa.y, fa.z}, fb.x, fb.z, fb.y, bodyOf(fa));
		double length = std::numeric_limits<double>::infinity();
		if (route.found && !route.points.empty() && std::fabs(route.heights.back() - fb.y) <= kPathEndHeight)
		{
			length = 0.0;
			double px = fa.x;
			double py = fa.y;
			double pz = fa.z;
			for (std::size_t i = 0; i < route.points.size(); ++i)
			{
				length += std::hypot(std::hypot(route.points[i].x - px, route.heights[i] - py), route.points[i].z - pz);
				px = route.points[i].x;
				py = route.heights[i];
				pz = route.points[i].z;
			}
			length += std::hypot(std::hypot(fb.x - px, fb.y - py), fb.z - pz);
		}
		arena.pathCache_[key] = CombatArena::PathSample{length, fa.x, fa.y, fa.z, fb.x, fb.y, fb.z, arena.stepCount_};
		return length;
	}

	// The straight line first (never shorter than the walk), then the cached route within `limit`.
	bool withinPathReach(const Member& a, const Member& b, double limit)
	{
		if (straight(a.fighter, b.fighter) > limit)
			return false;
		return pathLength(a, b) <= limit;
	}

	// The original's melee line check (cc+0x2c0, kenshi-rwh.186 static / rwh.187 live): a navmesh-only straight walk from attacker to target,
	// recomputed every update; with it clear a fight engages and swings, with it blocked the fighter paths round to the target's side first
	// (state 4 -> 10 -> 11) and never starts a swing across. A table is a hole in that mesh (the furniture ring), a wall is too; the damage
	// box stays prop-blind. Never a collision raycast (user 2026-11-18, k16.6.415: a table never blocks sight or a shot). No navmesh: clear.
	bool straightWalkClear(const Fighter& a, const Fighter& b) const
	{
		const MeshNavigator* n = nav();
		if (n == nullptr)
			return true;
		// One walk per exact endpoint pair per step (inEngageRange, notDirect and approach all ask, several times per fighter): a fighter
		// that moved is a new key, so the answer is the one the uncached walk gives (kenshi-k16.6.444).
		const std::array<double, 6> key{a.x, a.y, a.z, b.x, b.y, b.z};
		const auto hit = walkMemo.find(key);
		if (hit != walkMemo.end())
			return hit->second;
		const bool clear = n->straightWalk(a.x, a.y, a.z, b.x, b.y, b.z);
		walkMemo.emplace(key, clear);
		return clear;
	}

	// tryEngageTarget (MEASURED.md item 18): the walk - target radius 4 <= cached reach (stats+0x150) + cc+0x27c,
	// where cc+0x27c = 1.5 w + D = 24. The live gate measured the crow on a flat floor; task AQ spends the same
	// budget on the path (straight - w on the left, 1.5 w + D on the right).
	bool inEngageRange(Member& m, const Member& target)
	{
		Fighter& f = m.fighter;
		if (f.cachedReach < 0.0)
			f.cachedReach = rules.weaponChangedReach(f);
		const double limit = f.cachedReach + 2.5 * CombatRules::kCharWidth + CombatRules::weaponLength(f);
		return withinPathReach(m, target, limit) && straightWalkClear(f, target.fighter);
	}

	// ---- the floors of a chase (task AS) ----
	// A target on another floor: the crow says it is close, but only a stair (or a ramp) reaches it, so the chase
	// must plan a route to its 3D point and never steer straight at its xz. OURS, kFloorApart.
	static bool floorApart(const Fighter& a, const Fighter& b)
	{
		const double gap = std::fabs(a.y - b.y);
		return gap > kFloorApart && gap > kFloorSlope * std::hypot(a.x - b.x, a.z - b.z);
	}

	// "No route" is only a fact about the world where the navigator has a mesh: at its streaming edge a route can
	// fail for lack of coverage. Both ends must be on it.
	bool onCoverage(const Fighter& a, const Fighter& b) const
	{
		const MeshNavigator* n = nav();
		return n != nullptr && n->covers(a.x, a.z) && n->covers(b.x, b.z);
	}

	// The target cannot be reached at all (task AS): a roof with no stair, a locked door. The original re-scores
	// visible hostiles after a second (crowd_melee.md 1-2); ours drops the target and skips it for kBlockedTarget
	// (the original's `addBlockedTarget`, as the 60 U clear-orders does), so pickTarget takes another enemy.
	// A player's explicit attack order stands: the player is never given up on here.
	// Task BN (kinspect 7726815, dt4): the block is the attacker's own (the victim may still target the attacker), and it
	// is re-tested, never latched. At expiry the exe re-queries the navmesh at the target's position: reachable, the entry
	// is erased and the target may be picked at once; unreachable, the 20 s timer is re-armed. An NPC's task order is
	// cleared: kept, pickTarget's order path re-sets the target every tick and the block is never seen (the roof-island
	// order loop).
	void giveUpUnreachable(Member& m, const Member& target)
	{
		Fighter& f = m.fighter;
		if (f.playerControlled)
			return;
		f.orderTarget = 0;
		f.aggroBlockId = target.id;
		f.aggroBlock = kBlockedTarget;
		setTarget(m, 0);
		f.chaseId = 0;
		f.forwardSpeed = 0.0;
		f.clearRoute(Fighter::kClearGiveUp);
	}

	// A target within the approach reach that no walk reaches: the same give-up, decided without a chase.
	bool unreachableNear(const Member& m, const Member& target)
	{
		return !m.fighter.playerControlled
			&& centre(m.fighter, target.fighter) <= kTuneApproachDistance
			&& onCoverage(m.fighter, target.fighter)
			&& !std::isfinite(pathLength(m, target));
	}

	const Technique* tech(const Fighter& f) const
	{
		return f.technique >= 0 ? &data.techniques[static_cast<std::size_t>(f.technique)] : nullptr;
	}

	double clipLength(const Technique& t) const
	{
		// Task BQ: an animal technique has no clip; its length is the measured time to the first impact (Technique::length).
		if (t.length > 0.0)
			return t.length;
		return t.clip >= 0 ? data.clips[static_cast<std::size_t>(t.clip)].length : 1.0;
	}

	double clipProgress(const Fighter& f) const
	{
		const Technique* t = tech(f);
		return t == nullptr ? -1.0 : f.clipTime / clipLength(*t);
	}

	bool clipPlaying(const Fighter& f) const { return f.technique >= 0 && !f.techniqueFinished; }

	// The strafe lock's root-motion branch (ENGAGEMENT 9.1): while a technique's clip plays, the clip moves the body and no
	// other code does. A block clip carries no root motion; the state-1 footwork moves the blocker instead. Task BQ: an animal
	// technique has no clip but still commits (Technique::length), so it is locked the same way.
	bool rootLocked(const Fighter& f) const
	{
		if (!clipPlaying(f))
			return false;
		const Technique& t = data.techniques[static_cast<std::size_t>(f.technique)];
		return (t.clip >= 0 || t.length > 0.0) && !(t.isBlock && !t.isDodge);
	}

	// +0x140: swing progress to the impact.
	double computeSwingProgress(const Fighter& f) const
	{
		if (f.state == CombatState::Stumble)
			return -1.0;
		const Technique* t = tech(f);
		if (t == nullptr || t->isBlock || t->isDodge || f.state != CombatState::Attack)
			return 0.0;
		if (f.techniqueFinished)
			return 1.0;
		const std::size_t idx = static_cast<std::size_t>(
			std::min<std::int32_t>(f.impactIndex, static_cast<std::int32_t>(t->impacts.size()) - 1));
		const double raw = clipProgress(f) / t->impacts[idx].hitFrac;
		return raw >= 1.0 ? 1.0 : raw;
	}

	void setState(Member& m, CombatState to, bool force = false)
	{
		Fighter& f = m.fighter;
		if (f.state == to)
			return;
		if (f.state == CombatState::Stumble && !force)
			return; // setState ignores writes while in 8
		emit(CombatEvent::Kind::State, m.id, 0, static_cast<std::int32_t>(to),
			static_cast<std::int32_t>(f.state));
		f.state = to;
		f.stateTime = 0.0;
	}

	void stopTechnique(Fighter& f)
	{
		f.technique = -1;
		f.techniqueFinished = false;
		f.impactIndex = 0;
		f.lastImpact = 0;
		f.hitListBuilt = false;
	}

	// ---- attack slots (the victim's table, ENGAGEMENT.md 6) ----
	std::size_t slotLoop(const Fighter& owner) const
	{
		// The original loops to the global constant, not the effective count (the quirk).
		return arena.rules_.settings().fixExtraAttackSlots
			? owner.slots.size()
			: std::min<std::size_t>(owner.slots.size(), static_cast<std::size_t>(data.k.maxNumAttackSlots));
	}

	std::size_t slotCount(const Fighter& owner) const
	{
		if (owner.state == CombatState::Attack || owner.state == CombatState::Stumble || owner.down)
			return 1;
		const RaceCombat* race = data.race(owner.raceSid);
		const std::int32_t extra = race != nullptr ? race->extraAttackSlots : 0;
		return static_cast<std::size_t>(std::clamp(data.k.maxNumAttackSlots + extra, 1, 5));
	}

	bool tryAcquire(Member& owner, std::uint64_t holder)
	{
		Fighter& o = owner.fighter;
		const std::size_t count = std::min(slotCount(o), o.slots.size());
		for (std::size_t i = 0; i < count; ++i)
		{
			if (o.slots[i].holder == holder)
				return true;
		}
		for (std::size_t i = 0; i < count; ++i)
		{
			if (o.slots[i].holder == 0)
			{
				o.slots[i] = state::FighterSlot{holder, 0.0};
				emit(CombatEvent::Kind::SlotAcquire, owner.id, holder, static_cast<std::int32_t>(i));
				return true;
			}
		}
		return false;
	}

	void releaseSlot(Member& owner, std::uint64_t holder)
	{
		Fighter& o = owner.fighter;
		const std::size_t n = slotLoop(o);
		for (std::size_t i = 0; i < n; ++i)
		{
			if (o.slots[i].holder == holder)
			{
				o.slots[i] = {};
				emit(CombatEvent::Kind::SlotRelease, owner.id, holder, static_cast<std::int32_t>(i));
			}
		}
	}

	void clearAllSlots(Fighter& o)
	{
		const std::size_t n = slotLoop(o);
		for (std::size_t i = 0; i < n; ++i)
			o.slots[i] = {};
	}

	void tickSlots(Member& owner)
	{
		Fighter& o = owner.fighter;
		const std::size_t n = slotLoop(o);
		for (std::size_t i = 0; i < n; ++i)
		{
			state::FighterSlot& slot = o.slots[i];
			if (slot.holder == 0)
				continue;
			slot.timer += dt;
			if (slot.timer > 5.0)
			{
				Member* holder = member(slot.holder);
				const bool keep = holder != nullptr && usable(holder->fighter)
					&& holder->fighter.swingProgress > 0.0 && slot.timer <= 20.0;
				if (!keep)
					slot = {};
			}
		}
	}

	// ---- engaged list (+0x1f8) ----
	// The target's refusals last step (the crowd penalty's crowd count, crowd_melee.md §10). The saved Fighter::overflow carries
	// the same count (step()'s write-back), so a load resumes with the penalty the straight run has.
	std::uint32_t refusalsOf(const Member& target) const
	{
		return target.refused[arena.refusePhase_ ^ 1];
	}

	bool addEngaged(Member& owner, std::uint64_t attacker)
	{
		Fighter& o = owner.fighter;
		if (std::find(o.engaged.begin(), o.engaged.end(), attacker) != o.engaged.end())
			return true;
		if (o.engaged.size() > 4)
		{
			++owner.refused[arena.refusePhase_];
			return false;
		}
		o.engaged.push_back(attacker);
		owner.fightTier = true;
		return true;
	}

	void removeEngaged(Member& owner, std::uint64_t attacker)
	{
		auto& list = owner.fighter.engaged;
		list.erase(std::remove(list.begin(), list.end(), attacker), list.end());
	}

	// `observer` becomes (or stays) aware of `other` (onTargetedBy, 140999f00: every frame while engaged, so a fight never
	// loses its target to the memory); the transition is the Noticed event.
	// `known` false: the observer picked `other` as its own target (heard counts as aware here), which is not the exe's seen_now (k16.6.419,
	// rwh.152: a victim that only heard the assassin must not turn into a knowing one by targeting it). A target told of its attacker is known.
	void learn(Member& observer, std::uint64_t other, bool known = true)
	{
		if (perception::notice(observer.fighter, other, known))
			emit(CombatEvent::Kind::Noticed, observer.id, other);
	}

	// rwh.191 (findings/perception_cheese_mourn_hold.md): a ranged attacker the victim cannot sense (behind a wall, held aiming for hours) is never on
	// the victim's list: the victim learns of it when a bolt hits (landBlow) or its own scan senses it. The attacker still knows its target.
	// Only the ordered attack (goal 5) and the ranged tasks (262 / 263 / 266) count: a bandit that auto-picks a sneaker it heard still tells it (stealth KO).
	static bool unsensedShooter(const Fighter& attacker, std::uint64_t attackerId, const Fighter& victim, std::uint16_t goal)
	{
		const bool shooting = goal == kAttackOrderGoal || (goal >= 262 && goal <= 266);
		return shooting && attacker.gun().armed() && attacker.gun().ammo >= 1 && !perception::sensedWithin(victim, attackerId, perception::kRecordFresh);
	}

	// ---- targets ----
	// `goal` is the AI_TASK whose goal picked the target (task AT, the Goal row): recorded even when the target does not change, since a
	// player's attack order re-picks the same enemy every tick. 0 keeps the goal in hand; a dropped target clears it.
	void setTarget(Member& m, std::uint64_t id, std::uint16_t goal = 0)
	{
		Fighter& f = m.fighter;
		if (id != 0)
		{
			if (const Member* victim = member(id); victim != nullptr && caged(victim->fighter))
				id = 0;
		}
		if (id == 0)
			f.combatGoal = 0;
		else if (goal != 0)
			f.combatGoal = goal;
		if (f.target == id)
			return;
		if (f.target != 0)
		{
			if (Member* old = member(f.target))
			{
				removeEngaged(*old, m.id);
				releaseSlot(*old, m.id);
			}
		}
		f.target = id;
		if (id != 0)
		{
			m.fightTier = true;
			f.comfortRolls += 1.0; // a melee task start: the comfort radii re-roll (BQ12 a)
			Member* now = member(id);
			if (now != nullptr && unsensedShooter(f, m.id, now->fighter, goal))
			{
				learn(m, id, false);
				now = nullptr;
			}
			if (now != nullptr)
			{
				addEngaged(*now, m.id); // the target learns of its attacker (140999f00)
				if (now->fighter.target == 0)
					now->fighter.orderInterrupt = true; // not in combat: onTargetedBy requests a replan (Simulation::stepOrderCombat)
				learn(*now, m.id);
				learn(m, id, false);
				// onTargetedBy also clears the victim's stealth (1405ca9a0(c, 0); kinspect STEALTH_CLEAR.md section 2): a sneaking
				// character that is targeted stops sneaking, so it can answer its attacker (task sneak-break; the user's
				// bar_lure: a sneaking player stood still while the Dust Bandits cut it down). Unconditional, as the exe's
				// call is: no hostility, awareness, distance or player gate; every setTarget is a notification.
				if (now->fighter.sneak)
					clearSneak(now->fighter, SneakClear::Targeted);
				// BQ10 K5: targeting flips the victim's whole squad to hostile (a personal type-4 entry), unless the victim's faction is
				// allied to the attacker's; other same-faction squads are not flipped. Different teams are enemies already.
				// BQ14: a character with a package (joinDriven) is flipped across teams too: a bar guard targeted by a bandit lists the
				// bandit's squad, and the bandit targeted by a guard lists the guard's (its goal 16 then answers).
				// Task BZ: the same flip writes the AGGRESSOR bit (dt23): the victim's squad records MY squad as the attacker, so its
				// squadmates' goal 34 joins the fight ("already fighting me"). The exe gates the tag on the town/crime side; that gate is
				// not modelled (UNSURE).
				if ((now->fighter.team == f.team || now->joinDriven) && !arena.bonded(now->fighter.faction, f.faction))
				{
					markHostile(*now, f);
					markAggressor(*now, f);
				}
			}
		}
	}

	// My held target, while it can be hit. Not re-tested with enemy(): the attack task owns its target until the target is down,
	// dead or gone (dt23: the personal type-4 map is written and never read as a "keep fighting" flag, and goal 34's target - a
	// troublemaker, admitted though no relation makes it an enemy - is fought by its task). The swing's own filter admits the
	// current target too (applyImpacts).
	Member* targetOf(Member& m)
	{
		Member* t = m.fighter.target != 0 ? member(m.fighter.target) : nullptr;
		return t != nullptr && targetable(t->fighter) ? t : nullptr;
	}

	// SELF_PRESERVATION's hit memory lasts the fight it was made in: an entry goes when its attacker is gone (no member),
	// dead, or out of the fight (no target and no engaged attacker), and all of them go once the victim's own fight is
	// over (no target and no engaged attacker). Read before every use, so a stale hit never lifts the range for ever.
	void pruneHitBy(Member& m)
	{
		if (!perception::inCombat(m.fighter))
		{
			m.hitBy.clear();
			return;
		}
		std::erase_if(m.hitBy, [&](std::uint64_t id)
		{
			const Member* foe = member(id);
			return foe == nullptr || foe->fighter.dead || !perception::inCombat(foe->fighter);
		});
	}

	// SELF_PRESERVATION's range gate (goal 32): `foe` targets this member and is within kSelfDefenceRange of walking
	// (task AQ: the player's combat-proximity trigger is a path length too, so a character on a roof does not turn on
	// one below it), or it has landed a hit on it (the ranged shot the live victim turned on at once).
	bool selfDefenceRange(const Member& m, const Member& foe)
	{
		return std::find(m.hitBy.begin(), m.hitBy.end(), foe.id) != m.hitBy.end()
			|| withinPathReach(m, foe, kSelfDefenceRange);
	}

	// The nearest enemy that targets this character and passes that gate (goal 32). The task drop and joinFight's self
	// case read it; nullptr when nobody does.
	Member* selfDefence(Member& m)
	{
		pruneHitBy(m);
		const Fighter& f = m.fighter;
		Member* best = nullptr;
		double bestD = 0.0;
		for (Member& foe : arena.members_)
		{
			if (!usable(foe.fighter) || foe.fighter.target != m.id || !enemy(f, foe.fighter) || !perception::aware(f, foe.id)
				|| !selfDefenceRange(m, foe))
				continue;
			const double d = centre(f, foe.fighter);
			if (best == nullptr || d < bestD)
			{
				best = &foe;
				bestD = d;
			}
		}
		return best;
	}

	// PROTECT_ALLIES (SQUAD_FOLLOW.md s9, measured on the player faction): an idle character, one with no
	// running task (following, a move order or a drive), targets the nearest enemy that
	// is attacking an ally (itself included) and that it is aware of (perception.hpp: the measured cut-off
	// of 730 joined / 1030 never is the cone and light reach, see PERCEPTION_LIVE.md and SQUAD_FOLLOW.md s9). No
	// automatic whole-squad join: the attacker has to have registered (target set), and each squadmate decides alone.
	// SELF_PRESERVATION (goal 32, kinspect f5784bb + c1a305f) is the self case: an attacker on the character itself
	// is answered only within kSelfDefenceRange or after it has hit, and that half also runs under a task (a following
	// player fights back; UNSURE, goal 32 presumably outranks the follow task). PROTECT_ALLIES for OTHER allies is
	// unchanged: idle characters only, and never a sneaking one (task sneak-break).
	// What a holder can act on from its anchor (rwh.179: goal 15 fires, the body stays, the crossbow is still used): a target in close range, or in
	// ranged reach of an armed crossbow. A melee-only holder gets no goal against a farther target: it would hold it without moving or swinging.
	bool holdReach(const Fighter& f, const Fighter& target, double closeRange) const
	{
		return centre(f, target) <= closeRange || (f.ranged.armed() && f.ranged.ammo >= 1 && CombatArena::inRangedReach(f, target));
	}

	Member* joinFight(Member& m)
	{
		const Fighter& f = m.fighter;
		pruneHitBy(m);
		const bool tasked = f.taskRunning();
		Member* best = nullptr;
		double bestD = 0.0;
		for (Member& foe : arena.members_)
		{
			// BQ10: an aggressor at work against an ally (goal 21 target filter, no range) is joined too; the joiner then holds the
			// attacker's squad hostile for as long as the fight lasts.
			if (!usable(foe.fighter) || foe.fighter.target == 0)
				continue;
			const Member* ally = member(foe.fighter.target);
			if (ally == nullptr || ally->fighter.dead)
				continue;
			// Plan 13 ATTACK_ATTACKERS_OF (14099c3b0, rwh.228 s4): the attacker comes from the ally's aggressor list; the ally senses it fresh OR
			// I hold any record of it (existence), and it stands within 150 u of the ally (22500 = DAT_1416e345c). My own attacker needs a record.
			if (foe.fighter.target == m.id)
			{
				if (!perception::aware(f, foe.id))
					continue;
			} else {
				if ((!perception::sensedNow(ally->fighter, foe.id) && perception::find(f, foe.id) == nullptr) || !withinAllyReach(foe.fighter.target, foe))
					continue;
			}
			const double d = centre(f, foe.fighter);
			const bool hostile = enemy(f, foe.fighter) || goalTarget(foe.fighter, f);
			if (foe.fighter.target == m.id)
			{
				if (!hostile)
					continue;
				// SELF_PRESERVATION: my own attacker, within range or after it has hit me.
				if (!selfDefenceRange(m, foe) || (f.holdPosition && d > kHoldRange))
					continue;
			} else {
				// A sneaking character still does not join an ALLY's fight (SQUAD_FOLLOW.md s9: Izumi sneaking 30 u away never
				// joined; with sneak off she joined 33 frames later). Its OWN attacker is answered: every clear in the exe's
				// exhaustive list (kinspect STEALTH_CLEAR.md 71fa402: targeted, swing, hit, follow stop) runs through
				// clearSneak, so an engaged character is never still sneaking and self-defence arrives with the flag off.
				// OURS/UNSURE: the exe's join is gated by PROTECT_ALLIES' own conditions, not by the flag; we keep the flag as
				// the ally-fight gate because it is the one measured blocker (s9) and the clears above cover the rest.
				// rwh.99 section 5 (AI::vf14): PROTECT_ALLIES is utility 0 for a slave that carries its 187 row; its own attacker is still answered.
				// A passive member (CharStats+0x12c, rwh.148 3a: goal 15 utility 0) and a holder (3b) joins it (rwh.179: goal 15 fires under Hold,
				// the body stays at its anchor) only where it can act from there: holdReach. Both still answer their own attacker (goal 32; the holder within kHoldRange).
				if (f.sneak || tasked || f.obedient || f.passive)
					continue;
				// The attacker of an ally need not be hostile to ME (k16.6.238, user playtest: crime guards beating a thief of the
				// squad, hostile to the thief only): PROTECT_ALLIES answers whoever attacks the ally. Not my own squad (a spar, a duel).
				// A bonded attacker is NOT skipped (rwh.148: no special code; the crime's -20 per witness firing ends the bond before the fight).
				if (!hostile && f.squad != 0 && foe.fighter.squad == f.squad)
					continue;
				// The exe's ally (goal 15): the same faction, a personal friend (not modelled) or relation >= 50, i.e. the arena's bonded set.
				// NOT every team-0 character: worldTeam gives every non-hostile faction the player's team, so a townsperson fighting a bandit
				// used to pull an idle player into a fight 800 u away (user 2026-11-04).
				// An ally of another team is no ally (two factionless fighters are "bonded" for the wild squads, but the victim of a fight
				// against my squadmate is the enemy, not something to defend).
				if (!((f.squad != 0 && ally->fighter.squad == f.squad) || (ally->fighter.team == f.team && arena.bonded(f.faction, ally->fighter.faction))))
					continue;
				// Goal 15's own gates (rwh.179 / rwh.189 s3): MY sensor holds the ALLY, seen or heard now (not just the attacker), and the plan's
				// task 13 takes only an attacker within 150 u of the ally. A far animal that has picked a target pulls nobody in.
				if (!perception::sensedNow(f, ally->id) || centre(ally->fighter, foe.fighter) > kAttackerOfRadius)
					continue;
			}
			if (best == nullptr || d < bestD)
			{
				best = &foe;
				bestD = d;
			}
		}
		if (best != nullptr)
		{
			// onTargetedBy runs on the VICTIM (dt23): the joined squad takes my squad as a personal hostile; my own table gets nothing
			// (the attack task holds the target).
			if (!enemy(f, best->fighter))
				markHostile(*best, f);
			if (best->fighter.target != m.id)
				learn(m, best->id); // 140858f40(...,0): the winner is force-noticed into my senses
			// The goal this join runs (task AT, the Goal row): SELF_PRESERVATION for my own attacker, PROTECT_ALLIES for an ally's.
			const std::uint16_t joinGoal = best->fighter.target == m.id ? 32 : 15;
			setTarget(m, best->id, joinGoal);
			if (std::vector<CombatArena::FightCandidate> none; fightTraced(m))
				traceFight(m, best->id, joinGoal, false, none); // reported when made, not at the next full pick
		}
		return best;
	}

	// The hand's attacker: the fighter the fighting squadmate / ally is fighting. The exe's task 13 finder is not decoded; ours reads
	// the hand's own attack target (OURS/UNSURE).
	std::uint64_t attackerOf(std::uint64_t hand)
	{
		const Member* protector = hand != 0 ? member(hand) : nullptr;
		const Member* attacker = protector != nullptr && protector->fighter.target != 0 ? member(protector->fighter.target) : nullptr;
		// Task 13 takes only an attacker within 150 u of the ally (rwh.189 s3).
		return attacker != nullptr && centre(protector->fighter, attacker->fighter) <= kAttackerOfRadius ? attacker->id : 0;
	}

	// Goal 288's only radius (kenshi-rwh.215, task 13 at 14099c3b0): the ally's attacker is within 150 u OF THE ALLY (22500 squared).
	// There is no town leash: 288's resolver is goal 15's, and leaving town ends nothing.
	bool withinAllyReach(std::uint64_t ally, const Member& attacker)
	{
		const Member* a = member(ally);
		if (a == nullptr)
			return false;
		const double dx = attacker.fighter.x - a->fighter.x;
		const double dz = attacker.fighter.z - a->fighter.z;
		return dx * dx + dz * dz <= 22500.0;
	}

	// The combat goals' live evaluators (G4; dt24_143 / dt24_15 / dt24_16): the hand is the goal's target - the attacker for 15 / 143,
	// the enemy for 16.
	// Someone targeting me within melee reach (iAmBeingMeleeAttacked; OURS, UNSURE).
	bool beingMeleed(const Member& m) const
	{
		for (const Member& other : arena.members_)
			if (other.id != m.id && !other.fighter.dead && other.fighter.target == m.id && centre(m.fighter, other.fighter) <= kMeleeReach)
				return true;
		return false;
	}

	// The ranged task this fighter's attack runs as (task ds/ranged S2): the exe remaps the attack to 262 / 263 / 266 when the hand holds a
	// crossbow (CURSORS_LIVE C4; 263 the focused order, 266 the unprovoked one). 0 = the melee swing: no crossbow or no bolts, a down body, or
	// someone in melee reach (task 262's iAmBeingMeleeAttacked = 0 precondition: the shooter fights with its melee weapon then).
	std::uint16_t rangedTask(const Member& m) const
	{
		const Fighter& f = m.fighter;
		if (!f.ranged.armed() || f.ranged.ammo < 1 || f.down || beingMeleed(m))
			return 0;
		return f.combatGoal == kAttackOrderGoal ? goap::kRangedAttackFocused
			: f.combatGoal == kUnprovokedFocusedGoal ? goap::kRangedAttackFocusedUnprovoked : goap::kRangedAttack;
	}

	// One tick of a stealth KO (dt41_C, Task_StealthKO): approach the victim to 14 u, play the "knockout" clip, roll at half its progress, hold to its end.
	// Gone with the victim, or the assassin hurt, down or carried. The task aborts, before the roll, when the victim's sensor knows the assassin.
	void stealthKoStep(Member& m)
	{
		Fighter& f = m.fighter;
		Member* v = member(f.stealthKo.target);
		if (v == nullptr || v->fighter.dead || v->fighter.unconscious || v->fighter.carriedBy != 0 || f.down || f.unconscious || f.carriedBy != 0)
		{
			f.stealthKo = {};
			return;
		}
		Fighter& victim = v->fighter;
		const double dx = victim.x - f.x;
		const double dz = victim.z - f.z;
		const double distance = std::hypot(dx, dz);
		if (f.stealthKo.phase == 0)
		{
			if (stealthko::victimKnows(victim, m.id))
			{
				arena.stealthKos_.push_back({m.id, v->id, 2, 0.0, 0.0});
				emit(CombatEvent::Kind::StealthKo, v->id, m.id, 2); // SIMLINK event 50
				f.stealthKo = {};
				f.clearRoute();
				f.forwardSpeed = 0.0;
				return;
			}
			if (distance >= stealthko::kApproachDistance)
			{
				const double speed = std::min(runSpeed(f), distance / dt);
				if (nav() != nullptr)
				{
					chaseStep(f, victim.x, victim.z, std::nullopt, speed, true, true);
				} else {
					f.yaw = wrapAngle(f.yaw + std::clamp(wrapAngle(std::atan2(dx, dz) - f.yaw), -kPi * dt, kPi * dt));
					f.forwardSpeed = speed;
					shift(f, std::sin(f.yaw) * speed * dt, std::cos(f.yaw) * speed * dt);
				}
				return;
			}
			f.stealthKo.phase = 1;
			f.stealthKo.elapsed = 0.0;
		}
		f.clearRoute();
		f.forwardSpeed = 0.0;
		f.lateralSpeed = 0.0;
		f.yaw = std::atan2(dx, dz);
		f.stealthKo.elapsed += dt;
		if (f.stealthKo.phase == 1 && f.stealthKo.elapsed >= stealthko::kClipSeconds * stealthko::kResolveProgress)
		{
			f.stealthKo.phase = 2;
			resolveStealthKo(m, *v);
		}
		if (f.stealthKo.elapsed >= stealthko::kClipSeconds)
			f.stealthKo = {};
	}

	// State 2: the assault is recorded (before the roll, so a failed KO is an assault too), the roll is made, and the victim is knocked out for the
	// forced-KO time or force-notices and turns on the assassin. XP is withheld when the roll fails against a fighting victim that knew the assassin.
	void resolveStealthKo(Member& m, Member& v)
	{
		Fighter& f = m.fighter;
		Fighter& victim = v.fighter;
		arena.attacks_.push_back({m.id, v.id});
		const bool knew = perception::aware(victim, m.id);
		const double chance = stealthko::chance(f, victim, m.id);
		const double roll = draw(f.rng) * 100.0;
		const bool success = roll < chance;
		const double scale = victim.cageBuilding != 0 ? stealthko::kXpCaged : 1.0;
		if (success)
		{
			const double missing = std::max(rules.maxBlood(victim) - victim.blood, 0.0);
			const bool was = victim.unconscious;
			victim.koTimer = std::max(victim.koTimer, forcedKoSecondsParam(data.k.knockoutTimeBase, data.k.knockoutMult1, data.k.knockoutMult99,
				missing, victim.skills.toughness, f.skills.assassination * 0.01));
			victim.unconscious = true;
			if (!was)
			{
				emit(CombatEvent::Kind::Knockout, v.id, m.id);
				emitRagdoll(victim, v.id, 0, m.id, std::string(), 0.0);
			}
			rules.addXp(f.skills.assassination, scale * stealthko::kXpSuccess * stealthko::kXpScale);
		} else {
			if (!(knew && perception::inCombat(victim)))
				rules.addXp(f.skills.assassination, scale * stealthko::kXpFailure * stealthko::kXpScale);
			perception::notice(victim, m.id); // the assassin is force-noticed, the two are temporary enemies, the victim is targeted by it
			arena.markHostile(v.id, m.id);
			arena.markHostile(m.id, v.id);
			clearSneak(victim, SneakClear::Targeted);
		}
		arena.stealthKos_.push_back({m.id, v.id, static_cast<std::uint8_t>(success ? 0 : 1), chance, roll});
		emit(CombatEvent::Kind::StealthKo, v.id, m.id, success ? 0 : 1, 0, -1, chance); // SIMLINK event 50
	}

	// One tick of a mounted gunner: stand at the post, take a target, shoot it when it is in reach, in the arc and in sight (235, the shot).
	void mannedStep(Member& m)
	{
		Fighter& f = m.fighter;
		f.rangedTask = 0;
		f.clearRoute();
		f.forwardSpeed = 0.0;
		f.lateralSpeed = 0.0;
		if (const Member* t = pickTarget(m); t != nullptr)
		{
			const std::uint64_t id = t->id;
			const CombatArena::Shot shot = arena.rangedFire(m.id, id, dt);
			if (shot == CombatArena::Shot::Waiting || shot == CombatArena::Shot::Missed || shot == CombatArena::Shot::Hit)
			{
				f.rangedTask = goap::kShootAtTarget;
				if (const Member* aimed = member(id))
					f.yaw = std::atan2(aimed->fighter.x - f.x, aimed->fighter.z - f.z);
			}
		}
	}

	bool combatHolds(goap::Sym sym, const Member& m, std::uint64_t hand)
	{
		const Fighter& self = m.fighter;
		const Member* t = member(hand);
		switch (sym)
		{
		case goap::Sym::IsMovementAllowed: return !self.dead && !self.down && !self.unconscious && self.cageBuilding == 0;
		case goap::Sym::ReadyForAction: return true; // OURS: the exe's isReadyForAction reads the weapon hand, not modelled here
		case goap::Sym::OperatesTurret: return self.mount.mounted();
		case goap::Sym::HoldsCrossbow: return rangedTask(m) != 0; // the ranged task applies: bolts, standing, nobody in melee reach
		case goap::Sym::HasWeaponEquipped: return self.ranged.armed() || self.weapon.sid != data.unarmed.sid;
		case goap::Sym::HasTwoWorkingArms: return !self.down; // OURS (UNSURE): arm damage is not read
		case goap::Sym::IsCrouchingOrLying: return self.down;
		case goap::Sym::IsWithin50Meters: return t != nullptr && centre(self, t->fighter) <= 50.0;
		case goap::Sym::IsTargetDown:
			return t != nullptr && (t->fighter.dead || t->fighter.down || t->fighter.unconscious);
		case goap::Sym::IsAggressionTowards:
			return t != nullptr && t->fighter.target != 0; // it is fighting someone (the protected member, OURS: any target)
		case goap::Sym::IsAlliesFighting:
			for (const Member& other : arena.members_)
			{
				if (other.id == m.id || other.fighter.dead || other.fighter.unconscious || other.fighter.target == 0)
					continue;
				if ((self.squad != 0 && other.fighter.squad == self.squad)
					|| (self.faction != 0 && arena.bonded(self.faction, other.fighter.faction)))
					return true;
			}
			return false;
		case goap::Sym::IsEnemy: return t != nullptr && enemy(self, t->fighter);
		case goap::Sym::IAmBeingMeleeAttacked: return beingMeleed(m); // 248's precondition (false)
		default: return false;
		}
	}

	// The goal's plan against the live world (dt18 section 4 / section 6): no plan = the goal scores 0 this tick, re-planned next.
	goap::Plan combatPlan(const Member& m, std::uint16_t goal, std::uint64_t hand)
	{
		CombatPlanWorld world;
		world.eval = [this, &m, hand](goap::Sym sym, std::uint64_t, double, double) { return combatHolds(sym, m, hand); };
		return goap::plan(world, goal, hand, 0.0, 0.0);
	}

	// The goal that lets this pick happen (task AT, the Goal row). A package-driven character runs its package's joining goals through
	// their resolvers (G3, task CF) and the planner (G4): 143 for a fighting squadmate's attacker, 15 for an ally's, 16 for a hostile,
	// 21 for a hostile or neutral, 34 for a troublemaker in my town, 144 for a hostile inside my home ring, 41 for a slave's master's
	// enemies (task raid-goals). The candidate-only admissions (hand16 / hand21 / hand34 / hand144 / hand41) are not hands: the resolver
	// buffers and the picker chooses (dt24_16 / dt24_21 / dt24_34 / dt24_144 / dt24_41). A character with no package (a duel, a test) still
	// fights its attacker under SELF_PRESERVATION and anyone else under ATTACK_ENEMIES.
	std::uint16_t joinGoal(const Member& m, const Member& other, std::uint64_t hand143, std::uint64_t hand15, bool hand16, bool hand21,
		bool hand34, bool hand144, bool hand41, bool hand101, bool hand248, std::uint64_t hand288)
	{
		// 32 SELF_PRESERVATION is a goal like the others: the attacker is admitted (cond 39) and the goal's plan [13, 32] decides.
		if (other.fighter.target == m.id && combatPlan(m, routines::task::SelfPreservation, other.id).found())
			return routines::task::SelfPreservation;
		if (!m.joinDriven)
			return 16;
		if (hand288 != 0 && attackerOf(hand288) == other.id && withinAllyReach(hand288, other)
			&& combatPlan(m, routines::task::ProtectAlliesStayInTown, other.id).found())
			return routines::task::ProtectAlliesStayInTown;
		if (hand21 && combatPlan(m, routines::task::AttackEnemiesAndNeutrals, other.id).found())
			return routines::task::AttackEnemiesAndNeutrals;
		if (hand143 != 0 && attackerOf(hand143) == other.id
			&& combatPlan(m, routines::task::ProtectOwnSquad, other.id).found())
			return routines::task::ProtectOwnSquad;
		if (hand15 != 0 && attackerOf(hand15) == other.id
			&& combatPlan(m, routines::task::ProtectAllies, other.id).found())
			return routines::task::ProtectAllies;
		if (hand16 && combatPlan(m, routines::task::AttackEnemies, other.id).found())
			return routines::task::AttackEnemies;
		if (hand41 && combatPlan(m, routines::task::AttackSlavemasterEnemies, other.id).found())
			return routines::task::AttackSlavemasterEnemies;
		if (hand101 && combatPlan(m, routines::task::DefeatSquad, other.id).found())
			return routines::task::DefeatSquad;
		if (hand248 && combatPlan(m, routines::task::DefeatSquadLimitChase, other.id).found())
			return routines::task::DefeatSquadLimitChase;
		if (hand34 && combatPlan(m, routines::task::AttackTroubleMakers, other.id).found())
			return routines::task::AttackTroubleMakers;
		if (hand144 && combatPlan(m, routines::task::TerritorialAggression, other.id).found())
			return routines::task::TerritorialAggression;
		return 0;
	}

	// The candidate scan of the goal pick (task fight-goals: pure, so the goal selector can ask it too; pickTarget commits what it
	// returns). Every member the fighter targets, holds or admits through its package goals, scored by the picker (14059e300 stand-in).
	Member* scanCandidates(Member& m, std::uint16_t& goal, std::vector<CombatArena::FightCandidate>* seen = nullptr)
	{
#if KSIM_PROFILE
		struct ScanTimer
		{
			ThinkProbe& p;
			std::uint64_t began = knav::measure::now();
			~ScanTimer() { if (p.on) { ++p.scanCalls; p.scanNs += knav::measure::now() - began; } }
		} scanTimer{think};
#endif
		const Fighter& f = m.fighter;
		std::uint64_t hand143 = 0;
		std::uint64_t hand15 = 0;
		std::uint64_t hand288 = 0;
		const sensor::Context& ctx = arena.sensorContext();
		const auto lookup = [this](std::uint64_t id) -> const Fighter*
		{
			const Member* found = member(id);
			return found != nullptr ? &found->fighter : nullptr;
		};
		// The town I stand in (the exe's Town object, `Character vf+0x78`): goal 144's home ring, and goal 34's `myTown == 0`
		// gate with `getTown(t) == myTown` (dt24_34). The circle is the exe's (buildings::townRadius, see perception.hpp); the
		// same building is the admission's place test. The 500 u live-observed area of BQ14 is retired (retired/town_join_sight.txt).
		const int myTown = arena.environment_.townOf(f.x, f.z);
		const bool haveHome = myTown >= 0;
		const double homeX = haveHome ? arena.environment_.towns[myTown].x : 0.0;
		const double homeZ = haveHome ? arena.environment_.towns[myTown].z : 0.0;
		const double homeRadius = haveHome ? arena.environment_.towns[myTown].radius : 0.0;
		const bool hasTown = haveHome;
		const auto sameTown = [this, &f, myTown](const Fighter& other)
		{
			if (myTown >= 0 && arena.environment_.townOf(other.x, other.z) == myTown)
				return true;
			return f.building != 0 && other.building == f.building;
		};
		const auto tagged = [&f](std::uint64_t squad)
		{
			return std::binary_search(f.aggressorSquads.begin(), f.aggressorSquads.end(), squad);
		};
		if (m.joinDriven)
		{
			// Passive: goal 15 only (utility 0); hold: both squad goals (it does not run off to fight for anyone).
			if ((m.joinGoals & joins::kProtectSquad) != 0 && !f.holdPosition)
				hand143 = worldres::protectOwnSquad143(f, ctx, lookup);
			if ((m.joinGoals & joins::kProtectAllies) != 0 && !f.passive)
				hand15 = worldres::protectAllies15(f, ctx, lookup);
			if ((m.joinGoals & joins::kProtectAlliesTown) != 0 && !f.passive && !f.holdPosition)
				hand288 = worldres::protectAllies15(f, ctx, lookup);
		}
		Member* best = nullptr;
		double bestScore = 0.0;
		// AI+0xbc (rwh.228 s1.4): the count of records hostile and capable (not downed, dead, carried), remembered ones included. Goal 16 searches only
		// while it is non-zero.
		int someoneToFight = -1; // lazy: only a member with goal 16 in its join goals asks
		std::uint16_t bestGoal = 0;
		// Only a member the observer is aware of can be a candidate, and its sight records are sorted by id like members_: walk
		// those (a handful) instead of every member, in the same order, so the pick and its ties are unchanged.
		for (const state::SightRecord& sight : f.sight)
		{
			if (!sight.seen || sight.target == m.id)
				continue;
			Member* const found = member(sight.target);
			if (found == nullptr || !targetable(found->fighter))
				continue;
			Member& other = *found;
			// Task BV (2026-11-07): hostility is directional (FUN_14079c950, kinspect dt11), so two factions neutral to the player - the
			// Bar's crew and a Dust Bandit squad, say - share team 0 and enemy() is false between them. A joinDriven character still
			// joins the troublemaker goal 34's resolver admits (task CF, dt24_34: criminal - running an attack task, or tagged
			// AGGRESSOR on my squad's table - in my town), and the join writes the personal type-4 entry below, which makes the pair
			// real enemies for enemy(), targetOf and the swing. Never an ally, and never the player's own party: an assault inside a
			// town is the crime system's, not this join.
			const bool foe = enemy(f, other.fighter);
			// The task owns a target it has picked while the target is targetable (task BZ, as in the exe; dt23): a goal-34 troublemaker
			// that walks out of the building is kept and followed, with no re-admission - the same-building clause is the admission for
			// the first pick only. Only a goal-34 target: a goal-21 chase still ends when its hour window closes (ksim_aggressive K2).
			const bool held = other.id == f.target && f.combatGoal == 34;
			const bool admissible = m.joinDriven && !other.fighter.playerControlled && !arena.bonded(f.faction, other.fighter.faction);
			if (!foe && !held && !admissible)
				continue;
			// No plan, no pick (dt18 section 6): the goal scores 0 this tick and is re-planned next - never a terminal state, and no
			// hand-written fallback (task CF). Each goal admits this candidate (the resolvers buffer them all); the score below is the
			// picker's choice among the admitted ones (14059e300, our stand-in).
			std::uint16_t otherGoal = 0;
			if (held)
				otherGoal = f.combatGoal;
			else
			{
				// FRESH (rwh.212 / rwh.228): the held target (the goal's own, NONE strength) is not re-tested; a fresh one passes only while AI+0xbc
				// is set, and a conscious down body inside a town is dropped (14059f3a0's downed-in-a-town rule, a slave of mine excepted).
				const auto freshFight = [&]
				{
					if (other.fighter.down && other.fighter.slaveState != 1 && arena.environment_.townOf(other.fighter.x, other.fighter.z) >= 0)
						return false; // downed in a town
					if (someoneToFight < 0)
						someoneToFight = worldres::hostileCapable(f, ctx, lookup) ? 1 : 0;
					return someoneToFight == 1 && worldres::attackEnemies16(f, ctx, other.id, lookup);
				};
				const bool hand16 = m.joinDriven && (m.joinGoals & joins::kAttackEnemies) != 0
					&& ((other.id == f.target && f.combatGoal == 16) || freshFight());
				const bool hand21 = m.joinDriven && (m.joinGoals & joins::kAttackNeutrals) != 0
					&& worldres::attackEnemiesAndNeutrals21(f, ctx, other.id, lookup);
				const bool hand34 = m.joinDriven && (m.joinGoals & joins::kTroubleMakers) != 0
					&& worldres::troubleMaker34(f, ctx, other.id, lookup, m.id, hasTown, sameTown, tagged);
				const bool hand144 = m.joinDriven && (m.joinGoals & joins::kTerritorial) != 0
					&& worldres::territorial144(f, ctx, other.id, lookup, m.id, haveHome, homeX, homeZ, homeRadius);
				const bool hand41 = m.joinDriven && (m.joinGoals & joins::kMasterEnemies) != 0
					&& worldres::masterEnemies41(f, ctx, other.id, lookup, m.id);
				const bool squadFoe = m.joinDriven && (m.joinGoals & (joins::kDefeatSquad | joins::kDefeatSquadChase)) != 0
					&& worldres::defeatSquad101(f, ctx, other.id, lookup, m.id);
				const bool hand101 = squadFoe && (m.joinGoals & joins::kDefeatSquad) != 0;
				const bool hand248 = squadFoe && (m.joinGoals & joins::kDefeatSquadChase) != 0;
				otherGoal = joinGoal(m, other, hand143, hand15, hand16, hand21, hand34, hand144, hand41, hand101, hand248, hand288);
			}
			if (otherGoal == 0)
				continue;
			if (f.aggroBlock > 0.0 && other.id == f.aggroBlockId)
				continue; // a neutral chase that gave up: not again for a while
			const double d = centre(f, other.fighter);
			double crowd = static_cast<double>(other.fighter.engaged.size()) + static_cast<double>(refusalsOf(other));
			const bool currentTarget = other.id == f.target;
			if (currentTarget)
				crowd -= 1.0;
			double score = (1.0 - d / kPickRange) * 2.0 - crowd; // 0.01 per unit: one crowd member costs as much as 100 u
			if (currentTarget)
			{
				score += 5.0; // the current target
				if (other.fighter.engaged.size() < 2)
					score += 10.0; // the task target, while fewer than 2 attack it
			}
			// Task dt25 (45 BODYGUARD, kinspect dt25_bodyguard_unarmed_putdown.md section 1.5): the protected hand. A candidate that
			// is fighting the character my bodyguard goal guards scores +10 (the exe's DAT_14167fad4), while fewer than two attack
			// it (the engagement counters < 2) and it is not already the charge's own target (the exe's AI::vf5 counterpart test:
			// the guard does not crowd a fight the charge has in hand). OURS: the candidate's "counterpart" is its current target
			// (the exe also reads a recent-attacker record and its combat object), and our protected hand comes from a package's
			// bodyguard row or a player's 45 job rather than the exe's order hand.

			if (f.protectTarget != 0 && other.fighter.target == f.protectTarget && other.fighter.engaged.size() < 2)
			{
				const Fighter* charge = lookup(f.protectTarget);
				if (charge != nullptr && charge->target != other.id)
					score += 10.0;
			}
			if (seen != nullptr)
				seen->push_back({other.id, otherGoal, (1.0 - d / kPickRange) * 2.0, crowd, score});
			if (best == nullptr || score > bestScore)
			{
				bestScore = score;
				best = &other;
				bestGoal = otherGoal;
			}
		}
		goal = bestGoal;
		return best;
	}

	// The decision trace's fight hook (kenshi-k16.6.59): reports a pick only when the (goal, target) differs from the last one reported
	// for this member, so a target held for N ticks adds one row.
	void traceFight(const Member& m, std::uint64_t target, std::uint16_t goal, bool order, std::vector<CombatArena::FightCandidate>& seen)
	{
		const auto last = arena.fightLast_.try_emplace(m.id, std::pair<std::uint16_t, std::uint64_t>{0, 0}).first;
		if (last->second.first == goal && last->second.second == target)
			return;
		CombatArena::FightPick pick;
		pick.member = m.id;
		pick.previousGoal = last->second.first;
		pick.previousTarget = last->second.second;
		pick.target = target;
		pick.goal = goal;
		pick.order = order;
		for (const CombatArena::FightCandidate& c : seen)
		{
			if (c.id == target)
				pick.score = c.score;
		}
		pick.candidates = std::move(seen);
		last->second = {goal, target};
		arena.fightSink_(pick);
	}

	// A counter-retarget made outside pickTarget is reported when made, not at the member's next full pick (which a busy member reaches late).
	void traceCounter(const Member& m)
	{
		if (std::vector<CombatArena::FightCandidate> none; m.fighter.target != 0 && fightTraced(m))
			traceFight(m, m.fighter.target, m.fighter.combatGoal, false, none);
	}

	bool fightTraced(const Member& m) const
	{
		return arena.fightSink_ && (!arena.fightWants_ || arena.fightWants_(m.id));
	}

	// `pickTarget`: keep the target for a second, then the nearest usable enemy the character is aware of (its sensor
	// has seen or heard it within the memory: perception.hpp; this replaces the 600 unit proxy).
	Member* pickTarget(Member& m)
	{
#if KSIM_PROFILE
		if (think.on && think.depth == 0)
		{
			static constexpr int kSlot[12] = {0, 1, -1, 2, 3, 4, 5, -1, 6, 7, -1, 8}; // by CombatState value
			const int state = static_cast<int>(m.fighter.state);
			if (state >= 0 && state < 12 && kSlot[state] >= 0)
				++think.byState[static_cast<std::size_t>(kSlot[state])];
		}
		const ThinkTimer thinkTimer(think);
#endif
		Member* const held = targetOf(m);
		Member* picked = pickTargetPlain(m);
		// HOLD POSITION: no target beyond close range, so no chase (a player's attack order is its own task and stands).
		if (picked != nullptr && m.fighter.holdPosition && m.fighter.orderTarget == 0
			&& !holdReach(m.fighter, picked->fighter, picked == held ? kHoldKeep : kHoldRange))
		{
			setTarget(m, 0);
			return nullptr;
		}
		return picked;
	}

	Member* pickTargetPlain(Member& m)
	{
		Fighter& f = m.fighter;
		// Task BR: a character that is down (a limb collapse, a crawl) cannot fight: it takes no target, joins no fight and lets go of
		// the one it held. The original unequips and idles (kinspect dt1). Without this a down player kept answering its attacker
		// through joinFight, and a squad that had knocked it out stayed "in a fight" on both sides for ever (the client's live run
		// kept three guards and a bandit leader engaged 100 s past the KO, and no capture or loot could start).
		// A body in a cage or prison cannot reach anyone: it picks no target either (kenshi-k16.8.1; the original's prisoner targeting is not decoded,
		// UNSURE, deferred: playtesting). Otherwise a freed-to-fight prisoner targets the guard, whose squad then lists the prisoner's as hostile for good.
		if (f.down || f.cageBuilding != 0)
		{
			if (f.target != 0)
				setTarget(m, 0);
			return nullptr;
		}
		if (f.orderTarget != 0)
		{
			// A player's attack order: stays on that enemy, at any distance, until it is down. Not gated on enemy(): the order is an
			// attack task of its own (the player may attack a neutral), and the swing admits its current target.
			Member* ordered = member(f.orderTarget);
			if (ordered != nullptr && targetable(ordered->fighter))
			{
				setTarget(m, ordered->id, kAttackOrderGoal);
				if (std::vector<CombatArena::FightCandidate> none; fightTraced(m))
					traceFight(m, ordered->id, kAttackOrderGoal, true, none);
				return ordered;
			}
			f.orderTarget = 0;
		}
		// RUN_AWAY_FORCED (DIALOGUE.md 5, step 6b): a fleeing character takes no target, so its run is not dropped for a fight. UNSURE
		// whether the original's runner fights back when attacked (kinspect 6b question).
		if (f.fleeing || f.goal.kind == state::IndividualKind::RunAway)
		{
			setTarget(m, 0);
			return nullptr;
		}
		Member* current = targetOf(m);
		if (f.playerControlled)
		{
			// One action at a time: a running interact / lock / lift order stands over the fight; the body is only hit, and
			// Simulation::stepOrderCombat restarts the task on each targeting (rwh.165). No special case per order kind.
			if (f.orderBusy)
			{
				if (f.target != 0)
					setTarget(m, 0);
				return nullptr;
			}
			// An explicit move order stands over the player's own fight (the SELF_PRESERVATION row; OURS): the character runs
			// the order and joins nothing. Without this the first non-walk tick (a clip, a stagger) let `joinFight` set a target
			// and `approach` plan a chase route over the order's, after which `walking()` was false for the rest of the run and
			// the character circled at ring speed instead of running (the user's "gave up running and glided", task BE).
			if (f.walking() && !f.driving())
			{
				if (f.target != 0)
					setTarget(m, 0);
				return nullptr;
			}
			// A player character never picks fights by itself (SQUAD_FOLLOW.md s9): it keeps a target
			// until it is down, and otherwise joins one only through PROTECT_ALLIES. A target that is down or gone is dropped
			// first: kept, it held the wire's combat flag on with no enemy left (user playtest 2026-11-05; ksim_follow).
			if (current != nullptr)
				return current;
			if (f.target != 0)
				setTarget(m, 0);
			return joinFight(m);
		}
		// The hold (rwh113): the held target stands, unscored and unchecked, while `+0x2f0` < 1.0 s (< 0.3 s with CharStats+0x12b,
		// non-combat; not modelled, no character carries it here, so the one threshold: UNSURE). The timer counts up in the tick and
		// only a re-pick resets it (setTarget does not). `combat.retarget_hold_s` is the A/B switch (default 1.0 = the exe).
		if (current != nullptr && f.targetAge < arena.rules_.settings().retargetHold)
			return current;
		f.targetAge = 0.0;
		// The pick's score (crowd_melee.md §10 as corrected by kinspect ai_target_hand.md, tasks AU / BE): the distance term minus
		// the candidate's crowd, plus the exe's bonuses: +5 for the current target (the exe x4 under the picker's CharStats+0xe0
		// posture, which we do not model) and +10 for the task target while fewer than 2 attack it. The candidate set is NOT taken
		// from the exe: live it is the AI's own engaged list, its task target's engaged list and the task target, filled by a goal
		// finder we do not have; restricting our scan to it made the Bar's crew stick to the one bandit it first picked and the
		// other bandits never answered (ksim_aggro_drop, BE) - our scan stands in for that finder (OURS / UNSURE). UNSURE also: our
		// held target is both the combat and the task target (the exe keeps two hands), so both bonuses apply to it; the exe's crowd
		// x0.35 under a candidate's CharStats+0x12a, its +2.0 / -1.0 flags, its carrying -1 and the picker's +0x12b x0.01 beyond
		// 40 u are not modelled; the refusals keep the last-step count from crowd_melee §10 as part of the crowd (UNSURE).
		// Task CF (dt24_143 / dt24_15 / dt24_16): a package-driven character's hands, from the goals' G3 resolvers - a squadmate that
		// is fighting (143), an ally that is fighting (15); goal 16 has no single hand (it buffers every hostile candidate and the pick
		// below chooses, dt24_16). Each goal bit is the package's (join_rules.hpp).
		std::uint16_t goal = 0;
		std::vector<CombatArena::FightCandidate> seen;
		const bool traced = fightTraced(m);
		Member* best = scanCandidates(m, goal, traced ? &seen : nullptr);
		if (best == nullptr)
		{
			setTarget(m, 0);
			if (traced)
				traceFight(m, 0, 0, false, seen);
			return nullptr;
		}
		// The picked fighter that is no enemy of mine (the goal 34 troublemaker): the personal type-4 entry goes into ITS squad's map,
		// keyed by mine (dt23). My own table gets nothing - the task holds the target and my swing admits its current target.
		if (!enemy(f, best->fighter))
			markHostile(*best, f);
		setTarget(m, best->id, goal);
		emit(CombatEvent::Kind::Goal, m.id, best->id, goal); // the pick's task type (task CF fixtures; the wire's goal label too)
		if (traced)
			traceFight(m, best->id, goal, false, seen);
		return best;
	}

	// The sensors (perception.hpp): each character scans once per scanInterval, staggered by its dice key (task CC; the Id before,
	// which moved the first scan of every character when anything spawned before it), before anyone moves.
	void senseAll()
	{
		const double interval = perception::scanInterval(arena.members_.size());
		std::vector<perception::Other> others;
		std::vector<perception::Noticed> noticed;
		for (Member& m : arena.members_)
		{
			Fighter& f = m.fighter;
			if (!usable(f))
				continue;
			// An NPC outside a fight scans at the scaled interval at game speed above 1 (k16.6.633); a player character and a fighter keep it.
			const double mine = arena.environment_.cadenceScale > 1 && !f.playerControlled && f.target == 0 && f.engaged.empty()
				? interval * arena.environment_.cadenceScale : interval;
			// A due-stamp: `senseAt` is the time the clock counts from, so a member that is not due is not written (nothing owed, nothing dirty).
			const double now = arena.time_;
			if (f.senseAt <= Fighter::kNoStamp * 0.5)
			{
				f.senseAt = now - dt - mine * perception::stagger(arena.diceKey(m.id)); // the counted clock took this step's dt on the spot
				m.dirty = true;
			}
			if (now - f.senseAt < mine)
				continue;
			m.dirty = true;
			if (others.empty())
				for (const Member& o : arena.members_)
					others.push_back({o.id, &o.fighter});
			if (arena.environment_.weatherAt)
				arena.environment_.weatherAt(f.x, f.z, arena.environment_.weatherType, arena.environment_.weatherStrength);
			const RaceCombat* race = data.race(f.raceSid);
			perception::scan(m.id, f, race != nullptr ? race->visionRangeMult : 1.0, now - f.senseAt, others, arena.environment_,
				noticed);
			f.senseAt = now;
		}
		for (const perception::Noticed& n : noticed)
			emit(CombatEvent::Kind::Noticed, n.observer, n.target);
	}

	// The worn gear's weather protection for one affect type (weather_acid_rain.md section 3): the plain, unclamped sum of
	// `weather protection amount` over the pieces that list the type (1.3 = total protection).
	static double weatherProtection(const Fighter& f, int type) noexcept
	{
		double p = 0.0;
		for (const state::FighterArmour& a : f.armour)
			if (type >= 0 && type < 32 && (a.weatherTypes & (1u << type)) != 0)
				p += a.weatherProtection;
		return p;
	}

	// Acid rain (weather_acid_rain.md section 4, MedicalSystem 1406472c0): per tick every organic body part takes
	// d = 0.08 x dt x (1 - P) into its wear pool (the blunt channel with k = 0: no health, no bleed, no armour wear, no KO, and a
	// prosthetic limb takes none). The exposure gate is `indoors` (a roof deck is exposed) and a shelter roof overhead stops it;
	// the weather's `effect strength` is not read on this path.
	void weatherDamage()
	{
		if (!arena.environment_.weatherAt)
			return;
		for (Member& m : arena.members_)
		{
			Fighter& f = m.fighter;
			if (f.dead || f.carriedBy != 0 || f.indoors || f.sheltered)
				continue;
			int type = arena.environment_.weatherType;
			double strength = arena.environment_.weatherStrength;
			arena.environment_.weatherAt(f.x, f.z, type, strength);
			if (type == 2)
			{
				m.dirty = true;
				wear(f, type, kAcidDamagePerSecond);
			}
		}
	}

	// A weather effect's damage to one fighter: `perSecond` x dt x (1 - the worn protection of that type) into every organic part's
	// wear pool. The race's own immunity (RaceData::load's `weather immunity0..N` type set; kinspect c29899b) takes none, gear or no
	// gear: a skeleton or a hive drone that lists 2 takes no acid damage. `toHealth`: the beams take the part's health
	// (no stun, no wear); the acid fills the wear pool.
	void wear(Fighter& f, int type, double perSecond, bool toHealth = false)
	{
		const RaceCombat* race = data.race(f.raceSid);
		if (race != nullptr && (race->weatherImmunity & (1u << type)) != 0)
			return;
		const double d = perSecond * dt * (1.0 - weatherProtection(f, type));
		if (d <= 0.0)
			return;
		for (state::FighterPart& part : f.parts)
		{
			if (part.robotic)
				continue;
			if (toHealth) { part.health -= d; } else { part.wear += d; }
		}
	}

	// Venge's sky beams (VENGE_BEAMS_STATIC.md): d = 10 x dt x strength x (1 - P) off every organic part's health (rwh.175; not the
	// wear pool) for a character in the open within the beam's burn zone (a roof does not shield it: only `indoors` exempts); the strongest beam counts, beams do not stack.
	void beamDamage()
	{
		if (!arena.environment_.beamAt)
			return;
		for (Member& m : arena.members_)
		{
			Fighter& f = m.fighter;
			if (f.dead || f.carriedBy != 0 || f.indoors)
				continue;
			int type = 0;
			double strength = 0.0;
			arena.environment_.beamAt(f.x, f.z, type, strength);
			if (strength > 0.0)
			{
				m.dirty = true;
				wear(f, type, kBeamDamagePerSecond * strength, true);
			}
		}
	}

	// Enemies mid-swing at m (progress in (0, 1)): the +0x228 list (buildIncomingThreatList 140667b40, s74_static.md Q3): an attacker counts when
	// its target is m OR m is in its hit list (an impact of the same swing already reached m); no distance or facing test.
	// Retired: the target-only test (retired/combat_incoming_target_only.txt).
	std::vector<Member*> incoming(Member& m)
	{
		std::vector<Member*> out;
		for (Member& other : arena.members_)
		{
			if (other.id == m.id || !usable(other.fighter))
				continue;
			const Fighter& o = other.fighter;
			if (o.state == CombatState::Attack && o.swingProgress > 0.0 && o.swingProgress < 1.0
				&& (o.target == m.id || std::find(other.fighter.hitList.begin(), other.fighter.hitList.end(), m.id) != other.fighter.hitList.end()))
				out.push_back(&other);
		}
		return out;
	}

	Member* strongestThreat(const std::vector<Member*>& threats, double minProgress)
	{
		Member* best = nullptr;
		double bestP = minProgress;
		for (Member* t : threats)
		{
			if (t->fighter.swingProgress >= bestP)
			{
				bestP = t->fighter.swingProgress;
				best = t;
			}
		}
		return best;
	}

	// The impact box (`findImpactTargets` 1406087a0, MEASURED.md item 17): an oriented box ahead of the
	// attacker, tested against the target's current capsule. No reach roll; an empty box is a miss.
	// Local axes: forward = attacker yaw, lateral across. Half extents (s = 1): lateral w + 0.5,
	// forward D/2; centre D/4 + w + max(L - moved, -1) ahead, so the far edge is
	// 0.75 D + w + max(L - moved, -1) (17.5 + ... here). The box then stretches back towards the
	// attacker until its near edge is radius - D/2 forward. `moved` = root-motion distance of the
	// swing so far (the clip's cumulative root displacement at the current clip time).
	// Height: boxVerticalHit (rwh.214), characters only, no occlusion. No walk gate: the vertical bound alone stops a hit through a ceiling.
	// UNSURE: the stretch uses the attacker's radius w (a humanoid 4, a race's bodyWidth) and a horizontal |C0 - pos|; the
	// disassembly's 3D distance would include the +9 centre height.
	// The vertical overlap of the swing box and the target's box (kenshi-rwh.214). `sideways` = the impact's attack direction is LEFT/RIGHT.
	static bool boxVerticalHit(const Fighter& a, const Fighter& d, bool sideways)
	{
		const double half = sideways ? kBoxHalfUpSide : kBoxHalfUp;
		const double height = (d.down || d.unconscious || d.dead) ? kTargetHeightProne : kTargetHeight;
		const double centre = a.y + kBoxCentreUp;
		return d.y <= centre + half && d.y + height >= centre - half;
	}

	static bool sidewaysImpact(const ImpactPoint& i) { return i.dir == 2 || i.dir == 3; }

	bool inImpactBox(const Member& att, const Technique& t, const Member& def, const ImpactPoint* impact = nullptr)
	{
		if (!boxVerticalHit(att.fighter, def.fighter, impact != nullptr && sidewaysImpact(*impact)))
			return false;
		const Fighter& a = att.fighter;
		const Fighter& d = def.fighter;
		const double D = CombatRules::weaponLength(a);
		// Task BQ: the width is the attacker's body width (RaceCombat::bodyWidth: a humanoid 4.0, exactly the constant this used;
		// the beak 10.5, which gives the measured 22 u slab). The frame is the CURRENT facing: the body yaw for an animal, whose
		// swing is a box test along the facing it holds at the impact frame (kinspect findings/beak_zigzag.md), and the aim hand
		// for a human, whose box follows the look set at the swing's start (task AC; ENGAGEMENT.md 9.4 keeps a human's UNSURE).
		const RaceCombat* race = data.race(a.raceSid);
		const double w = race != nullptr ? race->bodyWidth : kImpactW;
		const double frame = a.animal ? a.yaw : a.aimYaw;
		double moved = 0.0;
		if (t.clip >= 0)
		{
			const Clip::Sample s = data.clips[static_cast<std::size_t>(t.clip)].at(a.clipTime);
			moved = std::hypot(static_cast<double>(s.x), static_cast<double>(s.z));
		}
		const double ahead = D / 4.0 + w + std::max(t.attackDistance - moved, -1.0);
		const double ext = std::max(0.0, (ahead - w) / 2.0);
		const double halfForward = D / 2.0 + ext;
		const double centre = ahead - ext;
		const double dx = d.x - a.x;
		const double dz = d.z - a.z;
		const double forward = dx * std::sin(frame) + dz * std::cos(frame) - centre;
		const double lateral = dx * std::cos(frame) - dz * std::sin(frame);
		// Circle (the capsule seen from above) against the rectangle.
		const double outF = std::max(0.0, std::fabs(forward) - halfForward);
		const double outL = std::max(0.0, std::fabs(lateral) - (w + 0.5));
		return std::hypot(outF, outL) <= kCapsuleRadius;
	}

	// The attacker's hit list (`140608460`) at the time the defender reacts: the impact box plus the
	// ground the swing's root motion will still cover (the swing lunges before it lands). The walk gate
	// is task AQ's, as in the impact box.
	bool inThreatReach(const Member& att, const Technique& t, const Member& def)
	{
		const Fighter& a = att.fighter;
		const Fighter& d = def.fighter;
		double lunge = 0.0;
		if (t.clip >= 0)
			lunge = std::max(0.0, static_cast<double>(data.clips[static_cast<std::size_t>(t.clip)].at(1.0e9).z));
		// The far edge of the real box grows with 0.75 D (item 17); the fit was made at D = 18.
		const double reach = hitReachBase() + 0.75 * (CombatRules::weaponLength(a) - CombatRules::kCharD)
			+ hitReachPerDistance() * t.attackDistance + lunge;
		bool sideways = !t.impacts.empty();
		for (const ImpactPoint& i : t.impacts)
			sideways = sideways && sidewaysImpact(i);
		if (!boxVerticalHit(a, d, sideways))
			return false;
		const double dx = d.x - a.x;
		const double dz = d.z - a.z;
		if (std::hypot(dx, dz) > reach || dx * std::sin(a.yaw) + dz * std::cos(a.yaw) <= 0.0)
			return false;
		return true;
	}

	// ---- XP (`1408c6980`) ----
	void awardDefenceXp(Fighter& me, double oppAttack, bool success)
	{
		const double X = rules.xpUnit();
		const double D = arena.rules_.settings().damageScale;
		const double mult = rules.skillDiffXpMult(me, CombatRules::dodgeSkillEff(me), oppAttack);
		rules.addXp(me.skills.dodge,
			me.encumbrance * (success ? 0.4 : 0.8) * X * D * mult);
	}

	void strengthTail(Fighter& me, double factor)
	{
		const double X = rules.xpUnit();
		const double D = arena.rules_.settings().damageScale;
		double f14 = data.k.xpRateStrength * 2.0 * factor;
		if (me.weapon.weaponType == 5)
			f14 *= (1.0 - me.encumbrance);
		else
		{
			const double s184 = data.k.minStrengthXpMult
				+ std::clamp((me.weapon.weight - me.skills.strength * CombatRules::effectiveStat(me, CombatRules::kStatStrength)) / data.k.weightStrengthDiff1x, 0.0,
					data.k.weightStrengthDiffMax);
			f14 = X * 0.55 * f14 * s184;
		}
		if (f14 > 0.0)
			rules.addXp(me.skills.strength, f14 * D);
	}

	double& attackSkillRef(Fighter& f) const
	{
		return f.weapon.weaponType == 5 ? f.skills.unarmed : f.skills.attack;
	}

	double& weaponSkillRef(Fighter& f) const
	{
		const state::FighterSkills& s = f.skills;
		(void) s;
		switch (f.weapon.weaponType)
		{
		case 0:
			return f.skills.katana;
		case 1:
			return f.skills.sabres;
		case 2:
			return f.skills.blunt;
		case 3:
			return f.skills.heavy;
		case 4:
			return f.skills.hackers;
		case 8:
			return f.skills.poles;
		default:
			return f.skills.unarmed;
		}
	}

	void awardCombatXp(Member& me, std::int32_t kind, Member& opp, const double dmg[6])
	{
		Fighter& f = me.fighter;
		const double X = rules.xpUnit();
		const double D = arena.rules_.settings().damageScale;
		const double mySkill = CombatRules::attackValue(f);
		const double oppDef = CombatRules::defenceValue(opp.fighter);
		const bool armed = f.weapon.weaponType != 5;
		emit(CombatEvent::Kind::Xp, me.id, opp.id, kind);
		auto attackerGain = [&](double attackScale, double weaponScale, double dexScale)
		{
			const double m = rules.skillDiffXpMult(f, mySkill, oppDef);
			rules.addXp(attackSkillRef(f), X * m * attackScale * D);
			if (armed)
			{
				const double wm = rules.skillDiffXpMult(f, CombatRules::weaponSkill(f), oppDef);
				rules.addXp(weaponSkillRef(f), wm * X * weaponScale * D);
				const double total = f.weapon.cutMult + f.weapon.bluntMult + f.weapon.pierceMult;
				const double share = total > 0.0 ? (f.weapon.pierceMult + f.weapon.cutMult) / total : 1.0;
				rules.addXp(f.skills.dexterity, X * m * share * dexScale * D);
			}
		};
		switch (kind)
		{
		case 0:
			attackerGain(1.0, 1.0, 1.0);
			strengthTail(f, 1.0);
			break;
		case 1:
			attackerGain(0.5, 0.4, 0.5);
			strengthTail(f, 1.0);
			break;
		case 2:
		{
			const double m = rules.skillDiffXpMult(f, CombatRules::defenceValue(f), CombatRules::attackValue(opp.fighter));
			rules.addXp(f.skills.defence, m * X * 0.25 * D);
			if (armed)
				rules.addXp(weaponSkillRef(f), m * X * 0.1 * D);
			strengthTail(f, 0.4);
			break;
		}
		case 4:
		{
			strengthTail(f, 0.1);
			const double attackerValue = CombatRules::attackValue(opp.fighter);
			if (!armed)
				awardDefenceXp(f, attackerValue, false);
			else
			{
				const double m = rules.skillDiffXpMult(f, CombatRules::defenceValue(f), attackerValue);
				rules.addXp(f.skills.defence, m * X * 2.0 * D);
			}
			const double total = dmg[0] + dmg[1] + dmg[2] + dmg[3];
			const double m = rules.skillDiffXpMult(f, f.skills.toughness, total);
			rules.addXp(f.skills.toughness, X * data.k.xpRateToughness * m * D);
			break;
		}
		case 6:
			attackerGain(0.25, 0.1, 0.0);
			strengthTail(f, 1.0);
			break;
		default:
			strengthTail(f, 1.0);
			break;
		}
	}

	// ---- MedicalSystem::receiveHit (`1406508d0`, RESOLUTION.md 4) ----
	void medicalHit(Member& defender, const Technique& t, std::int32_t dir, double dmg[6],
		std::string& partName, bool& knockedOut, bool& died, double absorbed[2])
	{
		// knockedOut / died are reported, not emitted: the caller emits Hit first, so the hit that
		// caused a KO or death always precedes it in the event stream.
		defender.dirty = true;
		Fighter& f = defender.fighter;
		const CombatConstants& k = data.k;
		// 4.1 part selection.
		std::vector<std::pair<std::size_t, double>> table;
		double total = 0.0;
		for (std::size_t i = 0; i < f.parts.size(); ++i)
		{
			const state::FighterPart& p = f.parts[i];
			const double e = p.maxHealth - p.wear;
			if (!(p.health > -e))
				continue;
			if (t.lowStrike && (p.type == 2 || p.type == 3))
				continue;
			const double w = p.chance * (dir == 6 ? 1.0 : p.heat);
			if (w > 0.0)
			{
				total += w;
				table.push_back({i, total});
			}
		}
		if (f.parts.empty())
			return;
		std::size_t chosen = f.parts.size() - 1;
		if (!table.empty())
		{
			const double r = draw(f.rng) * total;
			chosen = table.back().first;
			for (const auto& entry : table)
			{
				if (entry.second > r)
				{
					chosen = entry.first;
					break;
				}
			}
		}
		state::FighterPart& part = f.parts[chosen];
		partName = part.name;
		// 4.2 armour layers: a coverage roll per piece, per hit (only for pieces that list the part).
		const double before[3] = {dmg[0], dmg[1], dmg[2]};
		rules.applyArmour(f, part.name, dmg, [&f](std::size_t, double coverage) { return draw(f.rng) < coverage; });
		// What the armour removed (before the toughness factor); the pierce absorption is merged into cut (SIMLINK event 1 tail).
		absorbed[0] = (before[0] + before[2]) - (dmg[0] + dmg[2]);
		absorbed[1] = before[1] - dmg[1];
		// 4.4 toughness resistance and the global multiplier.
		const double factor = rules.toughnessFactor(f);
		const double D = arena.rules_.settings().damageScale;
		for (int i = 0; i < 4; ++i)
			dmg[i] *= factor;
		dmg[0] = std::max(dmg[0], 0.0);
		dmg[1] = std::max(dmg[1], 0.0);
		for (int i = 0; i < 4; ++i)
			dmg[i] *= D;
		// 5.1 applyDamageToPart.
		f.blood -= (dmg[0] + dmg[2]) * k.immediateBloodLoss * dmg[4] * f.bloodFactor;
		part.health -= k.bluntPermanentOrganDamage * dmg[1] + dmg[0] + dmg[2];
		part.stun += (1.0 - k.bluntPermanentOrganDamage) * dmg[1] + dmg[3];
		part.wear += (dmg[0] + dmg[1] + dmg[2]) * 0.04; // ko_death_model.md
		const double wound = std::max(dmg[0], dmg[2]) * k.bleedRate * dmg[4];
		if (wound > 0.0)
			f.wounds.push_back(wound);
		if (rules.trySever(f, part, true)) // Dismemberment >= 1: a hit past -M severs a humanoid's limb (BQ11)
			emit(CombatEvent::Kind::LimbSevered, defender.id, 0, 0, 0, -1, 0.0, 0.0, part.name);
		// Hit heat.
		const double e = part.maxHealth - part.wear;
		double cap = -999.0;
		if (part.health > -e)
		{
			cap = 4.0;
			if (e > 0.0 && 100.0 / e * CombatRules::partEffective(part) < 0.0 && part.heat > 2.0)
				part.heat = 2.0;
		} else {
			part.heat = 0.0;
		}
		if (part.heat < cap)
			part.heat += 1.0;
		rules.checkCollapse(f);
		// KO by a vital part (gate 0, VERIFIED live): every such hit restarts the timer and pays
		// the flat toughness XP, also on a body that is already down.
		if (part.vital && CombatRules::partEffective(part) < rules.koThreshold(f))
		{
			const bool was = f.unconscious;
			f.koTimer = rules.knockoutTime(f);
			f.unconscious = true;
			rules.addXp(f.skills.toughness, rules.xpUnit() * D);
			knockedOut = !was;
		}
		rules.updateMedical(f, 0.0);
		died = f.dead;	}

	// `resolveIncomingHit` (`140665fa0`): deterministic.
	std::int32_t resolveIncoming(const Fighter& def, const Fighter& att, std::int32_t dir)
	{
		const Technique* t = tech(def);
		// BLOCK_TIMING_LIVE.md (the exe's own test, 140665fa0): a block holds iff the technique's attack direction
		// matches the incoming one AND the clip's progress is > 0.5 (so raise time = 0.5 x clip length / clip speed);
		// a dodge holds for progress 0.1..0.98; anything else is a hit. A staggered defender (state 8) keeps its last
		// technique (enterStumble leaves the clip at its end, progress 1), so a matching stale block still holds.
		if (t == nullptr || (def.state != CombatState::Defend && !(def.state == CombatState::Stumble && t->isBlock)))
			return 2;
		const double p = clipProgress(def);
		if (t->isDodge)
		{
			if (p >= 0.10 && p <= 0.98)
				return 0;
		} else if (t->isBlock) {
			const double dx = att.x - def.x;
			const double dz = att.z - def.z;
			const bool behind = dx * std::sin(def.yaw) + dz * std::cos(def.yaw) < 0.0;
			std::int32_t d = dir;
			if (behind)
				d = dir == 1 ? 7 : (dir == 2 || dir == 3) ? 9 : dir;
			if (!t->impacts.empty() && t->impacts.back().dir == d && p > 0.5)
				return 5;
		}
		return 2;
	}

	// `Character::playHitReaction` (1404391f0, BLOCK_TIMING_LIVE.md 14b): after every LANDED hit a reaction clip starts if none is
	// playing, or if the hit is heavy (total damage > toughness x size x 0.01 x G, G = 52 with rebirth.mod); a heavy hit during a
	// reaction replaces the playing clip with a light one from t = 0 (63 of 66 such hits in r1-r20) and the state stays 8. The clip
	// is presentation only and is not modelled. What the sim takes from it is the stagger time: a hit that lands in state 8 re-arms
	// the 1.1 s timer (the combo's second impact: 40 of 97 logged combo staggers span 0.5 s, state 8 -> 3 -> 8 at the second impact,
	// i.e. 1.6 s from the first); the push (recoilPath) is not restarted: it was measured with these restarts in it. UNSURE what
	// decides the exit in general (logged 1.11 s mostly, 1.2 s, 1.6-3.0 s). The 25 % forced KO needs the inventory property of
	// `FUN_1405c8a10`: left out, UNSURE.
	bool heavyHit(const Fighter& def, const double dmg[6]) const
	{
		return dmg[0] + dmg[1] + dmg[2] + dmg[3] > def.skills.toughness * 0.01 * data.k.stumbleDamageMax;
	}

	// A player's move order is running (the same test as `step`'s playerOrder, minus the state): hits cost it a short hold, not a stagger.
	// Only a route the order planned (routeSource Order, as liveGoal's "Move order" label): a job or AI walk of a player character still staggers.
	static bool underMoveOrder(const Fighter& f) noexcept
	{
		return f.playerControlled && !f.driving() && f.walking() && f.routeSource == state::Fighter::PlanSource::Order && f.carrying == 0
			&& !f.sneak && !f.down && !f.unconscious;
	}

	void enterStumble(Member& m, const Fighter& from, bool heavy, bool knockedOut)
	{
		Fighter& f = m.fighter;
		if (underMoveOrder(f) && !knockedOut)
		{
			// The original's light hit leaves the combat state alone and stops the runner for a few frames; only a heavy hit staggers it.
			if (!heavy && f.state != CombatState::Stumble)
			{
				m.walkHold = kMoveHitHold;
				f.forwardSpeed = 0.0;
				return;
			}
		}
		if (f.state == CombatState::Stumble)
		{
			// The blow that knocks the defender out: the light pick is made but its setter is skipped, 8 -> 4 (CADENCE_LIVE.md Item 34 (f)).
			if (heavy && !knockedOut)
			{
				f.stumbleTimer = underMoveOrder(f) ? kMoveStumbleTime : kStumbleTime; // a runner is held ~0.45 s per blow
				f.reactClip = data.stumbleClip;
				f.reactTime = 0.0;
				// playHitReaction sets next = 8 again: the log shows 8 -> 3 -> 8 (63 "hit again" exits at 0.49 s), i.e. the entry
				// re-arms the escape timer and clears +0x120.
				f.escapeTimer = kStumbleHeavyTime;
				f.escapeAsked = false;
			}
			return;
		}
		const double rx = f.x - from.x;
		const double rz = f.z - from.z;
		const double rl = std::hypot(rx, rz);
		f.recoilX = rl > 1.0e-6 ? rx / rl : 0.0;
		f.recoilZ = rl > 1.0e-6 ? rz / rl : 0.0;
		const Technique* t = tech(f);
		if (t != nullptr && t->isBlock)
		{
			f.techniqueFinished = true; // the pointer stays (see resolveIncoming), the clip is gone
			f.clipTime = clipLength(*t);
			f.impactIndex = 0;
			f.lastImpact = 0;
			f.hitListBuilt = false;
		} else {
			stopTechnique(f);
		}
		f.recovering = false;
		setState(m, CombatState::Stumble);
		// The reaction runs its whole clip; the heavy `mid blow` (4.63 s) normally ends through the escape roll (stumble()).
		// A skeleton without the clip (a mod, a stripped test rig) has stumbleHeavyClip -1: the light timer and clip stand in.
		const bool heavyClip = heavy && data.stumbleHeavyClip >= 0;
		f.stumbleTimer = heavyClip ? data.clips[static_cast<std::size_t>(data.stumbleHeavyClip)].length : kStumbleTime;
		f.escapeTimer = kStumbleHeavyTime;
		f.escapeAsked = false;
		f.reactClip = heavyClip ? data.stumbleHeavyClip : data.stumbleClip;
		f.reactTime = 0.0;
		if (underMoveOrder(f) && !knockedOut)
			f.stumbleTimer = kMoveStumbleTime; // a heavy blow on a runner: held ~0.45 s, then the walk goes on
		emit(CombatEvent::Kind::Stumble, m.id, 0);
	}

	// `applyImpacts` (`14060a2f0`) for impact `index` of the attacker's technique.
	// Victim filter = `findImpactTargets` 1406087a0 (kinspect dt13, findings/dt13_prone_splash.md): not an ally, not
	// unconscious, and an enemy of the swinger or its current target. There is no down / posture test, so a conscious
	// prone enemy in the box is splashed at full damage, while a KO'd or neutral body is not. The "current target" arm is
	// modelled (task BV): the goal-34 join fights a troublemaker no relation makes an enemy. Not modelled: the
	// slave-state-1 arm.
	// UNSURE: the carried exclusion in usable() is ours.
	void applyImpacts(Member& att, std::int32_t index)
	{
		Fighter& a = att.fighter;
		const Technique& t = *tech(a);
		const ImpactPoint& impact = t.impacts[static_cast<std::size_t>(
			std::min<std::int32_t>(index, static_cast<std::int32_t>(t.impacts.size()) - 1))];
		std::vector<std::pair<double, Member*>> targets;
		for (Member& other : arena.members_)
		{
			// ... and an enemy of the swinger OR its current target (the arm is live since task BV's goal-34 join: the join's
			// type-4 entry goes into the troublemaker's table (dt23), not the joiner's, so the joiner's swing hits a target its own
			// relation never made an enemy).
			if (other.id == att.id || !usable(other.fighter) || !(enemy(a, other.fighter) || other.id == a.target))
				continue;
			if (inImpactBox(att, t, other, &impact))
				targets.push_back({centre(a, other.fighter), &other});
		}
		std::sort(targets.begin(), targets.end(),
			[](const auto& l, const auto& r) { return l.first != r.first ? l.first < r.first : l.second->id < r.second->id; });
		if (targets.size() > static_cast<std::size_t>(t.maxHits))
			targets.resize(static_cast<std::size_t>(t.maxHits));
		for (auto& entry : targets)
		{
			Member& def = *entry.second;
			if (std::find(a.hitList.begin(), a.hitList.end(), def.id) == a.hitList.end())
				a.hitList.push_back(def.id); // the swinger is a threat to it until the swing ends (incoming())
			def.fightTier = true;
			double dmg[6];
			rules.computeDamage(a, def.fighter, dmg);
			for (int i = 0; i < 4; ++i)
				dmg[i] *= impact.power;
			const std::int32_t result = resolveIncoming(def.fighter, a, impact.dir);
			emit(CombatEvent::Kind::Resolve, def.id, att.id, result, impact.dir, def.fighter.technique);
			std::string part;
			if (result == 5)
			{
				awardCombatXp(def, 2, att, dmg);
				def.fighter.reactTimer = 0.01;
			} else if (result == 2) {
				// A hit breaks the victim's stealth on its own: playHitReaction / receiveHit clear it with no target, awareness
				// or distance test (STEALTH_CLEAR.md section 4). The attacker's own clear is at its swing start.
				clearSneak(def.fighter, SneakClear::Hit);
				// SELF_PRESERVATION (goal 32): a landed hit lets the victim answer this attacker at any range.
				if (std::find(def.hitBy.begin(), def.hitBy.end(), att.id) == def.hitBy.end())
					def.hitBy.push_back(att.id);
				def.lastAttacker = att.id;
				if (def.fighter.target == 0)
					def.fighter.orderInterrupt = true; // receiveHit -> onTargetedBy
				bool knockedOut = false;
				bool died = false;
				double absorbed[2] = {};
				const bool heavy = heavyHit(def.fighter, dmg);
				const bool wasDown = def.fighter.unconscious;
				// Task BG2: a ragdoll exists only while the body is KO'd or dead (the original's down mask); a conscious down
				// crawler animates and is not one. Captured before the hit's own medical update can change either flag.
				const bool wasRagdoll = def.fighter.unconscious || def.fighter.dead;
				medicalHit(def, t, impact.dir, dmg, part, knockedOut, died, absorbed);
				// The hit's zero-dt medical update can put the body down without the vital-part path (blood at 0): still this hit's KO
				// event (knockedOut stays as it was: it picks the reaction clip and the stumble).
				const bool downedByHit = knockedOut || (!wasDown && def.fighter.unconscious && !died);
				awardCombatXp(def, 4, att, dmg);
				arena.attacks_.push_back({att.id, def.id, true});
				CombatEvent e;
				if (arena.observer_)
				{
					e.kind = CombatEvent::Kind::Hit;
					e.time = arena.time_;
					e.who = def.id;
					e.other = att.id;
					e.technique = a.technique;
					e.code = impact.dir;
					// code2: the reaction this hit starts (CADENCE_SLICE.md): 0 none (the KO blow), 1 a new heavy clip, 2 a light clip
					// from t = 0 over a playing reaction, 3 the playing clip kept running
					e.code2 = knockedOut ? 0 : def.fighter.state != CombatState::Stumble ? (heavy ? 1 : 2) : (heavy ? 2 : 3);
					for (int i = 0; i < 6; ++i)
						e.dmg[i] = dmg[i];
					e.absorbed[0] = absorbed[0];
					e.absorbed[1] = absorbed[1];
					e.part = part;
					arena.observer_(e);
				}
				if (downedByHit)
					emit(CombatEvent::Kind::Knockout, def.id, att.id); // other = the attacker; 0 for a KO with no hit (update())
				if (died)
				{
					emit(CombatEvent::Kind::Death, def.id, 0);
					arena.deaths_.push_back({def.id, att.id});
				}
				// EVENT 48 (task BG): the ragdoll this hit starts, right after the KO / death event. A death takes the cause even
				// when the body was already ragdolled; a KO blow (a crawler's included) is 0; a hit that moves a body already
				// KO'd or dead is a launch (3, BG2: a conscious crawler is not a ragdoll). A hit that only collapses a limb, or
				// lands on a crawler that stays conscious, sends nothing here: the arena's transition roll covers a ragdoll
				// start with no event.
				// A heavy hit also moves the body (the knockback, CombatArena::rollRagdoll): along the line from the attacker.
				const double fromX = def.fighter.x - att.fighter.x;
				const double fromZ = def.fighter.z - att.fighter.z;
				const double fromLen = std::hypot(fromX, fromZ);
				const double push = heavy && fromLen > 1.0e-6 ? CombatArena::kRagdollHitStrength * CombatArena::kKnockbackPerStrength : 0.0;
				const double pushX = fromLen > 1.0e-6 ? fromX / fromLen : 0.0;
				const double pushZ = fromLen > 1.0e-6 ? fromZ / fromLen : 0.0;
				if (died)
					emitRagdoll(def.fighter, def.id, 1, att.id, part, CombatArena::kRagdollHitStrength, pushX, pushZ, push);
				else if (downedByHit)
					emitRagdoll(def.fighter, def.id, 0, att.id, part, CombatArena::kRagdollHitStrength, pushX, pushZ, push);
				else if (wasRagdoll)
					emitRagdoll(def.fighter, def.id, 3, att.id, part, CombatArena::kRagdollHitStrength, pushX, pushZ, push);
				// A hit never staggers a fighter in state 0 (CADENCE_LIVE.md Item 42: 0 of 36 fights have a 0>8; the exe's opponent does
				// not attack before the swing's impact and the swing is cut right after it).
				if (def.fighter.state != CombatState::Attack)
					enterStumble(def, a, heavy, knockedOut);
			}
			// Attacker side: recovery by outcome and XP. A landed or blocked hit removes the target from the attacker's own engaged
			// list (removeEngagedAttacker(me, target), DUEL_CADENCE_STATIC.md): it must re-engage (its hub adds it again) before it
			// may attack.
			if (result != 0)
				removeEngaged(att, def.id);
			const int kind = result == 0 ? 6 : result == 5 ? 1 : 0;
			// SET, not added to the hub's 0.5 hold: e1 cx +0x148 reads 0.5 until the impact resolves, then 0.6
			// (hit) / 0.3 (blocked) at once, and counts down only after the swing ended.
			a.recoverTimer = CombatRules::attackRecoveryTime(kind) + (result == 0 ? kDodgedHold : 0.0);
			awardCombatXp(att, kind, def, dmg);
			selfInjury(a, impact.limb, result);
		}
	}

	// Unarmed attackers hurt their own hand (FUN_140666780, UNSURE which results arrive).
	void selfInjury(Fighter& a, std::int32_t limb, std::int32_t result)
	{
		if (a.weapon.weaponType != 5 || limb == 4)
			return;
		const char* name = limb == 0 ? "Left Arm" : limb == 1 ? "Right Arm" : limb == 2 ? "Left Leg" : "Right Leg";
		for (state::FighterPart& p : a.parts)
		{
			if (p.name != name)
				continue;
			const double D = arena.rules_.settings().damageScale;
			const double q = D * (1.0 - ((CombatRules::attackValue(a) * 0.01 + a.skills.toughness * 0.01) * 0.5));
			if (result == 5)
				p.health -= 5.0 * q;
			p.stun += q;
			// awardToughnessXP (1408c68e0, kinspect TOUGHNESS_XP_LIVE.md): 0.075 x k x 1.33 x D;
			// k = 0.5 when blocked, 0.05 otherwise (0.25 for result 1, which we never pass).
			rules.addXp(a.skills.toughness,
				rules.xpUnit() * (result == 5 ? 0.5 : result == 1 ? 0.25 : 0.05) * data.k.xpRateToughness * D);
		}
	}

	// ---- engagement (`decideEngagement`, ENGAGEMENT.md 5) ----
	std::int32_t decideEngagement(Member& d, Member& a)
	{
		Fighter& D = d.fighter;
		auto result = [&](std::int32_t r)
		{
			emit(CombatEvent::Kind::Engage, d.id, a.id, r);
			return r;
		};
		if (D.target == 0)
			return result(0); // D is not in combat
		const bool attacking = D.state == CombatState::Attack
			|| (D.state == CombatState::Between && D.pending == CombatState::Attack);
		// Step 2 reads D's swing progress ON A (`140665420`: +0x140 when A is D's target or in its hit list, else 0), so
		// a swing aimed at someone else does not answer 1 for A: it falls to step 6's roll, and a lost roll lets A swing
		// at D mid-swing. Without the test every asker was sent to defend while D swung at anyone, and a crowd stopped
		// swinging at her for the rest of her clip (BQ16: the wide ring's bandits left her free to attack).
		const bool swingOnA = D.target == a.id
			|| std::find(D.hitList.begin(), D.hitList.end(), a.id) != D.hitList.end();
		// Mid-swing: the swing started this very tick counts too (its swingProgress is only computed at the next
		// step; the originals alternate swings, KATANA_DUEL_STATS_LIVE.md).
		if (attacking && swingOnA && ((D.swingProgress > 0.0 && D.swingProgress < 1.0)
			|| (D.state == CombatState::Attack && clipPlaying(D) && D.clipTime == 0.0)))
			return result(1);
		if (!tryAcquire(d, a.id))
			return result(5);
		// A swing against the asker is only started by a fighter the asker's engaged list can hold: turning on the asker is setTarget, which
		// lists the defender there (a fresh attacker's list does not hold it yet), and a full list refuses it, so a Wait-ring attacker or one the
		// full list refused stays out of the fight however the roll falls (crowd-combat.md: 0 of 22 hits from outside the list).
		const auto& askerList = a.fighter.engaged;
		const bool askerListed = askerList.size() <= 4 || std::find(askerList.begin(), askerList.end(), d.id) != askerList.end();
		if (D.state == CombatState::Stumble || D.pending == CombatState::Stumble || D.down)
		{
			if (D.state == CombatState::Stumble)
				D.escapeAsked = true; // +0x120: arms the escape roll (stumble())
			return result(0);
		}
		const bool defending = D.state == CombatState::Defend
			|| (D.state == CombatState::Between && D.pending == CombatState::Defend);
		if (!D.recovering)
		{
			if (defending)
			{
				// The counter flip (CADENCE_SLICE.md Follow-up 3, 7): a defender waiting in state 3 with pending Defend, nothing aimed at
				// it and no clip running, whose own attack roll wins, turns to attack and retargets onto the asker (exe: 1.63 flip swings
				// per fight, a median 0.42 s after the hub).
				if (D.state == CombatState::Between && !clipPlaying(D) && incoming(d).empty() && askerListed
					&& rules.attackChanceRoll(CombatRules::attackValue(D), CombatRules::attackValue(a.fighter), D.rng))
				{
					D.pending = CombatState::Attack;
					setTarget(d, a.id);
					traceCounter(d);
					return result(D.target == a.id ? 1 : 5);
				}
				return result(0);
			}
			if (attacking)
			{
				// A true roll falls through to the last line of the original (1 if D's target is A, else 5), not to 5
				// (ENGAGEMENT.md 5 step 6 said 5): in the 19 katana re-stages all 229 true rolls (D attacking, in the hub, or
				// waiting) returned 1, never 5. The asker then waits in state 3 with pending 1 (the 0.5 s hub timer, 177 of 177
				// repeats at 0.5 s median) instead of circling and re-asking at once.
				if (rules.attackChanceRoll(CombatRules::attackValue(D), CombatRules::attackValue(a.fighter), D.rng))
					return result(D.target == a.id ? 1 : 5);
				// B5b: the lost roll of a swinger past its impact answers defend and sets +0x120; attackState's cut waits on it.
				if (D.state == CombatState::Attack)
					D.escapeAsked = true;
				D.pending = CombatState::Defend;
				return result(0);
			}
		}
		if (!incoming(d).empty())
		{
			D.pending = CombatState::Defend;
			if (!clipPlaying(D))
				setState(d, CombatState::Between);
			return result(0);
		}
		if (D.blockMode)
			return result(0);
		const bool roll = rules.attackChanceRoll(CombatRules::attackValue(D),
			CombatRules::attackValue(a.fighter), D.rng);
		if (!roll)
		{
			D.pending = CombatState::Defend;
			return result(0);
		}
		if (!attacking && !clipPlaying(D) && askerListed)
		{
			D.pending = CombatState::Attack;
			setState(d, CombatState::Between);
			setTarget(d, a.id);
			traceCounter(d);
		}
		return result(D.target == a.id ? 1 : 5);
	}

	// ---- the hub, state 4 ----
	void hub(Member& m)
	{
		Fighter& f = m.fighter;
		f.techniqueFinished = false;
		Member* target = pickTarget(m);
		if (target == nullptr)
		{
			setState(m, CombatState::Done);
			return;
		}
		if (unreachableNear(m, *target))
		{
			giveUpUnreachable(m, *target); // no walk reaches it from here (a roof hole, a locked door)
			// dt4 (kinspect 7726815): CombatClassAI::update drops the target, adds it to the blocked list and re-picks at
			// once; the picker skips the blocked candidate, so this can only take another enemy.
			target = pickTarget(m);
			if (target == nullptr)
			{
				setState(m, CombatState::Done);
				return;
			}
		}
		if (notDirect(m, *target))
		{
			setState(m, CombatState::Approach); // another floor is the stair's job (task AS)
			return;
		}
		f.pending = CombatState::Circle;
		f.recoverTimer = 0.5;
		setState(m, CombatState::Between);
		// tryEngageTarget: in the target's engaged list, inside reach, not in block mode.
		const bool listed = addEngaged(*target, m.id);
		if (!listed || f.blockMode)
		{
			// ENGAGEMENT.md 4.4: a threat -> defend; else Circle if I am in the target's engaged list, else Waiting (the list holds 5: the
			// sixth attacker and on wait on the ring). Before, an attacker left out of the list circled (retired/combat_hub_unlisted_circles.txt).
			if (!incoming(m).empty())
				f.pending = CombatState::Defend;
			else if (!listed)
				f.pending = CombatState::Wait;
			return;
		}
		if (inEngageRange(m, *target))
		{
			f.pending = static_cast<CombatState>(decideEngagement(*target, m));
			if (f.pending != CombatState::Attack && f.pending != CombatState::Defend)
				f.pending = CombatState::Circle;
		}
		// Out of engage range, or a circle answer, with a swing aimed at me: defend (what the hub did before the decision was asked).
		if (f.pending == CombatState::Circle && !incoming(m).empty())
			f.pending = CombatState::Defend;
	}

	// `chooseCounterTechnique` (140609290, BLOCK_TIMING_LIVE.md): the defender polls only when its action timer
	// (+0x14c, run down by dt every tick in update()) is below 0. The strongest threat (highest progress < 1) must
	// have the defender in its hit list (inThreatReach: the swept volume, approximated live) and progress >= 0.1;
	// not yet: retry in 0.1 s; picked: 0.2 s; pickTechnique found nothing: 1.2 s. There is no reaction delay: the
	// 0.14 s median pick after an idle defender's swing start is this poll (first poll fails at progress < 0.1).
	// `gated` is false for the re-pick inside state 1 (chooseNextAction calls it directly, ENGAGEMENT.md 3.5 step 5).
	// `escape` is the attacker the stagger's escape roll succeeded against (stumble()): a dodge is picked in stumble mode against it
	// and the state leaves 8 by force.
	bool chooseCounter(Member& m, bool gated = true, Member* escape = nullptr)
	{
		Fighter& f = m.fighter;
		if (gated && f.actionTimer >= 0.0)
			return false;
		auto threats = incoming(m);
		Member* e = escape != nullptr ? escape : strongestThreat(threats, -10.0);
		const Technique* et = e != nullptr ? tech(e->fighter) : nullptr;
		if (e == nullptr || et == nullptr || et->isBlock || e->fighter.swingProgress >= 1.0
			|| e->fighter.swingProgress < 0.1 || !inThreatReach(*e, *et, m))
		{
			f.actionTimer = 0.1;
			f.reactTimer = 0.25;
			return false;
		}
		const std::size_t idx = static_cast<std::size_t>(
			std::min<std::int32_t>(e->fighter.impactIndex, static_cast<std::int32_t>(et->impacts.size()) - 1));
		const std::int32_t dir = et->impacts[idx].dir;
		const double dx = e->fighter.x - f.x;
		const double dz = e->fighter.z - f.z;
		const bool behind = dx * std::sin(f.yaw) + dz * std::cos(f.yaw) < 0.0;
		const double attackerSkill = CombatRules::attackValue(e->fighter);
		const DefenceChoice choice = rules.pickDefence(f, dir, attackerSkill,
			e->fighter.weapon.weaponType == 5, behind, escape != nullptr, f.rng);
		emit(CombatEvent::Kind::PickDefence, m.id, e->id, 0, 0, choice.technique, choice.blockPercent,
			choice.dodgePercent);
		const bool dodge = choice.technique >= 0
			&& data.techniques[static_cast<std::size_t>(choice.technique)].isDodge;
		if (choice.technique < 0 || dodge)
			awardDefenceXp(f, attackerSkill, choice.technique >= 0);
		if (choice.technique < 0)
		{
			f.actionTimer = 1.2;
			f.reactTimer = 1.0;
			return false;
		}
		const Technique& t = data.techniques[static_cast<std::size_t>(choice.technique)];
		// Dodges play at the technique speed alone (measured: 1.1 after the first frame); blocks at
		// CharStats+0x18c x technique speed (ENGAGEMENT.md 3.5).
		startTechnique(m, choice.technique, (t.isDodge ? 1.0 : rules.reactClipSpeed(f)) * t.speedMult,
			e->fighter.x, e->fighter.z);
		f.actionTimer = 0.2;
		f.reactTimer = 1.0;
		f.lastImpact = static_cast<std::int32_t>(idx); // defender: the attacker's impact index at the pick (see defendState)
		setState(m, CombatState::Defend, escape != nullptr);
		return true;
	}

	void startTechnique(Member& m, std::int32_t index, double speed, double faceX, double faceZ)
	{
		Fighter& f = m.fighter;
		f.hitList.clear();
		f.technique = index;
		f.clipTime = 0.0;
		f.clipSpeed = speed;
		f.techniqueFinished = false;
		f.impactIndex = 0;
		f.lastImpact = 0;
		f.hitListBuilt = false;
		f.escapeAsked = false;
		// Retired: `f.yaw = std::atan2(faceX - f.x, faceZ - f.z)` (a snap onto the opponent; retired/combat_technique_yaw_snap.txt).
		// The technique's AIM is still set here (the exe's look hand, mover+0x250: tryStartAttack / faceTarget 140666400), but the body
		// turns onto it at the combat-ready rate instead (kinspect s70_static.md section 3, move_kinematics.md "Facing":
		// AnimationClassHuman::vf8 halves the step unless anim+0x244 == 1): integrate() turns f.yaw each tick.
		f.aimYaw = std::atan2(faceX - f.x, faceZ - f.z);
	}

	// The target is not directly reachable (the hub sends the fighter on to the move-in task, the pathing states 10 / 11 of rwh.108): beyond 150 u or on
	// another floor. The original's test is a navmesh line of sight (cc+0x2c0), which open ground never fails: UNSURE, ours, a target that is running
	// freely (rwh.108's gate) and seen along a clear line (MeshNavigator::lineClear) stays direct at any distance, until the steer's own stuck timer
	// routes it round whatever is in the way. Every other target keeps the 150 u rule.
	bool notDirect(const Member& m, const Member& target) const
	{
		if (floorApart(m.fighter, target.fighter))
			return true;
		// The navmesh straight walk is blocked (cc+0x2c0, a table's cut or a wall between them): the pathing states 10 / 11 walk round (kenshi-k16.6.416).
		if (!straightWalkClear(m.fighter, target.fighter))
			return true;
		const double d = centre(m.fighter, target.fighter);
		if (d <= kTuneApproachDistance)
			return false;
		// A player's attack order walks to its target by the move-in task (Approach) from the first tick, whatever the target does: the open-line
		// shortcut below sent it through Circle (kFlagCombat on, steering by the ring) until the target slowed, then back to Approach with the
		// walk restarted (kenshi-k16.6.183).
		if (m.fighter.playerControlled && m.fighter.orderTarget != 0 && m.fighter.orderTarget == target.id)
			return true;
		const MeshNavigator* n = nav();
		return !(n != nullptr && m.fighter.walkStuck < kStuckRepath && runsFreely(target.fighter)
			&& n->lineClear(m.fighter.x, m.fighter.y, m.fighter.z, target.fighter.x, target.fighter.y, target.fighter.z));
	}

	// rwh.108: the target is running on its own locomotion: not steering a fight of its own, not stumbling, not mid-action (no technique), above 9 u/s.
	static bool runsFreely(const Fighter& t)
	{
		return t.target == 0 && !t.down && !t.unconscious && !t.dead && t.state != CombatState::Stumble && t.technique < 0
			&& std::hypot(t.velX, t.velZ) > kLeadMinSpeed;
	}

	// FUN_1404354d0: the target's position plus facing x speed x t (the facing is the heading of a runner; ours is yaw, 0 = +z).
	static void predicted(const Fighter& t, double seconds, double& x, double& z)
	{
		const double speed = std::hypot(t.velX, t.velZ);
		x = t.x + std::sin(t.yaw) * speed * seconds;
		z = t.z + std::cos(t.yaw) * speed * seconds;
	}

	bool tryStartAttack(Member& m, Member& target)
	{
		Fighter& f = m.fighter;
		double reach = 0.0;
		// rwh.108 section 3 (AttackState::tryStartAttack): the edge is measured to where the target will be in 1.2 s down its facing, unless it is
		// steering a fight of its own.
		double aheadX = target.fighter.x;
		double aheadZ = target.fighter.z;
		if (target.fighter.target == 0 && !target.fighter.down && !target.fighter.unconscious)
			predicted(target.fighter, kLeadSwing, aheadX, aheadZ);
		const double d = pickDistance(f, aheadX, target.fighter.y, aheadZ);
		const bool stumbling = target.fighter.state == CombatState::Stumble || target.fighter.down;
		const std::int32_t pick = rules.pickAttack(f, d, CombatRules::weaponLength(f) / 2.0, f.previousTechnique, stumbling, reach, f.rng);
		f.cachedReach = reach;
		if (pick < 0)
			return false;
		const Technique& t = data.techniques[static_cast<std::size_t>(pick)];
		// Task BQ: an animal technique has no clip to play. Its length is the measured time to the impact (Technique::length), run at
		// clip speed 1: the data's `anim speed mult` (2.0 on the beak's attacks) scales a clip we do not have, and would halve the
		// time to the measured impact frame.
		startTechnique(m, pick, t.clip >= 0 ? rules.attackClipSpeed(f) * t.speedMult : 1.0,
			target.fighter.x, target.fighter.z);
		// The attack swing's engagement notify (14043a730 -> 140999d50(attacker AI, other, 1), STEALTH_CLEAR.md section 1):
		// the attacker loses stealth as it swings. AttackState::tryStartAttack also notifies the victim (STEALTH_CLEAR.md
		// section 2), which is the same clear as being targeted - it catches a character that turned stealth back on
		// while already under attack.
		clearSneak(f, SneakClear::Swing);
		clearSneak(target.fighter, SneakClear::Targeted);
		f.previousTechnique = pick;
		arena.attacks_.push_back({m.id, target.id});
		emit(CombatEvent::Kind::TechniqueStart, m.id, target.id, 0, 0, pick, d, f.clipSpeed);
		setState(m, CombatState::Attack); // the poll timer is not touched here (it is the defence poll's alone)
		return true;
	}

	void between(Member& m)
	{
		Fighter& f = m.fighter;
		Member* target = pickTarget(m);
		if (target == nullptr)
		{
			setState(m, CombatState::Done);
			return;
		}
		if (notDirect(m, *target))
		{
			setState(m, CombatState::Approach);
			return;
		}
		switch (f.pending)
		{
		case CombatState::Attack:
			if (tryStartAttack(m, *target))
				return;
			// AttackState::enter's failure path calls tryInterruptForThreat(0.2, 0.05) before the walk (s74_static.md Q3): an attacker already swinging at
			// this fighter turns the pending attack into a defence.
			if (f.swingProgress < 0.2 && strongestThreat(incoming(m), 0.05) != nullptr)
			{
				f.pending = CombatState::Defend;
				return;
			}
			// AttackState::enter walks towards the target and retries next frame.
			steer = {true, kAttackApproachInner, kAttackApproachOuter, kTuneWalkCap, false};
			break;
		case CombatState::Defend:
			if (chooseCounter(m))
				return;
			f.recoverTimer -= dt;
			steer = defendRing();
			if (f.recoverTimer < 0.0)
			{
				stopTechnique(f);
				setState(m, CombatState::Hub);
			}
			break;
		case CombatState::Wait:
		case CombatState::Circle:
		default:
			f.circleTimer = 3.0 * draw(f.rng) + 1.5;
			f.circleOffset = 50.0 * draw(f.rng) - 25.0;
			setState(m, f.pending == CombatState::Wait ? CombatState::Wait : CombatState::Circle);
			break;
		}
	}

	void circle(Member& m)
	{
		Fighter& f = m.fighter;
		ringRouted = 0;
		Member* target = pickTarget(m);
		if (target == nullptr)
		{
			setState(m, CombatState::Hub);
			return;
		}
		if (unreachableNear(m, *target))
		{
			giveUpUnreachable(m, *target);
			setState(m, CombatState::Hub);
			return;
		}
		if (f.state == CombatState::Wait)
		{
			// WaitingStateAI has no distance branch (s77_static.md b, BQ12 b): it steers straight at the target, capped at min(200, run speed)
			// while the gap is above 160 (cleared below 70), else 18. Retired: the far waiter (> 150 u) sent back to the hub
			// (retired/combat_waiting_far_to_hub.txt). The original's own ends for a chase are the hub's navmesh-LOS path state and the
			// move-in task's give-up monitor; a waiter the list has no room for never reaches the hub, so (UNSURE, ours) the same monitor
			// runs on the far waiter: ksim_bar_fight step 4 (a waiter chasing a runner for ever) needs some end.
			const double gap = centre(f, target->fighter) - footprintOf(f) - footprintOf(target->fighter);
			if (gap > kWaitFarOn)
				m.waitFar = true;
			else if (gap < kWaitFarOff)
				m.waitFar = false;
			if (m.waitFar && chaseGivesUp(m, *target))
			{
				setState(m, CombatState::Hub);
				return;
			}
			steer = waitingRing(m);
			steer.straight = true;
			if (m.waitFar)
			{
				steer.cap = kWaitFarCap;
				steer.run = true;
			}
		} else {
			// A target that got far away is not directly reachable: the hub sends a circling fighter on to the move-in task (Approach) with
			// its give-up monitor (hub(): step 2 of ENGAGEMENT.md 4). UNSURE, ours: the original's test is a navmesh line of sight (cc+0x2c0).
			if (notDirect(m, *target))
			{
				setState(m, CombatState::Hub);
				return;
			}
			steer = Steer{true, ringInner(), ringOuter(), kTuneWalkCap, false, true};
		}
		if (!incoming(m).empty())
		{
			setState(m, CombatState::Hub);
			return;
		}
		// WaitingStateAI::update: not in block mode, addEngagedAttacker success -> state 4 (the hub), where it is then in the list.
		if (f.state == CombatState::Wait && !f.blockMode && addEngaged(*target, m.id))
		{
			setState(m, CombatState::Hub);
			return;
		}
		f.circleTimer -= dt;
		if (f.blockMode)
		{
			if (f.circleTimer < 0.0)
			{
				clearAllSlots(f); // CircleStateAI::update clears its own table (ENGAGEMENT.md 6)
				f.circleTimer = 3.0 * draw(f.rng) + 1.5;
			}
			return;
		}
		const bool canEngage = inEngageRange(m, *target);
		if (f.state != CombatState::Wait && !canEngage
			&& centre(f, target->fighter) <= (f.routeChase && f.routeNext < f.route.size() ? kChaseRouteAbove : ringOuter() + 10.0))
		{
			// The crow says "near the ring", the walk says otherwise: an obstacle (a wall, a ledge, the side of a building)
			// makes the path longer than the engage range, and the ring steer only walks in beyond its outer radius, so a
			// circler there stands at velocity 0 for ever (task BI: both fighters 26 apart with a 39 u walk between them).
			// The mover walks the route round the obstacle instead.
			// A step the walk made keeps the tick (integrate's direct steer would undo it); no step (no route, the cooldown after that,
			// a route the walk cannot advance) leaves the tick to integrate, which gives up an unreachable target or steers across a pinch.
			const double x0 = f.x, z0 = f.z;
			const auto blocks = arena.reroutes_ + arena.routesBlocked_;
			chaseStep(f, target->fighter.x, target->fighter.z, std::nullopt, runSpeed(f), true, true);
			if ((f.x != x0 || f.z != z0) && arena.reroutes_ + arena.routesBlocked_ == blocks)
				ringRouted = m.id;
			return;
		}
		if (f.state != CombatState::Wait && canEngage && tryAcquire(*target, m.id))
		{
			setState(m, CombatState::Hub);
			return;
		}
		// No timer exit (CADENCE_LIVE.md Item 36 (a), H12/H13): the circle leaves only for an incoming attacker or a slot. The re-roll
		// of the timer (and the slot clear it brings) stays; it used to send the fighter back to the hub, a 1.5-4.5 s dwell the exe lacks.
		if (f.circleTimer < 0.0)
		{
			clearAllSlots(f);
			f.circleTimer = 3.0 * draw(f.rng) + 1.5;
		}
	}

	void attackState(Member& m)
	{
		Fighter& f = m.fighter;
		const Technique* t = tech(f);
		Member* target = targetOf(m);
		if (t == nullptr)
		{
			f.recovering = true;
			setState(m, CombatState::Hub);
			return;
		}
		if (target == nullptr)
		{
			stopTechnique(f);
			f.recovering = true;
			setState(m, CombatState::Hub);
			return;
		}
		// Interrupt (tryInterruptForThreat, `1406083d0(0.5, 0.5)`, BLOCK_TIMING_LIVE.md): the swing is abandoned for a defence only
		// while its own progress is <= 0.5 and the strongest threat's is >= 0.5 (1 of 349 swings in the 19 re-stages: practically never).
		auto threats = incoming(m);
		if (f.swingProgress < 0.5 && strongestThreat(threats, 0.5) != nullptr) // `<`: the exe tests `progress <= a && a != progress`
		{
			releaseSlot(*target, m.id);
			stopTechnique(f);
			f.pending = CombatState::Defend;
			setState(m, CombatState::Between);
			return;
		}
		const double p = clipProgress(f);
		const std::int32_t count = static_cast<std::int32_t>(t->impacts.size());
		std::int32_t n = 0;
		for (const ImpactPoint& impact : t->impacts)
		{
			if (impact.hitFrac < p)
				++n;
		}
		f.impactIndex = n;
		if (f.lastImpact < f.impactIndex)
			applyImpacts(m, f.lastImpact);
		f.lastImpact = f.impactIndex;
		if (f.impactIndex > count - 1)
		{
			f.impactIndex = count - 1;
			releaseSlot(*target, m.id);
		}
		if (f.techniqueFinished)
		{
			releaseSlot(*target, m.id);
			f.recovering = true;
			setState(m, CombatState::Hub);
		} else if (f.escapeAsked && n >= 1) {
			// updateSwing's +0x120 branch (CADENCE_LIVE.md Item 42): the opponent's attack pick after our impact asked decideEngagement
			// and we lost the roll. The swing is cut at the passed impact's stop frame (inferred): into the hold (the recovery timer is
			// applyImpacts' 0.6 / 0.3), or, past the technique's acceptable end time, into state 3 (defend).
			const ImpactPoint& passed = t->impacts[static_cast<std::size_t>(n - 1)];
			if (p >= passed.stopFrac)
			{
				releaseSlot(*target, m.id);
				stopTechnique(f);
				if (p < t->acceptableEndTime)
				{
					f.recovering = true;
					setState(m, CombatState::Hub);
				} else {
					f.pending = CombatState::Defend;
					setState(m, CombatState::Between);
				}
			}
		}
	}

	// The attacker's progress from the impact it passed last to the next one (0..1); the whole-clip progress to the first impact for impact 0.
	double segmentProgress(const Fighter& att) const
	{
		const Technique* t = tech(att);
		if (t == nullptr || t->impacts.empty() || att.impactIndex <= 0)
			return att.swingProgress;
		const std::size_t next = std::min<std::size_t>(static_cast<std::size_t>(att.impactIndex), t->impacts.size() - 1);
		const double from = t->impacts[next - 1].hitFrac;
		const double span = t->impacts[next].hitFrac - from;
		return span > 1.0e-9 ? std::clamp((clipProgress(att) - from) / span, 0.0, 1.0) : 1.0;
	}

	void defendState(Member& m)
	{
		Fighter& f = m.fighter;
		// A raised block holds its pose until the enemy swing has landed (the logged blocks resolve up to 0.78 s
		// after the pick, longer than the clip; the state leaves 1 within 0.02 s of the resolve).
		const Technique* t = tech(f);
		// chooseNextAction (140609c70, ENGAGEMENT.md 3.5 step 5; BLOCK_TIMING_LIVE.md "combo second hits"): every reactTimer
		// (+0x274, 1.0 s) or when the attacker's impact index changes, a defender that is not dodging re-picks if the
		// attack's direction no longer matches its technique's; a failed re-pick leaves state 1 (hub). The attacker's
		// index seen last is kept in `lastImpact`, which a defender's technique does not otherwise use (no new saved state).
		if (t != nullptr && !t->isDodge && !t->impacts.empty())
		{
			f.reactTimer -= dt;
			auto threats = incoming(m);
			Member* e = strongestThreat(threats, 0.0);
			const Technique* et = e != nullptr ? tech(e->fighter) : nullptr;
			if (et != nullptr && !et->isBlock && !et->impacts.empty())
			{
				const std::int32_t idx = std::min<std::int32_t>(e->fighter.impactIndex, static_cast<std::int32_t>(et->impacts.size()) - 1);
				if (idx != f.lastImpact || f.reactTimer <= 0.0)
				{
					f.lastImpact = idx;
					f.reactTimer = 1.0;
					const double dx = e->fighter.x - f.x;
					const double dz = e->fighter.z - f.z;
					std::int32_t dir = et->impacts[static_cast<std::size_t>(idx)].dir;
					if (dx * std::sin(f.yaw) + dz * std::cos(f.yaw) < 0.0)
						dir = dir == 1 ? 7 : (dir == 2 || dir == 3) ? 9 : dir;
					if (dir != t->impacts.front().dir)
					{
						// H11 cause 1 (CADENCE_LIVE.md Item 43 (d), 127 of the 208 runs): the block of impact 0 of a multi-impact attack
						// resolved, the next impact comes from another direction and chooseCounterTechnique fails (inferred: the attacker's
						// progress towards that impact, counted from the one just passed, is still below 0.1). setState(4) without the hold flag;
						// the hub picks the next block in its own frame (123 of 127), so the poll is armed now.
						const double fromPass = segmentProgress(e->fighter);
						if (fromPass < 0.1 || !chooseCounter(m, false))
						{
							f.actionTimer = -1.0;
							setState(m, CombatState::Hub);
							return;
						}
					}
				}
			}
		}
		t = tech(f);
		const bool holding = t != nullptr && t->isBlock && !incoming(m).empty();
		// State 1 steers (CADENCE_LIVE.md Item 34 (e)): block clips have no root motion, the follow pushes the blocker out of the ring.
		if (t != nullptr && t->isBlock)
			steer = defendRing(true);
		// A dodge's clip is over from progress 0.98 (Item 43 (d), from the decompile; DAT_14168db34).
		const bool dodgeOver = t != nullptr && t->isDodge && clipProgress(f) > 0.98;
		if (f.technique < 0 || ((f.techniqueFinished || dodgeOver) && !holding))
		{
			// H11 (CADENCE_LIVE.md Item 36 (a)): a dodge whose clip is over goes straight back to the hub (the frame after, 0.011 s,
			// 81 of the servant's runs); a block ends through the recovery countdown (H2, 0.2 s, getAttackRecoveryTime(2)).
			const bool dodged = t != nullptr && t->isDodge;
			stopTechnique(f);
			f.recovering = !dodged;
			f.recoverTimer = CombatRules::attackRecoveryTime(2);
			setState(m, CombatState::Hub);
		}
	}

	// State 8 (DUEL_DEFENCE_LIVE.md, kinspect item 27): there is NO defence poll in it and setState is ignored. The reaction clip
	// runs to its end (`mid blow light` 1.11 s; the heavy `mid blow` 4.63 s) and the state leaves into the hub, where the defence is
	// picked in that very frame (update() falls through to hub()). The one way out earlier is `updateStumbleState`'s escape roll: the
	// escape timer (+0x124, 1.2 s from the entry) runs down; once < 0 and +0x120 is set (by decideEngagement, an enemy asked about
	// this fighter), the attacker mid-swing at it (progress in [0.1, 0.99)) with the highest attack skill is taken, and
	// rand x 100 < calcDodgeChance(its skill) is success: pending Attack, state 3, action timer -1, then a stumble dodge is picked.
	// A failure pays defence XP and re-arms the timer at 1.2 s. UNSURE: the roll's own order relative to the clip end.
	void stumble(Member& m)
	{
		Fighter& f = m.fighter;
		f.stumbleTimer -= dt;
		// A player's move walk resumes kMoveStumbleTime after the hit (an order given mid-stagger too); the escape roll (a counter-attack)
		// never takes it over.
		const bool ordered = underMoveOrder(f);
		if (ordered)
			f.stumbleTimer = std::min(f.stumbleTimer, kMoveStumbleTime);
		if (f.stumbleTimer <= 0.0)
		{
			setState(m, CombatState::Hub, true);
			return;
		}
		if (ordered)
			return;
		// Item 44 (c): the staggered fighter's own AI update arms the gate (+0x120) every frame in 8; it does not wait for an
		// enemy's decideEngagement (that only set it in 40 of 75 episodes). Only the timer and an attacker in the window gate the roll.
		f.escapeTimer -= dt;
		if (f.escapeTimer >= 0.0)
			return;
		Member* e = nullptr;
		double best = -1.0;
		for (Member* t : incoming(m))
		{
			const double skill = CombatRules::attackValue(t->fighter);
			if (t->fighter.swingProgress >= 0.1 && t->fighter.swingProgress < 0.99 && skill > best)
			{
				best = skill;
				e = t;
			}
		}
		// No attacker with progress in [0.1, 0.99): no roll (CADENCE_LIVE.md Item 34 (b)); the timer stays below 0 and the poll continues.
		// UNSURE: re-arming the timer to 1.2 s here (2.45 escapes per fight against 3.2) pushed the bleed bands over (median 0.143, p90 0.275).
		if (e == nullptr)
			return;
		const bool won = draw(f.rng) * 100.0 < rules.dodgeChance(f, best);
		emit(CombatEvent::Kind::EscapeRoll, m.id, e->id, won ? 1 : 0);
		if (won)
		{
			stopTechnique(f);
			f.pending = CombatState::Attack;
			f.actionTimer = -1.0;
			setState(m, CombatState::Between, true);
			chooseCounter(m, true, e);
		} else {
			awardDefenceXp(f, best, false);
			f.escapeTimer = kStumbleHeavyTime;
		}
	}

	// The chase give-up monitor (`chaseGiveUpMonitor` 14033a800, called from Task_Move, so only while the fighter moves in on its target;
	// s73_static.md section 3). The goal layer holds a target while it is sensed or the attacker `learn`s of it (a fight never loses its
	// target to the memory); this task layer is what ends a chase of someone who got away. After 5 game minutes of chase, and with the
	// target not sensed by a scan: no sight record (never here), a record unsensed for more than 12 U (13.2 game minutes), or the chaser
	// within 60 u of the record's position, ends it: EV_GIVE_UP_CHASE, whose dialogues all go home. Here: the target is dropped and
	// forgotten, and the squad AI that carried the fighter before gives its order again. True when it gave up.
	bool chaseGivesUp(Member& m, const Member& target)
	{
		Fighter& f = m.fighter;
		if (f.playerControlled)
			return false; // the monitor is off for player-owned chasers (Task_Move N4)
		const Fighter& t = target.fighter;
		if (f.chaseId != target.id)
		{
			f.chaseId = target.id;
			f.chaseAge = 0.0;
			f.chaseUnseen = 0.0;
			f.chaseX = t.x;
			f.chaseZ = t.z;
		}
		f.chaseAge += dt;
		if (f.chaseAge > kChaseClearOrders)
		{
			// The monitor's `clearOrders` after 60 U (66 game minutes; s73_static.md section 3, H): the goal is force-cleared and replanned,
			// whether or not the target is sensed, so a target that stays in sight yet is never reached (across water, behind a wall) does not
			// hold the chaser for ever. The give-up path adds NO blocked-target timer (rwh.203: the 20 s block comes only from a move failure, the
			// reachability poll and CombatClassAI), so the target is picked again at the next selector burst. No GiveUpChase event (the original
			// emits it only on a give-up). The chase age restarts.
			setTarget(m, 0);
			f.chaseId = 0;
			return true;
		}
		if (perception::sensedWithin(f, target.id, kPerceivedSlack * perception::scanInterval(arena.members_.size())))
		{
			f.chaseUnseen = 0.0;
			f.chaseX = t.x;
			f.chaseZ = t.z;
			return false;
		}
		f.chaseUnseen += dt;
		if (f.chaseUnseen <= (arena.environment_.inWalls(t.x, t.z) ? kChaseTrackTown : kChaseTrackOpen))
		{
			f.chaseX = t.x;
			f.chaseZ = t.z;
		}
		if (f.chaseAge < kChaseMonitorStart
			|| (f.chaseUnseen <= kChaseGiveUpAge && std::hypot(f.x - f.chaseX, f.z - f.chaseZ) >= kChaseGiveUpRadius))
			return false;
		emit(CombatEvent::Kind::GiveUpChase, m.id, target.id, 0, 0, -1, f.chaseAge, f.chaseUnseen);
		perception::forget(f, target.id);
		setTarget(m, 0);
		f.chaseId = 0;
		return true;
	}

	void approach(Member& m)
	{
		Fighter& f = m.fighter;
		Member* target = pickTarget(m);
		if (target != nullptr && centre(f, target->fighter) > ringOuter() + 10.0 && chaseGivesUp(m, *target))
			target = nullptr;
		// The move-in ends within the ring on the target's own floor; a target on another floor keeps it going
		// until the stair is walked (task AS).
		m.lineBlocked = target != nullptr && !straightWalkClear(f, target->fighter);
		if (target == nullptr || (!floorApart(f, target->fighter) && centre(f, target->fighter) <= ringOuter() + 10.0 && !m.lineBlocked))
		{
			setState(m, CombatState::Hub);
			return;
		}
		steer = {true, 0.0, ringOuter(), 1.0e9, true};
	}

	const MeshNavigator* nav() const { return arena.navigation_.get(); }

	// The wall room a move keeps (Walker::step's radius): the body's path radius (the cut-out edge is the raster's, up to a cell off the hull), so a prop's cut-out (exact outline, no baked ring) and a wall keep
	// the body off by the query's convention, whatever moves it (kenshi-k16.7.129). ksim_prop_clip holds the body to the radius less 1.5 u.
	double bodyRoom(const Fighter& f) const { return arena.data().pathRadius(f.raceSid); }

	// Moves f by (dx, dz): through the Walker when there is a navmesh (slides along edges, sets y),
	// freely on the plane in a bare combat harness (setNavigation). A fighter the Walker finds off
	// the mesh does not move; the correction at the start of the next step deals with it.
	void shift(Fighter& f, double dx, double dz)
	{
		const MeshNavigator* n = nav();
		if (n == nullptr)
		{
			f.x += dx;
			f.z += dz;
			return;
		}
		const knav::StepResult r = n->walker().step({f.x, f.y, f.z}, dx, dz, bodyRoom(f));
		if (r.onMesh)
		{
			f.x = r.at.x;
			f.y = r.at.y;
			f.z = r.at.z;
		}
	}

	// A straight way from (x, z) to (tx, tz) that a body of the route's radius fits: the line and the two lines a radius to either side of it all
	// walk to the end without a block (the offset ones start off the mesh beside a wall, which says no).
	bool clearWay(const Fighter& f, double x, double z, double tx, double tz, bool sides = true) const
	{
		const MeshNavigator* n = nav();
		const double length = std::hypot(tx - x, tz - z);
		if (n == nullptr || length < 1.0e-6)
			return false;
		const double radius = arena.data().pathRadius(f.raceSid);
		const double sx = -(tz - z) / length * radius;
		const double sz = (tx - x) / length * radius;
		for (const double side : { 0.0, 1.0, -1.0 })
		{
			if (side != 0.0 && !sides)
				break;
			const knav::StepResult r = n->walker().step({x + sx * side, f.y, z + sz * side}, tx - x, tz - z);
			if (!r.onMesh || r.blocked || std::hypot(tx + sx * side - r.at.x, tz + sz * side - r.at.z) > 0.5)
				return false;
		}
		return true;
	}

	// A formation slot off the mesh is moved clear of the solid it is in (clearFormationSlot).
	void clearSlot(const Fighter& f, double& ax, double& az) const
	{
		if (nav() != nullptr && !clearFormationSlot(nav()->walker(), f.y, arena.data().pathRadius(f.raceSid), ax, az))
		{
			double px = 0.0;
			double pz = 0.0;
			standOffPush(nav()->walker(), f.y, ax, az, px, pz);
			if ((px != 0.0 || pz != 0.0) && nav()->walker().onMesh({ax + px, f.y, az + pz}))
			{
				ax += px;
				az += pz;
			}
		}
	}

	// A follower on the clearance route of a direct-mode follow keeps the direct steer's facing: the velocity while it is above 0.1 u/s.
	void faceRouteStep(Fighter& f, double mx, double mz) const
	{
		if (std::hypot(mx, mz) / dt > 0.1)
			f.yaw = std::atan2(mx, mz);
	}

	// f stands within its body radius of a wall or prop on either side of the heading (ux, uz): a step of the radius sideways is cut short.
	bool hugsWall(const Fighter& f, double ux, double uz) const
	{
		const double radius = arena.data().pathRadius(f.raceSid);
		for (const double side : { 1.0, -1.0 })
		{
			const knav::StepResult r = nav()->walker().step({f.x, f.y, f.z}, -uz * radius * side, ux * radius * side);
			if (r.onMesh && r.blocked)
				return true;
		}
		return false;
	}

	// The first waypoint of `f`'s route from `at` not yet passed by (x, z). A waypoint is passed when the position crosses its portal
	// (approximated by the plane through the corner square to the bisector of the legs in and out of it) or comes within a unit; the
	// last one is the arrival test. The step is a vector, so a corner the arc cuts inside of is not landed on.
	std::size_t passedWaypoints(const Fighter& f, std::size_t at, double x, double z) const
	{
		while (at + 1 < f.route.size())
		{
			// The route's start, a few units off where the walker stands, with a clear way from here to the next point: dropped, not walked to
			// (the walker beside the start passed it 1 u off, steered back for a tick and flipped 82 degrees, k16.8.8). A start behind a wall's
			// corner (the k16.8.1 re-path case, 7 u off) keeps the arrival rule.
			if (at == 0 && std::hypot(f.route[0].x - x, f.route[0].z - z) <= kFirstPointDrop && clearWay(f, x, z, f.route[1].x, f.route[1].z))
			{
				++at;
				continue;
			}
			// A player's move order shoved past its own start (a hit, a body, a stagger before the order took): it walks on from where it is,
			// not back to the start (the original re-plans nothing when displaced, COMBAT_MOVE_LIVE.md 4). Only with the clear way on.
			if (at == 0 && f.playerControlled && f.routeSource == state::Fighter::PlanSource::Order
				&& (x - f.route[0].x) * (f.route[1].x - f.route[0].x) + (z - f.route[0].z) * (f.route[1].z - f.route[0].z) >= 0.0
				&& clearWay(f, x, z, f.route[1].x, f.route[1].z))
			{
				++at;
				continue;
			}
			const state::RoutePoint& wp = f.route[at];
			const state::RoutePoint& before = f.route[at > 0 ? at - 1 : 0];
			const state::RoutePoint& after = f.route[at + 1];
			const double inLen = std::hypot(wp.x - before.x, wp.z - before.z);
			const double outLen = std::hypot(after.x - wp.x, after.z - wp.z);
			double bx = inLen > 1.0e-6 ? (wp.x - before.x) / inLen : 0.0;
			double bz = inLen > 1.0e-6 ? (wp.z - before.z) / inLen : 0.0;
			if (outLen > 1.0e-6)
			{
				bx += (after.x - wp.x) / outLen;
				bz += (after.z - wp.z) / outLen;
			}
			// The route's first point is where it starts, not a corner: a re-path from beside a wall starts the funnel pushed off that wall (the clear start,
			// up to a body's width away), and a walker past it along the first leg but off the leg is not on the clear way: it goes to the start first, not
			// straight at the next point across the wall's corner (the cause of a re-path loop against a wall, k16.8.1: 3 re-paths, abandoned 16 u from its bed).
			const bool crossed = at > 0 && (x - wp.x) * bx + (z - wp.z) * bz >= 0.0 && std::hypot(bx, bz) > 1.0e-3;
			if (!crossed && std::hypot(wp.x - x, wp.z - z) > kArriveReach)
				break;
			++at;
		}
		return at;
	}

	// How far (3D) `f` is from the route's window of points [routeNext-1 .. routeNext+4], the polyline the exe's face window stands for.
	static double distanceToRouteWindow(const Fighter& f, double x, double y, double z)
	{
		const std::size_t from = f.routeNext > 0 ? f.routeNext - 1 : 0;
		const std::size_t to = std::min<std::size_t>(f.routeNext + 4, f.route.size() - 1);
		double best = std::hypot(f.route[from].x - x, f.route[from].y - y, f.route[from].z - z);
		for (std::size_t i = from; i < to; ++i)
		{
			const state::RoutePoint& a = f.route[i];
			const state::RoutePoint& b = f.route[i + 1];
			const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
			const double len2 = abx * abx + aby * aby + abz * abz;
			const double t = len2 > 1.0e-9 ? std::clamp(((x - a.x) * abx + (y - a.y) * aby + (z - a.z) * abz) / len2, 0.0, 1.0) : 0.0;
			best = std::min(best, std::hypot(a.x + t * abx - x, a.y + t * aby - y, a.z + t * abz - z));
		}
		return best;
	}

	// PATH_WALK.md 3e: whether (x, z) lies in the corridor polygon of portals [portalNext-1, portalNext+4): the quads between consecutive portals
	// (a point portal makes a triangle). A window with a portal whose polygon ref is gone (a loaded save) cannot say: the caller falls back to the polyline.
	bool inCorridorWindow(const Fighter& f, double x, double z, bool& known) const
	{
		const std::size_t from = f.portalNext > 0 ? f.portalNext - 1 : 0;
		const std::size_t to = std::min<std::size_t>(f.portalNext + 4, f.portals.size());
		known = f.portalNext > 0 || (!f.portals.empty() && f.portals[0].point == 0); // before the first portal the walker is on the start leg, which no quad covers
		for (std::size_t i = from; i < to; ++i)
			known = known && f.portals[i].ref != 0;
		const auto inTri = [&](double ax, double az, double bx, double bz, double cx, double cz)
		{
			const double d1 = (bx - ax) * (z - az) - (bz - az) * (x - ax);
			const double d2 = (cx - bx) * (z - bz) - (cz - bz) * (x - bx);
			const double d3 = (ax - cx) * (z - cz) - (az - cz) * (x - cx);
			return !((d1 < 0.0 || d2 < 0.0 || d3 < 0.0) && (d1 > 0.0 || d2 > 0.0 || d3 > 0.0));
		};
		if (to - from == 1)
			return std::hypot(f.portals[from].lx - x, f.portals[from].lz - z) < 1.0;
		for (std::size_t i = from; i + 1 < to; ++i)
		{
			const state::RoutePortal& a = f.portals[i];
			const state::RoutePortal& b = f.portals[i + 1];
			if (inTri(a.lx, a.lz, a.rx, a.rz, b.rx, b.rz) || inTri(a.lx, a.lz, b.rx, b.rz, b.lx, b.lz))
				return true;
			if (a.isPoint() && b.isPoint()) // a leg between two point portals has no area: the polyline within the reach (kenshi-e9s9.7)
			{
				const double abx = b.lx - a.lx, abz = b.lz - a.lz, len2 = abx * abx + abz * abz;
				const double t = len2 > 1.0e-9 ? std::clamp(((x - a.lx) * abx + (z - a.lz) * abz) / len2, 0.0, 1.0) : 0.0;
				if (std::hypot(a.lx + t * abx - x, a.lz + t * abz - z) <= kOffPathReach * footprintOf(f))
					return true;
			}
		}
		return false;
	}

#if KSIM_DEV
	// kenshi-k16.8.39: where along the whole route `f` is nearest (3D distance, segment index), against the window the tracker tests.
	static std::pair<double, std::size_t> nearestRouteSegment(const Fighter& f)
	{
		double best = std::numeric_limits<double>::infinity();
		std::size_t at = 0;
		for (std::size_t i = 0; i + 1 < f.route.size(); ++i)
		{
			const state::RoutePoint& a = f.route[i];
			const state::RoutePoint& b = f.route[i + 1];
			const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
			const double len2 = abx * abx + aby * aby + abz * abz;
			const double t = len2 > 1.0e-9 ? std::clamp(((f.x - a.x) * abx + (f.y - a.y) * aby + (f.z - a.z) * abz) / len2, 0.0, 1.0) : 0.0;
			const double d = std::hypot(a.x + t * abx - f.x, a.y + t * aby - f.y, a.z + t * abz - f.z);
			if (d < best) { best = d; at = i; }
		}
		return {best, at};
	}
#endif

	// The walk of a move order: along the route the server made on the navmesh (knav::Walker::follow),
	// at the gait speed, accelerating at the race's pathfind acceleration and braking for sharp corners and the end
	// (knav::Walker::speedLimit), the body turning towards the next waypoint at 20 rad/s (s70 sections 2-3). Re-paths
	// after 5 s of barely moving. No walk/run choice, no avoidance (UNSURE, not original behaviour).
	// D5: the room an arc may overshoot into at a corner is measured (MeshNavigator::roomAhead), at most kMaxCornerRoomFactor x the footprint and at least the footprint (the route's own
	// clearance: a vertex the probe reads as pinned is no worse than before).
	static constexpr double kMaxCornerRoomFactor = 1.5; // x the footprint: the probe is not the arc's true swing, a wider room moved storm_door / group_settle / move_detour
	// The arc overshoots along the leg in and swings out to the outside of the turn (the direction in - out): the room is the shorter of the two runs.
	static double cornerRoom(const MeshNavigator& n, double footprint, const std::vector<state::RoutePoint>& route, double x, double z, double inX, double inZ, double outX, double outZ)
	{
		// At the corner's own height, not the walker's (on a slope the walker is 20+ u above a corner ahead, off the probe's window: the room read 0 until it came down, a jump in the limit).
		double y = 0.0;
		for (const state::RoutePoint& p : route)
			if (p.x == x && p.z == z)
				y = p.y;
		double room = n.roomAhead(x, y, z, inX, inZ, footprint, footprint * kMaxCornerRoomFactor);
		const double sideX = inX - outX;
		const double sideZ = inZ - outZ;
		const double side = std::hypot(sideX, sideZ);
		if (side > 1.0e-6)
			room = std::fmin(room, n.roomAhead(x, y, z, sideX / side, sideZ / side, footprint, footprint * kMaxCornerRoomFactor));
		return std::fmax(footprint, room); // never less than the footprint the route was planned with
	}
	// walkOrder, then the corridor state kept in step (PATH_WALK.md 3a; nothing walks by it yet): the corridor of a route no installer laid (an older
	// save) derived once, portalNext after routeNext, the velocity as a vector.
	void walkOrderSynced(Member& m, bool crawl = false)
	{
		Fighter& f = m.fighter;
		if (f.portals.empty() && f.route.size() >= 2)
			rebuildCorridor(f, nav(), bodyOf(f));
		else if (!f.portals.empty() && f.portals.front().ref == 0 && f.route.size() >= 2 && nav() != nullptr)
		{
			// A loaded walk: the portals' polygon refs are not saved (derived). Take them from the corridor the route gives again, so the off-path test
			// sees the same window as the uninterrupted walk (kenshi-e9s9.23, ksim_interact save + load).
			const std::vector<state::RoutePortal> again = nav()->corridorFor(f.route, bodyOf(f));
			if (again.size() == f.portals.size())
				for (std::size_t i = 0; i < again.size(); ++i)
					f.portals[i].ref = again[i].ref;
		}
		walkOrder(m, crawl);
		if (!dev::pathWalk())
			syncPortalNext(f); // the corridor walk keeps portalNext itself (portals share route points)
		if (dev::pathWalk() && !f.portals.empty())
		{
			// The corridor walk's velocity is its own vector (walkOrder set it), at the speed that was covered; the body's facing lags it at 20 rad/s and
			// would bend a set-off facing away from the aim through the facing (kenshi-e9s9.20).
			const double mag = std::hypot(f.vx, f.vz);
			if (mag > 1.0e-6)
			{
				f.vx *= f.forwardSpeed / mag;
				f.vz *= f.forwardSpeed / mag;
				return;
			}
		}
		const double heading = f.yaw == f.walkFacing ? f.walkHeading : f.yaw;
		f.vx = f.forwardSpeed * std::sin(heading);
		f.vz = f.forwardSpeed * std::cos(heading);
	}

	void walkOrder(Member& m, bool crawl = false)
	{
#if defined(KSIM_PROFILE) && KSIM_PROFILE
		const knav::measure::WalkTimer walkTimer;
#endif
		Fighter& f = m.fighter;
		const MeshNavigator* n = nav();
		if (n == nullptr)
		{
			// A route with no navmesh (a save loaded without one): abandoned, counted.
			++arena.routesBlocked_;
			f.clearRoute(4);
			return;
		}
		if (f.state != CombatState::Hub && !crawl)
			setState(m, CombatState::Hub, true); // a crawler keeps its Stumble pose (task BF)
		f.lateralSpeed = 0.0;
		if (m.walkHold > 0.0)
		{
			m.walkHold -= dt;
			f.forwardSpeed = 0.0;
			return;
		}
#if KSIM_DEV
		arena.walkStats_.walkSeconds += dt;
		arena.walkStats_.maybePrint(arena.time_);
#endif
		const knav::Gait gait = gaitOf(f);
		// KSIM_PATH_WALK (kenshi-e9s9.5): the corridor walk, PATH_WALK.md 3b / 3c. The aim is the per-tick funnel's first corner, pushed off the wall.
		const bool pw = dev::pathWalk() && !f.portals.empty();
		if (pw)
		{
			if (f.portalNext >= f.portals.size())
				f.portalNext = static_cast<std::uint32_t>(f.portals.size() - 1);
			f.routeNext = f.portals[f.portalNext].point;
			if (f.routeNext >= f.route.size())
				f.routeNext = static_cast<std::uint32_t>(f.route.size() - 1);
		} else {
			f.routeNext = static_cast<std::uint32_t>(passedWaypoints(f, f.routeNext, f.x, f.z)); // the funnel's next corner: not the one under the feet
		}
		const state::RoutePoint& next = f.route[f.routeNext];
		double cornerX = next.x, cornerZ = next.z; // pw: the pushed funnel corner (the goal when the funnel sees it)
		bool cornerGoal = f.routeNext + 1 == f.route.size();
		bool cornerLegEnd = false;
		double brakeToX = 0.0, brakeToZ = 0.0; // pw: the brake's toCorner (the flip check)
		double arcLimit = std::numeric_limits<double>::infinity(); // pw: the speed the turn after the corner allows (ours, D6)
		if (pw)
		{
			// The route's first point is where the walk starts, not a corner to aim at (a walker set down 2 u from it would aim back at it for a tick, then swing
			// 40+ degrees onto the leg: kenshi-e9s9.20). Its point portal is passed before the funnel runs.
			// So is the first portal's line when the walker stands within 4 u of it (a walk set down short of its first point): the funnel's corner would be the portal's near end for the first ticks.
			while (f.portalNext == 0 && f.portals.size() > 1 && (f.portals[0].isPoint() ? f.portals[0].point == 0 : f.portals[0].side(f.x, f.z) > -4.0))
				f.portalNext = 1;
			const state::RoutePoint& goal = f.route.back();
			const knav::FunnelCorner c = knav::firstCorner(f.portals, f.portalNext, f.x, f.z, goal.x, goal.z);
			cornerGoal = c.goal || c.index + 1 >= f.portals.size(); // the last portal is the route's end
			cornerLegEnd = cornerGoal && f.routeMoreLegs; // a far order's leg end is a straight continuation: no gate, no arrival brake (the next leg follows)
			cornerX = c.x;
			cornerZ = c.z;
			brakeToX = c.x - f.x;
			brakeToZ = c.z - f.z;
			const state::RoutePortal* end = c.goal ? nullptr : &f.portals[c.index];
			if (end != nullptr && !cornerGoal)
			{
				// D6 (ours, kenshi-e9s9.20): the turn at the corner and the road after it. The exe's brake looks at the first corner only, which lets a walker reach a
				// sharp corner with a short leg behind it at speed and orbit it. The leg out ends at the portal's route point (a point portal's: the next one).
				// A leg out shorter than kMinLeg is a kink of the same corner (route points 1-2 u apart), not a leg: the anchor is the first point beyond it.
				constexpr double kMinLeg = 8.0;
				std::size_t anchor = end->isPoint() ? end->point + 1 : end->point;
				while (anchor < f.route.size() && std::hypot(f.route[anchor].x - c.x, f.route[anchor].z - c.z) < kMinLeg)
					++anchor;
				if (anchor < f.route.size())
				{
					const double outX = f.route[anchor].x - c.x, outZ = f.route[anchor].z - c.z;
					const double inX = c.x - f.x, inZ = c.z - f.z;
					// The leg out runs along the route until the next real turn (more than ~6 degrees off its first direction) or the goal: a bend followed by a
					// near-straight polyline is one long leg, not an 8 u one (kenshi-e9s9.23, ksim_group_settle).
					constexpr double kSlightBend = 6.0 * 3.14159265358979323846 / 180.0;
					const double firstHeading = std::atan2(outX, outZ);
					const double firstLen = std::hypot(outX, outZ);
					double outLen = firstLen;
					for (std::size_t k = anchor + 1; k < f.route.size(); ++k)
					{
						const double segX = f.route[k].x - f.route[k - 1].x, segZ = f.route[k].z - f.route[k - 1].z;
						if (std::hypot(segX, segZ) < 1.0e-6)
							continue;
						double turnOut = std::fabs(std::atan2(segX, segZ) - firstHeading);
						turnOut = std::min(turnOut, 2.0 * 3.14159265358979323846 - turnOut);
						if (turnOut > kSlightBend)
							break;
						outLen = std::hypot(f.route[k].x - c.x, f.route[k].z - c.z);
					}
					const double inLen = std::hypot(inX, inZ);
					if (inLen > 1.0e-3)
					{
						const double turn = std::acos(std::clamp((inX * outX + inZ * outZ) / (inLen * firstLen), -1.0, 1.0));
						arcLimit = knav::arcCornerSpeed(turn, gait.acceleration, 0.35 * outLen);
					}
				}
			}
			const double sight = std::hypot(brakeToX, brakeToZ);
			// A point portal is the planner's own bend, a taut path round a wall's vertex: the exe's corner there is a portal end whose clearance is the vertex's own (0 at a wall's
			// vertex, rwh.308), so the aim is pushed r + 5 - clearance off the vertex, to the outside of the turn (the wall is on its inside). Ours: only where the route turns by more than
			// ~15 degrees (a straight-through point would jink 9 u sideways), and a point with no route point after it (the goal) gets none.
			double pointSide = 0.0;
			if (end != nullptr && end->isPoint() && sight > 1.0e-6 && end->point + 1 < f.route.size())
			{
				const double outX = f.route[end->point + 1].x - c.x, outZ = f.route[end->point + 1].z - c.z;
				const double outLen = std::hypot(outX, outZ);
				const double turnCross = brakeToX * outZ - brakeToZ * outX;
				if (outLen > 1.0e-6 && std::fabs(turnCross) / (sight * outLen) > std::sin(15.0 * 3.14159265358979323846 / 180.0) && brakeToX * outX + brakeToZ * outZ > -0.99 * sight * outLen)
					pointSide = turnCross > 0.0 ? -1.0 : 1.0;
			}
			if (end != nullptr && (!end->isPoint() || pointSide != 0.0) && sight > 1.0e-6)
			{
				const double push = end->isPoint() ? std::max(0.0, gait.footprint + 5.0 - std::min(end->clearLeft, end->clearRight)) * std::min(1.0, sight / 12.0) : std::max(0.0, gait.footprint + 5.0 - (c.left ? end->clearLeft : end->clearRight));
				const double side = end->isPoint() ? pointSide : c.left ? 1.0 : -1.0; // a left end is pushed to the right of the sight line, a right end to the left
				const double px = side * brakeToZ / sight * push;
				const double pz = -side * brakeToX / sight * push;
				cornerX += px;
				cornerZ += pz;
				// The flip check (rwh.268 section 6): the brake's toCorner is the pushed vector unless the push cuts progress along v.
				const double sp = std::hypot(f.vx, f.vz);
				if (sp < 1.0e-6 || (px * f.vx + pz * f.vz) >= 0.0)
				{
					brakeToX = cornerX - f.x;
					brakeToZ = cornerZ - f.z;
				}
			}
		}
		// A fighter its squadmates follow walks at the group's pace, not the gait's top speed: the carry gives a world squad's
		// leader a short route every step, so at the top speed it would reach each carrot and stand until the next step.
		const bool paced = !f.playerControlled && isFollowed(m.id);
		const double top = crawl ? std::min(knav::gaitSpeed(gait), CombatArena::kCrawlSpeed)
			: paced ? std::min(knav::gaitSpeed(gait), UtilityGroupLayer::kWalkSpeed) : knav::gaitSpeed(gait);
		const double toX = cornerX - f.x;
		const double toZ = cornerZ - f.z;
		const double toGo = std::hypot(toX, toZ);
		// D5: our route corners clear by the route's own radius (pathRadius = the footprint), not the exe's footprint + 5, so the corner speed comes from
		// the geometry of the arc (knav::arcCornerSpeed), not from the exe's sqrt(2 a L): a near-straight corner is not braked for, a 90-degree turn in a
		// short leg slows to a near-stop and pivot (speedLimit chains it back along the route).
		// The slew's budget a dt a tick is shared with turning the velocity towards the leg: what is left for braking is sqrt((a dt)^2 - (v sin(error))^2),
		// the error being the angle between the velocity and the way to the corner (none left once the lateral change exceeds the budget), so the
		// stopping distance of a corner's first leg counts for that share of it, not for all of it.
		double brakeShare = 1.0;
		if (toGo > 1.0e-6 && std::fabs(f.forwardSpeed) > 1.0e-6)
		{
			const double lateral = std::min(1.0, std::fabs(f.forwardSpeed) * std::fabs(std::sin(f.yaw) * toZ - std::cos(f.yaw) * toX) / toGo / (gait.acceleration * dt));
			brakeShare = std::sqrt(1.0 - lateral * lateral);
		}
		const double limit = pw ? top : knav::Walker::speedLimit({f.x, f.y, f.z}, f.route, f.routeNext, top,
			gait.acceleration, gait.footprint, true, std::fabs(f.forwardSpeed) * dt, brakeShare, std::fabs(f.forwardSpeed),
			[&](double x, double z, double inX, double inZ, double outX, double outZ) { return cornerRoom(*n, gait.footprint, f.route, x, z, inX, inZ, outX, outZ); });
		// The step never overshoots the route's end, measured along the route: the crow to the end reads 0 when the
		// end is on the floor above (its x,z is the walker's), and braked the walk to a stop under the goal (task AQ).
		double remaining = pw ? std::hypot(next.x - f.x, next.z - f.z) : toGo;
		for (std::size_t i = f.routeNext; i + 1 < f.route.size(); ++i)
			remaining += std::hypot(f.route[i + 1].x - f.route[i].x, f.route[i + 1].z - f.route[i].z);
		// The arc's speed holds at the corner, not all along the approach: the walker may carry what it can still brake down to it in the distance (D7, ours).
		const double arcAhead = std::isfinite(arcLimit) ? std::sqrt(arcLimit * arcLimit + 2.0 * gait.acceleration * toGo) : arcLimit;
		double desired = std::min(std::min(limit, arcAhead), remaining / dt);
		double aimX = toGo > 1.0e-6 ? toX / toGo * desired : 0.0;
		double aimZ = toGo > 1.0e-6 ? toZ / toGo * desired : 0.0;
		double perpAim = 0.0;
		// A corner within one step (at least 1 u) that is not the end is reached this tick: the aim would flip to behind the walker on the next, reverse the
		// velocity through zero and stall it on the vertex (a funnel corner with no push, or a point portal; kenshi-e9s9.20). Keep the heading, no brake;
		// the portal advance moves the aim on once the walker is across.
		const double heading = std::hypot(f.vx, f.vz);
		const bool atCorner = pw && !cornerGoal && heading > 1.0e-3 && toGo < std::max(1.0, heading * dt);
		if (atCorner)
		{
			aimX = f.vx / heading * desired;
			aimZ = f.vz / heading * desired;
		}
		if (pw && toGo > 1.0e-6 && !atCorner)
		{
			// PATH_WALK.md 3c: the brake on the funnel's first corner only. Gate: proj - 1 u < s^2 / 2a; speed: sqrt(2 a 10 u |sin|), sin against the unit funnel direction.
			const double s = std::hypot(f.vx, f.vz);
			const double dx = toX / toGo;
			const double dz = toZ / toGo;
			if (s > 1.0e-3)
			{
				const double hx = f.vx / s;
				const double hz = f.vz / s;
				const double proj = hx * brakeToX + hz * brakeToZ;
				// A goal not dead ahead (the funnel sees it as the corner is rounded) is braked for on the crow distance, not its projection on the velocity, and
				// with the budget the turn leaves (as brakeShare does for a corner): v lands on it, not past it (kenshi-e9s9.23, ksim_move_detour: 80 degrees at the end).
				const double goalAhead = cornerGoal ? 0.0 : proj; // the goal's profile below caps by itself, no gate
				if (!cornerLegEnd && goalAhead - 1.0 < s * s / (2.0 * gait.acceleration))
				{
					if (cornerGoal)
					{
						{
						// Arrival: stops at the goal. The slew sheds a dt of speed a tick and each tick steps at its new speed, so the speed that stops in toGo is
						// u with u^2/2a + u dt = toGo (a tick of margin for the quantised steps), not the continuous sqrt(2 a toGo) (which arrives at ~20 u/s and snaps from it).
						const double half = gait.acceleration * dt;
						desired = std::min(desired, std::sqrt(half * half + 2.0 * gait.acceleration * toGo * std::pow(std::clamp(proj / toGo, 0.2, 1.0), 3.0)) - half);
					}
					} else {
						const double sine = hz * dx - hx * dz; // dot(perp(v_hat), d), perp(v) = (v.z, -v.x)
						const double k = std::fabs(2.0 * gait.acceleration * 10.0 * sine);
						if (desired * desired > k)
							desired = std::min(desired, std::max({std::sqrt(k), std::sqrt(2.0 * gait.acceleration), arcLimit})); // floor: the speed that stops in 1 u, so a dead-straight corner never stalls the walker (ours); and what the arc after the corner allows (D7, ours: a corner headed at straight with a slight bend behind it is not braked to the floor, kenshi-e9s9.23)
						else
							perpAim = sine < 0.0 ? -1.0 : 1.0; // the corner is across the travel: the velocity goes sideways at vmax (rwh.268 section 5)
					}
					aimX = perpAim != 0.0 ? perpAim * hz * desired : dx * desired;
					aimZ = perpAim != 0.0 ? -perpAim * hx * desired : dz * desired;
				}
			}
		}
		// The velocity vector slews towards desired x the direction to the next corner by at most
		// accel x dt, so a corner is an arc and a reversal brakes through zero (HAVOKCHAR_EXE.md section 3, updateVelocity). UNSURE (ours): the
		// corner brake is speedLimit over the route's corners, not the exe's lateral-offset test of the first corner alone.
		// The velocity so far is forwardSpeed along the body's facing (the facing follows the velocity above 4 u/s, so they agree; it is
		// not last tick's net displacement, which a push, a teleport or a slide into a wall would corrupt).
		const double memory = f.yaw == f.walkFacing ? f.walkHeading : f.yaw;
		// The corridor walk keeps the velocity's own heading (f.vx, f.vz: last tick's command): the body's facing turns at 20 rad/s and a walker that sets off
		// facing away from its aim would swing its velocity through the facing (a 30+ degree turn a tick at the route start, kenshi-e9s9.20).
		const double heldSpeed = std::hypot(f.vx, f.vz);
		const knav::Velocity held = pw && heldSpeed > 1.0e-3 ? knav::Velocity{f.vx / heldSpeed * f.forwardSpeed, f.vz / heldSpeed * f.forwardSpeed}
			: knav::Velocity{f.forwardSpeed * std::sin(memory), f.forwardSpeed * std::cos(memory)};
		knav::Velocity v = knav::slewVelocity(held, aimX, aimZ, gait.acceleration, dt);
		const bool last = pw ? cornerGoal && f.portalNext + 1 == f.portals.size() : f.routeNext + 1 == f.route.size();
		const bool snap = last && std::hypot(v.x, v.z) * dt >= toGo;
		if (snap)
			v = {toX / dt, toZ / dt};
		// The body faces the velocity above 4 u/s, the way to the next corner below it (s70 section 3), at no more than 20 rad/s.
		const double speed = std::hypot(v.x, v.z);
		// A player's move walk takes no push at all (playerOrderWalk: it walks through its squadmates, the original's group move, k16.6.231), so the
		// facing is the travel direction; the squadmate push of k16.6.182's net-displacement facing is gone with it.
		const bool byVelocity = speed > kFaceByVelocity;
		// The corridor walk's last step (under max(1 u, a step) to the goal) holds the facing of the last real step: a pivot towards a goal a hair away is noise
		// (kenshi-e9s9.23, ksim_group_settle facing at rest).
		const bool holdFacing = pw && cornerGoal && toGo < std::max(1.0, speed * dt);
		if (!holdFacing)
			f.yaw = knav::turnToward(f.yaw, byVelocity ? std::atan2(v.x, v.z) : std::atan2(toX, toZ), knav::kTurnRate, dt);
		f.walkHeading = f.yaw; // saved with the fighter (v109); the memory and the yaw are one again
		f.walkFacing = f.yaw;
		f.forwardSpeed = snap ? 0.0 : speed;
		std::size_t at = f.routeNext;
		// Walking into the character that pushed it this step (within ~37 degrees of head-on): the step bends half its length sideways, to the side
		// the push leans to, and slides around it (bq16_deadlocks.md Q1 b, 140147ca0). UNSURE: the exe also adds the push into the velocity and
		// acceleration-limits it; ours applies the push to the position only, and the bent step is taken off the route (the next step re-aims).
		// The chase step has no sidestep yet (its target is usually the neighbour).
		const double pushed = std::hypot(m.pushX, m.pushZ);
		knav::StepResult r;
		if (pushed > 0.0 && toGo > 1.0e-6 && (m.pushX * toX + m.pushZ * toZ) / (toGo * pushed) < -0.8)
		{
			const double ux = toX / toGo;
			const double uz = toZ / toGo;
			const double side = m.pushX * uz - m.pushZ * ux < 0.0 ? -0.5 : 0.5;
			const double step = speed * dt;
			r = n->walker().step({f.x, f.y, f.z}, (ux + side * uz) * step, (uz - side * ux) * step, bodyRoom(f));
		} else {
			r = n->walker().step({f.x, f.y, f.z}, v.x * dt, v.z * dt);
		}
		if (r.onMesh && !pw)
			at = passedWaypoints(f, at, r.at.x, r.at.z);
		// Barely moving while asked to: 5 s of it is a re-path, like a block (s70 section 4: 5 s off the corridor). The progress is how much closer the
		// step got to its waypoint, from where it stood before the push: a walker pressing into (or sidestepping around, side to side) a character that
		// stands on its way gains its step and loses it to the push, and would never stop.
		const double asked = speed * dt;
		// A step that hit the mesh edge leaves the velocity the distance actually covered (the exe: velocity := (pos - old) / dt after a hit).
		if (r.blocked && r.onMesh)
			f.forwardSpeed = std::min(speed, std::hypot(r.at.x - f.x, r.at.z - f.z) / dt); // never above what was commanded (the mesh's float32 coordinates blur the distance by ~0.08 u/s)
		// After a hit the velocity is the slide's displacement as a vector, not the commanded direction at the lower speed (rwh.271: v := (pos - old) / dt), so the next
		// tick's slew starts along the wall instead of into it.
		const knav::Velocity carried = pw && r.blocked && r.onMesh ? knav::Velocity{(r.at.x - f.x) / dt, (r.at.z - f.z) / dt} : v;
		const double progress = std::hypot(toX + m.pushX, toZ + m.pushZ) - std::hypot((pw ? cornerX : next.x) - r.at.x, (pw ? cornerZ : next.z) - r.at.z);
		f.walkStuck = asked > 0.01 && progress < 0.1 * asked ? f.walkStuck + dt : 0.0;
		if (r.onMesh)
		{
			f.x = r.at.x;
			f.y = r.at.y;
			f.z = r.at.z;
			f.routeNext = static_cast<std::uint32_t>(at);
			if (!last && at + 1 < f.route.size())
			{
				// A corner the arc cut leaves the walker on the mesh edge, 0.75 u off a prop (the ring): eased back inside kSlotStandOff of it
				// (kenshi-k16.6.548). Not on the last leg: the goal may stand at an edge and the walker must arrive.
				double px = 0.0;
				double pz = 0.0;
				standOffPush(n->walker(), f.y, f.x, f.z, px, pz);
				shift(f, px, pz);
			}
			if (pw)
			{
				// Portal advance (3b step 5): the position on the far side of the portal's line; a point portal by the plane across its leg. The last
				// portal is passed only by the stop snap.
				while (f.portalNext + 1 < f.portals.size())
				{
					const state::RoutePortal& pt = f.portals[f.portalNext];
					bool passed;
					if (!pt.isPoint())
						passed = pt.side(f.x, f.z) > -kOnPortalLine; // a walker sliding along the portal's own line (a zone border) never gets strictly across it
					else
					{
						const state::RoutePoint& a = f.route[pt.point > 0 ? pt.point - 1 : 0];
						// Or within a step (at least 1 u) of it: the aim would flip to behind the walker next tick, reverse the velocity through zero and stall it at the point (kenshi-e9s9.20).
						passed = (f.x - pt.lx) * (pt.lx - a.x) + (f.z - pt.lz) * (pt.lz - a.z) >= 0.0 || std::hypot(f.x - pt.lx, f.z - pt.lz) < std::max(1.0, speed * dt);
					}
					if (!passed)
						break;
					++f.portalNext;
				}
				if (snap)
					f.portalNext = static_cast<std::uint32_t>(f.portals.size());
				f.routeNext = f.portalNext < f.portals.size() ? f.portals[f.portalNext].point : static_cast<std::uint32_t>(f.route.size());
				f.vx = carried.x;
				f.vz = carried.z;
			}
		}
		// Off the route for 5 s: the path is re-requested from here to the same goal (the exe's timeOffPath > 5.0 s -> BROKEN -> requestPath, the
		// walk keeps going on the old route meanwhile). Back on the route resets it. Not counted toward kMaxReroutes: it is the exe's own
		// recovery, one per 5 s off the route, and a failed re-request keeps the old route and tries again 5 s later.
		bool corridorKnown = false;
		const bool inCorridor = pw && inCorridorWindow(f, f.x, f.z, corridorKnown);
		const bool onRoute = r.onMesh && (pw && corridorKnown ? inCorridor : distanceToRouteWindow(f, f.x, f.y, f.z) <= kOffPathReach * footprintOf(f)); // 3e: the polygon; the polyline within 3 footprints only when a portal's ref is stale
		f.offPath = onRoute ? 0.0 : f.offPath + dt;
		if (f.offPath > kOffPathRepath)
		{
			f.offPath = 0.0;
			const state::RoutePoint end = f.route.back();
#if KSIM_DEV
			const auto [wholeDist, seg] = nearestRouteSegment(f);
			const std::size_t winFrom = f.routeNext > 0 ? f.routeNext - 1 : 0;
			const std::size_t winTo = std::min<std::size_t>(f.routeNext + 4, f.route.size() - 1);
#endif
			if (replanWalk(m, end, Fighter::kReplanOffPath))
			{
#if KSIM_DEV
				dev::WalkStats& ws = arena.walkStats_;
				++ws.windowReplans;
				if (r.onMesh && wholeDist <= kOffPathReach * footprintOf(f))
				{
					++ws.windowLost;
					if (seg + 1 < winFrom) ++ws.windowLostBehind;
					else if (seg > winTo) ++ws.windowLostAhead;
				} else {
					++ws.windowReal;
				}
#endif
				++arena.offPathRepaths_;
				f.swingProgress = computeSwingProgress(f);
				return;
			}
#if KSIM_DEV
			++arena.walkStats_.windowFailed;
#endif
		}
		const bool stuck = f.walkStuck >= kStuckRepath;
		if (stuck)
		{
			f.walkStuck = 0.0;
			// Held within one footprint sum of the end (characters stand on it): arrived, as close as the crowd allows. OURS, UNSURE: the exe's
			// arrival in a crowd is not decoded (BQ16 follow-up).
			const state::RoutePoint& end = f.route.back();
			if (std::hypot(end.x - f.x, end.z - f.z) <= 2.0 * footprintOf(f))
			{
#if KSIM_DEV
				const double gap = std::hypot(end.x - f.x, end.z - f.z);
				++arena.walkStats_.stuckArrivals;
				arena.walkStats_.stuckArrivalGapSum += gap;
				arena.walkStats_.stuckArrivalGapMax = std::max(arena.walkStats_.stuckArrivalGapMax, gap);
#endif
				const bool moveOrder = f.routeSource == Fighter::PlanSource::Order; // a move order's walk, not an interact's (the use point is its stop)
				f.routeSeq = 0; // an order's route that arrived is done (routeSeq is the live order: world_tasks.cpp freeMember)
				f.clearRoute(Fighter::kClearArrived);
				f.forwardSpeed = 0.0;
				f.swingProgress = computeSwingProgress(f);
				if (f.playerControlled && moveOrder)
					settleSelf(m);
				return;
			}
		}
		// A slide that bent the step by a hair is not a shut way: a clearance route runs past walls, and the walker grazes
		// an edge nearly every tick without losing ground (the distance the step covered says which it is: an arc's progress toward its waypoint
		// can be nil while it moves freely, and that must not start a re-path).
		// A step into a wall on momentum (the velocity points more than ~45 degrees off the way to the waypoint: an arc's overshoot) loses
		// the velocity and re-aims next tick; only a step aimed at the waypoint that goes nowhere is a shut way, never a re-path loop.
		const bool aimed = speed > 1.0e-6 && toGo > 1.0e-6 && (v.x * toX + v.z * toZ) / (speed * toGo) > 0.7;
		const bool blocked = (r.blocked && r.moved < 0.5 * asked && aimed) || stuck;
		if (blocked && f.routeTries < kMaxReroutes)
		{
			++f.routeTries;
			const state::RoutePoint end = f.route.back();
			if (replanWalk(m, end, Fighter::kReplanBlocked))
			{
#if KSIM_DEV
				++(stuck ? arena.walkStats_.stuckReplans : arena.walkStats_.blockedReplans);
#endif
				++arena.reroutes_;
				f.swingProgress = computeSwingProgress(f);
				return;
			}
#if KSIM_DEV
			++arena.walkStats_.replanFailed;
#endif
		}
		if (blocked && f.routeNext + 1 < f.route.size())
		{
			// Out of re-paths at a corner the momentum carried it into: the velocity is lost (forwardSpeed 0), and if the leg after the corner is open from where
			// it stands (a slide test of up to 10 u towards its end) it resumes on that leg, not abandoning a route that is reachable.
			const state::RoutePoint& onward = f.route[f.routeNext + 1];
			const double ahead = std::hypot(onward.x - f.x, onward.z - f.z);
			if (ahead > 1.0e-6)
			{
				const double probe = std::min(ahead, 10.0);
				const knav::StepResult open = n->walker().step({f.x, f.y, f.z}, (onward.x - f.x) / ahead * probe, (onward.z - f.z) / ahead * probe);
				if (open.onMesh && open.moved >= 0.5 * probe)
				{
					++f.routeNext;
					f.forwardSpeed = 0.0;
					f.swingProgress = computeSwingProgress(f);
					return;
				}
			}
		}
		if (blocked)
		{
			++arena.routesBlocked_; // out of retries (or nothing to route): abandoned
#if KSIM_DEV
			++arena.walkStats_.abandoned;
#endif
		}
		// Arrived within 1 of the end on the last leg (bq16_deadlocks.md Q2, 140144440): a walker the push holds just short of it (a character
		// standing on it, the sidestep bending the last step off) would never land on it exactly. It stops where it is, like the exe (stepping onto
		// the end instead cost ksim_world_follow 7 of its 22 moving hours).
		// Only once the walk has braked to what one slew step can drop (accel x dt) or is held (walkStuck): the end's brake takes it to 0 at the
		// target, and cutting it off at 1 u stopped a walker doing ~30 u/s dead in one tick (kenshi-k16.8.9). UNSURE (ours): the exe's last
		// ticks are not recorded; default ease to 0. deferred: playtesting.
		if (f.routeNext + 1 == f.route.size() && std::hypot(f.route.back().x - f.x, f.route.back().z - f.z) <= kArriveReach
			&& (std::fabs(f.forwardSpeed) <= gait.acceleration * dt + 1.0e-9 || f.walkStuck > 0.0))
			f.routeNext = static_cast<std::uint32_t>(f.route.size());
		if (blocked || !f.walking())
		{
			if (!blocked)
			{
#if KSIM_DEV
				const double gap = std::hypot(f.route.back().x - f.x, f.route.back().z - f.z);
				++arena.walkStats_.walks;
				arena.walkStats_.endGapSum += gap;
				arena.walkStats_.endGapMax = std::max(arena.walkStats_.endGapMax, gap);
				if (gap > 1.0) ++arena.walkStats_.endOver1;
#endif
				const bool moveOrder = f.routeSource == Fighter::PlanSource::Order;
				f.routeSeq = 0; // an order's route that arrived is done (a blocked one keeps its seq: the order still stands)
				if (f.playerControlled && moveOrder)
					settleSelf(m);
			}
			f.clearRoute(blocked ? Fighter::kClearWayShut : Fighter::kClearArrived); // arrived, or the way is shut
			f.forwardSpeed = 0.0;
			f.walkStuck = 0.0;
		}
		f.swingProgress = computeSwingProgress(f);
	}

	// A walk's re-request (off the route, or blocked) to its own goal. A failed one blocks that goal for kBlockedTarget (the exe's
	// addBlockedTarget on a failed request): asks for it, or for one within kRequestDedup of it, return false without a search. The
	// old corridor stays meanwhile. Each failed ask is two searches (kenshi-k16.6.262).
	bool replanWalk(Member& m, const state::RoutePoint& end, std::uint8_t why)
	{
		const auto it = arena.replanFail_.find(m.id);
		if (it != arena.replanFail_.end() && arena.time_ < it->second.until
			&& std::hypot(end.x - it->second.x, end.z - it->second.z) <= kRequestDedup)
			return false;
		Fighter& f = m.fighter;
		if (replan(f, end.x, end.z, false, f.routeApproach, end.y, why))
		{
			if (it != arena.replanFail_.end())
				arena.replanFail_.erase(it);
			return true;
		}
		arena.replanFail_[m.id] = {end.x, end.z, arena.time_ + kBlockedTarget};
		return false;
	}

	// Plans a new route from where f stands to (tx, tz) and installs it (heights from the mesh, routeVersion
	// bumped, routeSeq untouched for an order's reroute). False: no usable route; the old one stays.
	// `ty`: the end's height when known (an order's reroute keeps its route's end). Without it the end is
	// snapped from f's own height, which on a slope can pick other ground 40 u off the planned end
	// (ksim_nav_regions: a reroute on the ridge above the iron deposit ended on a rock top short of it).
	bool replan(Fighter& f, double tx, double tz, bool chase, bool approach, std::optional<double> ty = std::nullopt, std::uint8_t why = Fighter::kReplanChase)
	{
		const MeshNavigator* n = nav();
		if (n == nullptr)
			return false;
#if KSIM_PROFILE
		const knav::measure::WhoScope whoScope(f.diceSeed, 2);
#endif
		const Route again = routeForBody(*n, {f.x, f.y, f.z}, tx, tz, ty, bodyOf(f));
		if (!again.found || again.points.size() < 2)
			return false;
		std::vector<state::RoutePoint> points;
		for (std::size_t i = 0; i < again.points.size(); ++i)
			points.push_back({again.points[i].x, again.heights[i], again.points[i].z});
		setRoute(f, std::move(points), n, bodyOf(f));
		f.offPath = 0.0;
		++f.routeVersion;
		f.clearWhy = why;
		f.routeChase = chase;
		f.routeApproach = approach;
		if (chase)
			f.routeSeq = 0; // the AI's own route
		return true;
	}

	// The chase of an attack target farther than the ring, by route (see kChaseDirectBelow). Called from
	// integrate when `dist` (the straight distance to the target) calls for a route. Follows the
	// chase route at `speed` (ramped to it, or at it at once), re-planning when the target (tx, tz) has moved
	// `replanDrift` (kChaseReplan; a follower's slot: kFollowReplan) from the plan, when it has changed floor (the plan's end height against `ty`), when the route
	// runs out short of it, and after a block (bounded by kMaxReroutes). The follow task's path mode uses it too,
	// with its slot as the target (`target` false: a leader that cannot be routed to is not dropped).
	// False: a target chase (`target` true) whose fresh plan finds no route at all — the target is unreachable.
	// True when the body's y is more than kOffSurface from the route's surface at the nearest point of the leg it walks (routeNext - 1 -> routeNext).
	static bool offRouteSurface(const Fighter& f)
	{
		if (!f.routeChase || f.routeNext >= f.route.size())
			return false;
		const state::RoutePoint& b = f.route[f.routeNext];
		const state::RoutePoint& a = f.route[f.routeNext > 0 ? f.routeNext - 1 : 0];
		const double lx = b.x - a.x;
		const double lz = b.z - a.z;
		const double len2 = lx * lx + lz * lz;
		const double t = len2 > 1.0e-9 ? std::clamp(((f.x - a.x) * lx + (f.z - a.z) * lz) / len2, 0.0, 1.0) : 1.0;
		return std::fabs(f.y - (a.y + (b.y - a.y) * t)) > kOffSurface;
	}

	bool chaseStep(Fighter& f, double tx, double tz, std::optional<double> ty, double speed, bool ramp, bool target, double accel = kFootworkAcceleration, double replanDrift = kChaseReplan)
	{
		const MeshNavigator* n = nav();
		f.lateralSpeed = 0.0;
		f.chasePlanAge += dt;
		if (f.chaseCooldown > 0.0)
		{
			f.chaseCooldown -= dt;
			f.forwardSpeed = 0.0;
			return true;
		}
		const bool stale = !f.routeChase || f.routeNext >= f.route.size()
			|| std::hypot(tx - f.chaseTx, tz - f.chaseTz) > replanDrift
			|| (ty && !f.route.empty() && std::fabs(f.route.back().y - *ty) > kFloorApart)
			|| offRouteSurface(f);
		if (stale && (!f.routeChase || f.chasePlanAge >= kChaseMinPlan))
		{
			f.chasePlanAge = 0.0;
			f.chaseTx = tx;
			f.chaseTz = tz;
			if (!replan(f, tx, tz, true, true, ty))
			{
				// The old route may still be walkable (the target moved); an exhausted one is no route at all.
				if (f.routeChase && f.routeNext < f.route.size())
					return true;
				f.clearRoute(Fighter::kClearGiveUp);
				f.forwardSpeed = 0.0;
				f.chaseCooldown = kChaseCooldown; // nothing usable: wait, then ask again
				return !target;
			}
		}
		if (f.routeNext >= f.route.size())
		{
			f.forwardSpeed = 0.0;
			return true;
		}
		knav::StepResult r;
		if (f.building == 0)
		{
			// Outside, the chase steers like a move order's walk (k16.8.24): the velocity slews towards the next corner at the race's pathfind
			// acceleration (a slow turner brakes through the turn and pivots, then catches up), the body facing the velocity.
			// The speed builds at the race's own acceleration (the slew), not the caller's footwork ramp: a body that turned about would re-ramp at 15 u/s^2 and lose the
			// chase (ksim_bw_orders). It never exceeds `speed`, the chase's own cap (a crawler's, a gait's).
			f.routeNext = static_cast<std::uint32_t>(passedWaypoints(f, f.routeNext, f.x, f.z));
			const state::RoutePoint& next = f.route[f.routeNext];
			const knav::Gait gait = gaitOf(f);
			const double toX = next.x - f.x;
			const double toZ = next.z - f.z;
			const double toGo = std::hypot(toX, toZ);
			const double desired = speed; // no corner brake (speedLimit): a chase's route is re-planned from the body every few steps, its first corner sits under it and braked bandits never closed (ksim_bw_orders); a sharp turn still costs speed through the slew
			const double memory = f.yaw == f.walkFacing ? f.walkHeading : f.yaw;
			knav::Velocity v = knav::slewVelocity({f.forwardSpeed * std::sin(memory), f.forwardSpeed * std::cos(memory)}, toGo > 1.0e-6 ? toX / toGo * desired : 0.0,
				toGo > 1.0e-6 ? toZ / toGo * desired : 0.0, gait.acceleration, dt);
			const bool footwork = f.state != CombatState::Approach && std::fabs(f.forwardSpeed) < kFaceByVelocity && f.chasePlanAge < 0.3;
			const double cap = ramp && footwork ? std::min(speed, std::fabs(f.forwardSpeed) + accel * dt) : speed; // the footwork ramp is a fight's start from a standstill (the first 0.3 s of a plan); an approach, and a body under way, run at the slew's own acceleration
			if (const double top = std::hypot(v.x, v.z); top > cap && top > 1.0e-9)
			{
				v.x *= cap / top;
				v.z *= cap / top;
			}
			const bool last = f.routeNext + 1 == f.route.size();
			if (last && std::hypot(v.x, v.z) * dt >= toGo)
				v = {toX / dt, toZ / dt}; // the step would pass the route's end: onto it
			const double vSpeed = std::hypot(v.x, v.z);
			f.yaw = knav::turnToward(f.yaw, vSpeed > kFaceByVelocity ? std::atan2(v.x, v.z) : std::atan2(toX, toZ), knav::kTurnRate, dt);
			f.walkHeading = f.yaw;
			f.walkFacing = f.yaw;
			f.forwardSpeed = vSpeed;
			// The position follows the route's points at the slewed speed (a step along the velocity leaves the route: the chase's next corner moves
			// every step and the bandits of ksim_bw_orders never closed); the slew gives the speed, the pivots and the facing.
			std::size_t at = f.routeNext;
			r = n->walker().follow({f.x, f.y, f.z}, f.route, at, vSpeed * dt, 0.0, f.routeDoors);
			if (r.onMesh)
			{
				f.x = r.at.x;
				f.y = r.at.y;
				f.z = r.at.z;
				f.routeNext = static_cast<std::uint32_t>(at);
			}
		} else {
			// Inside a building (stairs, doors: stored points) the chase follows its points at the run speed, not braked for corners.
			const state::RoutePoint& next = f.route[f.routeNext];
			const double want = std::atan2(next.x - f.x, next.z - f.z);
			f.yaw = wrapAngle(f.yaw + std::clamp(wrapAngle(want - f.yaw), -kPi * dt, kPi * dt));
			if (ramp)
				f.forwardSpeed = knav::rampSpeed(f.forwardSpeed, speed, dt, accel);
			else
				f.forwardSpeed = speed;
			std::size_t at = f.routeNext;
			r = n->walker().follow({f.x, f.y, f.z}, f.route, at, f.forwardSpeed * dt, 0.0, f.routeDoors);
			if (r.onMesh)
			{
				f.x = r.at.x;
				f.y = r.at.y;
				f.z = r.at.z;
				f.routeNext = static_cast<std::uint32_t>(at);
			}
		}
		if (!r.blocked)
		{
			f.routeTries = 0;
			return true;
		}
		if (f.routeTries < kMaxReroutes)
		{
			++f.routeTries;
			if (replan(f, tx, tz, true, true, ty, Fighter::kReplanBlocked))
			{
				++arena.reroutes_;
				f.chasePlanAge = 0.0;
				f.chaseTx = tx;
				f.chaseTz = tz;
				return true;
			}
		}
		++arena.routesBlocked_;
		f.clearRoute(Fighter::kClearWayShut);
		f.routeTries = 0;
		f.forwardSpeed = 0.0;
		f.chaseCooldown = kChaseCooldown;
		return true;
	}

	// ---- the follow task (SQUAD_FOLLOW.md s4 and s9) ----
	static void velocity(const Fighter& f, double& vx, double& vz)
	{
		vx = f.velX;
		vz = f.velZ;
	}

	// The top of the chain a follower follows (s4 step 3): a leader that is itself following a squadmate is
	// replaced by what it follows. Bounded, so a loop cannot hang.
	Member* chainTop(Member& leader)
	{
		Member* top = &leader;
		for (int depth = 0; depth < 8 && top->fighter.followTarget != 0; ++depth)
		{
			Member* next = member(top->fighter.followTarget);
			if (next == nullptr || next->fighter.dead || next->fighter.team != leader.fighter.team)
				break;
			top = next;
		}
		return top;
	}

	// The slot speed cap (s4 step 6) for a leader that is stopped, by the squared distance X to the slot.
	static double stoppedSpeed(double x)
	{
		const auto sat = [](double v) { return std::clamp(v, 0.0, 1.0); };
		if (x < 400.0)
			return 0.0;
		if (x < 2500.0)
			return 10.0 + 10.0 * sat((x - 400.0) / 2100.0);
		if (x < 40000.0)
			return 20.0 + 10.0 * sat((x - 2500.0) / 37500.0);
		return 30.0 + 969.0 * sat((x - 40000.0) / 210000.0);
	}

	void followStep(Member& m)
	{
		Fighter& f = m.fighter;
		f.lateralSpeed = 0.0;
		f.followAge += dt;
		Member* lm = member(f.followTarget);
		if (lm == nullptr || lm->fighter.dead)
		{
			endFollow(f);
			f.forwardSpeed = 0.0;
			return;
		}
		if (f.state != CombatState::Hub)
			setState(m, CombatState::Hub, true);
		const Fighter& lead = lm->fighter;
		// Give-up monitor: only while the leader is not perceived (s4 step 2). A captor's shackled slave keeps following without it
		// (task BR): the follow is its captivity, not a task it chose, and a sight gap (a wall between them, a scan phase) ended it
		// in the live chain's own timing (the BP capture with the fight ending at the KO). OURS/UNSURE.
		const bool shackled = f.slaveState == 1 && arena.captiveOf(f.captorFaction, lead.faction);
		if (shackled
			|| perception::seenWithin(f, f.followTarget, kPerceivedSlack * perception::scanInterval(arena.members_.size())))
		{
			f.followUnseen = 0.0;
			f.followSeenX = lead.x;
			f.followSeenZ = lead.z;
		} else {
			f.followUnseen += dt;
			if (f.followAge >= kFollowMonitorStart
				&& (f.followUnseen > kFollowGiveUpAge
					|| std::hypot(f.x - f.followSeenX, f.z - f.followSeenZ) < kFollowGiveUpRadius))
			{
				endFollow(f);
				f.forwardSpeed = 0.0;
				return;
			}
		}
		// Followers mirror the leader's sneak through the exe's masked read (1405c9750: the flag AND not in combat,
		// STEALTH_CLEAR.md section 3), so a squad whose leader is fighting drops stealth on the next follow step (task sneak-break).
		f.sneak = lead.sneak && !perception::inCombat(lead);

		// SPECIAL-CASE SHIM (kenshi-k16.6.673; user 2026-11-21, revisit with the medic/formation systems): a follower whose followed actor
		// is first-aiding it holds still until the aid is gone. The original does this with squad order 0x3e; ours reads the target's task
		// instead (Simulation sets heldForAid from the medic's InteractTask). Only a medic the patient is FOLLOWING ever triggers it, so it
		// cannot freeze anyone who chose not to follow (no exploit). Without it a conscious patient keeps formation and the medic loops.
		if (f.heldForAid)
		{
			f.forwardSpeed = 0.0;
			f.clearRoute();
			return;
		}

		// Task BP: a shackled slave of the leader's faction follows like a squadmate (the tight slot, not the 40 u standoff for a leader
		// outside the squad): a captive led along at 40 u loses sight of its captor in a town and the follow ends (OURS/UNSURE).
		const bool squadmate = lead.team == f.team || (f.slaveState == 1 && arena.captiveOf(f.captorFaction, lead.faction));
		Member* topMember = squadmate ? chainTop(*lm) : lm;
		const Fighter& top = topMember->fighter;
		double ax = 0.0;
		double az = 0.0;
		double leaderSpeed = 0.0;
		double ldx = 0.0;
		double ldz = 0.0;
		{
			double vx = 0.0;
			double vz = 0.0;
			velocity(top, vx, vz);
			leaderSpeed = std::hypot(vx, vz);
			if (leaderSpeed > 0.0)
			{
				ldx = vx / leaderSpeed;
				ldz = vz / leaderSpeed;
			}
		}
		if (!squadmate)
		{
			const double dx = f.x - top.x;
			const double dz = f.z - top.z;
			const double d = std::hypot(dx, dz);
			ax = top.x + (d > 1.0e-9 ? dx / d : 1.0) * kFollowStandoff;
			az = top.z + (d > 1.0e-9 ? dz / d : 0.0) * kFollowStandoff;
		} else {
			// The smoothed heading: nudged towards the leader's direction while it moves, renormalised.
			// A reversal is a near fixed point (the original's jank); the option snaps it instead.
			if (leaderSpeed > 0.0)
			{
				const double hl = std::hypot(f.followHx, f.followHz);
				if (arena.rules_.settings().squadFollowHeadingFix && hl > 0.0
					&& (f.followHx * ldx + f.followHz * ldz) / hl < kFollowFixCos)
				{
					f.followHx = ldx;
					f.followHz = ldz;
				} else {
					f.followHx += (ldx - f.followHx) * kFollowHeadingRate * dt;
					f.followHz += (ldz - f.followHz) * kFollowHeadingRate * dt;
				}
				const double norm = std::hypot(f.followHx, f.followHz);
				if (norm > 0.0)
				{
					f.followHx /= norm;
					f.followHz /= norm;
				}
			}
			// No heading yet (a stopped leader at a fresh task): local -X is forward.
			const bool fresh = f.followHx == 0.0 && f.followHz == 0.0;
			const double hx = fresh ? -1.0 : f.followHx;
			const double hz = fresh ? 0.0 : f.followHz;
			double ox = 0.0;
			double oz = 0.0;
			arena.formationOffset(m.id, topMember->id, ox, oz);
			// Local +x is behind the heading, +z to its right (the sim's yaw: right of (0,1) is (1,0)).
			ax = top.x + hx * leaderSpeed * kFollowSlotLead - hx * ox + hz * oz;
			az = top.z + hz * leaderSpeed * kFollowSlotLead - hz * ox - hx * oz;
		}

		clearSlot(f, ax, az);
		const double dx = ax - f.x;
		const double dz = az - f.z;
		const double x2 = dx * dx + dz * dz;
		const double d = std::sqrt(x2);
		double want = 0.0; // the cap of s4 step 6
		if (x2 >= 1.0)
		{
			if (leaderSpeed <= 0.0)
			{
				want = stoppedSpeed(x2);
			} else {
				double mx = 0.0;
				double mz = 0.0;
				velocity(f, mx, mz);
				const double mine = std::hypot(mx, mz);
				const double dot = mine > 0.0 ? ldx * mx / mine + ldz * mz / mine : 0.0;
				want = d * std::max(kFollowDirectionFloor, 0.5 + 0.5 * dot);
			}
		}
		const double speed = std::min(runSpeed(f), want);
		if (x2 < 2500.0)
		{
			if (!f.followDirect)
				f.clearRoute(); // the path mode's route ends here: a route in direct mode is one the steer began (below)
			f.followDirect = true;
		} else if (f.followDirect && x2 >= 10000.0)
			f.followDirect = false;
		if (speed <= 0.0)
		{
			f.forwardSpeed = 0.0;
			if (nav() != nullptr)
			{
				// A halted follower is eased back from a mesh edge it stands within kSlotStandOff of (a prop's ring edge, 0.75 u off the prop).
				double px = 0.0;
				double pz = 0.0;
				standOffPush(nav()->walker(), f.y, f.x, f.z, px, pz);
				const double len = std::hypot(px, pz);
				if (len > 1.0e-6)
					shift(f, px / len * std::min(len, kFollowStandOffRate * dt), pz / len * std::min(len, kFollowStandOffRate * dt));
			}
			if (f.routeChase)
				f.clearRoute(Fighter::kClearWentDirect);
			f.swingProgress = computeSwingProgress(f);
			return;
		}
		// Direct steering has no clearance (the walker slides along a wall at 0 u: a guard along the Heft Longhouse's cage, kenshi-k16.6.253), so
		// it is only for a line a body of the route's radius fits; a line open at its centre but not that wide takes the clearance-keeping route to the
		// slot. A shut centre line keeps the direct steer (its slide closes on a slot behind a wall's corner, where the route's stale end stopped the follower 8 u short).
		// A route planned for this slot is walked to its end before the steer takes over again: the steer's wall slide is undone into a route, and a route dropped at
		// once for the next tick's open line was re-planned every other tick (43 switches in 600 ticks, ksim_follow "corner follower").
		const bool onRoute = f.routeChase && f.routeNext < f.route.size() && std::hypot(ax - f.chaseTx, az - f.chaseTz) <= kFollowReplan; // planned for this slot
		if (nav() != nullptr && (!f.followDirect || onRoute || (!clearWay(f, f.x, f.z, ax, az) && clearWay(f, f.x, f.z, ax, az, false))))
		{
			const double qx = f.x;
			const double qz = f.z;
			chaseStep(f, ax, az, std::nullopt, speed, false, false, kFootworkAcceleration, kFollowReplan);
			if (f.followDirect)
				faceRouteStep(f, f.x - qx, f.z - qz);
			f.swingProgress = computeSwingProgress(f);
			return;
		}
		if (f.routeChase)
			f.clearRoute(Fighter::kClearWentDirect); // direct again: the wire sees the clear
		// Direct steering at the slot (mode 2, rwh110): move straight at the slot whatever the facing, the step clamped to
		// sqrt(distance); facing := the velocity while it is above 0.1 u/s (no turn rate), frozen on a "stuck" frame
		// (moved < 20 % of dt x the previous speed); the facing is never aimed at the slot.
		const double px = f.x;
		const double pz = f.z;
		const double step = std::min(speed * dt, std::sqrt(d));
		const double fy = f.y;
		shift(f, dx / d * step, dz / d * step);
		if (nav() != nullptr && hugsWall(f, dx / d, dz / d))
		{
			// The slide along a wall or prop at 0 u (the shut centre line's steer): undone, the clearance route instead.
			f.x = px;
			f.y = fy;
			f.z = pz;
			chaseStep(f, ax, az, std::nullopt, speed, false, false, kFootworkAcceleration, kFollowReplan);
			faceRouteStep(f, f.x - px, f.z - pz);
			f.swingProgress = computeSwingProgress(f);
			return;
		}
		const double mx = f.x - px;
		const double mz = f.z - pz;
		const double prevStep = 0.2 * dt * f.forwardSpeed;
		if (prevStep * prevStep < mx * mx + mz * mz)
		{
			f.forwardSpeed = std::hypot(mx, mz) / dt;
			if (f.forwardSpeed > 0.1)
				f.yaw = std::atan2(mx, mz);
		} else {
			f.forwardSpeed = 0.0;
		}
		// Stuck between 50 and 100 units (s4 step 7, the +0xac test): path-find instead. UNSURE: the
		// original's literal test (distance from the last position <= speed) is read as "barely moved".
		if (f.followDirect && x2 >= 2500.0 && std::hypot(mx, mz) < 0.25 * speed * dt)
			f.followDirect = false;
		f.swingProgress = computeSwingProgress(f);
	}

	// Direct drive (SIMLINK.md section 12): the sticky state of the last drive order, stepped by
	// the shared Walker. Forward faces the heading (turning at 20 rad/s, the original's rate); backward
	// is a backpedal and keeps the facing; a strafe (drive 0 / 1 / 2 plus a side step) faces the heading too.
	void driveStep(Member& m, bool crawl = false)
	{
		Fighter& f = m.fighter;
		const MeshNavigator* n = nav();
		if (n == nullptr)
		{
			f.drive = state::DriveMode::Stop;
			f.driveStrafe = 0;
			return;
		}
		if (f.state != CombatState::Hub && !crawl)
			setState(m, CombatState::Hub, true); // a crawler keeps its Stumble pose (task BF)
		f.lateralSpeed = 0.0;
		const bool back = f.drive == state::DriveMode::Backward;
		if (!back || f.driveStrafe != 0) // a strafe holds the facing at the heading, whatever the drive
		{
			f.yaw = knav::turnToward(f.yaw, f.driveHeading, knav::kTurnRate, dt);
		}
		const double top = crawl ? std::min(gaitSpeed(f), CombatArena::kCrawlSpeed) : gaitSpeed(f);
		const knav::StepResult r = n->walker().drive({f.x, f.y, f.z},
			static_cast<knav::Drive>(f.drive), f.driveHeading, top, f.forwardSpeed, dt,
			gaitOf(f).acceleration, f.driveStrafe, bodyRoom(f));
		if (r.onMesh)
		{
			f.x = r.at.x;
			f.y = r.at.y;
			f.z = r.at.z;
		}
		if (r.blocked && !f.driveBlocked)
			emit(CombatEvent::Kind::DriveBlocked, m.id, 0);
		f.driveBlocked = r.blocked;
		f.swingProgress = computeSwingProgress(f);
	}

	// Turn in place (SIMLINK.md section 12): a drive 0 in the drive hold turns the body towards the heading at the
	// walking rate and moves nothing. Done when it faces it. A drive 0 at the current heading is a no-op.
	void driveTurnStep(Member& m)
	{
		Fighter& f = m.fighter;
		f.forwardSpeed = 0.0;
		f.lateralSpeed = 0.0;
		f.yaw = knav::turnToward(f.yaw, f.driveHeading, knav::kTurnRate, dt);
		if (std::fabs(wrapAngle(f.driveHeading - f.yaw)) < 1.0e-9)
			f.driveTurn = false;
		f.swingProgress = computeSwingProgress(f);
	}

	// A fighter off the navmesh is a bug state: bring it back and say so (SIMLINK.md section 12). Deterministic: runs first in every step, in Id order.
	// Back to its own level (ENGAGEMENT.md 15, kenshi-h2v.30): the nearest mesh point, unless that is on another level (more than kCorrectionLevel up or
	// down) and there is one on the fighter's own.
	void correct(Member& m)
	{
		const MeshNavigator* n = nav();
		Fighter& f = m.fighter;
		// A caged character stands in its cage, a hole in the mesh: the snap would put it back on the walkable edge beside the bars (k16.6.370).
		if (n == nullptr || f.dead || f.cageBuilding != 0)
			return;
		const knav::Position at{f.x, f.y, f.z};
		if (n->walker().inside(at))
			return;
		double distance = -1.0;
		std::optional<knav::Position> snapped = n->walker().snap(at, kCorrectionReach, &distance);
		if (snapped && std::fabs(snapped->y - f.y) > kCorrectionLevel)
		{
			// The nearest point is on another level: the one on the fighter's own floor, if there is one in reach (the window finds polygons that reach into it,
			// so a sloped one can still be off by more than the window).
			double ownDistance = -1.0;
			const std::optional<knav::Position> own = n->walker().snap(at, kCorrectionReach, &ownDistance, kCorrectionLevel);
			if (own && std::fabs(own->y - f.y) <= kCorrectionLevel && ownDistance <= kCorrectionOwnReach)
			{
				snapped = own;
				distance = ownDistance;
			}
		}
		if (snapped)
		{
			f.x = snapped->x;
			f.y = snapped->y;
			f.z = snapped->z;
			m.dirty = true;
			++arena.corrections_;
		} else {
			++arena.uncorrectable_;
		}
		emit(CombatEvent::Kind::PositionCorrected, m.id, 0, 0, 0, -1, distance);
	}

	// ---- one fighter ----
	void update(Member& m)
	{
		Fighter& f = m.fighter;
#if KSIM_PROFILE
		branch = BrOther;
#endif
		if (f.dead)
		{
#if KSIM_PROFILE
			branch = BrDead;
#endif
			return;
		}
		steer = {};
		clipAdvanced = false;
		f.aggroBlock = std::max(f.aggroBlock - dt, 0.0);
		if (f.aggroBlock <= 0.0 && f.aggroBlockId != 0)
		{
			// Task BN (dt4, kinspect 7726815): at expiry the exe re-queries the navmesh at the blocked target's position;
			// reachable (or gone) erases the entry, unreachable re-arms the 20 s timer. So a blocked target is re-tested
			// every kBlockedTarget and never latched, and the attacker is not dragged back to it every 20 s in between.
			const Member* blocked = member(f.aggroBlockId);
			bool reachable = blocked != nullptr;
			const MeshNavigator* n = nav();
			if (reachable && n != nullptr)
			{
#if KSIM_PROFILE
				const knav::measure::WhoScope whoScope(f.diceSeed, 3);
#endif
				const Route route = routeForBody(*n, {f.x, f.y, f.z}, blocked->fighter.x, blocked->fighter.z,
					blocked->fighter.y, bodyOf(f));
				reachable = route.found;
			}
			if (reachable)
				f.aggroBlockId = 0;
			else
				f.aggroBlock = kBlockedTarget;
		}
		// Task BG/BG2: a ragdoll exists only while the body is KO'd or dead. A crawler (down, conscious) is not one, so its
		// no-blow death is a cause-1 ragdoll start, while a body that was already KO'd was drawn ragdolled already.
		const bool wasRagdoll = f.unconscious;
		std::uint32_t severedBefore = 0; // parts are <= 16 on the wire (CHAR_MEDICAL severed bits)
		for (std::size_t i = 0; i < f.parts.size() && i < 32; ++i)
			severedBefore |= f.parts[i].severed() ? 1u << i : 0u;
		// Hunger and a medically quiet member's medical run once a second on the member's own slice, with the time since the stamp (so a
		// skipped step owes nothing); any other member runs the medical update at every step it takes (k16.6.607 item 6).
		const double stepSeconds = arena.stepSeconds_;
		// A clock jump (setClockTick, a debug time skip) moves the phase clock without time passing in the fight: capped at kMaxOwedSeconds.
		constexpr double kMaxOwedSeconds = 10.0;
		const double hungerOwed = std::min(kMaxOwedSeconds, static_cast<double>(arena.phaseStep_ - f.hungerStep) * stepSeconds);
		const bool quiet = rules.medicallyQuiet(f);
		// A member that stopped being quiet in this step (a KO, a wound) owes only this step: the quiet time before it must not run down a
		// timer set a moment ago (the forced KO).
		const double medicalOwed = std::min(quiet ? kMaxOwedSeconds : m.stepDt, static_cast<double>(arena.phaseStep_ - f.medStep) * stepSeconds);
		// Due on the first step after the member's own second boundary (the phase is the Id's), measured from the stamp, whatever the stride.
		const std::uint64_t slice = arena.sliceSteps_;
		const std::uint64_t phase = arena.diceKey(m.id); // the stable dice identity, not the Id: an unrelated spawn must not move anyone's slice
		const std::uint64_t nowSlice = (arena.phaseStep_ + phase) / slice;
		const bool hungerDue = nowSlice > (f.hungerStep + phase) / slice;
		bool died = false;
		if (hungerDue || nowSlice > (f.medStep + phase) / slice || !quiet)
		{
			died = rules.updateMedical(f, medicalOwed, hungerDue ? hungerOwed : 0.0);
			f.medStep = arena.phaseStep_;
			if (hungerDue)
				f.hungerStep = arena.phaseStep_;
		}
		for (std::size_t i = 0; i < f.parts.size() && i < 32; ++i) // the Dismemberment 2 sink path of updateMedical
		{
			if (f.parts[i].severed() && (severedBefore & (1u << i)) == 0)
				emit(CombatEvent::Kind::LimbSevered, m.id, 0, 0, 0, -1, 0.0, 0.0, f.parts[i].name);
		}
		// A KO with no hit (vital sink, blood, hunger, a re-KO after waking): the same Knockout event, attacker 0. A hit's KO set
		// `unconscious` already (medicalHit emitted it), so wasRagdoll keeps this to one event per transition. A crawler that
		// goes unconscious sends this like anyone: cause 0, whether the blow or the medical step did it.
		if (!wasRagdoll && f.unconscious && !died)
		{
			emit(CombatEvent::Kind::Knockout, m.id, 0);
			// EVENT 48 cause 0: a collapse, no blow behind it (direction 0,0,0 and strength 0 on the wire).
			emitRagdoll(f, m.id, 0, 0, std::string(), 0.0);
		}
		if (died)
		{
#if KSIM_PROFILE
			branch = BrDead;
#endif
			emit(CombatEvent::Kind::Death, m.id, 0);
			// EVENT 48 cause 1, unless the body was already ragdolled (BG2: KO'd or dead, which the death makes of itself): the
			// client is already showing a ragdoll and a death changes nothing on screen. A crawler's no-blow death is a new
			// ragdoll, so it sends cause 1.
			if (!wasRagdoll)
				emitRagdoll(f, m.id, 1, 0, std::string(), 0.0);
			// The victim's last attacker, or 0 for a death with no blow behind it (hunger, a script).
			arena.deaths_.push_back({m.id, m.lastAttacker});
			stopTechnique(f);
			setTarget(m, 0);
			clearAllSlots(f);
			f.forwardSpeed = 0.0;
			f.clearRoute(Fighter::kClearIncapacitated);
			f.drive = state::DriveMode::Stop;
			f.driveStrafe = 0;
			return;
		}
		tickSlots(m);
		trackPending(m);
		f.actionTimer = std::max(f.actionTimer - dt, -1.0);
		f.stateTime += dt;
		f.targetAge += dt;
		if (f.target != 0)
		{
			learn(m, f.target, false);
			// The per-frame refresh of the victim's record is for an attacker its scans sense: one behind a wall (teleported out of sight, live) is dropped
			// within seconds, memory expiry included (rwh.191 control).
			if (Member* t = member(f.target); t != nullptr && perception::sensedWithin(t->fighter, m.id, perception::kRecordFresh))
				learn(*t, m.id);
		}
		if (f.orderStop)
		{
			f.orderStop = false;
			setTarget(m, 0);
		}
		if (f.unconscious || f.down)
		{
#if KSIM_PROFILE
			branch = BrDown;
#endif
			// Incapacitated: no actions, no fighting. The crawl pose is state Stumble with no clip. The technique goes whatever the state
			// was: a block's pointer outlives its clip through the stagger (resolveIncoming), so a KO that lands in state Stumble kept
			// it, and a woken player read "in combat, Attacking target" with no target for good (the client's slaver re-test).
			stopTechnique(f);
			if (f.unconscious)
			{
				// A KO clears the orders, task and goal before the ragdoll (rwh.195: the mask apply runs AITaskSytem::clearOrders): the attack
				// order and the move / follow / drive orders live in the cleared job_queue, so nothing re-commits them on waking.
				f.orderTarget = 0;
				if (f.routeSource == Fighter::PlanSource::Order) // a player's walk; an AI plan's route is not in the job_queue
					f.clearRoute(Fighter::kClearIncapacitated);
				if (f.followSource == Fighter::PlanSource::Order)
					endFollow(f);
				// State 8 is animation-only (Task_Stumble): it ends with its clip, KO or not, and never outlasts it.
				f.recovering = false;
				f.reactClip = -1;
				if (f.state == CombatState::Stumble)
				{
					f.stumbleTimer -= dt;
					if (f.stumbleTimer <= 0.0)
						setState(m, CombatState::Hub, true);
				} else {
					setState(m, CombatState::Hub, true);
				}
			} else if (f.state != CombatState::Stumble) {
				f.recovering = false;
				setState(m, CombatState::Stumble);
				f.stumbleTimer = kStumbleTime;
				f.reactClip = -1;
			}
			f.swingProgress = -1.0;
			if (f.unconscious)
			{
				// A KO'd body lies where it fell: it takes no move order. A player's attack order ends with the KO, as it does when its
				// target goes down (task BK: "does not re-engage when the body wakes"); kept, the woken body resumed the attack, held a
				// target again and read as a troublemaker to goal 34 (ksim_bar_brawl seed 1, k16.6.384).
				f.orderTarget = 0;
				if (f.target != 0)
					setTarget(m, 0);
				f.forwardSpeed = 0.0;
				return;
			}
			// A conscious down crawler takes a move order or a drive (task BF; the original crawls, user play): it moves
			// at kCrawlSpeed with `down` still set, and nothing else runs.
			if (f.driving())
			{
				driveStep(m, true);
				return;
			}
			if (f.walking())
			{
				walkOrderSynced(m, true);
				return;
			}
			// A player's attack order is taken too (kinspect bf_crawl_down.md 9a539ac: the down player crawled ~200 u to
			// the target, entered combat and landed no blow in 10 s). The order lives in orderTarget until the combat hub
			// would pick it up, which a down body never reaches: it crawls to the engage range and lies there instead.
			// An NPC with a fight target does not move by itself (no self-crawl was seen).
			if (f.playerControlled && f.orderTarget != 0)
			{
				const Member* t = member(f.orderTarget);
				if (t != nullptr && !t->fighter.dead && !t->fighter.unconscious && !inEngageRange(m, *t))
				{
					chaseStep(f, t->fighter.x, t->fighter.z, std::nullopt, CombatArena::kCrawlSpeed, true, true);
					return;
				}
				if (t == nullptr || t->fighter.dead || t->fighter.unconscious)
					f.orderTarget = 0;
			}
			f.forwardSpeed = 0.0;
			return;
		}
		// A player character's move order stands over its own combat state machine (the SELF_PRESERVATION row: goal 32 over an
		// explicit move is not measured and the Bar walk-in keeps its order; OURS). A technique the character started is dropped
		// and the recovery hold ends, so a clip never suspends the walk; with `pickTarget`'s player-order guard the state machine
		// also never sees a non-walk tick to plan a chase route over the order. A stagger from a hit is a reaction, not a
		// technique: it still plays, and the walk resumes when it ends.
		const bool playerOrder = f.playerControlled && !f.driving() && f.walking() && f.carrying == 0
			&& !f.sneak && f.state != CombatState::Stumble;
		if (playerOrder)
		{
			if (clipPlaying(f))
				stopTechnique(f);
			f.recovering = false;
		}
		// Advance the running clip.
		if (clipPlaying(f))
		{
			const Technique& t = data.techniques[static_cast<std::size_t>(f.technique)];
			const double length = clipLength(t);
			clipT0 = f.clipTime;
			f.clipTime += dt * f.clipSpeed;
			if (f.clipTime >= length)
			{
				f.clipTime = length;
				f.techniqueFinished = true;
			}
			clipT1 = f.clipTime;
			clipAdvanced = true;
		}
		// Recovery hold: forced state 6 while the recovery flag is set.
		// No defence poll inside the hold (CADENCE_LIVE.md Items 40 / 42: 0 picks in 24 + 36 fights; the next block comes from the hub run
		// the hold ends in). The older reading (192 of ~400 picks with +0x144 set) was the hub's own 0.5 s timer, state 3.
		if (f.recovering && f.state != CombatState::Done)
		{
			if (!m.fighter.holdSteer.inHold)
			{
				// Hold entry (kinspect 48 (c)): from its own swing the steer is on only with nothing pending and the swing's velocity is
				// dropped; from a block (state 1) the steer, and the speed it had, carry on even with pending set.
				m.fighter.holdSteer.inHold = true;
				m.fighter.holdSteer.steerOn = m.fighter.holdSteer.came == CombatState::Defend || !m.fighter.holdSteer.pending;
				if (m.fighter.holdSteer.came == CombatState::Attack)
					f.forwardSpeed = 0.0;
				m.fighter.holdSteer.came = CombatState::Hub;
			}
			if (f.state != CombatState::Stumble && f.state != CombatState::Attack
				&& f.state != CombatState::Defend)
				setState(m, CombatState::Wait);
			if (!clipPlaying(f))
			{
				f.recoverTimer -= dt;
				if (f.recoverTimer <= 0.0)
				{
					f.recovering = false;
					if (f.state != CombatState::Stumble)
						setState(m, CombatState::Hub);
				}
			}
			if (f.recovering)
			{
				steer = waitingRing(m);
				f.swingProgress = computeSwingProgress(f);
				integrate(m);
				return;
			}
		}
		// (b38291f's walker aggros here; task fight-goals moved it to the goal selector: the package's fight goals preempt a squad plan in
		// stepWorldTasks' per-character pass, kinspect patrol_aggro.md c332e51 section 3. Combat no longer decides whether a walker fights.)
		// A player character that is FOLLOWING answers an attacker on itself once goal 32 fires (within the self-defence
		// range or after a hit) and the follow task is dropped, as an NPC's is; UNSURE whether the original resumes the
		// order afterwards (goal 32 presumably outranks the follow, not measured). A move order or a drive stands: goal 32
		// over a player's explicit move is not measured, and the Bar walk-in keeps its order (ksim_bar_fight 3b).
		// PROTECT_ALLIES for another ally still never interrupts a player's task.
		if (f.playerControlled && !f.sneak && f.carrying == 0 && f.followTarget != 0
			&& !clipPlaying(f) && f.state != CombatState::Stumble && selfDefence(m) != nullptr)
		{
			f.clearRoute();
			endFollow(f);
		}
		// A stealth KO (task ds/stealth-ko S4, dt41_C): the task owns the body until it ends.
		if (f.stealthKo.active())
		{
			stealthKoStep(m);
			if (f.stealthKo.active())
				return;
		}
		if (f.driveTurn && (!f.hold || f.driving()))
			f.driveTurn = false; // the hold ended, or a drive took over
		if (f.driveTurn && !clipPlaying(f) && f.state != CombatState::Stumble)
		{
#if KSIM_PROFILE
			branch = BrDrive;
#endif
			driveTurnStep(m);
			return;
		}
		// Direct drive, then a player's move order (no avoidance, no fighting on the way).
		if (f.driving() && !clipPlaying(f) && f.state != CombatState::Stumble)
		{
#if KSIM_PROFILE
			branch = BrDrive;
#endif
			driveStep(m);
			return;
		}
		// Squad order 0x3e (hold still, kenshi-k16.6.674): a same-squad patient under a first aid interact does not walk off (the fight
		// response below it is untouched). Posted by tasks 25 / 249 / 60 (InteractAction::FirstAid), gone when the interact stops.
		if (f.heldForAid && f.walking() && !clipPlaying(f) && f.state != CombatState::Stumble)
		{
			f.forwardSpeed = 0.0;
			f.clearRoute();
			return;
		}
		if (f.walking() && !clipPlaying(f) && f.state != CombatState::Stumble)
		{
#if KSIM_PROFILE
			branch = BrWalk;
#endif
			walkOrderSynced(m);
			return;
		}
		if (f.followTarget != 0 && !clipPlaying(f) && f.state != CombatState::Stumble)
		{
#if KSIM_PROFILE
			branch = BrFollow;
#endif
			followStep(m);
			return;
		}
		// A gunner at a turret (task ds/turret S3, dt41_B): it holds the post and runs the turret's shot state machine on the target it picks; a
		// target out of range, out of the arc or behind a wall is simply not shot. Knocked down or carried off the post it lets go of the turret.
		if (f.mount.mounted())
		{
			if (f.down || f.unconscious || f.carriedBy != 0 || std::hypot(f.x - f.mount.x, f.z - f.mount.z) > kPostSlack)
				f.mount = {};
			else
				return mannedStep(m);
		}
		// A crossbow hand (task ds/ranged S2): the attack is the ranged task. The fighter stands and shoots while the target is in reach and the
		// line is clear; out of reach, or with a wall or an ally in the line, it falls through to the usual approach (a chase round the wall).
		f.rangedTask = 0;
		if (const std::uint16_t task = rangedTask(m);
			task != 0 && !clipPlaying(f) && (f.state == CombatState::Hub || f.state == CombatState::Approach || f.state == CombatState::Circle
				|| f.state == CombatState::Wait || f.state == CombatState::Between))
		{
			if (const Member* t = pickTarget(m); t != nullptr && CombatArena::inRangedReach(f, t->fighter))
			{
				const std::uint64_t id = t->id;
				const CombatArena::Shot shot = arena.rangedFire(m.id, id, dt, task != goap::kRangedAttackFocusedUnprovoked);
				if (shot != CombatArena::Shot::Blocked && shot != CombatArena::Shot::OutOfRange)
				{
					f.rangedTask = task;
					f.clearRoute();
					f.forwardSpeed = 0.0;
					f.lateralSpeed = 0.0;
					if (const Member* aimed = member(id))
						f.yaw = std::atan2(aimed->fighter.x - f.x, aimed->fighter.z - f.z);
					return;
				}
			}
		}
		// A swing aimed at me pulls a non-attacking fighter into defence. Not from the hub: there tryEngageTarget asks
		// `decideEngagement` first, whose pre-roll answer for a target mid-swing at me is "defend" (DUEL_CADENCE_LIVE.md (a): 106 of the
		// servant's 284 logged decisions, 37 %; skipping the decision here cost the servant half of its decisions).
		if (f.state == CombatState::Circle || f.state == CombatState::Wait
			|| (f.state == CombatState::Between && f.pending != CombatState::Defend))
		{
			if (!incoming(m).empty())
			{
				f.pending = CombatState::Defend;
				setState(m, CombatState::Between);
			}
		}
		if (f.state != CombatState::Wait)
			m.waitFar = false;
		if (f.state != CombatState::Approach && !(f.state == CombatState::Wait && m.waitFar))
			f.chaseId = 0; // the monitor belongs to the move-in task (and a far waiter): each spell of approach is a new Task_Move
#if KSIM_PROFILE
		branch = f.state == CombatState::Hub || f.state == CombatState::Done ? BrIdle : f.state == CombatState::Stumble ? BrOther : BrFight;
#endif
		switch (f.state)
		{
		case CombatState::Hub:
			hub(m);
			if (f.state == CombatState::Between)
				between(m);
			break;
		case CombatState::Between:
			between(m);
			break;
		case CombatState::Attack:
			attackState(m);
			break;
		case CombatState::Defend:
			defendState(m);
			break;
		case CombatState::Circle:
		case CombatState::Wait:
			circle(m);
			break;
		case CombatState::Approach:
			approach(m);
			break;
		case CombatState::Stumble:
			stumble(m);
			if (f.state == CombatState::Hub)
			{
				hub(m); // the clip ended: the defence pick comes in this very frame
				if (f.state == CombatState::Between)
					between(m);
			}
			break;
		case CombatState::Done:
			setState(m, CombatState::Hub);
			break;
		}
		// The chase's stuck timer (MINE/UNSURE: the exe's behaviour for a target it keeps moving at but cannot close on -
		// behind a wall, round a corner - is not decoded; the timer is ours, kStuckRepath as the walk's). A fighter that
		// spends kStuckRepath on a target beyond its approach ring without a route under it switches the chase to the
		// routed mode (integrate's chaseDirect). A walk order's own accumulation (walkOrder) is left alone.
		const Member* chaseTarget = f.target != 0 ? member(f.target) : nullptr;
		const double beforeDist = chaseTarget != nullptr ? centre(f, chaseTarget->fighter) : 0.0;
		f.swingProgress = computeSwingProgress(f);
		integrate(m);
		m.lineBlocked = false;
		const bool routing = f.routeChase && f.routeNext < f.route.size();
		if (chaseTarget != nullptr && (f.route.empty() || f.routeChase))
		{
			// A chaser that is moving after a freely running target it leads (rwh.108) is not stuck, whatever the gap: the original has no such timer.
			const bool leadingOn = m.leadHold >= kLeadHold && std::hypot(f.velX, f.velZ) > kLeadMinSpeed;
			if (!f.unconscious && !f.down && beforeDist > ringOuter() + 2.0 && !routing && !leadingOn)
				f.walkStuck += dt;
			else
				f.walkStuck = 0.0;
		}
	}

	// Group mode (SIMLINK order 10 which 5): every member is capped at the slowest live, conscious member's own
	// (uncapped) run speed; the dead, the unconscious and the carried stop counting.
	double groupCap(const Fighter& f) const
	{
		double cap = 0.0;
		for (const Member& m : arena.members_)
		{
			if (m.fighter.speedGroup != f.speedGroup || !usable(m.fighter))
				continue;
			knav::Gait gait = ksim::gaitOf(data, m.fighter);
			gait.run = true;
			gait.cap = 0.0;
			const double speed = knav::runSpeed(gait);
			cap = cap == 0.0 ? speed : std::min(cap, speed);
		}
		return cap;
	}
	knav::Gait gaitOf(const Fighter& f) const
	{
		knav::Gait gait = ksim::gaitOf(data, f);
		if (f.moveMode == 3 && f.speedGroup != 0)
			gait.cap = groupCap(f);
		return gait;
	}
	double gaitSpeed(const Fighter& f) const { return knav::gaitSpeed(gaitOf(f)); }
	// A chase or a follow task always runs: the speed modes cap move orders and drives only (SIMLINK.md order 10
	// `which` 5). The gait's run flag and cap are the mode's, overridden here.
	double runSpeed(const Fighter& f) const
	{
		knav::Gait gait = gaitOf(f);
		gait.run = true;
		gait.cap = 0.0;
		return knav::runSpeed(gait);
	}

	// The recovery hold's outward push (kinspect 48 (a)): forwardSpeed is the signed speed along the heading, outward negative.
	// Sub-steps of one exe frame; the drive target is kHoldPushSpeed until the centre reaches `cut`, then 0; steer off brakes linearly.
	void holdPush(Fighter& f, bool on, double ux, double uz, double dist, double cut)
	{
		double left = dt;
		double out = std::max(-f.forwardSpeed, 0.0);
		double moved = 0.0;
		while (left > 1.0e-9)
		{
			const double h = std::min(left, kExeFrame);
			left -= h;
			if (on)
			{
				const double target = dist + moved < cut ? kHoldPushSpeed : 0.0;
				out += (1.0 - std::pow(1.0 - kHoldPushLag, h / kExeFrame)) * (target - out);
				if (target == 0.0 && out < 0.3)
					out = 0.0;
			} else {
				out = std::max(out - kHoldBrake * h, 0.0);
			}
			moved += out * h;
		}
		f.forwardSpeed = -out;
		f.lateralSpeed = 0.0;
		shift(f, -ux * moved, -uz * moved);
	}

	// Position and yaw: root motion while a clip plays (the strafe lock), steering otherwise.
	void integrate(Member& m)
	{
		Fighter& f = m.fighter;
		if (f.state == CombatState::Stumble && f.reactClip >= 0 && f.reactTime < (f.reactClip == data.stumbleHeavyClip ? kStumbleHeavyTime : kStumbleTime))
		{
			const Clip& clip = data.clips[static_cast<std::size_t>(f.reactClip)];
			const double push = clip.at(f.reactTime).z - clip.at(f.reactTime + dt).z; // the clip's local -z is away from the attacker
			f.reactTime += dt;
			if (!underMoveOrder(f))
				shift(f, f.recoilX * push, f.recoilZ * push); // a runner is not pushed back (no knockback in the original)
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			return;
		}
		if (clipAdvanced && f.technique >= 0)
		{
			const Technique& t = data.techniques[static_cast<std::size_t>(f.technique)];
			// Task BQ: an animal technique has no clip: no root motion and no animated turn, so the beast holds its place and its
			// facing through the swing (rootLocked), and the impact box is the slab along that facing. Letting the footwork run
			// would turn the body onto a strafing target and the box would follow it (a dodge would never work).
			if (t.clip < 0 && t.length > 0.0)
			{
				f.forwardSpeed = 0.0;
				f.lateralSpeed = 0.0;
				return;
			}
			// Facing turns the body towards its opponent at the combat-ready rate (AnimationClassHuman::vf8, s70 section 3; the look
			// hand is re-aimed: updateSwing at index 0, faceTarget 140666400 for a block unless the last impact dir is 7/8/9). The
			// swing's box and root motion follow the AIM (Fighter::aimYaw), set at the technique start; the body-facing tests
			// (perception, a block's behind test) follow f.yaw: this turn is the body catching up with the aim.
			// UNSURE: whether a swing waits for the facing (no source says so; swings start while still turning).
			if (t.clip >= 0)
			{
				const Member* face = f.state == CombatState::Defend ? strongestThreat(incoming(m), -10.0) : nullptr;
				if (face == nullptr)
					face = member(f.target);
				if (face != nullptr)
					f.yaw = knav::turnToward(f.yaw, std::atan2(face->fighter.x - f.x, face->fighter.z - f.z), knav::kTurnRateCombat, dt);
			}
			if (t.clip >= 0 && !(t.isBlock && !t.isDodge && steer.active)) // a block clip has no root motion: the state-1 steer moves it
			{
				const Clip& clip = data.clips[static_cast<std::size_t>(t.clip)];
				const Clip::Sample a = clip.at(clipT0);
				const Clip::Sample b = clip.at(clipT1);
				const double dz = b.z - a.z;
				const double dx = b.x - a.x;
				shift(f, std::sin(f.aimYaw) * dz + std::cos(f.aimYaw) * dx, std::cos(f.aimYaw) * dz - std::sin(f.aimYaw) * dx);
				// The clip's root yaw is NOT applied: the character's facing stays on the target through the swing
				// (heavy swing: logged yaw within 5 degrees of the bearing for the whole 2.4 s; applying the
				// clip's 1.35 rad turned the club away and every swing missed). Retired: `f.yaw += b.yaw - a.yaw`.
				f.forwardSpeed = dt > 0.0 ? dz / dt : 0.0;
				f.lateralSpeed = dt > 0.0 ? dx / dt : 0.0;
				return;
			}
		}
		Member* target = f.target != 0 ? member(f.target) : nullptr;
		if (!steer.active || f.hold || target == nullptr || !targetable(target->fighter))
		{
			// The chase route survives a clip (a swing, a stumble): the footwork resumes when it ends, from where the
			// clip left the fighter. It goes when the chase itself ends - the target is gone or has gone down (task BK:
			// the attacker stops where it is, no circling), or a hold stops it.
			const bool chaseOver = f.hold || target == nullptr || !targetable(target->fighter);
			if (f.routeChase && f.followTarget == 0 && chaseOver)
				f.clearRoute(Fighter::kClearWentDirect);
			// The hub is a frame between two steers, not a stop: a chaser that is running down a freely running target (rwh.108) carries its speed
			// through it, instead of standing dead at the band and re-ramping while the target leaves (k16.6.153).
			if (!chaseOver && f.state == CombatState::Hub && m.leadHold >= kLeadHold)
				return;
			f.forwardSpeed = 0.0;
			f.lateralSpeed = 0.0;
			return;
		}
		double dx = target->fighter.x - f.x;
		double dz = target->fighter.z - f.z;
		double dist = std::hypot(dx, dz);
		if (ringRouted != 0)
		{
			const bool mine = ringRouted == m.id; // a stale flag (this tick's integrate returned early) never stops another fighter's steer
			// circle() already walked this tick's step of a route to a target inside the ring's reach that the walk does not reach
			// (kscen_radius: the direct steer below saw dist <= outer, set the speed to 0 and cleared the route every tick, so the
			// pair crept at 0.4 u/s with a 48 u walk between them).
			ringRouted = 0;
			if (mine)
				return;
		}
		const bool apart = floorApart(f, target->fighter);
		// rwh.108 section 1-2: once the target has run freely for 1.5 s the steer aims at its predicted point (the band, the far flag and the heading
		// are the point's); before that, and for the routed chase (the pathing states 10 / 11), at the body.
		m.leadHold = runsFreely(target->fighter) ? m.leadHold + dt : 0.0;
		const bool leading = m.leadHold >= kLeadHold && !apart && f.walkStuck < kStuckRepath;
		if (nav() != nullptr)
		{
			// Direct below 50, route beyond 100, the current mode in between. A fighter that pushes straight at
			// a target it cannot walk at (behind a wall, round a corner, within the 50 u band) would push for
			// ever: after kStuckRepath of chasing beyond the ring with no route, the routed chase takes over,
			// and it keeps its route until that ends. MINE/UNSURE: the exe's behaviour for a target under 50 u
			// behind a wall is not decoded; the timer and the switch are ours, kStuckRepath as the walk's. If
			// the route fails too, the chase give-up monitor and the 60 U clear-orders rule (chaseGivesUp) end it.
			// A target on another floor is never direct (task AS): the route is the stair, however close its xz.
			const bool routing = f.routeChase && f.routeNext < f.route.size();
			if (leading)
				f.chaseDirect = true; // the lead is the combat steer's: a target the chaser sees is aimed at, not routed to
			else if (dist >= kChaseRouteAbove || apart || f.walkStuck >= kStuckRepath || m.lineBlocked)
				f.chaseDirect = false;
			else if (dist < kChaseDirectBelow && !routing)
				f.chaseDirect = true;
			// The WaitingStateAI steer (straight, no pathing: s77_static.md b) routes too once the stuck timer has
			// run out, and keeps its route until it ends: a straight steer into a wall is the same freeze.
			if (!f.chaseDirect && (dist > steer.outer || apart || m.lineBlocked) && (!steer.straight || routing || f.walkStuck >= kStuckRepath || apart || m.lineBlocked))
			{
				if (!chaseStep(f, target->fighter.x, target->fighter.z,
						apart ? std::optional<double>(target->fighter.y) : std::nullopt, runSpeed(f), true, true,
						m.leadHold > 0.0 ? gaitOf(f).acceleration : kFootworkAcceleration) // path mode ignores the steer accumulator: after a freely running target the body follows at the movement's own acceleration (rwh.116 section 2)
					&& onCoverage(f, target->fighter))
				{
					// No route reaches it from here: give it up (task AS) instead of standing under it.
					giveUpUnreachable(m, *target);
				}
				return;
			}
		}
		if (f.routeChase && f.followTarget == 0)
			f.clearRoute(Fighter::kClearWentDirect); // the chase went direct (or ended): the wire sees the clear
		double cap = steer.cap;
		bool run = steer.run;
		if (leading)
		{
			double lead = kLeadBase + (1.0 - rules.attackClipSpeed(f)) * kLeadAnimWeight; // CharStats+0x188 is the attack animation speed mult
			lead = std::max(lead, 0.0);
			double aimX = 0.0;
			double aimZ = 0.0;
			predicted(target->fighter, lead, aimX, aimZ);
			dx = aimX - f.x;
			dz = aimZ - f.z;
			dist = std::hypot(dx, dz);
			// The far flag of the aim (rwh.116 section 5): set above a gap of 90 (160 in Waiting), cleared below 70, with the 18 u/s crawl while it is clear.
			const double gap = dist - footprintOf(f) - footprintOf(target->fighter);
			if (gap > (f.state == CombatState::Wait ? kWaitFarOn : kSteerFarOn))
				m.leadFar = true;
			else if (gap < kWaitFarOff)
				m.leadFar = false;
			cap = m.leadFar ? kWaitFarCap : kTuneWalkCap;
			run = m.leadFar;
		} else {
			m.leadFar = false;
		}
		// Face the target (turn rate capped at a half turn per second).
		const double want = std::atan2(dx, dz);
		const double turn = std::clamp(wrapAngle(want - f.yaw), -kPi * dt, kPi * dt);
		f.yaw = wrapAngle(f.yaw + turn);
		if (m.fighter.holdSteer.inHold && dist <= steer.outer)
		{
			holdPush(f, m.fighter.holdSteer.steerOn, dx / dist, dz / dist, dist, steer.inner - kFollowOffset + kHoldPushCutExtra);
			avoid(m, 0.0, 0.0);
			return;
		}
		// The speed accumulator: per character, never reset (not by a chase ending, not by a Hub frame), 0 after a load.
		m.steerSpeed = std::min(m.steerSpeed + dt * kSteerAccumulate, std::min(runSpeed(f), m.fighter.holdSteer.inHold ? kSteerHoldSpeed : kWaitFarCap));
		double desired = 0.0;
		if (dist > steer.outer)
			desired = std::min(cap, run ? runSpeed(f) : cap);
		else if (dist < steer.inner && steer.backOff)
			desired = -kTuneBackCap;
		// The accumulator limits, and the body follows it at the movement acceleration, for the chase that leads its target (rwh.116). A steer on the body keeps the
		// fitted 15 u/s^2 ramp (k16.6.172 deviation: the captures' circling / crawling / back-off speeds are fitted to it; ENGAGEMENT 16).
		if (leading)
			desired = std::min(desired, m.steerSpeed);
		if (std::fabs(desired) > std::fabs(f.forwardSpeed))
			f.forwardSpeed = knav::rampSpeed(f.forwardSpeed, desired, dt, leading ? gaitOf(f).acceleration : kFootworkAcceleration);
		else
			f.forwardSpeed = desired;
		f.lateralSpeed = 0.0;
		const double gx = dist > 1.0e-6 ? dx / dist * f.forwardSpeed : 0.0;
		const double gz = dist > 1.0e-6 ? dz / dist * f.forwardSpeed : 0.0;
		if (dist > 1.0e-6)
			shift(f, gx * dt, gz * dt);
		avoid(m, gx, gz);
	}

	// The two avoidance calls of CombatSteer::steer (s74_static.md 1.4 / 1.5), for a fighter that steers (a ring state): the repulsion of the
	// neighbours is added to the goal velocity (gx, gz) and dropped when the sum is under the dead zone (3-6 u/s while moving, 39-66 standing).
	// The goal term itself is already applied by the caller, so only the repulsion is moved here, capped at 18 u/s (the near-target cap).
	// Neighbours: standing, uncarried, conscious characters within r + r + 40 horizontally and 20 vertically; KO'd and dead bodies are skipped,
	// so they neither push nor are avoided (Q2); the current target is skipped.
	void avoid(Member& m, double gx, double gz)
	{
		Fighter& f = m.fighter;
		const CombatArena::Comfort comfort = arena.comfort(m.id, static_cast<std::uint32_t>(f.comfortRolls));
		const double rs = footprintOf(f);
		// +0x100 is 1.5 x the footprint plus the roll (3.9 for a human); the table of the roll is for the human.
		const double friendlyRadius = comfort.friendlyRadius + 1.5 * (rs - kAvoidFootprint);
		double rx = 0.0;
		double rz = 0.0;
		int counted = 0;
		for (Member& other : arena.members_)
		{
			const Fighter& o = other.fighter;
			if (other.id == m.id || other.id == f.target || !usable(o) || o.down)
				continue;
			if (std::fabs(f.y - o.y) > kAvoidVertical)
				continue; // the cheap test first: the race lookup below is not
			const double dx = f.x - o.x;
			const double dz = f.z - o.z;
			const double d = std::hypot(dx, dz);
			const double rn = footprintOf(o);
			const double rsum = rs + rn;
			if (d > rsum + kAvoidMargin)
				continue;
			const bool hostile = enemy(f, o);
			const double reach = rn + rsum + (hostile ? comfort.hostileRadius : friendlyRadius);
			if (d >= reach)
				continue;
			const double push = kAvoidScale * (1.0 - d / reach) * (hostile ? 1.0 : kAvoidFriendlyWeight);
			// Exactly stacked: split along an axis fixed by the ids, so the result does not depend on the order.
			const double ux = d > 1.0e-6 ? dx / d : (m.id < other.id ? -1.0 : 1.0);
			const double uz = d > 1.0e-6 ? dz / d : 0.0;
			rx += ux * push;
			rz += uz * push;
			if (++counted >= kAvoidMaxNeighbours)
				break;
		}
		rx *= kAvoidGain;
		rz *= kAvoidGain;
		const bool moving = std::hypot(f.forwardSpeed, f.lateralSpeed) > 0.1;
		if (std::hypot(rx + gx, rz + gz) < (moving ? comfort.deadMoving : comfort.deadStanding))
			return;
		const double len = std::hypot(rx, rz);
		if (len > kAvoidSpeedCap)
		{
			rx *= kAvoidSpeedCap / len;
			rz *= kAvoidSpeedCap / len;
		}
		shift(f, rx * dt, rz * dt);
	}

	// Controller depenetration (s77_static.md c, s80_static.md Q1 / Q3, kinspect BQ16): every active character pushes ITSELF once per step, before the
	// velocity step (so it runs at the head of CombatArena::step, on the positions the last step left), with no dt: along the line away from the ONE
	// neighbour with the largest push (not the sum), m = (R - |d|) x (0.5 + (r_n - r_s) / R), R = r_s + r_n (the race footprints, unscaled), clamped to
	// [0, 10], horizontal distance only (|dy| < 20). Neighbours and pushers are the same set: not dead, KO'd, carried, asleep
	// (the +0x2f8 bed stand-in) or exempt through CombatArena::setPushExempt (seated, operating a machine). A conscious down crawler IS a neighbour
	// (kinspect finding/bar_pile_throwout.md 4: the exe's push has no down check; only the ragdoll list is gated); its own push is x0.02 like the
	// sneaking posture's. The pusher's posture (sneak, crouched, down) scales its own push to x0.02. The pushes run one after the other in Id order, each
	// on the positions the previous ones left, so two equal humans resolve 0.75 of an overlap per step, not all of it (a lunge's per-step root motion
	// leaves a steady overlap of about 1.33 x the step, the live resting distances below R). UNSURE: the exe's perception list (rebuilt 8 characters per
	// frame, R + 40, 40 + 20 cap) is not modelled, every neighbour within R counts; the off-screen / dt >= 0.5 exemptions have no counterpart (every arena
	// member is simulated); a seat sets the bed flag +0x2f8 only by our stand-in; a player character never depenetrates in the exe (traced, a GAP here).
	// Retired: the pair rule of BQ9 (both fighters must have a target), the simultaneous pass, the 7.07 / 8 scale (retired/combat_depenetrate_scale.txt).
	void separate()
	{
		struct Body
		{
			double x;
			Member* m;
		};
		for (Member& m : arena.members_)
			m.pushX = m.pushZ = 0.0;
		std::vector<Body> bodies;
		bodies.reserve(arena.members_.size());
		double widest = 0.0;
		for (Member& m : arena.members_)
		{
			if (pushes(m))
			{
				bodies.push_back({m.fighter.x, &m});
				widest = std::max(widest, footprintOf(m.fighter));
			}
		}
		std::sort(bodies.begin(), bodies.end(), [](const Body& a, const Body& b)
		{
			return a.x != b.x ? a.x < b.x : a.m->id < b.m->id;
		});
		// Positions move by at most the cap during the pass: the window is the largest R plus both caps.
		const double window = 2.0 * widest + 2.0 * kDepenetrateCap;
		for (const Body& body : bodies)
		{
			Member& self = *body.m;
			Fighter& a = self.fighter;
			if (moveWalking(self))
				continue;
			const double ra = footprintOf(a);
			std::vector<std::pair<std::uint64_t, int>> contacts;
			double bestMag = 0.0;
			double bestX = 0.0;
			double bestZ = 0.0;
			std::uint64_t bestId = 0;
			auto from = std::lower_bound(bodies.begin(), bodies.end(), a.x - window, [](const Body& b, double v) { return b.x < v; });
			for (; from != bodies.end() && from->x <= a.x + window; ++from)
			{
				// A player character on its move walk pushes the bodies it passes (rwh.188) but not a squadmate of its own (the group walks through itself).
				if (from->m == &self || (a.playerControlled && moveWalking(*from->m)))
					continue;
				const Fighter& b = from->m->fighter;
				const double rb = footprintOf(b);
				const double R = ra + rb;
				const double dx = a.x - b.x;
				const double dz = a.z - b.z;
				const double d = std::hypot(dx, dz);
				if (d >= R || std::fabs(a.y - b.y) >= kAvoidVertical)
					continue;
				// The lag is the passing runner's (rwh.188): only a move-walking neighbour is held back by the contact set; every other pair pushes at once.
				if (moveWalking(*from->m))
				{
					int streak = 0;
					for (const auto& c : self.contacts)
					{
						if (c.first == from->m->id)
							streak = c.second;
					}
					contacts.push_back({from->m->id, streak + 1});
					if (streak < std::lround(kContactTime / dt))
						continue;
				}
				const double mag = std::clamp((R - d) * (0.5 + (rb - ra) / R), 0.0, kDepenetrateCap);
				if (mag > bestMag)
				{
					// Exactly stacked: along +x for the lower id, so the result is deterministic.
					bestMag = mag;
					bestId = from->m->id;
					bestX = d > 1.0e-6 ? dx / d : (self.id < from->m->id ? -1.0 : 1.0);
					bestZ = d > 1.0e-6 ? dz / d : 0.0;
				}
			}
			self.contacts = std::move(contacts);
			if (bestMag < kDepenetrateMin)
				continue;
			// A fighter under a technique's strafe lock gives way to nobody but its own target's push: the clip's root motion is the only
			// thing that moves its body (ENGAGEMENT 9.1, `integrate`). Its target steps out of a lunge itself; any other neighbour (a
			// squadmate in the way, a second swinger) is pushed through, or two swinging allies stack (1 vs 10: closest pair 0.5-2.4 u,
			// the capture's 6.98; k16.6.27). Pushed mid-clip it slid under the animation at up to 37 u/s (task BE: 4,612
			// clip-tick pushes in the Hub brawl, median 3 u/s, tail 20-37). UNSURE whether the exe's depenetration pass also spares
			// a locked fighter (not decoded); the lock's "code does not move the character" is what we follow. A block clip is not
			// locked (no root motion; the state-1 footwork moves it), so a blocker is still pushed.
			if (rootLocked(a) && bestId == a.target)
				continue;
			if (a.sneak || a.crouched || a.down)
				bestMag *= kDepenetratePosture; // a crawler gives way like a sneaker (live, bar_pile_throwout.md 4)
			// The camera's character is never moved (k16.6.597, user 2026-11-21): the others yield, and a neighbour that would have shared the overlap
			// with it yields all of it.
			if (self.id == arena.cameraFollow_)
				continue;
			if (bestId == arena.cameraFollow_)
				bestMag = std::min(bestMag * 2.0, pushCap(a)); // a walk-aside, never a jump
			// A player character moves at most a walk step per tick under any push, from any pusher (k16.6.597, ksim decision 2026-11-21); NPC-on-NPC
			// keeps the exe's rule.
			bestMag = std::min(bestMag, pushCap(a));
			const double fromX = a.x;
			const double fromZ = a.z;
			shift(a, bestX * bestMag, bestZ * bestMag);
			self.pushX = a.x - fromX;
			self.pushZ = a.z - fromZ;
		}
		// The body layer runs after the uprights, so their pushes never read a body and a body never reads them (task AW).
		spreadBodies();
	}

	// A player character on its own move order's walk is out of the push both ways (the original's group move: the members walk through each
	// other and a standing one is not moved by a walker, kenshi-rwh.136 live, k16.6.231). Its one depenetration is settleSelf at the stop. Only
	// that order's route counts (routeSource Order: an interact's route, a follow task's or a world task's plan walk are not it); every other
	// player character (following, driving, in a fight, idle) and every NPC pushes as before.
	bool moveWalking(const Member& m) const
	{
		const Fighter& f = m.fighter;
		return f.playerControlled && f.routeSource == Fighter::PlanSource::Order && f.drive == state::DriveMode::Stop && f.walking() && f.carrying == 0
			&& !f.sneak && f.state != CombatState::Stumble;
	}

	// The stop of a player's move walk (k16.6.231, kenshi-rwh.136): the one self-depenetration the original shows, in the last still-moving frames: the
	// controller's own rule of separate() (the one largest overlap, m = (R - d) x (0.5 + (rb - ra) / R)) run to rest, so a body whose goal lay
	// within R of a neighbour ends R = 8 away (live 7.98-7.99) and only it moves; the neighbour stays. A walker pushes nobody and is pushed by nobody (moveWalking).
	void settleSelf(Member& self)
	{
		Fighter& a = self.fighter;
		const double ra = footprintOf(a);
		if (self.id == arena.cameraFollow_)
			return; // the camera's character stays; its neighbours yield in separate()
		double budget = pushCap(a, false); // one tick's walk, never a jump; what is left of the overlap is walked off by separate()
		for (int pass = 0; pass < kSettlePasses && budget > 0.0; ++pass)
		{
			double bestMag = 0.0;
			double bestX = 0.0;
			double bestZ = 0.0;
			for (const Member& other : arena.members_)
			{
				// A squadmate still on its own move walk is passing through, not standing here: it is no one to step off from (it settles at its own stop).
				if (&other == &self || !pushes(other) || moveWalking(other))
					continue;
				const Fighter& b = other.fighter;
				const double rb = footprintOf(b);
				const double R = ra + rb;
				const double dx = a.x - b.x;
				const double dz = a.z - b.z;
				const double d = std::hypot(dx, dz);
				if (d >= R || std::fabs(a.y - b.y) >= kAvoidVertical)
					continue;
				const double mag = std::clamp((R - d) * (0.5 + (rb - ra) / R), 0.0, kDepenetrateCap);
				if (mag > bestMag)
				{
					bestMag = mag;
					bestX = d > 1.0e-6 ? dx / d : (self.id < other.id ? -1.0 : 1.0);
					bestZ = d > 1.0e-6 ? dz / d : 0.0;
				}
			}
			if (bestMag < kDepenetrateMin)
				return;
			bestMag = std::min(bestMag, budget);
			budget -= bestMag;
			shift(a, bestX * bestMag, bestZ * bestMag);
		}
	}

	// The most a push may move a player character in one tick: its walking speed x dt, so any nudge is a walk-aside, never a position jump
	// (k16.6.597, user 2026-11-21; kenshi-k16.17.13.8: standing players drifting 2.7 u/tick under patrollers). Others keep the exe's own cap.
	double pushCap(const Fighter& f, bool onTopOfPace = true) const
	{
		if (!f.playerControlled)
			return kDepenetrateCap;
		knav::Gait gait = gaitOf(f);
		gait.run = false;
		const double walkStep = knav::gaitSpeed(gait) * dt;
		// A character already walking or running gets only what is left of a walk step on top of its own pace, so the total never beats it
		// (ksim_speed_modes: a group's fast member shoved on top of its capped drive).
		const double own = onTopOfPace ? std::hypot(f.velX, f.velZ) * dt : 0.0;
		return std::min(kDepenetrateCap, std::max(0.0, walkStep - own));
	}

	// A lying body (task AW): KO'd or dead (a ragdoll), not being carried, and keeping its place in a bed or a cage. A conscious down
	// crawler is not one: it pushes as a normal neighbour and gives way like a sneaker (kinspect bar_pile_throwout.md 4).
	bool isBody(const Member& m) const
	{
		const Fighter& f = m.fighter;
		return (f.dead || f.unconscious) && f.carriedBy == 0 && !f.asleep && f.cageBuilding == 0
			&& arena.pushExempt_.count(m.id) == 0;
	}

	// Body vs body (task AW, OURS): lying bodies push only each other, by the upright pass's largest-overlap rule in the same Id
	// order, with the longer body spacing and the slower cap. An upright in the pair is ignored on both sides: it walks over the
	// body, and the body under it stays where it lies. A coincident pair splits along +x by Id, as in the upright pass. Runs on the
	// navmesh like every other push (shift() steps through the Walker), and after the upright pass.
	void spreadBodies()
	{
		struct Body
		{
			double x;
			Member* m;
		};
		std::vector<Body> bodies;
		bodies.reserve(arena.members_.size());
		for (Member& m : arena.members_)
		{
			if (isBody(m))
				bodies.push_back({m.fighter.x, &m});
		}
		if (bodies.size() < 2)
			return;
		std::sort(bodies.begin(), bodies.end(), [](const Body& a, const Body& b)
		{
			return a.x != b.x ? a.x < b.x : a.m->id < b.m->id;
		});
		const double window = CombatArena::kBodySpacing + 2.0 * kBodySpreadCap;
		for (const Body& body : bodies)
		{
			Member& self = *body.m;
			Fighter& a = self.fighter;
			double bestMag = 0.0;
			double bestX = 0.0;
			double bestZ = 0.0;
			auto from = std::lower_bound(bodies.begin(), bodies.end(), a.x - window, [](const Body& b, double v) { return b.x < v; });
			for (; from != bodies.end() && from->x <= a.x + window; ++from)
			{
				if (from->m == &self)
					continue;
				const Fighter& b = from->m->fighter;
				const double dx = a.x - b.x;
				const double dz = a.z - b.z;
				const double d = std::hypot(dx, dz);
				if (d >= CombatArena::kBodySpacing || std::fabs(a.y - b.y) >= kAvoidVertical)
					continue;
				const double mag = std::min((CombatArena::kBodySpacing - d) * kBodySpreadRate, kBodySpreadCap);
				if (mag > bestMag)
				{
					bestMag = mag;
					bestX = d > 1.0e-6 ? dx / d : (self.id < from->m->id ? -1.0 : 1.0);
					bestZ = d > 1.0e-6 ? dz / d : 0.0;
				}
			}
			if (bestMag < kBodySpreadMin)
				continue;
			shift(a, bestX * bestMag, bestZ * bestMag);
		}
	}

	// In the push at all, as pusher and as neighbour (see separate()). A conscious down crawler is in it: the exe's push has no down
	// check, only the ragdoll list is gated (kinspect bar_pile_throwout.md 4).
	bool pushes(const Member& m) const
	{
		return usable(m.fighter) && !m.fighter.asleep && arena.pushExempt_.count(m.id) == 0;
	}
};

std::vector<std::uint64_t> CombatArena::threatsOf(std::uint64_t id)
{
	std::vector<std::uint64_t> out;
	Ctx ctx(*this);
	if (Member* m = ctx.member(id))
	{
		for (const Member* t : ctx.incoming(*m))
			out.push_back(t->id);
	}
	return out;
}

std::uint64_t CombatArena::diceKey(std::uint64_t id) const noexcept
{
	const state::Fighter* f = find(id);
	return f != nullptr && f->diceSeed != 0 ? f->diceSeed : id;
}

CombatArena::Comfort CombatArena::comfort(std::uint64_t id, std::uint32_t rolls) const
{
	RngStream rng(streamSeed(streamSeed(streamSeed(seed_, kAvoidStream), static_cast<std::size_t>(diceKey(id))), rolls));
	Comfort c;
	c.hostileRadius = 15.0 + 30.0 * rng.uniform01();
	c.friendlyRadius = kComfortFootprintBase + 30.0 * rng.uniform01();
	c.deadMoving = std::min(3.0 + 3.0 * rng.uniform01(), 20.0);
	c.deadStanding = std::clamp(12.0 * rng.uniform01() + 3.0 * c.deadMoving + 30.0, c.deadMoving + 10.0, 90.0);
	return c;
}

CombatArena::CombatArena(std::shared_ptr<const CombatData> data, CombatSettings settings,
	std::uint64_t seed)
	: rules_(std::move(data), settings)
	, seed_(seed)
{
}

void CombatArena::markHostile(std::uint64_t id, std::uint64_t other)
{
	const state::Fighter* me = find(id);
	const state::Fighter* them = find(other);
	if (me == nullptr || them == nullptr || me->squad == them->squad)
		return;
	for (Member& mate : members_)
	{
		if (mate.fighter.squad != me->squad)
			continue;
		mate.dirty = true;
		auto& list = mate.fighter.hostileSquads;
		const auto at = std::lower_bound(list.begin(), list.end(), them->squad);
		if (at == list.end() || *at != them->squad)
			list.insert(at, them->squad);
	}
}

void CombatArena::forgiveHostile(std::uint64_t id, std::uint64_t other)
{
	const state::Fighter* me = find(id);
	const state::Fighter* them = find(other);
	if (me == nullptr || them == nullptr)
		return;
	for (Member& mate : members_)
	{
		if (mate.fighter.squad != me->squad)
			continue;
		mate.dirty = true;
		auto& list = mate.fighter.hostileSquads;
		const auto at = std::lower_bound(list.begin(), list.end(), them->squad);
		if (at != list.end() && *at == them->squad)
			list.erase(at);
		// Task BZ: the aggressor tag releases with the type-4 entry (the map's one row; dt23 clears on the same releases).
		auto& aggressors = mate.fighter.aggressorSquads;
		const auto aggressor = std::lower_bound(aggressors.begin(), aggressors.end(), them->squad);
		if (aggressor != aggressors.end() && *aggressor == them->squad)
			aggressors.erase(aggressor);
	}
}

void clearSneak(state::Fighter& f, SneakClear reason) noexcept
{
	(void)reason; // the trigger is for the reader; the exe's setter does the same thing for every clearing call site
	if (!f.sneak)
		return;
	f.sneak = false;
	f.moveMode = 2; // the mover default (SQUAD_FOLLOW.md s9: 1405ca9a0(c, 0) resets the speed mode)
	f.speedGroup = 0;
}

void CombatArena::endFollow(state::Fighter& f) noexcept
{
	if (f.followTarget == 0)
		return;
	f.followTarget = 0;
	f.followSource = state::Fighter::PlanSource::None;
	f.followHx = 0.0;
	f.followHz = 0.0;
	f.followDirect = true;
	f.followAge = 0.0;
	f.followUnseen = 0.0;
	f.followSeenX = 0.0;
	f.followSeenZ = 0.0;
	clearSneak(f, SneakClear::FollowStop); // Task_Follow stop: 1405ca9a0(char, 0)
	if (f.routeChase)
		f.clearRoute();
}

bool CombatArena::followLoops(std::uint64_t subject, std::uint64_t leader) const noexcept
{
	std::uint64_t at = leader;
	for (int depth = 0; depth < 16; ++depth)
	{
		if (at == subject)
			return true;
		const state::Fighter* f = find(at);
		if (f == nullptr || f->followTarget == 0)
			return false;
		at = f->followTarget;
	}
	return true;
}

// The formation of the squad led by `top` (SQUAD_FOLLOW.md s3): the leader is at (0,0); the followers, by
// ascending Id, take indices 1..n-1. A pure function of the follower list, so a change of membership
// re-lays it out (the original rebuilds on a lookup miss) and a save needs nothing. Local frame: -X is
// forward, so +x is behind the leader and +z to its right.
void CombatArena::formationOffset(std::uint64_t id, std::uint64_t top, double& ox, double& oz) const
{
	ox = 0.0;
	oz = 0.0;
	const state::Fighter* leader = find(top);
	if (leader == nullptr)
		return;
	std::vector<std::uint64_t> squad;
	// A follower of the leader itself ends the chain at once when the leader follows nobody (or is dead): no lookup for it.
	const bool leaderEnds = leader->dead || leader->followTarget == 0;
	for (const Member& m : members_)
	{
		if (m.fighter.dead || m.fighter.followTarget == 0 || m.id == top || m.fighter.team != leader->team)
			continue;
		std::uint64_t at = m.fighter.followTarget;
		for (int depth = 0; depth < 8 && !(at == top && leaderEnds); ++depth)
		{
			const state::Fighter* f = find(at);
			if (f == nullptr || f->dead || f->followTarget == 0 || f->team != leader->team)
				break;
			at = f->followTarget;
		}
		if (at == top)
			squad.push_back(m.id);
	}
	const auto it = std::find(squad.begin(), squad.end(), id);
	if (it == squad.end())
		return;
	const std::size_t k = static_cast<std::size_t>(it - squad.begin()) + 1;
	const std::size_t n = squad.size() + 1;
	switch (rules_.settings().squadFormation)
	{
	case 1: // caravan (kinspect 0789d56, DECODED + live): a single file behind the leader,
		// gap = rand(10, 20) + r_prev + r_i (live: Nomads 16, gaps 21..35.5, human r ~6), |z| <= 1.5
	{
		RngStream rng(streamSeed(streamSeed(seed_, kFormationStream), static_cast<std::size_t>(diceKey(top))));
		for (std::size_t i = 1; i <= k; ++i)
		{
			ox += 10.0 + rng.uniform01() * 10.0 + 2.0 * kFormationRadius;
			if (i == k)
				oz = rng.uniform01() * 3.0 - 1.5;
		}
		break;
	}
	case 2: // military: a grid of round(sqrt(n)) columns, the leader in cell 0 (UNSURE: spacings)
	{
		const std::size_t columns = static_cast<std::size_t>(std::lround(std::sqrt(static_cast<double>(std::max<std::size_t>(n, 2)))));
		ox = static_cast<double>(k / columns) * (25.0 + kFormationRadius);
		oz = static_cast<double>(k % columns) * (10.0 + 2.0 * kFormationRadius);
		break;
	}
	default: // random (kinspect 0789d56 + f834513, DECODED + live): a disc of radius sqrt(n x 100)
		// seeds the draw; the push-out sets the slot (leader-member 25, member-member 17; live:
		// Starving Bandits, 14 members, blob x -67..11, z -62..52; 2-member offset 25.0 live)
	{
		// The layout is a pure function of (seed, the leader's dice key, n): every follower of a squad asks for it each step, so keep it.
		const std::array<std::uint64_t, 3> key{seed_, diceKey(top), n};
		if (const auto cached = formationCache_.find(key); cached != formationCache_.end())
		{
			ox = cached->second[k].first;
			oz = cached->second[k].second;
			return;
		}
		const double reach = std::sqrt(static_cast<double>(n) * 100.0);
		std::vector<double> px(n, 0.0), pz(n, 0.0); // px[0], pz[0]: the leader at the origin
		RngStream rng(streamSeed(streamSeed(seed_, kFormationStream), static_cast<std::size_t>(diceKey(top))));
		for (std::size_t i = 1; i < n; ++i)
		{
			const double angle = rng.uniform01() * 2.0 * kPi;
			const double r = rng.uniform01() * reach;
			px[i] = r * std::cos(angle);
			pz[i] = r * std::sin(angle);
		}
		// Push the overlaps out to the pair distance (f834513: 25 against the fixed leader, 17
		// member-member). The leader stays at the origin (s3: its offset is (0,0,0)); a member against
		// it takes the full push.
		// Gauss-Seidel sweeps, the direction alternating (a one-way sweep can cycle and starve a pair).
		for (std::size_t iter = 0; iter < 16 * n; ++iter)
		{
			double worst = 0.0;
			const auto pair = [&](std::size_t i, std::size_t j)
			{
				const double dx = px[j] - px[i];
				const double dz = pz[j] - pz[i];
				const double d2 = dx * dx + dz * dz;
				const double minDist = i == 0 ? kLeaderRing : kMemberGap;
				if (d2 >= minDist * minDist)
					return;
				// A coincident pair splits along +x; anything else pushes apart along its line.
				double ux = 1.0, uz = 0.0;
				double d = std::sqrt(d2);
				if (d > 1.0e-9)
				{
					ux = dx / d;
					uz = dz / d;
				} else {
					d = 0.0;
				}
				const double overlap = minDist - d;
				worst = std::max(worst, overlap);
				if (i == 0)
				{
					px[j] += ux * overlap;
					pz[j] += uz * overlap;
				} else {
					px[i] -= ux * overlap / 2.0;
					pz[i] -= uz * overlap / 2.0;
					px[j] += ux * overlap / 2.0;
					pz[j] += uz * overlap / 2.0;
				}
			};
			if (iter % 2 == 0)
			{
				for (std::size_t i = 0; i < n; ++i)
					for (std::size_t j = i + 1; j < n; ++j)
						pair(i, j);
			} else {
				for (std::size_t i = n; i-- > 0;)
					for (std::size_t j = n; j-- > i + 1;)
						pair(i, j);
			}
			if (worst < 1.0e-9)
				break;
		}
		ox = px[k];
		oz = pz[k];
		if (formationCache_.size() >= 256)
			formationCache_.clear();
		std::vector<std::pair<double, double>>& laid = formationCache_[key];
		for (std::size_t i = 0; i < n; ++i)
			laid.emplace_back(px[i], pz[i]);
		break;
	}
	}
}

void CombatArena::add(std::uint64_t id, state::Fighter fighter)
{
	if (fighter.rng == 0)
		fighter.rng = streamSeed(streamSeed(seed_, kCombatStream),
			static_cast<std::size_t>(fighter.diceSeed != 0 ? fighter.diceSeed : id));
	if (fighter.slots.empty())
		fighter.slots.resize(5);
	if (fighter.squad == 0)
		fighter.squad = (std::uint64_t{1} << 62) | id; // a squadless fighter is a squad of one
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it != members_.end() && it->id == id)
		it->fighter = std::move(fighter);
	else
		it = members_.insert(it, Member{id, std::move(fighter)});
	// A member added during a step has not moved in it: the reported fwd/lat start from where it appeared.
	it->preX = it->fighter.x;
	it->preZ = it->fighter.z;
	initSchedule(*it);
	stampsPending_ = true; // the first step places the member's stamps (and converts a counted sensor clock)
	it->stepFwd = 0.0;
	it->stepLat = 0.0;
	// The crowd penalty at a load resumes from the saved Fighter::overflow: the next step's readers use the phase that survives
	// its flip (see step()'s write-back), so that is the slot the count goes in.
	it->refused[refusePhase_] = it->fighter.overflow;
	it->refused[refusePhase_ ^ 1] = 0;
	// The saved pair-distance samples (v114) go back into the cache; the age counts in steps from this arena's own count.
	for (const state::Fighter::PathSample& s : it->fighter.pathSamples)
		pathCache_[{id, s.other}] = PathSample{s.length, s.ax, s.ay, s.az, s.bx, s.by, s.bz, stepCount_ - s.ageTicks};
	it->fighter.pathSamples.clear();
}

std::vector<state::Fighter::PathSample> CombatArena::pathSamplesOf(std::uint64_t id) const
{
	std::vector<state::Fighter::PathSample> out;
	for (auto it = pathCache_.lower_bound({id, 0}); it != pathCache_.end() && it->first.first == id; ++it)
	{
		const PathSample& s = it->second;
		out.push_back({it->first.second, s.length, s.ax, s.ay, s.az, s.bx, s.by, s.bz, stepCount_ - s.tick});
	}
	return out;
}

bool CombatArena::setJoinGoals(std::uint64_t id, bool driven, std::uint16_t goals)
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return false;
	it->joinDriven = driven;
	it->joinGoals = goals;
	return true;
}

CombatArena::FightGoal CombatArena::fightGoalOf(std::uint64_t id)
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id || !it->joinDriven)
		return {};
	// The same scan the combat pick commits (task fight-goals: one implementation, so the selector and the tick never disagree).
	Ctx ctx(*this);
	std::uint16_t goal = 0;
	const Member* best = ctx.scanCandidates(*it, goal);
	return best != nullptr ? FightGoal{best->id, goal} : FightGoal{};
}

bool CombatArena::dropTarget(std::uint64_t id)
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return false;
	Ctx ctx(*this);
	ctx.setTarget(*it, 0);
	return true;
}

bool CombatArena::orderFight(std::uint64_t id, std::uint64_t target, std::uint16_t task)
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return false;
	Ctx ctx(*this);
	Member* foe = ctx.member(target);
	if (foe == nullptr)
		return false;
	if (!ctx.enemy(it->fighter, foe->fighter))
		ctx.markHostile(*foe, it->fighter);
	ctx.setTarget(*it, target, task);
	ctx.emit(CombatEvent::Kind::Goal, id, target, task);
	// The selector's commit is a pick too: reported here, not at the member's next pickTarget (a walker's combat step runs late).
	if (std::vector<CombatArena::FightCandidate> none; ctx.fightTraced(*it))
		ctx.traceFight(*it, target, task, false, none);
	return true;
}

bool CombatArena::torture(std::uint64_t occupantId, bool bandaged, double dt, bool* finished)
{
	state::Fighter* f = find(occupantId);
	if (f == nullptr || f->dead)
		return false;
	if (!bandaged)
	{
		// The peeler severs while Dismemberment != 0 (rwh42 1.1) and walks the limbs in the exe's order: Left Arm, Right Arm, Left Leg, Right Leg.
		std::vector<std::size_t> order;
		for (const char* name : {"Left Arm", "Right Arm", "Left Leg", "Right Leg"})
			for (std::size_t i = 0; i < f->parts.size(); ++i)
				if (f->parts[i].name == name)
					order.push_back(i);
		for (std::size_t i = 0; i < f->parts.size(); ++i)
			if (std::find(order.begin(), order.end(), i) == order.end())
				order.push_back(i);
		for (const std::size_t at : order)
		{
			state::FighterPart& p = f->parts[at];
			if (!p.limb() || p.severed())
				continue;
			if (p.health - 1.1 * p.stun > -(p.maxHealth - p.wear))
				p.health -= dt;
			// Severed in the tick the limb reaches its line: with the torso pass gone nothing else delays the release below.
			if (p.health - 1.1 * p.stun <= -(p.maxHealth - p.wear) && rules_.settings().limbLossLevel != 0 && rules_.severNow(*f, p))
			{
				severs_.push_back({occupantId, p.name});
				rules_.addXp(f->skills.toughness, 300.0 * kEatXpScale);
			}
		}
		// rwh97 7: TortureBuilding::vf28 walks the parts with part+0x20 != 0 only (the limbs): head and torso are never touched by the
		// machine (the occupant then bleeds from the severed stumps). rwh42 5.5's torso pass is gone.
		rules_.checkCollapse(*f);
		// Every part at its line (a severed limb counts): the machine releases the occupant.
		if (finished != nullptr)
		{
			*finished = true;
			for (const state::FighterPart& p : f->parts)
				*finished = *finished && (!p.limb() || p.severed() || p.health - 1.1 * p.stun <= -(p.maxHealth - p.wear));
		}
	}
	return true;
}

CombatArena::EatOutcome CombatArena::eat(std::uint64_t victimId, bool drainPath, double dt, EatState& state)
{
	EatOutcome out;
	state::Fighter* f = find(victimId);
	if (f == nullptr)
	{
		out.result = EatResult::Inedible;
		return out;
	}
	std::size_t robotic = 0;
	for (const state::FighterPart& part : f->parts)
		robotic += part.robotic ? 1 : 0;
	const RaceCombat* victimRace = data().race(f->raceSid);
	if ((victimRace != nullptr && victimRace->isRobot) || (!f->parts.empty() && robotic * 2 > f->parts.size()))
	{
		out.result = EatResult::Inedible;
		return out;
	}
	const bool conscious = !f->dead && !f->unconscious;
	double amount = 1.0;
	if (f->dead)
		amount *= 5.0;
	else
		amount *= 2.0 * (1.0 - std::clamp(f->skills.toughness * 0.01, 0.0, 1.0));
	if (conscious)
	{
		out.screamed = true;
		rules_.addXp(f->skills.toughness, 10.0 * dt * kEatXpScale);
	}
	bool counted = false;
	if (drainPath)
	{
		state.drain -= 0.5 * amount * dt * 0.01;
		f->blood -= 0.07 * amount * dt;
		counted = state.drain <= -0.5;
	} else {
		counted = true; // reaches the counter only when every part is consumed
		// The exe's vector is Head, Stomach, Chest, Left Arm, Right Arm, Left Leg, Right Leg, walked from the back; ours is built from
		// the race anatomy in another order, so the walk goes by name (any other part after them, from the back).
		std::vector<std::size_t> order;
		for (const char* name : {"Right Leg", "Left Leg", "Right Arm", "Left Arm", "Chest", "Stomach", "Head"})
			for (std::size_t i = 0; i < f->parts.size(); ++i)
				if (f->parts[i].name == name)
					order.push_back(i);
		for (std::size_t i = f->parts.size(); i-- > 0;)
			if (std::find(order.begin(), order.end(), i) == order.end())
				order.push_back(i);
		for (const std::size_t i : order)
		{
			state::FighterPart& p = f->parts[i];
			if (p.severed())
				continue;
			if (p.health - 1.1 * p.stun > -(p.maxHealth - p.wear))
			{
				p.health -= dt * amount;
				counted = false;
				break;
			}
			if (p.limb() && rules_.settings().limbLossLevel != 0 && rules_.severNow(*f, p))
			{
				severs_.push_back({victimId, p.name, false}); // severPart(.., 0, ..): no limb item
				rules_.addXp(f->skills.toughness, 300.0 * kEatXpScale);
			}
		}
		rules_.checkCollapse(*f);
	}
	if (counted)
	{
		if (state.count > kEatCounterMax)
			out.result = EatResult::Eaten;
		else
			++state.count;
	}
	return out;
}

CombatArena::Member* CombatArena::memberOf(std::uint64_t id) noexcept
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id, [](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return nullptr;
	it->dirty = true; // a mutable handle: a possible write
	return &*it;
}

void CombatArena::landBlow(Ctx& ctx, Member& att, Member& def, double dmg[6])
{
	if (std::find(def.hitBy.begin(), def.hitBy.end(), att.id) == def.hitBy.end())
		def.hitBy.push_back(att.id);
	def.lastAttacker = att.id;
	perception::notice(def.fighter, att.id); // a bolt that lands tells the victim who shot it
	if (def.fighter.target == 0)
		def.fighter.orderInterrupt = true;
	const bool wasDown = def.fighter.unconscious;
	const Technique blow; // the chop is no technique of the table
	std::string part;
	bool knockedOut = false;
	bool died = false;
	double absorbed[2] = {};
	ctx.medicalHit(def, blow, 0, dmg, part, knockedOut, died, absorbed);
	if (knockedOut || (!wasDown && def.fighter.unconscious && !died))
		ctx.emit(CombatEvent::Kind::Knockout, def.id, att.id);
	if (died)
	{
		ctx.emit(CombatEvent::Kind::Death, def.id, 0);
		deaths_.push_back({def.id, att.id});
	}
}

bool CombatArena::strikeFromTask(std::uint64_t attacker, std::uint64_t defender)
{
	Member* att = memberOf(attacker);
	Member* def = memberOf(defender);
	if (att == nullptr || def == nullptr)
		return false;
	Ctx ctx(*this);
	double dmg[6];
	rules_.computeDamage(att->fighter, def->fighter, dmg);
	landBlow(ctx, *att, *def, dmg);
	return true;
}

// dt41_A section 1: no skill in the range test; the height factor is 1 for a target at y 0, else 1 - 0.6 x above the shooter (min 0.4),
// 1 + 0.5 x below it (max 1.5), x = clamp((|dy| - 20) / 280, 0, 1).
bool CombatArena::inRangedReach(const state::Fighter& shooter, const state::Fighter& target) noexcept
{
	double factor = 1.0;
	if (target.y != 0.0)
	{
		const double x = std::clamp((std::fabs(shooter.y - target.y) - 20.0) / (300.0 - 20.0), 0.0, 1.0);
		factor = target.y > shooter.y ? 1.0 - 0.6 * x : 1.0 + 0.5 * x;
	}
	const double reach = shooter.gun().range * factor;
	const double dx = target.x - shooter.x;
	const double dy = target.y - shooter.y;
	const double dz = target.z - shooter.z;
	return dx * dx + dy * dy + dz * dz < reach * reach;
}

namespace
{
constexpr double kRetrySeconds = 0.3; // gun+0x70 while the shot is not ready (dt41_A section 2)
// rwh.166: the aim payout, 0.02 x dt x 0.75 x weight x race rate per tick, the weights of CharStats 1408c7250 (stat ids 0x18 / 0x23 / 0x0b).
constexpr double kAimXp = 0.02; // x 0.75 = `exp gain multiplier` 3 x 0.25 (rules_.data().k.expGainMultiplier x 0.25)
constexpr double kPerceptionWeight = 3.0;
constexpr double kCrossbowsWeight = 1.2;
constexpr double kTurretsWeight = 1.0;
constexpr std::int32_t kStatPerception = 0x18;
constexpr std::int32_t kStatCrossbows = 0x23;
constexpr std::int32_t kStatTurrets = 0x0b;
constexpr double kBodyHalfWidth = 5.6; // the shot hits when its line passes within this of the body centre (OURS, the arena's kFootprint)
constexpr double kTurretArcHalf = kPi / 2.0; // UNSURE: dt41_B has no firing arc; ksim's brief wants one, so a turret covers 180 degrees ahead of its facing
constexpr double kMuzzleHeight = 18.0; // LineOfSight::kEyeHeight: the muzzle stands at the shooter's eye (barrel pos Y/Z are not applied, UNSURE)
} // namespace

bool CombatArena::inTurretArc(const state::Fighter& shooter, const state::Fighter& target) noexcept
{
	double turn = std::atan2(target.x - shooter.x, target.z - shooter.z) - shooter.mount.yaw;
	while (turn > kPi)
		turn -= 2.0 * kPi;
	while (turn < -kPi)
		turn += 2.0 * kPi;
	return std::fabs(turn) <= kTurretArcHalf;
}

bool CombatArena::mountTurret(std::uint64_t id, std::uint64_t building, double x, double z, double yaw, const state::FighterRanged& gun)
{
	Member* m = memberOf(id);
	if (m == nullptr || building == 0 || m->fighter.dead || m->fighter.mount.mounted())
		return false;
	m->fighter.mount = {building, x, z, yaw, gun};
	return true;
}

void CombatArena::dismountTurret(std::uint64_t id)
{
	if (Member* m = memberOf(id))
		m->fighter.mount = {};
}

std::string CombatArena::checkStealthKo(std::uint64_t assassin, std::uint64_t victim) const
{
	const Fighter* me = find(assassin);
	const Fighter* him = find(victim);
	if (me == nullptr || him == nullptr || assassin == victim)
		return "bad_target";
	const Fighter& a = *me;
	const Fighter& v = *him;
	// Row 228's preconditions: a working arm, movement allowed (dt41_C); the victim must still be standing to be knocked out.
	if (a.dead || a.down || a.unconscious || a.carriedBy != 0 || a.mount.mounted() || a.cageBuilding != 0 || (!a.rightArmOk && !a.leftArmOk)
		|| v.dead || v.down || v.unconscious || v.carriedBy != 0)
		return "not_allowed";
	return {};
}

std::string CombatArena::orderStealthKo(std::uint64_t assassin, std::uint64_t victim)
{
	if (std::string reason = checkStealthKo(assassin, victim); !reason.empty())
		return reason;
	Fighter& f = memberOf(assassin)->fighter;
	f.stealthKo = {victim, 0, 0.0};
	f.clearRoute();
	f.drive = state::DriveMode::Stop;
	f.driveStrafe = 0;
	f.orderTarget = 0;
	f.followTarget = 0;
	f.hold = false;
	return {};
}

void CombatArena::cancelStealthKo(std::uint64_t assassin)
{
	if (Member* m = memberOf(assassin))
		m->fighter.stealthKo = {};
}

CombatArena::Shot CombatArena::rangedFire(std::uint64_t attacker, std::uint64_t defender, double dt, bool aimXp)
{
	Member* att = memberOf(attacker);
	Member* def = memberOf(defender);
	if (att == nullptr || !att->fighter.gun().armed())
		return Shot::NoWeapon;
	state::FighterRanged& gun = att->fighter.gun();
	if (def == nullptr || def->fighter.dead || def == att || Ctx::caged(def->fighter))
		return Shot::NoTarget;
	const state::Fighter& me = att->fighter;
	if (!inRangedReach(me, def->fighter))
		return Shot::OutOfRange;
	// The aim XP (rwh.166): every tick the range test passes with a bolt loaded, before the arc / timer / line decisions, so a wall still pays.
	// UNSURE: the original's loaded count (gunStats+0x20) is mapped to gun.ammo; it is not the carried stock. The original's 6-per-frame
	// off-screen throttle is not ported (we pay per sim tick).
	if (aimXp && gun.ammo >= 1)
	{
		state::Fighter& trainee = att->fighter;
		const RaceCombat* own = rules_.data().race(trainee.raceSid);
		const bool turret = trainee.mount.mounted();
		const double unit = kAimXp * dt * rules_.data().k.expGainMultiplier * 0.25;
		const auto rate = [&](std::int32_t stat) { return own != nullptr ? own->xpRate(stat) : 1.0; };
		rules_.addXp(trainee.skills.perception, unit * kPerceptionWeight * rate(kStatPerception));
		rules_.addXp(turret ? trainee.skills.turrets : trainee.skills.crossbows,
			unit * (turret ? kTurretsWeight : kCrossbowsWeight) * rate(turret ? kStatTurrets : kStatCrossbows));
	}
	if (me.mount.mounted() && !inTurretArc(me, def->fighter))
		return Shot::OutOfArc;
	gun.timer -= dt;
	if (gun.timer > 0.0)
		return Shot::Waiting;
	if (gun.ammo < 1)
		return Shot::NoAmmo;
	// SIMLINK event 49: one per bolt volley that leaves the gun (a hit or a miss; a Blocked return is a held shot and sends nothing). who = the target, as event 1 is routed.
	const auto announce = [&](std::int32_t result)
	{
		if (!observer_)
			return;
		CombatEvent e;
		e.kind = CombatEvent::Kind::Shot;
		e.time = time_;
		e.who = defender;
		e.other = attacker;
		e.code = result;
		e.part = gun.sid;
		e.turret = me.mount.building;
		e.dmg[0] = me.x;
		e.dmg[1] = me.y + kMuzzleHeight;
		e.dmg[2] = me.z;
		e.dmg[3] = def->fighter.x;
		e.dmg[4] = def->fighter.y;
		e.dmg[5] = def->fighter.z;
		observer_(e);
	};
	Ctx ctx(*this);
	// An ally (or any body that is not an enemy) on the line aborts the shot: no friendly fire (dt41_A section 3).
	const double ax = me.x;
	const double az = me.z;
	const double sx = def->fighter.x - ax;
	const double sz = def->fighter.z - az;
	const double length2 = sx * sx + sz * sz;
	for (const Member& other : members_)
	{
		if (other.id == attacker || other.id == defender || !ctx.usable(other.fighter) || ctx.enemy(me, other.fighter))
			continue;
		const double along = length2 > 0.0 ? ((other.fighter.x - ax) * sx + (other.fighter.z - az) * sz) / length2 : 2.0;
		const double px = ax + along * sx - other.fighter.x;
		const double pz = az + along * sz - other.fighter.z;
		if (along > 0.0 && along < 1.0 && px * px + pz * pz < kBodyHalfWidth * kBodyHalfWidth)
		{
			gun.timer = kRetrySeconds;
			return Shot::Blocked;
		}
	}
	// A hull or the terrain short of the target stops it (the first blocker, LineOfSight::firstBlocker).
	if (environment_.shotBlocker && environment_.shotBlocker({me.x, me.y + kMuzzleHeight, me.z}, def->fighter) >= 0.0)
	{
		gun.timer = kRetrySeconds;
		return Shot::Blocked;
	}
	// The shot. Deviation: the weapon's at skill 0 shrinking to none at `accuracy perfect skill` (UNSURE: linear; the curve is unread).
	const double skill = me.mount.mounted() ? me.skills.turrets : me.skills.crossbows; // a turret's skill is 0xb, not Crossbows (dt41_B section 3)
	const double degrees = gun.deviation * std::clamp(1.0 - skill / std::max(gun.perfectSkill, 1.0), 0.0, 1.0);
	const double distance = std::sqrt(length2);
	state::Fighter& shooter = att->fighter;
	shooter.yaw = std::atan2(sx, sz);
	const RaceCombat* race = rules_.data().race(def->fighter.raceSid);
	bool landed = false;
	for (std::int32_t i = 0; i < gun.numShots && gun.ammo > 0; ++i)
	{
		--gun.ammo;
		const double miss = std::tan((2.0 * draw(shooter.rng) - 1.0) * degrees * kPi / 180.0) * distance;
		if (std::fabs(miss) >= kBodyHalfWidth || !ctx.usable(def->fighter))
			continue;
		// UNSURE: the pierce roll is uniform over the weapon's min..max (the decode has no damage path).
		const double pierce = gun.pierceMin + draw(shooter.rng) * (gun.pierceMax - gun.pierceMin);
		double dmg[6] = {0.0, 0.0, pierce * (race != nullptr && race->isRobot ? gun.robotMult : gun.humanMult), 0.0, gun.bleedMult, 0.0};
		landBlow(ctx, *att, *def, dmg);
		landed = true;
	}
	// The next shot after the reload: skill interpolates, 0 skill = `reload time max` (dt41_A table).
	gun.timer = gun.reloadMax + (gun.reloadMin - gun.reloadMax) * std::clamp(skill * 0.01, 0.0, 1.0);
	announce(landed ? 0 : 2);
	return landed ? Shot::Hit : Shot::Missed;
}

bool CombatArena::remove(std::uint64_t id)
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return false;
	members_.erase(it);
	fightLast_.erase(id);
	replanFail_.erase(id);
	std::erase_if(pathCache_, [id](const auto& entry) { return entry.first.first == id || entry.first.second == id; });
	return true;
}

// A mutable find is a possible write from outside the arena: the member is marked for the next mirror (a const find is a read).
state::Fighter* CombatArena::find(std::uint64_t id) noexcept
{
	auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it == members_.end() || it->id != id)
		return nullptr;
	it->dirty = true;
	return &it->fighter;
}

const state::Fighter* CombatArena::find(std::uint64_t id) const noexcept
{
	const auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	return it != members_.end() && it->id == id ? &it->fighter : nullptr;
}

void CombatArena::touch(std::uint64_t id) noexcept
{
	if (Member* m = memberOf(id))
		m->dirty = true;
}

#if KSIM_PROFILE
#define STEP_MARK(which) do { if (profiler_ != nullptr) profiler_->sub(profile::Sub::which); } while (0)
#else
#define STEP_MARK(which) do {} while (0)
#endif

void CombatArena::setSensorContext(sensor::Context context)
{
	sensor_ = std::move(context);
	if (!sensor_.relation)
		return;
	sensor_.relation = [memo = relationMemo_, inner = std::move(sensor_.relation)](std::uint64_t from, std::uint64_t to)
	{
		if (!memo->on)
			return inner(from, to);
		auto& row = memo->byFrom[from];
		const auto hit = row.find(to);
		if (hit != row.end())
			return hit->second;
		return row.emplace(to, inner(from, to)).first->second;
	};
}

#if KSIM_PROFILE
// kenshi-k16.6.305: the update loop's think / route / total times and the interaction islands, into the tick's probe columns. The islands use the
// positions after the update (the loop moves a member by a fraction of a unit): union-find over the characters within the sensor's longest sight
// (1500 u, race mult 1), and over the target links; each island's share of the loop is the sum of its members' own update times.
void CombatArena::profileIslands(const std::vector<std::uint64_t>& updateNs, std::uint64_t updateTotalNs, std::uint64_t routeNs, std::uint64_t thinkNs,
	std::uint64_t thinkRouteNs, std::uint64_t thinkCalls, std::uint64_t scanNs, std::uint64_t scanCalls)
{
	using profile::Probe;
	profiler_->probe(Probe::ThinkUs, static_cast<double>(thinkNs) / 1000.0);
	profiler_->probe(Probe::ThinkRouteUs, static_cast<double>(thinkRouteNs) / 1000.0);
	profiler_->probe(Probe::ThinkCalls, static_cast<double>(thinkCalls));
	profiler_->probe(Probe::ScanUs, static_cast<double>(scanNs) / 1000.0);
	profiler_->probe(Probe::ScanCalls, static_cast<double>(scanCalls));
	profiler_->probe(Probe::UpdUs, static_cast<double>(updateTotalNs) / 1000.0);
	profiler_->probe(Probe::UpdRouteUs, static_cast<double>(routeNs) / 1000.0);
	profiler_->probe(Probe::UpdMembers, static_cast<double>(members_.size()));
	const std::uint64_t began = knav::measure::now();
	static thread_local dev::IslandProbe uf;
	static thread_local std::vector<double> xs, zs;
	static thread_local std::vector<char> active;
	const std::size_t n = members_.size();
	xs.resize(n);
	zs.resize(n);
	active.resize(n);
	for (std::size_t i = 0; i < n; ++i)
	{
		const Fighter& f = members_[i].fighter;
		xs[i] = f.x;
		zs[i] = f.z;
		active[i] = Ctx::usable(f) ? 1 : 0;
	}
	uf.reset(n);
	uf.joinByRange(xs, zs, active, 1500.0);
	const dev::IslandStats sight = uf.stats(active);
	std::uint64_t topNs = 0;
	std::uint64_t allNs = 0;
	for (std::size_t i = 0; i < n; ++i)
	{
		allNs += updateNs[i];
		if (active[i] && uf.find(i) == sight.largestIndex)
			topNs += updateNs[i];
	}
	const double unionUs = static_cast<double>(knav::measure::now() - began) / 1000.0;
	// The target links alone: a member and the one it targets (a fight, a chase).
	uf.reset(n);
	for (std::size_t i = 0; i < n; ++i)
		if (active[i] && members_[i].fighter.target != 0)
			if (const auto it = std::lower_bound(members_.begin(), members_.end(), members_[i].fighter.target, [](const Member& m, std::uint64_t v) { return m.id < v; });
				it != members_.end() && it->id == members_[i].fighter.target)
				uf.join(i, static_cast<std::size_t>(it - members_.begin()));
	const dev::IslandStats links = uf.stats(active);
	std::size_t activeCount = 0;
	for (const char a : active)
		activeCount += a != 0 ? 1 : 0;
	profiler_->probe(Probe::IslN, static_cast<double>(sight.components));
	profiler_->probe(Probe::IslMax, static_cast<double>(sight.largest));
	profiler_->probe(Probe::Isl2nd, static_cast<double>(sight.second));
	profiler_->probe(Probe::IslSingle, static_cast<double>(sight.singletons));
	profiler_->probe(Probe::IslTopShare, allNs > 0 ? static_cast<double>(topNs) / static_cast<double>(allNs) : 0.0);
	profiler_->probe(Probe::IslUfUs, unionUs);
	profiler_->probe(Probe::TgtN, static_cast<double>(links.components));
	profiler_->probe(Probe::TgtMax, static_cast<double>(links.largest));
	profiler_->probe(Probe::TgtNonsingle, static_cast<double>(links.nonSingleton));
	profiler_->probe(Probe::IslActive, static_cast<double>(activeCount));
	profiler_->probe(Probe::Steps, 1.0);
}
#endif

bool CombatArena::inFightTier(const Member& m) const
{
	const Fighter& f = m.fighter;
	return f.target != 0 || !f.engaged.empty() || !f.hitList.empty() || f.technique >= 0 || f.reactClip >= 0 || f.recovering
		|| f.state == CombatState::Stumble || f.unconscious || f.down || f.orderTarget != 0 || f.carrying != 0 || f.carriedBy != 0
		|| f.stealthKo.active() || f.mount.building != 0;
}

// Whether `m` is updated this step. The tier is the fight predicate now, or an event promotion (the setters above and promote(), which set
// fightTier between two steps) not yet consumed here. Nothing else is remembered, so a load that re-derives the tier from the fighter state lands
// on the straight run's schedule exactly (kenshi-k16.6.658). A member that leaves the tier on an off-phase step owes its idle step the whole
// span since its last update (the dt is phaseStep - lastStep), which is what initSchedule's latest-due-step gives a reload.
bool CombatArena::stepsNow(Member& m)
{
	const int fight = std::max(1, rules_.settings().fightStride);
	const int idle = std::max(1, rules_.settings().idleStride);
	if (fight == 1 && idle == 1)
		return true;
	const bool fighting = inFightTier(m);
	const bool tier = fighting || m.fightTier;
	m.fightTier = fighting;
	const std::uint64_t stride = static_cast<std::uint64_t>(tier ? fight : idle);
	return (phaseStep_ + m.id) % stride == 0;
}

// The last update: the previous step for the fight tier, else the latest due step of the idle phase (what the straight run's schedule had).
void CombatArena::initSchedule(Member& m)
{
	m.fightTier = inFightTier(m);
	m.lastStep = phaseStep_;
	if (!m.fightTier)
		m.lastStep -= (phaseStep_ + m.id) % static_cast<std::uint64_t>(std::max(1, rules_.settings().idleStride));
}

// A member whose Fighter carries an older save's counted sensor clock, or no medical / hunger step (a new character or an older save): the stamps
// say the time up to the previous step is accounted; the member's own second boundary (the Id's phase) then staggers when each runs (k16.6.637).
void CombatArena::placeStamps(Member& m)
{
	Fighter& f = m.fighter;
	if (f.senseClock >= 0.0)
	{
		f.senseAt = time_ - stepSeconds_ - f.senseClock; // the save's seconds since the last scan; the counted clock took this step's dt on the spot
		f.senseClock = -1.0;
		m.dirty = true;
	}
	if (f.medStep == Fighter::kNoStep || f.hungerStep == Fighter::kNoStep)
	{
		const std::uint64_t last = phaseStep_ - 1; // everything up to the previous step is accounted (a counted save, or a new character)
		if (f.medStep == Fighter::kNoStep)
			f.medStep = last;
		if (f.hungerStep == Fighter::kNoStep)
			f.hungerStep = last;
		m.dirty = true;
	}
}

void CombatArena::setPhaseStep(std::uint64_t steps) noexcept
{
	if (steps == phaseStep_)
		return;
	phaseStep_ = steps;
	for (Member& m : members_)
		initSchedule(m);
}

void CombatArena::publishDue(std::uint64_t tick, int steps)
{
	const int fight = std::max(1, rules_.settings().fightStride);
	const int idle = std::max(1, rules_.settings().idleStride);
	const bool all = fight == 1 && idle == 1;
	for (Member& m : members_)
	{
		bool due = all;
		if (!all)
		{
			// The tier the first step would use (promotion is the scan or an event; the demotion lands on a due step), then any of the steps.
			const std::uint64_t stride = static_cast<std::uint64_t>(m.fightTier || inFightTier(m) ? fight : idle);
			for (int k = 1; k <= steps && !due; ++k)
				due = (phaseStep_ + static_cast<std::uint64_t>(k) + m.id) % stride == 0;
		}
		m.tickStepped = false;
		// A route set or cleared since its last pass is due at once: the door planner has to see a walker that starts beside a shut door
		// before its first step carries it past (a pure function of the fighter's state).
		due = due || m.fighter.routeVersion != m.dueRouteVersion;
		m.due = due;
		if (due)
		{
			markDue(m, tick, all);
			m.dueRouteVersion = m.fighter.routeVersion;
		}
	}
}

void CombatArena::markDue(Member& m, std::uint64_t tick, bool all)
{
	m.dueTicks = all || m.dueLastTick == 0 || m.dueLastTick >= tick ? 1 : static_cast<std::uint32_t>(tick - m.dueLastTick);
	m.dueLastTick = tick;
}

void CombatArena::settleDue(std::uint64_t tick)
{
	// The pre-pass ran on the prediction; a step that landed on a member the prediction missed (a promotion mid-tick) puts it on the list now,
	// for the loops that run after the arena.
	for (Member& m : members_)
		if (m.tickStepped && !m.due)
		{
			m.due = true;
			markDue(m, tick, false);
		}
}

void CombatArena::promote(std::uint64_t id) noexcept
{
	const auto it = std::lower_bound(members_.begin(), members_.end(), id, [](const Member& m, std::uint64_t v) { return m.id < v; });
	if (it != members_.end() && it->id == id)
		it->fightTier = true;
}

void CombatArena::step(double dt)
{
	if (members_.empty())
		return;
	struct MemoScope
	{
		RelationMemo& memo;
		explicit MemoScope(RelationMemo& m) : memo(m) { memo.byFrom.clear(); memo.on = true; }
		~MemoScope() { memo.on = false; }
	} memoScope(*relationMemo_);
	time_ += dt;
	stepSeconds_ = dt;
	sliceSteps_ = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(1.0 / std::max(dt, 1.0e-6))));
	++stepCount_;
	++phaseStep_;
	// The crowd penalty's double buffer (crowd_melee.md §10): this step's refusals accumulate in the new phase; every reader sees
	// the other phase, i.e. the previous step's count. The new phase starts empty (no decay, no accumulation).
	refusePhase_ ^= 1;
	for (Member& m : members_)
		m.refused[refusePhase_] = 0;
	// The body's actual motion for the wire's fwd/lat is a displacement over the whole step, so it includes the correction
	// snap and the separation push, not only the walker's own move (SIMLINK "CHAR_DYNAMIC").
	for (Member& m : members_)
	{
		m.preX = m.fighter.x;
		m.preZ = m.fighter.z;
		if (stampsPending_)
			placeStamps(m);
	}
	stampsPending_ = false;
	Ctx ctx(*this);
	ctx.dt = dt;
	ctx.indexFollows();
	STEP_MARK(CsIndex);
	ctx.senseAll();
	STEP_MARK(CsSense);
	ctx.weatherDamage();
	ctx.beamDamage();
	for (Member& m : members_)
		ctx.correct(m);
	STEP_MARK(CsEnv);
	ctx.separate(); // the controller's push runs before the velocity step (s80_static.md Q1.6)
	STEP_MARK(CsSeparate);
	for (Member& m : members_)
	{
		const double progress = ctx.computeSwingProgress(m.fighter);
		if (progress != m.fighter.swingProgress)
		{
			m.fighter.swingProgress = progress;
			m.dirty = true;
		}
	}
#if KSIM_PROFILE
	const bool probing = profiler_ != nullptr && profiler_->probing();
	std::vector<std::uint64_t> updateNs;
	std::uint64_t routeBefore = 0;
	std::uint64_t updateBegan = 0;
	if (probing)
	{
		updateNs.assign(members_.size(), 0);
		routeBefore = knav::measure::counters.routeNs.load(std::memory_order_relaxed);
		ctx.think.on = true;
		updateBegan = knav::measure::now();
	}
	std::size_t memberIndex = 0;
	std::vector<std::pair<double, double>> players;
	if (probing)
	{
		for (const Member& m : members_)
		{
			if (m.fighter.playerControlled && !m.fighter.dead)
				players.emplace_back(m.fighter.x, m.fighter.z);
		}
	}
#endif
	for (Member& m : members_)
	{
		const double px = m.fighter.x;
		const double pz = m.fighter.z;
#if KSIM_PROFILE
		const std::uint64_t memberBegan = probing ? knav::measure::now() : 0;
		if (probing)
		{
			double nearest = std::numeric_limits<double>::infinity();
			for (const auto& [qx, qz] : players)
				nearest = std::min(nearest, std::hypot(px - qx, pz - qz));
			ctx.band = nearest <= 2000.0 ? 0 : nearest <= 3250.0 ? 1 : 2;
		}
#endif
		m.stepped = stepsNow(m);
		if (m.stepped)
		{
			m.tickStepped = true;
			// Owed time: the steps since its last update (1 in the fight tier, the stride otherwise; a tier change flushes the partial).
			m.stepDt = m.lastStep + 1 == phaseStep_ ? dt : static_cast<double>(phaseStep_ - m.lastStep) * dt;
			m.lastStep = phaseStep_;
			ctx.dt = m.stepDt;
			ctx.update(m);
			ctx.dt = dt;
			m.fighter.velX = (m.fighter.x - px) / m.stepDt; // the mover's velocity (+0xd0, +0xd8) the follow task reads
			m.fighter.velZ = (m.fighter.z - pz) / m.stepDt;
			m.dirty = true;
		}
#if KSIM_PROFILE
		if (probing)
		{
			updateNs[memberIndex] = knav::measure::now() - memberBegan;
			++ctx.branchN[static_cast<std::size_t>(ctx.branch * 3 + ctx.band)];
		}
		++memberIndex;
#endif
	}
#if KSIM_PROFILE
	if (probing)
	{
		ctx.think.on = false;
		for (std::size_t i = 0; i < ctx.branchN.size(); ++i)
			profiler_->probe(static_cast<profile::Probe>(static_cast<std::size_t>(profile::Probe::BranchBase) + i), static_cast<double>(ctx.branchN[i]));
		for (std::size_t i = 0; i < ctx.think.byState.size(); ++i)
			profiler_->probe(static_cast<profile::Probe>(static_cast<std::size_t>(profile::Probe::PickBase) + i), static_cast<double>(ctx.think.byState[i]));
		profileIslands(updateNs, knav::measure::now() - updateBegan, knav::measure::counters.routeNs.load(std::memory_order_relaxed) - routeBefore, ctx.think.thinkNs,
			ctx.think.thinkRouteNs, ctx.think.thinkCalls, ctx.think.scanNs, ctx.think.scanCalls);
	}
#endif
	STEP_MARK(CsUpdate);
	// The ragdoll's lying yaw (task BG, PHYSICS.md 3): a prone body (flag bit 5) keeps one rest yaw; a body that goes down
	// without an EVENT 48 of its own (a limb collapse, a load of an older save) rolls one here, and a body that is up again
	// drops it. The events of the step rolled theirs already (CombatArena::rollRagdoll sets the flag).
	for (Member& m : members_)
	{
		Fighter& f = m.fighter;
		if (!(f.down || f.unconscious || f.dead))
		{
			if (f.restYawValid)
				m.dirty = true;
			f.restYawValid = false;
		} else if (!f.restYawValid) {
			rollRagdoll(f, m.id);
			m.dirty = true;
		}
	}
	// The facing at the end of the step: forward is (sin yaw, cos yaw), the right is (cos yaw, -sin yaw).
	for (Member& m : members_)
	{
		const double dx = m.fighter.x - m.preX;
		const double dz = m.fighter.z - m.preZ;
		const double sin = std::sin(m.fighter.yaw);
		const double cos = std::cos(m.fighter.yaw);
		if (m.stepped)
		{
			m.stepFwd = (dx * sin + dz * cos) / m.stepDt;
			m.stepLat = (dx * cos - dz * sin) / m.stepDt;
		}
		// The count the next step's readers will see, into the saved field: a load must resume with the same crowd penalty the
		// straight run would (ksim_nav_regions compares the two hashes). Fighter::overflow is what the save carries.
		if (m.fighter.overflow != m.refused[refusePhase_])
		{
			m.fighter.overflow = m.refused[refusePhase_];
			m.dirty = true;
		}
		// Whatever moved (the correction snap, the separation push, a walker) differs from the ECS copy.
		if (dx != 0.0 || dz != 0.0)
			m.dirty = true;
	}
	// A member a stepping member is linked to may have been written by it (the victim's engaged list and slots, a hit, a carried body): the
	// same set the fight tier promotes, so a far member the step touched is copied at this tick's mirror.
	for (const Member& m : members_)
	{
		if (!m.stepped)
			continue;
		const Fighter& f = m.fighter;
		for (const std::uint64_t id : {f.target, f.carrying, f.carriedBy, f.orderTarget, f.followTarget})
			if (id != 0)
				touch(id);
		for (const std::uint64_t id : f.engaged)
			touch(id);
		for (const std::uint64_t id : f.hitList)
			touch(id);
	}
	STEP_MARK(CsTail);
}

std::uint32_t CombatArena::lastRefusals(std::uint64_t id) const noexcept
{
	const auto it = std::lower_bound(members_.begin(), members_.end(), id,
		[](const Member& m, std::uint64_t v) { return m.id < v; });
	return it != members_.end() && it->id == id ? it->refused[refusePhase_ ^ 1] : 0;
}

std::optional<std::pair<double, double>> CombatArena::waitRingOf(std::uint64_t waiterId) const noexcept
{
	const auto find = [this](std::uint64_t id) -> const Member*
	{
		const auto it = std::lower_bound(members_.begin(), members_.end(), id,
			[](const Member& m, std::uint64_t v) { return m.id < v; });
		return it != members_.end() && it->id == id ? &*it : nullptr;
	};
	const Member* waiter = find(waiterId);
	const Member* target = waiter != nullptr && waiter->fighter.target != 0 ? find(waiter->fighter.target) : nullptr;
	if (target == nullptr)
		return std::nullopt;
	// WaitingStateAI's n (crowd_melee.md §10.4): the TARGET's engaged list plus the refusals it made last step; the follow args carry
	// the same 2 x charWidth - 5.6 offset the other rings use.
	const double n = static_cast<double>(target->fighter.engaged.size()) + static_cast<double>(target->refused[refusePhase_ ^ 1]);
	const double offset = 2.0 * CombatRules::kCharWidth - 5.6;
	return std::pair<double, double>{n * 0.25 + 15.0 + offset, n * 3.0 + 25.0 + offset};
}

std::uint32_t CombatArena::rollRagdoll(state::Fighter& f, std::uint64_t id, double pushX, double pushZ, double push)
{
	// A fighter's own stream (kRagdollStream), advanced once per ragdoll start: the seed the client flops from and the yaw it
	// comes to rest at. The count is saved, so a replay and a save/load continue the same sequence; the combat dice never move.
	RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kRagdollStream), static_cast<std::size_t>(diceKey(id))),
		static_cast<std::size_t>(f.restRolls++)));
	f.restYaw = dice.uniform01() * 6.28318530717958647692;
	f.restYawValid = true;
	const std::uint32_t seed = static_cast<std::uint32_t>(dice.next());
	// The rest point: a body in a bed, a cage, asleep or carried keeps its place, and so does one with no navmesh to keep it on.
	// A body inside a building rests on its floor (no slope to slide down).
	if (navigation_ == nullptr || !(f.dead || f.unconscious) || f.carriedBy != 0 || f.asleep || f.cageBuilding != 0
		|| pushExempt_.count(id) != 0)
		return seed;
	const knav::Walker& walker = navigation_->walker();
	const bool grounded = f.building == 0; // a body inside a building has no slope to slide down
	const auto move = [&](double dx, double dz)
	{
		const knav::StepResult r = walker.step({f.x, f.y, f.z}, dx, dz);
		if (!r.onMesh)
			return;
		// A step that ends blocked at an edge can land a unit or more off the mesh (the next step's correction would snap a live
		// fighter, never a dead one): snap it here, and keep the old point when there is none.
		knav::Position at = r.at;
		if (!walker.inside(at))
		{
			const auto snapped = walker.snap(at, kRestSnapReach);
			if (!snapped)
				return;
			at = *snapped;
		}
		f.x = at.x;
		f.y = at.y;
		f.z = at.z;
	};
	if (push > 0.0)
		move(pushX * push, pushZ * push);
	// A body that died keeps its death point: only the living (KO'd) slide.
	if (!grounded || f.dead)
		return seed;
	double bestSlope = kSlideMinSlope;
	double slideX = 0.0;
	double slideZ = 0.0;
	for (int i = 0; i < 8; ++i)
	{
		const double angle = static_cast<double>(i) * 0.7853981633974483;
		const double dx = std::sin(angle) * kSlideProbe;
		const double dz = std::cos(angle) * kSlideProbe;
		const knav::StepResult r = walker.step({f.x, f.y, f.z}, dx, dz);
		if (!r.onMesh || r.moved < 0.5 * kSlideProbe)
			continue;
		const double slope = (f.y - r.at.y) / r.moved;
		if (slope > bestSlope)
		{
			bestSlope = slope;
			slideX = dx / kSlideProbe;
			slideZ = dz / kSlideProbe;
		}
	}
	if (bestSlope > kSlideMinSlope)
		move(slideX * std::min(bestSlope * kSlideGain, kSlideCap), slideZ * std::min(bestSlope * kSlideGain, kSlideCap));
	return seed;
}

double CombatArena::rollDropDistance(const state::Fighter& f, std::uint64_t id) const
{
	RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kDropStream), static_cast<std::size_t>(diceKey(id))),
		static_cast<std::size_t>(f.restRolls)));
	return kDropMin + (kDropMax - kDropMin) * dice.uniform01();
}

double CombatArena::runSpeed(const state::Fighter& f, bool laden) const
{
	knav::Gait gait = gaitOf(rules_.data(), f);
	gait.run = true;
	if (!laden)
	{
		gait.encumbrance = 1.0;
		gait.sneak = -1.0;
	}
	return knav::runSpeed(gait);
}

} // namespace ksim
