#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "characters.hpp"
#include "cpustage.hpp"
#include "dev/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "drawstats.hpp"
#include "sidorder.hpp"
#include "eventlog.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include "ao.hpp"
#include "lightvol.hpp"
#include "config.hpp"
#include "dds.hpp"
#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "ogremesh.hpp"
#include "speccube.hpp"
#include "stb_image.h"
#include "weathermap.hpp"

#include "character.glsl.h"

namespace
{

constexpr int kMaxBones = 60;	// skin.hlsl BONES
constexpr int kVertexFloats = 18;	// pos 3, normal 3, uv 2, tangent 4, bone indices (4 bytes as one float slot), weights 4, part map bits
constexpr int kVertexBytes = kVertexFloats * 4;

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
const cfg::Float kIdleDraw{ cfg::Group{ "Locomotion" }, "anim.idle_draw", 0.0, 0.0, 1.0, "The standing idle pick (kenshi-k16.2.162.1, rwh101): 1 = the original's weighted random draw, held for the record's `idle time` (stealth idle 10-40 s); 0 = the first maximum in its idle list's order (deterministic, what its combat picker does). Every ANIMATION record of this data has idle time 10..40, so the draw makes a sneaking armed character flip between `stealth idle` and `stand 1 sword`: 0 keeps the crouch the original shows" };
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
// render.char_instancing (kenshi-k16.2.205.8): the parts of every character grouped by (mesh, material) and drawn instanced (one draw per group, per pass) from one
// palette buffer per frame; 0 = one draw per part in character order from the same buffers (the old draw count, for the A/B)
const cfg::Bool kCharInstancing{ cfg::Group{ "Performance" }, "render.char_instancing", true, "Characters: draw the parts grouped by mesh and material, instanced, from one palette buffer per frame (off: one draw per part per character, the old draw count; for comparisons)" };
// anim.action_root (fix 2, the same report): a technique's displacement is the clip's own root replayed tick by tick (COMBAT_ANIM.md 5, the sim:
// clip.at(t1) - clip.at(t0) in x/z, rotated by the aim yaw fixed at the technique's start, no root yaw, at its playback rate). The client zeroed
// an action's root x/z and drew the body at the wire, so a clip's stance foot slid at body speed. Draw the body along the anchor + the clip's root
// and hold the action clip's own contact windows with the foot lock; where the wire differs (navmesh clamp, separation push, stumble recoil) the
// drawn path eases onto it. 0 = the original's wire position and no action lock.
const cfg::Bool kActionRoot{ cfg::Group{ "Locomotion" }, "anim.action_root", true, "Animation: an action draws along its clip root replayed from the action's start (the technique's own displacement) and the foot lock holds the action's contact windows (0 = the wire position, no action lock)" };
// anim.inertialize (CLIENT_GOALS "Locomotion fidelity" 5): a clip transition's pose offset and velocity decay onto the target pose over the
// crossfade's own time (Bollo 2016) instead of the linear weight ramp; 0 = the original's linear crossfade exactly
// anim.pose_threads (kenshi-k16.2.205.12.102, overlap review .85 B2 / T22): the threads Characters::update's pose pass runs on, the calling (logic)
// thread included; 1 = the serial loop. The output is the same at any count (each pose writes its own character only).
const cfg::Int kPoseThreads{ cfg::Group{ "Performance" }, "anim.pose_threads", 4, 1, 16, "Characters: threads the per-frame pose sampling runs on, the logic thread included (1 = serial; the poses are the same at any count)" };
const cfg::Bool kInertialize{ cfg::Group{ "Locomotion" }, "anim.inertialize", true, "Locomotion: a clip change keeps the old pose's motion, decaying onto the new clip, instead of a linear crossfade (0 = the original's linear crossfade)" };

// render.loading_standin (kenshi-k16.2.258.1, user 2026-11-15): what a character whose GPU objects are not ready yet draws as, at its wire position
// and animation — its race's prewarmed body in the stand-in's material, then the two-stage switch into its real look (Characters::kStandinShape /
// kStandinColour). silhouette: flat black, unlit, no textures (it reads as loading, never as an identity; the user's pick for the default);
// double: the race body in a plain neutral material; off: nothing drawn (the behaviour before this).
const cfg::Text kLoadingStandin{ cfg::Group{ "Character loading" }, "render.loading_standin", "silhouette", { "silhouette", "double", "off" },
	"Characters still uploading draw as their race's body: silhouette = flat black, unlit (the default); double = a plain neutral body; off = not drawn" };
#ifdef KCLIENT_DEV_TOOLS
// dev.standin_hold (KCLIENT_DEV_TOOLS, kenshi-k16.2.258.1): hold every non-squad character in its stand-in for this many seconds after its build, so
// the A/B and the switch can be seen live and in a script (a script sets it to 0 on a frame to release the whole crowd into the fade at once).
const cfg::Float kStandinHold{ cfg::Group{ "Character loading", "", "Debug" }, "dev.standin_hold", 0.0, 0.0, 100000.0, "Dev: keep every non-squad character in its loading stand-in this many s after it is built (0 = off; a script sets it back to 0 to release the fade)" };
#endif

// The contact table's resolution (MINE): phase samples per clip cycle, and how close to the cycle's lowest sole a point is on the ground.
constexpr int kContactSamples = 96;
constexpr float kContactHeight = 0.4f;
// anim.action_root (fix 2): the seconds the drawn action path takes to ease onto the wire where the two differ (the sim's navmesh clamp, separation
// pushes, stumble recoil): short enough that a clamped dash stays at the wire, long enough that a push is not a one-frame jump (MINE), and the step
// the drawn body may lead the wire by in the meantime (a walk's step; past it the pose would be drawn through the crowd the sim stopped it in)
constexpr float kActEase = 0.15f;
constexpr double kActMaxOff = 1.0;
// foot lock (foot IK slice 1): a per-frame knee step past this reads as a pop (the swing-foot sanity, log feet; MINE)
constexpr float kKneePopDeg = 15.0f;
// the applied knee may move only so fast: past this the foot slides a little instead (720 deg/s = 12 deg/frame at 60 fps, the pop threshold
// with a margin for frames that run a little long)
constexpr float kKneeRate = 720.0f;

static_assert(Characters::kMaxCascades == Shadow::kCascades, "cascade count");
static_assert(sizeof(ShadowParams) == sizeof(shadow_params_t), "shadow_params layout");

std::string folded(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

// column-major 4x4: out = a * b
void mul(float out[16], const float a[16], const float b[16])
{
	float r[16];
	for (int c = 0; c < 4; c++)
	{
		for (int i = 0; i < 4; i++) { r[c * 4 + i] = a[i] * b[c * 4] + a[4 + i] * b[c * 4 + 1] + a[8 + i] * b[c * 4 + 2] + a[12 + i] * b[c * 4 + 3]; }
	}
	memcpy(out, r, sizeof r);
}

// ANIMATION `delete ...` flags and the bones whose tracks each one destroys (traced: FUN_140869860, by bone name).
struct DeleteGroup { const char* field; std::vector<const char*> bones; };
const DeleteGroup kDeleteGroups[] = {
	{ "delete root", { "Bip01" } },
	{ "delete L arm", { "Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand" } },
	{ "delete R arm", { "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand" } },
	{ "delete below waist", { "Bip01", "Bip01 Pelvis", "Bip01 R Thigh", "Bip01 L Thigh", "Bip01 R Calf", "Bip01 L Calf", "Bip01 R Foot", "Bip01 L Foot", "Bip01 R Toe0", "Bip01 L Toe0" } },
	{ "delete spine0", { "Bip01 Spine" } },
	{ "delete body head", { "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck", "Bip01 Head" } },
	{ "delete weapons", { "Bip01 Prop2", "Bip01 Prop1" } },
	{ "delete tail", { "Bip01 Tail", "Bip01 Tail1", "Bip01 Tail2", "Bip01 Tail3", "Bip01 Tail4" } },
};

// The editor's "Skin Tone" colours for one gender (data/editor/editor_data_*.xml: <Config gender="Male"> ... <ColourRange name="Skin Tone"> <Colour r g b/>).
int readSkinRange(const std::string& path, bool female, float out[8][3])
{
	std::ifstream in(path);
	if (!in) { return 0; }
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string xml = ss.str();
	size_t at = xml.find(female ? "gender=\"Female\"" : "gender=\"Male\"");
	if (at == std::string::npos) { return 0; }
	at = xml.find("name=\"Skin Tone\"", at);
	const size_t end = at == std::string::npos ? at : xml.find("</ColourRange>", at);
	int n = 0;
	while (at != std::string::npos && n < 8)
	{
		at = xml.find("<Colour ", at + 1);
		if (at == std::string::npos || at > end) { break; }
		const char* keys[3] = { "r=\"", "g=\"", "b=\"" };
		for (int k = 0; k < 3; k++)
		{
			const size_t v = xml.find(keys[k], at);
			out[n][k] = v == std::string::npos ? 1.0f : (float)atof(xml.c_str() + v + 3) / 255.0f;
		}
		n++;
	}
	return n;
}

// The editor's sliders for one gender: every <Range> of the <Config>. mid defaults to round(min + (max - min) / 2) (140077090).
std::vector<Characters::EditorRange> readRanges(const std::string& path, bool female)
{
	std::vector<Characters::EditorRange> out;
	std::ifstream in(path);
	if (!in) { return out; }
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string xml = ss.str();
	size_t at = xml.find(female ? "gender=\"Female\"" : "gender=\"Male\"");
	const size_t end = at == std::string::npos ? at : xml.find("</Config>", at);
	while (at != std::string::npos)
	{
		at = xml.find("<Range ", at + 1);
		if (at == std::string::npos || at > end) { break; }
		const std::string tag = xml.substr(at, xml.find('>', at) - at);
		auto attr = [&](const char* key) -> std::string
		{
			const std::string k = std::string(" ") + key + "=\"";
			const size_t v = tag.find(k);
			return v == std::string::npos ? std::string() : tag.substr(v + k.size(), tag.find('"', v + k.size()) - v - k.size());
		};
		Characters::EditorRange r = { attr("name"), attr("target"), attr("target_opposite"), (float)atof(attr("min").c_str()), (float)atof(attr("max").c_str()), 0 };
		if (const size_t cat = xml.rfind("<Category name=\"", at); cat != std::string::npos && cat > xml.rfind("<Config ", at))
		{
			r.category = xml.substr(cat + 16, xml.find('"', cat + 16) - cat - 16);
		}
		r.variation = (float)atof(attr("random_variation").c_str());
		if (const std::string group = attr("random_group"); !group.empty()) { r.group = atoi(group.c_str()); }
		const std::string mid = attr("mid");
		r.mid = mid.empty() ? std::round(r.min + (r.max - r.min) * 0.5f) : (float)atof(mid.c_str());
		out.push_back(r);
	}
	return out;
}

}

namespace
{
	// The Inventory_Items switch values (the variant an inventory move sound plays; kenshi-inspect findings/rwh65/switches.tsv, the table at
	// 141f56840): ITEM / CONTAINER / MAP_ITEM records pick with their `inventory sound` int, ARMOUR with its `material type`. The tsv's value
	// list, grouped as it is there: the item / container segment first, then the armour's. Past a segment: empty (never a guess).
	const char* inventory_sound_item(int v)
	{
		static const char* const kNames[] = { "Backpack", "Building_Material", "Fabric", "Flour", "Food", "Kits", "Leather", "Luxury", "Narcotics",
			"Ore", "Potato", "Robotic_Component", "Rum", "Steel_Bar", "Tools", "Water", "Wheat", "Armor_Plating", "Blueprints", "Meat" };
		return v >= 0 && v < (int)std::size(kNames) ? kNames[v] : "";
	}
	const char* inventory_sound_armour(int v)	// ARMOUR `material type` (MaterialType 0 cloth .. 3 metal plate)
	{
		static const char* const kNames[] = { "Fabric", "Leather", "Robotic_Component", "Steel_Bar" };
		return v >= 0 && v < (int)std::size(kNames) ? kNames[v] : "";
	}
}

void Characters::init(const ResourceIndex* res, const kdata::GameData& gd, bool gpuNow)
{
	res_ = res;
	for (const kdata::Record& r : gd.records()) { sidNames_[r.stringID] = r.name; }
	for (const char* type : { "WEAPON", "ARMOUR" }) { for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type))) { equippable_.insert(r->stringID); } }
	// Item prices and food (kinspect items14_static.md): GLOBAL CONSTANTS from the merged data
	float foodQuality = 1, foodPrice = 1, tradePrice = 1, globalPrice = 1;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CONSTANTS")))
	{
		foodQuality = r->floatOr("food quality mult", foodQuality);
		foodPrice = r->floatOr("food price mult", foodPrice);
		tradePrice = r->floatOr("trade price mult", tradePrice);
		globalPrice = r->floatOr("global price mult", globalPrice);
	}
	// FUN_140896e20 with the mults on: an item with `ingredients` = the sum of each ingredient's price x amount (v0 percent) x (1 + its `profitability`),
	// each term rounded; a leaf = `value` x (food crop ? food : trade) price mult x global, rounded (not for artifacts). The exe has no cycle guard; MINE: depth 16.
	std::function<int(const kdata::Record&, int)> price = [&](const kdata::Record& r, int depth) -> int
	{
		const kdata::RefCategory* in = r.findRefs("ingredients");
		if (!in || in->entries.empty())
		{
			const float v = (float)r.intOr("value", 0);
			return r.boolOr("artifact", false) ? (int)v : (int)std::lround(v * (r.boolOr("food crop", false) ? foodPrice : tradePrice) * globalPrice);
		}
		const float profit = r.floatOr("profitability", 0.2f);
		int total = 0;
		for (const kdata::RefEntry& e : in->entries)
		{
			const kdata::Record* ing = depth < 16 ? gd.find(e.targetID) : nullptr;
			if (ing) { total += (int)std::floor(price(*ing, depth + 1) * (e.v0 * 0.01f) * (1 + profit) + 0.5f); }
		}
		return total;
	};
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("WEAPON_MANUFACTURER")))
	{
		ModInfo& m = mods_[r->stringID];
		m.cut = r->floatOr("cut damage mod", 1);
		m.blunt = r->floatOr("blunt damage mod", 1);
		m.description = r->stringOr("company description");
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("MATERIAL_SPECS_WEAPON")))
	{
		ModInfo& m = mods_[r->stringID];
		m.attack = r->intOr("attack mod", 0);
		m.defence = r->intOr("defence mod", 0);
	}
	for (const char* type : { "ITEM", "WEAPON", "ARMOUR", "CONTAINER", "CROSSBOW", "MAP_ITEM", "LIMB_REPLACEMENT" })
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		{
			ItemInfo& it = items_[r->stringID];
			it.w = std::max(1, r->intOr("inventory footprint width", 1));
			it.h = std::max(1, r->intOr("inventory footprint height", 1));
			const bool weapon = !strcmp(type, "WEAPON") || !strcmp(type, "CROSSBOW");
			const int slot = !strcmp(type, "ARMOUR") || !strcmp(type, "CONTAINER") ? r->intOr("slot", 7) : 7;
			it.kind = weapon ? 1 : slot == 7 ? 0 : slot == 1 ? 15 : slot;
			it.icon = r->fileOr("icon");
			it.plain = !strcmp(type, "ITEM");
			it.function = r->intOr("item function", 0);
			it.value = r->intOr("value", 0);
			it.weight = r->floatOr("weight kg", 0);
			it.description = r->stringOr("description");
			if (const kdata::RefCategory* in = r->findRefs("ingredients"))
			{
				for (const kdata::RefEntry& e : in->entries)
				{
					const auto n = sidNames_.find(e.targetID);
					if (n != sidNames_.end()) { it.ingredients.push_back(n->second); }
				}
			}
			it.charges = std::max(r->floatOr("charges", 1), 1.0f);
			if (it.function == 3 || it.function == 0xf)
			{
				if (it.charges > 1) { it.charges *= foodQuality; }
				else { it.function = 0; }	// the Item ctor FUN_14075faf0 (kinspect items14_static.md a)
			}
			if (!strcmp(type, "ITEM")) { it.price = price(*r, 0); }
			if (weapon)
			{
				it.cut = r->floatOr("cut damage multiplier", 0);
				it.blunt = r->floatOr("blunt damage multiplier", 0);
				it.bleed = r->floatOr("bleed mult", 1);
				it.penetration = r->floatOr("armour penetration", 0);
				it.vsRobot = r->floatOr("robot damage mult", 1);
				it.vsHuman = r->floatOr("human damage mult", 1);
				it.vsAnimal = r->floatOr("animal damage mult", 1);
				it.attack = r->intOr("attack mod", 0);
				it.defence = r->intOr("defence mod", 0);
				it.indoors = r->intOr("indoors mod", 0);
				it.skillCategory = r->intOr("skill category", -1);
				it.shootSound = r->intOr("shoot sound", -1);
			}
			if (!strcmp(type, "ARMOUR"))
			{
				it.armourClass = std::clamp(r->intOr("class", 0), 0, 3);
				it.material = std::clamp(r->intOr("material type", 0), 0, 3);
				it.unarmedBonus = r->intOr("unarmed bonus", 0);
				it.perceptionBonus = r->intOr("perception bonus", 0);
				it.rangedMult = r->floatOr("ranged skill mult", 1);
				it.dodgeMult = r->floatOr("dodge mult", 1);
				it.damageMult = r->floatOr("damage output mult", 1);
				it.assassinMult = r->floatOr("asassination mult", 1);
				it.fistMult = r->floatOr("fist injury mult", 1);
				if (const kdata::RefCategory* parts = r->findRefs("part coverage"))
				{
					for (const kdata::RefEntry& e : parts->entries)
					{
						const auto n = sidNames_.find(e.targetID);
						if (n != sidNames_.end()) { it.coverage.push_back({ n->second, e.v0 }); }
					}
				}
			}
			if (!strcmp(type, "CONTAINER"))
			{
				it.encumbrance = r->floatOr("encumbrance effect", 1);
				it.athletics = r->floatOr("athletics mult", 1);
				it.combatSpeed = r->floatOr("combat speed mult", 1);
				it.stealth = r->floatOr("stealth mult", 1);
				it.combatSkill = r->intOr("combat skill bonus", 0);
			}
			// the inventory move sounds' switch (audio_sfx_triggers.md section 5; the tables above): WEAPON / CROSSBOW stay empty, their `material`
			// ref's value mapping is not in the findings, and LIMB_REPLACEMENT has no `inventory sound` field
			if (!strcmp(type, "ARMOUR")) { it.soundSwitch = inventory_sound_armour(it.material); }
			else if (!strcmp(type, "ITEM")) { it.soundSwitch = it.function == 0x6f ? "Robotic_Component" : inventory_sound_item(r->intOr("inventory sound", 0)); }
			else if (!strcmp(type, "CONTAINER") || !strcmp(type, "MAP_ITEM")) { it.soundSwitch = inventory_sound_item(r->intOr("inventory sound", 0)); }
		}
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RACE")))
	{
		RaceInfo ri;
		ri.name = r->name;
		ri.carryBone = r->stringOr("carry bone");	// animals: the ragdoll body pinned to the carrier's shoulder (rwh.118); empty = the skeleton's root
		for (int g = 0; g < 2; g++)
		{
			const std::string gn = g ? "female" : "male";
			ri.mesh[g] = r->fileOr(gn + " mesh");
			ri.tex[g] = r->fileOr("body texture " + gn);
			ri.nrm[g] = r->fileOr("nm " + gn);
			ri.nrmSkinny[g] = r->fileOr("nm " + gn + " skinny");
			ri.nrmStrong[g] = r->fileOr("nm " + gn + " strong");
			ri.mask[g] = r->fileOr("body mask " + gn);
			ri.partMap[g] = r->fileOr("part map " + gn);
			ri.ragdoll[g] = r->fileOr(gn + " ragdoll");	// the .phs the original turns this race into on KO / death (no fall or death clip)
			if (const kdata::RefCategory* heads = r->findRefs("heads " + gn))
			{
				for (const kdata::RefEntry& e : heads->entries)
				{
					if (const kdata::Record* h = gd.find(e.targetID)) { ri.heads[g].push_back({ h->fileOr("texture map"), h->fileOr("normal map"), h->fileOr("mask map") }); }
				}
			}
		}
		ri.editor = r->fileOr("editor limits");
		ri.playable = r->boolOr("playable", false);
		ri.description = r->stringOr("description");
		for (const auto& [field, out] : { std::pair{ "hairs", &ri.hairs }, std::pair{ "hair colors", &ri.hairColours } })
		{
			if (const kdata::RefCategory* refs = r->findRefs(field))
			{
				for (const kdata::RefEntry& e : refs->entries)
				{
					if (const kdata::Record* t = gd.find(e.targetID)) { out->push_back(t->name); (out == &ri.hairs ? ri.hairWeights : ri.hairColourWeights).push_back((float)e.v0); }
				}
			}
		}
		for (int k = 0; k < 16; k++)
		{
			if (const int g = r->intOr("stats good" + std::to_string(k), 0)) { ri.statsGood.push_back(g); }
			if (const int b = r->intOr("stats bad" + std::to_string(k), 0)) { ri.statsBad.push_back(b); }
		}
		ri.strength = r->floatOr("strength", 1.0f);
		ri.dexterity = r->floatOr("dexterity", 1.0f);
		ri.bleedRate = r->floatOr("bleed rate", 1.0f);
		ri.sounds = r->intOr("sounds", 0);
		ri.accel = r->floatOr("pathfind acceleration", 15.0f) * 10.0f;	// ARCHITECTURE "Acceleration": the sim's u/s^2 (Human 20 -> 200)
		ri.attach = r->fileOr("attachment points");
		// AppearanceHuman vf13 (140533b50): bloodColour = `blood colour` as ARGB with alpha 1, bloodScale = (`blood horizontal`, `blood vertical`)
		const uint32_t bc = (uint32_t)r->intOr("blood colour", 0x800000);
		for (int k = 0; k < 3; k++) { ri.bloodCol[k] = (float)((bc >> (16 - 8 * k)) & 0xff) / 255.0f; }
		ri.bloodScale[0] = (float)r->intOr("blood horizontal", 1);
		ri.bloodScale[1] = r->floatOr("blood vertical", 0.15f);
		ri.portrait[0] = r->floatOr("portrait distance", 10);
		ri.portrait[1] = r->floatOr("portrait offset x", 0);
		ri.portrait[2] = r->floatOr("portrait offset y", 0.5f);
		ri.portrait[3] = r->floatOr("portrait offset z", 0);
		ri.hull[0] = r->floatOr("hull size X", 5);
		ri.hull[1] = r->floatOr("hull size Y", 20);
		ri.hull[2] = r->floatOr("hull size Z", 5);
		if (const kdata::RefCategory* limbs = r->findRefs("limb replacement"))
		{
			for (const kdata::RefEntry& e : limbs->entries)
			{
				const kdata::Record* l = gd.find(e.targetID);
				const int slot = l ? l->intOr("slot", 0) - 50 : -1;
				if (slot >= 0 && slot < 4) { ri.stump[slot][0] = l->fileOr("mesh"); ri.stump[slot][1] = l->fileOr("mesh female"); }
			}
		}
		// body part order (140075600): the `combat anatomy` LOCATIONAL_DAMAGE records sorted by a per `body part type` key plus `collapse part` & 0x12
		// (key table 141683564: 20 60 40 0); the sort (MSVC std::sort, insertion sort at this size) keeps ref order on ties
		if (const kdata::RefCategory* anatomy = r->findRefs("combat anatomy"))
		{
			std::vector<std::pair<int, std::pair<std::string, std::vector<std::string>>>> parts;
			const int kKey[4] = { 20, 60, 40, 0 };
			for (const kdata::RefEntry& e : anatomy->entries)
			{
				const kdata::Record* p = gd.find(e.targetID);
				if (!p) { continue; }
				const int type = p->intOr("body part type", 0);
				ri.partMax[p->name] = (float)e.v1;
				std::vector<std::string> bones;
				for (const char* key : { "bone name", "bone name 2", "bone name 3" }) { if (std::string b = p->stringOr(key); !b.empty()) { bones.push_back(std::move(b)); } }
				parts.push_back({ (type >= 0 && type < 4 ? kKey[type] : 0) + (0x12 & p->intOr("collapse part", 0)), { p->name, std::move(bones) } });
			}
			std::stable_sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			for (auto& p : parts) { ri.anatomy.push_back(std::move(p.second)); }
		}
		races_.push_back(std::move(ri));
	}
	// a female field left empty falls back to the male one
	auto gendered = [](const kdata::Record* r, const std::string& key, std::string out[2]) { out[0] = r->fileOr(key); out[1] = r->fileOr(key + " female"); if (out[1].empty()) { out[1] = out[0]; } };
	// CONTAINER = the backpacks (slot 12): the same fields, the mesh rigged to the character skeleton like armour, so they are worn the same way
	std::vector<const kdata::Record*> worn = gd.byType(kworld::itemTypeByName("ARMOUR"));
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CONTAINER"))) { worn.push_back(r); }
	for (const kdata::Record* r : worn)
	{
		ArmourInfo a;
		gendered(r, "mesh", a.mesh);
		gendered(r, "vest texture", a.vestTex);
		gendered(r, "vest normalmap", a.vestNrm);
		gendered(r, "vest colormap", a.vestCmap);
		a.colorise = !r->boolOr("dont colorise", false);
		a.hideHair = r->boolOr("hide hair", false);
		a.slot = r->intOr("slot", -1);
		a.armourClass = std::clamp(r->intOr("class", 0), 0, 3);
		a.hideParts = r->intOr("hide parts", 0);
		const kdata::RefCategory* mat = r->findRefs("material");
		if (const kdata::Record* m = mat && !mat->entries.empty() ? gd.find(mat->entries[0].targetID) : nullptr)
		{
			a.matTex = m->fileOr("texture map");
			a.matNrm = m->fileOr("normal map");
			a.glossMult = m->floatOr("specular mult", 1.0f);
			a.matCmap = m->fileOr("color map");
		}
		const kdata::RefCategory* col = r->findRefs("color");
		if (const kdata::Record* cd = col && !col->entries.empty() ? gd.find(col->entries[0].targetID) : nullptr) { a.colour = folded(cd->name); }
		armour_[folded(r->name)] = std::move(a);
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RACE_GROUP")))
	{
		std::vector<std::string> names;
		if (const kdata::RefCategory* refs = r->findRefs("races"))
		{
			for (const kdata::RefEntry& e : refs->entries) { if (const kdata::Record* t = gd.find(e.targetID)) { names.push_back(t->name); } }
		}
		raceGroups_.push_back({ r->name, std::move(names) });
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ATTACHMENT")))
	{
		HairInfo h;
		h.slot = r->intOr("attach slot", 2);
		gendered(r, "mesh", h.mesh);
		gendered(r, "texture map", h.tex);
		gendered(r, "head texture", h.head);
		h.diffuseCh = std::clamp(r->intOr("hair diffuse channel", 0), 0, 3);
		h.alphaCh = std::clamp(r->intOr("hair alpha channel", 3), 0, 3);
		h.headCh[0] = std::clamp(r->intOr("head channel", 0), 0, 3);
		h.headCh[1] = std::clamp(r->intOr("head channel female", h.headCh[0]), 0, 3);
		h.headAlphaCh[0] = std::clamp(r->intOr("head alpha channel", 1), 0, 3);
		h.headAlphaCh[1] = std::clamp(r->intOr("head alpha channel female", h.headAlphaCh[0]), 0, 3);
		h.threshold = (float)r->intOr("alpha rejection", 128) / 255.0f;
		h.spec = r->floatOr("specular", 1.0f);
		h.mipBias = r->floatOr("mipmap bias", 0.0f);
		hair_[folded(r->name)] = std::move(h);
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CONSTANTS"))) { blendRate_ = r->floatOr("animation blend rate", blendRate_); }
	std::vector<std::pair<std::string, std::string>> stances;	// stringID, ANIMATION name
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMATION")))
	{
		AnimInfo a;
		a.name = r->name;
		a.clip = r->stringOr("anim name", r->name);
		for (size_t i = 0; i < std::size(kDeleteGroups); i++) { a.deletes |= r->boolOr(kDeleteGroups[i].field, false) ? 1u << i : 0u; }
		a.loop = r->boolOr("loop", true);
		a.reverse = r->boolOr("reverse looping", false);
		a.synchs = r->boolOr("synchs", false);
		a.relocates = r->boolOr("relocates", false);
		a.synchOffset = r->floatOr("synch offset", 0.0f);
		// locomotion (docs/reference/LOCOMOTION.md section 2)
		const std::string layer = folded(r->stringOr("layer", "all"));
		a.code = layer == "lower" ? 0 : layer == "overlay" ? 2 : layer == "tail" ? 3 : layer == "ears" ? 4 : 1;
		a.upper = layer == "upper";
		a.idle = r->boolOr("idle", false);
		a.usable = !r->boolOr("disabled", false) && r->intOr("weather type", 0) <= 0;
		auto tri = [&](const char* key) { const int v = r->intOr(key, 2); return v == 0 || v == 1 ? v : 2; };
		a.handL = tri("has weapon L");
		a.handR = tri("has weapon R");
		a.combat = tri("is combat mode");
		a.stealth = tri("stealth mode");
		a.category = r->intOr("category", 0);
		const char* bits[] = { "katanas", "sabre", "blunt", "heavy weapons", "hackers", "unarmed", nullptr, nullptr, "polearm" };
		for (unsigned i = 0; i < std::size(bits); i++) { a.weaponBits |= bits[i] && r->boolOr(bits[i], false) ? 1u << i : 0u; }
		a.carry = (r->boolOr("being carried", false) ? 1u : 0u) | (r->boolOr("carrying left", false) ? 2u : 0u) | (r->boolOr("carrying right", false) ? 4u : 0u);
		a.move = r->floatOr("move speed", 0.0f);
		a.min = r->floatOr("min speed", 0.0f);
		a.max = r->floatOr("max speed", 0.0f);
		a.play = r->floatOr("play speed", 0.0f);
		a.idleChance = (float)r->intOr("idle chance", 100) * 0.01f;
		a.idleTimeMin = (float)r->intOr("idle time min", 0);	// int seconds in the data
		a.idleTimeMax = std::max(a.idleTimeMin, (float)r->intOr("idle time max", 0));
		a.crouched = r->boolOr("crouched", false);
		a.sid = r->stringID;
		for (int i = 0; i < 2; i++)
		{
			const std::string side = i == 0 ? "R leg damage " : "L leg damage ";
			a.leg[i][0] = r->floatOr(side + "min", 0.0f);
			a.leg[i][1] = r->floatOr(side + "max", 100.0f);
			a.leg[i][2] = r->floatOr(side + "ideal", 100.0f);
		}
		a.usesL = r->boolOr("uses left arm", false);
		a.usesR = r->boolOr("uses right arm", false);
		a.overrides = (r->boolOr("override L arm", false) ? 1u : 0u) | (r->boolOr("override R arm", false) ? 2u : 0u) | (r->boolOr("override head", false) ? 4u : 0u);
		// the editor's Idle stance choices (AppearanceManager 14007d480 over the idle list 1405bbd40): usable idles off the overlay layer, no weapon
		// in either hand, not crouched
		if (a.usable && a.idle && a.code != 2 && a.handL == 0 && a.handR == 0 && !r->boolOr("crouched", false)) { stances.push_back({ r->stringID, r->name }); }
		AnimInfo& stored = anims_[folded(r->name)] = std::move(a);
		animOrder_.push_back(&stored);
	}
	// the original's idle list order (kenshi-rwh.101): GameData's ANIMATION index walked in its sid hash order (sidorder.hpp); each record remembers its place
	{
		std::vector<std::string> sids;
		for (const AnimInfo* a : animOrder_) { sids.push_back(a->sid); }
		const std::vector<std::size_t> walk = sidorder::walk(sids);
		for (std::size_t i = 0; i < walk.size(); i++) { const_cast<AnimInfo*>(animOrder_[walk[i]])->order = (std::uint32_t)i; }
		// the findings' table (section 3, the stock 126 sids, B = 128): guard5 8, guard4 main 9, stealth idle 23, guard 1h 41, strafe upper 60, stand 1 sword 96
		std::string at;
		for (const char* n : { "guard5", "guard4 main", "stealth idle", "guard 1h", "strafe upper", "stand 1 sword" })
		{
			const auto it = anims_.find(folded(n));
			if (it != anims_.end())
			{
				const AnimInfo& a = it->second;
				char b[160];
				snprintf(b, sizeof b, " %s %u (chance %.2f dwell %.0f..%.0f hands %d/%d stealth %d combat %d bits 0x%x)", n, a.order, a.idleChance, a.idleTimeMin, a.idleTimeMax, a.handL, a.handR, a.stealth, a.combat, a.weaponBits);
				at += b;
			}
		}
		fprintf(stderr, "kclient: anim: idle list order (the sid hash walk, %zu records; the findings' stock table: guard5 8 guard4 main 9 stealth idle 23 guard 1h 41 strafe upper 60 stand 1 sword 96):%s\n", walk.size(), at.c_str());
	}
	// the root-motion clips (ANIMATION `relocates`): their feet are not planted on the ground the body moves over, so the foot lock leaves them alone
	{
		std::string list;
		for (const AnimInfo* a : animOrder_) { if (a->relocates) { list += (list.empty() ? "" : ", ") + a->name; } }
		if (!list.empty()) { fprintf(stderr, "kclient: anim: relocates (root motion): %s\n", list.c_str()); }
	}
	// the editor's measured order (findings/charedit.md, live: the slider stepped 1..8); the exe's order is the animation manager's, not the stringID. The filter
	// fields (+0x78, +0x7c, +0x9c of AnimationData) stay UNSURE, so an unlisted record sorts after these by stringID
	static const char* const kStanceOrder[] = { "squat", "idle_stand_stiff", "idle_stand_normal", "idle_stand_confident", "idle_stand_timid", "idle_stand_guard", "stand 1", "idle_stand_relax" };
	const auto rank = [](const std::string& name) { return std::find(std::begin(kStanceOrder), std::end(kStanceOrder), name) - std::begin(kStanceOrder); };
	std::sort(stances.begin(), stances.end(), [&](const auto& a, const auto& b) { return std::make_tuple(rank(a.second), a.first) < std::make_tuple(rank(b.second), b.first); });
	idleStances_.clear();
	for (const auto& [sid, name] : stances) { idleStances_.push_back(name); }
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("LOCATIONAL_DAMAGE")))
	{
		const kdata::RefCategory* refs = r->findRefs("pain anim");
		const kdata::Record* a = refs && !refs->entries.empty() ? gd.find(refs->entries[0].targetID) : nullptr;
		if (a) { pain_[folded(r->name)] = { a->name, (float)refs->entries[0].v0 * 0.01f }; }
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("COMBAT_TECHNIQUE")))
	{
		techniques_[folded(r->name)] = { r->stringOr("anim name", r->name), r->floatOr("anim speed mult", 1.0f), r->boolOr("is block", false) && !r->boolOr("is dodge", false), notesOf(gd, r, true) };
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMATION")))
	{
		const auto ai = anims_.find(folded(r->name));
		if (ai != anims_.end()) { ai->second.notes = notesOf(gd, r, false); }
	}
	fprintf(stderr, "kclient: anim notifies: %s\n", noteReport().c_str());
	actionInfo_.loop = false;
	actionInfo_.combat = actionInfo_.stealth = 2;
	// item scale (FUN_140840380): mesh x = `scale thickness`, y = `scale length`, z = `scale width`, all x `overall scale`
	auto itemScale = [](const kdata::Record* r, float out[3])
	{
		const float all = r->floatOr("overall scale", 1.0f);
		out[0] = r->floatOr("scale thickness", 1.0f) * all;
		out[1] = r->floatOr("scale length", 1.0f) * all;
		out[2] = r->floatOr("scale width", 1.0f) * all;
	};
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("WEAPON")))
	{
		// the animation category: `skill category animation override` unless ATTACK_NULL (21)
		const int over = r->intOr("skill category animation override", 21);
		WeaponInfo& w = weapons_[folded(r->name)] = { r->fileOr("mesh"), r->fileOr("bare sword"), r->fileOr("sheath"), over != 21 ? over : r->intOr("skill category", 0) };
		itemScale(r, w.scale);
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CROSSBOW")))
	{
		// The animation category a held crossbow gives the filters is 0 (live: AC+0x264 = the item's +0x238, written by 51c9c0; 5 with nothing in hand).
		// The ANIMATION `category` 8 records (stand 1 crossbow, aimwalk, aimH, reload 1..3 phase) need the controller's state int (AC+0xf0) at 8, which the
		// game never sets: aim (aimshoot held at time 0) and the weapon's `reload anim` are played directly by Task_RangedAttack (type 263, tick 14033bfb0),
		// with no layer-0 clip. Holstered = the unarmed set; drawn idle = stand 1 sword (kenshi-inspect findings/crossbow_anim.md)
		WeaponInfo& w = weapons_[folded(r->name)] = { r->fileOr("mesh"), {}, {}, 0 };
		w.crossbow = true;
		w.loaded = r->fileOr("extra mesh loaded");
		w.unloaded = r->fileOr("extra mesh unloaded");
		w.ammo = r->fileOr("live ammo");
		// GunClassPersonal ctor 14043c180: the bolt node at (`barrel pos Y`, `barrel pos Z`, 0)
		w.bolt[0] = r->floatOr("barrel pos Y", 0.0f);
		w.bolt[1] = r->floatOr("barrel pos Z", 0.0f);
		itemScale(r, w.scale);
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("MATERIAL_SPECS_WEAPON")))
	{
		WeaponModel& m = weaponModels_[folded(r->name)] = { r->fileOr("texture map"), r->fileOr("normal map"), r->floatOr("specular mult", 1.0f) };
		itemScale(r, m.scale);
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("COLOR_DATA")))
	{
		const int v = r->intOr("color 1", 0), w = r->intOr("color 2", 0);
		colours_[folded(r->name)] = { (float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f, (float)(v & 255) / 255.0f,
			(float)((w >> 16) & 255) / 255.0f, (float)((w >> 8) & 255) / 255.0f, (float)(w & 255) / 255.0f };
	}
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("FACTION")))
	{
		const kdata::RefCategory* col = r->findRefs("color");
		if (const kdata::Record* cd = col && !col->entries.empty() ? gd.find(col->entries[0].targetID) : nullptr) { factionColours_[folded(r->name)] = folded(cd->name); }
	}

	// the material's defaults (data/materials/deferred/character.material): the fallback views makeGpu publishes, a file's texture over each
	const Tex fallback[3];
	if (gpuNow) { makeGpu(fallback); }	// init is a load step (one thread, RENDER_THREAD.md §7.1 rule 7): made here, published at once
	else { rq::push("characters-init", [this, fallback] { makeGpu(fallback); }); }
	Tex* dst[3] = { &nothing_, &flat_, &white_ };
	const char* files[3] = { "nothing.dds", "flat.dds", "white.dds" };
	for (int i = 0; i < 3; i++) { *dst[i] = texture(res_->find(files[i]), fallback[i]); }
	blood_ = texture(res_->find("bloody.png"), white_);
	prewarm();
}

void Characters::makeGpu(const Tex (&fallback)[3])
{
	// one vertex layout for every program (they share char_vs, so the attribute slots agree)
	sg_pipeline_desc pd = {};
	pd.layout.buffers[0].stride = kVertexBytes;
	pd.layout.attrs[ATTR_character_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_normal] = { .buffer_index = 0, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_texcoord0] = { .buffer_index = 0, .offset = 24, .format = SG_VERTEXFORMAT_FLOAT2 };
	pd.layout.attrs[ATTR_character_tangent] = { .buffer_index = 0, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_character_bone_idx] = { .buffer_index = 0, .offset = 48, .format = SG_VERTEXFORMAT_UBYTE4 };
	pd.layout.attrs[ATTR_character_bone_wgt] = { .buffer_index = 0, .offset = 52, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_character_part_mask] = { .buffer_index = 0, .offset = 68, .format = SG_VERTEXFORMAT_FLOAT };
	pd.layout.buffers[1].stride = 8;	// per instance: the character's skin row and the part's record
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.attrs[ATTR_character_inst_ids] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_UINT2 };
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	// Ogre's default culling (clockwise = back faces culled, front faces wind counter-clockwise); the Hair material turns it off
	pd.face_winding = SG_FACEWINDING_CCW;
	pd.cull_mode = SG_CULLMODE_BACK;
	shd_ = sg_make_shader(character_shader_desc(sg_query_backend()));
	pd.shader = shd_;
	pd.label = "characters";
	pip_ = sg_make_pipeline(&pd);
	armourShd_ = sg_make_shader(character_armour_shader_desc(sg_query_backend()));
	pd.shader = armourShd_;
	pd.label = "characters-armour";
	armourPip_ = sg_make_pipeline(&pd);
	hairShd_ = sg_make_shader(character_hair_shader_desc(sg_query_backend()));
	pd.shader = hairShd_;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "characters-hair";
	hairPip_ = sg_make_pipeline(&pd);
	shadowShd_ = sg_make_shader(character_shadow_shader_desc(sg_query_backend()));
	pd.shader = shadowShd_;
	// the shadow vertex shader reads fewer attributes: its own slots
	pd.layout = {};
	pd.layout.buffers[0].stride = kVertexBytes;
	pd.layout.buffers[1].stride = 8;
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.attrs[ATTR_character_shadow_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_shadow_texcoord0] = { .buffer_index = 0, .offset = 24, .format = SG_VERTEXFORMAT_FLOAT2 };
	pd.layout.attrs[ATTR_character_shadow_bone_idx] = { .buffer_index = 0, .offset = 48, .format = SG_VERTEXFORMAT_UBYTE4 };
	pd.layout.attrs[ATTR_character_shadow_bone_wgt] = { .buffer_index = 0, .offset = 52, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_character_shadow_inst_ids] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_UINT2 };
	pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
	pd.color_count = 0;
	pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
	pd.sample_count = 1;
	pd.label = "characters-shadow";
	shadowPip_ = sg_make_pipeline(&pd);

	// The loading stand-in (kenshi-k16.2.258.1): its own vertex shader (it mixes the race base body into this build's on the per-character shape t,
	// so buffer 2 is the base body's vertices) and only the attributes it reads. The silhouette binds no texture; the double lights the plain body.
	pd = {};
	pd.layout.buffers[0].stride = kVertexBytes;
	pd.layout.attrs[ATTR_character_standin_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_standin_position1] = { .buffer_index = 2, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_standin_normal] = { .buffer_index = 0, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_character_standin_bone_idx] = { .buffer_index = 0, .offset = 48, .format = SG_VERTEXFORMAT_UBYTE4 };
	pd.layout.attrs[ATTR_character_standin_bone_wgt] = { .buffer_index = 0, .offset = 52, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.buffers[1].stride = 8;
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.attrs[ATTR_character_standin_inst_ids] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_UINT2 };
	pd.layout.buffers[2].stride = kVertexBytes;
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.face_winding = SG_FACEWINDING_CCW;
	pd.cull_mode = SG_CULLMODE_BACK;
	standinShd_ = sg_make_shader(character_standin_shader_desc(sg_query_backend()));
	pd.shader = standinShd_;
	pd.label = "characters-standin";
	standinPip_ = sg_make_pipeline(&pd);
	standinLitShd_ = sg_make_shader(character_standin_lit_shader_desc(sg_query_backend()));
	pd.shader = standinLitShd_;
	pd.label = "characters-standin-double";
	standinLitPip_ = sg_make_pipeline(&pd);

	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_BORDER;
	sd.border_color = SG_BORDERCOLOR_TRANSPARENT_BLACK;
	sd.max_anisotropy = 8;
	border_ = sg_make_sampler(&sd);
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	wrap_ = sg_make_sampler(&sd);

	const uint32_t px[3] = { 0x00000000, 0xffff8080, 0xffffffff };
	for (int i = 0; i < 3; i++)
	{
		sg_image_desc id = {};
		id.width = id.height = 1;
		id.pixel_format = SG_PIXELFORMAT_RGBA8;
		id.data = { &px[i], 4 };
		const sg_image img = sg_make_image(&id);
		images_.push_back(img);
		sg_view_desc vd = {};
		vd.texture.image = img;
		fallback[i].publish(sg_make_view(&vd));
	}
	makeSet(frame_, kMaxSkins, kMaxInsts, kMaxList, "character-frame");
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


// A character mesh's vertex and index arrays, in two halves (kenshi-k16.2.205.12.68: the build of a 1 MB+ armour mesh was 12-17 ms of a burst
// frame on the main thread; the prefetch workers now run the first half for gear meshes). buildVerts: every submesh into one buffer, the bone
// bytes the mesh's own skeleton handles (skinned) or 0 (rigid); ownBones: the mesh's skeleton bone names, for the remap. patchBones: the bone
// bytes onto the target skeleton (skinned) or the palette slot (rigid). The same arrays as the single pass before.
namespace
{
struct MeshArrays { std::vector<float> verts; std::vector<uint32_t> idx; };

void buildVerts(const ogre::Mesh& mesh, bool rigid, const std::vector<uint16_t>* partBits, int partW, int partH, MeshArrays& out)
{
	out.verts.clear();
	out.idx.clear();
	size_t nv = 0;
	for (const ogre::Submesh& sm : mesh.submeshes) { nv += mesh.geometry(sm).count; }
	out.verts.reserve(nv * kVertexFloats);
	for (const ogre::Submesh& sm : mesh.submeshes)
	{
		const ogre::Geometry& geo = mesh.geometry(sm);
		if ((!rigid && geo.boneIndex.empty()) || geo.normal.empty() || geo.uv.empty()) { continue; }
		const uint32_t base = (uint32_t)(out.verts.size() / kVertexFloats);
		for (uint32_t i = 0; i < geo.count; i++)
		{
			float v[kVertexFloats] = {};
			memcpy(v, &geo.pos[i * 3], 12);
			memcpy(v + 3, &geo.normal[i * 3], 12);
			memcpy(v + 6, &geo.uv[i * 2], 8);
			if (geo.tangentComps >= 3) { memcpy(v + 8, &geo.tangent[(size_t)i * geo.tangentComps], 12); v[11] = geo.tangentComps == 4 ? geo.tangent[(size_t)i * 4 + 3] : 1.0f; }
			else { v[8] = 1.0f; v[11] = 1.0f; }
			if (rigid)
			{
				v[13] = 1.0f;	// bone bytes 0: patchBones writes the slot
				out.verts.insert(out.verts.end(), v, v + kVertexFloats);
				continue;
			}
			memcpy(v + 12, &geo.boneIndex[i * 4], 4);
			memcpy(v + 13, &geo.boneWeight[i * 4], 16);
			// 140071f90: the part map pixel at (int(w u), int(h v)), clamped to the image
			if (partBits && !partBits->empty())
			{
				const int px = std::clamp((int)((float)partW * geo.uv[i * 2]), 0, partW - 1);
				const int py = std::clamp((int)((float)partH * geo.uv[i * 2 + 1]), 0, partH - 1);
				v[17] = (float)(*partBits)[(size_t)py * partW + px];
			}
			out.verts.insert(out.verts.end(), v, v + kVertexFloats);
		}
		const std::vector<uint32_t> tl = sm.triangleList();
		for (uint32_t k : tl) { out.idx.push_back(base + k); }
	}
}

// The mesh's own skeleton bone names (none when it links no skeleton); false with err when the link names a missing or broken file
bool ownBones(const ResourceIndex& res, const ogre::Mesh& mesh, std::vector<std::string>& names, std::string& err)
{
	names.clear();
	if (mesh.skeleton.empty()) { return true; }
	try
	{
		const std::string skelFile = res.find(mesh.skeleton);
		if (skelFile.empty()) { err = "skeleton " + mesh.skeleton + " not found"; return false; }
		const kworld::Skeleton own = kworld::loadSkeleton(skelFile);
		for (const auto& b : own.bones) { names.push_back(b.name); }
	}
	catch (const std::exception& e)
	{
		err = e.what();
		return false;
	}
	return true;
}

// rigid >= 0: every vertex on palette slot rigid. Else the bone bytes through the remap from own (the mesh's skeleton names) to target (null or
// no own names: the indices are already the body's handles, as most hair / armour meshes link no skeleton)
bool patchBones(std::vector<float>& verts, const std::string& file, const kworld::Skeleton* target, const std::vector<std::string>& own, int rigid, std::string& err)
{
	const size_t n = verts.size() / kVertexFloats;
	if (rigid >= 0)
	{
		const uint8_t bi[4] = { (uint8_t)rigid, 0, 0, 0 };
		for (size_t i = 0; i < n; i++) { memcpy(&verts[i * kVertexFloats + 12], bi, 4); }
		return true;
	}
	uint8_t remap[256];
	for (int i = 0; i < 256; i++) { remap[i] = (uint8_t)i; }
	if (target)
	{
		for (size_t i = 0; i < own.size() && i < 256; i++)
		{
			const int b = target->bone(own[i]);
			if (b < 0) { fprintf(stderr, "kclient: characters: %s: bone %s not in the body skeleton\n", file.c_str(), own[i].c_str()); }
			remap[i] = (uint8_t)std::max(b, 0);
		}
	}
	for (size_t i = 0; i < n; i++)
	{
		uint8_t bi[4];
		memcpy(bi, &verts[i * kVertexFloats + 12], 4);
		for (int k = 0; k < 4; k++)
		{
			bi[k] = remap[bi[k]];
			if (bi[k] >= kMaxBones) { err = "bone index " + std::to_string(bi[k]) + " past the shader's " + std::to_string(kMaxBones); return false; }
		}
		memcpy(&verts[i * kVertexFloats + 12], bi, 4);
	}
	return true;
}
}

// Texture decode workers (kenshi-k16.2.205.10). Jobs are keyed by the located file; a finished job holds the DdsImage until texture() takes it.
struct Characters::Prefetch
{
	struct Job { int state = 0; DdsImage img; std::string err; };	// state 0 pending / running, 1 decoded, 2 failed (under m)
	std::mutex m;
	std::condition_variable cv, done;
	std::deque<std::string> queue, bodyQueue;
	std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
	// a race's body mesh + its skeleton, parsed; keyed by bodyKey() (mesh file, newline, the RACE part map record). built: the CPU half of body()
	// done here too (kenshi-k16.2.205.12.79: the inverse binds, the part map, the vertex / index arrays with their bone bytes checked and the sole
	// points; body() then only queues the upload); null when that build failed (body() builds it again and reports the error), mesh and skel kept
	struct BodyJob { int state = 0; ogre::Mesh mesh; kworld::Skeleton skel; std::string err; std::unique_ptr<Body> built; MeshArrays arrays;
		// the race variants (restKey()) of the races sharing this body, registered before it starts; rests: each one's char.body.rest read after the build
		// (a variant added later is read by finishBody)
		std::vector<std::string> variants; std::map<std::string, Rest> rests; bool queued = false; };
	std::unordered_map<std::string, std::shared_ptr<BodyJob>> bodyJobs;
	// an armour / hair / weapon / stump mesh file, parsed (kenshi-k16.2.205.12.64) and its arrays built for a rigid (weapon) or skinned use, with its
	// own skeleton's bone names (kenshi-k16.2.205.12.68): mesh() only patches the bone bytes and queues the upload
	// patched: a skinned mesh that links no skeleton has its bone bytes checked here too (the identity remap, the same for every body;
	// kenshi-k16.2.205.12.79), patchOk / patchErr its result: mesh() then only queues the upload
	struct MeshJob { int state = 0; bool rigid = false; ogre::Mesh mesh; MeshArrays arrays; std::vector<std::string> own; bool ownOk = true; std::string ownErr, err;
		bool patched = false, patchOk = false; std::string patchErr; };
	std::unordered_map<std::string, std::shared_ptr<MeshJob>> meshJobs;
	std::deque<std::string> meshQueue;
	const ResourceIndex* res = nullptr;
	std::vector<std::thread> threads;
	bool stop = false;
	explicit Prefetch(const ResourceIndex* r) : res(r)
	{
		for (int i = 0; i < 3; i++)
		{
			threads.emplace_back([this]
			{
				for (;;)
				{
					std::string file;
					std::shared_ptr<Job> job;
					{
						std::unique_lock<std::mutex> lk(m);
						cv.wait(lk, [&] { return stop || !queue.empty() || !bodyQueue.empty() || !meshQueue.empty(); });
						if (stop) { return; }
						if (!bodyQueue.empty())
						{
							const std::string key = std::move(bodyQueue.front());
							bodyQueue.pop_front();
							const std::shared_ptr<BodyJob> bj = bodyJobs[key];
							const std::vector<std::string> variants = bj->variants;
							lk.unlock();
							const size_t nl = key.find('\n');
							const std::string mf = key.substr(0, nl);
							std::string err;
							bool ok = ogre::load(mf, bj->mesh, err);
							if (ok)
							{
								try
								{
									const std::string skelFile = res->find(bj->mesh.skeleton);
									if (skelFile.empty()) { ok = false; err = "skeleton " + bj->mesh.skeleton + " not found"; }
									else { bj->skel = kworld::loadSkeleton(skelFile); }
								}
								catch (const std::exception& e) { ok = false; err = e.what(); }
							}
							if (ok) { buildBody(*res, mf, nl == std::string::npos ? std::string() : key.substr(nl + 1), variants, *bj); }
							lk.lock();
							bj->err = err;
							bj->state = ok ? 1 : 2;
							lk.unlock();
							done.notify_all();
							continue;
						}
						if (!meshQueue.empty())
						{
							const std::string mf = std::move(meshQueue.front());
							meshQueue.pop_front();
							const std::shared_ptr<MeshJob> mj = meshJobs[mf];
							lk.unlock();
							std::string err;
							const bool ok = ogre::load(mf, mj->mesh, err);
							if (ok)
							{
								buildVerts(mj->mesh, mj->rigid, nullptr, 0, 0, mj->arrays);
								if (!mj->rigid) { mj->ownOk = ownBones(*res, mj->mesh, mj->own, mj->ownErr); }
								if (!mj->rigid && mj->ownOk && mj->own.empty())
								{
									mj->patched = true;
									mj->patchOk = patchBones(mj->arrays.verts, mf, nullptr, mj->own, -1, mj->patchErr);
								}
							}
							lk.lock();
							mj->err = err;
							mj->state = ok ? 1 : 2;
							lk.unlock();
							done.notify_all();
							continue;
						}
						file = std::move(queue.front());
						queue.pop_front();
						job = jobs[file];
					}
					DdsImage img;
					std::string err;
					const bool ok = dds_load(file, img, err);
					{
						std::lock_guard<std::mutex> lk(m);
						if (ok) { job->img = std::move(img); }
						job->err = err;
						job->state = ok ? 1 : 2;
					}
					done.notify_all();
				}
			});
		}
	}
	// body()'s CPU half for a decoded body (the same steps and order as body()'s own path); j.built stays null when the skeleton or the bone bytes
	// overflow the shader's palette, with j.mesh and j.skel left for body() to report it
	static void buildBody(const ResourceIndex& res, const std::string& mf, const std::string& partRec, const std::vector<std::string>& variants, BodyJob& j)
	{
		if (j.skel.bones.size() > (size_t)kMaxBones) { return; }
		auto b = std::make_unique<Body>();
		b->skel = std::move(j.skel);
		b->src = std::move(j.mesh);
		for (size_t i = 0; i < b->skel.bones.size(); i++)
		{
			std::array<float, 16> m;
			kworld::toMatrix(kworld::inverse(kworld::sampleModel(b->skel, nullptr, (int)i, 0.0f)), m.data());
			b->invBind.push_back(m);
		}
		readPartMap(res, partRec, *b);
		buildVerts(b->src, false, &b->partBits, b->partW, b->partH, j.arrays);
		std::string err;
		if (!patchBones(j.arrays.verts, mf, nullptr, {}, -1, err))
		{
			j.skel = std::move(b->skel);
			j.mesh = std::move(b->src);
			j.arrays = {};
			return;
		}
		fitSolePoints(*b);
		for (const std::string& v : variants)	// editor, attach, sex
		{
			const size_t a = v.find('\n'), z = v.rfind('\n');
			readRest(res, v.substr(0, a), v.substr(a + 1, z - a - 1), v.substr(z + 1) == "1" ? 1 : 0, b->skel, j.rests[v]);
		}
		j.built = std::move(b);
	}
	~Prefetch()
	{
		{ std::lock_guard<std::mutex> lk(m); stop = true; }
		cv.notify_all();
		for (std::thread& t : threads) { t.join(); }
	}
	void queueFile(const std::string& file)
	{
		std::lock_guard<std::mutex> lk(m);
		if (jobs.count(file)) { return; }
		jobs[file] = std::make_shared<Job>();
		queue.push_back(file);
		cv.notify_one();
	}
	// bodyKey(); variant a restKey() the job reads too (none: finishBody reads the race's rest itself); defer: register only, startBodies() queues
	void queueBody(const std::string& key, const std::string& variant = std::string(), bool defer = false)
	{
		std::lock_guard<std::mutex> lk(m);
		auto it = bodyJobs.find(key);
		if (it == bodyJobs.end()) { it = bodyJobs.emplace(key, std::make_shared<BodyJob>()).first; }
		BodyJob& j = *it->second;
		if (!j.queued && !variant.empty() && std::find(j.variants.begin(), j.variants.end(), variant) == j.variants.end()) { j.variants.push_back(variant); }
		if (defer || j.queued) { return; }
		j.queued = true;
		bodyQueue.push_back(key);
		cv.notify_one();
	}
	void startBodies()	// queue the registered jobs not queued yet
	{
		std::lock_guard<std::mutex> lk(m);
		for (auto& [key, j] : bodyJobs) { if (!j->queued) { j->queued = true; bodyQueue.push_back(key); } }
		cv.notify_all();
	}
	void queueMesh(const std::string& file, bool rigid)
	{
		std::lock_guard<std::mutex> lk(m);
		if (meshJobs.count(file)) { return; }
		meshJobs[file] = std::make_shared<MeshJob>();
		meshJobs[file]->rigid = rigid;
		meshQueue.push_back(file);
		cv.notify_one();
	}
	bool meshDone(const std::string& file)	// a queued decode that finished (state 1 or 2), so takeMesh does not wait (kenshi-k16.2.205.12.77)
	{
		std::lock_guard<std::mutex> lk(m);
		const auto it = meshJobs.find(file);
		return it != meshJobs.end() && it->second->state != 0;
	}
	bool meshPending(const std::string& file)
	{
		std::lock_guard<std::mutex> lk(m);
		const auto it = meshJobs.find(file);
		return it != meshJobs.end() && it->second->state == 0;
	}
	// The finished parse of file, removed from the table; null when it was never queued (the caller loads it itself). Waits for a running job.
	std::shared_ptr<MeshJob> takeMesh(const std::string& file)
	{
		std::unique_lock<std::mutex> lk(m);
		const auto it = meshJobs.find(file);
		if (it == meshJobs.end()) { return nullptr; }
		const std::shared_ptr<MeshJob> job = it->second;
		done.wait(lk, [&] { return job->state != 0; });
		meshJobs.erase(it);
		return job;
	}
	bool bodyPending(const std::string& meshFile)
	{
		std::lock_guard<std::mutex> lk(m);
		const auto it = bodyJobs.find(meshFile);
		return it != bodyJobs.end() && it->second->state == 0;
	}
	// The finished parse of meshFile, removed from the table (a later race on the same file queues its own decode); null when it was never queued.
	// wait false: null with pending set while the job is still queued or running (the caller retries next frame, kenshi-k16.2.205.12.63).
	std::shared_ptr<BodyJob> takeBody(const std::string& meshFile, bool wait, bool& pending)
	{
		pending = false;
		std::unique_lock<std::mutex> lk(m);
		const auto it = bodyJobs.find(meshFile);
		if (it == bodyJobs.end()) { return nullptr; }
		const std::shared_ptr<BodyJob> job = it->second;
		if (job->state == 0 && !wait) { pending = true; return nullptr; }
		done.wait(lk, [&] { return job->state != 0; });
		bodyJobs.erase(it);
		return job;
	}
	bool pending(const std::string& file)
	{
		std::lock_guard<std::mutex> lk(m);
		const auto it = jobs.find(file);
		return it != jobs.end() && it->second->state == 0;
	}
	// The finished decode of file (waits when it is still running), removed from the table; null when it was never queued.
	std::shared_ptr<Job> take(const std::string& file)
	{
		std::unique_lock<std::mutex> lk(m);
		const auto it = jobs.find(file);
		if (it == jobs.end()) { return nullptr; }
		const std::shared_ptr<Job> job = it->second;
		done.wait(lk, [&] { return job->state != 0; });
		jobs.erase(it);
		return job;
	}
};

const std::string& Characters::located(const std::string& rec)
{
	auto it = located_.find(rec);
	if (it == located_.end()) { it = located_.emplace(rec, rec.empty() ? std::string() : res_->locate(rec)).first; }
	return it->second;
}

std::vector<std::string> Characters::textureFiles(const Spec& s)
{
	std::vector<std::string> out;
	auto add = [&](const std::string& rec)
	{
		if (rec.empty()) { return; }
		const std::string& f = located(rec);
		if (f.size() > 4 && folded(f.substr(f.size() - 4)) == ".dds" && !views_.count(f)) { out.push_back(f); }
	};
	const int g = s.female ? 1 : 0;
	for (const RaceInfo& ri : races_)
	{
		if (folded(ri.name) != folded(s.race)) { continue; }
		for (const std::string* r : { &ri.tex[g], &ri.nrm[g], &ri.nrmSkinny[g], &ri.nrmStrong[g], &ri.mask[g] }) { add(*r); }
		for (const auto& h : ri.heads[g]) { for (const std::string& t : h) { add(t); } }
	}
	for (const std::string& w : s.wear)
	{
		const auto it = armour_.find(folded(w));
		if (it == armour_.end()) { continue; }
		const ArmourInfo& a = it->second;
		for (const std::string* r : { &a.vestTex[g], &a.vestNrm[g], &a.vestCmap[g], &a.matTex, &a.matNrm, &a.matCmap }) { add(*r); }
	}
	for (const std::string* name : { &s.hair, &s.beard })
	{
		const auto it = name->empty() ? hair_.end() : hair_.find(folded(*name));
		if (it != hair_.end()) { add(it->second.head[g]); add(it->second.tex[g]); }
	}
	for (const std::string& w : s.weapons)
	{
		if (w.empty()) { continue; }
		const size_t slash = w.find('/');
		const auto mi = weaponModels_.find(folded(slash == std::string::npos ? "Edge Type 1" : w.substr(slash + 1)));
		if (mi != weaponModels_.end()) { add(mi->second.tex); add(mi->second.nrm); }
	}
	return out;
}

// The body decode job's key for a race and sex: its mesh file and part map record (two races on one mesh may differ in part map); "" when the
// mesh is not found (kenshi-k16.2.205.12.79).
std::string Characters::bodyKey(const RaceInfo& r, int g)
{
	const std::string& f = located(r.mesh[g]);
	return f.empty() ? std::string() : f + '\n' + r.partMap[g];
}

// The race's body decode key when its body is not built yet ("" otherwise).
std::string Characters::bodyMeshFile(const Spec& s)
{
	if (bodies_.count(folded(s.race) + (s.female ? "/f" : "/m"))) { return {}; }
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(s.race)) { return bodyKey(r, s.female ? 1 : 0); } }
	return {};
}

// Every race's body mesh + skeleton decode on the workers from init on (as 7601aa7d did), so a join frame does not parse them (kenshi-k16.2.205.12.63).
void Characters::prewarm()
{
	if (!pf_) { pf_ = std::make_shared<Prefetch>(res_); }
	for (const RaceInfo& r : races_)
	{
		for (int g = 0; g < 2; g++)
		{
			if (r.mesh[g].empty()) { continue; }
			if (const std::string key = bodyKey(r, g); !key.empty()) { pf_->queueBody(key, restKey(r, g), true); }
		}
	}
	pf_->startBodies();
}

// The race part of s on the workers: its body mesh + skeleton and the race's skin and head textures (no armour, hair or weapons).
void Characters::prefetchRace(const Spec& s)
{
	if (!pf_) { pf_ = std::make_shared<Prefetch>(res_); }
	if (const std::string mf = bodyMeshFile(s); !mf.empty()) { pf_->queueBody(mf); }
	Spec race;
	race.race = s.race;
	race.female = s.female;
	for (const std::string& f : textureFiles(race)) { pf_->queueFile(f); }
}

void Characters::prefetch(const Spec& s)
{
	if (!pf_) { pf_ = std::make_shared<Prefetch>(res_); }
	const std::string mf = bodyMeshFile(s);
	if (!mf.empty()) { pf_->queueBody(mf); }
	for (const std::string& f : textureFiles(s)) { pf_->queueFile(f); }
	gearPending(s, true);
}

// The armour, hair, weapon and stump mesh files of s that mesh() would still load (not already uploaded for this body) on the workers.
// queue: queue the missing decodes. Returns true while one is still decoding (kenshi-k16.2.205.12.64).
bool Characters::gearPending(const Spec& s, bool queue)
{
	if (!pf_) { return false; }
	const int g = s.female ? 1 : 0;
	const auto bi = bodies_.find(folded(s.race) + (s.female ? "/f" : "/m"));
	const Body* body = bi == bodies_.end() ? nullptr : bi->second.get();
	bool pending = false;
	auto want = [&](const std::string& rec, bool rigid)
	{
		if (rec.empty()) { return; }
		const std::string& f = located(rec);
		if (f.empty()) { return; }
		if (!rigid && body && meshes_.count(f + "@" + std::to_string((uintptr_t)body))) { return; }
		if (rigid)
		{
			bool have = false;
			for (int n = 0; n < 4 && !have; n++) { have = meshes_.count(f + "@slot" + std::to_string(kMaxBones - 1 - n)) != 0; }
			if (have) { return; }
		}
		if (queue) { pf_->queueMesh(f, rigid); }
		pending = pending || pf_->meshPending(f);
	};
	for (const std::string& w : s.wear)
	{
		const auto it = armour_.find(folded(w));
		if (it != armour_.end()) { want(it->second.mesh[g], false); }
	}
	if (!s.hair.empty())
	{
		const auto it = hair_.find(folded(s.hair));
		if (it != hair_.end()) { want(it->second.mesh[g], false); }
	}
	for (int k = 0; k < 4; k++)
	{
		if (!(s.severed >> k & 1)) { continue; }
		for (const RaceInfo& ri : races_) { if (folded(ri.name) == folded(s.race)) { want(ri.stump[k][g], false); break; } }
	}
	for (const std::string& w : s.weapons)
	{
		if (w.empty()) { continue; }
		const auto wi = weapons_.find(folded(w.substr(0, w.find('/'))));
		if (wi == weapons_.end()) { continue; }
		const WeaponInfo& wp = wi->second;
		for (const std::string* r : { &wp.mesh, &wp.bare, &wp.sheath, &wp.loaded, &wp.unloaded, &wp.ammo }) { want(*r, true); }
	}
	return pending;
}

bool Characters::warm(const Spec& s)
{
	if (!pf_ || s.carriedBy >= 0) { return false; }
	const std::string key = folded(s.race) + (s.female ? "/f" : "/m");
	if (warmed_.count(key)) { return false; }
	const auto bi = bodies_.find(key);
	if (bi == bodies_.end())
	{
		// the body: its decode and the race's own textures done, so body() neither queues, waits nor reads a file here
		Spec race;
		race.race = s.race;
		race.female = s.female;
		prefetchRace(race);	// idempotent: the decodes a character's own prefetch already queued
		if (!texturesReady(race)) { return false; }
		CPUSTAGE_ACC("char.warm.body");
		std::string err;
		bool notReady = false;
		if (!body(s.race, s.female, err, &notReady) && !notReady) { warmed_.insert(key); }	// an error: the add reports it, as before
		return !notReady;
	}
	const Body* b = bi->second.get();
	// the skinned gear meshes add() would load for this body (armour, hair unless a head item hides it, stumps): one whose decode finished
	const int g = s.female ? 1 : 0;
	bool did = false;
	auto gear = [&](const std::string& rec)
	{
		if (did || rec.empty()) { return; }	// one mesh a step (a 4096-vertex armour piece is 1-5 ms)
		const std::string& f = located(rec);
		if (f.empty() || meshes_.count(f + "@" + std::to_string((uintptr_t)b)) || !pf_->meshDone(f)) { return; }
		CPUSTAGE_ACC("char.warm.gear");
		std::string err;
		mesh(rec, b, err);	// an error: the add logs it when it loads the mesh again
		did = true;
	};
	bool hideHair = false;
	for (const std::string& w : s.wear)
	{
		const auto it = armour_.find(folded(w));
		if (it == armour_.end()) { continue; }
		gear(it->second.mesh[g]);
		hideHair |= it->second.hideHair && it->second.slot == 3;
	}
	if (!s.hair.empty() && !hideHair)
	{
		const auto it = hair_.find(folded(s.hair));
		if (it != hair_.end()) { gear(it->second.mesh[g]); }
	}
	for (int k = 0; k < 4; k++) { if (s.severed >> k & 1) { gear(b->stump[k]); } }
	if (did) { return true; }
	// the race-only build: s's shape (its morph bake) and locomotion records (their contacts, cached by body, clip, record and bone sizes) without
	// the gear, whose textures may still be decoding; dropped at once
	Spec d = s;
	d.wear.clear();
	d.weapons.clear();
	d.hair.clear();
	d.beard.clear();
	d.drawn = false;
	d.severed = 0;
	d.carry = 0;
	d.action.clear();
	warmed_.insert(key);
	CPUSTAGE_ACC("char.warm.build");
	std::string err;
	if (add(d, err, nullptr)) { chars_.pop_back(); }
	return true;
}

bool Characters::texturesReady(const Spec& s)
{
	if (!pf_) { return true; }
	if (const std::string mf = bodyMeshFile(s); !mf.empty() && pf_->bodyPending(mf)) { return false; }
	for (const std::string& f : textureFiles(s)) { if (pf_->pending(f)) { return false; } }
	return !gearPending(s, false);
}

// The decode stays here (logic side, or a prefetch worker's finished one); the image and its view are a stream-lane job.
Characters::Tex Characters::makeView(std::shared_ptr<const DdsImage> dds, std::shared_ptr<const std::vector<uint8_t>> rgba, int w, int h)
{
	Tex out;
	auto finish = [this, out](sg_image img)
	{
		images_.push_back(img);
		sg_view_desc vd = {};
		vd.texture.image = img;
		out.publish(sg_make_view(&vd));
	};
	if (dds)	// a large one in slices over several drains (dds_queue_image, kenshi-k16.2.205.12.74); the Handle is ready once the last one ran
	{
		dds_queue_image(std::move(dds), "character-texture", dds_slice(), [](size_t b, std::function<void()> job) { rq::push("character-texture", std::move(job), rq::Lane::kStream, b); }, finish);
		return out;
	}
	const size_t bytes = rgba->size();
	rq::push("character-texture", [finish, rgba = std::move(rgba), w, h]
	{
		sg_image_desc id = {};
		id.width = w;
		id.height = h;
		id.pixel_format = SG_PIXELFORMAT_RGBA8;
		id.data = { rgba->data(), rgba->size() };
		id.label = "character-texture";
		finish(sg_make_image(&id));
	}, rq::Lane::kStream, bytes);
	return out;
}

Characters::Tex Characters::texture(const std::string& recordPath, const Tex& fallback)
{
	CPUSTAGE_ACC("char.tex");	// every texture() call incl. its sub-stages below (kenshi-k16.2.205.13c: stage timing, behaviour-neutral)
	const std::string file = recordPath.empty() ? "" : res_->locate(recordPath);
	if (file.empty())
	{
		if (!recordPath.empty()) { fprintf(stderr, "kclient: characters: texture %s not found\n", recordPath.c_str()); }
		return fallback;
	}
	if (auto it = views_.find(file); it != views_.end()) { return it->second; }
	if (file.size() > 4 && folded(file.substr(file.size() - 4)) == ".png")
	{
		int w = 0, h = 0, comp = 0;
		CPUSTAGE_ACC("char.tex.png");	// a .png decoded on the main thread
		stbi_uc* px = stbi_load(file.c_str(), &w, &h, &comp, 4);
		if (!px) { fprintf(stderr, "kclient: characters: %s: unreadable png\n", file.c_str()); return fallback; }
		auto rgba = std::make_shared<const std::vector<uint8_t>>(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
		return views_[file] = makeView(nullptr, std::move(rgba), w, h);
	}
	DdsImage dds;
	std::string err;
	std::shared_ptr<Prefetch::Job> job;
	{
		CPUSTAGE_ACC("char.tex.wait");	// the decode worker's share when its job is still queued or running (take blocks)
		job = pf_ ? pf_->take(file) : nullptr;
	}
	if (job)
	{
		if (job->state != 1) { fprintf(stderr, "kclient: characters: %s\n", job->err.c_str()); return fallback; }
		dds = std::move(job->img);
	}
	else
	{
		CPUSTAGE_ACC("char.tex.sync");	// no prefetch job: the decode runs here
		if (!dds_load(file, dds, err)) { fprintf(stderr, "kclient: characters: %s\n", err.c_str()); return fallback; }
	}
	CPUSTAGE_ACC("char.tex.queue");	// the stream-lane job's creation (the upload itself runs on rq's lane)
	return views_[file] = makeView(std::make_shared<const DdsImage>(std::move(dds)), nullptr, 0, 0);
}

bool Characters::upload(const std::string& file, const kworld::Skeleton* target, int rigid, GpuMesh& out, std::string& err)
{
	ogre::Mesh mesh;
	return ogre::load(file, mesh, err) && upload(mesh, file, target, rigid, out, err);
}

bool Characters::upload(const ogre::Mesh& mesh, const std::string& file, const kworld::Skeleton* target, int rigid, GpuMesh& out, std::string& err, const Body* parts)
{
	// Most hair / armour meshes link no skeleton: their indices are already the body's handles (the game shares the body's skeleton instance).
	std::vector<std::string> own;
	if (target && !ownBones(*res_, mesh, own, err)) { return false; }
	MeshArrays a;
	buildVerts(mesh, rigid >= 0, parts ? &parts->partBits : nullptr, parts ? parts->partW : 0, parts ? parts->partH : 0, a);
	return finish(std::move(a.verts), std::move(a.idx), file, target, own, rigid, out, err);
}

// The main-thread half: the bone bytes, then the stream job (a prefetch worker's build of a gear mesh joins here)
bool Characters::finish(std::vector<float> verts, std::vector<uint32_t> idx, const std::string& file, const kworld::Skeleton* target, const std::vector<std::string>& own, int rigid, GpuMesh& out, std::string& err, bool patched)
{
	if (!patched && !patchBones(verts, file, target, own, rigid, err)) { return false; }
	if (idx.empty()) { err = "no skinned submesh"; return false; }
	queueBuffers(out, std::move(verts), std::move(idx));
	return true;
}

void Characters::queueBuffers(GpuMesh& out, std::vector<float> verts, std::vector<uint32_t> idx)
{
	out.gpu = {};	// a fresh Handle: a draw names this mesh only once its own buffers are made
	out.indexCount = (int)idx.size();
	const size_t bytes = verts.size() * sizeof(float) + idx.size() * sizeof(uint32_t);
	auto data = std::make_shared<const std::pair<std::vector<float>, std::vector<uint32_t>>>(std::move(verts), std::move(idx));
	rq::push("character-mesh", [gpu = out.gpu, data]
	{
		Buffers b;
		sg_buffer_desc bd = {};
		bd.data = { data->first.data(), data->first.size() * sizeof(float) };
		bd.label = "character-verts";
		b.vbuf = sg_make_buffer(&bd);
		bd = {};
		bd.usage.index_buffer = true;
		bd.data = { data->second.data(), data->second.size() * sizeof(uint32_t) };
		bd.label = "character-indices";
		b.ibuf = sg_make_buffer(&bd);
		gpu.publish(b);
	}, rq::Lane::kStream, bytes);
}

void Characters::queueFree(const GpuMesh& m)
{
	rq::push("character-mesh-free", [gpu = m.gpu]
	{
		sg_destroy_buffer(gpu.get().vbuf);
		sg_destroy_buffer(gpu.get().ibuf);
	}, rq::Lane::kStream, 0);
}

const Characters::Body* Characters::body(const std::string& race, bool female, std::string& err, bool* notReady)
{
	if (notReady) { *notReady = false; }
	const std::string key = folded(race) + (female ? "/f" : "/m");
	if (auto it = bodies_.find(key); it != bodies_.end()) { return it->second.get(); }
	CPUSTAGE_ACC("char.body");	// a race body's first build: decode wait, skeleton, mesh upload, textures (cached after)
	const RaceInfo* ri = nullptr;
	for (const RaceInfo& r : races_) { if (folded(r.name) == folded(race)) { ri = &r; } }
	if (!ri) { err = "no RACE record named " + race; return nullptr; }
	const int g = female ? 1 : 0;
	const std::string meshFile = res_->locate(ri->mesh[g]);
	const std::string jobKey = bodyKey(*ri, g);
	std::shared_ptr<Prefetch::BodyJob> pre;
	{
		CPUSTAGE_ACC("char.body.wait");	// the decode worker's share of the body mesh + skeleton (0 when notReady: a pending job is not waited on)
		bool pending = false;
		pre = pf_ && !jobKey.empty() ? pf_->takeBody(jobKey, notReady == nullptr, pending) : nullptr;
		// no decode queued yet (another race on the same file took the last one): queue it rather than parse it here
		if (!pre && !pending && notReady && pf_ && !jobKey.empty()) { pf_->queueBody(jobKey, restKey(*ri, g)); pending = true; }
		if (pending) { *notReady = true; return nullptr; }	// still decoding: not an error, the caller retries next frame
	}
	std::unique_ptr<Body> b;
	const Rest* rest = nullptr;
	if (pre && pre->state == 1 && pre->built)
	{
		// the worker built the CPU half (kenshi-k16.2.205.12.79): only the stream job is made here
		b = std::move(pre->built);
		CPUSTAGE_ACC("char.body.upload");
		if (pre->arrays.idx.empty()) { err = race + ": mesh " + ri->mesh[g] + ": no skinned submesh"; return nullptr; }
		queueBuffers(b->mesh, std::move(pre->arrays.verts), std::move(pre->arrays.idx));
		b->meshFile = meshFile;
		if (const auto rit = pre->rests.find(restKey(*ri, g)); rit != pre->rests.end()) { rest = &rit->second; }
	}
	else
	{
		const bool decoded = pre && pre->state == 1;
		if (!buildBody(race, *ri, g, meshFile, decoded ? &pre->mesh : nullptr, decoded ? &pre->skel : nullptr, b, err)) { return nullptr; }
	}
	return finishBody(key, *ri, g, race, std::move(b), rest);
}

// body()'s CPU half on the logic thread when no worker built it (no prefetch, or its build failed: the error is reported here). decoded /
// decodedSkel: a worker's parse of the mesh and its skeleton (both or neither), moved from.
bool Characters::buildBody(const std::string& race, const RaceInfo& rinfo, int g, const std::string& meshFile, ogre::Mesh* decoded, kworld::Skeleton* decodedSkel, std::unique_ptr<Body>& out, std::string& err)
{
	const RaceInfo* ri = &rinfo;
	ogre::Mesh mesh;
	if (decoded) { mesh = std::move(*decoded); }
	else if (meshFile.empty() || !ogre::load(meshFile, mesh, err)) { err = race + ": mesh " + ri->mesh[g] + (meshFile.empty() ? " not found" : ": " + err); return false; }
	auto b = std::make_unique<Body>();
	try
	{
		if (decodedSkel) { b->skel = std::move(*decodedSkel); }
		else
		{
			const std::string skelFile = res_->find(mesh.skeleton);
			if (skelFile.empty()) { err = race + ": skeleton " + mesh.skeleton + " not found"; return false; }
			b->skel = kworld::loadSkeleton(skelFile);
		}
	}
	catch (const std::exception& e)
	{
		err = race + ": " + e.what();
		return false;
	}
	if (b->skel.bones.size() > (size_t)kMaxBones) { err = race + ": " + std::to_string(b->skel.bones.size()) + " bones, the shader holds " + std::to_string(kMaxBones); return false; }
	for (size_t i = 0; i < b->skel.bones.size(); i++)
	{
		std::array<float, 16> m;
		kworld::toMatrix(kworld::inverse(kworld::sampleModel(b->skel, nullptr, (int)i, 0.0f)), m.data());
		b->invBind.push_back(m);
	}
	readPartMap(*res_, ri->partMap[g], *b);
	{
		CPUSTAGE_ACC("char.body.upload");	// vertex/index build + the buffer job's creation
		if (!upload(mesh, meshFile, nullptr, -1, b->mesh, err, b.get())) { err = race + ": mesh " + ri->mesh[g] + ": " + err; return false; }
	}
	b->src = std::move(mesh);
	b->meshFile = meshFile;
	{ CPUSTAGE_ACC("char.body.soles"); fitSolePoints(*b); }
	out = std::move(b);
	return true;
}

// body()'s rest, either way: the part per bone, the race's colours, its textures, sliders and attachment points; cached under the race key.
const Characters::Body* Characters::finishBody(const std::string& key, const RaceInfo& rinfo, int g, const std::string& race, std::unique_ptr<Body> b, const Rest* rest)
{
	const RaceInfo* ri = &rinfo;

	// part per bone (140075600): the part naming the bone, else its nearest ancestor's, else part 1
	memcpy(b->bloodCol, ri->bloodCol, sizeof b->bloodCol);
	memcpy(b->portrait, ri->portrait, sizeof b->portrait);
	memcpy(b->bloodScale, ri->bloodScale, sizeof b->bloodScale);
	for (int k = 0; k < 4; k++) { b->stump[k] = ri->stump[k][g]; }
	b->bonePart.assign(b->skel.bones.size(), -1.0f);
	for (size_t i = 0; i < ri->anatomy.size(); i++)
	{
		b->parts.push_back(ri->anatomy[i].first);
		for (const std::string& name : ri->anatomy[i].second) { if (const int bone = b->skel.bone(name); bone >= 0) { b->bonePart[(size_t)bone] = (float)i; } }
	}
	std::vector<float> own = b->bonePart;
	for (size_t i = 0; i < own.size(); i++)
	{
		if (own[i] >= 0) { continue; }
		b->bonePart[i] = 1;
		for (int p = b->skel.bones[i].parent; p >= 0; p = b->skel.bones[(size_t)p].parent) { if (own[(size_t)p] >= 0) { b->bonePart[i] = own[(size_t)p]; break; } }
	}
	CPUSTAGE_DECL(bodyTexAcc);
	CPUSTAGE_OPT(bodyTexAcc, "char.body.tex");	// the body's own textures and every head variant (each texture() also counts in char.tex)
	b->tex = texture(ri->tex[g], nothing_);
	b->nrm = texture(ri->nrm[g], flat_);
	b->nrmSkinny = ri->nrmSkinny[g].empty() ? b->nrm : texture(ri->nrmSkinny[g], flat_);
	b->nrmStrong = ri->nrmStrong[g].empty() ? b->nrm : texture(ri->nrmStrong[g], flat_);
	b->mask = texture(ri->mask[g], white_);
	for (const auto& h : ri->heads[g]) { b->heads.push_back({ texture(h[0], nothing_), texture(h[1], flat_), texture(h[2], white_) }); }
	if (b->heads.empty()) { b->heads.push_back({ nothing_, flat_, white_ }); }
	CPUSTAGE_DECL(bodyRestAcc);
	CPUSTAGE_OPT(bodyRestAcc, "char.body.rest");
	// the editor xml and the attachment file: read by a worker already when the body job had this race's variant (kenshi-k16.2.205.12.81)
	Rest restOwn;
	if (!rest) { CPUSTAGE_NOTE("body %s: rest read on the logic thread", race.c_str()); readRest(*res_, ri->editor, ri->attach, g, b->skel, restOwn); rest = &restOwn; }
	if (!rest->ok) { fprintf(stderr, "kclient: characters: %s: attachment points %s not found\n", race.c_str(), ri->attach.c_str()); }
	b->skinStops = rest->skinStops;
	memcpy(b->skinRange, rest->skinRange, sizeof b->skinRange);
	b->ranges = rest->ranges;
	b->attach = rest->attach;
	return (bodies_[key] = std::move(b)).get();
}

std::string Characters::restKey(const RaceInfo& r, int g) { return r.editor + '\n' + r.attach + '\n' + (char)('0' + g); }

// The editor limits xml's skin range and sliders for sex g and the RACE's attachment points file (char.body.rest). Static: a prefetch worker runs it
// with the body's build (kenshi-k16.2.205.12.81). out.ok false when the attachment file is not found (the points stay empty; nothing logged here).
void Characters::readRest(const ResourceIndex& res, const std::string& editorRec, const std::string& attachRec, int g, const kworld::Skeleton& skel, Rest& out)
{
	const std::string editor = res.locate(editorRec);
	out.skinStops = editor.empty() ? 0 : readSkinRange(editor, g == 1, out.skinRange);
	if (!editor.empty()) { out.ranges = readRanges(editor, g == 1); }
	if (attachRec.empty()) { return; }
	const std::string phs = res.locate(attachRec);
	if (phs.empty()) { out.ok = false; }
	else { out.attach = readAttachPoints(phs, skel); }
}

// RACE `part map male/female` (140071f90): a pixel with alpha > 0 gives its exact colour's bit (any other colour is logged); the shader hides skin whose
// bits meet hiddenMask. The records name .dds files the install has only as .png (UNSURE how the game's lookup gets there; we try the .png).
// Static: a prefetch worker reads it for the body it builds (kenshi-k16.2.205.12.79).
void Characters::readPartMap(const ResourceIndex& res, const std::string& recordPath, Body& b)
{
	if (recordPath.empty()) { return; }
	std::string file = res.locate(recordPath);
	if (file.empty() && recordPath.size() > 4) { file = res.locate(recordPath.substr(0, recordPath.size() - 4) + ".png"); }
	int w = 0, h = 0, comp = 0;
	stbi_uc* px = file.empty() ? nullptr : stbi_load(file.c_str(), &w, &h, &comp, 4);
	if (!px) { fprintf(stderr, "kclient: characters: part map %s %s\n", recordPath.c_str(), file.empty() ? "not found" : "unreadable"); return; }
	static const std::pair<uint32_t, uint16_t> kColours[] = { { 0xffffff, 1 }, { 0xff0000, 2 }, { 0x00ff00, 4 }, { 0x0000ff, 8 }, { 0xffff00, 0x10 },
		{ 0xff00ff, 0x20 }, { 0x00ffff, 0x40 }, { 0xff8000, 0x80 }, { 0x8000ff, 0x100 }, { 0x008080, 0x200 } };
	b.partW = w;
	b.partH = h;
	b.partBits.assign((size_t)w * h, 0);
	int bad = 0;
	for (size_t i = 0; i < b.partBits.size(); i++)
	{
		const stbi_uc* p = px + i * 4;
		const uint32_t rgb = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
		if (p[3] == 0 || rgb == 0) { continue; }
		bool found = false;
		for (const auto& [colour, bit] : kColours) { if (rgb == colour) { b.partBits[i] = bit; found = true; } }
		bad += !found;
	}
	if (bad) { fprintf(stderr, "kclient: characters: part map %s: %d pixels of no part colour\n", file.c_str(), bad); }
	stbi_image_free(px);
}

const Characters::GpuMesh* Characters::mesh(const std::string& recordPath, const Body* body, std::string& err, int rigid)
{
	const std::string file = res_->locate(recordPath);
	if (file.empty()) { err = "mesh " + recordPath + " not found"; return nullptr; }
	const std::string key = file + "@" + (rigid >= 0 ? "slot" + std::to_string(rigid) : std::to_string((uintptr_t)body));
	if (auto it = meshes_.find(key); it != meshes_.end()) { return it->second.get(); }
	CPUSTAGE_ACC("char.mesh");	// an armour/hair/weapon mesh's first load: file read + upload
	auto m = std::make_unique<GpuMesh>();
	// a decode and build the workers already did (prefetch / add): only the bone bytes and the stream job run here (kenshi-k16.2.205.12.68); a job
	// built for the other use (rigid / skinned) builds again from its parsed mesh; no job (or a failed one) loads it as before
	std::shared_ptr<Prefetch::MeshJob> pre;
	if (pf_) { CPUSTAGE_ACC("char.mesh.wait"); pre = pf_->takeMesh(file); }
	const kworld::Skeleton* target = body ? &body->skel : nullptr;
	bool ok;
	if (pre && pre->state == 1 && pre->rigid == (rigid >= 0))
	{
		CPUSTAGE_ACC("char.mesh.finish");
		// a worker that checked the bone bytes already (no own skeleton: the same for every body) leaves only its result here
		ok = target && !pre->ownOk ? (err = pre->ownErr, false) : pre->patched && !pre->patchOk ? (err = pre->patchErr, false)
			: finish(std::move(pre->arrays.verts), std::move(pre->arrays.idx), file, target, pre->own, rigid, *m, err, pre->patched);
	}
	else
	{
		CPUSTAGE_ACC("char.mesh.build");	// the whole build here: no job, or one for the other use
		ok = pre && pre->state == 1 ? upload(pre->mesh, file, target, rigid, *m, err) : upload(file, target, rigid, *m, err);
	}
	if (!ok) { err = recordPath + ": " + err; return nullptr; }
	return (meshes_[key] = std::move(m)).get();
}

// The body with its morph poses baked in (140071ac0: Mesh::softwareVertexPoseBlend per pose with weight != 0, offsets and normals linear), cached by the weights.
const Characters::GpuMesh* Characters::morphed(const Body& body, const std::unordered_map<std::string, float>& weights, std::string& err)
{
	CPUSTAGE_ACC("char.morph");	// the key, and on a cache miss the body copy, the pose bake and the upload
	// The key first: a cache hit (346 of the 354 adds of a crowd burst) returns before the body is copied. The copy alone was 0.2-0.3 ms an add,
	// 3 ms of a burst frame's char.add (kenshi-k16.2.205.12.76).
	std::string key = body.meshFile + "@morph";
	bool any = false;
	for (const ogre::Pose& p : body.src.poses)
	{
		const auto it = weights.find(p.name);
		if (it == weights.end() || it->second == 0) { continue; }
		key += " " + p.name + "=" + std::to_string(it->second);
		any = true;
	}
	if (!any) { return &body.mesh; }
	if (auto it = meshes_.find(key); it != meshes_.end()) { return it->second.get(); }
	ogre::Mesh mesh;
	{ CPUSTAGE_ACC("char.morph.copy"); mesh = body.src; }
	for (const ogre::Pose& p : body.src.poses)
	{
		const auto it = weights.find(p.name);
		if (it == weights.end() || it->second == 0) { continue; }
		const float w = it->second;
		ogre::Geometry& g = p.submesh < 0 ? mesh.shared : mesh.submeshes[(size_t)p.submesh].geometry;
		for (size_t i = 0; i < p.index.size(); i++)
		{
			const size_t v = p.index[i];
			if (v >= g.count) { continue; }
			for (int k = 0; k < 3; k++) { g.pos[v * 3 + k] += w * p.offset[i * 3 + k]; }
			for (int k = 0; k < 3 && !p.normal.empty() && !g.normal.empty(); k++) { g.normal[v * 3 + k] += w * p.normal[i * 3 + k]; }
		}
	}
	// a live editor makes a new weight set per slider step: past kMorphCache baked bodies, drop the ones no character draws
	constexpr size_t kMorphCache = 64;
	if (std::count_if(meshes_.begin(), meshes_.end(), [](const auto& e) { return e.first.find("@morph") != std::string::npos; }) >= (std::ptrdiff_t)kMorphCache)
	{
		std::unordered_set<const GpuMesh*> used;
		for (const Char& ch : chars_) { used.insert(ch.mesh); if (ch.was) { used.insert(ch.was->mesh); } }	// the build a replace still shows draws its mesh
		std::erase_if(meshes_, [&](const auto& e)
		{
			if (e.first.find("@morph") == std::string::npos || used.count(e.second.get())) { return false; }
			queueFree(*e.second);	// the stream lane, behind its create
			return true;
		});
	}
	auto m = std::make_unique<GpuMesh>();
	CPUSTAGE_ACC("char.morph.upload");
	if (!upload(mesh, body.meshFile, nullptr, -1, *m, err, &body)) { return nullptr; }
	return (meshes_[key] = std::move(m)).get();
}

// Bone sizes from the editor values (14052e6c0, docs/reference/BODY_SHAPE.md): v = slider -> raw value (100 = 1). Bip01 is not sized.
// UNSURE (left out): the race factor R (1 for humans), the thigh 0.8 flag (prosthetics?).
void Characters::boneSizes(Char& c, const std::unordered_map<std::string, float>& v)
{
	const kworld::Skeleton& sk = c.body->skel;
	c.poseValid = false;	// a pose input changed under the cache's hash (which mixes the sizes' length only): kenshi-k16.2.205.11
	c.size.assign(sk.bones.size(), { 1, 1, 1 });
	c.posSize.assign(sk.bones.size(), { 1, 1, 1 });
	auto x = [&](const char* name) { const auto it = v.find(name); return it == v.end() ? 1.0f : it->second * 0.01f; };
	auto set = [&](const char* bone, kworld::SkelVec3 size, kworld::SkelVec3 pos = { 1, 1, 1 })
	{
		if (const int b = sk.bone(bone); b >= 0) { c.size[(size_t)b] = size; c.posSize[(size_t)b] = pos; }
	};
	auto mul = [](kworld::SkelVec3 a, kworld::SkelVec3 b) { return kworld::SkelVec3{ a.x * b.x, a.y * b.y, a.z * b.z }; };
	auto sc = [](kworld::SkelVec3 a, float f) { return kworld::SkelVec3{ a.x * f, a.y * f, a.z * f }; };
	const bool female = c.spec.female;
	const float sk0 = std::clamp(c.spec.skinny, 0.0f, 1.0f), m = c.spec.muscle;
	auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	const float tW = lerp(1, 0.7f, sk0), tC = lerp(1, 0.86f, sk0), tS = lerp(1, 0.6f, sk0), tB = lerp(1, 0.4f, sk0);
	const float M1 = lerp(1, female ? 1.24f : 1.27f, m), M2 = lerp(1, female ? 1.12f : 1.13f, m);
	const float Hs = x("Height"), Fs = x("Frame"), Y = Hs + x("Leg length") - 1;
	// the body node sits (Hs + Ls - 2) x the root bone's bind height off the logical position, so short legs reach the ground (findings/
	// ft1_feet_ground_offset.md). UNSURE: the non-human fallback (Hs - 1) and the armour `boot height` term (0 in vanilla) are left out.
	if (const int r = sk.bone("Bip01"); r >= 0) { c.rootShift = (Y - 1) * sk.bones[(size_t)r].bind.pos.y; }
	c.soleFit = false;
	c.soleLift = 0;
	c.soleRest = false;
	const float Lb = x("Legs bulk") * tW * M1;
	const float f = (x("Legs shape") * Lb + (x("Hips") - 1) / 3) * Fs;
	const kworld::SkelVec3 pelvis = { f, Y, f };
	set("Bip01 Pelvis", pelvis);
	const kworld::SkelVec3 leg = { 1, 0.95f, 1 };
	const float g = (2 - x("Legs shape")) * Lb * Fs;
	const float Fe = x("Feet") * Hs;
	for (const char* side : { "R", "L" })
	{
		const std::string b = std::string("Bip01 ") + side;
		set((b + " Thigh").c_str(), mul(pelvis, leg), { 1, Fs * (2 - Hs) * x("Hips"), 1 });
		set((b + " Calf").c_str(), mul(sc({ g, Y, g }, g), leg));
		set((b + " Foot").c_str(), sc({ Fe, x("Leg length"), Fe }, Fe));
		set((b + " Toe0").c_str(), sc({ Fe, Fe, Fe }, Fe), { 1, 1, Fe });
	}
	const float k = 1 + (x("Hips") - 1) * 0.6f, St = x("Stomach") * tS;
	set("Bip01 Spine", { k * Fs, Hs, k * St * Fs });
	set("Bip01 Spine1", { x("Waist") * tW * Fs, Hs, St * Fs });
	const float d = x("Chest") * M2 * tC - 1, cx = 1 + 0.45f * d, cz = 1 + 0.9f * d;
	set("Bip01 Spine2", { cx * Fs, Hs, cz * Fs });
	const float S = x("Shoulders") * M2;
	const float Ab = x("Arm bulk") * tW * M1, A = Ab * Fs, h = x("Hands");
	for (const char* side : { "R", "L" })
	{
		const std::string b = std::string("Bip01 ") + side;
		set((b + " Clavicle").c_str(), { S * Fs, (1 + 0.3f * (S - 1)) * Fs, S * Fs });
		set((b + " UpperArm").c_str(), sc({ A, Hs, (1 + 1.5f * (Ab - 1)) * Fs }, A), { S * cx, 1, 1 });
		set((b + " Forearm").c_str(), sc({ A, Hs, A }, A));
		set((b + " Hand").c_str(), sc({ A, Hs, A }, h * h));
	}
	set("Bip01 Neck", { x("Neck width") * tS * M2 * Fs, x("Neck length"), x("Neck") * tW * Fs });
	const float Hd = x("Head size"), q = 1 + 0.25f * (Fs - 1);
	const kworld::SkelVec3 head = { q * Hd * x("Head shape"), Hd, q * Hd };
	set("Bip01 Head", head);
	set("Bip01 Jaw", mul({ x("Jaw"), 1, 1 }, head));
	const float bs = x("Breast size") * tB;
	const float bx = bs >= 1 ? bs : 1 + 0.75f * (bs - 1), by = bs >= 1 ? bs : 1 + 0.5f * (bs - 1);
	// positional size: height and spacing only on a female (+0x140), else 1; spacing and depth follow Spine2's z and 2 - Hs
	const float h2 = 2 - Hs, bh = c.spec.female ? x("Breast height") : 1, bsp = c.spec.female ? x("Breast spacing") : 1;
	for (const char* bone : { "L Boob", "R Boob" }) { set(bone, { bx * Fs, by * Hs, bs * Fs }, { bh, bsp * cz * Fs * (1 + 0.5f * (h2 - 1)), h2 * cz * Fs }); }
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
		return;
	}
	if (!drawable(chars_[i])) { chars_[i].was = std::move(was); }
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
	if (standin_ == kStandinOff || c.replaced || !c.body) { return 0; }
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

void Characters::keep(const std::vector<int>& from)
{
	// the common case (a burst adds, nobody left or moved): from is 0..n-1, so the first n stay where they are and only the rest go, without moving
	// every character (0.2-0.7 ms a rebuild frame for a crowd; kenshi-k16.2.205.12.77)
	bool prefix = from.size() <= chars_.size();
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
		if (!editor.empty()) { out.ranges[g] = readRanges(editor, g == 1); }
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

bool Characters::add(const Spec& in, std::string& err, bool* notReady)
{
	if (notReady) { *notReady = false; }
	CPUSTAGE_ACC("char.add");	// the whole call, with char.body / char.morph / char.mesh / char.tex inside
	CPUSTAGE_DECL(lap);	// its stages in order: body, shape (the morph bake inside), layers, gear (worn items, hair, weapons), locomote, push (kenshi-k16.2.205.12.76)
	CPUSTAGE_OPT(lap, "char.add.body");
	Spec s = in;
	if (s.carriedBy >= 0)
	{
		// being carried (1405ced90): the `carry me` record alone, by name, outside the locomotion lists
		if ((size_t)s.carriedBy >= chars_.size() || !chars_[(size_t)s.carriedBy].spec.carry) { err = "carried by " + std::to_string(s.carriedBy) + ": no character there carrying"; return false; }
		// An animal's skeleton has no `carry me` (the goat: "no clip named carry me" left it lying where it fell while the sim carried it, k16.2.168),
		// nor a lying clip: it keeps its locomotion spec, frozen, until its ordinary ragdoll takes over (Ragdolls::startCarried).
		const Body* carriedBody = body(s.race, s.female, err, notReady);
		if (notReady && *notReady) { return false; }
		if (carriedBody && !carriedBody->skel.animation("carry me")) { s.speed = 0.0f; }
		else
		{
			s.loco = false;
			s.anims = { "carry me" };
			// the record's own play speed (rwh.98 2.1: 0.006, a very slow drape, not a frozen pose); the clip time advances at spec.speed while not
			// locomotive (update), so the carried body hangs instead of cycling the clip at 1x
			const auto ci = anims_.find("carry me");
			if (ci != anims_.end() && ci->second.play > 0) { s.speed = ci->second.play; }
		}
	}
	Char c;
	c.spec = s;
	c.body = body(s.race, s.female, err, notReady);
	if (!c.body) { return false; }
	// the gear meshes still decoding on the workers: add it again next frame (never missing, a frame late), kenshi-k16.2.205.12.64
	if (notReady && gearPending(s, true)) { *notReady = true; return false; }
	for (const RaceInfo& ri : races_) { if (folded(ri.name) == folded(s.race)) { c.accel = ri.accel; break; } }
	const kworld::Skeleton& sk = c.body->skel;
	// the editor values: the race xml's mids, then the spec's; morph sliders become pose weights (14007a490: v x 0.01, a negative value on the
	// opposite pose when there is one)
	CPUSTAGE_OPT(lap, "char.add.shape");
	std::unordered_map<std::string, float> values, weights;
	for (const EditorRange& r : c.body->ranges) { values[r.name] = r.mid; }
	for (const auto& [name, value] : s.shape)
	{
		if (!values.count(name)) { fprintf(stderr, "kclient: characters: %s: no editor slider named %s\n", label(s).c_str(), name.c_str()); }
		values[name] = value;
	}
	for (const EditorRange& r : c.body->ranges)
	{
		if (r.target.empty()) { continue; }
		const float w = values[r.name] * 0.01f;
		if (!r.opposite.empty() && w < 0) { weights[r.opposite] = -w; }
		else { weights[r.target] = w; }
	}
	c.mesh = morphed(*c.body, weights, err);
	if (!c.mesh) { return false; }
	boneSizes(c, values);
	c.carryLift = (values["Height"] - 80.0f) * 0.025f * 2.0f;
	// the body normals (140531820): bodyBlendNormal mixed over normalMap by muscleBlend (character.hlsl lerp)
	const float strong = std::clamp(c.spec.strong, 0.0f, 1.0f), skinny = std::clamp(c.spec.skinny, 0.0f, 1.0f);
	if (skinny > 0.25f || skinny > strong)
	{
		c.bodyNrm = strong > 0.33f ? c.body->nrmStrong : c.body->nrm;
		c.blendNrm = c.body->nrmSkinny;
		c.muscleBlend = skinny;
	} else {
		c.bodyNrm = c.body->nrm;
		c.blendNrm = c.body->nrmStrong;
		c.muscleBlend = strong;
	}
	// bloodAmount (140530b30): per body part 1 - a health ratio of the part's fields (+0x40 - +0x44 + +0x48 over a product of +0x50..+0x5c).
	// MINE: 1 - the wound's health (which of those fields is what is not traced)
	for (const auto& [part, health] : s.wounds)
	{
		for (size_t i = 0; i < c.body->parts.size() && i < std::size(c.blood); i++)
		{
			if (folded(c.body->parts[i]) == folded(part)) { c.blood[i] = std::clamp(1.0f - health, 0.0f, 1.0f); }
		}
	}
	CPUSTAGE_OPT(lap, "char.add.layers");
	auto layer = [&](const std::string& clip, unsigned deletes) { return addLayer(c, clip, deletes, s.time, err); };
	for (const std::string& name : s.anims)
	{
		const auto it = anims_.find(folded(name));
		if (it == anims_.end()) { err = "no ANIMATION named " + name; return false; }
		Layer* l = layer(it->second.clip, it->second.deletes);
		if (!l) { return false; }
		l->loop = it->second.loop;
		l->reverse = it->second.reverse;
		l->synchs = it->second.synchs;
		l->synchOffset = it->second.synchOffset;
	}
	if (s.loco)
	{
		// the hands and weapon category the animation filters see: the first weapon in the right hand when drawn, else unarmed
		c.handR = s.drawn && !s.weapons.empty();
		if (c.handR)
		{
			const auto wi = weapons_.find(folded(s.weapons[0].substr(0, s.weapons[0].find('/'))));
			c.weaponCategory = wi == weapons_.end() ? 0 : wi->second.category;
		}
		const auto si = anims_.find("strafe lower");
		const AnimInfo* strafe = si == anims_.end() ? nullptr : &si->second;
		// carrying (14051c8d0 sets the side): the layer 1/2 records' carry flags must equal it exactly (14051d660), so the carry variants replace the
		// upper body; vf30 plays `carrying left / right arm only` on the overlay layer in place of the pain overlay, at any speed
		const unsigned carryBits = s.carry == 1 ? 2u : s.carry == 2 ? 4u : 0u;
		const AnimInfo* armOnly = nullptr;
		if (carryBits)
		{
			const auto ai = anims_.find(s.carry == 1 ? "carrying left arm only" : "carrying right arm only");
			armOnly = ai == anims_.end() ? nullptr : &ai->second;
		}
		for (const AnimInfo* a : animOrder_)
		{
			// the records this character can play at all: usable, normal category, the carry state, hands that fit; moving or idle (overlays: by
			// name only, except a carry idle such as `stand carrying left`; MINE: the idle chooser taking overlay records)
			const bool carryFits = a->code == 0 ? a->carry == 0 : a->carry == carryBits;
			const bool overlayIdle = a->code == 2 && carryBits && a->idle && a != armOnly;
			if (!a->usable || a->category != 0 || !carryFits || (a->code == 2 && !overlayIdle) || (a->move == 0 && !a->idle && a != strafe) || !handsFit(c, *a)) { continue; }
			if (!sk.animation(a->clip)) { continue; }
			Layer* l = layer(a->clip, a->deletes);
			l->loop = a->loop;
			l->synchs = a->synchs;
			l->synchOffset = a->synchOffset;
			l->info = a;
			l->weight = 0;
			l->vweight = 0;
		}
		// the pain overlay (14064a100 -> vf15, played by vf30 140520000): parts under their `pain anim` threshold, most hurt first (a set ordered by
		// health: equal health keeps the first); the first whose clip passes the arm check (14051fe60) plays. An arm is busy when its hand holds
		// something; a clip needing the busy left arm falls back to its `-R` mirror if the right is free.
		// an overlay layer; its override bones (applyToNode's override flag) replace the other layers by its weight, see pose
		auto overlay = [&](const AnimInfo* a)
		{
			Layer* l = layer(a->clip, a->deletes);
			l->info = a;
			l->overlay = true;
			l->weight = 0;
			l->vweight = 0;
			const std::pair<unsigned, std::initializer_list<const char*>> groups[] = {
				{ 1u, { "Bip01 L Clavicle", "Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 Prop1" } },
				{ 2u, { "Bip01 R Clavicle", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand", "Bip01 Prop2" } },
				{ 4u, { "Bip01 Neck", "Bip01 Head" } } };
			for (const auto& [bit, bones] : groups)
			{
				if (!(a->overrides & bit)) { continue; }
				for (const char* name : bones) { if (const int b = sk.bone(name); b >= 0 && l->keep[(size_t)b]) { l->keep[(size_t)b] = 2; } }
			}
		};
		if (armOnly && sk.animation(armOnly->clip)) { overlay(armOnly); }
		std::vector<std::pair<float, const PainInfo*>> hurt;
		for (const auto& [part, health] : s.wounds)
		{
			if (armOnly) { break; }
			const auto pi = pain_.find(folded(part));
			if (pi == pain_.end() || health >= pi->second.threshold) { continue; }
			if (std::none_of(hurt.begin(), hurt.end(), [&](const auto& h) { return h.first == health; })) { hurt.push_back({ health, &pi->second }); }
		}
		std::stable_sort(hurt.begin(), hurt.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		auto armsFit = [&](const AnimInfo& a) -> const AnimInfo*
		{
			if (a.usesR && c.handR) { return nullptr; }
			if (!c.handL || !a.usesL) { return &a; }
			if (c.handR) { return nullptr; }
			const auto m = anims_.find(folded(a.clip + "-R"));
			return m == anims_.end() ? nullptr : &m->second;
		};
		for (const auto& h : hurt)
		{
			const auto ai = anims_.find(folded(h.second->anim));
			const AnimInfo* a = ai == anims_.end() ? nullptr : armsFit(ai->second);
			if (!a || !sk.animation(a->clip)) { continue; }
			overlay(a);
			break;
		}
	}
	else if (s.anims.empty() && !s.clip.empty() && !layer(s.clip, 0)) { return false; }
	// Body posture (FUN_14084b890 / 14052cf80): the editor's Posture, Shoulder set and Neck position scrub these clips at length x value x 0.01, weight 1;
	// 14086dfe0 strips their hand and prop tracks. MINE: added on top (cumulative), since averaging them in at weight 1 would halve every motion.
	const std::pair<const char*, float> postures[] = { { "postures", s.posture }, { "shoulder set", s.shoulderSet }, { "neck set", s.neckPosition } };
	// a furniture pose is complete on its own: the live sleeper's root is the clip's, the posture states do not enter (kinspect pu7)
	if (!s.posed)
	{
		for (const auto& [clip, value] : postures)
		{
			if (!sk.animation(clip)) { continue; }
			Layer* l = layer(clip, 0);
			for (const char* bone : { "Bip01 L Hand", "Bip01 R Hand", "Bip01 Prop1", "Bip01 Prop2" }) { if (const int b = sk.bone(bone); b >= 0) { l->keep[(size_t)b] = 0; } }
			l->time = l->clip->length * std::clamp(value, 0.0f, 100.0f) * 0.01f;
			c.postures.push_back(std::move(*l));
			c.layers.pop_back();
		}
	}
	CPUSTAGE_OPT(lap, "char.add.gear");
	const int g = s.female ? 1 : 0;
	// Skin tone: the colour at `skin` along the editor range; the shader darkens by 1 - colour where the mask is set (white = the texture as it is).
	// NOT TRACED: how the exe turns the editor colour into the skintone uniform; 1 - colour is the reading under which white leaves the texture alone.
	float col[3] = { 1, 1, 1 };
	if (c.body->skinStops > 0)
	{
		const float f = std::clamp(s.skin, 0.0f, 1.0f) * (float)(c.body->skinStops - 1);
		const int i = std::min((int)f, c.body->skinStops - 1), j = std::min(i + 1, c.body->skinStops - 1);
		for (int k = 0; k < 3; k++) { col[k] = c.body->skinRange[i][k] + (c.body->skinRange[j][k] - c.body->skinRange[i][k]) * (f - (float)i); }
	}
	for (int k = 0; k < 3; k++) { c.skintone[k] = 1.0f - col[k]; }

	// Worn items: a vest texture set goes into the body shader (the last worn wins), a mesh is drawn with the armour shader.
	// The vest normal's alpha is its coverage, so no shirt binds nothing.dds there (the material's flat.dds default would cover the body).
	c.vestTex = c.vestNrm = c.vestCmap = c.hairTex = c.beardTex = nothing_;
	bool hideHair = false;
	// Armour colour (traced, docs/reference/ARMOUR_COLOUR.md): the item's own `color` ref, else the wearer's (here: the faction's); none when `dont colorise`
// (FUN_1405358a0 clears it). A mesh is tinted only with a `color map`; a shirt with its `vest colormap`, or all over (white.dds) without one (140533b50).
	std::string wearerColour;
	if (!s.faction.empty())
	{
		const auto fi = factionColours_.find(folded(s.faction));
		if (fi == factionColours_.end()) { fprintf(stderr, "kclient: characters: no FACTION with a color named %s\n", s.faction.c_str()); }
		else { wearerColour = fi->second; }
	}
	// severed limbs (kinspect cq_sever_visual.md, severPart 14064edc0): the limb's skin masked out (hiddenMask bit 12 + slot), the bones below the cut
	// at scale 0 (with everything under them: a bone's scale is its own here, not inherited), a severed leg's thigh 0.8 across, and the race's stump mesh
	for (int k = 0; k < 4; k++)
	{
		if (!(s.severed >> k & 1)) { continue; }
		c.hidden |= 1 << (12 + k);
		const std::string side = k % 2 ? " R " : " L ";
		const int cut = sk.bone("Bip01" + side + (k < 2 ? "Forearm" : "Calf"));
		for (int b : sk.parentFirst)
		{
			for (int a = b; a >= 0; a = sk.bones[(size_t)a].parent)
			{
				if (a == cut) { c.boneScale.push_back({ b, { 0, 0, 0 } }); break; }
			}
		}
		if (const int thigh = k >= 2 ? sk.bone("Bip01" + side + "Thigh") : -1; thigh >= 0) { c.boneScale.push_back({ thigh, { 0.8f, 1, 0.8f } }); }
		std::string merr;
		const GpuMesh* m = c.body->stump[k].empty() ? nullptr : mesh(c.body->stump[k], c.body, merr);
		if (m) { c.stumps.push_back(m); }
		else { fprintf(stderr, "kclient: characters: %s: no stump for limb %d%s%s\n", label(s).c_str(), k, merr.empty() ? "" : ": ", merr.c_str()); }
	}
	for (const std::string& w : s.wear)
	{
		const auto it = armour_.find(folded(w));
		if (it == armour_.end()) { fprintf(stderr, "kclient: characters: no ARMOUR named %s\n", w.c_str()); continue; }
		const ArmourInfo& a = it->second;
		c.hidden |= a.hideParts;
		const auto ci = a.colorise ? colours_.find(a.colour.empty() ? wearerColour : a.colour) : colours_.end();
		if (!a.vestTex[g].empty())
		{
			c.vestTex = texture(a.vestTex[g], nothing_);
			c.vestNrm = texture(a.vestNrm[g], flat_);
			c.vestCmap = ci == colours_.end() ? nothing_ : texture(a.vestCmap[g], white_);
			if (ci != colours_.end()) { memcpy(c.vestCol, ci->second.data(), sizeof(float) * 3); }
		}
		if (a.mesh[g].empty()) { continue; }
		std::string merr;
		const GpuMesh* m = mesh(a.mesh[g], c.body, merr);
		if (!m) { fprintf(stderr, "kclient: characters: %s: %s\n", w.c_str(), merr.c_str()); continue; }
		Part p = { kArmour, m, texture(a.matTex, nothing_), texture(a.matNrm, flat_), {}, { 0, 0, 0, 1 }, 0.6f, {}, a.glossMult };
		p.cmap = nothing_;
		if (ci != colours_.end() && !a.matCmap.empty())
		{
			p.cmap = texture(a.matCmap, nothing_);
			memcpy(p.col1, ci->second.data(), sizeof p.col1);
			memcpy(p.col2, ci->second.data() + 3, sizeof p.col2);
		}
		c.parts.push_back(p);
		// NOT TRACED: hide hair is 1 on most items (shirts, pants, coats), so it is honoured on head gear (slot 3) only
		hideHair |= a.hideHair && a.slot == 3;
	}
	const auto ci = s.hair.empty() && s.beard.empty() ? colours_.end() : colours_.find(folded(s.hairColour));
	if ((!s.hair.empty() || !s.beard.empty()) && ci == colours_.end()) { fprintf(stderr, "kclient: characters: no COLOR_DATA named %s\n", s.hairColour.c_str()); }
	const std::array<float, 3> colour = ci == colours_.end() ? std::array<float, 3>{ 0.35f, 0.24f, 0.16f } : std::array<float, 3>{ ci->second[0], ci->second[1], ci->second[2] };
	memcpy(c.hairCol, colour.data(), sizeof(float) * 3);
	if (!s.hair.empty())
	{
		const auto it = hair_.find(folded(s.hair));
		if (it == hair_.end()) { fprintf(stderr, "kclient: characters: no ATTACHMENT named %s\n", s.hair.c_str()); }
		else
		{
			const HairInfo& h = it->second;
			// the head texture: hair painted on the scalp under the mesh (character.hlsl hairMap)
			c.hairTex = texture(h.head[g], nothing_);
			c.hairMult[h.headCh[g]] = 1;
			c.hairAlpha[h.headAlphaCh[g]] = 1;
			std::string merr;
			const GpuMesh* m = hideHair || h.mesh[g].empty() ? nullptr : mesh(h.mesh[g], c.body, merr);
			if (!merr.empty()) { fprintf(stderr, "kclient: characters: %s: %s\n", s.hair.c_str(), merr.c_str()); }
			if (m)
			{
				Part p = { kHair, m, texture(h.tex[g], nothing_), nothing_, {}, {}, h.threshold, { colour[0], colour[1], colour[2] }, h.spec, h.mipBias };
				p.cmap = nothing_;	// unbound (hair_fs): a ready Handle, so drawable() passes
				p.diffuseCh[h.diffuseCh] = 1;
				p.clipCh[h.alphaCh] = 1;
				c.parts.push_back(p);
			}
		}
	}
	if (!s.beard.empty())
	{
		// a beard ATTACHMENT has no mesh: its head texture is character.hlsl's beardMap in the hair colour. NOT TRACED: beardAlpha = its head channel (the
		// BeardBaseHead textures pack a beard per channel, Beard01..04 = channels 0..3 of _01; head alpha channel is 1 on every beard)
		const auto it = hair_.find(folded(s.beard));
		if (it == hair_.end()) { fprintf(stderr, "kclient: characters: no ATTACHMENT named %s\n", s.beard.c_str()); }
		else
		{
			c.beardTex = texture(it->second.head[g], nothing_);
			c.beardAlpha[it->second.headCh[g]] = 1;
		}
	}
	const char* points[2] = { "hip", "back" };
	for (size_t i = 0; i < s.weapons.size() && i < 2; i++) { if (!s.weapons[i].empty()) { addWeapon(c, s.weapons[i], points[i], i == 0 && s.drawn, s.loaded); } }	// "" = nothing at the hip
	CPUSTAGE_OPT(lap, "char.add.locomote");
	c.palette.assign((size_t)kMaxBones * 12, 0.0f);
	if (s.loco)
	{
		{ CPUSTAGE_ACC("char.add.contacts"); deriveContacts(c); }
		{ CPUSTAGE_ACC("char.add.loco.init"); locomote(c, 0.0f, true); }
		{ CPUSTAGE_ACC("char.add.loco.time"); locomote(c, s.time, false); }
		if (!s.action.empty())
		{
			// the action started actionTime ago: added at time 0, weight 0, then played forward in frame steps (the crossfades are per frame)
			CPUSTAGE_ACC("char.add.action");
			if (!startAction(c, s.action, s.actionSpeed, err)) { return false; }
			for (float t = s.actionTime; t > 0; t -= 1.0f / 60.0f) { locomote(c, std::min(t, 1.0f / 60.0f), false); }
		}
	}
	CPUSTAGE_OPT(lap, "char.add.push");
	chars_.push_back(std::move(c));
	return true;
}

// RACE `attachment points` (.phs, a Scythe physics export; layout read off data/ragdoll/attachments-weapons.phs, docs/formats/ASSET_INVENTORY.md .phs):
// a point is a length-prefixed name, a few ints, the length-prefixed bone name, that bone's bind model transform (vec3 + 3x3 by columns), one byte, and
// the item's bind model transform (3x3 by columns + vec3). A point is found by its bone string: a string naming a skeleton bone right after another string.
std::vector<Characters::AttachPoint> Characters::readAttachPoints(const std::string& file, const kworld::Skeleton& skel)
{
	std::vector<AttachPoint> out;
	std::ifstream in(file, std::ios::binary);
	const std::vector<char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	auto str = [&](size_t at, std::string& s)
	{
		uint32_t len = 0;
		if (at + 4 > d.size()) { return false; }
		memcpy(&len, &d[at], 4);
		if (len == 0 || len > 255 || at + 4 + len > d.size()) { return false; }
		for (size_t i = 0; i < len; i++) { if (d[at + 4 + i] < 32 || d[at + 4 + i] > 126) { return false; } }
		s.assign(&d[at + 4], len);
		return true;
	};
	std::string prev, s;
	size_t prevEnd = 0;
	for (size_t at = 0; at + 4 <= d.size(); at++)
	{
		if (!str(at, s)) { continue; }
		const size_t end = at + 4 + s.size();
		const int bone = skel.bone(s);
		if (bone >= 0 && !prev.empty() && at - prevEnd < 32 && end + 97 <= d.size())
		{
			float f[12];
			memcpy(f, &d[end + 49], sizeof f);
			AttachPoint p = { prev, bone, { f[0], f[1], f[2], 0, f[3], f[4], f[5], 0, f[6], f[7], f[8], 0, f[9], f[10], f[11], 1 } };
			out.push_back(std::move(p));
			prev.clear();
			at = end + 96;
			continue;
		}
		prev = s;
		prevEnd = end;
		at = end - 1;
	}
	return out;
}

// A weapon on an attachment point. Traced: FUN_1405dbd80 hangs an item on the point its inventory section names (`back` becomes `back2` for a second
// item); drawing (FUN_1405dbf80, FUN_1405d24b0) puts the `bare sword` mesh on `hands` = Bip01 Prop2 with no offset (FUN_1405373e0) and the `sheath` mesh
// on the point. Drawn or not is the wire's kFlagDrawn (the sim's equip state: the plan's EQUIP_WEAPON / UNEQUIP_WEAPON, kenshi-k16.6.132), never the
// item's wire section, which the sim sends as the held one today. The item's offset from its bone is the .phs bind transform taken out of the bone's bind (MINE reading, it matches the bone positions).
// Texture: a MATERIAL_SPECS_WEAPON (the manufacturer's weapon model); NOT TRACED which one an item gets, the spec names it (default Edge Type 1).
// Crossbows (vf123: a crossbow draws its `mesh` instead of `bare sword`; it has no sheath): the GunClassTurret part (14043d120 loads, 140436460 shows) adds
// `extra mesh loaded` while bolts are in, else `extra mesh unloaded`, and one `live ammo` per loaded bolt. The strings sit on the item's node; a single bolt
// on a child node at the barrel offset (several go round a `radius`, turrets only).
void Characters::addWeapon(Char& c, const std::string& spec, const std::string& point, bool drawn, bool loaded)
{
	const size_t slash = spec.find('/');
	const std::string name = spec.substr(0, slash), model = slash == std::string::npos ? "Edge Type 1" : spec.substr(slash + 1);
	const auto wi = weapons_.find(folded(name));
	if (wi == weapons_.end()) { fprintf(stderr, "kclient: characters: no WEAPON named %s\n", name.c_str()); return; }
	const auto mi = weaponModels_.find(folded(model));
	if (mi == weaponModels_.end()) { fprintf(stderr, "kclient: characters: no MATERIAL_SPECS_WEAPON named %s\n", model.c_str()); }
	const WeaponModel wm = mi == weaponModels_.end() ? WeaponModel{} : mi->second;
	const kworld::Skeleton& sk = c.body->skel;
	auto mount = [&](const std::string& file, const std::string& at, const float* offset = nullptr)
	{
		if (file.empty()) { return; }
		// local: the item in bind-pose model space (the bone's skinning matrix takes the bind out); on `hands` the bone's own bind transform
		Char::Mount mt = { kMaxBones - 1 - (int)c.mounts.size(), -1, {} };
		if (at == "hands" && (mt.bone = sk.bone("Bip01 Prop2")) >= 0) { kworld::toMatrix(kworld::sampleModel(sk, nullptr, mt.bone, 0.0f), mt.local); }
		for (const AttachPoint& p : c.body->attach)
		{
			if (p.name == at) { mt.bone = p.bone; memcpy(mt.local, p.model, sizeof mt.local); }
		}
		if (mt.bone < 0) { fprintf(stderr, "kclient: characters: %s: no attachment point %s\n", label(c.spec).c_str(), at.c_str()); return; }
		// the item node's scale (140536210: WEAPON x its MATERIAL_SPECS_WEAPON), in the item's own axes
		const float scale[16] = { wi->second.scale[0] * wm.scale[0], 0, 0, 0, 0, wi->second.scale[1] * wm.scale[1], 0, 0, 0, 0, wi->second.scale[2] * wm.scale[2], 0, 0, 0, 0, 1 };
		mul(mt.local, mt.local, scale);
		if (offset)
		{
			// the bolt node: translate, then (14043d120, when vt+0xc0 holds) orientation x Rx(-90 deg) x Rz(-90 deg): the bolt's x, y, z along the item's z, x, y
			const float t[16] = { 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0, offset[0], offset[1], offset[2], 1 };
			mul(mt.local, mt.local, t);
		}
		if (mt.slot < (int)sk.bones.size()) { fprintf(stderr, "kclient: characters: %s: no palette slot left for %s\n", label(c.spec).c_str(), name.c_str()); return; }
		std::string err;
		const GpuMesh* m = mesh(file, nullptr, err, mt.slot);
		if (!m) { fprintf(stderr, "kclient: characters: %s: %s\n", name.c_str(), err.c_str()); return; }
		c.mounts.push_back(mt);
		// the dump line (dump bones -> clips): where the item hangs, in the hand or sheathed
		c.mountNames.push_back((at == "hands" ? std::string("in the hand") : std::string("sheathed")) + " @ " + at + " (" + (mt.bone >= 0 && (size_t)mt.bone < sk.bones.size() ? sk.bones[(size_t)mt.bone].name : std::string("?")) + ")");
		Part p = { kArmour, m, texture(wm.tex, nothing_), texture(wm.nrm, flat_), {}, { 0, 0, 0, 1 }, 0.6f, {}, wm.spec };
		p.cmap = nothing_;
		p.mounted = true;
		c.parts.push_back(p);
	};
	const WeaponInfo& w = wi->second;
	const std::string& at = drawn ? std::string("hands") : point;
	if (drawn && !w.crossbow)
	{
		mount(w.bare, at);
		mount(w.sheath, point);
	}
	else { mount(w.mesh, at); }
	if (w.crossbow)
	{
		mount(loaded ? w.loaded : w.unloaded, at);
		if (loaded) { mount(w.ammo, at, w.bolt); }
	}
}

// The hand filter (14051d240): per hand 0 = must be empty, 1 = must hold a weapon, 2 = either; a record wanting both passes with exactly one held.
bool Characters::handsFit(const Char& c, const AnimInfo& a) const
{
	if (a.handL == 1 && a.handR == 1) { return c.handL != c.handR; }
	auto fits = [](bool held, int want) { return !(held && want == 0) && !(!held && want == 1); };
	return fits(c.handL, a.handL) && fits(c.handR, a.handR);
}

// A window's weight (14051cd10, used for speed bands and leg health): rises lo..mid, falls mid..hi; open below when lo < 1, above when hi < 0.5.
// Constants verified (pe.py): lower clamp DAT_141681b38 = 0.0, open-below -40 (1416e459c), open-above 0.5 (14167e8d8), 1.0 (14167c308).
static float windowHat(float x, float lo, float hi, float mid)
{
	if (x < -40 && lo < -40) { return 1.0f; }
	float w = 0;
	if (x < lo || x > mid)
	{
		if (mid <= x && (x <= hi || hi < 0.5f)) { w = 1.0f - (x - mid) / (hi - mid); }
	}
	else { w = lo < 1.0f ? 1.0f : (x - lo) / (mid - lo); }
	return std::clamp(w, 0.0f, 1.0f);
}

// Locomotion (docs/reference/LOCOMOTION.md): moving, every moving record weighs in by its speed band at x = spec.move (hat, sharpened by a sigmoid, the
// upper list's weapon filter), normalised per layer, the synching clips on one shared phase that advances x x the weighted play speed of the lower list
// (or, with anim.stride_warp, by the clips' measured stance ground: task L4).
// At rest the idle chooser: the stored stance if it fits, else the highest `idle chance` (the first of equals). Weights move linearly toward their targets
// at the layer's rate (faster fading in at a fast clip). instant: weights jump (the first frame).
// Combat (vf28, docs/reference/COMBAT_ANIM.md): the lower list weighs in at any speed (the shuffle holds still at 0), the chosen idle is the guard stance
// (weight = its band hat, so it stays alone on layer 1), and `strafe lower` takes frac = |lateral| / (|fwd| + |lateral|) of the lower layer.
// An action (technique, stumble) takes layer 1 alone: the rest of layer 1 fades out, the lower layer is capped at 1 - its weight; it plays from 0 at its
// speed and is cut at progress 0.98 (fading out), the clips it displaced restarting from time 0.
// Carrying: the admitted records are the carry variants (locoAdd), plus the arm overlay.
// NOT YET: the record flag that skips the leg window (+0x9f), the idle re-pick timer (the chooser is deterministic here), hesitate holds, `slave anim`
// (the carrier's clip names `carry me` for the carried; whether it syncs the two is not traced).
void Characters::locomote(Char& c, float dt, bool instant)
{
	const Spec& s = c.spec;
	// every locomotion layer carries its record; one without (a pose clip left on a character that walks again) crashed the client
	// (2026-11-06, characters.cpp:1661). Loud, once per character, and this character's locomotion stops instead of the client.
	for (const Layer& l : c.layers)
	{
		if (l.info) { continue; }
		if (!c.infoless)
		{
			std::string anims;
			for (const std::string& a : s.anims) { anims += " " + a; }
			fprintf(stderr, "kclient: BUG: moving character%s with a record-less layer (should not happen, see layer-null): clip %s, %zu layers, move %.2f clip '%s' anims [%s ] action '%s' carriedBy %d carry %d posed %d\n",
				idTag(s).c_str(), l.clip ? l.clip->name.c_str() : "?", c.layers.size(), s.move, s.clip.c_str(), anims.c_str(), s.action.c_str(), s.carriedBy, s.carry, (int)s.posed);
			c.infoless = true;
		}
		return;
	}
	// Speed == 0 is the idle chooser (140520500, LOCOMOTION.md 1); the sim's stop gives exactly 0 but the drawn body's measured velocity (the catch-up, the trimmed
	// mean) leaves a residue of hundredths of a unit a second, and any non-zero speed weighs the walk clips and the weights are normalised to 1, so a standing guard
	// held `walk lower` / `walk upper` at frame 0 (a frozen stride, arms hanging; kenshi-h2v.11.5.1). MINE: below 0.5 u/s (a tenth of the slowest walk) the body is at rest.
	constexpr float kRestSpeed = 0.5f;
	const float x = std::fabs(s.move) < kRestSpeed ? 0.0f : s.move;
	auto stateFits =[&](const AnimInfo& a) { return (a.combat == 2 || (a.combat == 1) == s.combat) && (a.stealth == 2 || (a.stealth == 1) == s.stealth); };
	// the weapon filter (14051d660, layer 1/2 records): the held category's bit, or unarmed, which damps records that want a weapon
	auto weaponFactor = [&](const AnimInfo& a)
	{
		if (a.code != 1 && a.code != 2) { return 1.0f; }
		if (c.weaponCategory == 5) { return a.handL == 1 || a.handR == 1 ? 0.1f : 1.0f; }
		return a.weaponBits & (1u << c.weaponCategory) ? 1.0f : 0.0f;
	};
	// the leg window (14051d660): the worse leg's value against that leg's window. The window peaks at the record's `max` and closes at its `ideal`
	// (cd10(v, min, ideal, max)): the only reading that fits the live samples (kinspect legs_anim.md: healthy 100 = `walk lower` 1 (60..100, ideal 90),
	// left leg 49.7 = `limp L lower` 1 (-100..75, ideal 50))
	auto legFactor = [&](const AnimInfo& a)
	{
		const int i = s.legs[0] <= s.legs[1] ? 0 : 1;
		return windowHat(s.legs[i], a.leg[i][0], a.leg[i][2], a.leg[i][1]);
	};
	Layer* action = c.action >= 0 ? &c.layers[(size_t)c.action] : nullptr;
	const bool acting = action && !c.actionDone;
	// strafe split (14051de70): frac of the lower layer to `strafe lower`, played at play speed x lateral speed
	const float sum = std::fabs(x) + std::fabs(s.strafe);
	const float frac = s.combat && sum > 1 ? std::fabs(s.strafe) / sum : 0.0f;
	float total[5] = {};
	for (Layer& l : c.layers)
	{
		const AnimInfo& a = *l.info;
		if (&a == &actionInfo_) { continue; }
		if (l.overlay)
		{
			// vf30: the pain overlay only below speed 50 (141683170; MINE: not during an action), the carry arm at any speed
			l.target = s.carry || (x <= 50 && !acting) ? 1.0f : 0.0f;
			l.rate = a.play;
			continue;
		}
		l.target = 0;
		l.rate = a.idle ? a.play : a.play * x;	// K = 1 for humans
		if (a.move == 0 && !a.idle)
		{
			// strafe lower (the only one admitted with no band): timed by the lateral speed over its planted stride (plan 3, coordinator
			// 2026-11-07), never by the forward-driven shared phase; the record's play sign keeps the clip's direction. A bad planted stride
			// is caught by the warp's same 12x sanity guard, logged once per character and clip.
			const float base = a.play * s.strafe;	// the original's rate: the sanity reference
			l.rate = l.contacts.stride > 0 ? (a.play < 0 ? -s.strafe : s.strafe) / l.contacts.stride : base;	// the direction stays the record's
			if (base != 0 && std::fabs(l.rate) > 12.0f * std::fabs(base))
			{
				l.rate = std::copysign(12.0f * std::fabs(base), l.rate);
				if (std::find(c.warpClamped.begin(), c.warpClamped.end(), a.name) == c.warpClamped.end())
				{
					c.warpClamped.push_back(a.name);
					fprintf(stderr, "kclient: stride warp: %s clip \"%s\" (lateral) clamped to 12x (stride %.2f u/cycle, lateral %.1f u/s)\n",
						label(c.spec).c_str(), a.name.c_str(), l.contacts.stride, s.strafe);
					evlog::add("warp", -1, c.spec.id, "%s clip \"%s\" (lateral) clamped to 12x (stride %.2f u/cycle, lateral %.1f u/s)", label(c.spec).c_str(), a.name.c_str(),
						l.contacts.stride, s.strafe);
				}
			}
			l.target = frac > 0 && stateFits(a) ? frac : 0.0f;
			continue;
		}
		if ((x == 0 && !s.combat) || a.idle || !stateFits(a)) { continue; }
		// kenshi-k16.2.237.1 (MINE): the game data has no backward clip outside combat (`walk lower combat shuffle long BK` is the only backward set and its state
		// filter is combat), so out of combat a body backing away (the direct drive's S) takes the forward walk / jog / run bands by its speed's magnitude; the
		// layer's rate keeps the speed's sign, so the clip plays in reverse, upper body (arms) included, as the combat sets' negative play speeds do.
		const float hat = windowHat(s.combat ? x : std::fabs(x), a.min, a.max, a.move);
		if (hat <= 0) { continue; }
		l.target = 1.0f / (1.0f + std::exp(-(2.0f * hat - 1.0f) * 10.0f)) * legFactor(a) * (a.upper ? weaponFactor(a) : 1.0f);
		total[a.code] += l.target;
	}
	if (x == 0 || s.combat)
	{
		// The idle pick as the original does it (kenshi-rwh.101, rwh101_idle_pick_static.md; the gates H, the order H for the stock set):
		// - the gates are pass/fail: category (the idle list), `is combat mode` / `stealth mode` (2 or equal; nothing ranks an exact match over an `either`), `crouched`
		//   (equal to the crouch flag, which the client never sets: 0), the arm/hand fit; the weights are the weapon category's bit, the leg window and, for the normal
		//   picker only, `idle chance`; the combat picker also has the speed window's logistic (a record's band at the drawn speed).
		// - normal (14051ec80), anim.idle_draw 1: a WEIGHTED RANDOM draw over the fitting records in the idle list's order (cumulative weights, rand x total); the drawn record is held
		//   until it stops fitting or its dwell, rand x (`idle time max` - `idle time min`) + `idle time min` s, runs out (14051d3b0), so a record with no dwell is drawn again
		//   about every frame and one with a dwell (stealth idle 10-40 s) holds the pose. The generator is ours (a per-character xorshift seeded from its slot, so
		//   headless shots are stable); the exe's is rand().
		//   With anim.idle_draw 0 (the default, MINE: a deliberate deviation, rwh101 section 5) the normal pick is the combat picker's first maximum instead: every ANIMATION record
		//   of this data has `idle time` 10..40 (not only `stealth idle`, which the findings assume), so the draw would hold `stand 1 sword` half the time a sneaking armed
		//   character is drawn, and the original shows the crouch (k16.2.160.1).
		// - combat (14051d660 + 140523450): the FIRST maximum (strict <) over the same candidates in the idle list's order, held rand x 60 + 5 s.
		// - the order is the walk of the ANIMATION index in sid hash order (sidorder.hpp, AnimInfo::order), not load order.
		// MINE, not from the exe: the character's stored stance (Spec::stance) is tried first when it fits (the exe does the same through animclass+0x2e0, LOCOMOTION.md 1);
		// none fitting leaves no idle (the exe falls back to the first record of the list); the combat picker's hand-carry byte match is not modelled.
		const bool combatPick = s.combat;
		auto fits = [&](const Layer& l) { const AnimInfo& a = *l.info; return a.idle && !l.overlay && stateFits(a) && !a.crouched && handsFit(c, a); };
		auto idleWeight = [&](const Layer& l)
		{
			if (!fits(l)) { return 0.0f; }
			const AnimInfo& a = *l.info;
			float w = weaponFactor(a) * legFactor(a);
			if (combatPick)
			{
				const float hat = windowHat(x, a.min, a.max, a.move);
				w *= 1.0f / (1.0f + std::exp(-(2.0f * hat - 1.0f) * 10.0f));
			}
			else { w *= a.idleChance; }
			return w;
		};
		const int state = (s.combat ? 1 : 0) | (s.stealth ? 2 : 0);
		const bool held = c.idle >= 0 && c.idleCombat == combatPick && c.idleState == state && idleWeight(c.layers[(size_t)c.idle]) > 0;
		c.idleTimer -= dt;
		if (!held || c.idleTimer < 0)
		{
			if (c.idleRng == 0)
			{
				const std::uint32_t slot = (std::uint32_t)(&c - chars_.data());	// a temporary Char (the portrait copy) is out of the vector: any slot will do
				c.idleRng = 2463534242u ^ (slot * 2654435761u + 0x9e3779b9u);
				if (c.idleRng == 0) { c.idleRng = 1; }
			}
			auto rnd = [&]() { c.idleRng ^= c.idleRng << 13; c.idleRng ^= c.idleRng >> 17; c.idleRng ^= c.idleRng << 5; return (float)(c.idleRng >> 8) * (1.0f / 16777216.0f); };
			c.idle = -1;
			const auto st = anims_.find(folded(s.stance));
			if (!combatPick)
			{
				for (size_t i = 0; i < c.layers.size() && st != anims_.end(); i++)
				{
					if (c.layers[i].info == &st->second && fits(c.layers[i])) { c.idle = (int)i; }
				}
			}
			if (c.idle < 0)
			{
				std::vector<size_t> cand;	// the fitting records, in the idle list's order
				for (size_t i = 0; i < c.layers.size(); i++) { if (idleWeight(c.layers[i]) > 0) { cand.push_back(i); } }
				std::sort(cand.begin(), cand.end(), [&](size_t p, size_t q) { return c.layers[p].info->order < c.layers[q].info->order; });
				if (combatPick)
				{
					float best = 0;
					for (const size_t i : cand) { if (const float w = idleWeight(c.layers[i]); best < w) { best = w; c.idle = (int)i; } }	// strict <: the first maximum wins
				}
				else if (kIdleDraw.get() < 0.5)
				{
					float best = 0;
					for (const size_t i : cand) { if (const float w = idleWeight(c.layers[i]); best < w) { best = w; c.idle = (int)i; } }
				}
				else
				{
					float total = 0;
					for (const size_t i : cand) { total += idleWeight(c.layers[i]); }
					const float r = rnd() * total;
					float run = 0;
					for (const size_t i : cand)
					{
						run += idleWeight(c.layers[i]);
						if (r < run) { c.idle = (int)i; break; }
					}
					if (c.idle < 0 && !cand.empty()) { c.idle = (int)cand.back(); }
				}
			}
			c.idleCombat = combatPick;
			c.idleState = state;
			const AnimInfo* picked = c.idle >= 0 ? c.layers[(size_t)c.idle].info : nullptr;
			c.idleTimer = combatPick ? rnd() * 60.0f + 5.0f : picked ? rnd() * (picked->idleTimeMax - picked->idleTimeMin) + picked->idleTimeMin : 0.0f;
		}
		if (c.idle >= 0)
		{
			// in combat the guard stance weighs its band (-99..0..99), so it fades as speed rises (MINE: no sigmoid)
			Layer& l = c.layers[(size_t)c.idle];
			l.target = s.combat ? windowHat(x, l.info->min, l.info->max, l.info->move) : 1.0f;
			total[l.info->code] += l.target;
		}
	}
	float phaseRate = 0, strideSum = 0;
	for (Layer& l : c.layers)
	{
		if (l.target <= 0 || l.info == &actionInfo_ || l.overlay) { continue; }
		if (l.info->code != 0 || l.info->move != 0 || l.info->idle) { l.target /= total[l.info->code]; }
		if (l.info->code == 0 && l.info->move != 0) { l.target *= 1.0f - frac; }
		if (acting && l.info->code == 1) { l.target = 0; }
		if (!l.info->idle && !l.info->upper && !(l.info->code == 0 && l.info->move == 0))	// the shared phase: never the strafe lower (move 0: timed by lateral speed)
		{
			phaseRate += l.target * l.info->play;
			if (l.info->code == 0 && l.contacts.stride > 0) { strideSum += l.target * l.contacts.stride * (l.info->play < 0 ? -1.0f : 1.0f); }
		}
	}
	// stride warp (task L4): the phase rate from the clips' planted stride (deriveContacts: the ground per cycle with the feet held), so a
	// planted foot stays put. 434d373a's 1.4x band was too tight for a run (the clip's own cadence lags the sim's speed), but fdbea25c's
	// 12x let the legs whirl at several times cadence through a stop: MINE, the forward-clip bound is 2.5x, and a warp past 2x is logged once
	// per clip (the hand-off's k in the stop steer must stay under 1.4 to read as plain footing).
	float advance = x * phaseRate;
	if (strideWarp_ && strideSum != 0 && advance != 0)
	{
		const float warped = x / strideSum;
		if ((advance > 0) == (warped > 0))
		{
			const float base = advance, lo = 0.7f * advance, hi = 2.5f * advance;
			advance = std::fabs(warped) < std::fabs(lo) ? lo : warped;
			const float warp = advance / base;
			if (std::fabs(warp) > 2.0f)
			{
				const Layer* heaviest = nullptr;	// the clip that asked: the heaviest synching lower one
				for (const Layer& l : c.layers)
				{
					if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || !l.synchs || l.target <= 0 || l.contacts.stride <= 0) { continue; }
					if (!heaviest || l.target * l.contacts.stride > heaviest->target * heaviest->contacts.stride) { heaviest = &l; }
				}
				// The clips are the same for every character, so log once per clip, not once per character (13 characters x 2-3 clips was 34 lines).
				static std::vector<std::string> warpLogged;
				if (heaviest && std::find(warpLogged.begin(), warpLogged.end(), heaviest->info->name) == warpLogged.end())
				{
					warpLogged.push_back(heaviest->info->name);
					fprintf(stderr, "kclient: stride warp: clip \"%s\" warped %.1fx%s (stride %.2f u/cycle, speed %.1f u/s)\n",
						heaviest->info->name.c_str(), warp, std::fabs(advance) > std::fabs(hi) ? ", clamped to 2.5x" : "", heaviest->contacts.stride, x);
					evlog::add("warp", -1, 0, "clip \"%s\" warped %.1fx%s (stride %.2f u/cycle, speed %.1f u/s)", heaviest->info->name.c_str(), warp,
						std::fabs(advance) > std::fabs(hi) ? ", clamped to 2.5x" : "", heaviest->contacts.stride, x);
				}
			}
			if (std::fabs(advance) > std::fabs(hi)) { advance = hi; }
			c.warped = true;
		}
	}
	// stop contact (task L3, ds/stop-fix): the sim brakes at the race's `pathfind acceleration` (the drawn speed steps down at that rate),
	// so a phase rate k x the natural one covers k x the natural travel (v^2 / 2a) and can be steered to put the rest inside a sole contact
	// interval. The aim is latched once per stop (434d373a) from the blended lower clips: a rest phase counts while one sole point of the blend
	// is more than half down there (the foot lock's own rule), and the aim is the middle of the nearest such stretch the steer's 0.7..1.4 band
	// reaches. fdbea25c re-aimed every frame in a 0.7..12 band and, as nat shrinks with x^2, k climbed to the top near v = 0 and the legs
	// whirled at several times cadence through a stop (user 2026-11-07).
	// ds/stop-fix: the decel is the sim's own speed's, not the drawn one's - a catch-up step raises the drawn speed and drops it again, and the end of
	// such a step read as a big decel and fired the steer mid-run (simview's vSim)
	const float decel = dt > 0 ? (c.moveSimPrev - s.moveSim) / dt : 0.0f;
	c.moveSimPrev = s.moveSim;
	// off when the body stops, when it speeds up, or when the speed has not fallen for a few sim ticks: a ramp down to a *lower* limit
	// (a corner, a slower order) ends there, and the steer must not keep holding the phase to its windows in a normal cycle
	constexpr float kStopQuiet = 0.12f;	// seconds without a decel step (the sim's ramp steps once per tick, 0.05 s)
	if (c.stopActive && (x <= 0.0f || decel < -0.25f * c.accel)) { c.stopActive = false; }
	else if (!c.stopActive && stopContact_ && dt > 0 && x > 2.0f && decel > 0.5f * c.accel)	// MINE: from a walk or faster
	{
		c.stopActive = true;
		c.stopQuiet = 0;
		c.stopClip = nullptr;
		c.stopTarget = -1;
		c.stopKMin = c.stopKMax = 0;
		c.stopSpeed0 = x;
		c.stopN = 0;
	}
	if (c.stopActive)
	{
		// ds/stop-fix: the decel is the sim's, so the route end's dead stop no longer steps it; while the sim is already stopped and the drawn body is
		// still closing its gap (the catch-up at up to kCatchMin), the steer must stay on to that rest, not time out mid-catch-up
		const bool simStopped = std::fabs(s.wireMove) < 0.5f;
		c.stopQuiet = decel > 0.25f * c.accel || (simStopped && x > 0.5f) ? 0.0f : c.stopQuiet + dt;
		if (c.stopQuiet > kStopQuiet) { c.stopActive = false; }
	}
	if (!c.stopActive && c.stopN > 0)
	{
		// log feet: this stop's steer - the k range must stay in the 0.7..1.4 band (ds/stop-fix: fdbea25c's re-aiming ran it to 12x and the legs whirled)
		fprintf(stderr, "kclient: stop steer%s: speed %.2f -> %.2f u/s, k %.2f..%.2f, %d frames\n", idTag(c.spec).c_str(), c.stopSpeed0, x, c.stopKMin, c.stopKMax, c.stopN);
		c.stopN = 0;
	}
	if (c.stopActive && dt > 0 && x > 0 && advance > 0 && strideSum > 0)
	{
		// cycles the phase would cover unsteered before v = 0: the rate is proportional to the speed, which falls to 0, so half the rate x the time
		float nat = 0.5f * advance * (x / c.accel);
		// ds/run-stop-2: the drawn speed does not follow that ramp here - the sim's route end dead-stops its last tick (walkOrder sets the speed to
		// what is left of the route) and the drawn speed then coasts to rest through the interpolation, which made every stop land short of its aim
		// (measured -0.04..-0.18 cycles). Once the sim's own speed is 0 the drawn body's remaining travel is its distance to the sim's final
		// position, so the phase's own rate x that gap is what it will cover; that is the travel to aim by
		if (s.wireMove > -0.5f && s.wireMove < 0.5f && s.wireGap > 0) { nat = std::min(nat, advance * (s.wireGap / x)); }
		float total = 0;	// the lower moving clips' weight (the foot lock's denominator; the crossfade-would-be weight, as the lock reads it)
		for (const Layer& l : c.layers)
		{
			if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
			total += l.vweight;
		}
		// the down weight of the blend at a rest phase: each lower moving clip's windows at that phase (the shared phase + its offset); with
		// flat set, the foot's *worse* sole point (both down: a flat plant), else its better one (the foot lock's rule, either point held)
		auto depthAt = [&](float p, bool flat)
		{
			p -= std::floor(p);
			float depth = 0;	// the best foot
			for (const Layer& l : c.layers)
			{
				if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
				float q = p + l.synchOffset; q -= std::floor(q);
				for (int f = 0; f < 2; f++)
				{
					float pt[2] = {};
					for (const SoleContact& sc : l.contacts.window[f])
					{
						if (sc.end <= 1.0f ? q >= sc.start && q < sc.end : q >= sc.start || q < sc.end - 1.0f) { pt[sc.point] += l.vweight; }
					}
					depth = std::max(depth, flat ? std::min(pt[0], pt[1]) : std::max(pt[0], pt[1]));
				}
			}
			return depth;
		};
		if (total > 0)
		{
			// the aim is LATCHED once per stop (again when the heaviest lower clip changes), 434d373a's rule: fdbea25c re-aimed it every frame and,
			// as nat shrinks with x^2 and the wireGap clamp, k = aim / nat ran up to the band's top near v = 0 - the legs whirled at several times
			// cadence through a stop. The aim is the middle of a stretch the 0.7..1.4 band reaches, on a 1/128 grid over the next cycle: a good
			// stretch is one where the phase is down by more than half the blend. A flat plant (both sole points down) is tried first: a toe-off
			// edge is down past half by the lock's rule, but the foot is rolling off there and the pin cannot keep it (ds/run-stop-2); the
			// any-point stretches are the fallback
			const Layer* dom = nullptr;
			for (const Layer& l : c.layers)
			{
				if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
				if (!dom || l.vweight > dom->vweight) { dom = &l; }
			}
			if (dom && dom->info != c.stopClip)
			{
				c.stopClip = dom->info;
				c.stopTarget = -1;
				c.stopLen = 0;
				const float step = 1.0f / 128.0f, tolNeed = 0.5f * nat;	// a stretch narrower than the estimate's own error is a last resort
				auto scan = [&](bool flat, bool requireMargin, float& aimOut, float& lenOut)
				{
					float depth[129];
					for (int i = 0; i <= 128; i++) { depth[i] = depthAt(c.phase + (float)i * step, flat); }
					float bestTol = -1;
					bool in = false;
					int startI = 0;
					for (int i = 0; i <= 128; i++)
					{
						const bool good = i < 128 && depth[i] > 0.5f * total;
						if (good && !in) { in = true; startI = i; }
						if (in && (!good || i == 128))
						{
							const float start = (float)startI * step, end = (float)i * step;
							const float lo = std::max(start, 0.7f * nat), hi = std::min(end, 1.4f * nat);
							if (lo <= hi)
							{
								const float aim = 0.5f * (lo + hi), tol = std::min(aim - start, end - aim);	// the middle of the reachable part
								if (tol > bestTol && (!requireMargin || tol >= tolNeed)) { bestTol = tol; aimOut = aim; lenOut = end - start; }
							}
							in = false;
						}
					}
				};
				float aim = -1, len = 0;
				scan(true, true, aim, len);
				if (aim < 0) { scan(false, true, aim, len); }
				if (aim < 0) { scan(true, false, aim, len); }
				if (aim < 0) { scan(false, false, aim, len); }
				if (aim >= 0) { c.stopTarget = c.phase + aim; c.stopLen = len; }
			}
			if (c.stopTarget >= 0.0f)
			{
				// a rate k x the natural one, held from here, lands the phase k x nat ahead (k times the falling speed integrates to k x v^2 / 2a), so
				// k puts the interval's start at v = 0; k stays in 0.7..1.4 (434d373a's band: past it the footing stops reading as the clip's own)
				float d = c.stopTarget - c.phase;
				d -= std::round(d);	// signed distance to the start, (-0.5, 0.5]
				if (d + c.stopLen > 0.0f)	// not already past it
				{
					const float kLo = d > 0.0f ? std::max(0.7f, d / nat) : 0.7f, kHi = std::min(1.4f, (d + c.stopLen) / nat);
					const float adv0 = advance;
					if (kLo <= kHi) { advance = std::clamp(advance, kLo * advance, kHi * advance); }
					else if (kLo > 1.4f) { advance = 1.4f * advance; }	// out of reach ahead: as fast as the bound allows
					else { advance = 0.7f * advance; }	// too near: brake the phase as far as the bound allows
					const float k = adv0 != 0.0f ? advance / adv0 : 1.0f;
					c.stopKMin = c.stopN == 0 ? k : std::min(c.stopKMin, k);
					c.stopKMax = c.stopN == 0 ? k : std::max(c.stopKMax, k);
					c.stopN++;
					c.steered = true;
				}
			}
		}
	}
	c.phase += dt * advance;
	c.phase -= std::floor(c.phase);
	if (action)
	{
		action->target = c.actionDone ? 0.0f : 1.0f;
		action->rate = c.actionDone ? 0.0f : c.actionSpeed;
	}
	const float kLayerScale[5] = { 2.0f, 1.25f, 1.0f, 0.5f, 0.5f };	// x animation blend rate (1406216b0, 140636dc0)
	// Inertialization (anim.inertialize; Bollo 2016): a clip's target jumping (the clip enters or leaves the blend) is a transition. The weights
	// snap to the targets, so the new clip plays at full weight from this frame, and the length the linear ramp would have taken becomes the
	// decay's (pose). A target that drifts - a speed band sweeping as the body accelerates - keeps the ramp: there is no clip change to
	// smooth, and re-capturing an offset every frame would only lag the pose. MINE: the jump threshold, well over the band sigmoid's ~0.3/frame
	// at the sim's acceleration, so a stop or a start reads as a transition and a steady walk does not.
	constexpr float kInertJump = 0.5f;
	// A band swap is a weight change within one layer, not a new motion: a moving clip hands its layer to its band neighbour as the speed
	// crosses an edge (stepback -> stepback short), the two being the same gait at a neighbouring speed on the same phase. It keeps the
	// linear crossfade, and a decay already running keeps going: re-capturing would add the pose both clips share to the offset (the action's
	// end, where a locomotion band swapped inside its fade, swung to 44 deg/frame that way; off 16). Detected within the jumping layers only: a
	// moving clip's jump out of the blend paired with a moving clip's jump into it (any layer: the lower and upper lists hand over at slightly
	// different speeds). The idle chooser, the action and the overlays never pair - their clip leaving or entering is the motion's change (a
	// stop, a start, an action), and it captures.
	// the target a layer is actually moving toward: an action displacing the lower body pins those layers under 1 - the action's weight (the
	// cap below), so their band target is not what they ramp to. MINE: without that, a pinned lower layer read a >0.5 jump on every frame of an
	// attack and the client captured once a frame - the last capture's decay (0.11-0.25 s) was still swinging through the action's out.
	auto targetOf = [&](const Layer& l) { return acting && !c.actionBlock && l.info->code == 0 ? std::min(l.target, 1.0f - action->weight) : l.target; };
	auto bandSwap = [&](const Layer& a)
	{
		if (!inertOn_ || a.info->idle || a.overlay || &a == action) { return false; }
		const float da = targetOf(a) - a.weight;
		if (std::fabs(da) <= kInertJump) { return false; }
		for (const Layer& b : c.layers)
		{
			if (&b == &a || b.info->idle || b.overlay || &b == action) { continue; }
			// half the jump: the partner of a redistribution lags its own weight, so it can sit just under the capture threshold (walk lower
			// hands to jog lower at 0.50/0.50)
			const float db = targetOf(b) - b.weight;
			if (da > 0 ? db < -0.5f * kInertJump : db > 0.5f * kInertJump) { return true; }
		}
		return false;
	};
	// MINE (ds/inertialize-4/5): the action layer's whole blend keeps the linear crossfade, in and out; inertialization captures only a stop and
	// a start (locomotion <-> idle). A capture snapped the clips it let in to full weight on that frame and their own motion showed at once: the
	// out often comes as the body backs away, so the stepback's 27 deg/frame landed on the fade's first frames where the ramp gives 16, and the
	// in played the technique's own wind-up at full weight (10-frame peak 31 vs off 12.7; the smoother transition frame did not buy that back).
	// With the layer's weight off its target - either direction - there is no capture and the weights ramp as with anim.inertialize 0. The whole
	// of each fade, not just the jump frame: a band re-choice inside it otherwise captured and snapped every weight.
	auto actionBlend = [&] { return action && std::fabs(action->weight - action->target) > 1e-4f; };
	float fade = 0;	// this frame's transition length, s (the longest snapped layer's ramp); 0 = no transition
	if (inertOn_ && !instant && !actionBlend())
	{
		for (const Layer& l : c.layers)
		{
			const float d = targetOf(l) - l.weight;
			if (std::fabs(d) <= kInertJump || bandSwap(l)) { continue; }
			const float r = blendRate_ * kLayerScale[l.info->code];
			fade = std::max(fade, std::fabs(d) / (d > 0 ? r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) : r));
		}
	}
	for (Layer& l : c.layers)
	{
		const float r = blendRate_ * kLayerScale[l.info->code];
		if (instant) { l.vfade = 0; }
		else if (fade > 0) { l.vfade = std::fabs(l.target - l.weight) / (l.target > l.weight ? r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) : r); }
		if (instant || fade > 0) { l.weight = l.target; }
		else if (l.weight < l.target) { l.weight = std::min(l.target, l.weight + r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) * dt); }
		else { l.weight = std::max(l.target, l.weight - r * dt); }
		// the lower cap: 1 - the action's weight. MINE: not under a block, whose clip has no leg tracks (the legs would sag toward the bind pose)
		if (acting && !c.actionBlock && l.info->code == 0) { l.weight = std::min(l.weight, 1.0f - action->weight); }
		// the crossfade-would-be weight (vweight): the linear ramp above, walked on through a snap for the length that ramp would have taken. The
		// foot lock's contact windows read it: a snapped layer sits at its target this frame, which would end the stance the fade still holds down.
		if (l.vfade > 0)
		{
			l.vfade -= dt;
			l.vweight = l.vweight < l.target ? std::min(l.target, l.vweight + r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) * dt) : std::max(l.target, l.vweight - r * dt);
		}
		else { l.vweight = l.weight; }
		const float len = l.clip->length;
		// a layer in its virtual fade keeps its clip running: the crossfade would, and the contact windows the lock reads come from its phase
		if (len <= 0 || (l.weight <= 0 && l.vweight <= 0)) { continue; }
		if (l.synchs)
		{
			const float p = c.phase + l.synchOffset;
			l.time = (p - std::floor(p)) * len;
			continue;
		}
		l.time += dt * l.rate;
		l.time = l.loop ? l.time - std::floor(l.time / len) * len : std::clamp(l.time, 0.0f, len);
	}
	c.inert.pending = fade;	// the transition pose() captures the last pose from and decays onto these snapped weights
	if (acting && !action->loop && action->time >= 0.98f * action->clip->length)
	{
		// cut at progress 0.98: it fades out; what it displaced restarts from 0
		c.actionDone = true;
		for (Layer& l : c.layers) { if (l.weight <= 0 && !l.synchs) { l.time = 0; } }
	}
	if (action && c.actionDone && action->weight <= 0)
	{
		c.layers.erase(c.layers.begin() + c.action);
		c.action = -1;
		c.actionDone = false;
	}
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
	}
	for (Char& c : chars_)
	{
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

WorkPool& Characters::posePool(size_t items)
{
	// anim.pose_threads: the threads the pose pass runs on, the logic thread included (1 = serial); read on a cfg change. Under kPoseMinItems the
	// pass runs serial (a wake costs more than a few poses).
	const int want = items < kPoseMinItems ? 1 : std::max(1, kPoseThreads.get());
	if (!posePool_ || posePool_->workers() != want - 1) { posePool_ = std::make_unique<WorkPool>(want - 1, "kpose"); }
	return *posePool_;
}

// anim.action_root (fix 2): the sim's own technique displacement IS the clip's root replayed tick by tick (clip.at(t1) - clip.at(t0) in x/z, rotated
// by the aim yaw fixed at the technique's start, no root yaw, at the technique's playback rate; COMBAT_ANIM.md 5), while the client zeroed the action's
// root and drew the body at the wire, so a clip's stance foot slid under it at body speed. Draw the node along the same replay: anchor + R(yaw0) x
// (root(clip time) - root(0)), the anchor taken so the first drawn frame is on the wire. Where the wire differs (navmesh clamp, separation push,
// stumble recoil) the path eases onto it over kActEase, never away from it. The phase is the wire's own action_t x its rate, so the path cannot drift
// from the sim's integrate(). yaw0 is the sim's aim yaw (the CHAR_DYNAMIC aim tail): combat.cpp sets it from the bearing to the target at the
// technique's start and lets the body turn onto it, so it is not the body yaw; an older server without the tail: the wire's entity yaw at the
// action's first sample.
bool Characters::actionPath(Char& c, float dt)
{
	if (!actionRoot_ || !c.actPath || c.action < 0 || (size_t)c.action >= c.layers.size()) { return false; }
	const Layer& l = c.layers[(size_t)c.action];
	if (!l.clip || l.clip->length <= 0) { return false; }
	const kworld::Skeleton& sk = c.body->skel;
	const int b = sk.bone("Bip01");
	if (b < 0) { return false; }
	const float len = l.clip->length;
	const float t = (c.spec.actionTime > 0 ? c.spec.actionTime : l.time / std::max(c.actionSpeed, 1e-3f)) * c.actionSpeed;
	const float u = std::clamp(t, 0.0f, len);	// the sim clamps clipTime at the clip's length and ends the technique there (combat.cpp)
	const kworld::SkelVec3 r = kworld::sampleLocal(sk, l.clip, b, u).pos;
	const kworld::SkelVec3 d = { r.x - c.actRoot0[0], 0, r.z - c.actRoot0[1] };
	if (!c.actAnchored)
	{
		if (!std::isfinite(c.spec.yaw) || !std::isfinite(c.spec.pos[0]) || !std::isfinite(c.spec.pos[2]))
		{
			// no finite wire yaw / position this character (a sim state, not this path): draw it at the wire, no path
			static int bad = 0;
			if (bad++ < 4) { fprintf(stderr, "kclient: action path%s: %s: non-finite wire yaw %.2f pos %.1f %.1f, path off\n", idTag(c.spec).c_str(), c.spec.action.c_str(), c.spec.yaw, c.spec.pos[0], c.spec.pos[2]); }
			c.actPath = false;
			return false;
		}
		c.actYaw0 = c.spec.hasAim ? c.spec.aimYaw : c.spec.yaw;	// the aim is fixed at the technique's start
		c.actAnchored = true;
		c.actCorr[0] = c.actCorr[1] = 0;
		const kworld::SkelQuat q0 = { std::cos(c.actYaw0 * 0.5f), 0, std::sin(c.actYaw0 * 0.5f), 0 };
		const kworld::SkelVec3 o0 = kworld::rotate(q0, d);
		c.actAnchor[0] = c.spec.pos[0] - o0.x;	// the first path frame lands on the wire
		c.actAnchor[1] = c.spec.pos[2] - o0.z;
	}
	const kworld::SkelQuat qy = { std::cos(c.actYaw0 * 0.5f), 0, std::sin(c.actYaw0 * 0.5f), 0 };
	const kworld::SkelVec3 o = kworld::rotate(qy, d);
	const double path[2] = { c.actAnchor[0] + o.x, c.actAnchor[1] + o.z };
	c.actPathPt[0] = path[0];
	c.actPathPt[1] = path[1];
	const float e = std::min(1.0f, dt / kActEase);
	c.actCorr[0] += (c.spec.pos[0] - path[0] - c.actCorr[0]) * e;
	c.actCorr[1] += (c.spec.pos[2] - path[1] - c.actCorr[1]) * e;
	// never fight the wire: the drawn body may lead it by at most a step (the clip's replay between samples, or a push easing in); a clamp
	// (a dash into a crowd) is the wire's, and drawing through it would take the whole pose past it
	double drawn[2] = { path[0] + c.actCorr[0], path[1] + c.actCorr[1] };
	const double ox = drawn[0] - c.spec.pos[0], oz = drawn[1] - c.spec.pos[2], off = std::hypot(ox, oz);
	if (off > kActMaxOff) { drawn[0] = c.spec.pos[0] + ox / off * kActMaxOff; drawn[1] = c.spec.pos[2] + oz / off * kActMaxOff; }
	c.pos[0] = drawn[0];
	c.pos[2] = drawn[1];
	c.actG[0] = c.pos[0] - c.spec.pos[0];	// the hand-off's offset from the wire, should the action end right after this frame
	c.actG[1] = c.pos[2] - c.spec.pos[2];
	c.actWas = true;
	return true;
}

// 1405cda20 (every frame while carried) and 1405b5980: the carried character's node follows the carrier's upper arm, `Bip01 L UpperArm` when carrying
// left, else R (1405ced90). Position = carrier position + carrier orientation x the bone's model position + (carrier orientation x the bone's model
// orientation) x an offset in the bone's frame; the offset runs A -> B -> C as the carried's Height goes 80 -> 100 -> 120 (lift = (Height - 80) x 0.025
// x 2: 14052e6c0 +0x18c x 2). Orientation = that bone orientation x Q1 (90 degrees about x) x Q2 (14 degrees about z). The carrier is posed first (a
// lower index). The game skips this while the carried is a ragdoll (vtable +0x248), which is not modelled.
void Characters::carryPlace(Char& c, const Char& carrier, const std::vector<kworld::SkelTransform>& model, const kworld::SkelQuat& orient, const double pos[3])
{
	const kworld::Skeleton& sk = carrier.body->skel;
	const int b = sk.bone(carrier.spec.carry == 1 ? "Bip01 L UpperArm" : "Bip01 R UpperArm");
	if (b < 0 || model.empty()) { return; }
	const kworld::SkelVec3 kA = { -1.4f, 0.0f, 1.0f }, kB = { -1.9f, -1.0f, 2.6f }, kC = { -3.0f, -0.4f, 5.0f };	// 1421301c0 / b0 / a0
	const float s = c.carryLift;
	const kworld::SkelVec3& p = s < 1.0f ? kA : kB;
	const kworld::SkelVec3& q = s < 1.0f ? kB : kC;
	const float t = s < 1.0f ? s : s - 1.0f;
	const kworld::SkelVec3 offset = { p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t };
	const kworld::SkelTransform& bone = model[(size_t)b];
	const kworld::SkelVec3 bp = kworld::rotate(orient, bone.pos);
	const kworld::SkelQuat br = kworld::multiply(orient, bone.rot);
	const kworld::SkelVec3 o = kworld::rotate(br, offset);
	c.pos[0] = pos[0] + bp.x + o.x;
	c.pos[1] = pos[1] + bp.y + o.y;
	c.pos[2] = pos[2] + bp.z + o.z;
	const kworld::SkelQuat q1 = { 0.70710677f, 0.70710677f, 0, 0 }, q2 = { 0.99254614f, 0, 0, 0.12186930f };	// 1416f0b38, 1416f0b30 / b34
	c.orient = kworld::multiply(kworld::multiply(br, q1), q2);
}

// The skinning palette of this frame: node orientation x model(t) x inverse(bind) per bone, as row-major 3x4 rows.
// Layers apply like Ogre's AnimationTrack::applyToNode on a reset bone: each adds its keyframe offset scaled by its weight (translate x w, rotate by
// nlerp(identity, q, w) post-multiplied, scale lerped). MINE: weights are normalised per bone over the layers that move it (Ogre's average mode scales every
// state by 1 / total weight); with the traced deletes, lower and upper layers share no animated bone, so either reading gives the same pose.
void Characters::pose(Char& c, float dt)
{
	const kworld::Skeleton& sk = c.body->skel;
	// Pose cache (kenshi-k16.2.205.11): a frame that cannot move the pose (dt 0: the sim paused, or a scripted shot) and whose inputs are the same
	// as the last pose's computes the same palette and model; keep them (the same bone x layer sampling and blending would reproduce them bit for
	// bit). footOffset's node y step is the one input-side effect to replay (footLift is unchanged at dt 0). Any deeper state (a ragdoll's bones, the
	// get-up blend) keeps the cache off.
	const bool cacheable = dt == 0 && c.rag.empty() && c.riseDur == 0;
	std::uint64_t sig = 1469598103934665603ull;
	if (cacheable)
	{
		auto mix = [&sig](std::uint64_t v) { sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2); };
		auto fb = [](float x) { std::uint32_t u; std::memcpy(&u, &x, sizeof u); return (std::uint64_t)u; };
		auto db = [](double x) { std::uint64_t u; std::memcpy(&u, &x, sizeof u); return u; };
		mix((std::uint64_t)(std::uintptr_t)c.body);
		mix((std::uint64_t)(std::uintptr_t)c.mesh);
		mix(fb(c.orient.w)); mix(fb(c.orient.x)); mix(fb(c.orient.y)); mix(fb(c.orient.z));
		mix(db(c.pos[0])); mix(db(c.pos[1])); mix(db(c.pos[2]));
		mix((std::uint64_t)(c.spec.loco ? 1u : 0u) | (std::uint64_t)(c.spec.rootInPlace ? 2u : 0u) | (std::uint64_t)(footLock_ ? 4u : 0u));
		mix(c.layers.size()); mix(c.postures.size()); mix(c.boneScale.size()); mix(c.mounts.size()); mix(c.size.size()); mix(c.posSize.size());
		for (const Layer& l : c.layers) { mix((std::uint64_t)(std::uintptr_t)l.clip); mix((std::uint64_t)(std::uintptr_t)l.info); mix(fb(l.time)); mix(fb(l.weight)); }
		for (const Layer& l : c.postures) { mix((std::uint64_t)(std::uintptr_t)l.clip); mix(fb(l.time)); }
		for (const auto& [bone, sc] : c.boneScale) { mix((std::uint64_t)bone); mix(fb(sc.x)); mix(fb(sc.y)); mix(fb(sc.z)); }
		for (const Char::Mount& mt : c.mounts) { mix((std::uint64_t)(uint32_t)mt.slot); mix((std::uint64_t)(uint32_t)mt.bone); for (int k = 0; k < 16; k++) { mix(fb(mt.local[k])); } }
		if (c.poseValid && sig == c.poseSig)
		{
			if (c.spec.loco) { c.pos[1] += c.footLift; }	// Characters::footOffset's node step (below), the pose's only write back to the node
			return;
		}
	}
	std::vector<kworld::SkelTransform> local(sk.bones.size()), model(sk.bones.size());
	CPUSTAGE_DECL(stageAcc);
	CPUSTAGE_OPT(stageAcc, "pose.sample");
	const bool noSample = ablate::on("nosample");	// KCLIENT_ABL: bind pose, no clip sampling per bone (an upper-bound measurement)
	// The layers that can move a bone this frame: a weight-0 layer adds nothing (translate x 0, an identity rotation, a 0-lerped scale) and counts 0
	// in the normalisation, so skipping it is the same pose; a character keeps one layer per ANIMATION record it can play and holds a few at weight.
	std::vector<const Layer*> active;
	active.reserve(c.layers.size());
	for (const Layer& l : c.layers) { if (l.weight > 0) { active.push_back(&l); } }
	for (size_t b = 0; b < sk.bones.size(); b++)
	{
		const kworld::SkelTransform& bind = sk.bones[b].bind;
		if (noSample)
		{
			local[b] = bind;
			if (sk.bones[b].parent < 0) { c.rootXZ[0] = c.rootXZ[1] = 0; }
			continue;
		}
		// an override bone (keep 2; OldNodeAnimationTrack::applyToNode 180031260 with the override flag): before adding its rotation it pulls the
		// bone's accumulated orientation back toward the initial one by its weight. MINE: done as scaling the other layers by 1 - that weight.
		float total = 0, over = 0;
		for (const Layer* l : active)
		{
			if (l->keep[b] == 2) { over = std::max(over, l->weight); }
			else if (l->keep[b]) { total += l->weight; }
		}
		const float norm = (total > 1.0f ? 1.0f / total : 1.0f) * (1.0f - over);
		kworld::SkelTransform t = bind;
		auto apply = [&](const Layer& l, float w)
		{
			kworld::SkelTransform s = kworld::sampleLocal(sk, l.clip, (int)b, l.time);
			if (l.info == &actionInfo_ && sk.bones[b].parent < 0)
			{
				// an action plays in place: its root travel (gains ground, dodges) is in the sim's position already (COMBAT_ANIM.md section 5)
				const kworld::SkelTransform s0 = kworld::sampleLocal(sk, l.clip, (int)b, 0.0f);
				s.pos.x = s0.pos.x;
				s.pos.z = s0.pos.z;
			}
			// the keyframe offset back out of bind (+) offset
			t.pos.x += (s.pos.x - bind.pos.x) * w;
			t.pos.y += (s.pos.y - bind.pos.y) * w;
			t.pos.z += (s.pos.z - bind.pos.z) * w;
			const kworld::SkelQuat inv = { bind.rot.w, -bind.rot.x, -bind.rot.y, -bind.rot.z };
			kworld::SkelQuat q = kworld::multiply(inv, s.rot);
			if (q.w < 0) { q = { -q.w, -q.x, -q.y, -q.z }; }
			kworld::SkelQuat r = { 1 - w + q.w * w, q.x * w, q.y * w, q.z * w };
			const float len = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
			r = { r.w / len, r.x / len, r.y / len, r.z / len };
			t.rot = kworld::multiply(t.rot, r);
			t.scale.x *= 1 + (s.scale.x / bind.scale.x - 1) * w;
			t.scale.y *= 1 + (s.scale.y / bind.scale.y - 1) * w;
			t.scale.z *= 1 + (s.scale.z / bind.scale.z - 1) * w;
		};
		for (const Layer* l : active) { if (l->keep[b] == 1) { apply(*l, l->weight * norm); } }
		for (const Layer* l : active) { if (l->keep[b] == 2) { apply(*l, l->weight); } }
		for (const Layer& l : c.postures) { if (l.keep[b]) { apply(l, 1.0f); } }
		local[b] = t;
		// The root bone (Bip01) carries the entity's position, not just the skeleton's: the lab's root lock reports what the pose would add, then
		// zeroes it, so the caller's node carries it instead - the game's own rule for a relocates record and for an action (COMBAT_ANIM.md 5).
		if (sk.bones[b].parent < 0)
		{
			c.rootXZ[0] = t.pos.x - bind.pos.x;
			c.rootXZ[1] = t.pos.z - bind.pos.z;
			if (c.spec.rootInPlace) { local[b].pos.x = bind.pos.x; local[b].pos.z = bind.pos.z; }
		}
	}
	// anim.inertialize: the transition's decay onto the blended local pose, before the hierarchy and lockFeet, so the lock still fixes the feet
	CPUSTAGE_OPT(stageAcc, "pose.inertialize");
	if (dt > 0) { inertialize(c, local, dt); }
	CPUSTAGE_OPT(stageAcc, "pose.hierarchy");
	// OldBone::updateFromParentImpl (BODY_SHAPE.md): a bone's derived scale is its own size x its animation scale, not the parent's; its offset is
	// scaled by the parent's derived scale y x its own positional size
	std::vector<int> driven(c.rag.empty() ? 0 : sk.bones.size(), -1);	// bone -> its ragdoll entry
	for (size_t k = 0; k < c.rag.size(); k++) { if (c.rag[k].bone >= 0 && (size_t)c.rag[k].bone < driven.size()) { driven[(size_t)c.rag[k].bone] = (int)k; } }
	std::vector<int> rise(c.riseDur > 0 ? sk.bones.size() : 0, -1);	// bone -> the get-up blend's ragdoll pose
	for (size_t k = 0; k < c.riseFrom.size() && !rise.empty(); k++) { if (c.riseFrom[k].bone >= 0 && (size_t)c.riseFrom[k].bone < rise.size()) { rise[(size_t)c.riseFrom[k].bone] = (int)k; } }
	const kworld::SkelQuat unorient = { c.orient.w, -c.orient.x, -c.orient.y, -c.orient.z };
	for (int b : sk.parentFirst)
	{
		const int p = sk.bones[(size_t)b].parent;
		const kworld::SkelTransform& l = local[(size_t)b];
		kworld::SkelTransform& t = model[(size_t)b];
		if (!driven.empty() && driven[(size_t)b] >= 0)	// the body's world transform, back into the node's model space
		{
			const RagBone& r = c.rag[(size_t)driven[(size_t)b]];
			t.scale = { l.scale.x * c.size[(size_t)b].x, l.scale.y * c.size[(size_t)b].y, l.scale.z * c.size[(size_t)b].z };
			t.pos = kworld::rotate(unorient, { (float)(r.p[0] - c.pos[0]), (float)(r.p[1] - c.pos[1]), (float)(r.p[2] - c.pos[2]) });
			t.rot = kworld::multiply(unorient, r.q);
		}
		else
		{
			const kworld::SkelVec3& size = c.size[(size_t)b];
			t.scale = { l.scale.x * size.x, l.scale.y * size.y, l.scale.z * size.z };
			for (const auto& [bone, f] : c.boneScale) { if (bone == b) { t.scale = { t.scale.x * f.x, t.scale.y * f.y, t.scale.z * f.z }; } }
			if (p < 0)
			{
				t.pos = l.pos;
				t.rot = l.rot;
			}
			else
			{
				const kworld::SkelTransform& pt = model[(size_t)p];
				const kworld::SkelVec3& ps = c.posSize[(size_t)b];
				const kworld::SkelVec3 o = kworld::rotate(pt.rot, { l.pos.x * ps.x * pt.scale.y, l.pos.y * ps.y * pt.scale.y, l.pos.z * ps.z * pt.scale.y });
				t.pos = { pt.pos.x + o.x, pt.pos.y + o.y, pt.pos.z + o.z };
				t.rot = kworld::multiply(pt.rot, l.rot);
			}
		}
		// the get-up blend (riseFrom, Characters::rise): the ragdoll's last world pose cross-fades into the clip's over riseDur
		if (!rise.empty() && rise[(size_t)b] >= 0)
		{
			const RagBone& f = c.riseFrom[(size_t)rise[(size_t)b]];
			const float w = std::clamp(c.riseT / c.riseDur, 0.0f, 1.0f);
			const kworld::SkelVec3 fo = kworld::rotate(unorient, { (float)(f.p[0] - c.pos[0]), (float)(f.p[1] - c.pos[1]), (float)(f.p[2] - c.pos[2]) });
			t.pos.x = fo.x + (t.pos.x - fo.x) * w;
			t.pos.y = fo.y + (t.pos.y - fo.y) * w;
			t.pos.z = fo.z + (t.pos.z - fo.z) * w;
			kworld::SkelQuat fq = kworld::multiply(unorient, f.q);
			if (fq.w * t.rot.w + fq.x * t.rot.x + fq.y * t.rot.y + fq.z * t.rot.z < 0) { fq = { -fq.w, -fq.x, -fq.y, -fq.z }; }	// the shorter way round
			const kworld::SkelQuat q = { fq.w + (t.rot.w - fq.w) * w, fq.x + (t.rot.x - fq.x) * w, fq.y + (t.rot.y - fq.y) * w, fq.z + (t.rot.z - fq.z) * w };
			const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (len > 1e-6f) { t.rot = { q.w / len, q.x / len, q.y / len, q.z / len }; }
		}
	}
	CPUSTAGE_OPT(stageAcc, "pose.feet");
	if (c.spec.loco) { footOffset(c, model, dt); }	// the body's height from this frame's pose, before the feet are solved against it
	if (footLock_ && c.spec.loco) { lockFeet(c, local, model, dt); }
	for (int f = 0; f < 2; f++)
	{
		const int b = sk.bone(f ? "Bip01 R Foot" : "Bip01 L Foot");
		const kworld::SkelVec3 o = b >= 0 ? kworld::rotate(c.orient, model[(size_t)b].pos) : kworld::SkelVec3{};
		c.feet[f][0] = o.x; c.feet[f][1] = o.y; c.feet[f][2] = o.z;
	}
	CPUSTAGE_OPT(stageAcc, "pose.skin");
	float node[16];
	kworld::toMatrix({ {}, c.orient, { 1, 1, 1 } }, node);
	std::vector<std::array<float, 16>> skin(sk.bones.size());
	auto store = [&](size_t slot, const float m[16]) { for (int r = 0; r < 3; r++) { for (int k = 0; k < 4; k++) { c.palette[slot * 12 + r * 4 + k] = m[k * 4 + r]; } } };
	for (size_t b = 0; b < sk.bones.size(); b++)
	{
		float* m = skin[b].data();
		kworld::toMatrix(model[b], m);
		mul(m, node, m);
		mul(m, m, c.body->invBind[b].data());
		store(b, m);
	}
	for (const Char::Mount& mt : c.mounts)
	{
		float m[16];
		mul(m, skin[(size_t)mt.bone].data(), mt.local);
		store((size_t)mt.slot, m);
	}
	c.model = std::move(model);
	// the cache above: this pose's inputs, so an unchanged paused / scripted frame can keep it
	c.poseValid = cacheable;
	c.poseSig = cacheable ? sig : 0;
}

namespace
{
	kworld::SkelVec3 vadd(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	kworld::SkelVec3 vsub(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	kworld::SkelVec3 vmul(const kworld::SkelVec3& a, float s) { return { a.x * s, a.y * s, a.z * s }; }
	float vdot(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	kworld::SkelVec3 vcross(const kworld::SkelVec3& a, const kworld::SkelVec3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float vlen(const kworld::SkelVec3& a) { return std::sqrt(vdot(a, a)); }
	kworld::SkelQuat qconj(const kworld::SkelQuat& q) { return { q.w, -q.x, -q.y, -q.z }; }
	// the shortest rotation taking the unit vector a to the unit vector b
	kworld::SkelQuat qfromTo(const kworld::SkelVec3& a, const kworld::SkelVec3& b)
	{
		const float d = vdot(a, b);
		kworld::SkelQuat q;
		if (d < -0.99999f)
		{
			const kworld::SkelVec3 ax = std::fabs(a.y) < 0.9f ? kworld::SkelVec3{ 0, 1, 0 } : kworld::SkelVec3{ 1, 0, 0 };
			const kworld::SkelVec3 c = vcross(ax, a);
			q = { 0.0f, c.x, c.y, c.z };
		}
		else { q = { 1.0f + d, a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
		const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		return l > 1e-6f ? kworld::SkelQuat{ q.w / l, q.x / l, q.y / l, q.z / l } : kworld::SkelQuat{};
	}
	// the shortest rotation taking the local orientation a to b as an axis-angle vector (radians), and back: the inertialization offset, its
	// velocity and its decay (Characters::inertialize)
	kworld::SkelVec3 qaxis(const kworld::SkelQuat& a, const kworld::SkelQuat& b)
	{
		kworld::SkelQuat d = kworld::multiply(qconj(a), b);
		if (d.w < 0) { d = { -d.w, -d.x, -d.y, -d.z }; }
		const float s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		if (s < 1e-7f) { return {}; }
		const float k = 2.0f * std::atan2(s, d.w) / s;
		return { d.x * k, d.y * k, d.z * k };
	}
	kworld::SkelQuat qexp(const kworld::SkelVec3& v)
	{
		const float a = vlen(v);
		if (a < 1e-7f) { return {}; }
		const float s = std::sin(0.5f * a) / a;
		return { std::cos(0.5f * a), v.x * s, v.y * s, v.z * s };
	}
}

// Inertialization (Bollo, GDC 2016, "Inertialization: High-Performance Animation Transitions in Gears of War"; CLIENT_GOALS "Locomotion fidelity" 5).
// locomote snapped the layer weights to the new clip's targets and marked the transition with its length (the crossfade's would-be time). The pose
// shown last frame, with its velocity, is the source; each bone's offset from this frame's target pose (translation in the bone's parent frame,
// rotation as an axis-angle vector in its own) decays by 1 - quintic(t / T), the linear term carrying the velocity, so the body keeps its motion and
// the new clip plays at full weight from the transition. Per bone the fade clamps to |offset| / |velocity| when the velocity points away from the
// pose (the paper's rule), so no bone overshoots. Runs on the local pose before the hierarchy and lockFeet: the foot lock then fixes the feet on top.
void Characters::inertialize(Char& c, std::vector<kworld::SkelTransform>& local, float dt)
{
	Char::Inertial& in = c.inert;
	const size_t n = local.size();
	if (!inertOn_) { in.len = in.pending = 0; }	// 0: the pose is the linear crossfade's, and the tracking below only measures it
	bool capture = false;
	if (inertOn_ && in.pending > 0)
	{
		// the pose shown last frame needs a velocity: its interval from the frame before is that. A first pose has no source (the target stands),
		// and neither has the second: it has the one before it missing (both must be the same bone count as this pose: a body can change).
		capture = in.from.size() == n && in.prev.size() == n && in.dt > 0;
		if (capture)
		{
			in.len = in.pending;
			in.t = 0;
			in.pos.resize(n); in.posV.resize(n); in.rot.resize(n); in.rotV.resize(n); in.dur.resize(n); in.durR.resize(n);
			// MINE (ds/inertialize-3): cap the captured velocity. A stop's source pose is mid-stride, and the decay carries the stride's speed through
			// the fade: the run's legs turn 27 deg/frame into the 100 u/s stop, where the linear crossfade peaks at 18.1 deg/frame. A channel moving
			// away from its target also has its fade cut to |offset| / |velocity|, and the quintic then swings at 2.31x that velocity mid-fade (the
			// steepest slope, 1.875 |offset| / T = 1.875 |velocity|, plus the carried 0.44 v0): at the 70 u/s stop a 101 deg offset turn cut to
			// 0.159 s swung 24.6. So the carry is held to 7 deg/frame (420 deg/s): 2.31 x 7 = 16.2, under the crossfade's 18.1 with room for the
			// target clip's own ~1 deg/frame of motion on top. Translation likewise at 60 u/s (1 u/frame, a stride's limb speed; stops measured 20-56).
			constexpr float kInertV0Rot = 420.0f / 57.29578f, kInertV0Pos = 60.0f;	// rad/s, u/s
			for (size_t b = 0; b < n; b++)
			{
				const kworld::SkelTransform& src = in.from[b];
				in.pos[b] = vsub(src.pos, local[b].pos);
				in.posV[b] = vmul(vsub(src.pos, in.prev[b].pos), 1.0f / in.dt);
				in.rot[b] = qaxis(local[b].rot, src.rot);
				in.rotV[b] = vmul(qaxis(in.prev[b].rot, src.rot), 1.0f / in.dt);
				float vp = vlen(in.posV[b]);
				if (vp > kInertV0Pos) { in.posV[b] = vmul(in.posV[b], kInertV0Pos / vp); vp = kInertV0Pos; }
				float vr = vlen(in.rotV[b]);
				if (vr > kInertV0Rot) { in.rotV[b] = vmul(in.rotV[b], kInertV0Rot / vr); vr = kInertV0Rot; }
				// the velocity-away clamp, per channel: a channel whose motion leads the offset further from the target first gets the
				// shorter fade |offset| / |velocity| (so it does not overshoot); the other keeps the crossfade's time. Separate, because a
				// limb's translation moving fast must not cut a large rotation's fade to a few frames (a snap).
				float d = in.len;
				if (vp > 1e-6f && vdot(in.pos[b], in.posV[b]) > 0.0f) { d = std::min(d, vlen(in.pos[b]) / vp); }
				float dr = in.len;
				if (vr > 1e-6f && vdot(in.rot[b], in.rotV[b]) > 0.0f) { dr = std::min(dr, vlen(in.rot[b]) / vr); }
				in.dur[b] = std::max(d, 1e-4f);
				in.durR[b] = std::max(dr, 1e-4f);
			}
		}
		else { in.len = 0; }
		in.pending = 0;
	}
	if (in.len > 0 && in.dur.size() != n) { in.len = 0; }	// a capture from a pose of another bone count: its offsets index past this one
	if (in.len > 0)
	{
		// the capture frame advances one step: at t = dt the decay's slope is still 0, so the pose moves by the source's own velocity
		// (v0, captured), which keeps the frame smooth; showing t = 0 would repeat the previous pose for a frame (a hitch)
		in.t = std::min(in.len, in.t + dt);
		for (size_t b = 0; b < n; b++)
		{
			const float up = std::clamp(in.t / in.dur[b], 0.0f, 1.0f);
			const float ur = std::clamp(in.t / in.durR[b], 0.0f, 1.0f);
			const float decayP = 1.0f - up * up * up * (up * (up * 6.0f - 15.0f) + 10.0f);	// 1 - the quintic 6u^5 - 15u^4 + 10u^3
			const float decayR = 1.0f - ur * ur * ur * (ur * (ur * 6.0f - 15.0f) + 10.0f);
			const kworld::SkelVec3 pv = vadd(in.pos[b], vmul(in.posV[b], in.t));
			const kworld::SkelVec3 rv = vmul(vadd(in.rot[b], vmul(in.rotV[b], in.t)), decayR);
			local[b].pos = vadd(local[b].pos, vmul(pv, decayP));
			local[b].rot = kworld::multiply(local[b].rot, qexp(rv));
		}
		if (in.t >= in.len) { in.len = 0; }
	}
	// the pose shown this frame (the offsets included) is the next transition's source; with the one before it gives that transition's velocity.
	// It also gives the frame's largest local joint rotation step (log feet: the peak angular velocity across a transition, measured the same on
	// and off, so the option's effect is comparable on one build).
	c.poseStep = 0;
	if (in.from.size() == n)
	{
		for (size_t b = 0; b < n; b++) { c.poseStep = std::max(c.poseStep, vlen(qaxis(in.from[b].rot, local[b].rot))); }
		c.poseStep *= 57.29578f;
	}
	in.prev = in.from;
	in.from = local;
	in.dt = dt;
}

bool Characters::lockStopped(const Char& c)
{
	// anim.action_root (fix 2): an action whose clip root draws the body (actPath: a technique, not a block) keeps its own contact windows locked
	if ((c.action >= 0 && !c.actPath) || c.spec.carriedBy >= 0 || !c.rag.empty() || c.lieLayer >= 0 || c.riseDur > 0 || c.gettingUp || c.infoless) { return true; }
	for (const Layer& l : c.layers) { if (l.info && l.info->relocates && !l.overlay && l.weight > 0) { return true; } }	// root motion: the feet are not the ground's
	return false;
}

// The body's one height offset (kenshi-k16.17.7.5, review step 5; it replaces the slope compensation applied before the pose from the last pose's
// feet, and the slice-3 pelvis drop): each foot that is down in this frame's pose votes the ground under its sole over the ground under the node
// (the clip is authored for flat ground at the node), the body takes the lowest vote, so the lower foot stands on the ground and the higher one
// is the IK's to reach (a knee bend, never a leg past its reach). A foot is down when its lower sole point is within kPlanted of the lowest sole
// of the two. Same frame: read from the pose being drawn, not the one before it. Independent of anim.foot_lock (it is the original's slope
// compensation, user 2026-11-06); without the ground callback, or on a floor (SimView's 2 u rule), no offset.
void Characters::footOffset(Char& c, const std::vector<kworld::SkelTransform>& model, float dt)
{
	constexpr float kPlanted = 0.3f;	// a sole this close to the lowest one's height (u) is down with it (MINE: a walk's swing foot is past it within a few frames)
	constexpr float kRate = 24.0f;	// the smoothing of the offset's steps when the voting foot changes, 1/s (MINE: ~0.04 s)
	float target = 0;
	if (ground && dt > 0 && std::fabs(ground(c.spec.pos[0], c.spec.pos[2]) - c.spec.pos[1]) < 2.0)
	{
		double gx[2] = {}, gz[2] = {};
		float y[2] = { 1e9f, 1e9f };
		for (int f = 0; f < 2; f++)
		{
			for (int pt = 0; pt < 2; pt++)
			{
				const int b = c.body->soleBone[f][pt];
				if (b < 0 || (size_t)b >= model.size()) { continue; }
				kworld::SkelTransform at;
				at.pos = c.body->soleAt[f][pt];
				const kworld::SkelVec3 P = kworld::combine(model[(size_t)b], at).pos;
				if (P.y >= y[f]) { continue; }
				const kworld::SkelVec3 o = kworld::rotate(c.orient, P);
				y[f] = P.y; gx[f] = c.pos[0] + o.x; gz[f] = c.pos[2] + o.z;
			}
		}
		const float low = std::min(y[0], y[1]);
		if (low < 1e8f)
		{
			const double here = ground(c.pos[0], c.pos[2]);
			double best = 1e9;
			for (int f = 0; f < 2; f++) { if (y[f] <= low + kPlanted) { best = std::min(best, ground(gx[f], gz[f]) - here); } }
			target = (float)std::clamp(best, -3.0, 3.0);
		}
	}
	c.footLift += (target - c.footLift) * std::min(1.0f, dt * kRate);
	c.pos[1] += c.footLift;
}

// Foot lock (foot IK slices 1 and 2; CLIENT_GOALS "Locomotion fidelity" > "Foot IK"): a post-process on the blended pose, after the lower layers mix,
// before the palette. While the lower layers' weighted contact windows (deriveContacts) hold a sole point down, the point stays at the world
// position where it planted; two-bone IK (thigh, calf) with the knee in the clip's own bend plane, the foot's orientation kept, so the whole
// foot translates. Release at the window's end or past ~98% of the leg's reach: the foot blends back to the animated pose over 0.15 s.
// kenshi-k16.17.7.5 (review steps 4-6): plain locomotion only. No plant in combat mode, at speed ~0, or once the body's yaw has moved more than
// 20 deg from the yaw at the plant (the foot is released as at a window's end and does not re-plant until the next window); at once on an action
// (technique, block), carry, prone, ragdoll or get-up, and while no lower layer plays. The body's height is Characters::footOffset's, one offset
// from the pose of this frame; the lock only moves feet. The pelvis drop, the stop / idle-fade pins, the lifted step, the placed free foot and
// the sole pitch fit are retired (client/retired/foot_lock_slice3.cpp). No solver library, no hand-authored data.
void Characters::lockFeet(Char& c, std::vector<kworld::SkelTransform>& local, std::vector<kworld::SkelTransform>& model, float dt)
{
	constexpr float kRelease = 0.15f;	// the blend back to the animated foot, seconds (MINE)
	constexpr float kReach = 0.98f;	// release past this share of the leg's length (CLIENT_GOALS)
	constexpr float kSlopeIn = 0.1f;	// the slope rotation's ramp-in at the stance's start, seconds (MINE)
	constexpr float kMaxSlope = 0.7f;	// the steepest foot tilt from the ground (40 deg, MINE): past it the ground is a cliff, not a stance
	constexpr float kYawRelease = 0.3491f;	// the body's yaw away from the plant's yaw that lets the foot go, rad (20 deg, review step 4)
	const kworld::Skeleton& sk = c.body->skel;
	auto clear = [](Char::FootLock& fl) { fl.active = false; fl.releasing = false; fl.shiftPrev = {}; fl.slopeW = 0; };
	const bool stopped = lockStopped(c);
	// plain locomotion: a walk or faster, not in combat mode (the knees shuffled there: the contact windows are the walk's, not a fighter's)
	const bool plain = !stopped && !c.spec.combat && (std::fabs(c.spec.move) >= 0.5f || std::fabs(c.spec.strafe) >= 0.5f);
	int thigh[2], calf[2], foot[2], toe[2];
	for (int f = 0; f < 2; f++)
	{
		thigh[f] = c.body->legBone[f][0];
		calf[f] = c.body->legBone[f][1];
		foot[f] = c.body->legBone[f][2];
		toe[f] = c.body->soleBone[f][1];	// the ball's bone (Toe0)
		if (thigh[f] < 0 || calf[f] < 0 || foot[f] < 0 || sk.bones[(size_t)thigh[f]].parent < 0 || sk.bones[(size_t)calf[f]].parent != thigh[f] || sk.bones[(size_t)foot[f]].parent != calf[f])
		{
			static std::atomic<bool> once{ false };	// the pose pass runs on several threads (anim.pose_threads)
			if (!once.exchange(true)) { fprintf(stderr, "kclient: foot lock: %s: no thigh / calf / foot chain, off\n", label(c.spec).c_str()); }
			for (Char::FootLock& fl : c.feetLock) { clear(fl); fl.event = 0; fl.animKnee = -1; fl.dPrev = -1; }
			return;
		}
		if (toe[f] >= 0 && sk.bones[(size_t)toe[f]].parent != foot[f]) { toe[f] = -1; }
	}
	// the weighted contact windows of the lower moving layers, each at its own phase, at their crossfade-would-be weights (vweight): through an
	// inertial transition the snapped weight would end the stance the fade still holds down
	float down[2][2] = {}, total = 0;	// per foot (L, R) and point (heel, ball): the weight saying it is down; the lower layers' weight
	if (!stopped)
	{
		for (const Layer& l : c.layers)
		{
			if (!l.info || l.overlay || (l.info->code != 0 && l.info != &actionInfo_) || l.info->idle || !l.clip || l.clip->length <= 0 || l.vweight <= 0) { continue; }
			const float phase = l.time / l.clip->length;
			total += l.vweight;
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					for (const SoleContact& sc : l.contacts.window[f])
					{
						if (sc.point != w) { continue; }
						if (sc.end <= 1.0f ? phase >= sc.start && phase < sc.end : phase >= sc.start || phase < sc.end - 1.0f) { down[f][w] += l.vweight; break; }
					}
				}
			}
		}
	}
	const kworld::SkelQuat unorient = { c.orient.w, -c.orient.x, -c.orient.y, -c.orient.z };
	const float yaw = 2.0f * std::atan2(c.orient.y, c.orient.w);	// the node's pure yaw rotation
	auto modelAt = [&](const double world[3])
	{
		return kworld::rotate(unorient, { (float)(world[0] - c.pos[0]), (float)(world[1] - c.pos[1]), (float)(world[2] - c.pos[2]) });
	};
	// a model transform placed from its parent the pose() way (only rotations are solved, so the scales stand)
	auto place = [&](int b)
	{
		const int p = sk.bones[(size_t)b].parent;
		const kworld::SkelVec3& ps = c.posSize[(size_t)b];
		const kworld::SkelTransform& pt = model[(size_t)p];
		const kworld::SkelTransform& l = local[(size_t)b];
		const kworld::SkelVec3 o = kworld::rotate(pt.rot, { l.pos.x * ps.x * pt.scale.y, l.pos.y * ps.y * pt.scale.y, l.pos.z * ps.z * pt.scale.y });
		model[(size_t)b].pos = { pt.pos.x + o.x, pt.pos.y + o.y, pt.pos.z + o.z };
		model[(size_t)b].rot = kworld::multiply(pt.rot, l.rot);
	};
	// foot IK slice 2: the drawn ground under a world x/z and its slope normal, from the ground callback (the terrain's triangles). Over 2 u
	// above it the body stands on a floor (SimView's rule, simview.cpp sync): a flat plane at the body's own y, the height the gravity settle
	// fits the soles to. Without the option or the callback the slice-1 lock stands (the plant stays where the animation put it).
	const bool onTerrain = footTerrain_ && ground && std::fabs(ground(c.spec.pos[0], c.spec.pos[2]) - c.spec.pos[1]) < 2.0;
	auto groundAt = [&](double x, double z, kworld::SkelVec3* n) -> double
	{
		if (n) { *n = { 0, 1, 0 }; }
		if (!onTerrain) { return c.spec.pos[1]; }
		const double y = ground(x, z);
		if (n)
		{
			constexpr double e = 2.0;	// MINE: the normal's sample step, u
			const double dx = (ground(x + e, z) - ground(x - e, z)) / (2 * e), dz = (ground(x, z + e) - ground(x, z - e)) / (2 * e);
			const float len = (float)std::sqrt(dx * dx + dz * dz + 1.0);
			*n = { (float)(-dx / len), 1.0f / len, (float)(-dz / len) };
		}
		return y;
	};
	// The slice-2 slope rotation a stance foot gets about its ankle this frame (identity when none)
	auto slopeQuat = [&](int a, float w, bool* any) -> kworld::SkelQuat
	{
		*any = false;
		if (w <= 0.001f || !footTerrain_ || !ground) { return { 1, 0, 0, 0 }; }
		const kworld::SkelVec3 aw = kworld::rotate(c.orient, model[(size_t)a].pos);
		kworld::SkelVec3 nrm;
		groundAt(c.pos[0] + aw.x, c.pos[2] + aw.z, &nrm);
		const kworld::SkelVec3 nm = kworld::rotate(unorient, nrm);
		const float tilt = std::acos(std::clamp(nm.y, -1.0f, 1.0f));
		const float sw = std::min(w, tilt > kMaxSlope ? kMaxSlope / std::max(tilt, 1e-6f) : 1.0f);	// a cliff is not a stance
		const kworld::SkelQuat dq = qfromTo({ 0, 1, 0 }, nm);
		const kworld::SkelQuat q = { 1 - sw + dq.w * sw, dq.x * sw, dq.y * sw, dq.z * sw };	// nlerp(identity, dq, sw)
		const float ql = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (ql <= 1e-6f) { return { 1, 0, 0, 0 }; }
		*any = true;
		return { q.w / ql, q.x / ql, q.y / ql, q.z / ql };
	};
	for (int f = 0; f < 2; f++)
	{
		Char::FootLock& fl = c.feetLock[f];
		fl.event = 0;
		fl.animKnee = -1;
		if (stopped) { clear(fl); fl.blocked = false; fl.dPrev = -1; continue; }	// at once, no blend: the action / carry / ragdoll pose owns the feet
		const int w = down[f][1] > down[f][0] ? 1 : 0;
		const bool hold = plain && total > 0 && down[f][w] > 0.5f * total;
		// the settled idle: kenshi-k16.17.7.2 (review cause B): only on terrain whose two feet stand at different heights (> 0.3 u to start, > 0.15 u to
		// stay, the stance's weight fading in by slopeW); on a floor or flat ground the animation's feet are right and the idle fit does nothing. It
		// is the ground fit (both soles on the drawn ground), not a plant: the foot's x/z follow the idle's own sway
		const bool slopeDelta = onTerrain && std::fabs(ground(c.pos[0] + c.feet[0][0], c.pos[2] + c.feet[0][2]) - ground(c.pos[0] + c.feet[1][0], c.pos[2] + c.feet[1][2])) > (fl.slopeW > 0.01f ? 0.15 : 0.3);
		const bool idleStance = footTerrain_ && onTerrain && slopeDelta && total <= 0 && c.soleFit && c.idleFor >= 0.5f;
		const int h = thigh[f], k = calf[f], a = foot[f];
		auto soleY = [&](int pt) { const int b = c.body->soleBone[f][pt]; if (b < 0) { return 1e9f; } kworld::SkelTransform at; at.pos = c.body->soleAt[f][pt]; return kworld::combine(model[(size_t)b], at).pos.y; };
		int pi;
		if (!idleStance) { fl.idlePi = -1; }
		if (idleStance && fl.idlePi >= 0) { pi = fl.idlePi; }	// per stance, not per frame (kenshi-k16.17.7.4)
		else if (idleStance) { pi = soleY(1) < soleY(0) ? 1 : 0; fl.idlePi = pi; }	// the idle's point is the lower of the two (the settle's rule), chosen before the slope rotation moves them
		else { pi = fl.active ? fl.point : w; }	// the planted point stays the one the lock took (heel and ball are far apart)
		const int sb = c.body->soleBone[f][pi];
		if (sb < 0) { clear(fl); fl.dPrev = -1; continue; }
		// slice 2: in stance the foot is pitched and rolled to the drawn ground's slope about the ankle (the clip's own pitch and roll kept),
		// ramped in at the stance's start and blended back with the release below; the toe follows
		const bool stance = footTerrain_ && ground && (hold || idleStance);
		fl.slopeW = std::clamp(fl.slopeW + (stance ? dt / kSlopeIn : -dt / kRelease), 0.0f, 1.0f);
		bool sloped = false;
		const kworld::SkelQuat sq = slopeQuat(a, fl.slopeW, &sloped);
		if (sloped)
		{
			model[(size_t)a].rot = kworld::multiply(sq, model[(size_t)a].rot);
			local[(size_t)a].rot = kworld::multiply(qconj(model[(size_t)k].rot), model[(size_t)a].rot);
			if (toe[f] >= 0) { place(toe[f]); }
		}
		const kworld::SkelVec3 H = model[(size_t)h].pos, K = model[(size_t)k].pos, A = model[(size_t)a].pos;
		const kworld::SkelVec3 kh = vsub(K, H), ka = vsub(A, K);	// the animated thigh and calf directions, the IK's start
		const float l1 = vlen(kh), l2 = vlen(ka);
		if (l1 <= 0 || l2 <= 0) { clear(fl); fl.knee = -1; fl.dPrev = -1; continue; }
		fl.animKnee = std::acos(std::clamp(vdot(kh, ka) / (l1 * l2), -1.0f, 1.0f)) * 57.29578f;	// before the lock: what the blend alone did
		kworld::SkelTransform at;
		at.pos = c.body->soleAt[f][pi];
		const kworld::SkelVec3 P = kworld::combine(model[(size_t)sb], at).pos;	// the animated sole point, model space
		const kworld::SkelVec3 wp = kworld::rotate(c.orient, P);	// in world units from the body
		// the body has turned away from the plant: let go as at a window's end (blocked: no re-plant until that window is over)
		if (fl.active && !idleStance)
		{
			float dy = yaw - fl.yaw0;
			dy -= 6.2831853f * std::round(dy / 6.2831853f);
			if (std::fabs(dy) > kYawRelease) { fl.active = false; fl.blocked = true; fl.releasing = true; fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f); }
		}
		// the foot translation wanted this frame (== the sole's correction)
		kworld::SkelVec3 shift = {};
		if (!hold && !idleStance) { fl.blocked = false; }	// a new window may plant again
		if (idleStance)
		{
			// slice 2: the idle's foot set on the drawn ground under its sole; both feet fit with a bend, never past the reach (the body's height
			// is footOffset's). The foot's x/z follow the idle's own sway; at keeps them for a window that opens next.
			fl.event = fl.active ? 2 : 1;
			fl.active = true;
			fl.point = pi;
			fl.releasing = false;
			fl.blocked = false;
			fl.yaw0 = yaw;
			const double gy = groundAt(c.pos[0] + wp.x, c.pos[2] + wp.z, nullptr);
			fl.at[0] = c.pos[0] + wp.x; fl.at[1] = gy; fl.at[2] = c.pos[2] + wp.z;
			shift.y = (float)(gy - (c.pos[1] + wp.y));
		}
		else if (hold && !fl.active && !fl.blocked)
		{
			// the window's start: the plant is at the foot's current position (with whatever correction the last frame applied), so a window
			// that opens in the middle of a release does not jump; slice 2: set on the drawn ground under it
			fl.active = true;
			fl.point = w;
			fl.yaw0 = yaw;
			const kworld::SkelVec3 wq = kworld::rotate(c.orient, vadd(P, fl.shiftPrev));
			fl.at[0] = c.pos[0] + wq.x; fl.at[2] = c.pos[2] + wq.z;
			fl.at[1] = footTerrain_ && ground ? groundAt(fl.at[0], fl.at[2], nullptr) : c.pos[1] + wq.y;
			fl.releasing = false;
			fl.event = 1;
			shift = vsub(modelAt(fl.at), P);
		}
		else if (hold && fl.active) { shift = vsub(modelAt(fl.at), P); fl.event = 2; }
		else if (fl.active)
		{
			// the window ended (or the body slowed to rest, or a fight began): blend back from where the foot is
			fl.active = false;
			fl.releasing = true;
			fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
			shift = fl.shiftPrev;
			fl.event = 3;
		}
		else if (fl.releasing)
		{
			// back to the animation at the release's own rate (its length over kRelease seconds); the applied shift is the state, so a frame the
			// knee rate limit holds back only makes the return longer, never a jump
			const float sp = vlen(fl.shiftPrev);
			const float keep = sp > 1e-4f ? std::max(sp - fl.releaseLen * dt / kRelease, 0.0f) / sp : 0.0f;
			shift = vmul(fl.shiftPrev, keep);
			fl.event = fl.blocked ? 4 : 3;	// past the reach or the yaw: the blocked release, else the window-end blend
			if (vlen(shift) <= 1e-4f) { fl.releasing = false; }
		}
		const float sl = vlen(shift);
		if (sl <= 1e-4f)
		{
			fl.shiftPrev = {};
			fl.dPrev = vlen(vsub(A, H));
			continue;
		}
		// the ankle target: the foot moves rigidly with the sole point
		kworld::SkelVec3 A2 = vadd(A, shift);
		kworld::SkelVec3 v = vsub(A2, H);
		float d = vlen(v);
		if (d < 1e-4f) { fl.dPrev = d; continue; }
		const float dmax = kReach * (l1 + l2), dmin = std::fabs(l1 - l2) * 1.02f + 1e-3f;
		if (d > dmax && hold && fl.active)
		{
			// the lock needs the leg past its reach: release from the last reachable foot pose (clamping to the extended leg would snap the knee),
			// and stay released until the window ends
			fl.active = false;
			fl.blocked = true;
			fl.event = 4;
			fl.releasing = true;
			fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
			shift = fl.shiftPrev;
			A2 = vadd(A, shift);
			v = vsub(A2, H);
			d = vlen(v);
		}
		if (d > dmax || d < dmin)
		{
			const float d2 = std::clamp(d, dmin, dmax);
			A2 = vadd(H, vmul(v, d2 / d));
			shift = vsub(A2, A);
			d = d2;
			v = vsub(A2, H);
		}
		// The knee angle follows from the ankle distance alone (law of cosines), so the shown distance may only move so fast from the last
		// frame's: the desired direction stands, the distance is held inside that step, and the foot slides along the leg instead of the knee
		// snapping. (A release still continues from the applied shift, so holding it back only makes the return longer, never a jump.)
		if (dt > 0 && fl.dPrev > 1e-3f)
		{
			auto kneeDeg = [&](float dd) { return std::acos(std::clamp((dd * dd - l1 * l1 - l2 * l2) / (2 * l1 * l2), -1.0f, 1.0f)) * 57.29578f; };
			auto distAt = [&](float th) { th = std::clamp(th, 0.0f, 180.0f); return std::sqrt(std::max(0.0f, l1 * l1 + l2 * l2 + 2 * l1 * l2 * std::cos(th * 0.01745329f))); };
			const float lim = kKneeRate * dt, thPrev = kneeDeg(fl.dPrev);
			if (std::fabs(kneeDeg(d) - thPrev) > lim)
			{
				// the allowed distances are the angle window's, and at a nearly straight (or folded) knee the two bounds come back swapped: the
				// wider angle clamps at 180 (or 0) and sits on the larger distance, so ordering them is what keeps the limit there - the old
				// dhi > dlo test skipped the clamp entirely at a straight leg and the knee popped on the release (ds/run-stop-2)
				const float dlo = std::max(dmin, distAt(thPrev + lim)), dhi = std::min(dmax, distAt(thPrev - lim));
				const float lo = std::min(dlo, dhi), hi = std::max(dlo, dhi);
				if (hi > lo)
				{
					d = std::clamp(d, lo, hi);
					A2 = vadd(H, vmul(v, d / vlen(v)));
					shift = vsub(A2, A);
					v = vsub(A2, H);
				}
			}
		}
		fl.dPrev = d;
		if (vlen(shift) <= 1e-4f) { fl.shiftPrev = {}; fl.dPrev = vlen(vsub(A, H)); continue; }	// the rate limit took the whole correction: the animated pose stands
		const kworld::SkelVec3 u = vmul(v, 1.0f / d);
		// kenshi-k16.17.7.4: the bend plane is the animated knee's own (the clip's pose), so a small shift moves the knee along its own path and a zero
		// shift (handled above) leaves the pose as it was; the toe's direction only stands in when the leg is nearly straight and the knee has no side
		kworld::SkelVec3 side = vsub(kh, vmul(u, vdot(kh, u)));
		float sw = vlen(side);
		if (sw < 0.05f * l1 && toe[f] >= 0) { const kworld::SkelVec3 fw = vsub(model[(size_t)toe[f]].pos, A); side = vsub(fw, vmul(u, vdot(fw, u))); sw = vlen(side); }
		if (sw < 1e-3f) { side = vcross(u, std::fabs(u.y) < 0.9f ? kworld::SkelVec3{ 0, 1, 0 } : kworld::SkelVec3{ 1, 0, 0 }); sw = vlen(side); }
		if (sw > 1e-6f)
		{
			// the knee stays in the clip's own bend plane (side)
			side = vmul(side, 1.0f / sw);
			const float cosH = std::clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.0f, 1.0f);
			const float sinH = std::sqrt(std::max(0.0f, 1.0f - cosH * cosH));
			const kworld::SkelVec3 K2 = vadd(vadd(H, vmul(u, l1 * cosH)), vmul(side, l1 * sinH));
			const kworld::SkelQuat q1 = qfromTo(vmul(kh, 1.0f / l1), vmul(vsub(K2, H), 1.0f / l1));
			const kworld::SkelQuat q2 = qfromTo(vmul(ka, 1.0f / l2), vmul(vsub(A2, K2), 1.0f / l2));
			const kworld::SkelQuat Rp = sk.bones[(size_t)h].parent >= 0 ? model[(size_t)sk.bones[(size_t)h].parent].rot : kworld::SkelQuat{};
			const kworld::SkelQuat Rt = model[(size_t)h].rot, Rc = model[(size_t)a].rot;
			local[(size_t)h].rot = kworld::multiply(kworld::multiply(kworld::multiply(qconj(Rp), q1), Rp), local[(size_t)h].rot);
			model[(size_t)h].rot = kworld::multiply(Rp, local[(size_t)h].rot);
			local[(size_t)k].rot = kworld::multiply(kworld::multiply(kworld::multiply(qconj(model[(size_t)h].rot), q2), Rt), local[(size_t)k].rot);
			place(k);
			local[(size_t)a].rot = kworld::multiply(qconj(model[(size_t)k].rot), Rc);	// the foot keeps its orientation
			place(a);
			if (toe[f] >= 0) { place(toe[f]); }
			fl.shiftPrev = vsub(A2, A);	// what the next frame starts from (the plant and the release both)
		}
	}
	for (Char::FootLock& fl : c.feetLock) { if (fl.active) { fl.held++; } }
}

// The foot lock's knee sanity (log feet): each knee's per-frame angle step and the steps past kKneePopDeg, reported every 600 frames, split by
// this frame's lock event (engage / held / release blend / blocked past the reach / none) and, against the same frame of the blended animation
// before the lock, the steps only the lock added. Measured with anim.foot_lock off too; a character whose knees do not move stays silent.
void Characters::kneeReport(Char& c)
{
	for (int f = 0; f < 2; f++)
	{
		Char::FootLock& fl = c.feetLock[f];
		const int th = c.body->legBone[f][0], ca = c.body->legBone[f][1], fo = c.body->legBone[f][2];
		if (lockStopped(c) || th < 0 || ca < 0 || fo < 0) { fl.knee = -1; fl.animKneePrev = -1; continue; }
		if (!footLock_) { fl.animKnee = -1; fl.animKneePrev = -1; }
		const kworld::SkelVec3 d1 = vsub(c.model[(size_t)ca].pos, c.model[(size_t)th].pos);
		const kworld::SkelVec3 d2 = vsub(c.model[(size_t)fo].pos, c.model[(size_t)ca].pos);
		const float l = vlen(d1) * vlen(d2);
		const float ang = l > 1e-6f ? std::acos(std::clamp(vdot(d1, d2) / l, -1.0f, 1.0f)) * 57.29578f : 0.0f;
		if (fl.knee >= 0)
		{
			const float step = std::fabs(ang - fl.knee);
			fl.kneeStep = std::max(fl.kneeStep, step);
			if (step > kKneePopDeg)
			{
				fl.pops++;
				const int cls = footLock_ ? fl.event : 0;
				fl.popsBy[cls]++;
				// did the blended animation pop here too, or is the lock the only reason?
				if (fl.animKnee >= 0 && fl.animKneePrev >= 0 && std::fabs(fl.animKnee - fl.animKneePrev) <= kKneePopDeg)
				{
					fl.addedPops++;
					fl.addedBy[cls]++;
				}
			}
		}
		fl.knee = ang;
		fl.animKneePrev = fl.animKnee;
	}
	Char::FootLock& l0 = c.feetLock[0];
	if (++l0.frames < 600) { return; }
	if (c.feetLock[0].kneeStep > 0 || c.feetLock[1].kneeStep > 0)
	{
		fprintf(stderr, "kclient: foot lock: %s: knee step max L %.1f R %.1f deg/frame, pops > %.0f deg L %d [engage %d held %d release %d blocked %d none %d] added %d/%d/%d/%d/%d, "
			"R %d [engage %d held %d release %d blocked %d none %d] added %d/%d/%d/%d/%d, held L %d R %d of %d frames\n",
			label(c.spec).c_str(), c.feetLock[0].kneeStep, c.feetLock[1].kneeStep, kKneePopDeg,
			c.feetLock[0].pops, c.feetLock[0].popsBy[1], c.feetLock[0].popsBy[2], c.feetLock[0].popsBy[3], c.feetLock[0].popsBy[4], c.feetLock[0].popsBy[0],
			c.feetLock[0].addedBy[1], c.feetLock[0].addedBy[2], c.feetLock[0].addedBy[3], c.feetLock[0].addedBy[4], c.feetLock[0].addedBy[0],
			c.feetLock[1].pops, c.feetLock[1].popsBy[1], c.feetLock[1].popsBy[2], c.feetLock[1].popsBy[3], c.feetLock[1].popsBy[4], c.feetLock[1].popsBy[0],
			c.feetLock[1].addedBy[1], c.feetLock[1].addedBy[2], c.feetLock[1].addedBy[3], c.feetLock[1].addedBy[4], c.feetLock[1].addedBy[0],
			c.feetLock[0].held, c.feetLock[1].held, l0.frames);
	}
	for (Char::FootLock& fl : c.feetLock)
	{
		fl.kneeStep = 0; fl.pops = 0; fl.held = 0; fl.addedPops = 0;
		for (int& n : fl.popsBy) { n = 0; }
		for (int& n : fl.addedBy) { n = 0; }
	}
	l0.frames = 0;
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

// The heel and ball points of each sole (log feet), from the bind-pose mesh: the vertices weighted >= 0.5 to the foot or toe bone; the sole =
// those within 15% of the ankle's height above the lowest; heel = the rearmost 15% of the sole along the foot (ankle -> toe joint), ball =
// the sole within 10% of the foot's length of the toe joint; both at the lowest height. MINE: the fractions.
void Characters::fitSolePoints(Body& b)
{
	const kworld::Skeleton& sk = b.skel;
	for (int f = 0; f < 2; f++)
	{
		const int foot = sk.bone(f ? "Bip01 R Foot" : "Bip01 L Foot"), toe = sk.bone(f ? "Bip01 R Toe0" : "Bip01 L Toe0");
		b.legBone[f][0] = sk.bone(f ? "Bip01 R Thigh" : "Bip01 L Thigh");
		b.legBone[f][1] = sk.bone(f ? "Bip01 R Calf" : "Bip01 L Calf");
		b.legBone[f][2] = foot;
		if (foot < 0 || toe < 0) { continue; }
		const kworld::SkelTransform fm = kworld::sampleModel(sk, nullptr, foot, 0), tm = kworld::sampleModel(sk, nullptr, toe, 0);
		double dir[2] = { tm.pos.x - fm.pos.x, tm.pos.z - fm.pos.z };
		const double len = std::hypot(dir[0], dir[1]);
		if (len <= 0) { continue; }
		dir[0] /= len;
		dir[1] /= len;
		std::vector<std::array<double, 3>> pts;	// x, z, y
		auto scan = [&](const ogre::Geometry& g)
		{
			if (g.boneIndex.empty()) { return; }
			for (uint32_t v = 0; v < g.count; v++)
			{
				for (int k = 0; k < 4; k++)
				{
					const int bi = g.boneIndex[v * 4 + k];
					if ((bi == foot || bi == toe) && g.boneWeight[v * 4 + k] >= 0.5f) { pts.push_back({ g.pos[v * 3], g.pos[v * 3 + 2], g.pos[v * 3 + 1] }); break; }
				}
			}
		};
		if (b.src.hasShared) { scan(b.src.shared); }
		for (const ogre::Submesh& sm : b.src.submeshes) { if (!sm.shared) { scan(sm.geometry); } }
		if (pts.empty()) { continue; }
		double minY = 1e30, lo = 1e30, hi = -1e30;
		for (const auto& p : pts) { minY = std::min(minY, p[2]); }
		const double band = minY + 0.15 * (fm.pos.y - minY);
		auto along = [&](const std::array<double, 3>& p) { return (p[0] - fm.pos.x) * dir[0] + (p[1] - fm.pos.z) * dir[1]; };
		for (const auto& p : pts) { if (p[2] <= band) { lo = std::min(lo, along(p)); hi = std::max(hi, along(p)); } }
		const double footLen = hi - lo;
		double sum[2][2] = {};
		int n[2] = {};
		for (const auto& p : pts)
		{
			if (p[2] > band) { continue; }
			const double a = along(p);
			const int which = a <= lo + 0.15 * footLen ? 0 : std::fabs(a - len) <= 0.1 * footLen ? 1 : -1;
			if (which < 0) { continue; }
			sum[which][0] += p[0];
			sum[which][1] += p[1];
			n[which]++;
		}
		for (int w = 0; w < 2; w++)
		{
			if (!n[w]) { continue; }
			kworld::SkelTransform at;
			at.pos = { (float)(sum[w][0] / n[w]), (float)minY, (float)(sum[w][1] / n[w]) };
			b.soleAt[f][w] = kworld::combine(kworld::inverse(w ? tm : fm), at).pos;
			b.soleBone[f][w] = w ? toe : foot;
		}
	}
}

// The sole contacts and the stride of each lower moving clip, derived at load (task L4; MINE: the sample count and contact height).
// The clip alone is posed over one cycle with this body's bone sizes (as pose() bakes them, minus the severed-limb scales), its heel and
// ball soles (Body::soleBone / soleAt) sampled: a point is in contact while within kContactHeight of its foot's own lowest sole over the
// cycle (not both feet's: a crouch walk keeps one foot up all cycle) and moving backward against the foot's heel -> ball axis. The
// stride is the ground the body covers in one cycle with those points planted (the planted figure, coordinator 2026-11-07); the older
// window sum (the windows' travel over their length, task L4b) is kept for the forward clips where the two agree within 5%. Nothing is
// hand-authored.
void Characters::deriveContacts(Char& c)
{
	for (Layer& l : c.layers)
	{
		if (!l.info || l.overlay || l.info->code != 0 || l.info->idle || !l.clip || l.clip->length <= 0) { continue; }
		contactsOf(c, l);
	}
}

// One clip's contact windows and stride over this body (deriveContacts; and the action clip at its start, anim.action_root).
void Characters::contactsOf(Char& c, Layer& l) const
{
	const kworld::Skeleton& sk = c.body->skel;
	const Body& body = *c.body;
	for (int f = 0; f < 2; f++) { for (int w = 0; w < 2; w++) { if (body.soleBone[f][w] < 0) { return; } } }
	if (!l.clip || l.clip->length <= 0) { return; }
	// The figures depend on the body (skeleton, sole points), the clip, its record and this character's bone sizes only: the same inputs give the
	// same result from the cache (a fresh layer's contacts are empty, so the cached ones are exactly what the derivation would append).
	const bool fresh = l.contacts.window[0].empty() && l.contacts.window[1].empty();
	std::string key;
	if (fresh)
	{
		const void* const ptrs[3] = { &body, l.clip, l.info };
		key.append((const char*)ptrs, sizeof ptrs);
		key.append((const char*)c.size.data(), c.size.size() * sizeof c.size[0]);
		key.append((const char*)c.posSize.data(), c.posSize.size() * sizeof c.posSize[0]);
		if (const auto it = contactCache_.find(key); it != contactCache_.end()) { l.contacts = it->second; return; }
	}
	CPUSTAGE_ACC("char.contacts.derive");	// a cache miss
	struct Store
	{
		std::unordered_map<std::string, Contacts>& cache;
		std::string& key;
		const Layer& l;
		bool on;
		~Store()
		{
			if (!on) { return; }
			if (cache.size() >= 16384) { cache.clear(); }	// bounded: ~40 clips a body and slider set
			cache.emplace(std::move(key), l.contacts);
		}
	} store{ contactCache_, key, l, fresh };
	std::vector<kworld::SkelTransform> model(sk.bones.size());
	// Only the sole bones are read, so only they and their ancestors are posed (in parentFirst order: the same sums, the same figures). The whole
	// skeleton 96 times per moving clip was 2.2 ms an add, 80% of char.add in a crowd burst (kenshi-k16.2.205.12.76).
	std::vector<char> need(sk.bones.size(), 0);
	for (int f = 0; f < 2; f++) { for (int w = 0; w < 2; w++) { for (int b = body.soleBone[f][w]; b >= 0 && !need[(size_t)b]; b = sk.bones[(size_t)b].parent) { need[(size_t)b] = 1; } } }
	std::vector<int> chain;
	for (const int b : sk.parentFirst) { if (need[(size_t)b]) { chain.push_back(b); } }
	// the clip at time t, the bones scaled by this body's sizes as pose() bakes them (the clip alone at weight 1)
	auto poseAt = [&](const kworld::SkeletonAnimation& clip, float t)
	{
		for (const int b : chain)
		{
			kworld::SkelTransform& m = model[(size_t)b];
			const kworld::SkelTransform local = kworld::sampleLocal(sk, &clip, b, t);
			const kworld::SkelVec3& size = c.size[(size_t)b];
			m.scale = { local.scale.x * size.x, local.scale.y * size.y, local.scale.z * size.z };
			const int p = sk.bones[(size_t)b].parent;
			if (p < 0) { m.pos = local.pos; m.rot = local.rot; continue; }
			const kworld::SkelVec3& ps = c.posSize[(size_t)b];
			const float sy = model[(size_t)p].scale.y;
			const kworld::SkelVec3 o = kworld::rotate(model[(size_t)p].rot, { local.pos.x * ps.x * sy, local.pos.y * ps.y * sy, local.pos.z * ps.z * sy });
			m.pos = { model[(size_t)p].pos.x + o.x, model[(size_t)p].pos.y + o.y, model[(size_t)p].pos.z + o.z };
			m.rot = kworld::multiply(model[(size_t)p].rot, local.rot);
		}
	};
	{
		const kworld::SkeletonAnimation& clip = *l.clip;
		double pos[kContactSamples][2][2][3];
		bool planted[kContactSamples][2][2];
		float lowest[2] = { 1e30f, 1e30f };
		for (int k = 0; k < kContactSamples; k++)
		{
			poseAt(clip, clip.length * (float)k / kContactSamples);
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					kworld::SkelTransform at;
					at.pos = body.soleAt[f][w];
					const kworld::SkelVec3 q = kworld::combine(model[(size_t)body.soleBone[f][w]], at).pos;
					pos[k][f][w][0] = q.x;
					pos[k][f][w][1] = q.y;
					pos[k][f][w][2] = q.z;
					lowest[f] = std::min(lowest[f], q.y);
				}
			}
		}
		// moving backward: the point's horizontal speed at sample k against the foot's heel -> ball axis
		auto backward = [&](int k, int f, int w)
		{
			const double* p0 = pos[(k + kContactSamples - 1) % kContactSamples][f][w];
			const double* p1 = pos[(k + 1) % kContactSamples][f][w];
			const double fx = pos[k][f][1][0] - pos[k][f][0][0], fz = pos[k][f][1][2] - pos[k][f][0][2];
			const double len = std::hypot(fx, fz);
			return len > 1e-6 && ((p1[0] - p0[0]) * fx + (p1[2] - p0[2]) * fz) / len < 0;
		};
		float travel = 0, span = 0;
		for (int f = 0; f < 2; f++)
		{
			for (int w = 0; w < 2; w++)
			{
				for (int k = 0; k < kContactSamples; k++) { planted[k][f][w] = pos[k][f][w][1] - lowest[f] <= kContactHeight && backward(k, f, w); }
				// windows: the runs of contact samples, walked from a sample off the ground so one over the cycle boundary stays whole
				int start = 0;
				while (start < kContactSamples && planted[start][f][w]) { start++; }
				for (int i = 0; i < kContactSamples; )
				{
					if (!planted[(start + i) % kContactSamples][f][w]) { i++; continue; }
					int n = 0;
					while (n < kContactSamples && planted[(start + i + n) % kContactSamples][f][w]) { n++; }
					const int a = (start + i) % kContactSamples, b = (start + i + n - 1) % kContactSamples;
					const double fx = pos[a][f][1][0] - pos[a][f][0][0], fz = pos[a][f][1][2] - pos[a][f][0][2];
					const double len = std::hypot(fx, fz);
					const float win = (float)n / kContactSamples;
					const float gone = len > 1e-6 ? (float)(-((pos[b][f][w][0] - pos[a][f][w][0]) * fx + (pos[b][f][w][2] - pos[a][f][w][2]) * fz) / len) : 0.0f;
					if (gone > 0)
					{
						SoleContact sc;
						sc.start = (float)a / kContactSamples;
						sc.end = sc.start + win;
						sc.point = w;
						l.contacts.window[f].push_back(sc);
						// the stride counts the feet that reach the clip's ground only: a foot held above it all cycle (the crouch
						// walk's left, 0.56 u up) keeps its windows, but its slow hover would drag the mean (task L4b)
						if (lowest[f] - std::min(lowest[0], lowest[1]) <= kContactHeight) { travel += gone; span += win; }
					}
					i += n;
				}
			}
		}
		l.contacts.windows = span > 0 ? travel / span : 0.0f;
		// The planted figure (the animation lab's measure, coordinator 2026-11-07): with the down points held still the body must move
		// opposite to them; the mean of the planted points' velocities, carried over the whole cycle, is the ground per cycle (a coherent
		// clip pins both feet to one motion). Feet that never reach the clip's ground are left out, as in the window sum (task L4b).
		const float ground = std::min(lowest[0], lowest[1]);
		const bool reaches[2] = { lowest[0] - ground <= kContactHeight, lowest[1] - ground <= kContactHeight };
		const double dt = (double)clip.length / kContactSamples;
		double body[2] = {}, held = 0;	// model-space displacement while the feet are down (x = the character's right, z = forward)
		for (int k = 0; k < kContactSamples; k++)
		{
			double vx = 0, vz = 0; int n = 0;
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					if (!planted[k][f][w] || !reaches[f]) { continue; }
					const double* p0 = pos[(k + kContactSamples - 1) % kContactSamples][f][w];
					const double* p1 = pos[(k + 1) % kContactSamples][f][w];
					vx += (p1[0] - p0[0]) / (2 * dt);
					vz += (p1[2] - p0[2]) / (2 * dt);
					n++;
				}
			}
			if (n) { body[0] -= vx / n * dt; body[1] -= vz / n * dt; held += dt; }
		}
		const float plantedStride = held > 0 ? (float)(std::hypot(body[0], body[1]) / held * clip.length) : 0.0f;
		// Which figure the stride warp uses: the planted one for the strafe (timed by lateral speed) and the stepback records (backward),
		// and for a forward clip only where the window sum is off by more than 5%; that switch is reported (once per clip name at load: the
		// numbers are this body's, `dump contacts` prints both). A clip with no planted figure keeps the window sum.
		const bool lateral = l.info->move == 0;	// strafe lower: the only lower moving record with no speed band
		const bool backwardClip = !lateral && l.info->play < 0;	// stepback / stepback short
		const bool differs = plantedStride > 0 && l.contacts.windows > 0 && std::fabs(plantedStride - l.contacts.windows) > 0.05f * l.contacts.windows;
		l.contacts.stride = plantedStride <= 0 ? l.contacts.windows : lateral || backwardClip || differs ? plantedStride : l.contacts.windows;
		if (differs && !l.info->name.empty())	// a technique clip has none: its stride is never read (no stride warp on an action)
		{
			static std::vector<std::string> reported;
			if (std::find(reported.begin(), reported.end(), l.info->name) == reported.end())
			{
				reported.push_back(l.info->name);
				fprintf(stderr, "kclient: contacts: %s%s: planted stride %.2f u/cycle vs windows %.2f (%+.0f%%), the planted figure is used\n",
					l.info->name.c_str(), lateral ? " (lateral)" : backwardClip ? " (backward)" : "", plantedStride, l.contacts.windows,
					100.0 * (plantedStride - l.contacts.windows) / l.contacts.windows);
			}
		}
	}
}

bool Characters::solePoints(size_t i, double out[2][2][3]) const
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	const Char& c = chars_[i];
	for (int f = 0; f < 2; f++)
	{
		for (int w = 0; w < 2; w++)
		{
			const int b = c.body->soleBone[f][w];
			if (b < 0 || (size_t)b >= c.model.size()) { return false; }
			kworld::SkelTransform at;
			at.pos = c.body->soleAt[f][w];
			const kworld::SkelVec3 o = kworld::rotate(c.orient, kworld::combine(c.model[(size_t)b], at).pos);
			out[f][w][0] = c.pos[0] + o.x;
			out[f][w][1] = c.pos[1] + o.y;
			out[f][w][2] = c.pos[2] + o.z;
		}
	}
	return true;
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

// the slots every lit program shares (lighting.inc, shadow.inc, the fog map)
void Characters::bindLighting(sg_bindings& bind) const
{
	bind.views[VIEW_shadow_tex] = shadow->view();
	bind.views[VIEW_fog_map] = fog_map_binding().view;
	bind.views[VIEW_spec_cube] = spec_cube_binding().view;
	bind.views[VIEW_irr_cube] = irr_cube_binding().view;
	bind.samplers[SMP_shadow_smp] = shadow->sampler();
	bind.samplers[SMP_shadow_raw] = shadow->rawSampler();
	bind.samplers[SMP_fog_smp] = fog_map_binding().smp;
	bind.samplers[SMP_spec_smp] = spec_cube_binding().smp;
	bind.views[VIEW_ao_tex] = ao_binding().view;
	bind.views[VIEW_vol_sh] = lightvol::binding().sh;
	bind.views[VIEW_vol_use] = lightvol::binding().use;
	bind.views[VIEW_vol_lamp] = lightvol::binding().lamp;
	bind.views[VIEW_vol_sun0] = lightvol::binding().sun[0];	// render.sun_bounce
	bind.views[VIEW_vol_sun1] = lightvol::binding().sun[1];
	bind.views[VIEW_vol_sun2] = lightvol::binding().sun[2];
	bind.samplers[SMP_vol_smp] = lightvol::binding().smp;
	bind.views[VIEW_il_tex] = ao_binding().il;
	bind.samplers[SMP_ao_smp] = ao_binding().smp;
}

// ---- the instanced character draw (kenshi-k16.2.205.8) ----
// The old path uploaded a 4 KB vertex block per character per pass and drew every part on its own (~2600 shadow draws + ~700 main draws at Heft, 231
// characters): the draw count was the cost (~0.8 us each on the main thread). Now one frame holds every visible character's palette in one storage buffer,
// the parts are grouped by (mesh, material) per pass and each group is one instanced draw. The old path is in retired/characters_per_char_draw.txt.

bool Characters::ViewKey::operator==(const ViewKey& o) const
{
	if (mesh != o.mesh || mesh2 != o.mesh2) { return false; }
	for (int i = 0; i < 12; i++) { if (v[i].id != o.v[i].id) { return false; } }
	return true;
}

size_t Characters::ViewKeyHash::operator()(const ViewKey& k) const
{
	uint64_t h = (uint64_t)(uintptr_t)k.mesh * 0x9e3779b97f4a7c15ull;
	h = (h ^ (uint64_t)(uintptr_t)k.mesh2) * 0x100000001b3ull;
	for (int i = 0; i < 12; i++) { h = (h ^ k.v[i].id) * 0x100000001b3ull; h ^= h >> 29; }
	return (size_t)h;
}

uint32_t Characters::Plan::draws() const
{
	size_t n = body.size() + armour.size() + hair.size() + items.size() + standin.size();
	for (const auto& s : shadow) { n += s.size(); }
	return (uint32_t)n;
}

void Characters::makeSet(GpuSet& s, int skins, int insts, int list, const char* label)
{
	s.skinCap = skins;
	s.instCap = insts;
	s.listCap = list;
	sg_buffer_desc bd = {};
	bd.usage.vertex_buffer = false;
	bd.usage.storage_buffer = true;
	bd.usage.write_transient = true;
	bd.size = (size_t)skins * kSkinRows * 16;
	bd.label = label;
	s.skins = sg_make_buffer(&bd);
	bd.size = (size_t)insts * kInstRows * 16;
	s.insts = sg_make_buffer(&bd);
	bd.usage.storage_buffer = false;
	bd.usage.vertex_buffer = true;
	bd.size = (size_t)list * 8;
	s.list = sg_make_buffer(&bd);
	sg_view_desc vd = {};
	vd.storage_buffer.buffer = s.skins;
	s.skinsView = sg_make_view(&vd);
	vd.storage_buffer.buffer = s.insts;
	s.instsView = sg_make_view(&vd);
}

void Characters::freeSet(GpuSet& s)
{
	sg_destroy_view(s.skinsView);
	sg_destroy_view(s.instsView);
	sg_destroy_buffer(s.skins);
	sg_destroy_buffer(s.insts);
	sg_destroy_buffer(s.list);
	s = GpuSet{};
}

// Gathers the characters' skins, the per-part records and the instance lists of every pass into `s` (CPU side; uploadSet writes them).
const char* Characters::drawStateName(DrawState s)
{
	switch (s)
	{
	case kBodyDrawn: return "drawn";
	case kNoModel: return "no model built";
	case kFloorCut: return "a floor cut hides it";
	case kBuffersFull: return "the draw buffers ran out";
	case kStandin: return "a loading stand-in";	// its GPU objects are queued: drawn as its race's body (render.loading_standin)
	default: return "not in the frame's set";
	}
}

void Characters::buildSet(FrameData& s, const std::vector<Char>& all, bool cull, const double cam[3], const ShadowCascade* const cascades[kMaxCascades],
	int skinCap, int instCap, int listCap)
{
	s.clear();
	// what each slot draws (shownOf): a character whose GPU objects are still in the stream lane is left out, or stands in by the build it replaced
	const size_t n = all.size();
	std::vector<const Char*> shown(n);
	for (size_t i = 0; i < n; i++) { shown[i] = shownOf(all[i]); }
	auto list = [&](size_t i) -> const Char& { return *shown[i]; };
	if (cull) { drawState_.assign(n, kNotInSet); }	// the world frame's set: the portrait passes do not touch the record
	std::vector<int32_t> skin(n, -1), bodyRec(n, -1);
	std::vector<size_t> pbase(n + 1, 0);
	for (size_t i = 0; i < n; i++) { pbase[i + 1] = pbase[i] + (shown[i] ? shown[i]->parts.size() : 0); }
	std::vector<int32_t> partRec(pbase[n], -1);
	static bool warned = false;
	auto full = [&](const char* what)
	{
		if (!warned) { warned = true; fprintf(stderr, "kclient: characters: the %s buffer is full: the rest of the frame's characters are not drawn\n", what); }
	};

	// the character's skin slot (its first row), -1 when the buffer is full
	auto skinOf = [&](size_t i, const Char& c) -> int64_t
	{
		if (skin[i] >= 0) { return (int64_t)skin[i] * kSkinRows; }
		if ((int)(s.skins.size() / (kSkinRows * 4)) >= skinCap) { full("skin"); return -1; }
		const size_t at = s.skins.size();
		s.skins.resize(at + (size_t)kSkinRows * 4, 0.0f);
		float* d = &s.skins[at];
		static_assert(kMaxBones * 12 == 180 * 4, "180 bone rows");
		memcpy(d, c.palette.data(), (size_t)kMaxBones * 12 * sizeof(float));
		for (int k = 0; k < 3; k++) { d[180 * 4 + k] = (float)(c.pos[k] - cam[k]); }
		memcpy(d + 181 * 4, c.blood, sizeof c.blood);
		d[183 * 4] = c.body->bloodScale[0];
		d[183 * 4 + 1] = c.body->bloodScale[1];
		d[183 * 4 + 2] = (float)c.hidden;
		memcpy(d + 184 * 4, c.body->bonePart.data(), std::min((size_t)15 * 4, c.body->bonePart.size()) * sizeof(float));
		// row 199: the loading stand-in's clocks (kenshi-k16.2.258.1): x = the body's shape t, y = the real look's dither coverage, z = the stand-in
		// BODY's coverage, w = the stand-in PARTS' (the head / hair / armour dissolve in during stage 1; the body was already there, so it stays
		// whole). A dither coverage is a keep threshold: the real look keeps `ign < y`, the stand-in keeps `ign >= z / w`, so in stage 2 the two
		// are exact complements on the same pixels. A portrait set (cull false) is drawn whole: its character is drawable by definition and the UI
		// portrait must not come out half dithered.
		float shape = 0.0f, coverIn = 1.0f, coverBody = 1.0f, coverParts = 1.0f;
		if (cull)
		{
			switch (standinStage(c))
			{
			case 1: coverIn = 0.0f; coverBody = coverParts = 0.0f; break;	// standing in: the stand-in whole, the real look not yet
			case 2:	// stage 1: the shape morphs into this build's, still in the stand-in's material; a part cannot morph, it dissolves in
				shape = c.fadeAge / kStandinShape;
				coverIn = 0.0f;
				coverBody = 0.0f;
				coverParts = 1.0f - shape;
				break;
			case 3:	// stage 2: the real look dithers in where the stand-in dithers out, over the same pixels
				shape = 1.0f;
				coverIn = (c.fadeAge - kStandinShape) / kStandinColour;
				coverBody = coverParts = coverIn;
				break;
			default: break;	// the real look, whole
			}
		}
		d[199 * 4] = shape;
		d[199 * 4 + 1] = coverIn;
		d[199 * 4 + 2] = coverBody;
		d[199 * 4 + 3] = coverParts;
		skin[i] = (int32_t)(at / ((size_t)kSkinRows * 4));
		return (int64_t)skin[i] * kSkinRows;
	};
	// a record: kInstRows rows; -1 when the buffer is full
	auto newRecord = [&](float*& rec) -> int32_t
	{
		if ((int)(s.insts.size() / (kInstRows * 4)) >= instCap) { full("record"); return -1; }
		const size_t at = s.insts.size();
		s.insts.resize(at + (size_t)kInstRows * 4, 0.0f);
		rec = &s.insts[at];
		return (int32_t)(at / ((size_t)kInstRows * 4));
	};
	auto bodyRecord = [&](size_t i, const Char& c) -> int32_t
	{
		if (bodyRec[i] >= 0) { return bodyRec[i]; }
		float* r = nullptr;
		const int32_t id = newRecord(r);
		if (id < 0) { return -1; }
		for (int k = 0; k < 3; k++) { r[k] = c.skintone[k]; }
		r[3] = c.muscleBlend;
		memcpy(r + 4, c.hairCol, 16);
		memcpy(r + 8, c.hairMult, 16);
		memcpy(r + 12, c.hairAlpha, 16);
		memcpy(r + 16, c.vestCol, 16);
		memcpy(r + 20, c.beardAlpha, 16);
		memcpy(r + 24, c.body->bloodCol, 16);
		// rows 7 and 8 (clip channels, minimum): zero, a body casts whole
		return bodyRec[i] = id;
	};
	auto partRecord = [&](size_t i, const Char& c, size_t pi) -> int32_t
	{
		int32_t& slot = partRec[pbase[i] + pi];
		if (slot >= 0) { return slot; }
		float* r = nullptr;
		const int32_t id = newRecord(r);
		if (id < 0) { return -1; }
		const Part& p = c.parts[pi];
		if (p.kind == kArmour)
		{
			// COLOR_DATA colours (display-referred): raw; the shader's .a switch stays 0
			for (int k = 0; k < 3; k++) { r[k] = p.col1[k]; r[4 + k] = p.col2[k]; }
			r[8] = p.spec;
			r[11] = p.clipMin;
		}
		else
		{
			// the hair ATTACHMENT's COLOR_DATA colour (display-referred): raw; .w is the alpha threshold
			memcpy(r, p.diffuseCh, 16);
			r[4] = p.spec;
			r[5] = p.mipBias;
			for (int k = 0; k < 3; k++) { r[8 + k] = p.colour[k]; }
			r[11] = p.clipMin;
		}
		memcpy(r + 7 * 4, p.clipCh, 16);
		r[8 * 4] = p.clipMin;
		return slot = id;
	};

	struct Ent { int key; uint32_t skinRow, rec; };
	std::vector<ViewKey> keys;
	std::unordered_map<ViewKey, int, ViewKeyHash> keyIds;
	auto intern = [&](const ViewKey& k) -> int
	{
		const auto it = keyIds.find(k);
		if (it != keyIds.end()) { return it->second; }
		keys.push_back(k);
		return keyIds[k] = (int)keys.size() - 1;
	};
	// A pass's entries to groups: sorted by key so equal (mesh, material) draws are one instanced draw; render.char_instancing 0 keeps them in
	// character order, one draw each (the A/B against the grouped draw).
	auto flush = [&](std::vector<Ent>& ents, std::vector<Group>& out)
	{
		if (instancing_) { std::stable_sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.key < b.key; }); }
		size_t i = 0;
		while (i < ents.size())
		{
			size_t j = i + 1;
			if (instancing_) { while (j < ents.size() && ents[j].key == ents[i].key) { j++; } }
			if (s.list.size() / 2 + (j - i) > (size_t)listCap) { full("instance list"); break; }
			// the group's draw by value (the packet holds no pointer into the characters): the mesh's Handle is ready (shownOf's drawable())
			const ViewKey& key = keys[(size_t)ents[i].key];
			Group g;
			g.vbuf = key.mesh->gpu.get().vbuf;
			g.ibuf = key.mesh->gpu.get().ibuf;
			g.indexCount = key.mesh->indexCount;
			if (key.mesh2) { g.vbuf2 = key.mesh2->gpu.get().vbuf; }
			std::copy(std::begin(key.v), std::end(key.v), g.v);
			g.first = (uint32_t)(s.list.size() / 2);
			g.count = (uint32_t)(j - i);
			for (size_t k = i; k < j; k++)
			{
				s.list.push_back(ents[k].skinRow);
				s.list.push_back(ents[k].rec);
			}
			out.push_back(g);
			i = j;
		}
		ents.clear();
	};

	std::vector<Ent> body, armour, hair, items, standin, shadow[kMaxCascades];
	constexpr float kReach = 40.0f;	// units: a body is ~20 tall, with a long weapon or a mount about 40
	// which cascades take a character: a character wholly outside a cascade's tile box casts nothing into it (the box's sides, camera-relative,
	// inside positive, with kReach of slack for the body's height and what it holds): every character went into every cascade before, 4.4 ms of
	// CPU submit with 231 in range at Heft (kenshi-k16.2.205.1)
	auto cascadesOf = [&](const double pos[3], bool inCascade[kMaxCascades])
	{
		for (int k = 0; k < kMaxCascades; k++)
		{
			const ShadowCascade* sc = cascades ? cascades[k] : nullptr;
			inCascade[k] = false;
			if (!sc) { continue; }
			bool outside = false;
			for (int q = 0; q < 4 && !outside; q++)
			{
				const float* pl = sc->planes[q];
				outside = pl[0] * (float)(pos[0] - cam[0]) + pl[1] * (float)(pos[1] - cam[1]) + pl[2] * (float)(pos[2] - cam[2]) + pl[3] < -kReach;
			}
			inCascade[k] = !outside;
		}
	};
	for (size_t i = 0; i < n; i++)
	{
		const Char& own = all[i];
		// The loading stand-in (kenshi-k16.2.258.1, render.loading_standin): a build whose GPU objects are still in the stream lane, and a build
		// inside its two-stage switch, draw as their race's prewarmed body. A build that is still showing the one it replaced (was) is drawn as
		// before, with no stand-in.
		const bool stand = cull && standinLive(own) && (!shown[i] || shown[i] == &own) && own.body && own.body->mesh.gpu.ready()
			&& !(culled && culled(own.spec.id, own.pos));
		if (!shown[i])
		{
			if (cull) { drawState_[i] = stand ? kStandin : kNoModel; }	// nothing drawn yet: the stand-in, or nothing with render.loading_standin off
			if (!stand) { continue; }
		}
		const Char& c = shown[i] ? list(i) : own;
		if (!c.body || !c.mesh) { if (cull) { drawState_[i] = kNoModel; } continue; }
		if (cull && culled && culled(c.spec.id, c.pos)) { if (cull) { drawState_[i] = kFloorCut; } continue; }
		bool inCascade[kMaxCascades] = {};
		cascadesOf(c.pos, inCascade);
		const Body& b = *c.body;
		const auto& head = b.heads[(size_t)std::clamp(c.spec.head, 0, (int)b.heads.size() - 1)];
		ViewKey bk;
		bk.v[0] = b.tex.get();
		bk.v[1] = c.bodyNrm.get();
		bk.v[2] = b.mask.get();
		bk.v[3] = head[0].get();
		bk.v[4] = head[1].get();
		bk.v[5] = head[2].get();
		bk.v[6] = c.vestTex.get();
		bk.v[7] = c.vestNrm.get();
		bk.v[8] = c.vestCmap.get();
		bk.v[9] = c.hairTex.get();
		bk.v[10] = c.beardTex.get();
		bk.v[11] = c.blendNrm.get();
		ViewKey sk;
		sk.v[0] = white_.get();
		const int64_t row = skinOf(i, c);
		if (row < 0 || bodyRecord(i, c) < 0) { if (cull) { std::fill(drawState_.begin() + (long)i, drawState_.end(), kBuffersFull); } break; }	// this one and every one after it: flush()'s full buffers
		const int32_t br = bodyRecord(i, c);
		if (stand)
		{
			// The stand-in's own entries: the body (on its own clock the vertex shader mixes it in from the race base body, buffer 2) and each part
			// but the mounted items, which cannot morph and dissolve in on their own (skin row 199.w). The record id says which to the vertex
			// shader: 0 = the body, else a part. No texture is bound: the stand-in never shows one.
			const GpuMesh* base = &own.body->mesh;
			ViewKey k;
			k.v[0] = white_.get();
			k.mesh = own.mesh && own.mesh->gpu.ready() ? own.mesh : base;
			k.mesh2 = base;
			standin.push_back({ intern(k), (uint32_t)row, 0 });
			for (int kc = 0; kc < kMaxCascades; kc++) { if (inCascade[kc]) { shadow[kc].push_back({ intern(k), (uint32_t)row, (uint32_t)br }); } }
			for (const Part& p : own.parts)
			{
				if (p.mounted || !p.mesh->gpu.ready()) { continue; }	// a weapon is not an identity; a part's buffers may still be queued
				ViewKey pk;
				pk.mesh = pk.mesh2 = p.mesh;
				pk.v[0] = white_.get();
				standin.push_back({ intern(pk), (uint32_t)row, 1u });
			}
		}
		if (!shown[i]) { continue; }	// the stand-in alone: no real look to gather
		auto add = [&](const ViewKey& main, const ViewKey& shd, int32_t rec, std::vector<Ent>* mainList)
		{
			if (mainList) { mainList->push_back({ intern(main), (uint32_t)row, (uint32_t)rec }); }
			for (int k = 0; k < kMaxCascades; k++)
			{
				if (inCascade[k]) { shadow[k].push_back({ intern(shd), (uint32_t)row, (uint32_t)rec }); }
			}
		};
		bk.mesh = sk.mesh = c.mesh;
		add(bk, sk, br, &body);
		if (cull) { drawState_[i] = kBodyDrawn; }	// the body group is in; a flush() overflow can still drop it (it warns once, `full`)
		for (const GpuMesh* m : c.stumps)
		{
			bk.mesh = sk.mesh = m;
			add(bk, sk, br, &body);
		}
		for (size_t pi = 0; pi < c.parts.size(); pi++)
		{
			const Part& p = c.parts[pi];
			const int32_t rec = partRecord(i, c, pi);
			if (rec < 0) { break; }
			ViewKey mk, shd;
			mk.mesh = shd.mesh = p.mesh;
			if (p.kind == kArmour)
			{
				mk.v[0] = p.tex.get();
				mk.v[1] = p.nrm.get();
				mk.v[2] = p.cmap.get();
				shd.v[0] = p.nrm.get();
			}
			else
			{
				mk.v[0] = p.tex.get();
				shd.v[0] = p.tex.get();
			}
			add(mk, shd, rec, p.mounted ? &items : (p.kind == kArmour ? &armour : &hair));
		}
	}
	flush(body, s.plan.body);
	flush(armour, s.plan.armour);
	flush(hair, s.plan.hair);
	flush(items, s.plan.items);
	flush(standin, s.plan.standin);
	for (int k = 0; k < kMaxCascades; k++) { flush(shadow[k], s.plan.shadow[k]); }
}

void Characters::uploadSet(GpuSet& s, const FrameData& fd)
{
	s.written = false;
	s.plan = fd.plan;
	if (fd.skins.empty()) { return; }
	sg_write_buffer_desc wd = {};
	wd.src.data = { fd.skins.data(), fd.skins.size() * sizeof(float) };
	wd.dst.buffer = s.skins;
	sg_write_buffer_transient(&wd);
	wd.src.data = { fd.insts.data(), fd.insts.size() * sizeof(float) };
	wd.dst.buffer = s.insts;
	sg_write_buffer_transient(&wd);
	wd.src.data = { fd.list.data(), fd.list.size() * sizeof(uint32_t) };
	wd.dst.buffer = s.list;
	if (!fd.list.empty()) { sg_write_buffer_transient(&wd); }
	s.written = true;
}

void Characters::upload(const FrameData& fd)
{
	portraitUsed_ = 0;
	uploadSet(frame_, fd);
}

void Characters::prepare(const double cam[3], const ShadowCascade* const cascades[kMaxCascades], FrameData& out)
{
	instancing_ = kCharInstancing.get();
	out.clear();
	if (chars_.empty()) { return; }
	buildSet(out, chars_, true, cam, cascades, kMaxSkins, kMaxInsts, kMaxList);
	out.count = chars_.size();
	// dev: the draw counts per pass (draws / the parts they carry = what the per-part path drew), every 120 frames, with KCLIENT_STAGES set
	static const bool log = getenv("KCLIENT_STAGES") != nullptr;
	static int frames = 0;
	if (log && ++frames % 120 == 0)
	{
		auto sum = [](const std::vector<Group>& g) { size_t n = 0; for (const Group& x : g) { n += x.count; } return n; };
		const Plan& pl = out.plan;
		std::string line = "chars draws (groups/parts):";
		char b[96];
		auto add = [&](const char* name, const std::vector<Group>& g) { snprintf(b, sizeof b, " %s %zu/%zu", name, g.size(), sum(g)); line += b; };
		add("body", pl.body);
		add("armour", pl.armour);
		add("hair", pl.hair);
		add("items", pl.items);
		add("standin", pl.standin);
		for (int k = 0; k < kMaxCascades; k++) { snprintf(b, sizeof b, " shadow%d", k); add(b + 1, pl.shadow[k]); }
		int held = 0, fading = 0;
		for (const Char& c : chars_) { held += standinHeld(c) ? 1 : 0; fading += standinLive(c) ? 1 : 0; }
		fprintf(stderr, "%s skins %zu records %zu standin mode %d held %d live %d\n", line.c_str(), out.skins.size() / (size_t)(kSkinRows * 4),
			out.insts.size() / (size_t)(kInstRows * 4), standin_, held, fading);
	}
}

// the blocks every draw of a pipeline shares (lighting, AO, light volumes, shadow) and the view's own: applied once after the pipeline, not per draw
static void applyPassUniforms(const sg_range& lightRange, const sg_range& shadowRange)
{
	sg_apply_uniforms(UB_light_params, lightRange);
	sg_apply_uniforms(UB_ao_params, SG_RANGE(ao_binding().params));
	sg_apply_uniforms(UB_light_volumes, SG_RANGE(lightvol::binding().params));
	sg_apply_uniforms(UB_shadow_params, shadowRange);
}

bool Characters::renderShadow(int index, const ShadowCascade& sc)
{
	if (!frame_.written || index < 0 || index >= kMaxCascades) { return false; }
	const std::vector<Group>& groups = frame_.plan.shadow[index];
	if (groups.empty()) { return false; }
	drawstats::pipeline(shadowPip_);
	char_vs_view_t vv = {};
	memcpy(vv.mvp, sc.lvp, sizeof vv.mvp);
	sg_apply_uniforms(UB_char_vs_view, SG_RANGE(vv));
	shadow_view_t sv = {};
	memcpy(sv.bias, sc.bias, sizeof sv.bias);
	sg_apply_uniforms(UB_shadow_view, SG_RANGE(sv));
	for (const Group& g : groups)
	{
		sg_bindings bind = {};
		bind.vertex_buffers[0] = g.vbuf;
		bind.vertex_buffers[1] = frame_.list;
		bind.index_buffer = g.ibuf;
		bind.views[VIEW_skins] = frame_.skinsView;
		bind.views[VIEW_insts] = frame_.instsView;
		bind.views[VIEW_clip_tex] = g.v[0];
		bind.samplers[SMP_clip_smp] = wrap_;
		drawstats::bindings(&bind);
		drawstats::draw(0, g.indexCount, (int)g.count, (int)g.first);
	}
	return true;
}

float Characters::headHeight(size_t i)
{
	double target[3], eye[3];
	Char c;
	// portraitPose targets Bip01 Head + the race's `portrait offset y` (into a local copy: nothing the render side draws from)
	if (!portraitPose(i, 0, 0, target, eye, c)) { return 0; }
	return (float)(target[1] - chars_[i].pos[1]) - chars_[i].body->portrait[2];
}

bool Characters::portraitFrame(size_t i, float yaw, float pitch, double target[3], double eye[3], FrameData& out)
{
	out.clear();
	std::vector<Char> one(1);
	if (!portraitPose(i, yaw, pitch, target, eye, one[0])) { return false; }
	buildSet(out, one, false, eye, nullptr, kPortraitSkins, kPortraitInsts, kPortraitList);
	out.count = 1;
	return true;
}

bool Characters::portraitPose(size_t i, float yaw, float pitch, double target[3], double eye[3], Char& out)
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	if (!drawable(chars_[i])) { return false; }	// its GPU objects are still in the stream lane: the portrait waits (Portraits asks again)
	Char c = chars_[i];
	const int head = c.body->skel.bone("Bip01 Head");
	if (head < 0) { return false; }
	// FUN_140845b60 (the `idle stance` animation at time 0, weight 1): MINE: the stance's clip, all its bones, else the spec's raw clip
	c.layers.clear();
	c.idle = c.action = -1;
	const auto st = anims_.find(folded(c.spec.stance));
	std::string err;
	if (!addLayer(c, st != anims_.end() ? st->second.clip : c.spec.clip, 0, 0.0f, err)) { fprintf(stderr, "kclient: portrait: %s\n", err.c_str()); }
	// the original copies the node's attached entities (body, armour, hair); weapons hang on bones, not the node
	c.parts.erase(std::remove_if(c.parts.begin(), c.parts.end(), [](const Part& p) { return p.mounted; }), c.parts.end());
	c.mounts.clear();
	c.mountNames.clear();
	c.orient = { 1, 0, 0, 0 };
	pose(c);
	const kworld::SkelVec3& h = c.model[(size_t)head].pos;
	const float* pr = c.body->portrait;
	const float hp[3] = { h.x, h.y, h.z };
	// the eye: Ry(yaw) x Rx(pitch) x (0, 0, distance)
	const double v[3] = { std::sin(yaw) * std::cos(pitch) * pr[0], -std::sin(pitch) * pr[0], std::cos(yaw) * std::cos(pitch) * pr[0] };
	for (int k = 0; k < 3; k++)
	{
		target[k] = c.pos[k] + hp[k] + pr[1 + k];
		eye[k] = target[k] + v[k];
	}
	out = std::move(c);
	return true;
}

// mode: 0 body (and its stumps), 1 armour, 2 hair, 3 the loading stand-in (no textures: its own vertex shader mixes buffer 2 in)
void Characters::drawGroups(const GpuSet& s, const std::vector<Group>& groups, int mode)
{
	for (const Group& g : groups)
	{
		sg_bindings bind = {};
		bind.vertex_buffers[0] = g.vbuf;
		bind.index_buffer = g.ibuf;
		bind.vertex_buffers[1] = s.list;
		bind.views[VIEW_skins] = s.skinsView;
		if (mode == 3)
		{
			// the loading stand-in: buffer 2 is the race base body; the silhouette binds no texture, light or part record at all
			bind.vertex_buffers[2] = g.vbuf2;
			if (standin_ == kStandinDouble) { bindLighting(bind); }
			drawstats::bindings(&bind);
			drawstats::draw(0, g.indexCount, (int)g.count, (int)g.first);
			continue;
		}
		bind.views[VIEW_insts] = s.instsView;
		bindLighting(bind);
		if (mode == 0)
		{
			bind.views[VIEW_body_tex] = g.v[0];
			bind.views[VIEW_body_nrm] = g.v[1];
			bind.views[VIEW_body_mask] = g.v[2];
			bind.views[VIEW_head_tex] = g.v[3];
			bind.views[VIEW_head_nrm] = g.v[4];
			bind.views[VIEW_head_mask] = g.v[5];
			bind.views[VIEW_vest_tex] = g.v[6];
			bind.views[VIEW_vest_nrm] = g.v[7];
			bind.views[VIEW_vest_cmap] = g.v[8];
			bind.views[VIEW_hair_tex] = g.v[9];
			bind.views[VIEW_beard_tex] = g.v[10];
			bind.views[VIEW_body_blend] = g.v[11];
			bind.views[VIEW_blood_tex] = blood_.get();
			bind.samplers[SMP_border_smp] = border_;
			bind.samplers[SMP_blood_smp] = wrap_;
		}
		else if (mode == 1)
		{
			bind.views[VIEW_armour_tex] = g.v[0];
			bind.views[VIEW_armour_nrm] = g.v[1];
			bind.views[VIEW_armour_cmap] = g.v[2];
			bind.samplers[SMP_wrap_smp] = wrap_;
		}
		else
		{
			bind.views[VIEW_hair_map] = g.v[0];
			bind.samplers[SMP_wrap_smp] = wrap_;
		}
		drawstats::bindings(&bind);
		drawstats::draw(0, g.indexCount, (int)g.count, (int)g.first);
	}
}

// One pipeline's groups: the pipeline, the blocks it shares (lighting, shadow) and the view's (matrix, camera), then a draw per group
void Characters::drawMain(const GpuSet& s, const std::vector<Group> Plan::* which, int mode, const float mvp[16], const double cam[3], const LightParams& light,
	const ShadowParams& shadows)
{
	const std::vector<Group>& groups = s.plan.*which;
	if (!s.written || groups.empty()) { return; }
	const bool standin = mode == 3;
	const bool lit = !standin || standin_ == kStandinDouble;	// the silhouette's shader has no light, fog or view block
	const sg_range lightRange = { &light, sizeof light }, shadowRange = { &shadows, sizeof shadows };
	drawstats::pipeline(standin ? (standin_ == kStandinDouble ? standinLitPip_ : standinPip_) : mode == 0 ? pip_ : mode == 1 ? armourPip_ : hairPip_);
	if (lit) { applyPassUniforms(lightRange, shadowRange); }
	char_vs_view_t vv = {};
	memcpy(vv.mvp, mvp, sizeof vv.mvp);
	sg_apply_uniforms(UB_char_vs_view, SG_RANGE(vv));
	if (lit)
	{
		view_fs_t fv = {};
		for (int k = 0; k < 3; k++) { fv.cam[k] = (float)cam[k]; }
		sg_apply_uniforms(UB_view_fs, SG_RANGE(fv));
	}
	drawGroups(s, groups, mode);
}

void Characters::renderPortrait(const FrameData& fd, const float mvp[16], const double cam[3], const LightParams& light)
{
	if (portraitUsed_ >= kMaxPortraits) { return; }
	if (portraitUsed_ == (int)portraitSets_.size())
	{
		portraitSets_.push_back(std::make_unique<GpuSet>());
		makeSet(*portraitSets_.back(), kPortraitSkins, kPortraitInsts, kPortraitList, "character-portrait");
	}
	GpuSet& s = *portraitSets_[(size_t)portraitUsed_++];
	uploadSet(s, fd);
	const ShadowParams none{};	// sinfo.x 0: shadows off
	drawMain(s, &Plan::body, 0, mvp, cam, light, none);
	drawMain(s, &Plan::armour, 1, mvp, cam, light, none);
	drawMain(s, &Plan::hair, 2, mvp, cam, light, none);
}

void Characters::render(const float mvp[16], const double cam[3], const LightParams& light)
{
	const ShadowParams sp = shadow->params();
	// the loading stand-ins first: where the dither hands a pixel to the real look it is the later draw (kenshi-k16.2.258.1)
	drawMain(frame_, &Plan::standin, 3, mvp, cam, light, sp);
	drawMain(frame_, &Plan::body, 0, mvp, cam, light, sp);
	drawMain(frame_, &Plan::armour, 1, mvp, cam, light, sp);
	drawMain(frame_, &Plan::hair, 2, mvp, cam, light, sp);
}

// The items and weapons hung on a character's attachment points (kenshi-k16.2.101.1): the same armour pipeline and bindings as the worn
// pieces above, drawn in their own GPU section so a fight's weapon cost is visible beside the bodies.
void Characters::renderItems(const float mvp[16], const double cam[3], const LightParams& light)
{
	const ShadowParams sp = shadow->params();
	drawMain(frame_, &Plan::items, 1, mvp, cam, light, sp);
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
