#include "ui/font.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "stb_truetype.h"
#include "ui/skin.hpp"
#include "ui/xml.hpp"

namespace ui
{
	namespace
	{
		struct Range { unsigned first, count; };
		constexpr Range kRanges[] = { { 32, 95 }, { 160, 96 }, { 8192, 112 } };
		constexpr int kGlyphs = 95 + 96 + 112;

		std::map<std::string, std::unique_ptr<Font>> fonts_;
		std::string firstFont_;
		float scale_ = 1;

		int glyphIndex(unsigned cp)
		{
			int base = 0;
			for (const Range& r : kRanges)
			{
				if (cp >= r.first && cp < r.first + r.count) { return base + (int)(cp - r.first); }
				base += (int)r.count;
			}
			return '?' - 32;
		}
	}

	struct Font::Page
	{
		float px = 0, ascent = 0, line = 0;	// px: the em
		int size = 0;
		stbtt_packedchar chars[kGlyphs] = {};
		dl::TexId tex = 0;	// recorded as an upload (gfx::uploads); a re-bake is a new page with a new id

		~Page()
		{
			if (tex) { gfx::uploads().destroy(tex); }
		}
	};

	struct FontAccess
	{
		static std::unique_ptr<Font> make(std::string name, std::string file, float size, std::shared_ptr<std::vector<unsigned char>> ttf)
		{
			auto f = std::make_unique<Font>();
			f->name_ = std::move(name);
			f->file_ = std::move(file);
			f->size_ = size;
			f->ttf_ = std::move(ttf);
			return f;
		}
	};

	unsigned utf8Next(std::string_view s, size_t& i)
	{
		const unsigned char c = (unsigned char)s[i++];
		if (c < 0x80) { return c; }
		const int n = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
		if (n < 0 || i + n > s.size()) { return c; }
		unsigned cp = c & (0x3F >> n);
		for (int k = 0; k < n; k++)
		{
			const unsigned char d = (unsigned char)s[i + k];
			if ((d & 0xC0) != 0x80) { return c; }
			cp = (cp << 6) | (d & 0x3F);
		}
		i += n;
		return cp;
	}

	Font::~Font() = default;

	const Font::Page& Font::page() const
	{
		// MyGUI's rule (kenshi-k16.3.33): the size is FreeType's em in pixels (FT_Set_Char_Size, 26.6 fixed point), and the line is the ascent and
		// the descent each rounded up (ResourceTrueTypeFont's "real height"). The game's MyGUI.log (1440 lines) prints 23 / 25 / 30 for the
		// FloaterFonts, 23 for StandardFont_Medium, 25 for PaintedTextFont_Medium: em = Size x 50 / 72 x lines / 1080, exactly. stb's own font_size
		// is the ascent-to-descent height, 1.2 em for Exo2: that read made every Exo2 font 0.83 of the game's (FloaterFont_Small at 720 lines a 9 px
		// line, the game's 12). That old read is in client/retired/font_ascent_descent_size.cpp.
		const float px = std::max(4.0f, std::round(size_ * scale_ * 64) / 64);
		if (page_ && page_->px == px) { return *page_; }
		auto p = std::make_unique<Page>();
		p->px = px;
		const unsigned char* data = ttf_->data();
		stbtt_fontinfo fi;
		int a, d, g;
		stbtt_InitFont(&fi, data, stbtt_GetFontOffsetForIndex(data, 0));
		stbtt_GetFontVMetrics(&fi, &a, &d, &g);
		const float k = stbtt_ScaleForMappingEmToPixels(&fi, px);
		p->ascent = std::ceil(a * k);
		p->line = p->ascent + std::ceil(-d * k);
		std::vector<unsigned char> alpha;
		for (int side = 256; side <= 4096; side *= 2)
		{
			alpha.assign((size_t)side * side, 0);
			stbtt_pack_context pc;
			if (!stbtt_PackBegin(&pc, alpha.data(), side, side, 0, 1, nullptr)) { break; }
			stbtt_pack_range ranges[3];
			int base = 0;
			for (int r = 0; r < 3; r++)
			{
				ranges[r] = {};
				ranges[r].font_size = STBTT_POINT_SIZE(px);
				ranges[r].first_unicode_codepoint_in_range = (int)kRanges[r].first;
				ranges[r].num_chars = (int)kRanges[r].count;
				ranges[r].chardata_for_range = p->chars + base;
				base += (int)kRanges[r].count;
			}
			const int ok = stbtt_PackFontRanges(&pc, data, 0, ranges, 3);
			stbtt_PackEnd(&pc);
			if (ok) { p->size = side; break; }
		}
		if (!p->size) { fprintf(stderr, "kclient ui: font %s does not fit a 4096 page at %g px\n", name_.c_str(), px); }
		else
		{
			std::vector<unsigned char> rgba(alpha.size() * 4, 255);
			for (size_t i = 0; i < alpha.size(); i++) { rgba[i * 4 + 3] = alpha[i]; }	// white, coverage as alpha
			p->tex = dl::newTex();
			gfx::uploads().create(p->tex, p->size, p->size, std::move(rgba), 1, dl::Filter::kLinear, false, "ui-font");
		}
		page_ = std::move(p);
		return *page_;
	}

	float Font::lineHeight() const { return page().line; }
	float Font::ascent() const { return page().ascent; }

	float Font::width(std::string_view t) const
	{
		const Page& p = page();
		float w = 0;
		for (size_t i = 0; i < t.size();) { w += p.chars[glyphIndex(utf8Next(t, i))].xadvance; }
		return w;
	}

	size_t Font::fit(std::string_view t, float maxW) const
	{
		const Page& p = page();
		float w = 0;
		for (size_t i = 0; i < t.size();)
		{
			const size_t at = i;
			w += p.chars[glyphIndex(utf8Next(t, i))].xadvance;
			if (w > maxW) { return at; }
		}
		return t.size();
	}

	std::vector<std::string_view> Font::wrap(std::string_view t, float maxW) const
	{
		std::vector<std::string_view> out;
		size_t start = 0;
		while (start <= t.size())
		{
			const size_t nl = t.find('\n', start);
			std::string_view para = t.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
			do
			{
				size_t n = fit(para, maxW);
				if (n < para.size())
				{
					const size_t space = para.rfind(' ', n);
					if (space != std::string_view::npos && space > 0) { n = space; }
					else if (n == 0) { size_t one = 0; utf8Next(para, one); n = one; }	// not even one glyph fits: take it anyway
				}
				out.push_back(para.substr(0, n));
				para.remove_prefix(n);
				while (!para.empty() && para.front() == ' ') { para.remove_prefix(1); }
			}
			while (!para.empty());
			if (nl == std::string_view::npos) { break; }
			start = nl + 1;
		}
		return out;
	}

	void Font::draw(float x, float y, std::string_view t, const Color& c) const
	{
		const Page& p = page();
		if (!p.size || t.empty()) { return; }
		float px = std::round(x), py = std::round(y) + p.ascent;
		dl::List* rec = gfx::recording();
		if (!rec) { return; }
		dl::List& l = *rec;
		const uint32_t k = dl::pack(c.r, c.g, c.b, c.a);
		l.texture(p.tex);
		l.beginPrim(dl::Prim::kQuads);
		for (size_t i = 0; i < t.size();)
		{
			stbtt_aligned_quad q;
			stbtt_GetPackedQuad(p.chars, p.size, p.size, glyphIndex(utf8Next(t, i)), &px, &py, &q, 1);
			l.vertex(q.x0, q.y0, q.s0, q.t0, k); l.vertex(q.x1, q.y0, q.s1, q.t0, k); l.vertex(q.x1, q.y1, q.s1, q.t1, k); l.vertex(q.x0, q.y1, q.s0, q.t1, k);
		}
		l.endPrim();
	}

	namespace fonts
	{
		bool load()
		{
			std::string err;
			auto root = xml::load(res::file("fonts/kenshi_fonts.xml"), err);
			if (!root) { fprintf(stderr, "kclient ui: %s\n", err.c_str()); return false; }
			std::map<std::string, std::shared_ptr<std::vector<unsigned char>>> files;
			fonts_.clear();
			firstFont_.clear();
			for (const xml::Node* res : root->all("Resource"))
			{
				if (res->attr("type") != "ResourceTrueTypeFont") { continue; }
				std::string file;
				float size = 0, dpi = 96;	// MyGUI: Size is in points at Resolution dpi (its default 96); the game's fonts all say 50
				for (const xml::Node* p : res->all("Property"))
				{
					if (p->attr("key") == "Source") { file = p->attr("value"); }
					else if (p->attr("key") == "Size") { size = strtof(p->attr("value").c_str(), nullptr); }
					else if (p->attr("key") == "Resolution") { dpi = strtof(p->attr("value").c_str(), nullptr); }
				}
				size *= dpi / 72;	// pixels at 1080 lines: Kenshi_PaintedTextFont_Medium 25 pt -> 17.4 px, as measured in game
				if (file.empty() || size <= 0) { continue; }
				auto& ttf = files[file];
				if (!ttf)
				{
					std::ifstream in(res::file("fonts/" + file), std::ios::binary);
					ttf = std::make_shared<std::vector<unsigned char>>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
					if (ttf->empty() || stbtt_GetFontOffsetForIndex(ttf->data(), 0) < 0) { fprintf(stderr, "kclient ui: unusable font file %s\n", res::file("fonts/" + file).c_str()); }
				}
				if (ttf->empty()) { continue; }
				const std::string name = res->attr("name");
				if (firstFont_.empty()) { firstFont_ = name; }
				fonts_[name] = FontAccess::make(name, file, size, ttf);
			}
			return !fonts_.empty();
		}

		void shutdown()
		{
			fonts_.clear();
			firstFont_.clear();
		}

		void setScale(float scale) { scale_ = scale; }

		const Font* get(const std::string& name)
		{
			auto it = fonts_.find(name);
			if (it != fonts_.end()) { return it->second.get(); }
			static std::set<std::string> warned;
			if (warned.insert(name).second) { fprintf(stderr, "kclient ui: no font %s\n", name.c_str()); }
			it = fonts_.find(firstFont_);
			return it == fonts_.end() ? nullptr : it->second.get();
		}

		std::vector<std::string> names()
		{
			std::vector<std::string> out;
			for (const auto& kv : fonts_) { out.push_back(kv.first); }
			return out;
		}
	}
}
