#include "fogvolume.hpp"
#include "drawstats.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "fogvolume.glsl.h"

namespace
{

constexpr float kSphereMesh = 0.48f * 2.0f;	// fog_sphere_vs: the shader radius is 0.48 x node scale, the node scale is 2 x the file radius
constexpr float kBeamMesh = 0.4f * 2.0f;	// fog_beam_vs: 0.4 x scale
constexpr float kPlaneFloor = -2000.0f;		// the prisms have no floor plane; for the visibility bound only

float rd(const unsigned char* p) { float v; std::memcpy(&v, p, 4); return v; }

// Vertices of the prism for the bounding sphere: every triple of the seven planes plus a floor, kept when inside all of them.
void planeBound(FogVolumes::Volume& v)
{
	float pl[8][4];
	std::memcpy(pl, v.planes, sizeof v.planes);
	pl[7][0] = 0; pl[7][1] = -1; pl[7][2] = 0; pl[7][3] = -kPlaneFloor;	// y >= floor
	std::vector<std::array<double, 3>> pts;
	for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++) for (int k = j + 1; k < 8; k++)
	{
		// n.p = -d for each plane (the file's convention); solve by Cramer's rule
		const double a[3][3] = { { pl[i][0], pl[i][1], pl[i][2] }, { pl[j][0], pl[j][1], pl[j][2] }, { pl[k][0], pl[k][1], pl[k][2] } };
		const double w[3] = { i == 7 ? pl[i][3] : -pl[i][3], j == 7 ? pl[j][3] : -pl[j][3], k == 7 ? pl[k][3] : -pl[k][3] };
		auto det = [](const double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
		const double D = det(a);
		if (std::fabs(D) < 1e-9) { continue; }
		double p[3];
		for (int c = 0; c < 3; c++)
		{
			double m[3][3];
			std::memcpy(m, a, sizeof m);
			for (int r = 0; r < 3; r++) { m[r][c] = w[r]; }
			p[c] = det(m) / D;
		}
		bool in = true;
		for (int q = 0; q < 8 && in; q++) { in = pl[q][0] * p[0] + pl[q][1] * p[1] + pl[q][2] * p[2] <= (q == 7 ? pl[q][3] : -pl[q][3]) + 1.0; }
		if (in) { pts.push_back({ p[0], p[1], p[2] }); }
	}
	if (pts.empty()) { return; }
	double c[3] = {};
	for (const auto& p : pts) for (int a = 0; a < 3; a++) { c[a] += p[a] / pts.size(); }
	double r = 0;
	for (const auto& p : pts) { r = std::max(r, std::hypot(std::hypot(p[0] - c[0], p[1] - c[1]), p[2] - c[2])); }
	for (int a = 0; a < 3; a++) { v.bound[a] = c[a]; }
	v.bound[3] = r;
}

}

bool FogVolumes::load(const std::string& dataDir)
{
	const std::string path = dataDir + "/newland/land/fogfeatures.dat";
	std::ifstream in(path, std::ios::binary);
	const std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (b.size() < 8 || std::memcmp(b.data(), "FF02", 4) != 0)
	{
		fprintf(stderr, "kclient: fog volumes: cannot read %s\n", path.c_str());
		return false;
	}
	int n;
	std::memcpy(&n, &b[4], 4);
	size_t p = 8;
	static const int extra[5] = { 0, 0, 4, 8, 29 };
	for (int i = 0; i < n; i++)
	{
		Volume v;
		if (p >= b.size()) { break; }
		const int len = b[p];
		if (p + 1 + len + 22 > b.size()) { break; }
		v.name.assign((const char*)&b[p + 1], len);
		p += 1 + len;
		v.type = b[p];
		if (v.type < 2 || v.type > 4 || p + 21 + extra[v.type] * 4 > b.size()) { fprintf(stderr, "kclient: fog volumes: record %d (%s) has type %d, stopping\n", i, v.name.c_str(), v.type); break; }
		for (int k = 0; k < 3; k++) { v.rgb[k] = rd(&b[p + 1 + 4 * k]); }
		v.alpha = rd(&b[p + 13]);
		v.distance = rd(&b[p + 17]);
		p += 21;
		float e[29];
		for (int k = 0; k < extra[v.type]; k++) { e[k] = rd(&b[p + 4 * k]); }
		p += 4 * extra[v.type];
		if (v.type == 2)
		{
			std::memcpy(v.p1, e, 12);
			v.radius = e[3];
			for (int k = 0; k < 3; k++) { v.bound[k] = e[k]; }
			v.bound[3] = v.radius;
		} else if (v.type == 3) {
			std::memcpy(v.p1, e, 12);
			std::memcpy(v.p2, e + 3, 12);
			v.radius = e[6];
			v.blur = e[7];
			for (int k = 0; k < 3; k++) { v.bound[k] = 0.5 * (v.p1[k] + v.p2[k]); }
			v.bound[3] = 0.5 * std::sqrt((v.p2[0] - v.p1[0]) * (v.p2[0] - v.p1[0]) + (v.p2[1] - v.p1[1]) * (v.p2[1] - v.p1[1]) + (v.p2[2] - v.p1[2]) * (v.p2[2] - v.p1[2])) + v.radius;
		} else {
			v.blur = e[0];
			std::memcpy(v.planes, e + 1, sizeof v.planes);
			planeBound(v);
		}
		vols_.push_back(v);
	}
	printf("fog volumes: %zu from fogfeatures.dat\n", vols_.size());
	return !vols_.empty();
}

void FogVolumes::init()
{
	shd_ = sg_make_shader(fogvolume_shader_desc(sg_query_backend()));
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.color_count = 1;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
	pd.colors[0].blend.enabled = true;
	pd.colors[0].blend.src_factor_rgb = pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;	// premultiplied: the shader writes colour x alpha
	pd.colors[0].blend.dst_factor_rgb = pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.label = "fogvolume";
	pip_ = sg_make_pipeline(&pd);
}

void FogVolumes::shutdown()
{
	sg_destroy_pipeline(pip_);
	sg_destroy_sampler(smp_);
	sg_destroy_shader(shd_);
}

int FogVolumes::render(const Camera& c, const LightParams& light, float sunY, sg_view depth)
{
	drawn_ = 0;
	if (!enabled || vols_.empty()) { return 0; }
	// visible = bounding sphere against the four side planes and the near plane; far to near by distance to the centre
	const double tx = c.tanY * c.aspect, ty = c.tanY;
	const double sides[4][3] = { { 1, 0, -tx }, { -1, 0, -tx }, { 0, 1, -ty }, { 0, -1, -ty } };	// outward normals in (r, u, f) components
	struct Item { const Volume* v; double dist; int box[4]; };
	std::vector<Item> items;
	for (const Volume& v : vols_)
	{
		const double rel[3] = { v.bound[0] - c.pos[0], v.bound[1] - c.pos[1], v.bound[2] - c.pos[2] };
		const double comp[3] = { rel[0] * c.r[0] + rel[1] * c.r[1] + rel[2] * c.r[2], rel[0] * c.u[0] + rel[1] * c.u[1] + rel[2] * c.u[2], rel[0] * c.f[0] + rel[1] * c.f[1] + rel[2] * c.f[2] };
		bool out = comp[2] < -v.bound[3];
		for (int s = 0; s < 4 && !out; s++)
		{
			const double l = std::sqrt(sides[s][0] * sides[s][0] + sides[s][1] * sides[s][1] + sides[s][2] * sides[s][2]);
			out = (sides[s][0] * comp[0] + sides[s][1] * comp[1] + sides[s][2] * comp[2]) / l > v.bound[3];
		}
		if (out) { continue; }
		// screen box of the bounding sphere: x_ndc = tan(azimuth) / tx, y_ndc = tan(elevation) / ty, each from the sphere's silhouette in that plane (full screen when the camera is inside)
		double lo[2] = { -1, -1 }, hi[2] = { 1, 1 };
		const double R = v.bound[3], t[2] = { tx, ty };
		for (int a = 0; a < 2; a++)
		{
			const double dh = std::hypot(comp[a], comp[2]);
			if (dh <= R) { continue; }
			const double az = std::atan2(comp[a], comp[2]), al = std::asin(R / dh);
			if (az - al > -1.5 && az + al < 1.5) { lo[a] = std::fmax(-1.0, std::tan(az - al) / t[a]); hi[a] = std::fmin(1.0, std::tan(az + al) / t[a]); }
			else if (az - al > -1.5) { hi[a] = 1; lo[a] = std::fmax(-1.0, std::tan(az - al) / t[a]); }
			else if (az + al < 1.5) { lo[a] = -1; hi[a] = std::fmin(1.0, std::tan(az + al) / t[a]); }
		}
		if (lo[0] >= hi[0] || lo[1] >= hi[1]) { continue; }
		Item it = { &v, std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]), {} };
		it.box[0] = (int)std::floor((lo[0] + 1) * 0.5 * c.width);
		it.box[1] = (int)std::floor((lo[1] + 1) * 0.5 * c.height);
		it.box[2] = std::min(c.width, (int)std::ceil((hi[0] + 1) * 0.5 * c.width) + 1) - it.box[0];
		it.box[3] = std::min(c.height, (int)std::ceil((hi[1] + 1) * 0.5 * c.height) + 1) - it.box[1];
		items.push_back(it);
	}
	std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist > b.dist; });
	if (items.empty()) { return 0; }

	drawstats::pipeline(pip_);
	sg_bindings bd = {};
	bd.views[VIEW_depth_tex] = depth;
	bd.samplers[SMP_depth_smp] = smp_;
	drawstats::bindings(&bd);
	const float brighten = 1.6f * std::fmin(std::fmax(sunY * 3.0f + 0.2f, 0.0f), 1.0f);
	for (const Item& it : items)
	{
		const Volume& v = *it.v;
		fog_params_t p = {};
		for (int k = 0; k < 3; k++)
		{
			p.cam_f[k] = (float)c.f[k];
			p.cam_r[k] = (float)(c.r[k] * c.tanY * c.aspect);
			p.cam_u[k] = (float)(c.u[k] * c.tanY);
			p.wpos[k] = (float)c.pos[k];
			p.atm[k] = light.v[4][k];
		}
		p.zinfo[0] = c.nearZ;
		p.zinfo[1] = c.farZ;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
		p.zinfo[2] = 1;
#endif
		p.zinfo[3] = (float)v.type;
		// the light: sphere and beam multiply by sunColour.w; the planes by max(0.08, sunColour.w x brighten) (fog.hlsl:199)
		const float lightMul = (v.type == 4 ? std::fmax(0.08f, light.v[1][3] * brighten) : light.v[1][3]) * light.v[4][3];
		for (int k = 0; k < 3; k++) { p.col[k] = v.rgb[k] * lightMul; }
		p.col[3] = v.alpha;
		p.prm[0] = 1.0f / v.distance;
		p.prm[1] = v.blur > 0 ? 1.0f / v.blur : 1000.0f;
		p.prm[2] = light.v[5][0];
		p.prm[3] = light.v[5][1];
		if (v.type == 2)
		{
			for (int k = 0; k < 3; k++) { p.pa[k] = (float)(v.p1[k] - c.pos[k]); }
			p.pa[3] = v.radius * kSphereMesh;
		} else if (v.type == 3) {
			float ax[3], h = 0;
			for (int k = 0; k < 3; k++) { ax[k] = v.p2[k] - v.p1[k]; h += ax[k] * ax[k]; }
			h = std::sqrt(h);
			for (int k = 0; k < 3; k++)
			{
				p.pa[k] = (float)(v.p1[k] - c.pos[k]);
				p.pb[k] = ax[k] / h;
			}
			p.pa[3] = v.radius * kBeamMesh;
			p.pb[3] = h;
		} else {
			for (int i = 0; i < 7; i++)
			{
				const float* pl = v.planes[i];
				// the file stores n.p = -d; relative to the camera: n.p' <= -d - n.cam
				const double w = -pl[3] - (pl[0] * c.pos[0] + pl[1] * c.pos[1] + pl[2] * c.pos[2]);
				p.pl[i][0] = pl[0]; p.pl[i][1] = pl[1]; p.pl[i][2] = pl[2]; p.pl[i][3] = (float)w;
			}
		}
		sg_apply_scissor_rect(it.box[0], it.box[1], it.box[2], it.box[3], false);
		sg_apply_uniforms(UB_fog_params, SG_RANGE(p));
		drawstats::draw(0, 3, 1);
		drawn_++;
	}
	return drawn_;
}
