#pragma once
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The status panel's stats as a table of icons (user idea 2026-11-03; a kui_demo mockup, `--show statsmock`, not in the HUD yet): rows of cells,
// each an icon over its value, a note under it (the modifier, or a unit) and an optional XP strip. It would replace the original's stats rows
// and the ^ extended rows in one panel. Icons are our own drawing (the game has none for skills), in the cell's ink.
namespace ui
{
	enum class StatIcon { kFist, kSword, kShield, kCrossbow, kCrosshair, kEye, kArm, kBolt, kBricks, kDodge, kRun, kKnockout, kHunger };

	struct StatCell
	{
		StatIcon icon = StatIcon::kFist;
		std::string value;	// "24"
		std::string note;	// under the value: "+4", "kph"; empty: none
		std::string noteColour;	// a palette name ("HudGood" / "HudBad"); empty: the label ink
		float xp = -1;	// 0..1 to the next level, a strip at the cell's foot; < 0: none
		std::string tip;	// "Martial Arts: 24 (+4)"
		std::string label;	// "Str": drawn in the icon's place when the grid shows labels
	};

	class StatGrid : public Widget
	{
	public:
		StatGrid();
		std::vector<std::vector<StatCell>> rows;	// each row splits the width evenly
		float cellH = 72;
		bool labels = false;	// short text labels instead of the icons (for players who want no generated art)

		Size preferred() const override;
		std::string tip() const override;
		const StatCell* cellAt(float x, float y) const;	// pixels; nullptr outside every cell

	protected:
		void paint() const override;
	};

	void drawStatIcon(StatIcon icon, const Rect& px, const Color& ink);	// in a square box, pixels
}
