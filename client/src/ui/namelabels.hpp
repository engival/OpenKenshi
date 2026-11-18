#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The player characters' name labels in the world (the original's CharacterNameTag, Kenshi_CharacterNamePanel.layout; kenshi-inspect
// ui_captures/floating_text.md and s54_static.md 3). A panel 32 units high centred on the host's projected point (the character + hull height x
// scale + 5 world units up), the same size at any distance, no fade: the name in Kenshi_StandardFont_Large, centred, shadowed; unselected the plain
// name in NameTagUnselected (#b3b3b3), selected "[Name]" in NameTagSelected (#ffffff). In sneak mode the stealth icon on its left, tinted by
// detection (hidden / almost seen / seen), and the light-level icon on its right (frame (1 - light) x 6). With names off only the icons show. Under
// every other widget, never taking the mouse. Host-fed: which characters (the player faction's), the distance cut (~10 950 units from the camera),
// F7 (toggle_bar) and the "Show character names" option are the host's; a label is gone when the host stops passing it.
namespace ui
{
	struct NameLabel
	{
		uint64_t id = 0;
		std::string name;
		bool selected = false;
		float x = 0, y = 0;	// framebuffer pixels: the panel's centre
		int stealth = 0;	// 0 not sneaking (no icons), 1 hidden, 2 almost seen, 3 seen
		float light = 1;	// 0..1, the light-level icon while sneaking
	};

	class NameLabels
	{
	public:
		NameLabels();
		~NameLabels();
		void update(std::vector<NameLabel> labels, bool names = true);	// every frame, before ui::frame; names false: the icons only
		// For tests: last frame's panel, its text box (empty when the name is off) and the drawn text and colour.
		Rect placed(uint64_t id) const;
		Rect textBox(uint64_t id) const;
		std::string text(uint64_t id) const;
		Color colour(uint64_t id) const;

	private:
		class Layer;
		Layer* layer_ = nullptr;
	};
}
