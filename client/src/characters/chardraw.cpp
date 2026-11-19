#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "characters/characters.hpp"
#include "app/cpustage.hpp"
#include "dev/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
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
#include "world/weathermap.hpp"

#include "character.glsl.h"
#include "characters/characters_internal.hpp"

namespace
{

// render.char_instancing (kenshi-k16.2.205.8): the parts of every character grouped by (mesh, material) and drawn instanced (one draw per group, per pass) from one
// palette buffer per frame; 0 = one draw per part in character order from the same buffers (the old draw count, for the A/B)
const cfg::Bool kCharInstancing{ cfg::Group{ "Performance" }, "render.char_instancing", true, "Characters: draw the parts grouped by mesh and material, instanced, from one palette buffer per frame (off: one draw per part per character, the old draw count; for comparisons)" };

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

