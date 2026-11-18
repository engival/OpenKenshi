#include "ui/icons.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "stb_image.h"

namespace ui
{
	namespace icons
	{
		namespace
		{
			std::unordered_map<std::string, Picture> pictures_;	// failed loads too (empty), so a missing file is read and logged once
			std::function<std::string(const std::string&)> resolve_;
			const Picture none_;

			// The next mip level of an RGBA image: 2 x 2 averages (an odd last row / column repeats).
			std::vector<uint8_t> half(const uint8_t* px, int w, int h, int& hw, int& hh)
			{
				hw = std::max(1, w / 2);
				hh = std::max(1, h / 2);
				std::vector<uint8_t> out((size_t)hw * hh * 4);
				for (int y = 0; y < hh; y++)
				{
					for (int x = 0; x < hw; x++)
					{
						const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1), y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
						for (int c = 0; c < 4; c++)
						{
							const int sum = px[((size_t)y0 * w + x0) * 4 + c] + px[((size_t)y0 * w + x1) * 4 + c] + px[((size_t)y1 * w + x0) * 4 + c] + px[((size_t)y1 * w + x1) * 4 + c];
							out[((size_t)y * hw + x) * 4 + c] = (uint8_t)((sum + 2) / 4);
						}
					}
				}
				return out;
			}

			Picture load(const std::string& path)
			{
				const std::string file = path.front() == '/' ? path : resolve_ ? resolve_(path) : res::dataFile(path);
				int w, h, comp;
				stbi_uc* px = stbi_load(file.c_str(), &w, &h, &comp, 4);
				if (!px)
				{
					fprintf(stderr, "kclient ui: cannot read picture %s\n", file.c_str());
					return {};
				}
				// Every mip level in one blob, largest first (2 x 2 averages down to 1 x 1).
				std::vector<uint8_t> blob(px, px + (size_t)w * h * 4);
				stbi_image_free(px);
				int levels = 1;
				for (int mw = w, mh = h, at = 0; (mw > 1 || mh > 1) && levels < 16; levels++)
				{
					int nw, nh;
					const std::vector<uint8_t> next = half(blob.data() + at, mw, mh, nw, nh);
					at = (int)blob.size();
					blob.insert(blob.end(), next.begin(), next.end());
					mw = nw;
					mh = nh;
				}
				// Trilinear: icons are drawn far below their size.
				const dl::TexId tex = dl::newTex();
				gfx::uploads().create(tex, w, h, std::move(blob), levels, dl::Filter::kTrilinear, false, "ui-picture");
				return { tex, w, h };
			}
		}

		void setResolver(std::function<std::string(const std::string&)> resolve) { resolve_ = std::move(resolve); }

		const Picture& get(const std::string& path)
		{
			if (path.empty()) { return none_; }
			auto it = pictures_.find(path);
			if (it == pictures_.end()) { it = pictures_.emplace(path, load(path)).first; }
			return it->second;
		}

		std::string cached(const std::string& item, const std::string& material, const std::string& variant)
		{
			return "icons/" + item + "." + material + (variant.empty() ? "" : "." + variant) + ".png";
		}

		std::string recordPath(const std::string& field)
		{
			std::string p = field;
			std::replace(p.begin(), p.end(), '\\', '/');
			for (const char* lead : { "./", "data/" })
			{
				if (p.compare(0, strlen(lead), lead) == 0) { p.erase(0, strlen(lead)); }
			}
			return p;
		}

		void draw(const Picture& p, const Rect& r, const Color& c, bool flipH, bool keepAspect)
		{
			if (!p) { return; }
			Rect d = r;
			if (keepAspect)
			{
				const float k = std::min(r.w / p.w, r.h / p.h);
				d = { std::round(r.x + (r.w - p.w * k) / 2), std::round(r.y + (r.h - p.h * k) / 2), std::round(p.w * k), std::round(p.h * k) };
			}
			gfx::textured(p.tex, d, flipH ? 1 : 0, 0, flipH ? 0 : 1, 1, c);
		}

		void shutdown()
		{
			for (auto& [path, p] : pictures_)
			{
				if (p) { gfx::uploads().destroy(p.tex); }
			}
			pictures_.clear();
		}
	}

	void Picture::paint() const { icons::draw(icons::get(path), screen_, enabled ? tint : tint.alpha(0.5f), flipH, keepAspect); }
}
