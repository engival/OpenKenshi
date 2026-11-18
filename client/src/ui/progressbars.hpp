#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// Progress bars in the world (the original's Kenshi_ProgressBarPanel, kenshi-inspect's progbar.md / ui28_static.md): a thin floating panel
// 215 x 33 units (0.112 x 0.0306 of the screen, the same at any camera distance) holding a Kenshi_ProgressBarFill across it and centred,
// shadowed text (#ded8ce) over the bar. The game's tasks: lockpicking, unlocking, locking, prospecting, adding materials, training, restraining,
// cutting shackles (not building, repairing or dismantling); the player faction's characters only, one per task. Under every other widget,
// never taking the mouse. The host projects the character's position (no height offset) every frame: the panel's top centre, unclamped. A bar
// is gone when the host stops passing it. Bars overlap as in the game (one per character).
// Floating texts beside them (the game's ScreenLabel, kenshi-inspect floating_text.md: damage, "Pick success!", "Shackles broken!"): the host's
// text, colour and font (Kenshi_FloaterFont_Small / Medium / Large), top left at the host's point (the anchor + age world units up), faded from
// age 8 to none at 12, not drawn from 12. Age is the game's: speed units (age += speed x dt; speed 4, 8 or 10 by the caller), the host's.
namespace ui
{
	struct WorldProgress
	{
		uint64_t id = 0;	// the target
		std::string text;	// the host's ("Lockpicking (Chance 90.0%)")
		float progress = 0;	// 0..1
		float x = 0, y = 0;	// framebuffer pixels: the panel's top centre
	};

	struct WorldFloatText
	{
		uint64_t id = 0;
		std::string text;
		Color colour;	// the host's: the game's green (0.4, 0.8, 0.4, 0.9), red (0.7, 0.4, 0.4, 0.7)
		float x = 0, y = 0;	// framebuffer pixels: the text's top left
		float age = 0;	// the game's speed units (life 12 / speed seconds)
		int font = 0;	// 0 Kenshi_FloaterFont_Small (20 px at 1080), 1 Medium (22), 2 Large (26)
	};

	// A Kenshi_ProgressBarFill (the track, the fill to progress, the frame) over bar, pixels; returns the fill drawn. Between gfx::begin and gfx::end.
	// The track and fill keep the template's edge offsets (units), as MyGUI's Stretch align does, so they hug the frame at any bar size.
	Rect drawProgressBar(const Rect& bar, float progress, float scale);

	// Where a world bar's Kenshi_ProgressBarFill sits in its panel (pixels): the themed Kenshi_ProgressBarPanel.layout's ProgressBar.
	Rect progressBarPlace(const Rect& panel);

	class ProgressBars
	{
	public:
		ProgressBars();
		~ProgressBars();
		void update(std::vector<WorldProgress> bars);	// every frame, before ui::frame
		void floatTexts(std::vector<WorldFloatText> texts);	// every frame, as update
		Rect placed(uint64_t id) const;	// where a bar's panel was last drawn (pixels); empty when not shown. For tests.
		Rect filled(uint64_t id) const;	// its fill
		Rect floatPlaced(uint64_t id) const;	// a floating text's box; empty when not drawn
		float floatAlpha(uint64_t id) const;	// its drawn alpha

	private:
		class Layer;
		Layer* layer_ = nullptr;
	};
}
