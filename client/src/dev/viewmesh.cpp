// The mesh viewer (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7): --view-mesh (with --tex / --dist) renders one .mesh
// instead of the world: its init, its own frame path (an orbit camera round the mesh) and its draw, moved off main.cpp; the MeshView class moved
// here from src/meshview.*. The main frame loop asks dev::viewFrame (true: the packet is a kView one) and the render table dev::viewDraw; the
// camera, run_script and the shared app state arrive through dev::Context, in viewFrame only: viewDraw reads the packet (its camera and the early
// block's viewPos / viewText, render hook 9, kenshi-k16.2.205.12.65), never the Context. Built only with KCLIENT_DEV_TOOLS.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "camera.hpp"
#include "dds.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "dev/viewhost.hpp"
#include "drawstats.hpp"
#include "framestats.hpp"
#include "gputimer.hpp"
#include "materials.hpp"
#include "mesh.glsl.h"
#include "ogremesh.hpp"
#include "overlaylist.hpp"
#include "readback.hpp"
#include "renderframe.hpp"
#include "resources.hpp"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "worldmap.hpp"	// begin_overlay, end_overlay, draw_text, draw_panel, text_scale

namespace fs = std::filesystem;

namespace dev
{
namespace
{

// Renders one .mesh with its diffuse textures (the --view-mesh inspection mode). Opaque only: alpha-tested materials
// (foliage) are not handled yet.
class MeshView
{
public:
	// meshArg: a record path or bare mesh name. texOverride: texture name used for every submesh when non-empty.
	bool load(const ResourceIndex& res, const MaterialLib& mats, const std::string& meshArg, const std::string& texOverride, std::string& err);
	void shutdown();

	// mvp: projection * rotation-only view; cam: camera position in model space.
	void render(const float mvp[16], const double cam[3]) const;

	std::string path;
	float boundsMin[3] = {}, boundsMax[3] = {};
	float center[3] = {}, radius = 0;		// of the vertex AABB
	std::vector<std::string> report;		// one line per submesh: material, texture and where it came from

private:
	struct Part
	{
		sg_buffer vbuf, ibuf;
		int indexCount;
		sg_view view;
	};
	sg_view textureView(const std::string& file);

	std::vector<Part> parts_;
	std::unordered_map<std::string, sg_view> views_;	// by texture file path
	std::vector<sg_image> images_;
	std::vector<sg_view> ownedViews_;
	sg_view white_ = {};
	sg_image whiteImg_ = {};
	sg_sampler smp_ = {};
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
};

// View-only guess for meshes whose material names no texture (Kenshi binds those from game data): the .dds in the mesh's own
// directory that shares the longest name prefix with the mesh, skipping normal/specular maps.
std::string guessTexture(const std::string& meshPath, const ResourceIndex& res)
{
	const fs::path mp(meshPath);
	const std::string stem = lower(mp.stem().string());
	std::string best;
	size_t bestScore = 0, candidates = 0;
	std::error_code ec;
	for (const fs::directory_entry& e : fs::directory_iterator(mp.parent_path(), ec))
	{
		if (lower(e.path().extension().string()) != ".dds") { continue; }
		const std::string s = lower(e.path().stem().string());
		bool aux = false;
		for (const char* suffix : { "_n", "_nrm", "_nml", "_normal", "_nm", "_s", "_spec", "_m" })
		{
			const size_t n = strlen(suffix);
			aux = aux || (s.size() > n && s.compare(s.size() - n, n, suffix) == 0);
		}
		if (aux) { continue; }
		candidates++;
		size_t score = 0;
		while (score < s.size() && score < stem.size() && s[score] == stem[score]) { score++; }
		if (score > bestScore || best.empty()) { best = e.path().filename().string(); bestScore = score; }
	}
	if (candidates == 1 || bestScore >= 3) { return best; }
	return res.find(stem + ".dds").empty() ? "" : stem + ".dds";
}

bool MeshView::load(const ResourceIndex& res, const MaterialLib& mats, const std::string& meshArg, const std::string& texOverride, std::string& err)
{
	path = res.locate(meshArg);
	if (path.empty()) { err = "mesh not found: " + meshArg; return false; }
	ogre::Mesh mesh;
	if (!ogre::load(path, mesh, err)) { return false; }
	if (!mesh.vertexBounds(boundsMin, boundsMax)) { err = path + ": no vertices"; return false; }
	for (int k = 0; k < 3; k++) { center[k] = (boundsMin[k] + boundsMax[k]) / 2; }
	radius = std::sqrt((boundsMax[0] - boundsMin[0]) * (boundsMax[0] - boundsMin[0]) + (boundsMax[1] - boundsMin[1]) * (boundsMax[1] - boundsMin[1])
		+ (boundsMax[2] - boundsMin[2]) * (boundsMax[2] - boundsMin[2])) / 2;

	shd_ = sg_make_shader(mesh_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.attrs[ATTR_mesh_position].format = SG_VERTEXFORMAT_FLOAT3;
	pd.layout.attrs[ATTR_mesh_normal].format = SG_VERTEXFORMAT_FLOAT3;
	pd.layout.attrs[ATTR_mesh_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "mesh";
	pip_ = sg_make_pipeline(&pd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	sd.max_anisotropy = 8;
	smp_ = sg_make_sampler(&sd);
	const uint32_t grey = 0xff9a9a9a;
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = { &grey, sizeof grey };
	whiteImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = whiteImg_;
	white_ = sg_make_view(&vd);

	const std::string guess = guessTexture(path, res);
	for (size_t si = 0; si < mesh.submeshes.size(); si++)
	{
		const ogre::Submesh& sm = mesh.submeshes[si];
		const ogre::Geometry& g = mesh.geometry(sm);
		const std::vector<uint32_t> tris = sm.triangleList();
		if (tris.empty()) { continue; }

		std::vector<float> normals = g.normal;
		if (normals.empty())	// area-weighted vertex normals
		{
			normals.assign((size_t)g.count * 3, 0.0f);
			for (size_t i = 0; i + 2 < tris.size(); i += 3)
			{
				const float* a = &g.pos[tris[i] * 3], *b = &g.pos[tris[i + 1] * 3], *c = &g.pos[tris[i + 2] * 3];
				const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				const float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
				for (int k = 0; k < 3; k++) { for (int j = 0; j < 3; j++) { normals[tris[i + k] * 3 + j] += n[j]; } }
			}
		}
		std::vector<float> verts((size_t)g.count * 8, 0.0f);
		for (uint32_t i = 0; i < g.count; i++)
		{
			memcpy(&verts[(size_t)i * 8], &g.pos[(size_t)i * 3], 12);
			memcpy(&verts[(size_t)i * 8 + 3], &normals[(size_t)i * 3], 12);
			if (!g.uv.empty()) { memcpy(&verts[(size_t)i * 8 + 6], &g.uv[(size_t)i * 2], 8); }
		}

		std::string tex, how;
		if (!texOverride.empty()) { tex = texOverride; how = "--tex"; }
		else if (const std::string d = mats.diffuse(sm.material); !MaterialLib::placeholder(d)) { tex = d; how = "material script"; }
		else { tex = guess; how = guess.empty() ? "none" : "guess: same-directory dds"; }
		const std::string file = tex.empty() ? "" : fs::exists(fs::path(path).parent_path() / tex) ? (fs::path(path).parent_path() / tex).string() : res.find(tex);
		Part p = {};
		p.view = file.empty() ? white_ : textureView(file);
		report.push_back("submesh " + std::to_string(si) + " material " + sm.material + " -> " + (tex.empty() ? "(no texture)" : tex)
			+ (!tex.empty() && file.empty() ? " NOT FOUND" : "") + " (" + how + ")");

		sg_buffer_desc bd = {};
		bd.data = { verts.data(), verts.size() * sizeof(float) };
		bd.label = "mesh-verts";
		p.vbuf = sg_make_buffer(&bd);
		bd = {};
		bd.usage.index_buffer = true;
		bd.data = { tris.data(), tris.size() * sizeof(uint32_t) };
		bd.label = "mesh-indices";
		p.ibuf = sg_make_buffer(&bd);
		p.indexCount = (int)tris.size();
		parts_.push_back(p);
	}
	return true;
}

sg_view MeshView::textureView(const std::string& file)
{
	auto it = views_.find(file);
	if (it != views_.end()) { return it->second; }
	DdsImage dds;
	std::string err;
	sg_view v = white_;
	if (!dds_load(file, dds, err)) { fprintf(stderr, "kclient: %s\n", err.c_str()); }
	else
	{
		const sg_image img = dds_make_image(dds, "mesh-texture");
		sg_view_desc vd = {};
		vd.texture.image = img;
		v = sg_make_view(&vd);
		images_.push_back(img);
		ownedViews_.push_back(v);
	}
	views_[file] = v;
	return v;
}

void MeshView::shutdown()
{
	for (const Part& p : parts_)
	{
		sg_destroy_buffer(p.vbuf);
		sg_destroy_buffer(p.ibuf);
	}
	parts_.clear();
	for (sg_view v : ownedViews_) { sg_destroy_view(v); }
	ownedViews_.clear();
	for (sg_image i : images_) { sg_destroy_image(i); }
	views_.clear();
	images_.clear();
	sg_destroy_view(white_);
	sg_destroy_image(whiteImg_);
	sg_destroy_sampler(smp_);
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
}

void MeshView::render(const float mvp[16], const double cam[3]) const
{
	drawstats::pipeline(pip_);
	mesh_vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	for (int k = 0; k < 3; k++) { vs.origin[k] = (float)-cam[k]; }
	mesh_fs_params_t fsp = {};
	fsp.sun[0] = 0.4f; fsp.sun[1] = 0.8f; fsp.sun[2] = 0.45f;
	for (const Part& p : parts_)
	{
		sg_bindings b = {};
		b.vertex_buffers[0] = p.vbuf;
		b.index_buffer = p.ibuf;
		b.views[VIEW_diffuse_tex] = p.view;
		b.samplers[SMP_diffuse_smp] = smp_;
		drawstats::bindings(&b);
		sg_apply_uniforms(UB_mesh_vs_params, SG_RANGE(vs));
		sg_apply_uniforms(UB_mesh_fs_params, SG_RANGE(fsp));
		drawstats::draw(0, p.indexCount, 1);
	}
}

// The viewer's state: the flags and the mesh.
struct ViewState
{
	bool on = false;
	std::string mesh;	// --view-mesh NAME_OR_PATH
	std::string tex;	// --tex TEXTURE.dds
	double dist = 0;	// --dist D / the scroll wheel; 0 = fit the mesh
	MeshView view;
};
ViewState& vs() { static ViewState s; return s; }

}	// namespace

bool viewArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff)
{
	ViewState& s = vs();
	const std::string a = argv[i];
	const bool more = i + 1 < argc;
	if (a == "--view-mesh" && more) { s.mesh = argv[++i]; windowOff.push_back("--view-mesh " + s.mesh); s.on = true; return true; }
	if (a == "--tex" && more) { s.tex = argv[++i]; return true; }
	if (a == "--dist" && more) { s.dist = atof(argv[++i]); return true; }
	return false;
}

bool viewing() { return vs().on; }

void setViewDist(double d) { vs().dist = d; }

void viewScroll(double y) { vs().dist *= std::pow(0.9, y); }

void viewInit(const ResourceIndex& res, const MaterialLib& mats)
{
	ViewState& s = vs();
	std::string err;
	if (!s.view.load(res, mats, s.mesh, s.tex, err))
	{
		fprintf(stderr, "kclient: %s\n", err.c_str());
		exit(1);
	}
	const MeshView& m = s.view;
	printf("mesh %s\n", m.path.c_str());
	printf("bounds (world units) min %.2f %.2f %.2f  max %.2f %.2f %.2f  size %.2f x %.2f x %.2f (x y z)\n", m.boundsMin[0], m.boundsMin[1], m.boundsMin[2],
		m.boundsMax[0], m.boundsMax[1], m.boundsMax[2], m.boundsMax[0] - m.boundsMin[0], m.boundsMax[1] - m.boundsMin[1], m.boundsMax[2] - m.boundsMin[2]);
	for (const std::string& l : m.report) { printf("  %s\n", l.c_str()); }
	if (s.dist <= 0) { s.dist = std::max(0.01f, m.radius * 2.1f); }
}

bool viewFrame(const Context& cx, FramePacket& pk)
{
	ViewState& s = vs();
	if (!s.on) { return false; }
	cx.runScript(false);
	double f[3];
	cx.forwardVec(cx.yaw, cx.pitch, f);
	const MeshView& m = s.view;
	for (int k = 0; k < 3; k++) { cx.pos[k] = m.center[k] - f[k] * s.dist; }
	if (!cx.firstWorldFrame) { cx.firstWorldFrame = true; printf("first_world_frame: x=%.0f z=%.0f reason=viewer\n", cx.pos[0], cx.pos[2]); }
	cx.currentCamera((float)std::fmax(0.001, s.dist * 0.01), (float)(s.dist * 20 + m.radius * 4), pk.views[0]);
	pk.viewCount = 1;
	pk.path = FramePath::kView;
	// the draw's inputs (render hook 9): the camera position and the report panel and lines, recorded through ovl:: here, played by viewDraw
	std::copy(cx.pos, cx.pos + 3, pk.early.viewPos);
	pk.early.viewText.clear();
	ovl::Record rec(pk.early.viewText);
	const float lh = 9.0f * text_scale();
	char line[200];
	draw_panel(4, 4, 100.0f * 8 * text_scale(), lh * (2 + (float)m.report.size()) + 8);
	snprintf(line, sizeof line, "%s", m.path.c_str());
	draw_text(8, 8, 1.0f, 1.0f, 1.0f, line);
	snprintf(line, sizeof line, "size %.1f x %.1f x %.1f units (x y z)  dist %.1f", m.boundsMax[0] - m.boundsMin[0], m.boundsMax[1] - m.boundsMin[1],
		m.boundsMax[2] - m.boundsMin[2], s.dist);
	draw_text(8, 8 + lh, 0.8f, 1.0f, 0.8f, line);
	for (size_t i = 0; i < m.report.size(); i++) { draw_text(8, 8 + lh * (2 + (float)i), 0.9f, 0.9f, 0.7f, m.report[i].c_str()); }
	return true;
}

// The render half (the render table's kView slot): the mesh (render-owned once loaded: viewInit on the main thread before the first frame, viewShutdown
// after the last) and the packet's camera, position and text; nothing of the Context, so it can run on the render thread.
void viewDraw(const FramePacket& pk, GpuTimer& gpu)
{
	const MeshView& m = vs().view;
	float mvp[16];
	pk.views[0].mvp(mvp);
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0.20f, 0.22f, 0.26f, 1.0f };
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	m.render(mvp, pk.early.viewPos);
	begin_overlay();
	ovl::play(pk.early.viewText);	// viewFrame's panel and lines
	end_overlay();
	GPU_END(gpu);
	(void)gpu;	// KCLIENT_PROFILE off: GPU_END is empty
	sg_end_pass();
	readback::serve();	// logic_tail's requests (the script's shots)
	sg_commit();
	framestats::endFrame();	// the frame count (the render side's; logic mirrors it into app.frames)
}

void viewShutdown() { vs().view.shutdown(); }

}	// namespace dev
