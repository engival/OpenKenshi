#pragma once
// debug.pick_mask (kenshi-k16.2.239.5.1): dev-only, built with KCLIENT_DEV_TOOLS (src/dev/).
#include <cstdint>
#include <functional>

namespace dev::pickmask
{
// What the mask asks of the game: main.cpp answers, so the mask runs the real pick (pick_at) and copies none of its rules.
struct Host
{
	std::function<uint64_t(double mx, double my)> pickId;	// the thing a click at that pixel picks (item, door, container, producer), 0 = none
	std::function<bool(uint64_t id, float box[4])> screenBox;	// the thing's drawn box on screen: x0 y0 x1 y1; false: no box (a fallback square about the cursor is used)
	std::function<uint64_t()> cameraStamp;	// changes with the camera's position, angles and the window size
};
bool on();	// the settings key debug.pick_mask
// Once a frame after the hover pick: the hovered thing (0 none) and the cursor. Recomputes when the thing or the camera changed; one pass is cut into
// slices of kSliceMs so a frame never stalls on it.
void update(const Host& host, uint64_t hovered, double mx, double my);
// In the UI layer: the cells whose pick is the hovered thing, as flat translucent rectangles (nothing with the key off or nothing hovered).
void draw();
}
