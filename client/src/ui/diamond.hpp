#pragma once
#include <functional>
#include <string>

#include "ui/widget.hpp"

// A diamond button (docs/UI.md "HUD"; the original's floor selector and squad move speed, user screenshots 2026-11-03): a square turned 45°
// over its rect, an arrow tab at each tip of one axis (vertical: up and down, horizontal: left and right), a label or a picture in the middle.
// Each half of the axis is its own button; the hit area is the diamond, not its rect, so two diamonds may overlap like tiles. Our own drawing
// in every style (deferred: the game's skin in ui.style=original, once kinspect names it).
namespace ui
{
	class Diamond : public Widget
	{
	public:
		explicit Diamond(bool vertical = true, std::string font = "Kenshi_StandardFont_Medium");
		bool vertical;
		std::string text;	// centred; empty: none
		const Font* font;
		std::function<void(const Rect& px, const Color& ink)> picture;	// drawn in the middle (pixels), after the text
		bool firstEnabled = true, secondEnabled = true;	// up / left, down / right; off: the arrow drawn disabled, no click
		std::string firstTip, secondTip;	// the halves' tooltips; empty: tooltip
		bool active = false;	// a mode it stands for is on (the HUD: direct drive): the rim drawn in Accent, the face lit
		std::function<void(int side)> onStep;	// -1 the first half (up / left), +1 the second (down / right)

		int side(float x, float y) const;	// pixels: -1 / +1 for a point in the diamond, 0 outside it
		Widget* hit(float x, float y) override;
		bool onPress(Btn b, float x, float y) override;
		void onRelease(Btn b, float x, float y) override;
		void onClick(Btn b) override;
		std::string tip() const override;

	protected:
		void paint() const override;

	private:
		int pressed_ = 0, released_ = 0;
	};
}
