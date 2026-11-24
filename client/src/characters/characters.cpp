#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "characters/characters.hpp"
#include "app/cpustage.hpp"
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "render/drawstats.hpp"
#include "assets/sidorder.hpp"
#include "app/eventlog.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include "render/ao.hpp"
#include "render/lightvol.hpp"
#include "app/config.hpp"
#include "assets/dds.hpp"
#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "assets/ogremesh.hpp"
#include "render/speccube.hpp"
#include "stb_image.h"
#include "render/weathermap.hpp"

#include "character.glsl.h"
#include "characters/characters_internal.hpp"

namespace
{


// anim.stride_warp (task L4): the locomotion phase rate from the clips' measured stance speed instead of their play speed;
// 0 = the original's rate
const cfg::Int kStrideWarp{ cfg::Group{ "Locomotion" }, "anim.stride_warp", 1, 0, 1, "Locomotion: scale the shared phase so the lower clips' stance ground speed matches the body speed (0 = the original's play-speed rate)" };
// anim.stop_contact (task L3): steer the phase during the sim's braking so it lands on a sole contact when the speed reaches 0;
// 0 = the original's freeze mid-stride
const cfg::Int kStopContact{ cfg::Group{ "Locomotion" }, "anim.stop_contact", 1, 0, 1, "Locomotion: steer the phase during a stop so it lands on a sole contact as the body stops (0 = the phase freezes mid-stride)" };
// anim.fall_blend (task KO interim): the KO / death fall. The original ragdolls (kinspect ko_transitions.md; the real ragdoll is parked, CLIENT_GOALS
// "Ragdolls"); MINE: the standing pose cross-fades into the lying pose over this, 0 = the old snap.
const cfg::Float kFallBlend{ cfg::Group{ "KO and ragdolls" }, "anim.fall_blend", 0.4, 0.0, 2.0, "KO / death: the seconds the standing pose takes to blend into the lying pose (MINE: the original ragdolls; 0 = the snap)" };
// anim.rise_blend (ragdoll P3): the get-up. The ragdoll's last pose cross-fades into `standing up 3` over this (MINE, about the original's
// ragdoll releasing a body over a couple of frames); 0 = the snap to the clip's first frame.
const cfg::Float kRiseBlend{ cfg::Group{ "KO and ragdolls" }, "anim.rise_blend", 0.4, 0.0, 2.0, "KO recovery: the seconds the ragdoll's last pose takes to blend into `standing up 3` (0 = the snap)" };
// anim.foot_lock (foot IK slice 1): while the lower layers' weighted contact windows hold a sole point down, keep it at the world position where
// it planted (two-bone leg IK on the blended pose, Characters::lockFeet); 0 = the original's sliding feet. OFF by default (user 2026-11-12, k16.17.7.2: the IK never looked right; experimental)
const cfg::Bool kFootLock{ cfg::Group{ "Foot placement (experimental)" }, "anim.foot_lock", false, "Locomotion, experimental (kenshi-k16.17.7): hold each planted heel / ball at its world position while its contact window is down (0 = the original's sliding feet, the default)" };
// anim.foot_terrain (foot IK slice 2): in stance the foot is pitched and rolled to the drawn ground's slope and its sole set on the ground, the
// idle's both feet are fitted and its body height taken from the lower foot; 0 = slice 1's flat hold (the gravity estimate's toe-off sink stays)
const cfg::Bool kFootTerrain{ cfg::Group{ "Foot placement (experimental)" }, "anim.foot_terrain", false, "Locomotion, experimental (kenshi-k16.17.7): in stance, plant the foot's sole on the drawn ground's slope (pitch and roll), and fit the idle's feet and body height (0 = off, the default)" };
// anim.foot_pelvis (foot IK slice 3): a pinned leg at ~98% of its reach lowers the body by the shortfall (capped, smoothed) instead of
// releasing, and a steered stop's contact stays pinned through the locomotion -> idle fade; 0 = slice 2 exactly. Off by default: today every
// stop squats and lunges, then the foot slides into place (user 2026-11-07)
// anim.foot_pitch (slice 3c, WIP): a held foot's other sole point rotated onto the drawn ground's line so the sole lies flat on a slope; 0 =
// the slope rotation alone (slice 2's own pose). Off by default: untuned, and the all-off look is the bar
// anim.action_root (fix 2, the same report): a technique's displacement is the clip's own root replayed tick by tick (COMBAT_ANIM.md 5, the sim:
// clip.at(t1) - clip.at(t0) in x/z, rotated by the aim yaw fixed at the technique's start, no root yaw, at its playback rate). The client zeroed
// an action's root x/z and drew the body at the wire, so a clip's stance foot slid at body speed. Draw the body along the anchor + the clip's root
// and hold the action clip's own contact windows with the foot lock; where the wire differs (navmesh clamp, separation push, stumble recoil) the
// drawn path eases onto it. 0 = the original's wire position and no action lock.
const cfg::Bool kActionRoot{ cfg::Group{ "Locomotion" }, "anim.action_root", true, "Animation: an action draws along its clip root replayed from the action's start (the technique's own displacement) and the foot lock holds the action's contact windows (0 = the wire position, no action lock)" };
// anim.inertialize (CLIENT_GOALS "Locomotion fidelity" 5): a clip transition's pose offset and velocity decay onto the target pose over the
// crossfade's own time (Bollo 2016) instead of the linear weight ramp; 0 = the original's linear crossfade exactly
const cfg::Bool kInertialize{ cfg::Group{ "Locomotion" }, "anim.inertialize", true, "Locomotion: a clip change keeps the old pose's motion, decaying onto the new clip, instead of a linear crossfade (0 = the original's linear crossfade)" };

// render.loading_standin (kenshi-k16.2.258.1, user 2026-11-15): what a character whose GPU objects are not ready yet draws as, at its wire position
// and animation — its race's prewarmed body in the stand-in's material, then the two-stage switch into its real look (Characters::kStandinShape /
// kStandinColour). silhouette: flat black, unlit, no textures (it reads as loading, never as an identity; the user's pick for the default);
// double: the race body in a plain neutral material; off: nothing drawn (the behaviour before this).
const cfg::Text kLoadingStandin{ cfg::Group{ "Character loading" }, "render.loading_standin", "silhouette", { "silhouette", "double", "off" },
	"Characters still uploading draw as their race's body: silhouette = flat black, unlit (the default); double = a plain neutral body; off = not drawn" };
// render.char_fade (kenshi-k16.2.258.10.1, the user's "the units popped in" on the town approach): a character that first appears in the drawn set
// dithers in over this many seconds through the look's coverage, and one that leaves the set dithers out over the same, so a whole body never pops in
// or out in one frame. 0 = the old pop.
const cfg::Float kCharFade{ cfg::Group{ "Character loading" }, "render.char_fade", 1.0, 0.0, 5.0, "Characters: the seconds a character takes to dither in when it first appears in the drawn area, and out when it leaves (0 = the old one-frame pop)" };
#ifdef KCLIENT_DEV_TOOLS
// dev.standin_hold (KCLIENT_DEV_TOOLS, kenshi-k16.2.258.1): hold every non-squad character in its stand-in for this many seconds after its build, so
// the A/B and the switch can be seen live and in a script (a script sets it to 0 on a frame to release the whole crowd into the fade at once).
const cfg::Float kStandinHold{ cfg::Group{ "Character loading", "", "Debug" }, "dev.standin_hold", 0.0, 0.0, 100000.0, "Dev: keep every non-squad character in its loading stand-in this many s after it is built (0 = off; a script sets it back to 0 to release the fade)" };
#endif


static_assert(Characters::kMaxCascades == Shadow::kCascades, "cascade count");
static_assert(sizeof(ShadowParams) == sizeof(shadow_params_t), "shadow_params layout");


}

void Characters::shutdown()
{
	pf_.reset();
	// cleanup() (the render thread, after rq::flushAll ran every create): destroyed directly (§7.2 rule 4); a fallback view is in views_ too
	for (auto& [k, b] : bodies_) { sg_destroy_buffer(b->mesh.gpu.get().vbuf); sg_destroy_buffer(b->mesh.gpu.get().ibuf); }
	for (auto& [k, m] : meshes_) { sg_destroy_buffer(m->gpu.get().vbuf); sg_destroy_buffer(m->gpu.get().ibuf); }
	std::unordered_set<uint32_t> gone;
	for (auto& [k, v] : views_) { if (gone.insert(v.get().id).second) { sg_destroy_view(v.get()); } }
	for (const Tex* t : { &nothing_, &flat_, &white_ }) { if (gone.insert(t->get().id).second) { sg_destroy_view(t->get()); } }
	for (sg_image img : images_) { sg_destroy_image(img); }
	sg_destroy_sampler(border_);
	sg_destroy_sampler(wrap_);
	freeSet(frame_);
	for (auto& ps : portraitSets_) { freeSet(*ps); }
	portraitSets_.clear();
	for (sg_pipeline p : { pip_, armourPip_, hairPip_, shadowPip_, standinPip_, standinLitPip_ }) { sg_destroy_pipeline(p); }
	for (sg_shader s : { shd_, armourShd_, hairShd_, shadowShd_, standinShd_, standinLitShd_ }) { sg_destroy_shader(s); }
	contactCache_.clear();
	bodies_.clear();
	warmed_.clear();
	meshes_.clear();
	views_.clear();
	images_.clear();
	chars_.clear();
}


Characters::Layer* Characters::addLayer(Char& c, const std::string& clip, unsigned deletes, float time, std::string& err)
{
	const kworld::Skeleton& sk = c.body->skel;
	Layer l;
	l.clip = sk.animation(clip);
	if (!l.clip) { err = c.spec.race + ": no clip named " + clip; return nullptr; }
	l.keep.assign(sk.bones.size(), 0);
	for (size_t b = 0; b < sk.bones.size(); b++) { l.keep[b] = l.clip->track((int)b) != nullptr; }
	for (size_t i = 0; i < std::size(kDeleteGroups); i++)
	{
		if (!(deletes & (1u << i))) { continue; }
		for (const char* name : kDeleteGroups[i].bones) { if (const int b = sk.bone(name); b >= 0) { l.keep[(size_t)b] = 0; } }
	}
	l.time = time;
	c.layers.push_back(std::move(l));
	return &c.layers.back();
}

// Starts an action layer (a COMBAT_TECHNIQUE or ANIMATION name, or a raw clip) at time 0, weight 0; speed < 0: the technique's anim speed mult (1 for the others).
bool Characters::startAction(Char& c, const std::string& name, float speed, std::string& err)
{
	const kworld::Skeleton& sk = c.body->skel;
	// `<technique> override` (chooseCounterTechnique 140609290, the follow-up of a dodge): the block clone below
	const std::string suffix = " override";
	const bool override = name.size() > suffix.size() && folded(name.substr(name.size() - suffix.size())) == suffix;
	const auto ti = techniques_.find(folded(override ? name.substr(0, name.size() - suffix.size()) : name));
	const auto ai = ti == techniques_.end() ? anims_.find(folded(name)) : anims_.end();
	const std::string clip = ti != techniques_.end() ? ti->second.clip : ai != anims_.end() ? ai->second.clip : name;
	Layer* l = addLayer(c, clip, ai != anims_.end() ? ai->second.deletes : 0, 0.0f, err);
	if (!l) { return false; }
	c.actionBlock = ti != techniques_.end() && ti->second.block;
	c.actionName = name;
	c.actionNotes = ti != techniques_.end() ? &ti->second.notes : ai != anims_.end() ? &ai->second.notes : nullptr;
	if (c.actionBlock)
	{
		// 14086dfe0 -> 140868be0: a block clip loses its root, pelvis and leg tracks; its ` override` clone overrides the R arm and Prop2
		for (const char* bone : { "Bip01", "Bip01 Pelvis", "Bip01 R Thigh", "Bip01 L Thigh", "Bip01 R Calf", "Bip01 L Calf", "Bip01 R Foot", "Bip01 L Foot", "Bip01 R Toe0", "Bip01 L Toe0" })
		{
			if (const int b = sk.bone(bone); b >= 0) { l->keep[(size_t)b] = 0; }
		}
		for (const char* bone : { "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand", "Bip01 Prop2" })
		{
			if (const int b = sk.bone(bone); b >= 0 && override && l->keep[(size_t)b]) { l->keep[(size_t)b] = 2; }
		}
	}
	l->loop = false;
	l->info = &actionInfo_;
	l->weight = 0;
	l->vweight = 0;
	l->time = 0;
	c.action = (int)c.layers.size() - 1;
	c.actionSpeed = speed >= 0 ? speed : ti != techniques_.end() ? ti->second.mult : 1.0f;
	// anim.action_root (fix 2): the action clip's own contact windows (the lock holds them) and its root travel over the clip (a travelling root
	// replays the body; a punch in place keeps the wire's position). The derivation is the locomotion clips' own, over this body.
	c.actPath = false;
	c.actTravel = 0;
	c.actAnchored = false;
	c.actWas = false;
	c.actCorr[0] = c.actCorr[1] = 0;
	c.actG[0] = c.actG[1] = 0;
	c.actRoot0[0] = c.actRoot0[1] = 0;
	contactsOf(c, *l);
	if (actionRoot_ && !c.actionBlock && l->clip && l->clip->length > 0)
	{
		const kworld::Skeleton& sk = c.body->skel;
		const int b = sk.bone("Bip01");
		if (b >= 0)
		{
			const kworld::SkelVec3 r0 = kworld::sampleLocal(sk, l->clip, b, 0.0f).pos;
			c.actRoot0[0] = r0.x;
			c.actRoot0[1] = r0.z;
			for (int k = 1; k <= 16; k++)
			{
				const kworld::SkelVec3 r = kworld::sampleLocal(sk, l->clip, b, l->clip->length * (float)k / 16.0f).pos;
				c.actTravel = std::max(c.actTravel, (float)std::hypot(r.x - r0.x, r.z - r0.z));
			}
			c.actPath = c.actTravel > 0.5f;
		}
	}
	return true;
}

bool Characters::action(size_t i, const std::string& name, float speed, float t, bool loop, std::string& err)
{
	Char& c = chars_[i];
	const float age = t;	// anim.action_root: the age the drawn path replays from (a wire-driven character refreshes it every frame)
	if (c.action >= 0)
	{
		c.layers.erase(c.layers.begin() + c.action);
		c.action = -1;
		c.actionDone = false;
	}
	if (name.empty() || !c.spec.loco) { return true; }
	if (!startAction(c, name, speed, err)) { return false; }
	Layer& l = c.layers[(size_t)c.action];
	l.loop = loop;
	if (loop && t > 1 && l.clip->length > 0)
	{
		// a long-running loop (a reconnect mid-task): jump to its phase a second early and step only that second (the blend-in), not hours of frames
		l.time = std::fmod((t - 1) * c.actionSpeed, l.clip->length);
		t = 1;
	}
	for (; t > 0; t -= 1.0f / 60.0f) { locomote(c, std::min(t, 1.0f / 60.0f), false); }
	c.spec.actionTime = age;
	return true;
}

void Characters::endAction(size_t i)
{
	Char& c = chars_[i];
	if (c.action >= 0) { c.actionDone = true; }
}

bool Characters::replace(size_t i, const Spec& s, std::string& err)
{
	if (!add(s, err)) { return false; }
	swapIn(i);
	return true;
}

// The build add() just appended takes slot i. While its GPU objects are still queued, the old build stays what is drawn (Char::was).
void Characters::swapIn(size_t i)
{
	Char& old = chars_[i];
	// All or nothing (kenshi-k16.2.258.1.2.1): a build that is itself still standing in (queued, held by dev.standin_hold, or inside its switch) has
	// shown no real look yet, so it is no "old build on screen": it must not become `was` (shownOf would draw its whole real look) and its
	// replacement does not skip the stand-in. The replacement stands in on the same clock until its whole look is ready.
	const bool standing = standinLive(old);
	const float standAge = old.standAge;
	const float cover = old.cover;	// a rebuild is not an arrival: it keeps the fade's place (kenshi-k16.2.258.10.1)
	std::shared_ptr<const Char> was = standing ? nullptr : drawable(old) ? nullptr : old.was;
	if (!standing && !was && drawable(old))
	{
		old.was.reset();
		was = std::make_shared<const Char>(std::move(old));
	}
	chars_[i] = std::move(chars_.back());
	chars_.pop_back();
	if (standing)
	{
		chars_[i].standAge = standAge;
		chars_[i].cover = cover;
		return;
	}
	if (!drawable(chars_[i])) { chars_[i].was = std::move(was); }
	chars_[i].cover = cover;
	chars_[i].replaced = true;	// a build over one already on screen: it appears without the loading stand-in's fade (kenshi-k16.2.258.1)
}

bool Characters::drawable(const Char& c) const
{
	if (!c.body || !c.mesh || !c.mesh->gpu.ready() || !blood_.ready()) { return false; }
	const Body& b = *c.body;
	for (const Tex* t : { &b.tex, &b.mask, &c.bodyNrm, &c.blendNrm, &c.vestTex, &c.vestNrm, &c.vestCmap, &c.hairTex, &c.beardTex, &white_ })
	{
		if (!t->ready()) { return false; }
	}
	for (const auto& h : b.heads) { for (const Tex& t : h) { if (!t.ready()) { return false; } } }
	for (const GpuMesh* m : c.stumps) { if (!m->gpu.ready()) { return false; } }
	for (const Part& p : c.parts) { if (!p.mesh->gpu.ready() || !p.tex.ready() || !p.nrm.ready() || !p.cmap.ready()) { return false; } }
	return true;
}

const Characters::Char* Characters::shownOf(const Char& c) const
{
	if (drawable(c) && !standinHeld(c)) { return &c; }
	return c.was && drawable(*c.was) ? c.was.get() : nullptr;
}

// The dev hold (dev.standin_hold, KCLIENT_DEV_TOOLS): keep this build standing in for `dev.standin_hold` s after it was built, whatever its GPU
// objects are doing, so the stand-in and the switch can be seen live and in a script. The player's own squad is never held (the bead's text).
bool Characters::standinHeld(const Char& c) const
{
	if (c.spec.lowDetail && !c.replaced) { return true; }	// a low-detail figure stays the plain stand-in, with render.loading_standin off too: its minimal build is a bare body (kenshi-9mw3, kenshi-bqkd)
#ifdef KCLIENT_DEV_TOOLS
	if (c.replaced || standin_ == kStandinOff) { return false; }
	const double hold = kStandinHold.get();
	return hold > 0.0 && (double)c.standAge < hold;
#else
	(void)c;
	return false;
#endif
}

int Characters::standinStage(const Char& c) const
{
	if (c.replaced || !c.body || (standin_ == kStandinOff && !c.spec.lowDetail)) { return 0; }
	if (standinHeld(c) || c.fadeAge < 0) { return 1; }	// held, or its GPU objects are still in the stream lane: the stand-in whole
	if (c.fadeAge < kStandinShape) { return 2; }
	if (c.fadeAge < kStandinShape + kStandinColour) { return 3; }
	return 0;	// the real look, as before
}

// This build is drawn standing in (its GPU objects are queued) or inside the two-stage switch this frame.
bool Characters::standinLive(const Char& c) const { return standinStage(c) != 0; }

bool Characters::standinActive() const
{
	for (const Char& c : chars_) { if (standinLive(c) && !standinHeld(c)) { return true; } }
	return false;
}

// The KO / death fall (kinspect ko_transitions.md: the original ragdolls, there is no fall clip; the real ragdoll is parked). MINE: the interim is a
// cross-fade. The character is rebuilt from the lying spec s (weapons sheathed, no action, the frozen lying clip) but the layers of the pose it had
// are moved in front of the lying layer and fade out over cfg `anim.fall_blend`, so the body merges from the pose it went down in into the lying
// clip, with the root on the sim's position. False: the rebuild failed (the old character stays).
bool Characters::fall(size_t i, const Spec& s, std::string& err)
{
	if (!add(s, err)) { return false; }
	const std::vector<Layer> from = chars_[i].layers;
	swapIn(i);
	Char& c = chars_[i];
	if (from.empty() || c.layers.size() != 1) { return true; }	// no pose to fade from / more than the lying clip: the snap stands
	for (const Layer& l : from) { c.lieFrom.push_back(l.weight); }
	c.layers.insert(c.layers.begin(), from.begin(), from.end());
	c.lieLayer = (int)from.size();	// the lying clip's layer, after the old pose's
	c.lieT = 0;
	c.lieDur = (float)kFallBlend.get();
	c.layers[(size_t)c.lieLayer].weight = 0;
	if (c.lieDur <= 0) { finishFall(c); }
	return true;
}

void Characters::finishFall(Char& c)
{
	if (c.lieLayer < 0) { return; }
	const int lie = c.lieLayer - (int)c.lieFrom.size();
	c.layers.erase(c.layers.begin(), c.layers.begin() + (std::ptrdiff_t)c.lieFrom.size());
	c.lieFrom.clear();
	c.lieLayer = -1;
	c.lieT = c.lieDur = 0;
	if (lie >= 0 && (size_t)lie < c.layers.size()) { c.layers[(size_t)lie].weight = 1; }
}

// The get-up (kinspect ko_transitions.md 4: AnimationClassHuman::vf27 returns the animation group "standing up 3" once the down flag clears). The game
// data's ANIMATION record of that name gives the clip and its deletes, the same record -> clip resolution as startAction; MINE: no crossfade, the
// clip's first frame is the lying pose. The character stays out of locomotion (loco false) and holds the clip's last frame once it has played; SimView
// rebuilds it for the walk (a rebuild, never a loco flip: every pose layer must be gone before locomote runs, the record-less layer guard).
bool Characters::rise(size_t i, std::string& err)
{
	Char& c = chars_[i];
	const auto a = anims_.find(folded("standing up 3"));
	if (a == anims_.end()) { err = "no ANIMATION named standing up 3"; return false; }
	if (c.lieLayer >= 0) { finishFall(c); }	// recovered during the fall's fade: the lying clip alone first
	// MINE: a ragdoll-driven body starts the get-up from its last pose, cross-faded into the clip over `anim.rise_blend` (pose() blends it),
	// instead of snapping to the clip's first frame; the ragdoll itself goes (the driver's setRagdoll({}) then has nothing left to clear).
	c.riseFrom = std::move(c.rag);
	c.rag.clear();
	c.riseT = 0;
	c.riseDur = (float)kRiseBlend.get();
	if (c.riseDur <= 0) { c.riseFrom.clear(); }
	const size_t before = c.layers.size();
	Layer* l = addLayer(c, a->second.clip, a->second.deletes, 0.0f, err);
	if (!l) { return false; }
	l->loop = false;
	l->weight = 1;
	c.layers.erase(c.layers.begin(), c.layers.begin() + (std::ptrdiff_t)before);	// the frozen lying clip goes: the get-up starts from lying
	c.action = -1;
	c.idle = -1;
	c.gettingUp = true;
	c.spec.speed = 1;	// plays forward once (update: l.time += dt x speed, loop false)
	return true;
}

// An animal's get-up (plans/animal-down, MINE: the original's animal get-up is untraced, and the animal rigs have no "standing up 3"). The character
// is rebuilt from the live spec s and the ragdoll's last pose cross-fades into it over cfg `anim.rise_blend` (pose() blends riseFrom); at the blend's
// end the walk has it as usual. False: the rebuild failed, the old character (the ragdoll-driven pose) stays.
bool Characters::riseLive(size_t i, const Spec& s, std::string& err)
{
	if (i >= chars_.size()) { err = "no character " + std::to_string(i); return false; }
	const std::vector<RagBone> from = chars_[i].rag;	// copied: a failed rebuild keeps the ragdoll's pose, and the driver re-sets it every update
	if (!add(s, err)) { return false; }
	swapIn(i);
	Char& c = chars_[i];
	c.riseFrom = from;
	c.riseT = 0;
	c.riseDur = (float)kRiseBlend.get();
	if (c.riseDur <= 0) { c.riseFrom.clear(); }
	return true;
}

void Characters::keep(const std::vector<int>& from, const std::vector<int>& departing)
{
	// the common case (a burst adds, nobody left or moved): from is 0..n-1, so the first n stay where they are and only the rest go, without moving
	// every character (0.2-0.7 ms a rebuild frame for a crowd; kenshi-k16.2.205.12.77)
	bool prefix = departing.empty() && from.size() <= chars_.size();
	for (size_t i = 0; i < from.size() && prefix; i++) { prefix = from[i] == (int)i; }
	if (prefix)
	{
		const int n = (int)from.size();
		chars_.erase(chars_.begin() + n, chars_.end());
		for (Char& c : chars_) { if (c.spec.carriedBy >= n) { c.spec.carriedBy = -1; } }
		return;
	}
	std::vector<int> to(chars_.size(), -1);
	std::vector<Char> kept;
	for (const int i : from) { to[(size_t)i] = (int)kept.size(); kept.push_back(std::move(chars_[(size_t)i])); }
	// a carrier that went: carriedBy -1, drawn on its own until the sim view replaces it
	for (Char& c : kept) { if (c.spec.carriedBy >= 0) { c.spec.carriedBy = to[(size_t)c.spec.carriedBy]; } }
	// the characters that left the drawn set: kept after the shown ones, frozen in their last pose and dithered out (kenshi-k16.2.258.10.1)
	for (const int i : departing)
	{
		if (i < 0 || (size_t)i >= chars_.size()) { continue; }
		Char c = std::move(chars_[(size_t)i]);
		c.departing = true;
		c.spec.speed = 0;	// the clip holds its last frame while the body dithers out
		c.spec.carriedBy = c.spec.carriedBy >= 0 ? to[(size_t)c.spec.carriedBy] : -1;
		kept.push_back(std::move(c));
	}
	chars_ = std::move(kept);
}

// StatsEnumerated names (the ids come from the editor's stat-name switch 140888440, via findings/effective_stat.md; 14 / 15 are unnamed there)
static std::string statName(int id)
{
	static const char* const kNames[] = { "", "Strength", "Attack", "Labouring", "Science", "Engineering", "Robotics", "Weapon smith", "Armour smith", "First Aid", "Thievery", "Turrets", "Farming",
		"Cooking", "", "", "Stealth", "Athletics", "Dexterity", "Defence", "Weaponry", "Toughness", "Assassination", "Swimming", "Perception", "Katanas", "Sabres", "Hackers", "Heavy weapons", "Blunt",
		"Martial arts", "Polearms", "Dodging", "", "", "Crossbows", "Precision shooting", "Lockpicking", "" };
	return id > 0 && id < (int)(sizeof kNames / sizeof *kNames) ? kNames[id] : "";
}

Characters::RaceChoices Characters::choices(const std::string& race) const
{
	RaceChoices out;
	const auto ri = std::find_if(races_.begin(), races_.end(), [&](const RaceInfo& r) { return folded(r.name) == folded(race); });
	if (ri == races_.end()) { return out; }
	out.ok = true;
	out.playable = ri->playable;
	out.description = ri->description;
	for (size_t i = 0; i < ri->hairs.size(); i++)
	{
		const auto hi = hair_.find(folded(ri->hairs[i]));
		if (hi == hair_.end()) { continue; }
		const HairInfo& h = hi->second;
		const bool beard = h.slot == 13;
		if (h.mesh[0].empty() && h.mesh[1].empty() && h.head[0].empty() && h.head[1].empty()) { (beard ? out.beardNone : out.hairNone) = ri->hairWeights[i]; continue; }
		(beard ? out.beards : out.hairs).push_back(ri->hairs[i]);
		(beard ? out.beardWeights : out.hairWeights).push_back(ri->hairWeights[i]);
	}
	out.hairColours = ri->hairColours;
	for (const float w : ri->hairColourWeights) { out.hairColourWeights.push_back(w == 0 ? 100.0f : w); }	// findings/charedit8b_static.md
	// RACE STATS (1405ee0c0 [1405eda60], findings/charedit8b_static.md): "Hit Points" = the `combat anatomy` health (v1) of the first part in body
	// order (the head), 100 when below 1; "No bleeding" when `bleed rate` < 0.1. Multipliers: `stats good` 1.2, `stats bad` 0.8, `strength` /
	// `dexterity` times that.
	const auto head = ri->anatomy.empty() ? ri->partMax.end() : ri->partMax.find(ri->anatomy[0].first);
	out.hp = head != ri->partMax.end() && head->second >= 1 ? head->second : 100.0f;
	out.noBleeding = ri->bleedRate < 0.1f;
	std::map<int, float> mult;
	for (const int id : ri->statsGood) { mult.try_emplace(id, 1.0f).first->second *= 1.2f; }
	for (const int id : ri->statsBad) { mult.try_emplace(id, 1.0f).first->second *= 0.8f; }
	mult.try_emplace(1, 1.0f).first->second *= ri->strength;
	mult.try_emplace(18, 1.0f).first->second *= ri->dexterity;
	// UNSURE: the panel's order, inferred from the three measured races (charedit.md): one list, Strength after Attack when green and last when red
	static const int kOrder[] = { 37, 24, 21, 2, 1, 18, 17, 16, 12, 13, 10, 9, 6, 4, 3, 32, 5, 7, 8, 11, 19, 20, 22, 23, 25, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36, 38 };
	for (const bool green : { true, false })
	{
		std::vector<int> ids(std::begin(kOrder), std::end(kOrder));
		if (!green) { ids.erase(std::find(ids.begin(), ids.end(), 1)); ids.push_back(1); }
		for (const int id : ids)
		{
			const auto m = mult.find(id);
			if (m == mult.end() || std::lround(std::fabs(m->second - 1.0f) * 100) == 0 || (m->second > 1.0f) != green || statName(id).empty()) { continue; }
			out.skills.push_back({ statName(id), m->second });
		}
	}
	const std::string editor = ri->editor.empty() ? std::string() : res_->locate(ri->editor);
	for (int g = 0; g < 2; g++)
	{
		out.heads[g] = (int)ri->heads[g].size();
		if (!editor.empty()) { out.ranges[g] = readRanges(editor, g == 1); out.skinStops[g] = readSkinStops(editor, g == 1); }
	}
	return out;
}

std::vector<std::pair<std::string, std::vector<std::string>>> Characters::raceGroups() const
{
	const std::vector<std::string> playable = races(true);
	std::vector<std::pair<std::string, std::vector<std::string>>> out;
	for (const auto& [group, names] : raceGroups_)
	{
		std::vector<std::string> keep;
		for (const std::string& n : names) { if (std::find(playable.begin(), playable.end(), n) != playable.end()) { keep.push_back(n); } }
		if (!keep.empty()) { out.push_back({ group, std::move(keep) }); }
	}
	return out;
}

std::vector<std::string> Characters::races(bool playableOnly) const
{
	std::vector<std::string> out;
	for (const RaceInfo& r : races_)
	{
		if ((r.mesh[0].empty() && r.mesh[1].empty()) || (playableOnly && (!r.playable || r.editor.empty()))) { continue; }
		out.push_back(r.name);
	}
	return out;
}

Characters::Voice Characters::voiceOf(const std::string& race, bool female) const
{
	static const char* const kHuman[] = { "Human", "Shek", "Hive", "Robot" };
	static const char* const kCreature[] = { "Beak", "Bull", "Cage", "Dog", "Duck", "Gar", "Goat", "Gorillo", "IronSpider", "Leviathan", "Pack", "Spider", "Turtle" };
	Voice v;
	const RaceInfo* ri = nullptr;
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(race)) { ri = &r; break; } }
	if (!ri) { return v; }
	if (ri->sounds >= 4)
	{
		v.animal = true;
		const std::string name = kCreature[std::clamp(ri->sounds - 4, 3, 12)];	// min Dog (rwh65/switches.tsv)
		v.switches = { { "Creature_VO", name }, { "Weapons", name } };
	}
	else { v.switches = { { "VO_Races", std::string(kHuman[std::clamp(ri->sounds, 0, 3)]) + (female ? "_Female" : "_Male") } }; }
	return v;
}

bool Characters::animal(size_t i) const
{
	if (i >= chars_.size()) { return false; }
	return voiceOf(chars_[i].spec.race, chars_[i].spec.female).animal;
}

bool Characters::isWeapon(const std::string& name) const
{
	return weapons_.count(folded(name)) != 0;
}

std::string Characters::nameOf(const std::string& sid) const
{
	const auto it = sidNames_.find(sid);
	return it == sidNames_.end() ? sid : it->second;
}

void Characters::hull(const std::string& race, float out[3]) const
{
	const float kHuman[3] = { 5, 20, 5 };
	const float* h = kHuman;
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(race)) { h = r.hull; } }
	for (int k = 0; k < 3; k++) { out[k] = h[k]; }
}

bool Characters::pathBody(const std::string& race, double& radius, float& water) const
{
	radius = 2.6;
	water = 1.0f;
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(race)) { radius = r.footprint; water = r.waterCost; return true; } }
	return false;
}

int Characters::limbSlot(const std::string& race, const std::string& part) const
{
	for (const RaceInfo& r : races_)
	{
		if (folded(r.name) != folded(race)) { continue; }
		for (const auto& [name, bones] : r.anatomy)
		{
			if (name != part) { continue; }
			for (const std::string& b : bones)
			{
				for (int k = 0; k < 4; k++) { if (b == std::string("Bip01 ") + (k % 2 ? "R " : "L ") + (k < 2 ? "Forearm" : "Calf")) { return k; } }
			}
		}
	}
	return -1;
}

float Characters::partMaxHealth(const std::string& race, const std::string& part) const
{
	for (const RaceInfo& r : races_)
	{
		if (folded(r.name) != folded(race)) { continue; }
		const auto it = r.partMax.find(part);
		return it != r.partMax.end() ? it->second : 0.0f;
	}
	return 0.0f;
}

// The type-105 notifies a record carries (its `events` refs; findings anim_sfx_notifies.md): the ref's int is a percent of the clip, or on a COMBAT_TECHNIQUE
// a frame over `num frames` (1 frame = the percent rule too). Sorted ascending. The record's `event` / `override state` / `override value` / `stop`.
std::vector<Characters::Note> Characters::notesOf(const kdata::GameData& gd, const kdata::Record* owner, bool technique)
{
	std::vector<Note> out;
	const kdata::RefCategory* refs = owner->findRefs("events");
	if (!refs) { return out; }
	const float frames = technique ? owner->floatOr("num frames", 1.0f) : 1.0f;
	for (const kdata::RefEntry& e : refs->entries)
	{
		const kdata::Record* t = gd.find(e.targetID);
		if (!t || t->stringOr("event", "").empty()) { continue; }
		Note n;
		n.at = technique && frames > 1.0f ? (float)e.v0 / frames : (float)e.v0 * 0.01f;
		n.event = t->stringOr("event", "");
		n.group = t->stringOr("override state", "");
		n.value = t->stringOr("override value", "");
		n.stop = t->stringOr("stop", "");
		out.push_back(std::move(n));
	}
	std::stable_sort(out.begin(), out.end(), [](const Note& a, const Note& b) { return a.at < b.at; });
	return out;
}

std::string Characters::armourSwitch(size_t i) const
{
	static const char* const kNames[] = { "Cloth", "Leather", "Chain", "Plate" };
	int cls = 0;
	for (const std::string& w : chars_[i].spec.wear) { if (const auto a = armour_.find(folded(w)); a != armour_.end()) { cls = std::max(cls, a->second.armourClass); } }
	return kNames[cls];
}

std::string Characters::weaponSwitch(size_t i) const
{
	static const char* const kNames[] = { "Katanas", "Sabres", "Blunt", "Heavy", "Hackers", "", "Bow", "Turret", "Blunt" };
	const Char& c = chars_[i];
	return c.handR && c.weaponCategory >= 0 && c.weaponCategory < 9 ? kNames[c.weaponCategory] : "";
}

std::string Characters::noteReport() const
{
	size_t anims = 0, techs = 0, notes = 0;
	for (const auto& [k, a] : anims_) { if (!a.notes.empty()) { anims++; notes += a.notes.size(); } }
	for (const auto& [k, t] : techniques_) { if (!t.notes.empty()) { techs++; notes += t.notes.size(); } }
	char b[160];
	snprintf(b, sizeof b, "%zu notifies on %zu ANIMATION records and %zu COMBAT_TECHNIQUE records", notes, anims, techs);
	return b;
}

// Fire the notifies the playing layers passed (AnimationEvents::update 140064690): against the clip's normalised progress, ascending, each again on every
// loop. A locomotion layer counts while it carries the weight (> 0.5: a crossfade must not post both clips' steps; UNSURE, the game's state weight rule is
// not traced); the action layer always. A layer first seen only records its progress (a step already past is not posted late). Backward play (a body
// backing away) and a jump back that is not a wrap re-arm without firing.
void Characters::fireNotes(Char& c, size_t index)
{
	for (size_t li = 0; li < c.layers.size(); li++)
	{
		Layer& l = c.layers[li];
		const bool isAction = (int)li == c.action;
		const std::vector<Note>* notes = isAction ? c.actionNotes : l.info && l.info != &actionInfo_ ? &l.info->notes : nullptr;
		const float len = l.clip ? l.clip->length : 0;
		if (!notes || notes->empty() || len <= 0 || (!isAction && (l.weight <= 0.5f || l.rate < 0))) { l.noted = -1; continue; }
		const float p = l.time / len;
		if (l.noted < 0)
		{
			// an action's first frame posts a note at 0 (the animals' swings); one already well under way when the wire told us (a reconnect, a late
			// frame: more than 0.25 s of clip) does not post its early notes late
			l.noted = isAction && l.time <= 0.25f * c.actionSpeed ? -1e-4f : p;
			if (!isAction || l.noted >= 0) { continue; }
		}
		float from = l.noted;
		const bool wrapped = p < from && from - p > 0.5f;
		if (p < from && !wrapped) { l.noted = p; continue; }
		l.noted = std::max(p, 0.0f) + 1e-6f;
		const auto post = [&](const Note& n)
		{
			SoundNote s;
			s.index = index;
			s.event = n.event;
			s.group = n.group;
			s.value = n.value;
			if (isAction) { s.action = c.actionName; }
			s.gate = n.event == "Footsteps" && n.group.empty() ? 200.0f : 1000.0f;
			sounds_.push_back(std::move(s));
		};
		for (const Note& n : *notes)
		{
			const bool pass = wrapped ? (n.at > from || n.at <= p) : (n.at > from && n.at <= p) || (from < 0 && n.at == 0.0f && p >= 0);
			if (pass) { post(n); }
		}
	}
}

void Characters::update(double dt, double fadeDt)
{
	if (fadeDt < 0) { fadeDt = dt; }
	if (cfg::generation() != cfgGen_)
	{
		cfgGen_ = cfg::generation(); strideWarp_ = kStrideWarp.get(); stopContact_ = kStopContact.get(); footLock_ = kFootLock.get(); footTerrain_ = kFootTerrain.get(); actionRoot_ = kActionRoot.get(); inertOn_ = kInertialize.get();
		const std::string m = kLoadingStandin.get();	// render.loading_standin
		standin_ = m == "double" ? kStandinDouble : m == "off" ? kStandinOff : kStandinSilhouette;
		charFade_ = kCharFade.get();	// render.char_fade
	}
	for (Char& c : chars_)
	{
		// the arrival / departure dither (render.char_fade, kenshi-k16.2.258.10.1), on the frame's own dt like the stand-in's clocks; a body with no
		// sim id (a scene's, the creator's) is never an arrival and stays whole
		if (c.departing)
		{
			c.cover = charFade_ > 0 ? std::max(0.0f, c.cover - (float)(fadeDt / charFade_)) : 0.0f;
			if (c.cover <= 0) { departDone_ = true; }
		}
		else if (c.cover < 1 && c.spec.id) { c.cover = charFade_ > 0 ? std::min(1.0f, c.cover + (float)(fadeDt / charFade_)) : 1.0f; }
		// the loading stand-in's clocks (kenshi-k16.2.258.1), on the frame's own dt: it stands in while the build's GPU objects are queued (or the dev
		// hold keeps it), then the shape morph (stage 1) and the look's dither fade (stage 2) run on. A build that replaced one already on screen
		// (swapIn) appears without either: it had a body drawn a frame ago (standinStage reads that).
		if (standinHeld(c)) { c.fadeAge = -1; c.standAge += (float)fadeDt; }	// the dev hold: back to the stand-in whole; the switch runs again when it lets go
		else if (c.fadeAge < 0)
		{
			if (drawable(c)) { c.fadeAge = 0; }
			else { c.standAge += (float)fadeDt; }
		}
		else if (c.fadeAge < kStandinShape + kStandinColour) { c.fadeAge += (float)fadeDt; }
		if (c.was && drawable(c) && !standinHeld(c)) { c.was.reset(); }	// the rebuild's GPU objects are in: the old build goes
		c.warped = c.steered = false;	// this frame's locomotion flags (log feet): locomote sets them
		if (c.riseDur > 0)	// the get-up blend's clock; at its end the ragdoll's pose is gone
		{
			c.riseT = std::min(c.riseDur, c.riseT + (float)dt);
			if (c.riseT >= c.riseDur) { c.riseFrom.clear(); c.riseT = c.riseDur = 0; }
		}
		if (c.spec.loco) { CPUSTAGE_ACC("locomote+notes"); locomote(c, (float)dt * c.spec.speed, false); fireNotes(c, (size_t)(&c - chars_.data())); continue; }
		if (c.lieLayer >= 0)
		{
			// the KO fall's cross-fade (MINE): the pose it went down in fades out while the lying clip fades in; the times stay frozen (spec.speed 0)
			c.lieT = std::min(c.lieDur, c.lieT + (float)dt);
			const float t = c.lieDur > 0 ? c.lieT / c.lieDur : 1.0f;
			for (size_t j = 0; j < c.lieFrom.size(); j++) { c.layers[j].weight = c.lieFrom[j] * (1.0f - t); }
			c.layers[(size_t)c.lieLayer].weight = t;
			if (t >= 1.0f) { finishFall(c); }
		}
		const Layer* lead = nullptr;	// the first synching layer: the others take its phase
		for (Layer& l : c.layers)
		{
			const float len = l.clip->length;
			if (len <= 0) { continue; }
			if (lead && l.synchs)
			{
				const float phase = lead->time / lead->clip->length + l.synchOffset;
				l.time = (phase - std::floor(phase)) * len;
				continue;
			}
			l.time += (float)dt * c.spec.speed;
			if (!l.loop) { l.time = std::clamp(l.time, 0.0f, len); }
			else if (l.reverse)
			{
				// `reverse looping` (the ANIMATION field): the clip runs to its end then back again, never jumping from end to start
				const float span = 2.0f * len;
				const float t = l.time - std::floor(l.time / span) * span;
				l.time = t <= len ? t : span - t;
			}
			else { l.time = l.time - std::floor(l.time / len) * len; }
			if (l.synchs && !lead) { lead = &l; }
		}
		// the get-up clip is clamped at its end (loop false): from then on the character holds the standing pose until SimView hands it to locomotion
		if (c.gettingUp && !c.layers.empty() && c.layers.back().time >= c.layers.back().clip->length) { c.gettingUp = false; }
	}
	// The pose pass (kenshi-k16.2.205.12.102, overlap review .85 B2 / T22): every character's pose reads and writes its own state only, so the
	// poses run as a parallel-for over characters (anim.pose_threads). The one cross-character read is a carried body's carryPlace (the carrier's
	// shoulder): it keeps the serial loop's order exactly - a carrier earlier in chars_ is read posed this frame, one later as it stood before
	// this loop (snapshotted below) - so the output is the serial loop's, bit for bit, at any thread count. In order: (1) the per-character
	// prelude, serial (its once-only reports print in index order); (2) pose + the idle sole fit of every character not carried, in parallel;
	// (3) the carried, serial in index order; (4) the knee report, serial in index order.
	const size_t n = chars_.size();
	std::vector<uint8_t> idleOf(n, 0);
	std::vector<size_t> carried;
	std::vector<CarrierAt> carrierBefore;	// per carried entry, the carrier's state before this loop when it comes later in chars_
	for (size_t i = 0; i < n; i++)
	{
		Char& c = chars_[i];
		if (c.spec.carriedBy < 0) { continue; }
		carried.push_back(i);
		CarrierAt at;
		if ((size_t)c.spec.carriedBy > i)
		{
			const Char& k = chars_[(size_t)c.spec.carriedBy];
			at.snap = true;
			at.model = k.model;
			at.orient = k.orient;
			for (int a = 0; a < 3; a++) { at.pos[a] = k.pos[a]; }
		}
		carrierBefore.push_back(std::move(at));
	}
	for (size_t i = 0; i < n; i++)
	{
		Char& c = chars_[i];
		const bool idle = c.spec.loco && std::fabs(c.spec.move) < 0.5f && std::fabs(c.spec.strafe) < 0.5f && c.spec.action.empty();
		idleOf[i] = idle;
		c.idleFor = idle ? c.idleFor + (float)dt : 0.0f;
		if (c.spec.carriedBy >= 0) { continue; }	// (3): its node is the carrier's shoulder
		if (!c.soleRest && c.body && c.spec.loco) { c.soleLift = restLift(c); c.soleRest = true; }
		for (int k = 0; k < 3; k++) { c.pos[k] = c.spec.pos[k]; }
		c.pos[1] += c.rootShift - (c.spec.loco ? c.soleLift : 0.0f);	// normal locomotion only: carried (carryPlace) bypasses it, as 1405c84e0 does
		c.orient = { std::cos(c.spec.yaw * 0.5f), 0, std::sin(c.spec.yaw * 0.5f), 0 };
		if (previewOrbit_ && i == previewIndex_)	// the creator's assembly (setPreviewOrbit): p' = center + q x (p - center) + shift, the platform draws with the same q
		{
			const kworld::SkelVec3 r = kworld::rotate(previewQ_, { (float)(c.pos[0] - previewCenter_[0]), (float)(c.pos[1] - previewCenter_[1]), (float)(c.pos[2] - previewCenter_[2]) });
			c.pos[0] = previewCenter_[0] + previewShift_[0] + r.x;
			c.pos[1] = previewCenter_[1] + previewShift_[1] + r.y;
			c.pos[2] = previewCenter_[2] + previewShift_[2] + r.z;
			c.orient = kworld::multiply(previewQ_, c.orient);
		}
		// anim.action_root (fix 2): an action draws along its clip root; once it is not, the hand-off offset eases onto the wire
		if (actionPath(c, (float)dt)) { }
		else if (c.actWas)
		{
			c.pos[0] += c.actG[0];
			c.pos[2] += c.actG[1];
			const float e = std::min(1.0f, (float)dt / kActEase);
			c.actG[0] -= c.actG[0] * e;
			c.actG[1] -= c.actG[1] * e;
			if (std::hypot(c.actG[0], c.actG[1]) < 1e-3) { c.actWas = false; c.actG[0] = c.actG[1] = 0; }
		}
		// a non-finite drawn node would reach the heightmap and abort the run: the anchor refuses a non-finite wire sample, this catches one that
		// arrives mid-action (the clip root or the wire); report once and draw the character at the wire from there on
		if (!std::isfinite(c.pos[0]) || !std::isfinite(c.pos[2]))
		{
			static int nanN = 0;
			if (nanN++ < 6)
				fprintf(stderr, "kclient: action path%s: %s: non-finite node (wire %.2f %.2f yaw %.2f), path off\n",
					idTag(c.spec).c_str(), c.spec.action.c_str(), c.spec.pos[0], c.spec.pos[2], c.spec.yaw);
			c.pos[0] = c.spec.pos[0];
			c.pos[2] = c.spec.pos[2];
			c.actPath = false;
			c.actWas = false;
			c.actG[0] = c.actG[1] = c.actCorr[0] = c.actCorr[1] = 0;
			c.actAnchored = false;
		}
	}
	// kenshi-k16.17.7.2: soleLift is the body's standing offset (restLift), constant: no live measure, no easing. soleFit (what the idle fit
	// waits for) is read from the idle's blended pose.
	// soleFit only ever goes true (it is cleared when the character is made): measure the soles until they fit once. soles() skins every vertex of the
	// body on the CPU (~0.13 ms a character); run every frame for each idle character it was 13 of the 15 ms of chars.update at Heft (kenshi-k16.2.205.1)
	auto poseOne = [&](size_t i)
	{
		Char& c = chars_[i];
		{ CPUSTAGE_ACC("pose"); pose(c, (float)dt); }
		double s[2][3];
		if (idleOf[i] && !c.soleFit) { CPUSTAGE_ACC("soles"); if (solesOf(c, s)) { c.soleFit = true; } }
	};
	{
		CPUSTAGE_ACC("pose.pass");	// the parallel pass's wall time on this thread (+pose / +soles are the summed time of every thread)
		std::vector<size_t> own;	// the characters not carried
		own.reserve(n);
		for (size_t i = 0; i < n; i++) { if (chars_[i].spec.carriedBy < 0) { own.push_back(i); } }
		posePool(own.size()).run(own.size(), [&](size_t k) { poseOne(own[k]); }, [] { CPUSTAGE_MERGE_TO_CALLER(); });
		CPUSTAGE_MERGED_FROM_WORKERS();
	}
	for (size_t k = 0; k < carried.size(); k++)
	{
		Char& c = chars_[carried[k]];
		const Char& by = chars_[(size_t)c.spec.carriedBy];
		const CarrierAt& at = carrierBefore[k];
		if (at.snap) { carryPlace(c, by, at.model, at.orient, at.pos); }
		else { carryPlace(c, by, by.model, by.orient, by.pos); }
		c.footLift = 0;
		CPUSTAGE_ACC("pose");
		pose(c, (float)dt);
	}
	for (Char& c : chars_) { if (c.spec.loco) { CPUSTAGE_ACC("kneeReport"); kneeReport(c); } }
}

bool Characters::ragdollStart(size_t i, std::vector<RagBone>& io) const
{
	if (i >= chars_.size() || chars_[i].model.empty()) { return false; }
	const Char& c = chars_[i];
	for (RagBone& r : io)
	{
		if (r.bone < 0 || (size_t)r.bone >= c.model.size()) { continue; }
		const kworld::SkelTransform& m = c.model[(size_t)r.bone];
		const kworld::SkelVec3 o = kworld::rotate(c.orient, m.pos);
		r.p[0] = c.pos[0] + o.x; r.p[1] = c.pos[1] + o.y; r.p[2] = c.pos[2] + o.z;
		r.q = kworld::multiply(c.orient, m.rot);
	}
	return true;
}

bool Characters::ragdollStartAt(size_t i, std::vector<RagBone>& io, double yaw, const double at[3]) const
{
	if (!ragdollStart(i, io)) { return false; }
	const Char& c = chars_[i];
	const double nodeYaw = 2.0 * std::atan2((double)c.orient.y, (double)c.orient.w);	// orient is the node's pure yaw rotation
	const double d = yaw - nodeYaw, cs = std::cos(d), sn = std::sin(d);
	const kworld::SkelQuat turn = { (float)std::cos(d * 0.5), 0, (float)std::sin(d * 0.5), 0 };
	for (RagBone& r : io)
	{
		const double x = r.p[0] - c.pos[0], z = r.p[2] - c.pos[2];
		r.p[0] = at[0] + x * cs + z * sn;
		r.p[1] = at[1] + (r.p[1] - c.pos[1]);
		r.p[2] = at[2] - x * sn + z * cs;
		r.q = kworld::multiply(turn, r.q);
	}
	return true;
}

void Characters::setRagdoll(size_t i, std::vector<RagBone> v)
{
	if (i < chars_.size()) { chars_[i].rag = std::move(v); }
}

// The race's own ragdoll (RACE `male ragdoll` / `female ragdoll`): the original's KO / death body for every race (CLIENT_GOALS "Ragdolls"), and the
// body of a carried animal too (the RACE `carry ragdoll` fields are dead data in the exe, rwh.118). The record path (".\data\animal\ragdoll\mammal_beak.phs")
// is resolved like the race meshes; a female field left empty falls back to the male one (the data's rule for the other gendered fields).
// "" = the record names none: no ragdoll, nothing substituted.
std::string Characters::ragdollFile(size_t i) const
{
	if (i >= chars_.size()) { return std::string(); }
	const Spec& s = chars_[i].spec;
	const RaceInfo* ri = nullptr;
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(s.race)) { ri = &r; break; } }
	if (!ri) { return std::string(); }
	const std::string* field = ri->ragdoll;
	const std::string& path = s.female && !field[1].empty() ? field[1] : field[0];
	if (path.empty()) { return std::string(); }
	const std::string at = res_ ? res_->locate(path) : std::string();
	return at.empty() ? path : at;	// unresolved: hand the loader the record path, which reports it as a bad file
}

// The bone a carried animal's ragdoll is pinned by (RACE `carry bone`; the original's ragdoll bone setter, 1407d3850): the record's name when the
// skeleton has it, else the skeleton's root bone.
std::string Characters::carryBone(size_t i) const
{
	if (i >= chars_.size() || !chars_[i].body) { return std::string(); }
	const Char& c = chars_[i];
	const kworld::Skeleton& sk = c.body->skel;
	for (const RaceInfo& r : races_)
	{
		if (folded(r.name) == folded(c.spec.race)) { if (!r.carryBone.empty() && sk.bone(r.carryBone) >= 0) { return r.carryBone; } break; }
	}
	for (size_t b = 0; b < sk.bones.size(); b++) { if (sk.bones[b].parent < 0) { return sk.bones[b].name; } }
	return std::string();
}

bool Characters::carryFrame(size_t i, double p[3], kworld::SkelQuat& q) const
{
	if (i >= chars_.size() || !chars_[i].body || chars_[i].spec.carry == 0) { return false; }
	const Char& c = chars_[i];
	const int b = c.body->skel.bone(c.spec.carry == 1 ? "Bip01 L UpperArm" : "Bip01 R UpperArm");
	if (b < 0 || (size_t)b >= c.model.size()) { return false; }
	const kworld::SkelVec3 o = kworld::rotate(c.orient, c.model[(size_t)b].pos);
	p[0] = c.pos[0] + o.x;
	p[1] = c.pos[1] + o.y;
	p[2] = c.pos[2] + o.z;
	q = kworld::multiply(c.orient, c.model[(size_t)b].rot);
	return true;
}

void Characters::nodeWorld(size_t i, double out[3]) const
{
	const Char& c = chars_[i];
	for (int k = 0; k < 3; k++) { out[k] = c.pos[k]; }
}

bool Characters::boneWorld(size_t i, const char* name, double out[3]) const
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	const Char& c = chars_[i];
	const int b = c.body->skel.bone(name);
	if (b < 0 || (size_t)b >= c.model.size()) { return false; }
	const kworld::SkelVec3 o = kworld::rotate(c.orient, c.model[(size_t)b].pos);
	out[0] = c.pos[0] + o.x;
	out[1] = c.pos[1] + o.y;
	out[2] = c.pos[2] + o.z;
	return true;
}

void Characters::setPreviewOrbit(size_t i, const double center[3], const kworld::SkelQuat& q, const double shift[3])
{
	previewOrbit_ = true;
	previewIndex_ = i;
	previewQ_ = q;
	for (int k = 0; k < 3; k++) { previewCenter_[k] = center[k]; previewShift_[k] = shift[k]; }
}

// The pick capsules (see the header). Built from the first posed frame: every skinned vertex goes to its heaviest bone (weight >= 0.4), in that bone's
// frame; the capsule's axis runs from the bone origin to the vertices' centroid (the longest local axis when they surround the origin), its length is
// their extent along it and its radius the 85th percentile of their distance to it (a stray weighted vertex does not fatten the limb).
int Characters::pickCapsules(size_t i, std::vector<PickCapsule>& out) const
{
	if (i >= chars_.size() || !chars_[i].body || chars_[i].palette.empty() || chars_[i].model.empty()) { return 0; }
	const Char& c = chars_[i];
	const auto frame = [&](size_t b, double o[3], kworld::SkelQuat& q)
	{
		const kworld::SkelVec3 r = kworld::rotate(c.orient, c.model[b].pos);
		o[0] = c.pos[0] + r.x; o[1] = c.pos[1] + r.y; o[2] = c.pos[2] + r.z;
		q = kworld::multiply(c.orient, c.model[b].rot);
	};
	if (!c.capsBuilt)
	{
		c.capsBuilt = true;
		const size_t nb = c.model.size();
		std::vector<std::vector<std::array<float, 3>>> pts(nb);
		auto scan = [&](const ogre::Geometry& g)
		{
			if (g.boneIndex.empty()) { return; }
			for (uint32_t v = 0; v < g.count; v++)
			{
				int heaviest = -1;
				float top = 0;
				double p[3] = {};
				for (int k = 0; k < 4; k++)
				{
					const uint8_t b = g.boneIndex[v * 4 + k];
					const float w = g.boneWeight[v * 4 + k];
					if (w <= 0) { continue; }
					if (w > top) { top = w; heaviest = (int)b; }
					const float* m = &c.palette[(size_t)b * 12];
					for (int r = 0; r < 3; r++) { p[r] += w * (m[r * 4] * g.pos[v * 3] + m[r * 4 + 1] * g.pos[v * 3 + 1] + m[r * 4 + 2] * g.pos[v * 3 + 2] + m[r * 4 + 3]); }
				}
				if (heaviest < 0 || top < 0.4f || (size_t)heaviest >= nb) { continue; }
				double o[3];
				kworld::SkelQuat q;
				frame((size_t)heaviest, o, q);
				const kworld::SkelQuat qi{ q.w, -q.x, -q.y, -q.z };
				const kworld::SkelVec3 l = kworld::rotate(qi, kworld::SkelVec3{ (float)(c.pos[0] + p[0] - o[0]), (float)(c.pos[1] + p[1] - o[1]), (float)(c.pos[2] + p[2] - o[2]) });
				pts[(size_t)heaviest].push_back({ l.x, l.y, l.z });
			}
		};
		if (c.body->src.hasShared) { scan(c.body->src.shared); }
		for (const ogre::Submesh& sm : c.body->src.submeshes) { if (!sm.shared) { scan(sm.geometry); } }
		for (size_t b = 0; b < nb; b++)
		{
			const auto& v = pts[b];
			if (v.size() < 6) { continue; }
			double cen[3] = {}, lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
			for (const auto& q : v) { for (size_t k = 0; k < 3; k++) { cen[k] += q[k]; lo[k] = std::min(lo[k], (double)q[k]); hi[k] = std::max(hi[k], (double)q[k]); } }
			for (int k = 0; k < 3; k++) { cen[k] /= (double)v.size(); }
			double d[3] = { cen[0], cen[1], cen[2] };
			double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (len < 1.5)
			{
				int ax = 0;
				for (int k = 1; k < 3; k++) { if (hi[k] - lo[k] > hi[ax] - lo[ax]) { ax = k; } }
				d[0] = d[1] = d[2] = 0;
				d[ax] = 1;
				len = 1;
			}
			for (int k = 0; k < 3; k++) { d[k] /= len; }
			double tmin = 1e30, tmax = -1e30;
			std::vector<float> perp;
			perp.reserve(v.size());
			for (const auto& q : v)
			{
				const double t = q[0] * d[0] + q[1] * d[1] + q[2] * d[2];
				tmin = std::min(tmin, t);
				tmax = std::max(tmax, t);
				const double e[3] = { q[0] - d[0] * t, q[1] - d[1] * t, q[2] - d[2] * t };
				perp.push_back((float)std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
			}
			const size_t nth = perp.size() * 85 / 100;
			std::nth_element(perp.begin(), perp.begin() + (long)nth, perp.end());
			const double r = std::max(0.5f, perp[nth]);
			double t0 = tmin + r, t1 = tmax - r;
			if (t1 < t0) { t0 = t1 = 0.5 * (tmin + tmax); }
			Char::BoneCap cp{ (int)b, {}, {}, (float)r };
			for (int k = 0; k < 3; k++) { cp.a[k] = (float)(d[k] * t0); cp.b[k] = (float)(d[k] * t1); }
			c.caps.push_back(cp);
		}
	}
	int n = 0;
	for (const Char::BoneCap& cp : c.caps)
	{
		double o[3];
		kworld::SkelQuat q;
		frame((size_t)cp.bone, o, q);
		PickCapsule w;
		const float* e[2] = { cp.a, cp.b };
		double* dst[2] = { w.a, w.b };
		for (int s = 0; s < 2; s++)
		{
			const kworld::SkelVec3 r = kworld::rotate(q, kworld::SkelVec3{ e[s][0], e[s][1], e[s][2] });
			dst[s][0] = o[0] + r.x; dst[s][1] = o[1] + r.y; dst[s][2] = o[2] + r.z;
		}
		w.r = cp.r;
		out.push_back(w);
		n++;
	}
	return n;
}

// The ray's nearest point to a bone inside `radius` (see the header): bones are the skeleton's own joints, so the ray through a hand picks the hand.
bool Characters::skeletonRay(size_t i, const double origin[3], const double dir[3], double hit[3], double radius) const
{
	if (i >= chars_.size()) { return false; }
	const Char& c = chars_[i];
	double best = radius * radius;
	bool any = false;
	for (const kworld::SkelTransform& b : c.model)
	{
		const kworld::SkelVec3 o = kworld::rotate(c.orient, b.pos);
		const double w[3] = { c.pos[0] + o.x - origin[0], c.pos[1] + o.y - origin[1], c.pos[2] + o.z - origin[2] };
		const double t = w[0] * dir[0] + w[1] * dir[1] + w[2] * dir[2];
		if (t <= 0) { continue; }
		const double d2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2] - t * t;
		if (d2 >= best) { continue; }
		best = d2;
		any = true;
		for (int k = 0; k < 3; k++) { hit[k] = origin[k] + dir[k] * t; }
	}
	return any;
}

std::vector<std::pair<std::string, std::string>> Characters::animationRecords() const
{
	std::vector<std::pair<std::string, std::string>> out;
	out.reserve(animOrder_.size());
	for (const AnimInfo* a : animOrder_) { out.emplace_back(a->name, a->clip); }
	return out;
}

bool Characters::setClipTime(size_t i, float t)
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	Char& c = chars_[i];
	for (Layer& l : c.layers)
	{
		if (!l.clip) { continue; }
		const float len = l.clip->length;
		if (len <= 0) { l.time = 0; }
		else if (l.loop) { l.time = t - std::floor(t / len) * len; }
		else { l.time = std::clamp(t, 0.0f, len); }
	}
	pose(c);
	return true;
}

bool Characters::portraitCopy(size_t i, const double cam[3], FrameData& out)
{
	out.clear();
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	const std::vector<Char> one(1, chars_[i]);	// update() baked this frame's pose; the copy shares its meshes
	buildSet(out, one, false, cam, nullptr, kPortraitSkins, kPortraitInsts, kPortraitList);
	out.count = 1;
	return true;
}

bool Characters::soles(size_t i, double out[2][3]) const
{
	return i < chars_.size() && solesOf(chars_[i], out);
}

bool Characters::solesOf(const Char& c, double out[2][3])
{
	if (!c.body || c.palette.empty()) { return false; }
	const kworld::Skeleton& sk = c.body->skel;
	int side[256];
	std::fill(side, side + 256, -1);
	for (const char* name : { "Bip01 L Foot", "Bip01 L Toe0", "Bip01 R Foot", "Bip01 R Toe0" })
	{
		const int b = sk.bone(name);
		if (b >= 0 && b < 256) { side[b] = name[6] == 'R'; }
	}
	for (int f = 0; f < 2; f++) { out[f][1] = 1e30; }
	auto scan = [&](const ogre::Geometry& g)
	{
		if (g.boneIndex.empty()) { return; }
		for (uint32_t v = 0; v < g.count; v++)
		{
			int f = -1;
			double p[3] = {};
			for (int k = 0; k < 4; k++)
			{
				const uint8_t b = g.boneIndex[v * 4 + k];
				const float w = g.boneWeight[v * 4 + k];
				if (w <= 0) { continue; }
				if (side[b] >= 0 && w >= 0.5f) { f = side[b]; }
				const float* m = &c.palette[(size_t)b * 12];
				for (int r = 0; r < 3; r++) { p[r] += w * (m[r * 4] * g.pos[v * 3] + m[r * 4 + 1] * g.pos[v * 3 + 1] + m[r * 4 + 2] * g.pos[v * 3 + 2] + m[r * 4 + 3]); }
			}
			if (f < 0 || c.pos[1] + p[1] >= out[f][1]) { continue; }
			for (int r = 0; r < 3; r++) { out[f][r] = c.pos[r] + p[r]; }
		}
	};
	if (c.body->src.hasShared) { scan(c.body->src.shared); }
	for (const ogre::Submesh& sm : c.body->src.submeshes) { if (!sm.shared) { scan(sm.geometry); } }
	return out[0][1] < 1e29 && out[1][1] < 1e29;
}

// The world box of the posed character's vertices that the heaviest bone of each picks: one body part's (byPart: the part map's own rule, baked
// per vertex) or one bone's own (byPart false). The animal portrait's frame: its head's own size, so no per-race offset table is needed.
bool Characters::boundsOf(const Char& c, bool byPart, int index, double lo[3], double hi[3])
{
	if (!c.body || c.palette.empty() || index < 0) { return false; }
	for (int k = 0; k < 3; k++) { lo[k] = 1e30; hi[k] = -1e30; }
	auto scan = [&](const ogre::Geometry& g)
	{
		if (g.boneIndex.empty()) { return; }
		for (uint32_t v = 0; v < g.count; v++)
		{
			int heaviest = -1;
			float top = 0;
			double p[3] = {};
			for (int k = 0; k < 4; k++)
			{
				const uint8_t b = g.boneIndex[v * 4 + k];
				const float w = g.boneWeight[v * 4 + k];
				if (w <= 0) { continue; }
				if (w > top) { top = w; heaviest = (int)b; }
				const float* m = &c.palette[(size_t)b * 12];
				for (int r = 0; r < 3; r++) { p[r] += w * (m[r * 4] * g.pos[v * 3] + m[r * 4 + 1] * g.pos[v * 3 + 1] + m[r * 4 + 2] * g.pos[v * 3 + 2] + m[r * 4 + 3]); }
			}
			if (heaviest < 0) { continue; }
			if (byPart) { if ((size_t)heaviest >= c.body->bonePart.size() || (int)c.body->bonePart[(size_t)heaviest] != index) { continue; } }
			else if (heaviest != index) { continue; }
			for (int r = 0; r < 3; r++) { lo[r] = std::min(lo[r], c.pos[r] + p[r]); hi[r] = std::max(hi[r], c.pos[r] + p[r]); }
		}
	};
	if (c.body->src.hasShared) { scan(c.body->src.shared); }
	for (const ogre::Submesh& sm : c.body->src.submeshes) { if (!sm.shared) { scan(sm.geometry); } }
	return lo[1] < 1e29;
}

bool Characters::partBox(const Char& c, int part, double lo[3], double hi[3]) { return boundsOf(c, true, part, lo, hi); }

bool Characters::boneBox(const Char& c, int bone, double lo[3], double hi[3]) { return boundsOf(c, false, bone, lo, hi); }

// The skeleton's head bone. Every race the client loads (humans and the animals, `Bip01`-named rigs) has `Bip01 Head`; a rig named otherwise
// still portraits on its head: the first bone named like one.
int Characters::headBone(const kworld::Skeleton& sk)
{
	if (const int b = sk.bone("Bip01 Head"); b >= 0) { return b; }
	for (size_t b = 0; b < sk.bones.size(); b++)
	{
		const std::string n = folded(sk.bones[b].name);
		if (n.find("head") != std::string::npos && n.find("nub") == std::string::npos) { return (int)b; }
	}
	return -1;
}

float Characters::restLift(const Char& c)
{
	// A throwaway copy posed on the stance clip at time 0, weight 1 (the portrait's way): the clip's own sole height over the node, the same for the
	// body standing, walking or running. The original has no live fit; this is its clip + root shift made measurable.
	const auto st = anims_.find(folded("idle_stand_normal"));
	if (st == anims_.end() || !c.body) { return 0; }
	Char t = c;
	t.layers.clear();
	t.postures.clear();
	t.rag.clear();
	t.riseDur = 0;
	t.spec.loco = false;	// no foot lock on the copy
	t.idle = t.action = -1;
	std::string err;
	if (!addLayer(t, st->second.clip, 0, 0.0f, err)) { return 0; }
	t.pos[0] = t.pos[1] = t.pos[2] = 0;
	t.footLift = 0;
	t.orient = { 1, 0, 0, 0 };
	pose(t, 0);
	double s[2][3];
	if (!solesOf(t, s)) { return 0; }
	return (float)std::clamp(std::min(s[0][1], s[1][1]), -2.0, 2.0);
}

Characters::IdleStats Characters::idleStats() const
{
	IdleStats r;
	for (const Char& c : chars_)
	{
		if (c.feetLock[0].idlePi >= 0 || c.feetLock[1].idlePi >= 0) { r.engaged++; }
		r.slopeW = std::max({ r.slopeW, (double)c.feetLock[0].slopeW, (double)c.feetLock[1].slopeW });
		if (ground) { r.feetDelta = std::max(r.feetDelta, std::fabs(ground(c.pos[0] + c.feet[0][0], c.pos[2] + c.feet[0][2]) - ground(c.pos[0] + c.feet[1][0], c.pos[2] + c.feet[1][2]))); }
	}
	return r;
}

std::string Characters::restLine(size_t i) const
{
	if (i >= chars_.size() || !chars_[i].body) { return {}; }
	const Char& c = chars_[i];
	float knee[2] = { -1, -1 };
	for (int f = 0; f < 2; f++)
	{
		const int th = c.body->legBone[f][0], ca = c.body->legBone[f][1], fo = c.body->legBone[f][2];
		if (th < 0 || ca < 0 || fo < 0 || c.model.size() <= (size_t)std::max({ th, ca, fo })) { continue; }
		const kworld::SkelVec3 d1 = vsub(c.model[(size_t)ca].pos, c.model[(size_t)th].pos), d2 = vsub(c.model[(size_t)fo].pos, c.model[(size_t)ca].pos);
		const float l = vlen(d1) * vlen(d2);
		knee[f] = l > 1e-6f ? std::acos(std::clamp(vdot(d1, d2) / l, -1.0f, 1.0f)) * 57.29578f : 0.0f;
	}
	const int pel = c.body->skel.bone("Bip01 Pelvis");
	const double pelvis = pel >= 0 && (size_t)pel < c.model.size() ? c.pos[1] + c.model[(size_t)pel].pos.y - c.spec.pos[1] : -1e9;
	char b[200];
	snprintf(b, sizeof b, "rest %s knee L %.3f R %.3f pelvis %.4f", c.spec.stance.c_str(), knee[0], knee[1], pelvis);
	return b;
}

std::string Characters::fitReport(size_t i) const
{
	const Char& k = chars_[i];
	char b[256];
	snprintf(b, sizeof b, "root shift %+.3f sole lift %+.3f fit %d foot y %+.2f %+.2f foot lift %+.2f pos y %.2f spec y %.2f", k.rootShift,
		k.soleLift, (int)k.soleFit, k.feet[0][1], k.feet[1][1], k.footLift, k.pos[1], k.spec.pos[1]);
	return b;
}

// dump contacts: each moving lower clip's derived contact windows (phase start-end, per foot and sole point) and stride.
std::string Characters::contactReport(size_t i) const
{
	if (i >= chars_.size() || !chars_[i].body) { return "  no such character\n"; }
	std::string out;
	for (const Layer& l : chars_[i].layers)
	{
		if (!l.info || l.overlay || l.info->code != 0 || l.info->idle) { continue; }
		char b[512];
		snprintf(b, sizeof b, "  %-30s clip %-30s %.2fs play %+.3f band %+.0f..%+.0f move %+.0f synchs %d off %+.3f", l.info->name.c_str(), l.info->clip.c_str(),
			l.clip ? l.clip->length : 0.0f, l.info->play, l.info->min, l.info->max, l.info->move, (int)l.synchs, l.synchOffset);
		out += b;
		for (int f = 0; f < 2; f++)
		{
			out += f ? " | R" : " | L";
			if (l.contacts.window[f].empty()) { out += " none"; }
			for (const SoleContact& w : l.contacts.window[f])
			{
				snprintf(b, sizeof b, " %s %.3f-%.3f", w.point ? "ball" : "heel", w.start, w.end);
				out += b;
			}
		}
		if (l.contacts.windows > 0 && std::fabs(l.contacts.stride - l.contacts.windows) > 0.05f * l.contacts.windows)
		{
			// the figure the warp does not use (the old window sum), for the check
			snprintf(b, sizeof b, " | stride %6.2f u/cycle (windows %.2f)\n", l.contacts.stride, l.contacts.windows);
		}
		else { snprintf(b, sizeof b, " | stride %6.2f u/cycle\n", l.contacts.stride); }
		out += b;
	}
	return out;
}

std::string Characters::clipReport(size_t i) const
{
	std::string out;
	char b[160];
	const int root = chars_[i].body ? chars_[i].body->skel.bone("Bip01") : -1;
	auto add = [&](const Layer& l, const char* tag)
	{
		if (!l.clip) { return; }
		const bool r = root >= 0 && (size_t)root < l.keep.size() && l.keep[(size_t)root] != 0;
		const float ry = r ? kworld::sampleLocal(chars_[i].body->skel, l.clip, root, l.time).pos.y : 0;
		snprintf(b, sizeof b, "%s%s t %.3f/%.3f w %.2f%s%s", out.empty() ? "" : " ; ", l.clip->name.c_str(), l.time, l.clip->length, l.weight, r ? " root" : "", tag);
		if (r) { snprintf(b + strlen(b), sizeof b - strlen(b), " rooty %.3f", (double)ry); }
		out += b;
	};
	for (const Layer& l : chars_[i].layers) { add(l, ""); }
	for (const Layer& l : chars_[i].postures) { add(l, " posture"); }
	// the controller's inputs (kenshi-h2v.11.5.1): what weighed the layers above
	const Spec& sp = chars_[i].spec;
	char lb[200];
	snprintf(lb, sizeof lb, " ; loco move %.3f strafe %.3f legs %.0f/%.0f combat %d stealth %d weapon %d stance '%s'", (double)sp.move, (double)sp.strafe, (double)sp.legs[0], (double)sp.legs[1],
		(int)sp.combat, (int)sp.stealth, chars_[i].weaponCategory, sp.stance.c_str());
	out += lb;
	// the weapon's state: the wire's drawn bit the record choice and the attach follow (kenshi-h2v.11.5.2)
	out += std::string(" ; drawn ") + (sp.drawn ? "1" : "0") + " weapons";
	for (const std::string& w : sp.weapons) { out += " '" + w + "'"; }
	for (const std::string& m : chars_[i].mountNames) { out += " ; mount " + m; }
	return out;
}

Characters::LocoState Characters::locoState(size_t i) const
{
	const Char& c = chars_[i];
	LocoState ls{ c.warped, c.steered, c.stopActive, c.gettingUp, c.lieLayer, c.action, c.actionDone };
	for (int f = 0; f < 2; f++) { ls.lock[f] = c.feetLock[f].active ? 1 : c.feetLock[f].blocked ? 2 : 0; }
	ls.drop = c.footLift;
	ls.footLock = footLock_;
	ls.footTerrain = footTerrain_;
	ls.simY = (float)c.spec.pos[1];
	ls.drawnY = (float)c.pos[1];
	ls.soleLift = c.soleLift;
	ls.rootShift = c.rootShift;
	if (ground) { ls.terrainY = ground(c.spec.pos[0], c.spec.pos[2]); ls.onTerrain = std::fabs(ls.terrainY - c.spec.pos[1]) < 2.0; }
	return ls;
}

std::string Characters::idleLines() const
{
	std::string out;
	char b[160];
	for (size_t i = 0; i < chars_.size(); i++)
	{
		const Char& c = chars_[i];
		const std::string held = c.idle >= 0 && (size_t)c.idle < c.layers.size() && c.layers[(size_t)c.idle].info ? c.layers[(size_t)c.idle].info->name : "-";
		snprintf(b, sizeof b, "idle %zu weapon category %d combat %d stealth %d: %s\n", i, c.weaponCategory, (int)c.spec.combat, (int)c.spec.stealth, held.c_str());
		out += b;
	}
	return out;
}

bool Characters::actionRoot(size_t i, ActionRoot& out) const
{
	if (i >= chars_.size()) { return false; }
	const Char& c = chars_[i];
	if (!c.actPath || c.action < 0 || (size_t)c.action >= c.layers.size()) { return false; }
	const Layer& l = c.layers[(size_t)c.action];
	if (!l.clip) { return false; }
	out.path[0] = c.actPathPt[0];
	out.path[1] = c.actPathPt[1];
	out.wire[0] = c.spec.pos[0];
	out.wire[1] = c.spec.pos[2];
	out.drawn[0] = c.pos[0];
	out.drawn[1] = c.pos[2];
	out.corr[0] = c.actCorr[0];
	out.corr[1] = c.actCorr[1];
	out.root0[0] = c.actRoot0[0];
	out.root0[1] = c.actRoot0[1];
	out.clip = l.clip->name;
	out.clipTime = l.time;
	out.length = l.clip->length;
	return true;
}

std::string Characters::animSummary(size_t i) const
{
	std::string out;
	char w[16];
	for (const Layer& l : chars_[i].layers)
	{
		if (l.weight <= 0.05f || !l.clip) { continue; }
		snprintf(w, sizeof w, " %.2f ", l.weight);
		out += l.clip->name + w;
	}
	// the weapon's drawn state and where its meshes hang (kenshi-h2v.11.5.2): the wire's kFlagDrawn, the records, the attach points
	const Spec& sp = chars_[i].spec;
	out += std::string("; drawn ") + (sp.drawn ? "1" : "0");
	for (const std::string& n : sp.weapons) { out += " '" + n + "'"; }
	for (const std::string& m : chars_[i].mountNames) { out += " ; mount " + m; }
	return out;
}
