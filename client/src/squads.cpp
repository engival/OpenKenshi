#include "squads.hpp"
#include "drawstats.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <utility>

#include "squads.glsl.h"

namespace
{

struct Colour { const char* source; float r, g, b; };
const Colour kPalette[] = {
	{ "town resident", 0.25f, 0.55f, 1.0f },
	{ "town default resident", 0.35f, 0.85f, 0.95f },
	{ "town bar squad", 1.0f, 0.85f, 0.2f },
	{ "town roaming squad", 0.3f, 0.9f, 0.3f },
	{ "faction roaming squad", 1.0f, 0.3f, 0.3f },
	{ "biome nest town", 0.85f, 0.4f, 1.0f },
	{ "biome homeless spawn", 1.0f, 0.55f, 0.15f },
};
const Colour kOther = { "other", 0.8f, 0.8f, 0.8f };

// Reads only ksim_run's snapshot output (one squad object per line, flat keys), not general JSON: finds the value after `"key": `.
const char* find_value(const std::string& line, const char* key)
{
	const std::string k = std::string("\"") + key + "\": ";
	const size_t p = line.find(k);
	return p == std::string::npos ? nullptr : line.c_str() + p + k.size();
}

} // namespace

size_t Squads::load(const std::string& path, const kworld::Heightmap& hm)
{
	dots_.clear();
	sources_.clear();
	std::ifstream in(path);
	if (!in) { printf("squads: %s not found, no squads drawn\n", path.c_str()); return 0; }
	std::map<std::pair<float, float>, int> stacked;	// squads already placed at one point
	std::string line;
	while (std::getline(in, line))
	{
		const char* xs = find_value(line, "x");
		const char* zs = find_value(line, "z");
		const char* ss = find_value(line, "source");
		if (!xs || !zs || !ss || *xs == 'n' || *zs == 'n' || *ss != '"') { continue; }	// no position (wilderness) or not a squad line
		const std::string source(ss + 1, strchr(ss + 1, '"') - (ss + 1));
		Colour c = kOther;
		for (const Colour& p : kPalette) { if (source == p.source) { c = p; break; } }
		place(0, (float)atof(xs), (float)atof(zs), c.source, c.r, c.g, c.b, hm, stacked);
	}
	printf("squads: %zu shown from %s\n", dots_.size(), path.c_str());
	return dots_.size();
}

void Squads::setLive(const std::vector<Live>& squads, const kworld::Heightmap& hm)
{
	dots_.clear();
	sources_.clear();
	std::map<std::pair<float, float>, int> stacked;
	for (const Live& l : squads) { place(l.id, l.x, l.z, l.source, l.r, l.g, l.b, hm, stacked); }
}

// One pillar: squads at one point fan out on a golden-angle spiral (3 m steps); the legend counts it under `source`.
void Squads::place(uint64_t id, float x, float z, const std::string& source, float r, float g, float b, const kworld::Heightmap& hm, std::map<std::pair<float, float>, int>& stacked)
{
	const int k = stacked[{ x, z }]++;
	const double ang = k * 2.39996323, rad = 30.0 * std::sqrt((double)k);
	const float fx = x + (float)(rad * std::cos(ang)), fz = z + (float)(rad * std::sin(ang));
	Source* s = nullptr;
	for (Source& q : sources_) { if (q.name == source) { s = &q; break; } }
	if (!s)
	{
		sources_.push_back({ source, r, g, b, 0 });
		s = &sources_.back();
	}
	s->count++;
	dots_.push_back({ fx, (float)hm.height(fx, fz), fz, r, g, b, id });
}

void Squads::init()
{
	static const float verts[] = {	// unit box: xz -1..1, y 0..1
		-1, 0, -1,  1, 0, -1,  1, 0, 1,  -1, 0, 1,
		-1, 1, -1,  1, 1, -1,  1, 1, 1,  -1, 1, 1,
	};
	static const uint16_t idx[] = { 0,1,5, 0,5,4, 1,2,6, 1,6,5, 2,3,7, 2,7,6, 3,0,4, 3,4,7, 4,5,6, 4,6,7 };
	sg_buffer_desc bd = {};
	bd.data = SG_RANGE(verts);
	bd.label = "squads-box";
	box_ = sg_make_buffer(&bd);
	bd = {};
	bd.usage.index_buffer = true;
	bd.data = SG_RANGE(idx);
	bd.label = "squads-box-idx";
	boxIdx_ = sg_make_buffer(&bd);
	shd_ = sg_make_shader(squads_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.attrs[ATTR_squads_pos] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_squads_ipos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_squads_icol] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.buffers[1].stride = 32;
	pd.index_type = SG_INDEXTYPE_UINT16;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "squads";
	pip_ = sg_make_pipeline(&pd);
	if (dots_.empty()) { return; }
	std::vector<float> inst;
	inst.reserve(dots_.size() * 8);
	for (const Dot& d : dots_) { inst.insert(inst.end(), { d.x, d.y, d.z, 0.0f, d.r, d.g, d.b, 0.0f }); }
	bd = {};
	bd.data = { inst.data(), inst.size() * sizeof(float) };
	bd.label = "squads-inst";
	inst_ = sg_make_buffer(&bd);
}

void Squads::shutdown()
{
	sg_destroy_buffer(inst_);
	sg_destroy_buffer(boxIdx_);
	sg_destroy_buffer(box_);
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
}

void Squads::render(const float mvp[16], const double cam[3], int count) const
{
	if (count <= 0) { return; }
	drawstats::pipeline(pip_);
	vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	for (int i = 0; i < 3; i++) { vs.cam[i] = (float)cam[i]; }
	sg_bindings b = {};
	b.vertex_buffers[0] = box_;
	b.vertex_buffers[1] = inst_;
	b.index_buffer = boxIdx_;
	drawstats::bindings(&b);
	sg_apply_uniforms(UB_vs_params, SG_RANGE(vs));
	drawstats::draw(0, 30, count);
}
