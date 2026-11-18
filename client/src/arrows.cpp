#include "arrows.hpp"
#include "drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "arrow.glsl.h"
#include "ogremesh.hpp"

// One arrow.mesh, its triangles in buffers; a few submeshes at most.
bool Arrows::buildParts(const std::string& meshPath, std::string& err)
{
	ogre::Mesh mesh;
	if (!ogre::load(meshPath, mesh, err)) { return false; }
	if (mesh.submeshes.empty()) { err = meshPath + ": no submeshes"; return false; }
	const auto upload = [](const std::vector<float>& v, sg_buffer& out)
	{
		sg_buffer_desc bd = {};
		bd.data = { v.data(), v.size() * sizeof(float) };
		bd.label = "arrow-verts";
		out = sg_make_buffer(&bd);
	};
	const auto uploadIdx = [](const std::vector<uint32_t>& v, sg_buffer& out)
	{
		sg_buffer_desc bd = {};
		bd.usage.index_buffer = true;
		bd.data = { v.data(), v.size() * sizeof(uint32_t) };
		bd.label = "arrow-idx";
		out = sg_make_buffer(&bd);
	};
	if (mesh.hasShared)
	{
		std::vector<float> pos;
		pos.reserve(mesh.shared.pos.size());
		for (size_t i = 0; i + 2 < mesh.shared.pos.size(); i += 3) { pos.insert(pos.end(), { mesh.shared.pos[i], mesh.shared.pos[i + 1], mesh.shared.pos[i + 2] }); }
		upload(pos, sharedVb_);
	}
	for (const ogre::Submesh& s : mesh.submeshes)
	{
		Part p;
		const std::vector<uint32_t> idx = s.triangleList();
		if (idx.empty()) { continue; }
		if (!s.shared) { upload(mesh.geometry(s).pos, p.vb); }
		uploadIdx(idx, p.ib);
		p.indexCount = (int)idx.size();
		parts_.push_back(p);
	}
	if (parts_.empty()) { err = meshPath + ": no triangles"; return false; }
	return true;
}

bool Arrows::init(const std::string& meshPath, std::string& err)
{
	path_ = meshPath;
	if (!buildParts(meshPath, err)) { return false; }
	shd_ = sg_make_shader(arrow_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.buffers[1].stride = 48;
	pd.layout.attrs[ATTR_arrow_pos] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_arrow_i_origin_quat] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_arrow_i_quat] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_arrow_i_colour] = { .buffer_index = 1, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "stealth-arrows";
	pip_ = sg_make_pipeline(&pd);
	sg_buffer_desc bd = {};
	bd.usage.vertex_buffer = true;
	bd.usage.write_transient = true;
	bd.size = (size_t)kMaxMarks * 12 * sizeof(float);
	bd.label = "stealth-arrows-inst";
	inst_ = sg_make_buffer(&bd);
	printf("arrows: %s, %zu part(s)\n", path_.c_str(), parts_.size());
	return true;
}

void Arrows::shutdown()
{
	for (Part& p : parts_)
	{
		sg_destroy_buffer(p.ib);
		sg_destroy_buffer(p.vb);
	}
	parts_.clear();
	sg_destroy_buffer(sharedVb_);
	sg_destroy_buffer(inst_);
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
}

// The shortest-arc rotation taking a onto b (Ogre Quaternion::getRotationTo with the two unit vectors): [0,0,0,1] for parallel, [0,1,0,0] for opposite.
static void rotation_to(const double a[3], const double b[3], float q[4])
{
	const double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	if (d >= 1.0 - 1e-9) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
	if (d <= -1.0 + 1e-9) { q[0] = 0; q[1] = 1; q[2] = 0; q[3] = 0; return; }	// 180 degrees: any perpendicular axis (Ogre picks one)
	double c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
	const double s = std::sqrt((1.0 + d) * 2.0), inv = 1.0 / s;
	q[0] = (float)(c[0] * inv);
	q[1] = (float)(c[1] * inv);
	q[2] = (float)(c[2] * inv);
	q[3] = (float)(s * 0.5);
}

void Arrows::update(const std::vector<Mark>& marks, const double cam[3])
{
	count_ = 0;
	if (!loaded() || marks.empty()) { return; }
	const size_t n = std::min(marks.size(), (size_t)kMaxMarks);	// the buffer's capacity; the sim sends a handful
	std::vector<float> inst;
	inst.reserve(n * 12);
	static const double kAxis[3] = { -1.0, 0.0, 0.0 };	// the mesh's -X end is the head
	for (size_t i = 0; i < n; i++)
	{
		const Mark& m = marks[i];
		float q[4];
		rotation_to(kAxis, m.dir, q);
		inst.insert(inst.end(), { (float)(m.pos[0] - cam[0]), (float)(m.pos[1] - cam[1]), (float)(m.pos[2] - cam[2]), q[3],
			q[0], q[1], q[2], 0.0f, m.colour[0], m.colour[1], m.colour[2], 0.0f });
	}
	sg_write_buffer_desc wd = {};
	wd.src.data = { inst.data(), inst.size() * sizeof(float) };
	wd.dst.buffer = inst_;
	sg_write_buffer_transient(&wd);
	count_ = (int)n;
}

void Arrows::render(const float mvp[16])
{
	if (!loaded() || count_ == 0) { return; }
	drawstats::pipeline(pip_);
	arrow_vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	sg_apply_uniforms(UB_arrow_vs_params, SG_RANGE(vs));
	sg_bindings b = {};
	b.vertex_buffers[1] = inst_;
	for (const Part& p : parts_)
	{
		b.vertex_buffers[0] = p.vb.id ? p.vb : sharedVb_;
		b.index_buffer = p.ib;
		drawstats::bindings(&b);
		drawstats::draw(0, p.indexCount, count_);
	}
}
