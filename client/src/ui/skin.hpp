#pragma once
#include <map>
#include <string>
#include <vector>

#include "ui/gfx.hpp"

// The game's MyGUI resources read from the install: skins (gui/skins/*.xml), image sets (gui/images), named colours (gui/colours). docs/UI.md.
namespace ui
{
	// Widget visual state; checked is separate (MyGUI's "*_checked" states).
	enum class State { kNormal, kHighlighted, kPushed, kDisabled };

	struct Skin
	{
		// MyGUI align of a sub-skin inside the widget when it is not the skin's own size.
		enum Align : unsigned { kLeft = 1, kRight = 2, kHCenter = 0, kHStretch = 3, kTop = 4, kBottom = 8, kVCenter = 0, kVStretch = 12 };
		struct Part
		{
			enum class Kind { kImage, kTiled, kText } kind = Kind::kImage;
			Rect offset;		// skin units, at the skin's own size
			unsigned align = kLeft | kTop;
			bool has[8] = {};	// per state index (state + 4 * checked)
			Rect texels[8];		// kImage / kTiled
			Color colour[8];	// kText
			float tileW = 0, tileH = 0;
		};

		std::string name;
		float w = 0, h = 0;	// skin size in units
		std::vector<Part> parts;

		// The parts in r (pixels); scale = pixels per skin unit. Edges keep their size, stretched parts take the rest.
		void draw(const Rect& r, float scale, State s, bool checked, const Color& tint = {}) const;
		// The text area and colour, when the skin has a SimpleText / EditText part.
		bool text(const Rect& r, float scale, State s, bool checked, Rect& area, Color& colour) const;
	};

	struct ImageSet
	{
		float w = 0, h = 0;			// frame size (first group)
		std::map<std::string, Rect> frames;	// index name -> atlas texels (first frame)
	};

	namespace res
	{
		// Where gui files are read from: <dataDir>/gui, then each theme folder's gui/ on top (a mod folder such as Dark UI's); per file, the
		// last folder that has it wins, so a theme replaces only the files it ships. Set by ui::init before anything loads.
		void setRoots(const std::string& dataDir, const std::vector<std::string>& themes);
		std::string dataFile(const std::string& rel);	// "icons/x.png" -> <dataDir>/icons/x.png (game data outside gui/: item icons)
		std::string file(const std::string& rel);	// "skins/kenshi_skins.xml" -> path of the top-most copy (the base path when none has it)
		bool themed(const std::string& rel);	// a theme folder ships rel (it replaces the base copy)
		// Loads every skin, image set and colour file; false when the skins are unusable.
		bool load();
		const Skin* skin(const std::string& name);	// "Kenshi_Button1Skin" or "Kenshi_Button1"; null (logged once) when absent
		const ImageSet* images(const std::string& name);
		// Named colour of the active style ("Main", "BadBright", ...); def when absent.
		Color colour(const std::string& name, const Color& def = {});	// a palette name, or "#rrggbb[aa]"
		// The game's kenshi_colours.xml value; def when absent.
		Color fileColour(const std::string& name, const Color& def = {});
		std::vector<std::string> skinNames();
	}
}
