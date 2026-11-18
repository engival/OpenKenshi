#include "world/buildings.hpp"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <fstream>
#include <future>
#include <map>
#include <tuple>
#include <unordered_map>

#include "kdata/format.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/features.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"

namespace
{

uint64_t mix(uint64_t h)
{
	h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
	h ^= h >> 27; h *= 0x94d049bb133111ebULL;
	return h ^ (h >> 31);
}

uint64_t hashString(const std::string& s)
{
	uint64_t h = 1469598103934665603ULL;
	for (const unsigned char c : s) { h = (h ^ c) * 1099511628211ULL; }
	return h;
}

// Uniform 0..100, deterministic in (seed, salt).
double roll(uint64_t seed, const std::string& salt) { return (double)(mix(seed ^ hashString(salt)) >> 11) * (100.0 / 9007199254740992.0); }

// Weighted pick over the live entries of a ref category; weight(e) <= 0 counts as 100 (fcs.def: "0 -> 100").
template <class W>
const kdata::RefEntry* pick(const kdata::RefCategory& c, uint64_t seed, const std::string& salt, W weight)
{
	double total = 0;
	for (const kdata::RefEntry& e : c.entries) { if (e.v0 != kdata::kReferenceRemoved) { total += weight(e) > 0 ? weight(e) : 100; } }
	if (total <= 0) { return nullptr; }
	double r = roll(seed, salt) / 100.0 * total;
	const kdata::RefEntry* last = nullptr;
	for (const kdata::RefEntry& e : c.entries)
	{
		if (e.v0 == kdata::kReferenceRemoved) { continue; }
		last = &e;
		r -= weight(e) > 0 ? weight(e) : 100;
		if (r < 0) { break; }
	}
	return last;
}

float transformAt(const std::array<std::byte, 28>& t, int i)	// x y z qw qx qy qz
{
	float v;
	memcpy(&v, t.data() + 4 * i, 4);
	return v;
}

// A record's EFFECT instances (BUILDING_PART / BUILDING / MAP_FEATURES): appended to `out`, range returned. The EFFECT record names the
// ParticleUniverse script (`particle system`, its `system` name), its type and whether the weather wind drives it; the instance carries
// the local transform. The instance list has no category name, exactly like `lights`.
void readEffectDefs(const kdata::GameData& gd, const kdata::Record& rec, std::vector<EffectDef>& out, uint32_t& first, uint32_t& count)
{
	static const int effectType = kworld::itemTypeByName("EFFECT");
	first = (uint32_t)out.size();
	count = 0;
	for (const kdata::Instance& in : rec.instances)
	{
		const kdata::Record* r = gd.find(in.targetStringID);
		if (!r || r->type != effectType) { continue; }
		EffectDef e;
		e.script = r->stringOr("particle system");
		if (e.script.empty()) { continue; }
		e.type = r->intOr("type", 0);
		e.windAffected = r->boolOr("wind affected", false);
		for (int k = 0; k < 3; k++) { e.pos[k] = transformAt(in.transform, k); }
		for (int k = 0; k < 4; k++) { e.quat[k] = transformAt(in.transform, 3 + k); }
		out.push_back(e);
		count++;
	}
}

// A record's LIGHT instances (BUILDING records only in practice: the campfire and Holy torch lights). Same fields as the part lights;
// kept separate because these positions are in the building frame, not a part's. Read now; drawn in slice 2.
void readOwnLights(const kdata::GameData& gd, const kdata::Record& rec, std::vector<LightDef>& out, uint32_t& first, uint32_t& count)
{
	static const int lightType = kworld::itemTypeByName("LIGHT");
	first = (uint32_t)out.size();
	count = 0;
	for (const kdata::Instance& in : rec.instances)
	{
		const kdata::Record* r = gd.find(in.targetStringID);
		if (!r || r->type != lightType) { continue; }
		const float w = transformAt(in.transform, 3), x = transformAt(in.transform, 4), y = transformAt(in.transform, 5), z = transformAt(in.transform, 6);
		const int rgb = r->intOr("diffuse", 0xffffff);
		const float pi = 3.14159265f;
		LightDef l = { { transformAt(in.transform, 0), transformAt(in.transform, 1), transformAt(in.transform, 2) },
			{ -2 * (x * y - w * z), -(1 - 2 * (x * x + z * z)), -2 * (y * z + w * x) },
			{ ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f },
			r->floatOr("brightness", 1.0f), r->floatOr("variance", 0.0f), r->floatOr("radius", 100.0f),
			std::cos(r->floatOr("inner", 0.0f) * pi / 360), std::cos(r->floatOr("outer", 0.0f) * pi / 360), r->floatOr("falloff", 1.0f),
			r->intOr("type", 0) == 1, r->intOr("effect", 0) };
		out.push_back(l);
		count++;
	}
}

struct Resolver
{
	const kdata::GameData& gd;
	BuildingSet& out;
	std::map<std::tuple<std::string, std::string, float, float, int, float, float, float, bool, int, int, float, bool, float, float, float, float, float, float, int, int, bool, uint32_t, uint32_t>, uint32_t> partIds;
	std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> partLights;	// part record stringID -> its range in BuildingSet::lights
	std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> partEffects;	// part record stringID -> its range in BuildingSet::effects
	int kind = 0;		// BuildingPartDef::kind of the parts being added
	int parentFloor = 0;	// `building floor` of the enclosing part
	bool ruined = false;	// a ruined placement (its squad template's `building ruined`, as kworld's placementRuined): parts use `destroyed mesh`

	int floorOf(const kdata::Record& part) const { return std::clamp(std::max(parentFloor, part.intOr("building floor", 0)), 0, 3); }

	// MATERIAL_SPEC (collection: one weighted pick, val1 = weight) -> final spec record, or null.
	const kdata::Record* spec(const std::string& sid, uint64_t seed) const
	{
		const kdata::Record* r = gd.find(sid);
		for (int depth = 0; r && depth < 4; depth++)
		{
			const kdata::RefCategory* c = r->findRefs("material");
			if (!c || c->entries.empty()) { return r; }
			const kdata::RefEntry* e = pick(*c, seed, sid, [](const kdata::RefEntry& x) { return x.v0; });
			r = e ? gd.find(e->targetID) : nullptr;
		}
		return r;
	}

	// The material spec a part uses: part.material, else `material match`, else the base BUILDING's `material` (BUILDING_MATERIALS.md).
	const kdata::Record* partMaterial(const kdata::Record& part, const kdata::Record& building, uint64_t seed, int depth = 0) const
	{
		if (const kdata::RefCategory* c = part.findRefs("material"); c && !c->entries.empty()) { return spec(c->entries.front().targetID, seed); }
		if (depth < 3)
		{
			const kdata::RefCategory* match = part.findRefs("material match");
			if (match && roll(seed, part.stringID + "/match") < part.intOr("material match chance", 100))
			{
				for (const kdata::RefEntry& e : match->entries)
				{
					if (const kdata::Record* other = gd.find(e.targetID); other && e.v0 != kdata::kReferenceRemoved) { return partMaterial(*other, building, seed, depth + 1); }
				}
			}
		}
		const kdata::RefCategory* base = building.findRefs("material");
		if (base && !base->entries.empty())
		{
			const kdata::RefEntry* e = pick(*base, seed, part.stringID + "/base", [](const kdata::RefEntry&) { return 0; });
			return e ? spec(e->targetID, seed) : nullptr;
		}
		return nullptr;
	}

	float base[3] = {};	// summed offsets of the enclosing parts (a nested part sits in its parent's frame; parent rotation is ignored, except a wind aim)
	bool aiming = false;	// inside a wind-aimed part (rotation function 4): its children turn with it (the windmill heads' rotors)
	float aimPivot[3] = {};
	float aimSpeed = 0;	// the wind-aimed part's `rotation speed max`

	// The part's own offset X/Y/Z; zero for position markers, whose offset marks a standing spot.
	static void own(const kdata::Record& part, float out[3])
	{
		const bool marker = part.boolOr("is for position marker", false);
		out[0] = marker ? 0.0f : part.floatOr("offset X", 0.0f);
		out[1] = marker ? 0.0f : part.floatOr("offset Y", 0.0f);
		out[2] = marker ? 0.0f : part.floatOr("offset Z", 0.0f);
	}

	// A part record's `lights` (its LIGHT instances), read once per record.
	std::pair<uint32_t, uint32_t> lightsOf(const kdata::Record& part)
	{
		static const int lightType = kworld::itemTypeByName("LIGHT");
		auto [it, fresh] = partLights.emplace(part.stringID, std::make_pair((uint32_t)out.lights.size(), 0u));
		if (!fresh) { return it->second; }
		for (const kdata::Instance& in : part.instances)
		{
			const kdata::Record* r = gd.find(in.targetStringID);
			if (!r || r->type != lightType) { continue; }
			const float w = transformAt(in.transform, 3), x = transformAt(in.transform, 4), y = transformAt(in.transform, 5), z = transformAt(in.transform, 6);
			const int rgb = r->intOr("diffuse", 0xffffff);
			const float pi = 3.14159265f;
			LightDef l = { { transformAt(in.transform, 0), transformAt(in.transform, 1), transformAt(in.transform, 2) },
				{ -2 * (x * y - w * z), -(1 - 2 * (x * x + z * z)), -2 * (y * z + w * x) },
				{ ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f },
				r->floatOr("brightness", 1.0f), r->floatOr("variance", 0.0f), r->floatOr("radius", 100.0f),
				std::cos(r->floatOr("inner", 0.0f) * pi / 360), std::cos(r->floatOr("outer", 0.0f) * pi / 360), r->floatOr("falloff", 1.0f),
				r->intOr("type", 0) == 1, r->intOr("effect", 0) };
			out.lights.push_back(l);
			it->second.second++;
		}
		return it->second;
	}

	// A part record's `effects` (its EFFECT instances), read once per record like lightsOf.
	std::pair<uint32_t, uint32_t> effectsOf(const kdata::Record& part)
	{
		auto [it, fresh] = partEffects.emplace(part.stringID, std::make_pair((uint32_t)out.effects.size(), 0u));
		if (!fresh) { return it->second; }
		uint32_t first = 0, count = 0;
		readEffectDefs(gd, part, out.effects, first, count);
		it->second = { first, count };
		return it->second;
	}

	// EFFECT / LIGHT instances on a BUILDING record (Campfire/Cooking Pit effects, campfire + Holy torch lights). Read per placement:
	// the instances are few and part-cached effects keep the common path cheap.
	std::pair<uint32_t, uint32_t> ownEffectsOf(const kdata::Record& rec)
	{
		uint32_t first = 0, count = 0;
		readEffectDefs(gd, rec, out.effects, first, count);
		return { first, count };
	}

	std::pair<uint32_t, uint32_t> ownLightsOf(const kdata::Record& rec)
	{
		uint32_t first = 0, count = 0;
		readOwnLights(gd, rec, out.ownLights, first, count);
		return { first, count };
	}

	// MATERIAL_SPEC `tile X` / `tile Y`, which scale the mesh UVs (mesh.glsl v_uv = texcoord0 * tile.xy). A 0 is not a collapse: the only three
	// records that say a zero tile are hanging lamps (indoor_lamp1 behind the Ceiling Lamp, Indoor_GAS LAMP, Old-World_Flouresc-Light, all tile Y 0)
	// and the original draws their shade from the whole texture, not a single texel row (kenshi-k16.2.203.1). 0 reads as 1, as the light bake
	// takes it (WorldBuildings::bakeProbes).
	static float tileOf(const kdata::Record& m, const char* field)
	{
		const float v = m.floatOr(field, 1.0f);
		return v > 0.0f ? v : 1.0f;
	}

	void addPart(const kdata::Record& part, const kdata::Record& building, uint64_t seed, std::vector<uint32_t>& into)
	{
		std::string mesh = part.fileOr("phs or mesh");
		if (ruined)	// GAMEDATA_SCHEMA `destroyed mesh`: "Ground floor will use normal mesh if blank. Upper floors will be removed if blank."
		{
			const std::string& destroyed = part.fileOr("destroyed mesh");
			if (!destroyed.empty()) { mesh = destroyed; }
			else if (floorOf(part) >= 1) { return; }
		}
		std::string lowered = mesh;
		for (char& c : lowered) { c = (char)tolower((unsigned char)c); }
		const auto endsWith = [&](const char* tail) { const size_t n = strlen(tail); return lowered.size() >= n && lowered.compare(lowered.size() - n, n, tail) == 0; };
		if (!endsWith(".mesh") || endsWith("nodemarker.mesh")) { return; }	// nodemarker: the editor's white dummy on spawn / light / use nodes
		const kdata::Record* m = partMaterial(part, building, seed);
		BuildingPartDef d;
		d.mesh = mesh;
		if (m)
		{
			d.texture = m->fileOr("texture map");
			d.tileX = tileOf(*m, "tile X");
			d.tileY = tileOf(*m, "tile Y");
			d.materialType = m->intOr("material type", 0);
			d.normal = m->fileOr("normal map");	// the lit normal (objects.hlsl); its alpha: ALPHA / FOLIAGE cut, EMISSIVE glow
			if (d.materialType == 3) { d.texture2 = m->fileOr("texture map 2"); d.normal2 = m->fileOr("normal map 2"); }
		}
		own(part, d.offset);
		d.marker = part.boolOr("is for position marker", false);
		d.rotFunction = part.intOr("rotation function", 0);
		d.rotAxis = part.intOr("rotation axis", 1);
		const float lo = part.floatOr("rotation speed min", 0.0f), hi = part.floatOr("rotation speed max", 0.0f);
		d.rotSpeed = std::fabs(hi) >= std::fabs(lo) ? hi : lo;
		if (d.marker) { d.offset[0] = d.offset[1] = d.offset[2] = 0; }
		for (int k = 0; k < 3; k++) { d.offset[k] += base[k]; }
		d.aimed = aiming || d.rotFunction == 4;
		for (int k = 0; k < 3; k++) { d.aimPivot[k] = aiming ? aimPivot[k] : d.rotFunction == 4 ? d.offset[k] : 0.0f; }
		d.aimSpeed = aiming ? aimSpeed : d.rotFunction == 4 ? hi : 0.0f;
		if (d.rotFunction == 3)
		{
			d.windMin = part.floatOr("wind speed rotation min", 0.0f);
			d.windMax = part.floatOr("wind speed rotation max", 0.0f);
		}
		d.kind = kind;
		d.interior = kind == 1 || kind == 3;	// the exe's INTERIOR material: interior parts and interior-layout furniture (objects.hlsl dustAmount.y)
		d.floor = ruined ? 0 : floorOf(part);	// a ruin has no upper floor to slice: its destroyed walls (an upper-floor part's `destroyed mesh`) stand on floor 0
		d.blackCap = kind == 0 && lowered.find("black", lowered.find_last_of("/\\") + 1) != std::string::npos;	// the file name, not a directory (buildings/blackmarket/)
		d.sign = kind == 0 && (lowered.find("/signs/") != std::string::npos || lowered.find("\\signs\\") != std::string::npos || lowered.find("misc/signboard") != std::string::npos);
		d.stairs = part.boolOr("is stairs", false);
		std::tie(d.firstLight, d.lightCount) = lightsOf(part);
		std::tie(d.firstEffect, d.effectCount) = effectsOf(part);
		const auto key = std::make_tuple(d.mesh, d.texture, d.tileX, d.tileY, d.materialType, d.offset[0], d.offset[1], d.offset[2], d.marker, d.rotFunction, d.rotAxis, d.rotSpeed, d.aimed, d.aimPivot[0], d.aimPivot[1], d.aimPivot[2], d.aimSpeed, d.windMin, d.windMax, d.kind, d.floor, d.stairs,
			d.lightCount ? d.firstLight : 0u, d.effectCount ? d.firstEffect : 0u);
		auto [it, fresh] = partIds.emplace(key, (uint32_t)out.parts.size());
		if (fresh) { out.parts.push_back(d); }
		into.push_back(it->second);
	}

	// A ground item's part (BuildingSet::items): the mesh at the item's origin, no offset; false when the record has no drawable mesh.
	bool addItem(const kdata::Record& item, bool groundOnly, std::vector<uint32_t>& into)
	{
		std::string mesh = item.fileOr("ground mesh");
		if (mesh.empty() && !groundOnly) { mesh = item.fileOr("mesh"); }
		if (mesh.empty()) { return false; }
		BuildingPartDef d;
		d.mesh = mesh;
		d.limb = item.intOr("item function", 0) == 0x11;	// a severed limb: SeveredLimbItem binds the item's texture, normal and color maps
		const kdata::RefCategory* c = item.findRefs("material");
		if (const kdata::Record* m = c && !c->entries.empty() ? spec(c->entries.front().targetID, hashString(item.stringID)) : nullptr)
		{
			d.texture = m->fileOr("texture map");
			d.materialType = m->intOr("material type", 0);
			d.normal = m->fileOr("normal map");
			if (d.limb) { d.colorMap = m->fileOr("color map"); }
		}
		into.push_back((uint32_t)out.parts.size());
		out.parts.push_back(d);
		return true;
	}

	// `parts` of a BUILDING / BUILDING_PART: group 0 rolls each part by its chance, other groups pick one by chance weight.
	void collect(const kdata::Record& owner, const kdata::Record& building, uint64_t seed, std::vector<uint32_t>& into, int depth)
	{
		const kdata::RefCategory* c = owner.findRefs("parts");
		if (!c || depth > 3) { return; }
		std::map<int, kdata::RefCategory> groups;
		for (const kdata::RefEntry& e : c->entries)
		{
			if (e.v0 == kdata::kReferenceRemoved) { continue; }
			if (e.v0 == 0)	// group 0: each part rolled independently, v1 = absolute chance in percent
			{
				if (roll(seed, owner.stringID + "/" + std::to_string(&e - c->entries.data())) < e.v1) { include(e.targetID, building, seed, into, depth); }
			} else { groups[e.v0].entries.push_back(e); }
		}
		for (auto& [g, cat] : groups)
		{
			const kdata::RefEntry* e = pick(cat, seed, owner.stringID + "/g" + std::to_string(g), [](const kdata::RefEntry& x) { return x.v1; });
			if (e) { include(e->targetID, building, seed, into, depth); }
		}
	}

	void include(const std::string& sid, const kdata::Record& building, uint64_t seed, std::vector<uint32_t>& into, int depth)
	{
		const kdata::Record* part = gd.find(sid);
		if (!part) { return; }
		addPart(*part, building, seed, into);
		float o[3];
		own(*part, o);
		for (int k = 0; k < 3; k++) { base[k] += o[k]; }
		const int savedFloor = parentFloor;
		parentFloor = floorOf(*part);
		const bool savedAiming = aiming;
		const float savedPivot[3] = { aimPivot[0], aimPivot[1], aimPivot[2] }, savedSpeed = aimSpeed;
		if (!aiming && part->intOr("rotation function", 0) == 4)
		{
			aiming = true;
			for (int k = 0; k < 3; k++) { aimPivot[k] = base[k]; }	// base already holds this part's own offset
			aimSpeed = part->floatOr("rotation speed max", 0.0f);
		}
		collect(*part, building, seed, into, depth + 1);
		aiming = savedAiming;
		aimSpeed = savedSpeed;
		for (int k = 0; k < 3; k++) { aimPivot[k] = savedPivot[k]; }
		parentFloor = savedFloor;
		for (int k = 0; k < 3; k++) { base[k] -= o[k]; }
	}

	// A BUILDING's `interior` / `interior mask` list as parts of the given kind. Every live entry counts: the v0/v1 of these refs are 0/0 or 0/100
	// (INTERIORS.md), and a 0 chance on a mask cannot mean "never".
	void collectAll(const kdata::Record& building, const char* category, int partKind, uint64_t seed, std::vector<uint32_t>& into)
	{
		const kdata::RefCategory* c = building.findRefs(category);
		if (!c) { return; }
		kind = partKind;
		for (const kdata::RefEntry& e : c->entries) { if (e.v0 != kdata::kReferenceRemoved) { include(e.targetID, building, seed, into, 0); } }
		kind = 0;
	}
};

// Hand-placed features -> parts and instances of the same set. Each record mesh is one part (a FOLIAGE_MESH `leaves mesh` is a second,
// alpha-tested part); the placement's y is absolute, scale may be non-uniform (uniform part in `scale`, the rest in `stretch`).
void addFeatures(BuildingSet& set, const kdata::GameData& gd, const kworld::Heightmap& terrain, const std::string& dataDir)
{
	const kworld::Features features = kworld::Features::join(gd, dataDir);
	std::map<std::tuple<std::string, std::string, std::string, std::string, float, float, int, int, bool>, uint32_t> partIds;
	std::vector<std::vector<uint32_t>> recordParts(features.records().size());
	for (size_t r = 0; r < features.records().size(); r++)
	{
		for (const kworld::FeatureMesh& m : features.records()[r].meshes)
		{
			BuildingPartDef d;
			d.mesh = m.mesh;
			d.texture = m.texture;
			if (uvMapped((int)m.mode)) { d.normal = m.normalMap; }	// the lit normal; its alpha: EMISSIVE the glow, FOLIAGE the cut-out mask
			if (m.mode == kworld::FeatureMode::DualTexture || m.mode == kworld::FeatureMode::DualTriplanar) { d.texture2 = m.texture2; d.normal2 = m.normalMap2; }
			d.tileX = m.tileX;
			d.tileY = m.tileY;
			d.featureMode = (int)m.mode;
			d.alphaCut = m.alphaThreshold;
			d.sway = m.leaves;	// hand-placed trees sway too (mesh.glsl inst_vs)
			d.localCoords = features.records()[r].localCoordinates;
			const auto key = std::make_tuple(d.mesh, d.texture, d.normal, d.texture2, d.tileX, d.tileY, d.featureMode, d.alphaCut, d.localCoords);
			auto [it, fresh] = partIds.emplace(key, (uint32_t)set.parts.size());
			if (fresh) { set.parts.push_back(d); }
			recordParts[r].push_back(it->second);
		}
	}
	set.featurePlacements = features.placements().size();
	set.roadFoliage = features.roadFoliage().size();
	for (const kworld::FeaturePlacement& p : features.placements())
	{
		const kworld::FeatureRecord& rec = features.records()[(size_t)p.record];
		const std::vector<uint32_t>& parts = recordParts[(size_t)p.record];
		set.featureMissing += !rec.found;
		set.featureHidden += rec.found && rec.hidden;
		const float scale = std::fmax(std::fabs(p.scaleX), std::fmax(std::fabs(p.scaleY), std::fabs(p.scaleZ)));
		const kworld::ZoneCell cell = kworld::zoneCellOf(p.x, p.z);
		if (parts.empty() || scale <= 0 || cell.x < 0 || cell.z < 0 || cell.x >= kworld::kZoneCellsPerAxis || cell.z >= kworld::kZoneCellsPerAxis) { continue; }
		BuildingInstance b;
		b.feature = true;
		b.x = p.x; b.y = p.y; b.z = p.z;
		if (p.rotationFinite()) { b.qw = (float)p.qw; b.qx = (float)p.qx; b.qy = (float)p.qy; b.qz = (float)p.qz; }
		b.scale = scale;
		b.stretch[0] = p.scaleX / scale; b.stretch[1] = p.scaleY / scale; b.stretch[2] = p.scaleZ / scale;
		b.ground = (float)terrain.height(p.x, p.z);
		b.firstPart = (uint32_t)set.partRefs.size();
		b.partCount = (uint32_t)parts.size();
		for (const uint32_t id : parts) { set.partRefs.push_back(id); }
		if (const kdata::Record* frec = gd.find(p.recordID))
		{
			uint32_t first = 0, count = 0;
			readEffectDefs(gd, *frec, set.effects, first, count);	// the four MAP_FEATURES storm/steam systems
			b.firstEffect = first; b.effectCount = count;
			readOwnLights(gd, *frec, set.ownLights, first, count);
			b.firstOwnLight = first; b.ownLightCount = count;
		}
		set.featureModes[(int)set.parts[parts[0]].featureMode]++;
		set.cells[kworld::zoneIndexOf(cell)].push_back((uint32_t)set.instances.size());
		set.instances.push_back(b);
		set.featureInstances++;
	}
}

// ---- interiors (docs/reference/INTERIORS.md) ----

std::vector<std::byte> readBytes(const std::string& path)
{
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	if (!in) { throw std::runtime_error("cannot open " + path); }
	std::vector<std::byte> bytes((size_t)in.tellg());
	in.seekg(0);
	in.read((char*)bytes.data(), (std::streamsize)bytes.size());
	return bytes;
}

template <class F>
void parallelFor(size_t n, F f)
{
	std::atomic<size_t> next{ 0 };
	std::vector<std::future<void>> workers;
	for (int t = 0; t < 6; t++) { workers.push_back(std::async(std::launch::async, [&] { for (size_t i; (i = next++) < n;) { f(i); } })); }
	for (std::future<void>& w : workers) { w.get(); }
}

struct LayoutItem
{
	std::string id, target;
	float pos[3], quat[4];		// building-local position; rotation w, x, y, z
	int floor;			// state int `floor`, -1 if the state has none
	uint32_t index;			// the instance's position in the collection, every entry counted (the wire's container `piece`, SIMLINK kind 3 tail)
};

// interiors.level of the four layers in load order: collection sid `<building sid>-<layout name>` -> its furniture. A later layer's collection replaces an
// earlier one of the same sid (410 distinct sids, 71 in more than one layer). `EXT-<building sid>-<exterior layout name>` collections are the outside
// dressing (signs), joined the same way.
std::unordered_map<std::string, std::vector<LayoutItem>> loadLayouts(const std::string& dataDir)
{
	static const char* const layers[] = { "", "Newwworld/", "Dialogue/", "rebirth/" };
	const int collectionType = kworld::itemTypeByName("INSTANCE_COLLECTION"), stateType = kworld::itemTypeByName("GAMESTATE_BUILDING");
	kdata::File files[4];
	parallelFor(4, [&](size_t i)
	{
		const std::string path = dataDir + "/newland/leveldata/" + layers[i] + "interiors.level";
		files[i] = kdata::parse(readBytes(path), "interiors.level");
	});
	std::unordered_map<std::string, std::vector<LayoutItem>> out;
	for (const kdata::File& f : files)
	{
		std::unordered_map<std::string, const kdata::Record*> states;
		for (const kdata::Record& r : f.records) { if (r.type == stateType) { states[r.stringID] = &r; } }
		for (const kdata::Record& r : f.records)
		{
			if (r.type != collectionType || r.instances.empty()) { continue; }
			std::vector<LayoutItem>& items = out[r.stringID];
			items.clear();
			for (size_t n = 0; n < r.instances.size(); n++)
			{
				const kdata::Instance& in = r.instances[n];
				if (in.targetStringID.empty()) { continue; }
				LayoutItem it = { in.id, in.targetStringID, { transformAt(in.transform, 0), transformAt(in.transform, 1), transformAt(in.transform, 2) },
					{ transformAt(in.transform, 3), transformAt(in.transform, 4), transformAt(in.transform, 5), transformAt(in.transform, 6) }, -1, (uint32_t)n };
				const auto st = in.states.empty() ? states.end() : states.find(in.states.front());
				if (st != states.end()) { if (const kdata::FieldInt* fl = st->second->findInt("floor")) { it.floor = fl->value; } }
				items.push_back(std::move(it));
			}
		}
	}
	return out;
}

// "<zone file path>\n<placement instance id>" -> its `interior layout name` and `exterior layout name`, via the placement's first state, a
// GAMESTATE_BUILDING record of the same zone file. Only placements with a non-empty name are kept (the other may be empty). The zone files are parsed a
// second time here (kworld's ZoneFile drops the records).
struct LayoutNames { std::string interior, exterior; };
std::unordered_map<std::string, LayoutNames> loadLayoutNames(const kworld::Zones& zones)
{
	const int buildingList = kworld::itemTypeByName("INSTANCE_COLLECTION"), stateType = kworld::itemTypeByName("GAMESTATE_BUILDING");
	std::vector<std::unordered_map<std::string, LayoutNames>> perFile(zones.files().size());
	parallelFor(zones.files().size(), [&](size_t i)
	{
		const kworld::ZoneFile& zf = zones.files()[i];
		if (zf.placementCount == 0) { return; }
		const kdata::File f = kdata::parse(readBytes(zf.path), "zone");
		std::unordered_map<std::string, LayoutNames> names;
		for (const kdata::Record& r : f.records)
		{
			if (r.type != stateType) { continue; }
			const kdata::FieldString* in = r.findString("interior layout name");
			const kdata::FieldString* ex = r.findString("exterior layout name");
			LayoutNames n{ in ? in->value : "", ex ? ex->value : "" };
			if (!n.interior.empty() || !n.exterior.empty()) { names[r.stringID] = std::move(n); }
		}
		if (names.empty()) { return; }
		for (const kworld::ZoneInstance& zi : zf.instances)
		{
			if (zi.recordType != buildingList || zi.collectionID != "0-buildinglist" || zi.kind != kworld::ZoneInstanceKind::Placement || zi.states.empty()) { continue; }
			if (const auto n = names.find(zi.states.front()); n != names.end()) { perFile[i][zi.instanceID] = n->second; }
		}
	});
	std::unordered_map<std::string, LayoutNames> out;
	for (size_t i = 0; i < perFile.size(); i++) { for (const auto& [id, name] : perFile[i]) { out[zones.files()[i].path + "\n" + id] = name; } }
	return out;
}

void rotateBy(const float q[4], const double v[3], double out[3])	// q = w x y z
{
	const double ux = q[1], uy = q[2], uz = q[3], w = q[0];
	const double cx = uy * v[2] - uz * v[1] + w * v[0], cy = uz * v[0] - ux * v[2] + w * v[1], cz = ux * v[1] - uy * v[0] + w * v[2];
	out[0] = v[0] + 2 * (uy * cz - uz * cy);
	out[1] = v[1] + 2 * (uz * cx - ux * cz);
	out[2] = v[2] + 2 * (ux * cy - uy * cx);
}

// The furniture of one placed building: each layout item is a BUILDING placed in the building's local frame (the same mesh frame as the parts: a floor part
// at y 182 holds furniture at y 182, verified on the Outpost s-III), so its world transform is the building's composed with the item's.
// exterior: an `EXT-` layout (signs): shell parts (kind 0) in the building cells, drawn at the building's range, not only near the camera as furniture is.
void addFurniture(BuildingSet& set, Resolver& rs, const kdata::GameData& gd, const kworld::PlacedInstance& p, uint32_t parent, uint64_t key, const std::vector<LayoutItem>& items, bool exterior)
{
	const BuildingInstance pb = set.instances[parent];
	const float pq[4] = { pb.qw, pb.qx, pb.qy, pb.qz };
	for (const LayoutItem& it : items)
	{
		const kdata::Record* target = gd.find(it.target);
		if (!target) { continue; }
		std::vector<uint32_t> chosen;
		rs.kind = exterior ? 0 : 3;
		rs.collect(*target, *target, hashString(p.instanceID + "/" + it.id), chosen, 0);
		rs.kind = 0;
		if (chosen.empty()) { set.furnitureNoGeometry++; continue; }
		const double local[3] = { it.pos[0] * pb.scale, it.pos[1] * pb.scale, it.pos[2] * pb.scale };
		double rot[3];
		rotateBy(pq, local, rot);
		BuildingInstance f;
		f.x = pb.x + rot[0]; f.y = pb.y + rot[1]; f.z = pb.z + rot[2];
		f.qw = pb.qw * it.quat[0] - pb.qx * it.quat[1] - pb.qy * it.quat[2] - pb.qz * it.quat[3];
		f.qx = pb.qw * it.quat[1] + pb.qx * it.quat[0] + pb.qy * it.quat[3] - pb.qz * it.quat[2];
		f.qy = pb.qw * it.quat[2] - pb.qx * it.quat[3] + pb.qy * it.quat[0] + pb.qz * it.quat[1];
		f.qz = pb.qw * it.quat[3] + pb.qx * it.quat[2] - pb.qy * it.quat[1] + pb.qz * it.quat[0];
		f.scale = target->floatOr("scale", 1.0f) * pb.scale;
		f.ground = pb.ground;
		f.parent = (int32_t)parent;
		// the wire's container tail names an interior piece by this pair (SIMLINK kind 3 tail); an exterior piece's index is another collection's
		f.key = exterior ? 0 : key;
		f.piece = it.index;
		f.floor = (int8_t)std::clamp(it.floor, -1, 3);
		f.ceiling = target->boolOr("ceiling placement", false);
		std::tie(f.firstEffect, f.effectCount) = rs.ownEffectsOf(*target);	// a Campfire placed by a layout
		std::tie(f.firstOwnLight, f.ownLightCount) = rs.ownLightsOf(*target);
		f.firstPart = (uint32_t)set.partRefs.size();
		f.partCount = (uint32_t)chosen.size();
		for (const uint32_t id : chosen) { set.partRefs.push_back(id); }
		(exterior ? set.cells : set.interiorCells)[kworld::zoneIndexOf(kworld::zoneCellOf(f.x, f.z))].push_back((uint32_t)set.instances.size());
		set.instances.push_back(f);
		set.furnitureInstances++;
	}
}

}

BuildingSet BuildingSet::load(const std::string& dataDir, bool features)
{
	BuildingSet set;
	const auto t0 = std::chrono::steady_clock::now();
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const kworld::Zones zones = kworld::Zones::load(dataDir);
		const std::vector<kworld::PlacedInstance> placed = zones.placeMerged(terrain, gd);
		set.placedRaw = placed.size();

		const auto tInterior = std::chrono::steady_clock::now();
		const auto layouts = loadLayouts(dataDir);
		const auto layoutNames = loadLayoutNames(zones);
		set.interiorLoadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tInterior).count();
		std::unordered_map<std::string, uint32_t> nameIds;

		Resolver rs{ gd, set, {}, {}, {} };
		for (const kworld::PlacedInstance& p : placed)
		{
			const kdata::Record* building = gd.find(p.buildingID);
			if (!building) { set.missingTarget++; continue; }
			std::vector<uint32_t> chosen;
			const kdata::Record* squad = p.squadTemplate.empty() ? nullptr : gd.find(p.squadTemplate);
			rs.ruined = squad && squad->boolOr("building ruined", false);
			set.ruinedPlacements += rs.ruined;
			rs.collect(*building, *building, hashString(p.instanceID), chosen, 0);
			if (chosen.empty()) { rs.ruined = false; set.noGeometry++; continue; }
			BuildingInstance b;
			b.x = p.x; b.y = p.y; b.z = p.z;
			b.qw = (float)p.qw; b.qx = (float)p.qx; b.qy = (float)p.qy; b.qz = (float)p.qz;
			b.scale = p.scale;
			b.ground = (float)(p.y - p.relativeY);
			b.height = building->floatOr("building height", 100.0f);
			for (const uint32_t id : chosen) { set.typeCount[set.parts[id].materialType < 5 && set.parts[id].materialType >= 0 ? set.parts[id].materialType : 0]++; }
			const size_t shellParts = chosen.size();
			if (!rs.ruined)	// a ruin has no interior: the Hub's ruined Storm Houses have no stairs up in the original (STORM_HOUSE_INTERIOR_LIVE.md "Notes for the bake"); its debris is the shell's `destroyed mesh`
			{
				rs.collectAll(*building, "interior", 1, hashString(p.instanceID), chosen);
				rs.collectAll(*building, "interior mask", 2, hashString(p.instanceID), chosen);
			}
			const bool wasRuined = rs.ruined;
			rs.ruined = false;
			set.interiorPartUses += chosen.size() - shellParts;
			b.firstPart = (uint32_t)set.partRefs.size();
			b.partCount = (uint32_t)chosen.size();
			for (const uint32_t id : chosen) { set.partRefs.push_back(id); b.maxFloor = (uint8_t)std::max<int>(b.maxFloor, set.parts[id].floor); }
			b.nameId = nameIds.emplace(building->name, (uint32_t)set.names.size()).first->second;
			if (b.nameId == set.names.size()) { set.names.push_back(building->name); }
			std::tie(b.firstEffect, b.effectCount) = rs.ownEffectsOf(*building);	// Campfire / Cooking Pit flames
			std::tie(b.firstOwnLight, b.ownLightCount) = rs.ownLightsOf(*building);	// campfire light (read, drawn in slice 2)
			const uint32_t index = (uint32_t)set.instances.size();
			const uint64_t key = kworld::doorKey(kworld::placementId(p));	// the wire's `building` for its doors, characters and layout containers
			set.cells[kworld::zoneIndexOf(kworld::zoneCellOf(p.x, p.z))].push_back(index);
			set.byKey[key] = index;
			set.instances.push_back(b);
			if (b.maxFloor > 0) { set.floorable.push_back(index); }
			// The building's `doors` (child buildings modelled in the parent's frame: the leaf slab in the gap, kworld collision.cpp): one instance per
			// door, drawn with the parent's transform, moved by WorldBuildings::setDoors to the pose the sim's door state gives. Never loaded before
			// (kenshi-k16.2.241.1: the front door of the Storm House was missing open and closed). A ruin has no doors, only doorways.
			if (const kdata::RefCategory* doors = wasRuined ? nullptr : building->findRefs("doors"))
			{
				size_t at = 0;
				for (const kdata::RefEntry& e : doors->entries)
				{
					if (e.v0 == kdata::kReferenceRemoved) { continue; }
					const size_t number = at++;
					const kdata::Record* door = gd.find(e.targetID);
					if (!door) { continue; }
					std::vector<uint32_t> leaf;
					rs.collect(*door, *door, hashString(p.instanceID + "/" + door->stringID), leaf, 0);
					if (leaf.empty()) { continue; }
					BuildingInstance d = b;
					d.parent = (int32_t)index;
					d.door = kworld::doorKey(kworld::doorId(p, number));
					d.floor = 0;
					d.maxFloor = 0;
					d.effectCount = d.ownLightCount = 0;
					d.firstPart = (uint32_t)set.partRefs.size();
					d.partCount = (uint32_t)leaf.size();
					for (const uint32_t id : leaf) { set.partRefs.push_back(id); }
					set.cells[kworld::zoneIndexOf(kworld::zoneCellOf(p.x, p.z))].push_back((uint32_t)set.instances.size());
					set.instances.push_back(d);
					set.doorInstances++;
				}
			}
			const auto layoutName = layoutNames.find(p.zonePath + "\n" + p.instanceID);
			if (layoutName == layoutNames.end()) { continue; }
			if (!layoutName->second.interior.empty())
			{
				set.layoutPlacements++;
				if (const auto layout = layouts.find(p.buildingID + "-" + layoutName->second.interior); layout != layouts.end())
				{
					set.layoutJoined++;
					addFurniture(set, rs, gd, p, index, key, layout->second, false);
				} else { set.layoutMissing++; }
			}
			if (!layoutName->second.exterior.empty())
			{
				set.exteriorPlacements++;
				if (const auto layout = layouts.find("EXT-" + p.buildingID + "-" + layoutName->second.exterior); layout != layouts.end())
				{
					set.exteriorJoined++;
					addFurniture(set, rs, gd, p, index, key, layout->second, true);
				}
			}
		}
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("BUILDING")))
		{
			if (!r->boolOr("has inventory", false)) { continue; }
			if (const kdata::RefCategory* fn = r->findRefs("functionality"))
			{
				// the exe copies functionality[0].function only (kenshi-rwh.107): test the first entry, not any
				if (!fn->entries.empty())
				{
					const kdata::Record* f = gd.find(fn->entries.front().targetID);
					if (f && f->intOr("function", 0) == 9) { set.shopCounters.insert(r->stringID); }
				}
			}
			std::vector<uint32_t> chosen;
			rs.collect(*r, *r, hashString(r->stringID), chosen, 0);
			if (chosen.empty()) { continue; }
			set.storage[r->stringID] = { (uint32_t)set.partRefs.size(), (uint32_t)chosen.size() };
			set.partRefs.insert(set.partRefs.end(), chosen.begin(), chosen.end());
		}
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("BUILDING")))	// the player's buildings that are not storage: the same parts a placement of the record draws
		{
			if (set.storage.count(r->stringID)) { continue; }
			std::vector<uint32_t> chosen;
			rs.collect(*r, *r, hashString(r->stringID), chosen, 0);
			if (chosen.empty()) { continue; }
			BuildingSet::Shell sh;
			sh.first = (uint32_t)set.partRefs.size();
			sh.count = (uint32_t)chosen.size();
			sh.height = r->floatOr("building height", 100.0f);
			sh.nameId = nameIds.emplace(r->name, (uint32_t)set.names.size()).first->second;
			if (sh.nameId == set.names.size()) { set.names.push_back(r->name); }
			set.partRefs.insert(set.partRefs.end(), chosen.begin(), chosen.end());
			set.shells[r->stringID] = sh;
		}
		for (const char* type : { "ITEM", "NEST_ITEM", "WEAPON", "CROSSBOW", "ARMOUR", "CONTAINER" })
		{
			const bool groundOnly = strcmp(type, "ITEM") != 0 && strcmp(type, "NEST_ITEM") != 0;
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
			{
				std::vector<uint32_t> chosen;
				if (!rs.addItem(*r, groundOnly, chosen)) { continue; }
				set.items[r->stringID] = { (uint32_t)set.partRefs.size(), 1 };
				set.partRefs.push_back(chosen[0]);
			}
		}
		// the character editor's platform (docs/CLIENT.md "Creation screen"): the BUILDING_PART the editor spawns itself, in no BUILDING; one ref for WorldBuildings::setEditor
		if (const kdata::Record* platform = gd.find("5370-platform_otto.mod"))
		{
			std::vector<uint32_t> chosen;
			rs.addPart(*platform, *platform, 0, chosen);
			if (!chosen.empty()) { set.editorPlatformRef = (int)set.partRefs.size(); set.partRefs.push_back(chosen[0]); }
		}
		const size_t buildingInstances = set.instances.size() - set.furnitureInstances - set.doorInstances;
		if (features) { addFeatures(set, gd, terrain, dataDir); }
		char buf[640];
		snprintf(buf, sizeof buf, "%zu placements from kworld (layer-merged), %zu missing target, %zu without mesh parts; %zu instances in %zu cells, %zu distinct parts;"
			" part uses by shader DEFAULT %zu ALPHA %zu FOLIAGE %zu DUAL %zu EMISSIVE %zu;"
			" features: %zu placements, %zu instances (of %zu total), %zu hidden markers, %zu missing records, modes UV %zu TRIPLANAR %zu TERRAIN %zu DUAL %zu FOLIAGE %zu DUAL_TRI %zu EMISSIVE %zu;"
			" %zu road foliage points (no record, not drawn)",
			set.placedRaw, set.missingTarget, set.noGeometry, buildingInstances, set.cells.size(), set.parts.size(),
			set.typeCount[0], set.typeCount[1], set.typeCount[2], set.typeCount[3], set.typeCount[4],
			set.featurePlacements, set.featureInstances, set.instances.size(), set.featureHidden, set.featureMissing,
			set.featureModes[0], set.featureModes[1], set.featureModes[2], set.featureModes[3], set.featureModes[4], set.featureModes[5], set.featureModes[6],
			set.roadFoliage);
		set.report = buf;
		snprintf(buf, sizeof buf, "; interiors: %zu interior/mask part uses, %zu buildings with floors, %zu layout placements (%zu joined, %zu without a collection), %zu exterior layouts (%zu joined), %zu ruined placements, %zu furniture instances (%zu without mesh parts), %zu door leaves, layouts and layout names read in %.0f ms",
			set.interiorPartUses, set.floorable.size(), set.layoutPlacements, set.layoutJoined, set.layoutMissing, set.exteriorPlacements, set.exteriorJoined, set.ruinedPlacements, set.furnitureInstances, set.furnitureNoGeometry, set.doorInstances, set.interiorLoadMs);
		set.report += buf;
	}
	catch (const std::exception& e) { set = BuildingSet(); set.report = std::string("building load failed: ") + e.what(); }
	set.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return set;
}
