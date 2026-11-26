#include <atomic>
#include <set>
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

constexpr int kVertexFloats = 18;	// pos 3, normal 3, uv 2, tangent 4, bone indices (4 bytes as one float slot), weights 4, part map bits
constexpr int kVertexBytes = kVertexFloats * 4;

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

}

namespace characters_internal
{
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

// The gender's <Colour> count: the parse stores (count-1)*10 as the ColourRange's slider max (140077090; kinspect rwh47_skin_tone_scale_static.md).
int readSkinStops(const std::string& path, bool female)
{
	float swatches[8][3];
	return readSkinRange(path, female, swatches);
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
		ri.footprint = r->floatOr("pathfind footprint radius", 2.6f);
		{
			const float avoidance = r->floatOr("water avoidance", 0.0f);	// ksim combat_data.cpp: 1 + a for a >= 0, else 1 / (1 - a)
			ri.waterCost = avoidance >= 0.0f ? 1.0f + avoidance : 1.0f / (1.0f - avoidance);
		}
		ri.attach = r->fileOr("attachment points");
		// AppearanceHuman vf13 (140533b50): bloodColour = `blood colour` as ARGB with alpha 1, bloodScale = (`blood horizontal`, `blood vertical`)
		const uint32_t bc = (uint32_t)r->intOr("blood colour", 0x800000);
		for (int k = 0; k < 3; k++) { ri.bloodCol[k] = (float)((bc >> (16 - 8 * k)) & 0xff) / 255.0f; }
		ri.bloodScale[0] = (float)r->intOr("blood horizontal", 1);
		ri.bloodScale[1] = r->floatOr("blood vertical", 0.15f);
		ri.portraitSet = r->findFloat("portrait distance") != nullptr;	// the record's framing; finishBody clears it for an animal (the head part's bounds frame those)
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
	// one ANIMATION / ANIMAL_ANIMATION record (the same fields)
	const auto parseAnim = [&](const kdata::Record* r)
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
		return a;
	};
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMATION")))
	{
		AnimInfo a = parseAnim(r);
		// the editor's Idle stance choices (AppearanceManager 14007d480 over the idle list 1405bbd40): usable idles off the overlay layer, no weapon
		// in either hand, not crouched
		if (a.usable && a.idle && a.code != 2 && a.handL == 0 && a.handR == 0 && !r->boolOr("crouched", false)) { stances.push_back({ r->stringID, r->name }); }
		AnimInfo& stored = anims_[folded(r->name)] = std::move(a);
		animOrder_.push_back(&stored);
	}
	// An animal's clips are ANIMAL_ANIMATION records, not ANIMATION ones: ANIMAL_CHARACTER (`race` -> RACE) lists them in `animations`, with the `strafe anim` and
	// `turning anim` ones. The client loaded only ANIMATION, so no animal rig had a single locomotion candidate (no walk, no idle: a frozen bind pose). Each
	// RACE takes the first ANIMAL_CHARACTER that names it; its records are the race's candidates (Char layers), in data order (the idle chooser keeps the first of equals).
	{
		std::unordered_map<std::string, const AnimInfo*> byId;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMAL_ANIMATION")))
		{
			AnimInfo a = parseAnim(r);
			a.order = (std::uint32_t)animalAnims_.size();
			animalAnims_.push_back(std::make_unique<AnimInfo>(std::move(a)));
			byId[r->stringID] =animalAnims_.back().get();
		}
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMAL_CHARACTER")))
		{
			const kdata::RefCategory* rr = r->findRefs("race");
			const kdata::Record* race = rr && !rr->entries.empty() ? gd.find(rr->entries[0].targetID) : nullptr;
			if (!race) { continue; }
			for (RaceInfo& ri : races_)
			{
				if (ri.name != race->name || !ri.animalAnims.empty()) { continue; }
				auto list = [&](const char* key, std::vector<const AnimInfo*>& out)
				{
					if (const kdata::RefCategory* refs = r->findRefs(key))
					{
						for (const kdata::RefEntry& e : refs->entries)
						{
							const auto it = byId.find(e.targetID);
							if (it != byId.end() && std::find(out.begin(), out.end(), it->second) == out.end()) { out.push_back(it->second); }
						}
					}
				};
				list("animations", ri.animalAnims);
				std::vector<const AnimInfo*> strafe;
				list("strafe anim", strafe);
				if (!strafe.empty()) { ri.animalStrafe = strafe[0]; }
				list("turning anim", ri.animalAnims);
			}
		}
	}
	{
		std::string list;
		for (const RaceInfo& ri : races_) { if (!ri.animalAnims.empty()) { list += (list.empty() ? "" : ", ") + ri.name + " (" + std::to_string(ri.animalAnims.size()) + ")"; } }
		fprintf(stderr, "kclient: anim: animal races with ANIMAL_ANIMATION records: %s\n", list.c_str());
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
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ITEM")))
	{
		// the props a producer's functionality `special tool` names (Pickaxe, Hoe): the ITEM's mesh with its material spec's maps, as a ground item draws
		ToolInfo t = { r->fileOr("mesh"), {}, {}, r->floatOr("equip offset", 0.0f) };
		if (t.mesh.empty()) { continue; }
		const kdata::RefCategory* m = r->findRefs("material");
		const kdata::Record* spec = m && !m->entries.empty() ? gd.find(m->entries.front().targetID) : nullptr;
		for (int depth = 0; spec && depth < 4; depth++)
		{
			const kdata::RefCategory* n = spec->findRefs("material");	// a collection: its first entry
			if (!n || n->entries.empty()) { break; }
			spec = gd.find(n->entries.front().targetID);
		}
		if (spec) { t.tex = spec->fileOr("texture map"); t.nrm = spec->fileOr("normal map"); }
		tools_[folded(r->name)] = std::move(t);
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
	if (const auto ti = s.tool.empty() ? tools_.end() : tools_.find(folded(s.tool)); ti != tools_.end()) { add(ti->second.tex); add(ti->second.nrm); }
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
	if (const auto ti = s.tool.empty() ? tools_.end() : tools_.find(folded(s.tool)); ti != tools_.end()) { want(ti->second.mesh, true); }
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
	// The RACE's own portrait framing, for a humanoid. An animal's RACE carries the fields too (Bonedog: distance 15), but its portrait is framed on its
	// head bone's bounds instead (kenshi-k16.2.390): those fields are the main view's. An animal: ANIMAL_CHARACTER's animations, or the animal voice set.
	b->portraitSet = ri->portraitSet && ri->animalAnims.empty() && ri->sounds < 4;
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

// Bone sizes from the editor values (14052e6c0, docs/reference/BODY_SHAPE.md after rwh.234): v = slider -> raw value (100 = 1).
// Bip01 is not sized. rwh.234: there is no slider curve -- the 0.935063 the live forearms carry is the muscle factor M1 (m = -0.2405
// for a stat-1 body, below), and the table's trailing `* x` factors (A*A, h*h, g*g, Fe*Fe) are the decompile misreading a LIMB MASK
// (1.0 on an intact limb) for the previous local: no squares, and the arms' length axis is Height only.
// UNSURE (left out): the race factor R (1 for humans), the thigh 0.8 flag (prosthetics?), the severable-limb masks (1 here).
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
	// m is the character's muscle (appearance +0x1a0): UNCLAMPED, (max(stat)*0.01 - 0.2) / 0.79, so a stat-1 body has m = -0.2405
	// (M1 0.935, M2 0.969) and a trained one has m > 0 (rwh.234; the caller supplies it, nothing here invents it).
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
	// live BODY_SHAPE's "Pelvis" row is the THIGH's source vector: the bone itself is Hips x Frame, with the Height on y (rwh.234, rwh.229)
	const kworld::SkelVec3 pelvis = { f, Y, f };
	set("Bip01 Pelvis", { x("Hips") * Fs, Hs, x("Hips") * Fs });
	const kworld::SkelVec3 leg = { 1, 0.95f, 1 };
	const float g = (2 - x("Legs shape")) * Lb * Fs;
	const float Fe = x("Feet") * Hs;
	for (const char* side : { "R", "L" })
	{
		const std::string b = std::string("Bip01 ") + side;
		set((b + " Thigh").c_str(), mul(pelvis, leg), { 1, Fs * (2 - Hs) * x("Hips"), 1 });
		set((b + " Calf").c_str(), { g, Y, g });	// the 0.95 leg factor is the Thigh's only (rwh.229 live: Calf y = Y)
		set((b + " Foot").c_str(), { Fe, x("Leg length"), Fe });
		set((b + " Toe0").c_str(), { Fe, Fe, Fe }, { 1, 1, Fe });
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
		set((b + " UpperArm").c_str(), { A, Hs, (1 + 1.5f * (Ab - 1)) * Fs }, { S * cx, 1, 1 });
		set((b + " Forearm").c_str(), { A, Hs, A });
		set((b + " Hand").c_str(), sc({ A, Hs, A }, h));
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

// The built body's size numbers for the dev dump (`dump body`): the stat muscle m, its factor M1 (boneSizes) and the right forearm's width scale.
bool Characters::bodySize(size_t i, float& m, float& m1, float& forearm) const
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	const Char& c = chars_[i];
	m = c.spec.muscle;
	m1 = 1 + (c.spec.female ? 0.24f : 0.27f) * m;	// lerp(1, 1.24 / 1.27, m), as boneSizes
	forearm = 1;
	const int b = c.body->skel.bone("Bip01 R Forearm");
	if (b >= 0 && (size_t)b < c.size.size()) { forearm = c.size[(size_t)b].x; }
	return true;
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
		// an animal race plays its ANIMAL_CHARACTER's ANIMAL_ANIMATION records, every other race the ANIMATION ones
		const RaceInfo* animalRace = nullptr;
		for (const RaceInfo& ri : races_) { if (folded(ri.name) == folded(s.race)) { animalRace = ri.animalAnims.empty() ? nullptr : &ri; break; } }
		const AnimInfo* const strafeRec = animalRace ? animalRace->animalStrafe : strafe;
		const std::vector<const AnimInfo*>& candidates = animalRace ? animalRace->animalAnims : animOrder_;
		for (const AnimInfo* a : candidates)
		{
			// the records this character can play at all: usable, normal category, the carry state, hands that fit; moving or idle (overlays: by
			// name only, except a carry idle such as `stand carrying left`; MINE: the idle chooser taking overlay records)
			const bool carryFits = a->code == 0 ? a->carry == 0 : a->carry == carryBits;
			const bool overlayIdle = a->code == 2 && carryBits && a->idle && a != armOnly;
			if (!a->usable || a->category != 0 || !carryFits || (a->code == 2 && !overlayIdle) || (a->move == 0 && !a->idle && a != strafeRec) || !handsFit(c, *a)) { continue; }
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
	if (!s.tool.empty()) { addTool(c, s.tool); }
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
	// a new sim character dithers in (render.char_fade, kenshi-k16.2.258.10.1); a build that replaces one on screen takes that one's place (swapIn). A
	// scene / offline body (no sim id: --character, a scene's char=, a preview) is placed whole: it is not an arrival.
	c.cover = c.spec.id ? 0.0f : 1.0f;
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

// A producer's special tool (BUILDING_FUNCTIONALITY `special tool`, an ITEM: "stuff like pickaxes, just used for appearance sake") in the right hand:
// its mesh on `hands` = Bip01 Prop2 like a drawn weapon's bare mesh, at the ITEM's own scale (1), identity rotation (kenshi-rwh.272: the `hands` default record,
// 1405373e0; no grip rotation, no scale). The only addition is FUN_140535d50's (0, 0, ITEM `equip offset`) in the bone frame; whether the pickaxe's item class
// takes it (item vf+0x2d8, the weapon-class override) is UNSURE in the exe, so it is applied whenever the ITEM data has a value (0 = a no-op).
void Characters::addTool(Char& c, const std::string& item)
{
	const auto ti = tools_.find(folded(item));
	if (ti == tools_.end()) { fprintf(stderr, "kclient: characters: no tool ITEM named %s\n", item.c_str()); return; }
	const kworld::Skeleton& sk = c.body->skel;
	Char::Mount mt = { kMaxBones - 1 - (int)c.mounts.size(), sk.bone("Bip01 Prop2"), {} };
	if (mt.bone < 0) { fprintf(stderr, "kclient: characters: %s: no Bip01 Prop2 for %s\n", label(c.spec).c_str(), item.c_str()); return; }
	kworld::toMatrix(kworld::sampleModel(sk, nullptr, mt.bone, 0.0f), mt.local);
	for (const AttachPoint& p : c.body->attach) { if (p.name == "hands") { mt.bone = p.bone; memcpy(mt.local, p.model, sizeof mt.local); } }
	if (mt.slot < (int)sk.bones.size()) { fprintf(stderr, "kclient: characters: %s: no palette slot left for %s\n", label(c.spec).c_str(), item.c_str()); return; }
	const float eo = ti->second.equipOffset;
	for (int i = 0; i < 3; i++) { mt.local[12 + i] += mt.local[8 + i] * eo; }
	std::string err;
	const GpuMesh* m = mesh(ti->second.mesh, nullptr, err, mt.slot);
	if (!m) { fprintf(stderr, "kclient: characters: %s: %s\n", item.c_str(), err.c_str()); return; }
	static std::set<std::string> said;
	if (said.insert(item).second) { fprintf(stderr, "kclient: characters: tool %s equip offset %g\n", item.c_str(), eo); }
	c.mounts.push_back(mt);
	c.mountNames.push_back("in the hand @ hands (" + sk.bones[(size_t)mt.bone].name + ") tool " + item);
	Part p = { kArmour, m, texture(ti->second.tex, nothing_), texture(ti->second.nrm, flat_), {}, { 0, 0, 0, 1 }, 0.6f, {}, 1.0f };
	p.cmap = nothing_;
	p.mounted = true;
	c.parts.push_back(p);
}

// The hand filter (14051d240): per hand 0 = must be empty, 1 = must hold a weapon, 2 = either; a record wanting both passes with exactly one held.
bool Characters::handsFit(const Char& c, const AnimInfo& a) const
{
	if (a.handL == 1 && a.handR == 1) { return c.handL != c.handR; }
	auto fits = [](bool held, int want) { return !(held && want == 0) && !(!held && want == 1); };
	return fits(c.handL, a.handL) && fits(c.handR, a.handR);
}

