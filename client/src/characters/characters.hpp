#pragma once
#include <array>
#include <cmath>
#include <memory>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kdata/gamedata.hpp"
#include "kworld/ogreskeleton.hpp"
#include "assets/ogremesh.hpp"
#include "render/renderqueue.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "app/workpool.hpp"
#include "sokol_gfx.h"

// Skinned characters (docs/CLIENT_GOALS.md "Characters"): a RACE's body mesh posed by a clip of its skeleton (kworld/ogreskeleton.hpp), skinned on the GPU like the
// game's skin.hlsl (3 weights, a 3x4 matrix per bone in a uniform array) and shaded with its character.hlsl body shader (body + HEAD texture sets in one mesh)
// through the shared world lighting. Worn ARMOUR meshes (skin.hlsl main_fs) and the hair ATTACHMENT (hair_fs) skin against the body's skeleton like the game's
// shared skeleton instance; shirts are texture sets in the body shader's vest slots. Weapons are rigid meshes on the RACE's attachment points (.phs) or the
// hand bone, each its own palette slot. Not yet: morphs / bone scales, colourisation.
struct DdsImage;

class Characters
{
public:
	static constexpr int kMaxCascades = 4;	// Shadow::kCascades (static_assert in characters.cpp)
	static constexpr int kMaxPortraits = 64;	// renderPortrait() calls in one frame
	// One character. pos = feet on the ground (world), yaw in radians about +Y.
	// Animation notifies (type-105 ANIMATION_EVENT records, kenshi-h2v.29.6; findings anim_sfx_notifies.md): at fraction `at` of the clip the character posts
	// `event` (switch group / value when the record overrides one, `stop` an event to stop). Footsteps carry the Armor_Worn switch and a 200 u gate, the rest 1000 u.
	struct Note { float at = 0; std::string event, group, value, stop; };
	// A notify that fired this update: Characters::takeSounds hands them to the host, which owns the sound objects (the character's sim id) and posts them.
	struct SoundNote { size_t index = 0; std::string event, group, value, action; float gate = 0; };	// action: the technique / clip that fired it ("" for a locomotion layer)
	struct Spec
	{
		// The sim character this spec draws (SimView::specOf): the client's log lines print it beside the race, as the sim's own lines do
		// (`character 4679`). 0: not a sim character (the anim lab, the creator), and the lines then name the race alone.
		uint64_t id = 0;
		bool player = false;	// the player's own squad (the wire's kPlayer): the dev stand-in hold leaves it out (dev.standin_hold)
		std::string race = "Greenlander";
		bool female = false;
		std::string clip = "idle_stand_normal";	// a raw clip of the skeleton, all bones; used when anims is empty
		std::vector<std::string> anims;	// ANIMATION record names, layered (e.g. "walk lower" + "walk upper")
		// loco: the locomotion controller picks and blends the ANIMATION records for move, its forward speed. Not loco (seated, lying, carried):
		// clip / anims hold the pose. move alone cannot mark it: the sim backs a fighter away, and a negative speed is real motion.
		bool loco = false;
		float move = -1;	// locomotion: forward speed, 0 = idle; negative plays the clips in reverse (a body backing away)
		float wireMove = -1;	// the sim's own forward speed on the newest sample (the stop steer: its last-tick dead stop)
		float moveSim = 0;	// the drawn sample's own speed without the catch-up: the stop steer's decel (ds/stop-fix)
		float routeEnd = -1;	// the horizontal distance from the sim's newest position to its route's last waypoint, u; -1 = the wire has no route (kenshi-qaud.1: a speed drop far from the route's end is a corner, not a stop)
		float wireGap = 0;	// the drawn body's horizontal distance still to the newest sample, u (the stop steer's travel once the sim has stopped)
		float strafe = 0;	// locomotion, combat: lateral speed (the strafe split)
		bool strafeDrive = false;	// a direct-drive strafe is held: the strafe split runs out of combat too (k16.2.237.8)
		bool combat = false, stealth = false;	// locomotion state filters (`is combat mode`, `stealth mode`)
		float legs[2] = { 100, 100 };	// locomotion: right, left leg values (100 x (health - stun) / (max - wear), <= 100; LOCOMOTION.md; 100 = healthy, kinspect legs_anim.md) against the records' leg damage windows
		std::string stance = "idle_stand_normal";	// the stored idle stance (brave: idle_stand_guard, fearful: idle_stand_timid)
		float posture = 35, shoulderSet = 68, neckPosition = 53;	// editor sliders (0..100; defaults: the human ranges' mids): posture clips
		// seated / asleep in furniture: the clip is the whole body at the node (kinspect bed_sleeper_height.md). The posture clips are NOT layered on:
		// the live sleeper's Bip01 is the clip's own root (+6.66), not our postured one (+6.36 = dragged down by the default sliders).
		bool posed = false;
		std::vector<std::pair<std::string, float>> shape;	// editor sliders by name (raw values, Height 100); the rest at the race xml's mid
		float muscle = 0, skinny = 0;	// body condition (from stats in the game): muscle m (0..1, unclamped), skinny 0..1
		float strong = 0;	// share of the strong normal map 0..1 (stats in the game; docs/reference/BODY_SHAPE.md)
		// locomotion: an action on layer 1, full body: a COMBAT_TECHNIQUE (its anim name), an ANIMATION record (stumbles) or a raw clip
		std::string action;
		float actionTime = 0;	// seconds since it started
		float actionSpeed = -1;	// clip time per second; < 0: the technique's anim speed mult (1 for the others)
		float aimYaw = 0;	// the wire's aim tail while a technique plays (its clip root's turn, fixed at its start); valid when hasAim
		bool hasAim = false;
		std::vector<std::pair<std::string, float>> wounds;	// locomotion: LOCATIONAL_DAMAGE part name -> health 0..1 (the pain overlay)
		float time = 0;		// clip time in seconds (wraps); locomotion: seconds already walked
		float speed = 1;	// clip time per second (0 = frozen pose)
		int head = 0;		// index into the RACE's heads male / heads female
		float skin = 0.5f;	// 0..1 along the editor's "Skin Tone" colour range
		std::string hair;	// ATTACHMENT record name ("" = none)
		std::string beard;	// ATTACHMENT record name: a head texture only (Beard13), in the hair colour
		std::string hairColour = "Hair Colour Brown";	// COLOR_DATA record name
		std::string faction;	// FACTION record name: its `color` (COLOR_DATA) tints worn armour that has no colour of its own
		std::vector<std::string> wear;	// ARMOUR record names
		std::vector<std::string> weapons;	// WEAPON / CROSSBOW record names, "NAME/MATERIAL_SPECS_WEAPON" picks the model; [0] at the hip, [1] on the back
		bool drawn = false;	// weapons[0] in the hand (its bare sword), the sheath left at the hip
		std::string tool;	// ITEM record name held in the right hand (a producer's functionality `special tool`: Pickaxe, Hoe); "" = none. The caller sheathes the weapon (drawn = false) while it is held
		bool loaded = false;	// crossbows hold a bolt (the drawn string and the live ammo), else the slack string
		double pos[3] = {};
		float yaw = 0;
		int carry = 0;	// carrying a body: 1 on the left shoulder, 2 on the right (the carry clips; the body names this character in carriedBy)
		int carriedBy = -1;	// being carried by the character at this index (added earlier): plays `carry me`, pinned to the carrier's upper arm; pos / yaw unused
		int severed = 0;	// severed limbs, bit = limb slot: 0 left arm, 1 right arm, 2 left leg, 3 right leg (kinspect cq_sever_visual.md)
		// The animation lab's root lock: the pose zeroes Bip01's x/z and the caller's node carries them (Characters::rootOffset), as the game
		// zeroes a relocates record's or an action's root (COMBAT_ANIM.md section 5). Only the lab sets it.
		bool rootInPlace = false;
	};

	// A character editor slider (data/editor/editor_data_*.xml <Range>): morph sliders name a pose `target` (and `target_opposite`); category: its
	// <Category> ("Face", "Body"); variation: `random_variation` (0 = none); group: `random_group` (-1 none)
	struct EditorRange { std::string name, target, opposite; float min = 0, max = 0, mid = 0; std::string category = {}; float variation = 0; int group = -1; };
	// For the character creator: a RACE's editor choices. ok = false for an unknown race. Heads per sex; hairs / hair colours: the RACE's `hairs` /
	// `hair colors` refs (beards are the hairs without a mesh); ranges per sex (empty without an `editor limits` file).
	// beards: the `hairs` entries on `attach slot` 13 (hair: 2). hairWeights / beardWeights: their val0, parallel to hairs / beards (RANDOMISE picks by them,
	// 100 each); hairNone / beardNone: the weight of the race's empty hair / beard record (no mesh, no head texture: Bald, No beard 300), shown as the
	// picker's "none" and left out of the lists; 0 when the race has none.
	// hairColourWeights: the `hair colors` val0 (0 counts as 100), parallel to hairColours.
	// hp / skills / noBleeding: the editor's RACE STATS panel: "Hit Points", the skills whose race multiplier is not 1 (the greens first, in the
	// panel's order) and the "No bleeding" row.
	// skinStops: the <Colour> count of the gender's "Skin Tone" ColourRange: the editor's Skin Tone slider runs 0..10*(skinStops-1), integer tenths
	// of a step between the swatches (parse 140077090, sampler FUN_14006e340; kinspect rwh47_skin_tone_scale_static.md); 0 without an editor file.
	struct RaceChoices { bool ok = false, playable = false; std::string description; int heads[2] = {}; std::vector<std::string> hairs, beards, hairColours;
		std::vector<float> hairWeights, beardWeights, hairColourWeights;
		float hairNone = 0, beardNone = 0;
		float hp = 0; std::vector<std::pair<std::string, float>> skills; bool noBleeding = false;
		std::vector<EditorRange> ranges[2]; int skinStops[2] = {}; };
	RaceChoices choices(const std::string& race) const;
	std::vector<std::string> races(bool playableOnly) const;	// RACE names with a body mesh (and an editor file when playableOnly)
	// The creator's race / subrace pairs: RACE_GROUP name -> its playable races with an editor file (the editor's "Human" -> Greenlander, Scorchlander)
	std::vector<std::pair<std::string, std::vector<std::string>>> raceGroups() const;
	const std::vector<std::string>& idleStances() const { return idleStances_; }	// the creator's Idle stance slider, 1-based: ANIMATION names

	// Main thread, after sg_setup. gd: merged game data, kept only for the call (the records a character can use are copied out).
	// gpuNow false (a set made mid-game on the logic thread, the anim lab's, kenshi-k16.2.205.12.69): the pipelines, samplers, fallback textures and
	// the frame set are one frame-lane job (renderqueue.hpp) instead of made in the call; nothing may draw the set before that job's packet.
	void init(const ResourceIndex* res, const kdata::GameData& gd, bool gpuNow = true);
	void shutdown();
	// False with err set when the race, its mesh, skeleton or clip cannot be loaded. A missing armour / hair is a warning, the character is still added.
	// notReady: the race body's decode is still on the workers -> false, err empty, *notReady true (not an error: add it again next frame). Null: wait for it.
	bool add(const Spec& s, std::string& err, bool* notReady = nullptr);
	// Texture decode off the main thread (kenshi-k16.2.205.10: a 4096^2 armour .dds is 15-28 ms of file read + parse, the GPU upload 3-6). prefetch queues the
	// character's .dds files for the worker threads (idempotent, nothing that is already a view); texturesReady is true once none of them is still decoding.
	// texture() takes a finished decode instead of reading the file, so add() on a ready character spends only the uploads. Both are optimisations: a file
	// the enumeration missed still loads synchronously in add(). Main thread only.
	void prefetch(const Spec& s);
	void prefetchRace(const Spec& s);	// the race body + its skin and head textures only (a first roster's races, before its build)
	bool texturesReady(const Spec& s);
	bool gearPending(const Spec& s, bool queue);	// the armour/hair/weapon/stump mesh decodes: queued when queue, true while one is still running
	// One step of a race's first build ahead of its first character's add (kenshi-k16.2.205.12.77): the race body once its decode and skin / head
	// textures are in, then s's decoded skinned gear meshes for that body, then a race-only build of s (no gear: its morph bake and its contacts)
	// that is not kept. False when there was nothing to do now (not decoded yet, or the race is warm). Never waits on a worker. Main thread only.
	bool warm(const Spec& s);
	void clear() { chars_.clear(); }
	// keeps the characters at old indices from[], in that order (their animation state intact), drops the rest. departing: old indices whose character
	// left the drawn set this frame but should dither out, kept (frozen, render.char_fade) after the kept ones; they take the next indices, from.size() + j.
	void keep(const std::vector<int>& from, const std::vector<int>& departing = {});
	// The arrival / departure dither (render.char_fade, kenshi-k16.2.258.10.1): a character that first appears in the drawn set dithers in over that
	// many s and one that leaves dithers out over the same, so nothing pops a whole body in one frame. departDone: a departing character reached 0
	// and can be dropped (the next keep() does); takeDepartDone clears it. cover(i): the drawn fraction 0..1 (1 = whole).
	bool takeDepartDone() { return std::exchange(departDone_, false); }
	float cover(size_t i) const { return chars_[i].cover; }
	bool departing(size_t i) const { return chars_[i].departing; }
	bool replace(size_t i, const Spec& s, std::string& err);	// rebuilds character i from s (same index); false with err set, the old one kept	// the characters only; loaded meshes and textures stay cached
	// KO / death (kinspect ko_transitions.md: the original ragdolls, there is no fall clip; the real ragdoll is parked, CLIENT_GOALS "Ragdolls"). MINE:
	// fall() rebuilds character i from the lying spec s but cross-fades the pose it had into the lying clip over cfg `anim.fall_blend` (0 = the snap).
	// rise() plays the game data's ANIMATION "standing up 3" once from the lying pose; SimView hands the character back to locomotion at its end.
	// riseLive() is the same get-up for a race with no stand-up clip (an animal, whose lying pose is its ragdoll): the character goes straight to the
	// live spec s and the ragdoll's last pose cross-fades into it over cfg `anim.rise_blend` (the pose layer `rise()` blends, without a clip).
	bool fall(size_t i, const Spec& s, std::string& err);
	bool rise(size_t i, std::string& err);
	bool riseLive(size_t i, const Spec& s, std::string& err);
	bool rising(size_t i) const { return chars_[i].gettingUp; }	// the get-up clip is still playing
	// The get-up's cross-fade is still running (anim.rise_blend): SimView keeps the clip through it even when the sim starts walking (a stand-up
	// moves the body, which reads as a walk), so the ragdoll's pose is never snapped away.
	bool risingBlend(size_t i) const { return i < chars_.size() && chars_[i].riseDur > 0 && chars_[i].riseT < chars_[i].riseDur; }
	// Ragdoll (CLIENT_GOALS "Ragdolls" P3): a body drives one bone with a world transform; the other bones follow their parents with the pose
	// the layers give them. ragdollStart fills p / q of each entry from this frame's pose (the ragdoll starts there); setRagdoll replaces the
	// driven set (empty: animation again). Sizes and scales stay the animated ones.
	struct RagBone
	{
		int bone = -1;
		double p[3] = {};
		kworld::SkelQuat q;
	};
	bool ragdollStart(size_t i, std::vector<RagBone>& io) const;
	// A late joiner's start pose (CLIENT_GOALS "Ragdolls" Settling): ragdollStart's bones turned about the node to `yaw` and moved to `at`
	// (the sim's rest point), so a body already lying when it comes into view starts at its rest yaw.
	bool ragdollStartAt(size_t i, std::vector<RagBone>& io, double yaw, const double at[3]) const;
	const kworld::Skeleton* skeleton(size_t i) const { return i < chars_.size() && chars_[i].body ? &chars_[i].body->skel : nullptr; }
	// The ragdoll the character's race names (RACE `male ragdoll` / `female ragdoll`), resolved to a file; "" when the record has none (no
	// hard-coded fallback: the original's race record is the only source). The path is the record's own, so a mod's ragdoll is used as-is.
	std::string ragdollFile(size_t i) const;
	// A carried animal's anchor bone name (RACE `carry bone`, else the skeleton's root bone).
	std::string carryBone(size_t i) const;
	// A carrier's shoulder this frame (Characters::carryPlace's bone, `Bip01 L/R UpperArm` by Spec::carry): world position and orientation.
	// False when i carries nothing or has no pose yet. The carried ragdoll hangs from it (P4).
	bool carryFrame(size_t i, double p[3], kworld::SkelQuat& q) const;
	void nodePos(size_t i, double out[3]) const { for (int k = 0; k < 3; k++) { out[k] = chars_[i].pos[k]; } }
	// The animation lab's root lock: the root bone's x/z the last pose applied (model space; 0 with no body). The lab adds it to the node, and
	// Spec::rootInPlace has zeroed it in the pose, so the body stands where the clip's root would have put it.
	void rootOffset(size_t i, double out[2]) const { out[0] = out[1] = 0; if (i < chars_.size()) { out[0] = chars_[i].rootXZ[0]; out[1] = chars_[i].rootXZ[1]; } }
	void setRagdoll(size_t i, std::vector<RagBone> v);
	size_t size() const { return chars_.size(); }
	// Live driving (the sim link, simview.cpp): per-frame state of character i. pos, yaw, move, strafe, combat, stealth and legs take effect at the next update().
	Spec& live(size_t i) { return chars_[i].spec; }
	std::vector<SoundNote> takeSounds() { std::vector<SoundNote> out; out.swap(sounds_); return out; }	// the notifies fired since the last call
	// Notify coverage (kenshi-h2v.29.6): ANIMATION records / techniques carrying notifies, and the notes in all
	std::string noteReport() const;
	// The Wwise switch values a notify posts with: Armor_Worn (the heaviest armour class worn, 0 Cloth .. 3 Plate; UNSURE: the game's char+0x68c is
	// not traced beyond "Cloth/Leather/Chain/Plate") and Weapons (the held weapon's category, CharacterHuman::vf123 1405dbf80's table; "" = unarmed, none set)
	std::string armourSwitch(size_t i) const;
	std::string weaponSwitch(size_t i) const;
	// Replaces character i's playing action with `name` (technique / ANIMATION / clip; "" = none), played forward t seconds. Locomotion characters only.
	bool action(size_t i, const std::string& name, float speed, float t, bool loop, std::string& err);
	void endAction(size_t i);	// a looped action fades out
	std::string nameOf(const std::string& sid) const;	// record name of a stringID (the sid itself when unknown)
	// The name a client log line gives a character: "<id> <race>" (the sim's form, id first), as `foot lock: 41253 Greenlander: ...`; the race
	// alone when the spec is not a sim character. With no race in the line, `idTag` is the id alone, " 41253" ("" when there is none).
	static std::string label(const Spec& s) { return s.id ? std::to_string(s.id) + " " + s.race : s.race; }
	static std::string idTag(const Spec& s) { return s.id ? " " + std::to_string(s.id) : std::string(); }
	bool equippable(const std::string& sid) const { return equippable_.count(sid) != 0; }	// a WEAPON or ARMOUR record (SIMLINK order 62)
	// What the grid inventory draws of an item record (ITEM / WEAPON / ARMOUR / CONTAINER / CROSSBOW / MAP_ITEM / LIMB_REPLACEMENT): `inventory
	// footprint width / height` (at least 1 x 1), kind (1 weapon or crossbow, else the ARMOUR / CONTAINER `slot` AttachSlot, 15 for the back slot
	// (AttachSlot 1, which has no grid here), 0 plain), the record's `icon` file (ui::icons::recordPath's input)
	struct ItemInfo
	{
		int w = 1, h = 1, kind = 0; std::string icon;
		// The Inventory_Items switch value of its inventory move sounds (audio_sfx_triggers.md section 5, kenshi-inspect rwh65/switches.tsv): ITEM /
		// CONTAINER / MAP_ITEM the record's `inventory sound`, ARMOUR its `material type`; empty: none (weapons, limbs: not established)
		std::string soundSwitch;
		bool plain = false;	// an ITEM record (its value is the price; weapons and armour scale with grade and material)
		int function = 0, value = 0;	// ITEM `item function` (3 food, 0xd book, 0xf food for some races, 0x10 ammo), `value`
		float weight = 0;	// `weight kg`
		std::string description;
		std::vector<std::string> ingredients;	// the `ingredients` refs' names (crafting; an item with them is priced from them, not from `value`)
		// CONTAINER (backpacks): `encumbrance effect`, `combat skill bonus`, `athletics mult`, `combat speed mult`, `stealth mult`
		float encumbrance = 1, athletics = 1, combatSpeed = 1, stealth = 1;
		int combatSkill = 0;
		float charges = 1;	// max(`charges`, 1); food x CONSTANTS `food quality mult` (= its nutrition); food with charges <= 1 is demoted to function 0
		int price = 0;		// ITEM: FUN_140896e20 with the mults (the tooltip's Avg. Price): from `ingredients` when it has them, else `value` x the price mults
		// WEAPON: `cut damage multiplier`, `blunt damage multiplier`, `bleed mult`, `armour penetration`, the robot / human / animal damage mults
		float cut = 0, blunt = 0, bleed = 1, penetration = 0, vsRobot = 1, vsHuman = 1, vsAnimal = 1;
		int attack = 0, defence = 0, indoors = 0;	// `attack mod`, `defence mod`, `indoors mod`
		int shootSound = -1;	// CROSSBOW `shoot sound` (the Weapons switch of a shot: 0 Crossbow, 1 Harpoon, 2 Crossbow_Turret, 3 Harpoon_Turret); -1 none
		int skillCategory = -1;	// WEAPON `skill category` (the class line: 0 Katana ... 8 Polearm); -1 none
		// ARMOUR: `class` (ArmourClass 0 cloth ... 3 heavy; -1: not armour), `material type` (MaterialType 0 cloth ... 3 metal plate), `part coverage`
		// (the LOCATIONAL_DAMAGE name and v0, percent), in the record's order
		int armourClass = -1, material = 0;
		int unarmedBonus = 0, perceptionBonus = 0;	// ARMOUR `unarmed bonus`, `perception bonus` (the tooltip's [Skills] rows)
		// ARMOUR `ranged skill mult`, `dodge mult`, `damage output mult`, `asassination mult` (sic), `fist injury mult`: the [Skills] rows the
		// loader leaves as in the record (kinspect s66_static.md part B)
		float rangedMult = 1, dodgeMult = 1, damageMult = 1, assassinMult = 1, fistMult = 1;
		std::vector<std::pair<std::string, int>> coverage;
	};
	// WEAPON_MANUFACTURER `cut damage mod` / `blunt damage mod` and MATERIAL_SPECS_WEAPON (a weapon model) `attack mod` / `defence mod`, by stringID
	struct ModInfo { float cut = 1, blunt = 1; int attack = 0, defence = 0; std::string description; };	// description: WEAPON_MANUFACTURER `company description`
	const ModInfo* modInfo(const std::string& sid) const { const auto i = mods_.find(sid); return i == mods_.end() ? nullptr : &i->second; }
	const ItemInfo* itemInfo(const std::string& sid) const { const auto i = items_.find(sid); return i == items_.end() ? nullptr : &i->second; }
	void hull(const std::string& race, float out[3]) const;
	// The routing body of a race as ksim's CombatData::body has it (the route preview, kenshi-k16.2.331.2): clearance radius and water cost scale; false: the race is not in the data
	// (out stays the default radius 2.6, no water scale)
	bool pathBody(const std::string& race, double& radius, float& water) const;
	int limbSlot(const std::string& race, const std::string& part) const;	// the limb slot (0 left arm, 1 right arm, 2 left leg, 3 right leg) of a body part, by its Forearm / Calf bone; -1 none
	float partMaxHealth(const std::string& race, const std::string& part) const;	// the RACE `combat anatomy` health (v1) of a body part; 0 unknown	// RACE `hull size X/Y/Z`, the click box (5, 20, 5 when the race is unknown)
	// The voice switches of a race (audio_sfx_triggers.md section 8; CharacterHuman::vf128 / CharacterAnimal::vf128). A humanoid: VO_Races =
	// Human_Male ... Robot_Female. An animal (`sounds` >= 4): Creature_VO and Weapons = Beak ... Turtle, and its voice events are the VO_Creature_* ones.
	// No switches when the race is unknown.
	struct Voice { std::vector<std::pair<std::string, std::string>> switches; bool animal = false; };
	Voice voiceOf(const std::string& race, bool female) const;
	// The character's race is an animal (`sounds` >= 4): the only race class whose carried body is a ragdoll (rwh.98 5; a humanoid is pose + bone pin)
	bool animal(size_t i) const;
	bool isWeapon(const std::string& name) const;	// a WEAPON / CROSSBOW record name
	// debug (dump feet): per foot (0 left, 1 right) the lowest skinned vertex of the body mesh's foot and toe bones this frame, world; false: no body
	bool soles(size_t i, double out[2][3]) const;
	// debug (log feet): per foot (0 L, 1 R) the heel [0] and ball [1] sole points this frame, world; false: no body or no fit
	bool solePoints(size_t i, double out[2][2][3]) const;
	// The animation lab (CLIENT_GOALS "Animation lab"): the ANIMATION records as (name, clip) in file order, every layer set to clip time t (a loop
	// wraps, a one shot clamps) with the pose re-baked, and the live pose of character i gathered into `out` camera-relative to cam (portraitCopy,
	// logic side, at the packet's fill; renderPortrait(out, ...) draws it on the render side, kenshi-k16.2.205.12.67).
	std::vector<std::pair<std::string, std::string>> animationRecords() const;
	bool setClipTime(size_t i, float t);
	struct FrameData;	// below (the instanced character draw)
	bool portraitCopy(size_t i, const double cam[3], FrameData& out);
	float phase(size_t i) const { return chars_[i].phase; }	// log feet: the shared synch phase (cycles)
	float poseStep(size_t i) const { return chars_[i].poseStep; }	// log feet: this pose's largest local joint rotation step, deg
	// debug (dump bones): the drawn body node and a bone's world position this frame (the node plus the bone's model-space offset, rotated by it)
	void nodeWorld(size_t i, double out[3]) const;
	bool boneWorld(size_t i, const char* name, double out[3]) const;
	// The character editor's preview assembly (kenshi-k16.2.433.1, main.cpp creator_frame): character i is drawn where the creator builds it and its
	// node is then p' = center + q x (p - center) + shift, q x orient, by every update() until clearPreviewOrbit. The creator turns the character AND
	// the platform (which draws with the same q) about the pivot it frames, so the two stay one assembly as the original's charedit_chars_rotation
	// node does. center is the editor scene's pivot (a home-space point near the character, so the products stay small); shift is its world move.
	void setPreviewOrbit(size_t i, const double center[3], const kworld::SkelQuat& q, const double shift[3]);
	void clearPreviewOrbit() { previewOrbit_ = false; }
	// The creator's wheel pick: the point of the ray nearest a bone of character i's drawn skeleton within `radius`, in front of the eye. That point
	// lies on the ray, so moving the eye along the ray to it keeps it under the cursor. False: the ray misses the model.
	bool skeletonRay(size_t i, const double origin[3], const double dir[3], double hit[3], double radius) const;
	std::function<double(double, double)> ground;	// the drawn ground (x, z): keeps feet out of slopes; unset = no clamp
	float rootShift(size_t i) const { return chars_[i].rootShift; }	// dump feet
	float soleLift(size_t i) const { return chars_[i].soleLift; }	// dump feet
	std::string fitReport(size_t i) const;	// dump feet: the sole fit and the slope lift
	// kenshi-k16.17.7.18 (scene JSON): the settled idle fit over every character: how many stand in the idle stance (both feet planted by the fit), the
	// largest ground-height difference between a character's feet (u) and the largest ground-slope weight
	struct IdleStats { int engaged = 0; double feetDelta = 0, slopeW = 0; };
	IdleStats idleStats() const;
	// kenshi-k16.17.7.4 (dev rest-pose metric): the stance, both knee angles (deg between thigh and calf, 0 = straight) and the pelvis's height over the sim's ground, one line
	std::string restLine(size_t i) const;
	std::string contactReport(size_t i) const;	// dump contacts: the lower clips' sole contact windows and stride
	std::string idleLines() const;	// debug (dump idle): per character (scene and --character ones too) its weapon category, combat / stealth and the idle record held
	std::string animSummary(size_t i) const;	// debug (dump anim): the clips playing above 5% weight, "clip weight"
	// debug (log feet): this frame's locomotion state — the stride warp and the stop steer applied, the stop latch, the get-up, the lying fade
	struct LocoState { bool warped = false, steered = false, stop = false, rising = false; int lie = -1; int action = -1; bool done = false; int lock[2] = {}; float drop = 0; bool footLock = false, footTerrain = false, onTerrain = false; float simY = 0, drawnY = 0, soleLift = 0, rootShift = 0; double terrainY = 0; };	// lock: the foot lock per foot (0 none, 1 held, 2 released past its reach); drop: the pelvis offset this frame (footLift)
	LocoState locoState(size_t i) const;
	float drawnYaw(size_t i) const { return 2.0f * std::atan2(chars_[i].orient.y, chars_[i].orient.w); }	// the body node's yaw as drawn (c.orient), for the log gait dump
	// debug (log feet, actroot): anim.action_root's state this frame on a character drawn along an action clip's root — the clip root's replay
	// point (world x/z), the wire and the drawn body, the eased correction and the clip's own time over its length. False: not on the path.
	struct ActionRoot { double path[2] = {}, wire[2] = {}, drawn[2] = {}, corr[2] = {}, root0[2] = {}; float clipTime = 0, length = 0; std::string clip; };
	bool actionRoot(size_t i, ActionRoot& out) const;
	std::string clipReport(size_t i) const;	// debug (dump bones): each layer's clip name, time over its length, weight; the postures
	const float* bloodColour(size_t i) const { return chars_[i].body ? chars_[i].body->bloodCol : nullptr; }	// RACE `blood colour` (RGBA), null: no body

	// dt = the animation clock (0 while a scripted run freezes the poses or the sim pauses); fadeDt = the frame's own dt, which the loading
	// stand-in's clocks run on (a visual, wall-clock transition: it must finish even when the poses are held; kenshi-k16.2.258.1); < 0 = dt
	void update(double dt, double fadeDt = -1);
	// render.loading_standin: silhouette (default: a flat black body, unlit, no textures: it reads as loading, never as an identity), double (the
	// race body in a plain neutral material) or off (a build whose GPU objects are not ready is not drawn, as before kenshi-k16.2.258.1)
	enum StandinMode { kStandinOff = 0, kStandinSilhouette = 1, kStandinDouble = 2 };
	bool standinActive() const;	// a character this frame still stands in or is inside its two-stage switch: the scene test's load wait (world_loading)
	static constexpr float kStandinShape = 0.25f;	// s, stage 1: the body's shape morphs from the race body into this build's, still in the stand-in's material
	static constexpr float kStandinColour = 0.2f;	// s, stage 2: the real look dithers in (the stand-in's dithered out over the same s)
	// ---- the instanced character draw (kenshi-k16.2.205.8) ----
	// One instanced draw: the mesh's buffers and index count and every texture view it binds (unused: 0), resolved when the frame data is built (its
	// Handles are ready then: drawable()), and its run in the instance list. Values only: no pointer into the characters' state.
	struct Group
	{
		sg_buffer vbuf = {}, vbuf2 = {}, ibuf = {};	// vbuf2: the stand-in pass only (the race base body)
		int indexCount = 0;
		sg_view v[12] = {};
		uint32_t first = 0, count = 0;
	};
	struct Plan
	{
		std::vector<Group> body, armour, hair, items, standin, shadow[kMaxCascades];
		uint32_t draws() const;
	};
	// The frame data (render hook 7, kenshi-k16.2.205.12.26; the 7a pattern): the CPU half of a frame's character draw, built by prepare() in the logic
	// half and carried in the frame packet; upload() writes it into the GPU buffers on the render side and the render*() calls draw its plan. skins:
	// kSkinRows vec4 rows per character (palette, origin, blood, bone parts); insts: kInstRows rows per (character, part) record; list: the (skin row,
	// record) pairs, per pass grouped by (mesh, material). A character whose build is not drawable yet (its body, mesh or a texture still in the
	// stream lane, or its body not ready: Characters::body) has no entry: it is never drawn half-built.
	struct FrameData
	{
		std::vector<float> skins, insts;
		std::vector<uint32_t> list;
		Plan plan;
		size_t count = 0;	// the characters (size()) when it was built
		void clear() { skins.clear(); insts.clear(); list.clear(); plan = Plan{}; count = 0; }
	};
	// Once per frame in the logic half, after update() and the shadow cascades' update() (kenshi-k16.2.205.8, .12.26): the frame's skinning palettes and
	// per-part records, and the visible parts grouped by (mesh, material) per pass (the main view, the items, each shadow cascade) into instanced draws,
	// into `out` (every field, every call: no characters leaves it empty). cam = the camera position every render*() of the frame is given;
	// cascades[i] = the shadow cascade i's box (null: not rendered this frame). Also fills drawState(). No sokol call.
	void prepare(const double cam[3], const ShadowCascade* const cascades[kMaxCascades], FrameData& out);
	// Render side, once per drawn frame before the first character draw: the packet's frame data into the frame's GPU buffers (and the portrait sets
	// handed back). Legal twice on the same packet.
	void upload(const FrameData& fd);
	// Depth only, inside the shadow pass after selectTile(): cascade `index`, the one prepare() was given.
	bool renderShadow(int index, const ShadowCascade& c);	// whether anything was drawn (the shadow cache restores the tile only after a cascade that drew)
	void render(const float mvp[16], const double cam[3], const LightParams& light);
	// The weapons and items on the characters' attachment points (kenshi-k16.2.101.1): its own GPU section in the scene test (render()
	// leaves the mounted parts out). Draw it right after render().
	void renderItems(const float mvp[16], const double cam[3], const LightParams& light);
	// The portrait of character i (RTT_Portrait 14084b890), logic half (render hook, kenshi-k16.2.205.12.67): a copy at its own position but
	// unturned, no weapons, posed in its idle stance at time 0 plus its posture clips, gathered into `out` camera-relative to the eye (the same
	// buildSet as prepare(), on the portrait set's capacities). Its camera target and position out (world): Bip01 Head + the RACE `portrait offset`,
	// and the target + yaw x pitch x (0, 0, `portrait distance`). False (out cleared): no such character, not drawable yet or no head bone. No sokol
	// call; the copy is a local, so the render side never reads the character. Draw it with renderPortrait(out, ...).
	bool portraitFrame(size_t i, float yaw, float pitch, double target[3], double eye[3], FrameData& out);
	float headHeight(size_t i);	// Bip01 Head above the character's feet in its idle stance, 0 when there is none (the creator's FACE / HAIR camera)
	// Render side: a portraitFrame()'s data into a portrait buffer set of its own, drawn unshadowed (the portrait scene has no shadows)
	void renderPortrait(const FrameData& fd, const float mvp[16], const double cam[3], const LightParams& light);
	// renderPortrait() takes a small buffer set of its own per call (a GPU buffer written once in a frame cannot be written again after it is bound):
	// false once this frame's sets are all taken. upload() hands them back.
	bool portraitReady() const { return portraitUsed_ < kMaxPortraits; }
	// A Characters that draws only portraits (the anim lab's own) never runs upload(): its frame calls this before its renders instead.
	void portraitsNewFrame() { portraitUsed_ = 0; }
	static constexpr int kPortraitSkins = 2, kPortraitInsts = 64, kPortraitList = 128;	// a portrait set's capacities (buildSet's caps, makeSet's sizes)

	const Shadow::FrameData* sfd = nullptr;	// this frame's shadow data (the packet's; the render side points it there at the start of the draw)
	std::function<bool(uint64_t id, const double pos[3])> culled;	// a character standing there is not drawn, nor its shadow (a floor the building cut hides; main.cpp char_cut_hidden: the wire's floor over the shown level, one decision with the name tags and labels, k16.2.264.1)
	// Why a character in the world frame's set has no body drawn (kenshi-k16.2.264.1, the user's "went indoors and lost its character model" with its
	// name tag still drawn): one byte per Characters index, filled by every main-view buildSet (the portrait passes leave it alone). The run file's
	// `nobody` line and the dev `log pos` frame line read it; it is one frame behind a character's own sync.
	enum DrawState : uint8_t { kNotInSet = 0, kBodyDrawn = 1, kNoModel = 2, kFloorCut = 3, kBuffersFull = 4, kStandin = 5 };
	DrawState drawState(size_t i) const { return i < drawState_.size() ? (DrawState)drawState_[i] : kNotInSet; }
	size_t drawStates() const { return drawState_.size(); }	// characters the last main-view set covered (before the first prepare: 0)
	static const char* drawStateName(DrawState s);

private:
	// GPU objects are made on the render thread (docs/RENDER_THREAD.md §7.2, kenshi-k16.2.205.12.34): the logic half reads the files, builds the
	// vertices and decodes the pixels, and queues a stream-lane job that makes the buffers or the image + view and publishes them into a Handle.
	// The draw reads get(); a character is drawn only once every Handle it binds is ready() (drawable).
	using Tex = rq::Handle<sg_view>;
	struct Buffers { sg_buffer vbuf = {}, ibuf = {}; };
	// A skinned mesh on the GPU, its bone indices in the body skeleton's handles.
	struct GpuMesh
	{
		rq::Handle<Buffers> gpu;
		int indexCount = 0;
	};
	// A named point a rigid item hangs from: the bone and the item's bind-pose model transform (column-major).
	struct AttachPoint
	{
		std::string name;
		int bone = -1;
		float model[16];
	};
	// A race + gender body: its mesh, skeleton, inverse bind matrices and texture sets.
	// the race-specific rest of a body (char.body.rest): the editor xml's skin range and sliders, the attachment points; read on a prefetch worker
	// per race variant of a body job (kenshi-k16.2.205.12.81). ok false: the attachment file is not found
	struct Rest { int skinStops = 0; float skinRange[8][3] = {}; std::vector<EditorRange> ranges; std::vector<AttachPoint> attach; bool ok = true; };
	struct Body
	{
		GpuMesh mesh;
		kworld::Skeleton skel;
		std::vector<std::array<float, 16>> invBind;
		Tex tex, nrm, mask;
		Tex nrmSkinny, nrmStrong;	// RACE `nm <sex> skinny` / `strong` (the plain one when absent)
		std::vector<std::array<Tex, 3>> heads;	// texture, normal, mask per HEAD ref
		float skinRange[8][3] = {};	// editor "Skin Tone" colours 0..1
		int skinStops = 0;
		std::vector<AttachPoint> attach;	// the RACE's attachment points file
		ogre::Mesh src;	// for the morph bake
		std::string meshFile;
		std::vector<EditorRange> ranges;	// the race xml's sliders for this gender
		std::vector<std::string> parts;	// body part names by index (RaceInfo::anatomy)
		std::vector<float> bonePart;	// per skeleton bone: its body part index
		float bloodCol[4] = {}, bloodScale[2] = {};
		float portrait[4] = {};	// RaceInfo::portrait
		bool portraitSet = false;	// RaceInfo::portraitSet
		std::vector<uint16_t> partBits;	// the RACE part map as partData.y bits per pixel (140071f90), rows top first; empty: none
		std::string stump[4];	// per limb slot: the RACE `limb replacement` stump mesh for this sex (LIMB_REPLACEMENT `slot` 50 + limb)
		int partW = 0, partH = 0;
		// the foot log's sole points (MINE, fitted to the mesh at load): per foot (0 L, 1 R) the heel under the Foot bone and the ball under
		// Toe0, as offsets in that bone's bind frame; bone -1: none
		int soleBone[2][2] = { { -1, -1 }, { -1, -1 } };
		kworld::SkelVec3 soleAt[2][2];
		// the foot lock's leg chain, per foot (0 L, 1 R): Thigh, Calf, Foot; -1: none
		int legBone[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };
	};
	enum PartKind { kArmour, kHair };
	struct Part
	{
		PartKind kind;
		const GpuMesh* mesh;
		Tex tex, nrm;	// hair: tex only
		float diffuseCh[4];	// hair: the brightness channel
		float clipCh[4];	// the cut-out channel (armour: normal alpha, hair: its alpha channel)
		float clipMin;
		float colour[3];	// hair
		float spec;	// armour glossMult, hair specularMult
		float mipBias = 0;	// hair
		Tex cmap = {};	// armour: the colour map (r, g = color1 / color2 coverage, b = metalness); none = no tint, no metal
		float col1[3] = {}, col2[3] = {};	// armour: COLOR_DATA `color 1` / `color 2`
		bool mounted = false;	// a weapon on a mount (not in the portrait)
	};
	// locomotion fields (docs/reference/LOCOMOTION.md): layer code 0 lower, 1 upper / all, 2 overlay, 3 tail, 4 ears; tri-states 0 no, 1 yes, 2 either
	struct AnimInfo
	{
		std::vector<Note> notes;	// the record's `events` refs, ascending
		std::string name;	// the ANIMATION record's name (dump contacts)
		std::string clip;
		unsigned deletes = 0;
		bool loop = true, synchs = false;
		bool reverse = false;	// ANIMATION `reverse looping`: a looping clip plays to its end then back, not round (ping-pong)
		bool relocates = false;	// ANIMATION `relocates`: the clip's Bip01 drift is the entity's (root motion), so its feet are not the ground's
		float synchOffset = 0;
		bool usable = true, upper = false, idle = false;
		int code = 1, handL = 0, handR = 0, combat = 2, stealth = 2, category = 0;
		unsigned weaponBits = 0, carry = 0;
		float move = 0, min = 0, max = 0, play = 0, idleChance = 1;
		float leg[2][3] = {};	// R, L leg damage window: min, max, ideal
		bool usesL = false, usesR = false;	// `uses left arm` / `uses right arm` (the overlay arm check)
		unsigned overrides = 0;	// `override L arm` 1, `override R arm` 2, `override head` 4 (Animation::setOverrideBone, 1408690b0)
		// the idle pick (kenshi-k16.2.162.1, rwh101_idle_pick_static.md): the record's `idle time min` / `idle time max` s (the exe stores min at +0xe8 and max - min at +0xec: the
		// held idle's dwell is rand x (max - min) + min), `crouched` (+0x9c, an equality gate against the crouch flag), and the record's place in the original's idle list: the
		// walk of the GameData ANIMATION index (sidorder.hpp), the order the combat picker's first maximum and the normal picker's cumulative draw go through
		float idleTimeMin = 0, idleTimeMax = 0;
		bool crouched = false;
		std::string sid;
		std::uint32_t order = 0;
	};
	// A clip's sole contact, derived at load (task L4; MINE: the thresholds): one window is a phase span of one foot's sole point
	// on the ground. stride: the ground the body covers in one cycle with that foot planted, model u per cycle, this body's sizes.
	struct SoleContact
	{
		float start = 0, end = 0;	// phase 0..1; end may pass 1 (a window over the cycle boundary)
		int point = 0;	// 0 heel, 1 ball
	};
	struct Contacts
	{
		std::vector<SoleContact> window[2];	// per foot (0 L, 1 R)
		float stride = 0;	// 0: none derived (not a moving lower clip, or no contact found); the figure the stride warp uses
		float windows = 0;	// the older window-sum figure: kept where the planted figure agrees (forward clips, 5%)
	};
	// One playing clip. keep = the bones it moves: its tracks minus the ANIMATION record's deleted groups.
	struct Layer
	{
		const kworld::SkeletonAnimation* clip = nullptr;
		std::vector<uint8_t> keep;
		float time = 0, weight = 1;
		float vweight = 1;	// the weight the linear ramp would have: the foot lock's contact windows read it, so an inertial snap does not end a stance
		float vfade = 0;	// the time left of that ramp, s: a snap sets it to the ramp's length, and vweight walks it (0 = vweight is the weight)
		bool loop = true, synchs = false;
		bool reverse = false;	// the record's `reverse looping`: the loop is a ping-pong (the time walks in and back out)
		float synchOffset = 0;	// phase offset 0..1 against the first synching layer
		const AnimInfo* info = nullptr;	// locomotion: the record this layer plays (weight follows target)
		float target = 0, rate = 0;
		bool overlay = false;	// layer 2, outside the locomotion lists (the pain overlay)
		Contacts contacts;	// the moving lower clips: sole contacts and stride (deriveContacts)
		float noted = -1;	// the clip progress the notifies have fired up to (-1: not tracking: the layer was not playing)
	};
	struct Char
	{
		Spec spec;
		const Body* body = nullptr;
		std::vector<Layer> layers;
		bool noClipLogged = false;	// kenshi-k16.2.448: posed with no weighted clip (bind pose): logged once
		bool noIdleLogged = false;	// kenshi-k16.2.448: a body at rest found no idle clip (bind pose): logged once
		bool infoless = false;	// a moving character held a layer with no record (the 2026-11-06 segfault): logged once, locomotion skipped
		std::vector<Layer> postures;	// body posture clips: offsets on top at full weight, outside the per-bone normalisation
		const GpuMesh* mesh = nullptr;	// the body mesh, morphed
		std::vector<kworld::SkelVec3> size, posSize;	// per bone: OldBone::setBoneSize / setBonePositionalSize (docs/reference/BODY_SHAPE.md)
		float rootShift = 0;	// body node y over the logical position: (Hs + Ls - 2) * Bip01 bind y (kinspect ft1, 1405b6750 / 1405b18f0)
		// MINE (user 2026-11-06: the original floats too, a bug to fix): the idle pose's lowest sole over the node, measured after 0.5 s idle
		// and taken off the node y while walking or standing (loco), so the soles touch the ground
		float soleLift = 0;
		bool soleFit = false;
		bool soleRest = false;	// soleLift was taken from the body's idle clip (restLift)
		// slope compensation (user 2026-11-06): per foot (L, R) the Foot bone's offset from the node this frame, the idle sole's height under
		// that bone, and the rise the planted foot's slope adds to the node (smoothed, ~0.06 s)
		float feet[2][3] = {}, idleFor = 0;	// idleFor: seconds idle (the fit waits for the blended stance)
		float skintone[3] = {};
		Tex bodyNrm, blendNrm;	// normalMap / bodyBlendNormal, mixed by muscleBlend
		float muscleBlend = 0;
		float blood[8] = {};	// bloodAmount per body part, 0..1
		int hidden = 0;	// hiddenMask: the worn items' `hide parts` (140531cc0), bit 12 + limb slot for a severed limb
		std::vector<const GpuMesh*> stumps;	// severed limbs' stump meshes: on the body's skeleton, drawn with the body material
		std::vector<std::pair<int, kworld::SkelVec3>> boneScale;	// severed limbs: bone, scale (0 below the cut, the thigh 0.8 across)
		Tex vestTex, vestNrm, vestCmap, hairTex, beardTex;
		float vestCol[4] = {}, hairCol[4] = {}, hairMult[4] = {}, hairAlpha[4] = {}, beardAlpha[4] = {};
		std::vector<Part> parts;
		// replace / fall / riseLive while this build's GPU objects are still in the stream lane: the build it replaced, drawn (frozen in its last
		// pose) until this one is drawable, so a rebuild never blinks out; update() drops it then (RENDER_THREAD.md §7.2 "before a body is ready")
		std::shared_ptr<const Char> was;
		// The loading stand-in (kenshi-k16.2.258.1, user 2026-11-15: "a body double rendered as a pure black silhouette ... it *looks* like it's
		// loading"): a build whose GPU objects are not ready yet is drawn as its race's prewarmed body (render.loading_standin; its silhouette or a
		// plain double), then the two-stage switch: the shape morphs into this build's (still black, kStandinShape s), the real look fades in through
		// the dither (kStandinColour s). Both clocks run on the frame's own dt, not the animation clock (a scripted run freezes the poses).
		float standAge = 0;		// s this build has stood in (its own clock; a dev hold keeps the stand-in until it passes, dev.standin_hold)
		float fadeAge = -1;		// s since it became drawable and drawable as itself (-1: not yet; >= kStandinShape + kStandinColour: done)
		bool replaced = false;	// swapIn's build: it replaces one already on screen, so it appears without a stand-in and its fade
		// render.char_fade (kenshi-k16.2.258.10.1): the whole character's drawn fraction, 0..1, dithered through the look's coverage (chardraw's row
		// 199): 0 -> 1 over the cfg's seconds when a character first appears in the drawn set, 1 -> 0 when it leaves; departing keeps it in chars_
		// (frozen) until the fade ends.
		float cover = 1.0f;
		bool departing = false;
		// a rigid item's palette slot follows its bone: slot = skinning(bone) x local (local = the item in bind-pose model space)
		struct Mount { int slot, bone; float local[16]; };
		std::vector<Mount> mounts;
		std::vector<std::string> mountNames;	// per mount, its place ("in the hand @ hands (Bip01 Prop2)", "sheathed @ hip (...)"): the dump lines
		std::vector<float> palette;	// 3 rows x 4 per bone, this frame
		std::vector<kworld::SkelTransform> model;	// per bone, model space, this frame
		// Pose cache (kenshi-k16.2.205.11): a paused / scripted frame (dt 0) computes the same pose when nothing the pose reads changed
		// (Characters::pose's signature); the palette and model above then stay as the last frame's. Store the frame's pose inputs' hash.
		std::uint64_t poseSig = 0;
		bool poseValid = false;
		double pos[3] = {};	// the node: world position and orientation (spec pos / yaw, or the carrier's shoulder)
		kworld::SkelQuat orient;
		float rootXZ[2] = {};	// the root (Bip01) bone's x/z this pose applies, model space: the lab's root lock hands it to the node (rootOffset)
		std::vector<RagBone> rag;	// ragdoll-driven bones (setRagdoll)
		float carryLift = 1;	// being carried: the carried's (Height - 80) x 0.05, which picks the shoulder offset (1405cda20)
		// locomotion: the shared synch phase (cycles), the weapon category the upper-body filters see, the hands holding something, the chosen idle
		float phase = 0;
		float accel = 200;	// RACE `pathfind acceleration` x 10, u/s^2: the sim's braking rate (the stop steer, task L3)
		float footLift = 0;	// Characters::footOffset: the body y added to the node (the lowest down foot's ground over the node's ground; smoothed, world)
		float poseStep = 0;	// this pose's largest local joint rotation step, deg (log feet: the peak angular velocity across a transition, same on and off)
		// the stop steer (task L3, ds/stop-fix): on while the drawn speed falls at the sim's braking rate; the aim is latched once per stop
		// (again when the heaviest lower clip changes) and the phase rate is scaled by k in 0.7..1.4; `steered` says it scaled the rate this
		// frame (log feet), the k range and the speed are reported once per stop
		bool stopActive = false;
		float stopQuiet = 0;	// seconds since the drawn speed last stepped down: a settled speed ends the steer
		float stopTarget = -1;	// the latched aim: the phase the stop lands in (cycles, unnormalised)
		float stopLen = 0;	// that stretch's length (cycles)
		const AnimInfo* stopClip = nullptr;	// the heaviest lower clip the aim was latched from
		float stopKMin = 0, stopKMax = 0;	// the steer's k over this stop (0..0 = no steer applied)
		float stopSpeed0 = 0;	// the drawn speed when the steer took over
		float moveSimPrev = 0;	// last frame's sim forward speed (the stop steer's decel excludes the catch-up)
		float moveSimAge = 0;	// seconds since the sim speed last stepped (the decel's span; kenshi-qaud.1)
		int stopN = 0;	// the stop's steered frames
		bool warped = false;	// the stride warp replaced this frame's phase rate (log feet)
		bool steered = false;	// the stop steer scaled it (log feet)
		std::vector<std::string> warpClamped;	// the strafe clips already logged for its 12x sanity clamp (once per character and clip)
		int weaponCategory = 5;
		bool handL = false, handR = false;
		int idle = -1;	// index into layers
		float idleTimer = 0;	// the held idle's dwell left, s (the exe's animclass+0x240): below 0 the idle is picked again
		bool idleCombat = false;	// which picker made it (combat: the first maximum, else the weighted draw)
		int idleState = -1;	// combat | stealth << 1 when it was picked: a change resets the timer (14051cc30 / 14051cc70 reset animclass+0x240), so the pick is made again
		std::uint32_t idleRng = 0;	// this character's draw state (seeded from its slot: headless shots are stable); 0 = not seeded
		int action = -1;	// index into layers: the playing action (info = actionInfo_)
		float actionSpeed = 1;
		bool actionDone = false;
		std::string actionName;	// the playing action's name (debug log of the notifies)
		const std::vector<Note>* actionNotes = nullptr;	// the action's notifies (its technique's or ANIMATION's), fired by fireNotes
		bool actionBlock = false;	// a block technique: upper body only, the lower layer not capped
		// anim.action_root (fix 2): the action's own root replay. actPath: this action draws along the clip (a root track that travels, not a block);
		// actTravel its largest root travel over the clip (u); anchored once the first path frame has seen the wire (anchor, its yaw, the clip's
		// root there); corr: the drawn path's offset from the wire at the anchor, eased back onto the wire (the sim's clamps and pushes); g: the
		// hand-off, drawn - wire, decaying after the action ends. actYaw0: the sim's aim yaw (aim tail), else the wire's first-sample yaw.
		bool actPath = false;
		float actTravel = 0;
		bool actAnchored = false;
		double actAnchor[2] = {};	// world x / z
		double actPathPt[2] = {};	// the clip root's replay point this frame, world x / z (log feet actroot)
		float actYaw0 = 0;
		float actRoot0[2] = {};	// the clip root at the action's phase 0 (model space, x/z)
		double actCorr[2] = {};	// the drawn path's offset from the wire at the anchor: eased onto the wire
		double actG[2] = {};	// drawn - wire, the last path frame: decays over kActEase after the action
		bool actWas = false;	// the last frame drew along the path (the hand-off's start)
		// the KO fall (Characters::fall, MINE): the layers of the pose it had, their weights, and the lying layer they cross-fade into
		std::vector<float> lieFrom;
		int lieLayer = -1;	// -1: no fade running
		float lieT = 0, lieDur = 0;	// the fade's elapsed and length, seconds
		bool gettingUp = false;	// playing "standing up 3" once (Characters::rise)
		// the get-up blend (Characters::rise, MINE): the ragdoll's last world pose, the clip's pose cross-fades in over riseDur
		std::vector<RagBone> riseFrom;
		float riseT = 0, riseDur = 0;	// elapsed and length, seconds; riseDur 0 = no blend running
		// foot lock (foot IK slice 1, Characters::lockFeet): per foot (0 L, 1 R) the held sole point, the world position it planted at, and the
		// blend back to the animated pose; the knee sanity counters (log feet)
		struct FootLock
		{
			bool active = false;
			bool blocked = false;	// released past the leg's reach: no re-plant until this window ends
			int point = 0;	// 0 heel, 1 ball: c.body->soleBone[f][point]
			int idlePi = -1;	// the idle stance's sole point, taken once when the stance engages (k16.17.7.4: not re-picked as the pose sways); -1 outside a stance
			double at[3] = {};	// world
			bool releasing = false;	// the blend back to the animated foot is running
			float releaseLen = 0;	// its starting length (model space): the blend runs it off over kRelease seconds
			kworld::SkelVec3 shiftPrev;	// the correction applied last frame, model space: the plant and the release both continue from it
			float knee = -1, kneeStep = 0;	// the knee angle last frame (deg) and the largest per-frame change since the report
			int pops = 0, held = 0, frames = 0;	// knee steps past the pop threshold, frames held, and the report's frame count (the left foot's)
			int event = 0;	// this frame's lock event, the knee report's class: 0 none, 1 engage, 2 held, 3 release blend, 4 blocked (98% reach)
			int popsBy[5] = {};	// knee steps past the pop threshold, by event class
			int addedPops = 0;	// pops the blended (pre-lock) animation did not have: only the lock added them
			int addedBy[5] = {};	// those, by event class
			float animKnee = -1, animKneePrev = -1;	// the pre-lock knee angle (deg), this frame and the last
			float dPrev = -1;	// the ankle distance of the pose applied last frame (the knee rate limit's start)
			float yaw0 = 0;	// the body's yaw at the plant (rad): the lock lets go 20 deg from it
			float slopeW = 0;	// slice 2: the ground-slope rotation's weight; 1 in stance, ramped in at its start and blended out on release
		};
		FootLock feetLock[2];
		// Inertialization (anim.inertialize, Bollo 2016; locomote flags a transition, Characters::inertialize decays it onto the blend): per bone
		// the offset of the pose shown when the target changed from the new target pose - translation in the bone's parent frame, rotation an
		// axis-angle vector in its own - and its velocity, and the bone's fade (the velocity-away clamp); and the local poses shown last frame and
		// the one before (the capture and its velocity). t / len: the fade's elapsed and length, s (len 0 = none); pending: this frame's
		// transition, from locomote; dt: the interval between from and prev.
		struct Inertial
		{
			std::vector<kworld::SkelVec3> pos, posV, rot, rotV;
			std::vector<float> dur, durR;	// per bone: the translation's and the rotation's fade, s (the velocity-away clamp each)
			std::vector<kworld::SkelTransform> from, prev;
			float t = 0, len = 0, pending = 0, dt = 0;
		};
		Inertial inert;
	};

	// Records copied out of the game data (paths as the records give them; [0] male, [1] female).
	struct RaceInfo
	{
		std::string name; std::string mesh[2], tex[2], nrm[2], nrmSkinny[2], nrmStrong[2], mask[2], partMap[2], ragdoll[2], carryBone, editor, attach; std::vector<std::array<std::string, 3>> heads[2];
		bool playable = false; std::string description; std::vector<std::string> hairs, hairColours;	// the creator (choices())
		std::vector<float> hairWeights, hairColourWeights;	// parallel to hairs / hairColours: the ref's val0
		std::vector<int> statsGood, statsBad;	// `stats good N` / `stats bad N`: StatsEnumerated ids
		float strength = 1, dexterity = 1, bleedRate = 1;	// the `strength` / `dexterity` / `bleed rate` fields
		int sounds = 0;	// RACE `sounds`: the voice set (humanoid 0-3: Human / Shek / Hive / Robot; an animal 4+: Creature_VO index + 4)
		float accel = 200;	// RACE `pathfind acceleration` x 10, u/s^2: the sim's accelerating and braking rate (ARCHITECTURE "Acceleration")
		std::vector<std::pair<std::string, std::vector<std::string>>> anatomy;	// `combat anatomy` parts in body part index order: name, bones
		float bloodCol[4] = { 0.5f, 0, 0, 1 }, bloodScale[2] = { 1, 0.15f };
		float hull[3] = { 5, 20, 5 };
		float footprint = 2.6f, waterCost = 1.0f;	// RACE `pathfind footprint radius` and the routing cost scale of `water avoidance` as ksim derives them (combat_data.cpp): a route preview's body
		float portrait[4] = { 10, 0, 0.5f, 0 };	// `portrait distance`, `portrait offset x / y / z` (the Human values)
		bool portraitSet = false;	// the RACE has `portrait distance` and the race is a humanoid: its own framing; an animal: the portrait frames its head bone's bounds
		std::unordered_map<std::string, float> partMax;	// LOCATIONAL_DAMAGE name -> `combat anatomy` v1
		std::vector<const AnimInfo*> animalAnims;	// an animal: its ANIMAL_CHARACTER's ANIMAL_ANIMATION records (`animations`, `turning anim`); empty = a humanoid (the ANIMATION list)
		const AnimInfo* animalStrafe = nullptr;	// `strafe anim`
		std::string stump[4][2];	// `limb replacement`: per limb slot, male / female mesh
	};
	struct ArmourInfo { std::string mesh[2], vestTex[2], vestNrm[2], vestCmap[2], matTex, matNrm, matCmap; float glossMult = 1; bool hideHair = false, colorise = true; int slot = -1, hideParts = 0, armourClass = 0; std::string colour; };
	// category: WeaponCategory for the animation filters; scale: mesh x, y, z; crossbows: the string meshes, the bolt and its node offset (GunClassTurret 14043d120)
	struct WeaponInfo { std::string mesh, bare, sheath; int category = 0; float scale[3] = { 1, 1, 1 }; bool crossbow = false; std::string loaded = {}, unloaded = {}, ammo = {}; float bolt[3] = {}; };
	struct WeaponModel { std::string tex, nrm; float spec = 1; float scale[3] = { 1, 1, 1 }; };
	struct HairInfo { int slot = 2; std::string mesh[2], tex[2], head[2]; int diffuseCh = 0, alphaCh = 3, headCh[2] = {}, headAlphaCh[2] = { 1, 1 }; float threshold = 0.5f, spec = 1, mipBias = 0; };

	const Body* body(const std::string& race, bool female, std::string& err, bool* notReady = nullptr);	// notReady as add()'s
	void prewarm();	// every race's body mesh + skeleton queued on the workers (init)
	// rigid >= 0: every vertex on that palette slot (a weapon), the body is not needed
	const GpuMesh* mesh(const std::string& recordPath, const Body* body, std::string& err, int rigid = -1);
	// target: the skeleton whose handles the bone indices become (by bone name) when the mesh links its own; null keeps them
	bool upload(const std::string& file, const kworld::Skeleton* target, int rigid, GpuMesh& out, std::string& err);
	// parts: bake that body's part map bits into the vertices (the body mesh only)
	bool upload(const ogre::Mesh& mesh, const std::string& file, const kworld::Skeleton* target, int rigid, GpuMesh& out, std::string& err, const Body* parts = nullptr);
	// the main-thread half of upload(): built arrays (own: the mesh's skeleton bone names) get their bone bytes, then the stream job
	bool finish(std::vector<float> verts, std::vector<uint32_t> idx, const std::string& file, const kworld::Skeleton* target, const std::vector<std::string>& own, int rigid, GpuMesh& out, std::string& err, bool patched = false);
	static void readPartMap(const ResourceIndex& res, const std::string& recordPath, Body& b);
	// body() in parts (kenshi-k16.2.205.12.79): buildBody the CPU half when no prefetch worker built it, finishBody the rest either way
	bool buildBody(const std::string& race, const RaceInfo& ri, int g, const std::string& meshFile, ogre::Mesh* decoded, kworld::Skeleton* decodedSkel, std::unique_ptr<Body>& out, std::string& err);
	const Body* finishBody(const std::string& key, const RaceInfo& ri, int g, const std::string& race, std::unique_ptr<Body> b, const Rest* rest);
	static void readRest(const ResourceIndex& res, const std::string& editorRec, const std::string& attachRec, int g, const kworld::Skeleton& skel, Rest& out);
	static std::string restKey(const RaceInfo& r, int g);	// editor record, attachment record, sex (newline separated)
	static void fitSolePoints(Body& b);	// log feet: Body::soleBone / soleAt
	const GpuMesh* morphed(const Body& body, const std::unordered_map<std::string, float>& weights, std::string& err);
	static void boneSizes(Char& c, const std::unordered_map<std::string, float>& v);
	void addWeapon(Char& c, const std::string& spec, const std::string& point, bool drawn, bool loaded);
	void addTool(Char& c, const std::string& item);	// an ITEM's mesh on `hands` (the special tool of an operated producer)
	static std::vector<AttachPoint> readAttachPoints(const std::string& file, const kworld::Skeleton& skel);
	Tex texture(const std::string& recordPath, const Tex& fallback);
	Tex makeView(std::shared_ptr<const DdsImage> dds, std::shared_ptr<const std::vector<uint8_t>> rgba, int w, int h);	// queues the stream job (dds, else RGBA8 w x h)
	void queueBuffers(GpuMesh& out, std::vector<float> verts, std::vector<uint32_t> idx);	// queues the stream job
	void queueFree(const GpuMesh& m);	// the buffers' destroy, in the stream lane behind their create
	void swapIn(size_t i);	// replace / fall / riseLive: the build add() appended takes slot i, the old one kept as its `was` while it is not drawable
	bool drawable(const Char& c) const;	// every Handle the character binds is ready (logic names it in the draw list only then)
	const Char* shownOf(const Char& c) const;	// what is drawn for c: c once drawable, else the build it replaced while that one is (was), else nothing
	void pose(Char& c, float dt = 0);
	static bool solesOf(const Char& c, double out[2][3]);	// soles() of one character
	// kenshi-k16.17.7.2: the body's standing offset, once from the authored `idle_stand_normal` at time 0 (never a live pose): the lowest sole over the node, 0 if no clip
	float restLift(const Char& c);
	// anim.inertialize: a transition's per-bone offset and velocity decayed onto the blend (the pose shown before it, before lockFeet)
	void inertialize(Char& c, std::vector<kworld::SkelTransform>& local, float dt);
	// foot lock (foot IK slice 1): the blended pose's planted sole points held in the world (local / model as pose() leaves them, before the palette)
	void footOffset(Char& c, const std::vector<kworld::SkelTransform>& model, float dt);	// the one pelvis offset (footLift), from this frame's pose, before lockFeet
	void lockFeet(Char& c, std::vector<kworld::SkelTransform>& local, std::vector<kworld::SkelTransform>& model, float dt);
	static bool lockStopped(const Char& c);	// foot lock: no lock (and no knee sanity either) on an action without a root replay, carry, prone, ragdoll or get-up
	void kneeReport(Char& c);	// foot lock's swing-foot sanity (log feet): the knee's per-frame step and pops, printed every 600 frames
	// a carried body on its carrier's shoulder: the carrier's skeleton and carry side from `carrier`, its pose (model, orient, pos) as given (update()
	// passes the state the serial loop would have read: posed this frame for a carrier earlier in chars_, before the loop for one later)
	static void carryPlace(Char& c, const Char& carrier, const std::vector<kworld::SkelTransform>& model, const kworld::SkelQuat& orient, const double pos[3]);
	struct CarrierAt { bool snap = false; std::vector<kworld::SkelTransform> model; kworld::SkelQuat orient; double pos[3] = {}; };
	static constexpr size_t kPoseMinItems = 8;	// the pose pass runs serial under this many characters
	WorkPool& posePool(size_t items);	// anim.pose_threads' pool, remade when the option changes
	std::unique_ptr<WorkPool> posePool_;
	bool locoAdd(Char& c, std::string& err);
	Layer* addLayer(Char& c, const std::string& clip, unsigned deletes, float time, std::string& err);
	bool startAction(Char& c, const std::string& name, float speed, std::string& err);
	void locomote(Char& c, float dt, bool instant);
	static std::vector<Note> notesOf(const kdata::GameData& gd, const kdata::Record* owner, bool technique);
	void fireNotes(Char& c, size_t index);	// after locomote: the notifies the playing layers passed this update (sounds_)
	std::vector<SoundNote> sounds_;
	void finishFall(Char& c);	// the KO fall's fade is done: the old pose's layers go, the lying clip alone, frozen
	void deriveContacts(Char& c);	// the lower moving clips' sole contact windows and stride, at load
	void contactsOf(Char& c, Layer& l) const;	// one clip's sole contact windows and stride (deriveContacts; the action clip at its start)
	bool actionPath(Char& c, float dt);	// anim.action_root: draws c.pos along the action's clip root, the corr easing onto the wire (false = no path)
	bool handsFit(const Char& c, const AnimInfo& a) const;
	void bindLighting(sg_bindings& bind) const;
	// ---- the instanced character draw (kenshi-k16.2.205.8; Group, Plan and FrameData above) ----
	// Per character, one slot of the `skins` storage buffer (kSkinRows vec4 rows: 180 bone rows, origin, blood x2, blood scale, bone_part x15, then row
	// 199: x = the stand-in's shape t, y = the character's dither coverage, z = the stand-in body's, w = its parts', kenshi-k16.2.258.1), written
	// once per frame; per (character, part) one record of the `insts` storage buffer (kInstRows rows of tints and clip channels, read by the fragment
	// stage); per pass a list of (skin row, record) pairs as the instance-rate vertex buffer, in groups of one (mesh, material): one draw each.
	// The per-view data (the matrix, the camera, the cascade's bias) stays a small uniform block, apart from all of it.
	static constexpr int kSkinRows = 200, kInstRows = 9;
	static constexpr int kMaxSkins = 4096, kMaxInsts = 32768, kMaxList = 131072;
	struct ViewKey	// the mesh and every texture view a draw binds (unused: 0)
	{
		const GpuMesh* mesh = nullptr;
		const GpuMesh* mesh2 = nullptr;	// the stand-in pass only: the second vertex buffer (the race base body); null elsewhere
		sg_view v[12] = {};
		bool operator==(const ViewKey& o) const;
	};
	struct ViewKeyHash { size_t operator()(const ViewKey& k) const; };
	struct GpuSet	// the buffers one frame (or one portrait) draws from (render side)
	{
		sg_buffer skins = {}, insts = {}, list = {};
		sg_view skinsView = {}, instsView = {};
		int skinCap = 0, instCap = 0, listCap = 0;
		Plan plan;	// uploadSet's copy of the frame data's plan
		bool written = false;	// a frame's data is in the buffers
	};
	void makeSet(GpuSet& s, int skins, int insts, int list, const char* label);
	void makeGpu(const Tex (&fallback)[3]);	// init's GPU half: pipelines, samplers, the material's fallback textures (published into fallback), frame_
	void freeSet(GpuSet& s);
	// Gathers `list`'s characters into out (CPU only): cam = the camera every draw of the set is camera-relative to. main: the characters the main view
	// draws (culled out when `cull`); cascades: the shadow cascades to gather (null entries skipped). caps: the GPU set's skin, record and list capacity.
	void buildSet(FrameData& out, const std::vector<Char>& list, bool cull, const double cam[3], const ShadowCascade* const cascades[kMaxCascades],
		int skinCap, int instCap, int listCap);
	void uploadSet(GpuSet& s, const FrameData& fd);
	void drawGroups(const GpuSet& s, const std::vector<Group>& groups, int mode);	// the pipeline and the pass's uniforms are applied; mode 0 body, 1 armour, 2 hair, 3 the stand-in
	void drawMain(const GpuSet& s, const std::vector<Group> Plan::* which, int mode, const float mvp[16], const double cam[3], const LightParams& light, const ShadowParams& shadows);
	GpuSet frame_;
	std::vector<std::unique_ptr<GpuSet>> portraitSets_;
	int portraitUsed_ = 0;
	bool instancing_ = true;	// render.char_instancing
	// the posed portrait copy of character i into `out` (portraitFrame, headHeight; see portraitFrame); logic side
	bool portraitPose(size_t i, float yaw, float pitch, double target[3], double eye[3], Char& out);
	// the world box of one body part's skinned vertices in the posed character (an animal portrait's fallback head frame); false when no vertex is its
	static bool partBox(const Char& c, int part, double lo[3], double hi[3]);
	// the world box of the skinned vertices whose heaviest bone is `bone` (the animal portrait's head bone, narrower than its body part)
	static bool boneBox(const Char& c, int bone, double lo[3], double hi[3]);
	static bool boundsOf(const Char& c, bool byPart, int index, double lo[3], double hi[3]);	// partBox / boneBox's shared scan
	static int headBone(const kworld::Skeleton& sk);	// the skeleton's head bone: `Bip01 Head`, else a bone named like one (animals keep the Bip01 names; this is the fallback)

	const ResourceIndex* res_ = nullptr;
	std::vector<RaceInfo> races_;
	std::unordered_map<std::string, ArmourInfo> armour_;	// lower-case record name
	std::unordered_map<std::string, HairInfo> hair_;
	std::vector<std::pair<std::string, std::vector<std::string>>> raceGroups_;
	std::vector<std::string> idleStances_;	// RACE_GROUP name -> `races` names, data order
	std::unordered_map<std::string, AnimInfo> anims_;
	std::vector<std::unique_ptr<AnimInfo>> animalAnims_;	// the ANIMAL_ANIMATION records (RaceInfo::animalAnims points in)
	std::vector<const AnimInfo*> animOrder_;	// data order (the idle chooser keeps the first of equals)
	struct TechniqueInfo { std::string clip; float mult = 1; bool block = false; std::vector<Note> notes; };	// block: `is block` and not `is dodge`
	std::unordered_map<std::string, TechniqueInfo> techniques_;	// COMBAT_TECHNIQUE, lower-case name
	struct PainInfo { std::string anim; float threshold = 0; };
	std::unordered_map<std::string, PainInfo> pain_;	// LOCATIONAL_DAMAGE `pain anim` (ANIMATION name, health threshold), lower-case part name
	AnimInfo actionInfo_;	// the layer-1 record an action layer points at
	float blendRate_ = 3.0f;	// CONSTANTS `animation blend rate` (rebirth.mod: 4.0)
	int strideWarp_ = 1;	// anim.stride_warp, read on change (cfg generation); 0 = the original's phase rate
	int stopContact_ = 1;	// anim.stop_contact: steer the phase to a contact during a stop; 0 = the original's freeze
	bool footLock_ = false;	// anim.foot_lock: hold the planted sole points in the world; false = the original's sliding feet
	bool footTerrain_ = false;	// anim.foot_terrain: in stance, the foot pitched and rolled to the ground's slope and its sole on it (slice 2)
	bool actionRoot_ = true;	// anim.action_root: an action draws along its clip root and the lock holds the action clip's contact windows
	bool inertOn_ = true;	// anim.inertialize: a clip transition decays its old pose's offset and velocity onto the target; false = the linear ramp
	uint64_t cfgGen_ = 0;
	std::unordered_map<std::string, std::array<float, 6>> colours_;	// COLOR_DATA: color 1, color 2
	std::unordered_map<std::string, std::string> factionColours_;	// FACTION -> its COLOR_DATA name
	std::unordered_map<std::string, WeaponInfo> weapons_;
	std::unordered_map<std::string, WeaponModel> weaponModels_;
	struct ToolInfo { std::string mesh, tex, nrm; };	// an ITEM's mesh and its material spec's maps (the special tool props)
	std::unordered_map<std::string, ToolInfo> tools_;
	std::unordered_map<std::string, std::unique_ptr<Body>> bodies_;	// race name + gender
	std::unordered_set<std::string> warmed_;	// race keys (bodies_) whose race-only build warm() did, or whose body failed (kenshi-k16.2.205.12.77)
	// contactsOf's results by (body, clip, record, the bone sizes): a function of those alone; a crowd shares bodies and slider sets (kenshi-k16.2.205.12.76)
	mutable std::unordered_map<std::string, Contacts> contactCache_;
	std::unordered_map<std::string, std::unique_ptr<GpuMesh>> meshes_;	// file + body skeleton; morphed bodies: file + the pose weights
	std::unordered_map<std::string, Tex> views_;	// texture file -> view (ready once its job ran)
	struct Prefetch;
	std::shared_ptr<Prefetch> pf_;
	std::vector<std::string> textureFiles(const Spec& s);
	std::string bodyMeshFile(const Spec& s);	// the race's body decode key (bodyKey) while its body is not built
	std::string bodyKey(const RaceInfo& r, int g);	// a body decode job's key: mesh file, newline, part map record
	const std::string& located(const std::string& rec);	// the .dds files add(s) will read through texture()
	std::unordered_map<std::string, std::string> located_;	// record path -> file (textureFiles' cache)
	std::vector<sg_image> images_;	// render side: the jobs append, shutdown destroys
	std::vector<Char> chars_;
	bool departDone_ = false;	// a departing character's fade reached 0 (takeDepartDone; the owner drops it at its next keep)
	std::vector<uint8_t> drawState_;	// per index, the last main-view buildSet's DrawState (drawState())
	// The creator preview's assembly rotation (setPreviewOrbit): character previewIndex_'s node goes through it at every update()
	bool previewOrbit_ = false;
	size_t previewIndex_ = 0;
	double previewCenter_[3] = {}, previewShift_[3] = {};
	kworld::SkelQuat previewQ_;
	std::unordered_map<std::string, std::string> sidNames_;	// stringID -> record name, every record
	std::unordered_map<std::string, ItemInfo> items_;
	std::unordered_map<std::string, ModInfo> mods_;
	std::unordered_set<std::string> equippable_;	// WEAPON and ARMOUR stringIDs
	Tex nothing_, flat_, white_;
	Tex blood_;	// character.material bloodMap: bloody.png
	sg_sampler border_ = {}, wrap_ = {};
	sg_shader shd_ = {}, armourShd_ = {}, hairShd_ = {}, shadowShd_ = {};
	sg_shader standinShd_ = {}, standinLitShd_ = {};	// the loading stand-in: flat black, and the plain double under the shared light
	sg_pipeline pip_ = {}, armourPip_ = {}, hairPip_ = {}, shadowPip_ = {}, standinPip_ = {}, standinLitPip_ = {};
	int standin_ = kStandinSilhouette;	// render.loading_standin, read on change (cfg generation)
	float charFade_ = 1.0f;	// render.char_fade: the arrival / departure dither's seconds, read on change (cfg generation); 0 = the pop
	bool standinHeld(const Char& c) const;	// the dev hold: this build is kept in its stand-in (dev.standin_hold, KCLIENT_DEV_TOOLS)
	bool standinLive(const Char& c) const;	// this build should be drawn standing in or fading this frame
	// What this build draws as this frame: 0 as itself, 1 the stand-in whole (its GPU objects are queued, or the dev hold keeps it), 2 stage 1 (the
	// shape morphs into this build's, still in the stand-in's material), 3 stage 2 (the real look dithers in as the stand-in dithers out)
	int standinStage(const Char& c) const;
};
