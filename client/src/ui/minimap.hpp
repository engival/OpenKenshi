#pragma once
#include <functional>

#include "ui/overview.hpp"

// The minimap (docs/UI.md "Minimap"; user 2026-11-20, kenshi-k16.2.436): a small square window, frameless (the panel's border, no header) and
// without any text. The host's map image (MapActions::draw, as the overview's MAP page) centred on the camera, the host's markers over it: towns
// as icons (no names), the player's characters as dots, the camera's facing wedge and arrow at the centre. A left drag anywhere on it moves it
// (a click without movement does nothing: the host may use clicks later); the wheel over it steps the zoom level. Both end in an action, so the
// host keeps the place and the level across sessions and passes them back in the state.
//
// Host use, every frame while it shows:
//	ui::Minimap mm(actions);	// once, after ui::init; hidden until show(true)
//	mm.show(true);
//	mm.update(state);	// cheap when nothing changed
namespace ui
{
	struct MinimapState
	{
		MapState map;	// the markers (places, characters, the camera and its yaw); the map is centred on (map.cameraX, map.cameraZ).
				// extent and texels are unused here. Places draw as icons only (name and tip ignored).
		int level = 1;	// the zoom level, 0 (closest) .. kLevels - 1; the span shown is span(level). Out of range: clamped.
		float x = -1, y = -1;	// the window's top-left, units from the screen's top-left (1 unit = 1 px at 1080 lines); x < 0: the default
					// place (top right, under the HUD's speed panel). Kept on the screen whatever the size (a resize clamps it again).
		bool operator==(const MinimapState&) const = default;
	};

	struct MinimapActions
	{
		std::function<void(const MapFrame&)> draw;	// the map image for the frame: as MapActions::draw (render side, at the list's replay)
		std::function<void(int level)> level;	// the wheel stepped the level (the window shows it at once): the host saves it
		std::function<void(float x, float y)> moved;	// a drag ended: the new top-left, units as MinimapState::x / y (on the screen)
	};

	class Minimap
	{
	public:
		static constexpr int kLevels = 5, kDefaultLevel = 1;
		static constexpr float kSize = 256, kMargin = 16, kTop = 97;	// units: the side; the default place's right margin and top (0.09 of 1080)
		static double span(int level);	// world units across at a level: 4608 x 2^level (level 1: 9216 u, ~0.9 km; 3: the old 3.7 km)

		explicit Minimap(MinimapActions actions = {});
		~Minimap();
		Minimap(const Minimap&) = delete;
		Minimap& operator=(const Minimap&) = delete;
		void update(const MinimapState& s);	// a changed level / place in s replaces the window's own (a drag or wheel since keeps its own until then)
		void show(bool on);
		bool shown() const;
		int level() const;	// the level showing
		float x() const;	// the top-left showing, units (after layout: clamped on the screen)
		float y() const;
		// For scripted tests.
		Widget* window() const;
		Rect mapRect() const;	// the map's square inside the frame, pixels
		MapFrame frame() const;	// the view the host gets
		Rect marker(size_t place) const;	// place i's icon, pixels; empty while outside the map

	private:
		class View;
		View* view_ = nullptr;
		MinimapActions act_;
		MinimapState last_;
	};
}
