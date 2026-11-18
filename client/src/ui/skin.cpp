#include "ui/skin.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "ui/style.hpp"
#include "ui/xml.hpp"

namespace ui
{
	namespace
	{
		std::map<std::string, Skin> skins_;
		std::map<std::string, ImageSet> images_;
		std::map<std::string, Color> colours_;
		std::vector<std::string> roots_;	// gui folders, base first
		std::string dataDir_ = ".";

		const char* const kStates[8] = { "normal", "highlighted", "pushed", "disabled", "normal_checked", "highlighted_checked", "pushed_checked", "disabled_checked" };

		bool rect4(const std::string& s, Rect& r)
		{
			return sscanf(s.c_str(), "%f %f %f %f", &r.x, &r.y, &r.w, &r.h) == 4;
		}

		unsigned parseAlign(const std::string& s)
		{
			unsigned a = 0;
			bool h = false, v = false;
			size_t p = 0;
			while (p < s.size())
			{
				size_t e = s.find(' ', p);
				if (e == std::string::npos) { e = s.size(); }
				const std::string w = s.substr(p, e - p);
				p = e + 1;
				if (w == "Left") { a |= Skin::kLeft; h = true; }
				else if (w == "Right") { a |= Skin::kRight; h = true; }
				else if (w == "HCenter") { h = true; }
				else if (w == "HStretch") { a |= Skin::kHStretch; h = true; }
				else if (w == "Top") { a |= Skin::kTop; v = true; }
				else if (w == "Bottom") { a |= Skin::kBottom; v = true; }
				else if (w == "VCenter") { v = true; }
				else if (w == "VStretch") { a |= Skin::kVStretch; v = true; }
				else if (w == "Stretch") { a |= Skin::kHStretch | Skin::kVStretch; h = v = true; }
				else if (w == "Center") { h = v = true; }
			}
			if (!h) { a |= Skin::kLeft; }	// MyGUI's default is Left Top
			if (!v) { a |= Skin::kTop; }
			return a;
		}

		// A missing state falls back to its unchecked twin, then to normal (most skins only define normal).
		int stateIndex(const Skin::Part& p, State s, bool checked)
		{
			const int i = (int)s + (checked ? 4 : 0);
			if (p.has[i]) { return i; }
			if (p.has[(int)s]) { return (int)s; }
			if (checked && p.has[4]) { return 4; }
			return p.has[0] ? 0 : -1;
		}

		// The part's rect in pixels for a widget of r at scale (MyGUI align rules: the part keeps its distances to the edges it is aligned to).
		Rect place(const Skin& sk, const Skin::Part& p, const Rect& r, float scale)
		{
			const float dw = r.w / scale - sk.w, dh = r.h / scale - sk.h;
			float x = p.offset.x, y = p.offset.y, w = p.offset.w, h = p.offset.h;
			switch (p.align & Skin::kHStretch)
			{
				case Skin::kRight: x += dw; break;
				case Skin::kHStretch: w += dw; break;
				case Skin::kHCenter: x += dw / 2; break;
				default: break;
			}
			switch (p.align & Skin::kVStretch)
			{
				case Skin::kBottom: y += dh; break;
				case Skin::kVStretch: h += dh; break;
				case Skin::kVCenter: y += dh / 2; break;
				default: break;
			}
			// Whole pixels, edges rounded rather than sizes, so neighbouring parts meet without seams.
			const float x0 = std::round(r.x + x * scale), y0 = std::round(r.y + y * scale);
			return { x0, y0, std::round(r.x + (x + w) * scale) - x0, std::round(r.y + (y + h) * scale) - y0 };
		}

		void loadSkins(const xml::Node& root)
		{
			for (const xml::Node* res : root.all("Resource"))
			{
				if (res->attr("type") != "ResourceSkin") { continue; }
				Skin sk;
				sk.name = res->attr("name");
				sscanf(res->attr("size").c_str(), "%f %f", &sk.w, &sk.h);
				const std::string tex = res->attr("texture");
				if (!tex.empty() && tex != "Kenshi_UI.png") { fprintf(stderr, "kclient ui: skin %s uses %s, only Kenshi_UI.png is loaded\n", sk.name.c_str(), tex.c_str()); continue; }
				for (const xml::Node* b : res->all("BasisSkin"))
				{
					const std::string type = b->attr("type");
					Skin::Part p;
					if (type == "SubSkin" || type == "MainSkin") { p.kind = Skin::Part::Kind::kImage; }
					else if (type == "TileRect") { p.kind = Skin::Part::Kind::kTiled; }
					else if (type == "SimpleText" || type == "EditText") { p.kind = Skin::Part::Kind::kText; }
					else { continue; }	// RotatingSkin, PolygonalSkin: not used by the game's widgets we draw
					if (!rect4(b->attr("offset"), p.offset)) { continue; }
					p.align = parseAlign(b->attr("align"));
					for (const xml::Node* st : b->all("State"))
					{
						int i = 0;
						while (i < 8 && st->attr("name") != kStates[i]) { i++; }
						if (i == 8) { continue; }
						if (p.kind == Skin::Part::Kind::kText)
						{
							p.has[i] = Color::parse(st->attr("colour"), p.colour[i]);
							continue;
						}
						p.has[i] = rect4(st->attr("offset"), p.texels[i]);
						for (const xml::Node* prop : st->all("Property"))
						{
							if (prop->attr("key") == "TileSize") { sscanf(prop->attr("value").c_str(), "%f %f", &p.tileW, &p.tileH); }
						}
					}
					if (p.kind == Skin::Part::Kind::kText && !p.has[0]) { p.has[0] = true; p.colour[0] = colours_.count("Main") ? colours_["Main"] : Color{}; }
					sk.parts.push_back(p);
				}
				skins_[sk.name] = std::move(sk);
			}
		}

		void loadImages(const xml::Node& root)
		{
			for (const xml::Node* res : root.all("Resource"))
			{
				if (res->attr("type") != "ResourceImageSet") { continue; }
				ImageSet set;
				for (const xml::Node* g : res->all("Group"))
				{
					if (g->attr("texture") != "Kenshi_UI.png") { continue; }	// screen art (loading / title DDS) is not part of the atlas
					float w = 0, h = 0;
					sscanf(g->attr("size").c_str(), "%f %f", &w, &h);
					if (set.frames.empty()) { set.w = w; set.h = h; }
					for (const xml::Node* ix : g->all("Index"))
					{
						const xml::Node* f = ix->first("Frame");
						float x, y;
						if (f && sscanf(f->attr("point").c_str(), "%f %f", &x, &y) == 2) { set.frames.emplace(ix->attr("name"), Rect{ x, y, w, h }); }
					}
				}
				if (!set.frames.empty()) { images_[res->attr("name")] = std::move(set); }
			}
		}
	}

	void Skin::draw(const Rect& r, float scale, State s, bool checked, const Color& tint) const
	{
		for (const Part& p : parts)
		{
			if (p.kind == Part::Kind::kText) { continue; }
			const int i = stateIndex(p, s, checked);
			if (i < 0) { continue; }
			const Rect d = place(*this, p, r, scale);
			if (d.w <= 0 || d.h <= 0) { continue; }
			if (p.kind == Part::Kind::kTiled && p.tileW > 0) { gfx::tiled(d, p.texels[i], p.tileW * scale, p.tileH * scale, tint); }
			else { gfx::sprite(d, p.texels[i], tint); }
		}
	}

	bool Skin::text(const Rect& r, float scale, State s, bool checked, Rect& area, Color& colour) const
	{
		for (const Part& p : parts)
		{
			if (p.kind != Part::Kind::kText) { continue; }
			area = place(*this, p, r, scale);
			const int i = stateIndex(p, s, checked);
			colour = i < 0 ? Color{} : p.colour[i];
			return true;
		}
		return false;
	}

	namespace res
	{
		void setRoots(const std::string& dataDir, const std::vector<std::string>& themes)
		{
			dataDir_ = dataDir;
			roots_ = { dataDir + "/gui" };
			for (const std::string& t : themes) { roots_.push_back(t + "/gui"); }
		}

		std::string dataFile(const std::string& rel) { return dataDir_ + "/" + rel; }

		std::string file(const std::string& rel)
		{
			for (size_t i = roots_.size(); i-- > 1;)
			{
				std::error_code ec;
				const std::string p = roots_[i] + "/" + rel;
				if (std::filesystem::is_regular_file(p, ec)) { return p; }
			}
			return (roots_.empty() ? std::string("gui") : roots_[0]) + "/" + rel;
		}

		bool themed(const std::string& rel)
		{
			for (size_t i = 1; i < roots_.size(); i++)
			{
				std::error_code ec;
				if (std::filesystem::is_regular_file(roots_[i] + "/" + rel, ec)) { return true; }
			}
			return false;
		}

		bool load()
		{
			std::string err;
			skins_.clear();
			images_.clear();
			colours_.clear();
			if (auto c = xml::load(file("colours/kenshi_colours.xml"), err))
			{
				for (const xml::Node* p : c->all("Property"))
				{
					Color col;
					if (Color::parse(p->attr("value"), col)) { colours_[p->attr("key")] = col; }
				}
			}
			else { fprintf(stderr, "kclient ui: %s\n", err.c_str()); }
			for (const char* f : { "skins/common_skins.xml", "skins/kenshi_skins.xml" })
			{
				auto n = xml::load(file(f), err);
				if (!n) { fprintf(stderr, "kclient ui: %s\n", err.c_str()); return false; }
				loadSkins(*n);
			}
			if (auto n = xml::load(file("images/kenshi_images.xml"), err)) { loadImages(*n); }
			else { fprintf(stderr, "kclient ui: %s\n", err.c_str()); }
			return !skins_.empty();
		}

		const Skin* skin(const std::string& name)
		{
			auto it = skins_.find(name);
			if (it == skins_.end()) { it = skins_.find(name + "Skin"); }
			if (it != skins_.end()) { return &it->second; }
			static std::set<std::string> warned;
			if (warned.insert(name).second) { fprintf(stderr, "kclient ui: no skin %s\n", name.c_str()); }
			return nullptr;
		}

		const ImageSet* images(const std::string& name)
		{
			auto it = images_.find(name);
			return it == images_.end() ? nullptr : &it->second;
		}

		Color colour(const std::string& name, const Color& def)
		{
			Color c;
			if (!name.empty() && name[0] == '#' && Color::parse(name, c)) { return c; }	// a host-computed colour ("#00a31a")
			return style().colour(name, def);
		}

		Color fileColour(const std::string& name, const Color& def)
		{
			auto it = colours_.find(name);
			return it == colours_.end() ? def : it->second;
		}

		std::vector<std::string> skinNames()
		{
			std::vector<std::string> out;
			for (const auto& kv : skins_) { out.push_back(kv.first); }
			return out;
		}
	}
}
