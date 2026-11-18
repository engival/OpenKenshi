#include "cursors.hpp"
#include "sappcmd.hpp"
#include "drawstats.hpp"
#include "renderqueue.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <vector>

#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "stb_image.h"

namespace
{
	std::string slurp(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}

	// The Property values (or Group / Frame attributes) of each <Resource name="..."> block, keyed by resource name.
	std::unordered_map<std::string, std::string> resources(const std::string& xml)
	{
		std::unordered_map<std::string, std::string> out;
		static const std::regex block(R"re(<Resource\s+type="[^"]*"\s+name="([^"]*)">([\s\S]*?)</Resource>)re");
		for (std::sregex_iterator it(xml.begin(), xml.end(), block), end; it != end; ++it) { out[(*it)[1]] = (*it)[2]; }
		return out;
	}

	bool two(const std::string& body, const std::string& pattern, int& a, int& b)
	{
		std::smatch m;
		if (!std::regex_search(body, m, std::regex(pattern + R"re(\s*=?\s*"(-?\d+)\s+(-?\d+)")re"))) { return false; }
		a = std::stoi(m[1]);
		b = std::stoi(m[2]);
		return true;
	}
}

bool Cursors::init(const std::string& dataDir, std::function<sg_view()> shared, int sharedW, int sharedH)
{
	// The CPU half (logic): the pointer table and the atlas pixels. The GPU half is a frame-lane job (docs/RENDER_THREAD.md §7.2).
	pointers_.clear();
	const auto pointers = resources(slurp(dataDir + "/gui/pointer/kenshi_pointers.xml"));
	const auto images = resources(slurp(dataDir + "/gui/images/kenshi_images.xml"));
	std::string texture;
	for (const auto& [name, body] : pointers)
	{
		std::smatch m;
		if (name.empty() || !std::regex_search(body, m, std::regex(R"re(key="Resource"\s+value="([^"]+)")re"))) { continue; }
		const auto img = images.find(m[1]);
		std::smatch t;
		if (img == images.end() || !std::regex_search(img->second, t, std::regex(R"re(texture="([^"]+)")re"))) { continue; }
		if (texture.empty()) { texture = t[1]; }
		if (t[1] != texture) { fprintf(stderr, "cursors: %s: texture %s, not %s; skipped\n", name.c_str(), t[1].str().c_str(), texture.c_str()); continue; }
		Pointer p;
		if (!two(img->second, "size", p.w, p.h) || !two(img->second, "point", p.x, p.y)) { continue; }
		two(body, R"(key="Point"\s+value)", p.hotX, p.hotY);
		p.hotX = std::clamp(p.hotX, 0, p.w - 1);
		p.hotY = std::clamp(p.hotY, 0, p.h - 1);
		pointers_[name] = p;
	}
	if (pointers_.empty()) { fprintf(stderr, "cursors: no pointers in %s/gui\n", dataDir.c_str()); return false; }
	const bool useShared = shared && texture == "Kenshi_UI.png";
	std::vector<unsigned char> pixels;
	int w = sharedW, h = sharedH;
	if (!useShared)
	{
		int comp;
		unsigned char* px = stbi_load((dataDir + "/gui/gfx/" + texture).c_str(), &w, &h, &comp, 4);
		if (!px) { fprintf(stderr, "cursors: %s did not load\n", texture.c_str()); pointers_.clear(); return false; }
		pixels.assign(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
	}
	const size_t bytes = pixels.size();
	rq::push("cursors-init", [this, ptrs = pointers_, shared = std::move(shared), useShared, pixels = std::move(pixels), w, h, texture]
	{
		shutdown();	// a second sim_opened: free the previous sampler, and the image and view when they are ours (not the UI layer's)
		drawPointers_ = ptrs;
		atlasW_ = w;
		atlasH_ = h;
		sg_sampler_desc sd = {};
		sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
		sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
		smp_ = sg_make_sampler(&sd);
		if (useShared)
		{
			view_ = shared();
			printf("cursors: %zu from %s (the UI layer's copy)\n", ptrs.size(), texture.c_str());
			return;
		}
		sg_image_desc id = {};
		id.width = w;
		id.height = h;
		id.pixel_format = SG_PIXELFORMAT_RGBA8;
		id.data = { pixels.data(), pixels.size() };
		id.label = "cursors";
		img_ = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.texture.image = img_;
		view_ = sg_make_view(&vd);
		printf("cursors: %zu from %s\n", ptrs.size(), texture.c_str());
	}, rq::Lane::kFrame, bytes);
	return true;
}

void Cursors::shutdown()
{
	if (smp_.id == SG_INVALID_ID) { return; }
	sg_destroy_sampler(smp_);
	if (img_.id != SG_INVALID_ID)	// else the view is the UI layer's
	{
		sg_destroy_view(view_);
		sg_destroy_image(img_);
	}
	img_ = {};
	view_ = {};
	smp_ = {};
}

void Cursors::set(const std::string& name)
{
	if (name == current_) { return; }
	current_ = name;
	sappcmd::showMouse(!pointers_.count(name));
}

void Cursors::draw(const std::string& name, float mx, float my) const
{
	const auto it = drawPointers_.find(name);
	if (it == drawPointers_.end()) { return; }
	const Pointer& p = it->second;
	const float x0 = mx - p.hotX, y0 = my - p.hotY, x1 = x0 + p.w, y1 = y0 + p.h;
	const float u0 = (float)p.x / atlasW_, v0 = (float)p.y / atlasH_, u1 = (float)(p.x + p.w) / atlasW_, v1 = (float)(p.y + p.h) / atlasH_;
	sgl_enable_texture();
	sgl_texture(view_, smp_);
	sgl_begin_quads();
	sgl_c4f(1.0f, 1.0f, 1.0f, 1.0f);
	sgl_v2f_t2f(x0, y0, u0, v0); sgl_v2f_t2f(x1, y0, u1, v0); sgl_v2f_t2f(x1, y1, u1, v1); sgl_v2f_t2f(x0, y1, u0, v1);
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
	sgl_disable_texture();
}
