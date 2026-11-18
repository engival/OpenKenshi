#include "ui/gfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "stb_image.h"
#include "ui/skin.hpp"

namespace ui
{
	Rect Rect::intersect(const Rect& o) const
	{
		const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
		const float x1 = std::min(right(), o.right()), y1 = std::min(bottom(), o.bottom());
		return { x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0) };
	}

	bool Color::parse(const std::string& s, Color& out)
	{
		const char* p = s.c_str();
		while (*p == ' ') { p++; }
		if (*p == '#' || (s.size() == 6 && s.find(' ') == std::string::npos))
		{
			if (*p == '#') { p++; }
			char* end;
			const unsigned long v = strtoul(p, &end, 16);
			if ((end - p != 6 && end - p != 8) || *end) { return false; }
			const unsigned long rgb = end - p == 8 ? v >> 8 : v;	// #rrggbbaa: alpha last
			out = { ((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, end - p == 8 ? (v & 255) / 255.0f : 1.0f };
			return true;
		}
		float c[4] = { 1, 1, 1, 1 };
		int n = 0;
		for (; n < 4; n++)
		{
			char* end;
			const float v = strtof(p, &end);
			if (end == p) { break; }
			c[n] = v;
			p = end;
		}
		while (*p == ' ') { p++; }
		if (n < 3 || *p) { return false; }
		out = { c[0], c[1], c[2], c[3] };
		return true;
	}

	namespace gfx
	{
		namespace
		{
			dl::TexId atlas_ = 0;
			dl::List* list_ = nullptr;	// the list being recorded (begin .. end)
			dl::List pending_;	// uploads made outside a record: the next begin takes them
			std::vector<Rect> clips_;
			Rect screen_;

			void applyClip()
			{
				const Rect& c = clips_.back();
				if (list_) { list_->clip({ c.x, c.y, c.w, c.h }); }
			}

			// The pending uploads onto to, in order (they apply before to's draws).
			void takePending(dl::List& to)
			{
				for (dl::Upload& u : pending_.takeUploads())
				{
					switch (u.op)
					{
					case dl::Upload::Op::kCreate: to.create(u.id, u.w, u.h, std::move(u.rgba), u.mips, u.filter, u.updatable, u.label); break;
					case dl::Upload::Op::kUpdate: to.update(u.id, u.x, u.y, u.w, u.h, std::move(u.rgba)); break;
					case dl::Upload::Op::kDestroy: to.destroy(u.id); break;
					}
				}
			}

			// One sgl_begin .. sgl_end group: texture 0 is untextured (u = v = 0; sgl ignores uv without a texture).
			template <class F> void prim(dl::Prim p, dl::TexId tex, F&& verts)
			{
				if (!list_) { return; }
				list_->texture(tex);
				list_->beginPrim(p);
				verts(*list_);
				list_->endPrim();
			}

			uint32_t pack(const Color& c) { return dl::pack(c.r, c.g, c.b, c.a); }

			void quad(dl::List& l, const Rect& r, float u0, float v0, float u1, float v1, uint32_t c)
			{
				l.vertex(r.x, r.y, u0, v0, c); l.vertex(r.right(), r.y, u1, v0, c); l.vertex(r.right(), r.bottom(), u1, v1, c); l.vertex(r.x, r.bottom(), u0, v1, c);
			}
		}

		bool init()
		{
			const std::string path = res::file("gfx/Kenshi_UI.png");
			int w, h, comp;
			stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
			if (!px) { fprintf(stderr, "kclient ui: cannot read %s\n", path.c_str()); return false; }
			if (w != (int)kAtlasW || h != (int)kAtlasH) { fprintf(stderr, "kclient ui: %s is %dx%d, skins expect %gx%g\n", path.c_str(), w, h, kAtlasW, kAtlasH); }
			atlas_ = dl::newTex();
			uploads().create(atlas_, w, h, std::vector<uint8_t>(px, px + (size_t)w * h * 4), 1, dl::Filter::kLinear, false, "ui-atlas");
			stbi_image_free(px);
			return true;
		}

		void shutdown()
		{
			if (atlas_) { uploads().destroy(atlas_); }
			atlas_ = 0;
		}

		dl::TexId atlas() { return atlas_; }
		dl::List& uploads() { return list_ ? *list_ : pending_; }
		dl::List* recording() { return list_; }

		void begin(dl::List& list, int fbW, int fbH)
		{
			list_ = &list;
			list.begin(fbW, fbH);
			takePending(list);
			screen_ = { 0, 0, (float)fbW, (float)fbH };
			clips_.assign(1, screen_);
			applyClip();
		}

		void end()
		{
			if (!list_) { return; }
			list_->clip({ screen_.x, screen_.y, screen_.w, screen_.h });
			list_ = nullptr;
		}

		void fill(const Rect& r, const Color& c)
		{
			prim(dl::Prim::kQuads, 0, [&](dl::List& l) { quad(l, r, 0, 0, 0, 0, pack(c)); });
		}

		void gradient(const Rect& r, const Color& top, const Color& bottom)
		{
			prim(dl::Prim::kQuads, 0, [&](dl::List& l)
			{
				const uint32_t t = pack(top), b = pack(bottom);
				l.vertex(r.x, r.y, 0, 0, t); l.vertex(r.right(), r.y, 0, 0, t); l.vertex(r.right(), r.bottom(), 0, 0, b); l.vertex(r.x, r.bottom(), 0, 0, b);
			});
		}

		void gradientH(const Rect& r, const Color& left, const Color& right)
		{
			prim(dl::Prim::kQuads, 0, [&](dl::List& l)
			{
				const uint32_t a = pack(left), b = pack(right);
				l.vertex(r.x, r.y, 0, 0, a); l.vertex(r.right(), r.y, 0, 0, b); l.vertex(r.right(), r.bottom(), 0, 0, b); l.vertex(r.x, r.bottom(), 0, 0, a);
			});
		}

		void triangle(float x0, float y0, float x1, float y1, float x2, float y2, const Color& c)
		{
			prim(dl::Prim::kTriangles, 0, [&](dl::List& l)
			{
				const uint32_t k = pack(c);
				l.vertex(x0, y0, 0, 0, k); l.vertex(x1, y1, 0, 0, k); l.vertex(x2, y2, 0, 0, k);
			});
		}

		void rounded(const Rect& r, float radius, const Color& c)
		{
			if (r.w <= 0 || r.h <= 0) { return; }
			const float rad = std::clamp(radius, 0.5f, std::min(r.w, r.h) / 2);
			// The outline: each corner's arc (clockwise from the top-left), points on the true edge with their outward normals.
			const int n = std::clamp((int)std::ceil(rad / 2), 2, 16);	// segments per quarter
			struct P { float x, y, nx, ny; };
			std::vector<P> pts;
			pts.reserve(4 * (n + 1));
			const float cx[4] = { r.x + rad, r.right() - rad, r.right() - rad, r.x + rad };
			const float cy[4] = { r.y + rad, r.y + rad, r.bottom() - rad, r.bottom() - rad };
			for (int k = 0; k < 4; k++)
			{
				for (int i = 0; i <= n; i++)
				{
					const float a = (float)M_PI * (1 + k * 0.5f + 0.5f * i / n);
					const float nx = std::cos(a), ny = std::sin(a);
					pts.push_back({ cx[k] + nx * rad, cy[k] + ny * rad, nx, ny });
				}
			}
			const uint32_t in = pack(c), out = pack(c.alpha(0));
			const float mx = r.x + r.w / 2, my = r.y + r.h / 2;
			prim(dl::Prim::kTriangles, 0, [&](dl::List& l)
			{
				// The body: a fan from the centre to the outline pulled in half a pixel; then a one-pixel ring fading out across the edge.
				for (size_t i = 0; i < pts.size(); i++)
				{
					const P& a = pts[i];
					const P& b = pts[(i + 1) % pts.size()];
					const float ax = a.x - a.nx * 0.5f, ay = a.y - a.ny * 0.5f, bx = b.x - b.nx * 0.5f, by = b.y - b.ny * 0.5f;
					const float ox = a.x + a.nx * 0.5f, oy = a.y + a.ny * 0.5f, px = b.x + b.nx * 0.5f, py = b.y + b.ny * 0.5f;
					l.vertex(mx, my, 0, 0, in); l.vertex(ax, ay, 0, 0, in); l.vertex(bx, by, 0, 0, in);
					l.vertex(ax, ay, 0, 0, in); l.vertex(ox, oy, 0, 0, out); l.vertex(px, py, 0, 0, out);
					l.vertex(ax, ay, 0, 0, in); l.vertex(px, py, 0, 0, out); l.vertex(bx, by, 0, 0, in);
				}
			});
		}

		void frame(const Rect& r, float t, const Color& c)
		{
			fill({ r.x, r.y, r.w, t }, c);
			fill({ r.x, r.bottom() - t, r.w, t }, c);
			fill({ r.x, r.y + t, t, r.h - 2 * t }, c);
			fill({ r.right() - t, r.y + t, t, r.h - 2 * t }, c);
		}

		void textured(dl::TexId tex, const Rect& r, float u0, float v0, float u1, float v1, const Color& c)
		{
			if (!tex) { return; }
			prim(dl::Prim::kQuads, tex, [&](dl::List& l) { quad(l, r, u0, v0, u1, v1, pack(c)); });
		}

		void sprite(const Rect& r, const Rect& t, const Color& c)
		{
			textured(atlas_, r, t.x / kAtlasW, t.y / kAtlasH, t.right() / kAtlasW, t.bottom() / kAtlasH, c);
		}

		void spriteRotated(float cx, float cy, float w, float h, float angle, const Rect& t, const Color& c)
		{
			const float cs = std::cos(angle), sn = std::sin(angle);
			const float u0 = t.x / kAtlasW, v0 = t.y / kAtlasH, u1 = t.right() / kAtlasW, v1 = t.bottom() / kAtlasH;
			const float px[4] = { -w / 2, w / 2, w / 2, -w / 2 }, py[4] = { -h / 2, -h / 2, h / 2, h / 2 };
			const float u[4] = { u0, u1, u1, u0 }, v[4] = { v0, v0, v1, v1 };
			prim(dl::Prim::kQuads, atlas_, [&](dl::List& l)
			{
				const uint32_t k = pack(c);
				for (int i = 0; i < 4; i++) { l.vertex(cx + px[i] * cs - py[i] * sn, cy + px[i] * sn + py[i] * cs, u[i], v[i], k); }
			});
		}

		void tiled(const Rect& r, const Rect& t, float tileW, float tileH, const Color& c)
		{
			if (tileW < 1 || tileH < 1) { return; }
			prim(dl::Prim::kQuads, atlas_, [&](dl::List& l)
			{
				const uint32_t k = pack(c);
				for (float y = r.y; y < r.bottom(); y += tileH)
				{
					const float fh = std::min(1.0f, (r.bottom() - y) / tileH);
					for (float x = r.x; x < r.right(); x += tileW)
					{
						const float fw = std::min(1.0f, (r.right() - x) / tileW);
						quad(l, { x, y, tileW * fw, tileH * fh }, t.x / kAtlasW, t.y / kAtlasH, (t.x + t.w * fw) / kAtlasW, (t.y + t.h * fh) / kAtlasH, k);
					}
				}
			});
		}

		void pushClip(const Rect& r)
		{
			Rect c = clips_.back().intersect(r);
			// Whole pixels: the scissor rounds anyway, and a fractional clip would let the next sibling's edge show through.
			const float x0 = std::floor(c.x), y0 = std::floor(c.y);
			c = { x0, y0, std::ceil(c.right()) - x0, std::ceil(c.bottom()) - y0 };
			clips_.push_back(c);
			applyClip();
		}

		void host(const Rect& r, std::function<void(float x, float y, float w, float h)> draw)
		{
			if (!draw || !list_) { return; }
			pushClip(r);
			list_->host({ r.x, r.y, r.w, r.h }, std::move(draw));
			popClip();
		}

		void popClip()
		{
			if (clips_.size() > 1) { clips_.pop_back(); }
			applyClip();
		}

		const Rect& clip() { return clips_.back(); }
	}
}
