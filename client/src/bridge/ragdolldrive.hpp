#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "characters/physics.hpp"
#include "bridge/simview.hpp"
#include "kworld/ogreskeleton.hpp"

class Characters;
namespace ragdoll { struct Def; }
namespace kworld { class Heightmap; }

// Ragdolls from the sim's EVENT 48 (CLIENT_GOALS "Ragdolls and carried bodies" P3; PHYSICS.md 6): a KO, a death or a put-down starts a Jolt
// ragdoll from the body's animated pose at rest, the event's direction x strength kicks the named part, and the bodies drive the character's
// bones until the sim stops drawing it lying (wakes, is picked up). Bodies meet the world colliders and each other, so carried-out bodies pile.
// Settling (CLIENT_GOALS "Ragdolls" Settling): free fall, then a soft pull of the root body to the sim's rest point (CHAR_DYNAMIC flag bit 5:
// x, y, z + restYaw), reaching it by ~1.5 s, then sleep; a body already lying when it comes into view starts settled at its rest point and
// restYaw, with no fall. The get-up is blended by Characters::rise (anim.rise_blend); a race with no stand-up clip (an animal) by
// Characters::riseLive into the live pose. The .phs is the RACE record's own `male`/`female ragdoll` field, resolved by Characters.
// Carried bodies (P4): a character drawn carried becomes its race's `carry ragdoll`, started from its `carry me` pose on the shoulder; a servo on
// the root body holds it to that pose's place in the carrier's shoulder frame (soft in rotation, so it swings), stepped on the game clock with
// the rest. Past a lag cap the pull stiffens; a jump (teleport, load, re-entry) puts the whole body back on the shoulder. Put down (or the
// carrier falls), the same ragdoll drops and settles at the sim's rest point like a fresh KO.
// Budget (P5): ragdolls live only within the draw range (the interest radius round the camera; the player's own squad is sent from anywhere), and
// at most physics.ragdoll_cap of the lying ones are awake. A KO past either keeps the interim lying pose (no ragdoll); a settled body woken by a
// contact past the range, or over the cap (oldest first), is put back to sleep where it lies. Nothing is swapped or moved, so nothing pops.
class Ragdolls
{
public:
	Ragdolls(PhysicsWorld& world, StaticColliders& statics);
	~Ragdolls();
	// After the sim update (its cues are drained here), before Characters::update (which skins the driven pose). eye: the camera's ground point
	// (the budget's range).
	void update(SimView& sim, Characters& chars, const kworld::Heightmap* terrain, const double eye[3]);
	// The fixed physics step's hook (PhysicsWorld::stepFixed): the rest-point pull of every lying ragdoll, one 1/60 s step at a time, so its
	// servo never sees a whole game frame. A ragdoll whose root went non-finite is taken out here, before the step that would simulate it.
	void substep(SimView& sim, const kworld::Heightmap* terrain, double h);
	std::string report() const;	// script `dump ragdolls`
	void clear();	// EXIT GAME (end_session): every ragdoll out of the physics world and this session's state dropped (the .phs defs stay)
	// Where cues wait for a floor (a body on a building floor the colliders have not posted yet): StaticColliders::update posts round them.
	const std::vector<std::array<double, 3>>& waitingAt() const { return waitingAt_; }
private:
	struct Live
	{
		uint32_t rag = 0;
		std::vector<int> bones;	// def body -> skeleton bone
		std::string file;
		uint64_t started = 0;	// update count at the start
		double age = 0;	// game-speed seconds since the start (0 while paused)
		bool pulling = false;	// the rest-point pull has begun (logged once)
		double pullFrom[3] = {};	// the root's position where the pull began (its path to the target)
		bool settled = false;	// the rest-point pull is done (a late joiner starts settled)
		bool dead = false;	// the root went non-finite in a physics step: the pair is gone, the next update clears the character
		uint64_t carrier = 0;	// carried (P4): the carrier's sim id; 0 = lying
		std::vector<PhysicsWorld::Pose> hang;	// carried: every body's start pose (`carry me`) in the carrier's shoulder frame, def order
		bool anchored = false;	// carried: anchorP / anchorQ hold the carrier's shoulder this update
		double anchorP[3] = {};
		kworld::SkelQuat anchorQ;
		int dropping = 0;	// carried, the carry just ended: updates left to see the body lying (else the pair goes)
		int anchor = 0;	// carried: the def body pinned to the shoulder (the race's carry bone, else the root)
		uint64_t resets = 0;	// carried: times the body was put back on the shoulder (a jump of the carrier past kHangReset)
		double gap = 0, angle = 0;	// carried: the anchor's last distance (u) and rotation (deg) from its place (dump ragdolls): ~0, it is pinned
	};
	const ragdoll::Def* def(const std::string& path);
	std::string fileFor(uint64_t id, SimView& sim, Characters& chars, size_t index);	// the race's .phs; "" = none (logged once per race)
	// A lying state the client could not build, now carried by this body: SimView commits it as lying and the character's stride stops.
	void commitLying(SimView& sim, Characters& chars, uint64_t id);
	bool start(uint64_t id, SimView& sim, Characters& chars, const kworld::Heightmap* terrain, bool& retry);
	bool startLying(uint64_t id, SimView& sim, Characters& chars, const kworld::Heightmap* terrain, const double rest[3], float restYaw);
	bool pull(uint64_t id, Live& l, SimView& sim, const kworld::Heightmap* terrain, double h);
	bool startCarried(uint64_t id, uint64_t carrier, SimView& sim, Characters& chars);
	void hang(uint64_t id, Live& l, double h);	// the anchor pin's one fixed step (P4)
	std::vector<PhysicsWorld::Pose> hangPose(const Live& l) const;	// the hang pose at this update's anchor, world	// the rest-point pull's one fixed step (false: the ragdoll went non-finite)
	PhysicsWorld& world_;
	StaticColliders& statics_;
	std::map<std::string, std::unique_ptr<ragdoll::Def>> defs_;	// by resolved .phs path; null = failed to load (logged once)
	std::map<uint64_t, Live> live_;	// by sim id
	std::vector<std::pair<SimView::RagdollCue, int>> waiting_;	// cues retried next update (no pose yet, or no floor posted yet), tries left
	std::vector<std::array<double, 3>> waitingAt_;	// the waiting cues' node positions that need a floor
	std::map<uint64_t, uint64_t> carriedSince_;	// a character drawn carried and not yet a ragdoll: the update it was first seen so (its `carry me` pose is posed the next)
	std::set<uint64_t> seen_;	// characters seen standing this session: a body FIRST seen lying is a late joiner (started settled)
	std::set<uint64_t> farKO_;	// KOs cued past the range (P5): a late joiner once in range, though seen standing; cleared when the wire's rest point goes
	std::set<std::string> noRagdoll_;	// races whose RACE record names no ragdoll (logged once: no ragdoll, no lying pose)
	std::set<std::pair<std::string, std::string>> partLogged_;	// race, hit part name: a LOCATIONAL_DAMAGE name with no bone of its own
	std::string why_;	// the last start's reason to wait (logged when a cue runs out of tries)
	// the budget (P5): an id past the range or over the cap when cued: it keeps the interim lying pose until it is next seen standing
	bool overBudget(uint64_t id, SimView& sim, const double eye[3]) const;
	void budget(SimView& sim, const double eye[3]);	// puts settled bodies past the range, or over the cap, back to sleep
	uint64_t updates_ = 0, started_ = 0, refused_ = 0, late_ = 0, carried_ = 0, unbudgeted_ = 0, budgetSlept_ = 0;
};
