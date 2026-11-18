#include "render/drawlist_gpu.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "render/drawstats.hpp"
#include "sokol_gl.h"

namespace dl::gpu
{
	namespace
	{
		struct Entry
		{
			sg_image img = {};	// SG_INVALID_ID for a bound (host-owned) texture
			sg_view view = {};
			sg_sampler smp = {};
			bool owned = false;
			int w = 0, h = 0;
			Filter filter = Filter::kLinear;
			const char* label = "ui";
			std::vector<uint8_t> mirror;	// updatable: level 0, RGBA8
		};
		struct Dead { sg_image img; sg_view view; };

		std::unordered_map<TexId, Entry> table_;
		std::vector<Dead> pending_, next_;	// pending_: destroyed at the next apply; next_: this apply's, moved to pending_ at its end
		sgl_pipeline pip_ = {};
		sg_sampler linear_ = {}, trilinear_ = {};
		Stats stats_;

		// The UI's pipeline and samplers, as ui/gfx.cpp and ui/icons.cpp make theirs (same descs: the replay draws what they drew).
		void ready()
		{
			if (pip_.id != SG_INVALID_ID) { return; }
			sg_sampler_desc sd = {};
			sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
			sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
			sd.label = "dl-linear";
			linear_ = sg_make_sampler(&sd);
			sd.mipmap_filter = SG_FILTER_LINEAR;
			sd.label = "dl-trilinear";
			trilinear_ = sg_make_sampler(&sd);
			sg_pipeline_desc pd = {};
			pd.colors[0].blend.enabled = true;
			pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
			pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
			pd.label = "dl-ui-blend";
			pip_ = sgl_make_pipeline(&pd);
		}

		sgl_pipeline pipeline(Pipe) { return pip_; }	// kUi is the only one

		void make(Entry& e, const uint8_t* px, size_t bytes, int mips)
		{
			sg_image_desc id = {};
			id.width = e.w;
			id.height = e.h;
			id.pixel_format = SG_PIXELFORMAT_RGBA8;
			id.num_mipmaps = mips;
			id.data = { px, bytes };
			id.label = e.label;
			e.img = sg_make_image(&id);
			sg_view_desc vd = {};
			vd.texture.image = e.img;
			e.view = sg_make_view(&vd);
			e.smp = e.filter == Filter::kTrilinear ? trilinear_ : linear_;
			stats_.uploadBytes += bytes;
		}

		void retire(const Entry& e)
		{
			if (e.owned) { next_.push_back({ e.img, e.view }); }
		}

		void kill(const Dead& d)
		{
			if (d.view.id != SG_INVALID_ID) { sg_destroy_view(d.view); }
			if (d.img.id != SG_INVALID_ID) { sg_destroy_image(d.img); }
		}
	}

	void bind(TexId id, sg_view view, sg_sampler smp)
	{
		Entry& e = table_[id];
		retire(e);
		e = {};
		e.view = view;
		e.smp = smp;
	}

	void unbind(TexId id)
	{
		const auto it = table_.find(id);
		if (it == table_.end() || it->second.owned) { return; }
		table_.erase(it);
	}

	sg_view view(TexId id)
	{
		const auto it = table_.find(id);
		return it == table_.end() ? sg_view{} : it->second.view;
	}

	sg_sampler sampler(TexId id)
	{
		const auto it = table_.find(id);
		return it == table_.end() ? sg_sampler{} : it->second.smp;
	}

	void apply(std::vector<Upload> ups)
	{
		for (const Dead& d : pending_) { kill(d); }
		pending_.clear();
		if (!ups.empty()) { ready(); }
		for (Upload& u : ups)
		{
			switch (u.op)
			{
			case Upload::Op::kCreate:
			{
				Entry& e = table_[u.id];
				retire(e);
				e = {};
				e.owned = true;
				e.w = u.w;
				e.h = u.h;
				e.filter = u.filter;
				e.label = u.label;
				make(e, u.rgba.data(), u.rgba.size(), u.updatable ? 1 : u.mips);
				if (u.updatable) { e.mirror = std::move(u.rgba); e.mirror.resize((size_t)e.w * e.h * 4); }
				break;
			}
			case Upload::Op::kUpdate:
			{
				const auto it = table_.find(u.id);
				if (it == table_.end() || !it->second.owned || it->second.mirror.empty())
				{
					fprintf(stderr, "dl: update of texture %u, which is not an updatable one: ignored\n", u.id);
					break;
				}
				Entry& e = it->second;
				// The dirty rect into the CPU copy, then the image again: this sokol has no partial update outside a copy_dst image and a
				// staging buffer (sg_copy_buffer_to_image), and the UI's pages change seldom. The old image lives until the next apply.
				const int x0 = std::max(0, u.x), y0 = std::max(0, u.y), x1 = std::min(e.w, u.x + u.w), y1 = std::min(e.h, u.y + u.h);
				if (x1 <= x0 || y1 <= y0 || u.rgba.size() < (size_t)u.w * u.h * 4) { break; }
				for (int y = y0; y < y1; y++)
				{
					memcpy(&e.mirror[((size_t)y * e.w + x0) * 4], &u.rgba[((size_t)(y - u.y) * u.w + (x0 - u.x)) * 4], (size_t)(x1 - x0) * 4);
				}
				retire(e);
				make(e, e.mirror.data(), e.mirror.size(), 1);
				break;
			}
			case Upload::Op::kDestroy:
			{
				const auto it = table_.find(u.id);
				if (it == table_.end()) { break; }
				retire(it->second);
				table_.erase(it);
				break;
			}
			}
		}
		pending_.swap(next_);
		stats_.textures = (uint32_t)std::count_if(table_.begin(), table_.end(), [](const auto& kv) { return kv.second.owned; });
	}

	void replay(const List& list)
	{
		stats_.draws = stats_.vertices = stats_.missing = 0;
		if (list.empty()) { return; }
		ready();
		const Rect screen = { 0, 0, (float)list.width(), (float)list.height() };
		// gfx::begin
		sgl_push_pipeline();
		sgl_load_pipeline(pip_);
		Pipe pipe = Pipe::kUi;
		sgl_matrix_mode_projection();
		sgl_push_matrix();
		sgl_load_identity();
		sgl_ortho(0.0f, screen.w, screen.h, 0.0f, -1.0f, 1.0f);
		sgl_matrix_mode_modelview();
		sgl_push_matrix();
		sgl_load_identity();
		sgl_scissor_rectf(screen.x, screen.y, screen.w, screen.h, true);
		Rect clip = screen;
		bool clipKnown = true;
		const std::vector<Vertex>& vs = list.vertices();
		for (const Cmd& c : list.cmds())
		{
			if (!clipKnown || c.clip.x != clip.x || c.clip.y != clip.y || c.clip.w != clip.w || c.clip.h != clip.h)
			{
				clip = c.clip;
				clipKnown = true;
				sgl_scissor_rectf(clip.x, clip.y, clip.w, clip.h, true);
			}
			if (c.pipe != pipe) { pipe = c.pipe; sgl_load_pipeline(pipeline(pipe)); }
			if (c.layer != kLayerInherit) { sgl_layer(c.layer); }
			if (c.kind == Cmd::Kind::kHost)
			{
				// gfx::host: the host's sgl inside the clip, its pipeline and matrices undone afterwards
				const Host& h = list.hosts()[c.host];
				sgl_push_pipeline();
				sgl_matrix_mode_texture();
				sgl_push_matrix();
				sgl_matrix_mode_projection();
				sgl_push_matrix();
				sgl_matrix_mode_modelview();
				sgl_push_matrix();
				h.draw(h.r.x, h.r.y, h.r.w, h.r.h);
				sgl_matrix_mode_texture();
				sgl_pop_matrix();
				sgl_matrix_mode_projection();
				sgl_pop_matrix();
				sgl_matrix_mode_modelview();
				sgl_pop_matrix();
				sgl_pop_pipeline();
				sgl_disable_texture();
				clipKnown = false;	// gfx's popClip sets the outer clip again, whatever the host left
				continue;
			}
			if (c.tex == 0) { sgl_disable_texture(); }
			else
			{
				const auto it = table_.find(c.tex);
				if (it == table_.end() || it->second.view.id == SG_INVALID_ID) { stats_.missing++; continue; }
				sgl_enable_texture();
				sgl_texture(it->second.view, it->second.smp.id != SG_INVALID_ID ? it->second.smp : linear_);
			}
			if (c.prim == Prim::kQuads) { sgl_begin_quads(); }
			else { sgl_begin_triangles(); }
			for (uint32_t i = c.first; i < c.first + c.count; i++) { sgl_v2f_t2f_c1i(vs[i].x, vs[i].y, vs[i].u, vs[i].v, vs[i].rgba); }
			if (c.prim == Prim::kQuads) { DRAWSTATS_QUADS(sgl_num_vertices()); }
			else { DRAWSTATS_TRIANGLES(sgl_num_vertices()); }
			sgl_end();
			stats_.draws++;
			stats_.vertices += c.count;
		}
		// gfx::end
		sgl_scissor_rectf(screen.x, screen.y, screen.w, screen.h, true);
		sgl_matrix_mode_modelview();
		sgl_pop_matrix();
		sgl_matrix_mode_projection();
		sgl_pop_matrix();
		sgl_pop_pipeline();
		sgl_disable_texture();
	}

	void shutdown()
	{
		for (const Dead& d : pending_) { kill(d); }
		for (const Dead& d : next_) { kill(d); }
		pending_.clear();
		next_.clear();
		for (auto& [id, e] : table_) { if (e.owned) { kill({ e.img, e.view }); } }
		table_.clear();
		if (pip_.id != SG_INVALID_ID) { sgl_destroy_pipeline(pip_); }
		if (linear_.id != SG_INVALID_ID) { sg_destroy_sampler(linear_); }
		if (trilinear_.id != SG_INVALID_ID) { sg_destroy_sampler(trilinear_); }
		pip_ = {};
		linear_ = trilinear_ = {};
		stats_ = {};
	}

	const Stats& stats() { return stats_; }
}
