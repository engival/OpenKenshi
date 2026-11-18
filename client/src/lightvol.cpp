#include "lightvol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace lightvol
{
namespace
{
Binding g_live, g_none;	// what the lit programs bind now; the empty one

// The sun grid page A holds: the latest bake, else the one it replaced (page B is the same then: the fade is a no-op).
const lightbake::Grid* sunGrid(const Volume& v) { return v.sunNew ? v.sunNew.get() : v.sunOld.get(); }

uint16_t half(float f)
{
	uint32_t x;
	std::memcpy(&x, &f, 4);
	const uint32_t sign = (x >> 16) & 0x8000u;
	int e = (int)((x >> 23) & 0xff) - 127 + 15;
	uint32_t m = x & 0x7fffffu;
	if (e >= 31) { return (uint16_t)(sign | 0x7bffu); }	// clamp to the largest finite half
	if (e <= 0)
	{
		if (e < -10) { return (uint16_t)sign; }
		m |= 0x800000u;
		const int shift = 14 - e;
		return (uint16_t)(sign | ((m >> shift) + ((m >> (shift - 1)) & 1u)));
	}
	return (uint16_t)(sign | ((uint32_t)e << 10) | ((m + 0x1000u) >> 13));	// round to nearest
}
}

const Binding& binding() { return g_live; }

void Volumes::init()
{
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = sd.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);
	const uint16_t zero[4] = {};
	sg_image_desc id = {};
	id.type = SG_IMAGETYPE_3D;
	id.width = id.height = id.num_slices = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.data = SG_RANGE(zero);
	id.label = "lightvol-none";
	noneImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = noneImg_;
	noneView_ = sg_make_view(&vd);
	const uint8_t seen[8] = { 255, 255, 255, 255, 255, 255, 255, 255 };	// 2x1x1: the empty lamp volume reads 1 (never bound to a light: the code is 0 outside a volume)
	sg_image_desc ld = {};
	ld.type = SG_IMAGETYPE_3D;
	ld.width = 2;
	ld.height = ld.num_slices = 1;
	ld.pixel_format = SG_PIXELFORMAT_RGBA8;
	ld.data = SG_RANGE(seen);
	ld.label = "lightvol-lamp-none";
	noneLampImg_ = sg_make_image(&ld);
	vd = {};
	vd.texture.image = noneLampImg_;
	noneLampView_ = sg_make_view(&vd);
	g_none.sh = g_none.use = noneView_;
	g_none.lamp = noneLampView_;
	for (int c = 0; c < 3; c++) { g_none.sun[c] = noneView_; }
	g_none.smp = smp_;
	g_live = g_none;
}

void Volumes::destroyAtlas()
{
	if (shView_.id) { sg_destroy_view(shView_); }
	if (useView_.id) { sg_destroy_view(useView_); }
	if (lampView_.id) { sg_destroy_view(lampView_); }
	if (shImg_.id) { sg_destroy_image(shImg_); }
	if (useImg_.id) { sg_destroy_image(useImg_); }
	if (lampImg_.id) { sg_destroy_image(lampImg_); }
	for (int c = 0; c < 3; c++)
	{
		if (sunView_[c].id) { sg_destroy_view(sunView_[c]); }
		if (sunImg_[c].id) { sg_destroy_image(sunImg_[c]); }
		sunView_[c] = {};
		sunImg_[c] = {};
	}
	shView_ = useView_ = lampView_ = {};
	shImg_ = useImg_ = lampImg_ = {};
}

void Volumes::shutdown()
{
	destroyAtlas();
	if (noneView_.id) { sg_destroy_view(noneView_); }
	if (noneImg_.id) { sg_destroy_image(noneImg_); }
	if (noneLampView_.id) { sg_destroy_view(noneLampView_); }
	if (noneLampImg_.id) { sg_destroy_image(noneLampImg_); }
	noneLampView_ = {};
	noneLampImg_ = {};
	if (smp_.id) { sg_destroy_sampler(smp_); }
	noneView_ = {};
	noneImg_ = {};
	smp_ = {};
	g_live = g_none = {};
	slots_.clear();
	n_ = 0;
}

// The atlas: the volumes side by side along x (texel centres of one volume never filter into the next: the shader clamps to a volume's own probes). sh is stored times usable
// (an unusable probe holds zero), usable beside it, so the shader's filtered sh / filtered usable averages the usable probes only.
// The sun atlas has the same layout twice along x: page A = the latest sun bake, page B = the one it replaces (or a copy, when there is none), mixed by the volume's fade.
void Volumes::build(const std::vector<Volume>& vols)
{
	destroyAtlas();
	int w = 0, h = 1, d = 1;
	std::vector<int> off;
	for (const Volume& v : vols)
	{
		off.push_back(w);
		w += v.grid->dim[0];
		h = std::max(h, v.grid->dim[1]);
		d = std::max(d, v.grid->dim[2]);
	}
	atlas_[0] = std::max(w, 1); atlas_[1] = h; atlas_[2] = d;
	std::vector<uint16_t> sh((size_t)atlas_[0] * h * d * 4, 0), use((size_t)atlas_[0] * h * d * 2, 0);
	// the sun volumes (one page): only the ones whose grid is the same shape as their sky grid (the shader reuses the sky's probe index)
	std::vector<int> sunOff(vols.size(), -1);
	std::vector<const Volume*> sunVols;
	for (size_t k = 0; k < vols.size(); k++)
	{
		const lightbake::Grid* sg = sunGrid(vols[k]);
		if (!sg || !sg->sunValid()) { continue; }
		if (sg->dim[0] != vols[k].grid->dim[0] || sg->dim[1] != vols[k].grid->dim[1] || sg->dim[2] != vols[k].grid->dim[2]) { continue; }
		sunOff[k] = 0;
		sunVols.push_back(&vols[k]);
	}
	int sunW = 0;
	for (const Volume* v : sunVols) { sunW += (*sunGrid(*v)).dim[0]; }
	sunAtlas_[0] = std::max(sunW, 1); sunAtlas_[1] = 1; sunAtlas_[2] = 1;
	for (const Volume* v : sunVols) { sunAtlas_[1] = std::max(sunAtlas_[1], (*sunGrid(*v)).dim[1]); sunAtlas_[2] = std::max(sunAtlas_[2], (*sunGrid(*v)).dim[2]); }
	// sunW back to the running offset below
	int sunCur = 0;
	for (size_t k = 0; k < vols.size(); k++)
	{
		if (sunOff[k] >= 0) { sunOff[k] = sunCur; sunCur += vols[k].grid->dim[0]; }
		else { sunOff[k] = 0; }
	}
	std::vector<uint16_t> sun[3];
	const size_t sunTexels = (size_t)(2 * sunAtlas_[0]) * sunAtlas_[1] * sunAtlas_[2];
	for (int c = 0; c < 3; c++) { sun[c].assign(sunTexels * 4, 0); }
	for (size_t k = 0; k < vols.size(); k++)
	{
		const lightbake::Grid& g = *vols[k].grid;
		for (int z = 0; z < g.dim[2]; z++)
		{
			for (int y = 0; y < g.dim[1]; y++)
			{
				for (int x = 0; x < g.dim[0]; x++)
				{
					const size_t i = ((size_t)z * g.dim[1] + y) * g.dim[0] + x;	// Grid: x fastest, then y, then z
					const size_t t = ((size_t)z * h + y) * atlas_[0] + off[k] + x;
					if (!g.usable(i)) { continue; }
					for (int c = 0; c < 4; c++) { sh[t * 4 + c] = half(g.sh[i * 4 + c]); }
					use[t * 2] = half(1.0f);
					use[t * 2 + 1] = half(g.enclosure(i));	// times usable (1): the sun shafts' indoor density
				}
			}
		}
		const lightbake::Grid* newG = sunOff[k] >= 0 ? sunGrid(vols[k]) : nullptr;
		if (!newG || !newG->sunValid()) { continue; }
		const lightbake::Grid* oldG = vols[k].sunOld && vols[k].sunOld->sunValid() && vols[k].sunOld->count() == newG->count() ? vols[k].sunOld.get() : nullptr;
		for (int z = 0; z < g.dim[2]; z++)
		{
			for (int y = 0; y < g.dim[1]; y++)
			{
				for (int x = 0; x < g.dim[0]; x++)
				{
					const size_t i = ((size_t)z * g.dim[1] + y) * g.dim[0] + x;
					if (!g.usable(i)) { continue; }
					const size_t a = ((size_t)z * sunAtlas_[1] + y) * (size_t)(2 * sunAtlas_[0]) + sunOff[k] + x;
					const size_t b = a + (size_t)sunAtlas_[0];
					for (int c = 0; c < 3; c++)
					{
						for (int j = 0; j < 4; j++)
						{
							sun[c][a * 4 + j] = half(newG->sun[i * 12 + c * 4 + j]);
							sun[c][b * 4 + j] = oldG ? half(oldG->sun[i * 12 + c * 4 + j]) : sun[c][a * 4 + j];
						}
					}
				}
			}
		}
	}
	// the lamp visibility, lamps 0..3 of every probe in the left half and 4..7 in the right (a volume with none traced stays 0; its lights carry no code)
	std::vector<uint8_t> lamp((size_t)atlas_[0] * 2 * h * d * 4, 0);
	for (size_t k = 0; k < vols.size(); k++)
	{
		if (!vols[k].lamps) { continue; }
		const lightbake::Grid& g = *vols[k].grid;
		const std::vector<uint8_t>& src = *vols[k].lamps;
		if (src.size() != g.count() * lightbake::kMaxLamps) { continue; }
		for (int z = 0; z < g.dim[2]; z++)
		{
			for (int y = 0; y < g.dim[1]; y++)
			{
				for (int x = 0; x < g.dim[0]; x++)
				{
					const size_t i = ((size_t)z * g.dim[1] + y) * g.dim[0] + x;
					const size_t t = ((size_t)z * h + y) * atlas_[0] * 2 + off[k] + x;
					for (int c = 0; c < 4; c++) { lamp[t * 4 + c] = src[i * lightbake::kMaxLamps + c]; lamp[(t + atlas_[0]) * 4 + c] = src[i * lightbake::kMaxLamps + 4 + c]; }
				}
			}
		}
	}
	// unusable probes hold zero in the sun pages too: the shader divides by the sky's filtered usable, and a wall probe's transfer must not bleed in
	// (it is zero in the bake already: runSun leaves unusable probes at 0)
	sg_image_desc id = {};
	id.type = SG_IMAGETYPE_3D;
	id.width = atlas_[0];
	id.height = atlas_[1];
	id.num_slices = atlas_[2];
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.data = { sh.data(), sh.size() * sizeof(uint16_t) };
	id.label = "lightvol-sh";
	shImg_ = sg_make_image(&id);
	id.pixel_format = SG_PIXELFORMAT_RG16F;
	id.data = { use.data(), use.size() * sizeof(uint16_t) };
	id.label = "lightvol-use";
	useImg_ = sg_make_image(&id);
	id.width = atlas_[0] * 2;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = { lamp.data(), lamp.size() };
	id.label = "lightvol-lamp";
	lampImg_ = sg_make_image(&id);
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.width = 2 * sunAtlas_[0];
	id.height = sunAtlas_[1];
	id.num_slices = sunAtlas_[2];
	static const char* const kSunLabels[3] = { "lightvol-sun-r", "lightvol-sun-g", "lightvol-sun-b" };
	for (int c = 0; c < 3; c++)
	{
		id.data = { sun[c].data(), sun[c].size() * sizeof(uint16_t) };
		id.label = kSunLabels[c];
		sunImg_[c] = sg_make_image(&id);
	}
	sg_view_desc vd = {};
	vd.texture.image = lampImg_;
	lampView_ = sg_make_view(&vd);
	vd.texture.image = shImg_;
	shView_ = sg_make_view(&vd);
	vd.texture.image = useImg_;
	useView_ = sg_make_view(&vd);
	for (int c = 0; c < 3; c++)
	{
		vd.texture.image = sunImg_[c];
		sunView_[c] = sg_make_view(&vd);
	}
	// the blend of a volume that stays across a rebuild carries over (and its sun fade, when the pair of bakes did not change)
	std::vector<Slot> old = std::move(slots_);
	slots_.clear();
	for (size_t k = 0; k < vols.size(); k++)
	{
		Slot s = { vols[k].key, vols[k].grid.get(), vols[k].sunNew.get(), vols[k].sunOld.get(), off[k], sunOff[k], 0.0f, 0.0f, vols[k].lamps.get(), vols[k].lampCount };
		for (const Slot& o : old)
		{
			if (o.key != s.key || o.grid != s.grid) { continue; }
			s.blend = o.blend;
			if (o.sunNew == s.sunNew && o.sunOld == s.sunOld) { s.fade = o.fade; }
		}
		slots_.push_back(s);
	}
}

void Volumes::begin(const double cam[3], const std::vector<Volume>& in, float strength, float sunStrength, float groundAlbedo, float dt, bool keep)
{
	std::vector<Volume> vols;
	for (const Volume& v : in) { if (v.grid && v.grid->count() > 0 && (int)vols.size() < kMax) { vols.push_back(v); } }
	if ((strength <= 0.0f && !keep) || vols.empty())
	{
		n_ = 0;
		slots_.clear();
		g_live = g_none;
		return;
	}
	bool same = vols.size() == slots_.size();
	for (size_t k = 0; same && k < vols.size(); k++)
	{
		same = slots_[k].key == vols[k].key && slots_[k].grid == vols[k].grid.get() && slots_[k].sunNew == vols[k].sunNew.get() && slots_[k].sunOld == vols[k].sunOld.get() && slots_[k].lamps == vols[k].lamps.get();
	}
	if (!same) { build(vols); }
	n_ = vols.size();
	Binding& b = g_live;
	std::memset(b.params, 0, sizeof b.params);
	b.sh = shView_;
	b.use = useView_;
	b.lamp = lampView_;
	for (int c = 0; c < 3; c++) { b.sun[c] = sunView_[c].id ? sunView_[c] : noneView_; }
	b.smp = smp_;
	float* p = b.params;
	p[0] = (float)n_;
	p[1] = std::clamp(strength, 0.0f, 1.0f);
	p[2] = groundAlbedo;
	p[3] = std::clamp(sunStrength, 0.0f, 1.0f);
	p[4] = (float)atlas_[0]; p[5] = (float)atlas_[1]; p[6] = (float)atlas_[2];
	p[7] = (float)sunAtlas_[0];
	for (size_t k = 0; k < n_; k++)
	{
		Slot& s = slots_[k];
		s.blend = std::min(1.0f, s.blend + dt);	// about a second
		s.fade = s.sunNew && s.sunOld && s.sunNew != s.sunOld && s.sunOffset >= 0 ? std::min(1.0f, s.fade + dt) : 1.0f;	// the re-bake's cross-fade, about a second
		const Volume& v = vols[k];
		const lightbake::Grid& g = *v.grid;
		// R(q) e_k, the columns of the building -> world rotation; the rows of its transpose take a world vector into the building
		const double w = v.q[0], x = v.q[1], y = v.q[2], z = v.q[3];
		const double R[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y) },
								  { 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x) },
								  { 2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) } };
		const double d[3] = { cam[0] - v.origin[0], cam[1] - v.origin[1], cam[2] - v.origin[2] };
		float* m = p + 8 + k * 28;
		for (int i = 0; i < 3; i++)
		{
			const double rt[3] = { R[0][i], R[1][i], R[2][i] };	// the row i of R^T
			const double k1 = 1.0 / ((double)v.scale[i] * g.spacing);
			m[i * 4 + 0] = (float)(rt[0] * k1); m[i * 4 + 1] = (float)(rt[1] * k1); m[i * 4 + 2] = (float)(rt[2] * k1);
			m[i * 4 + 3] = (float)((rt[0] * d[0] + rt[1] * d[1] + rt[2] * d[2]) * k1 - g.min[i] / g.spacing);
			m[12 + i * 4 + 0] = (float)rt[0]; m[12 + i * 4 + 1] = (float)rt[1]; m[12 + i * 4 + 2] = (float)rt[2];
			m[12 + i * 4 + 3] = (float)g.dim[i];
		}
		m[24] = (float)s.offset;
		m[25] = s.blend;
		m[26] = s.fade;					// render.sun_bounce: 1 = the latest bake, ramping up from 0 as it replaces sunOld's page
		m[27] = s.sunOffset >= 0 ? 1.0f : 0.0f;	// this volume has a sun term packed
	}
}

int Volumes::lampCode(uint32_t key, int lamp) const
{
	for (size_t k = 0; k < n_; k++)
	{
		if (slots_[k].key == key) { return slots_[k].lamps && lamp >= 0 && lamp < slots_[k].lampCount ? 1 + (int)k * 8 + lamp : 0; }
	}
	return 0;
}

void Volumes::end()
{
	n_ = 0;
	g_live = g_none;
}
}
