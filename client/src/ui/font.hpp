#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui/gfx.hpp"

// The game's TrueType fonts (gui/fonts/kenshi_fonts.xml -> TTF files beside it), rasterised with stb_truetype. Read from the install at run time and
// never bundled: Sentencia_1 is not freely licensed (docs/UI.md). A font's "Size" is taken as its pixel height on a 1080-line screen at UI scale 100;
// the glyph page is re-baked whenever the pixel size changes. Codepoints: 32-126, 160-255 and 8192-8303 (dashes, quotes, ellipsis); others draw as '?'.
// Text is UTF-8; a byte that does not start a valid sequence is read as Latin-1.
namespace ui
{
	class Font
	{
	public:
		~Font();

		const std::string& name() const { return name_; }
		// Pixel metrics at the current UI scale.
		float lineHeight() const;
		float ascent() const;
		float width(std::string_view text) const;
		// Index into text of the first character that does not fit in maxW pixels (text.size() when all fit).
		size_t fit(std::string_view text, float maxW) const;
		// Word-wrapped lines of at most maxW pixels; '\n' breaks; a word longer than a line is split.
		std::vector<std::string_view> wrap(std::string_view text, float maxW) const;
		// One line with its top at y (the line box, not the baseline), snapped to whole pixels.
		void draw(float x, float y, std::string_view text, const Color& c) const;

	private:
		friend struct FontAccess;
		struct Page;

		std::string name_, file_;
		float size_ = 0;
		std::shared_ptr<std::vector<unsigned char>> ttf_;
		mutable std::unique_ptr<Page> page_;

		const Page& page() const;
	};

	namespace fonts
	{
		bool load();	// fonts/kenshi_fonts.xml (res::file); false when no font is usable
		void shutdown();
		void setScale(float scale);		// pixels per UI unit; pages re-bake lazily
		// "Kenshi_StandardFont_Medium" etc.; falls back to the first loaded font (logged once), null only when none loaded.
		const Font* get(const std::string& name);
		std::vector<std::string> names();
	}

	// Next codepoint of UTF-8 text at i, advancing i.
	unsigned utf8Next(std::string_view s, size_t& i);
}
