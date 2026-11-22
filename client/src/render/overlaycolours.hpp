#pragma once
// The debug overlays' shared colour table and range (user 2026-11-21, kenshi-k16.2.500; the table is written once, in docs/CLIENT.md "Debug overlays":
// change both together). The navmesh overlay (render/navoverlay.cpp), the collision overlay (dev/colloverlay.cpp) and kinspect's overlays
// (kenshi-cym.40.1.7) use these values. RGBA; the alpha is multiplied by the fade.

struct OverlayRgba { float r, g, b, a; };

namespace overlay_colour
{
	// collision shapes, by kworld::WorldShape (colloverlay.cpp classifies; first match wins in this order)
	constexpr OverlayRgba kDoor{ 1.00f, 0.90f, 0.20f, 0.90f };	// yellow: NavUse::Door, a doorway leaf
	constexpr OverlayRgba kFoliage{ 0.75f, 0.55f, 0.25f, 0.80f };	// brown: a generated rock / tree shape
	constexpr OverlayRgba kStairs{ 1.00f, 0.55f, 0.10f, 0.90f };	// orange: `solid` (a stair or gateway body, filled by column)
	constexpr OverlayRgba kFurniture{ 0.62f, 0.62f, 0.62f, 0.80f };	// grey: a layout item / `is interior furniture`, or a thin prop (rail, pole, sign)
	constexpr OverlayRgba kBarricade{ 1.00f, 0.35f, 0.30f, 0.90f };	// red: NavUse::Obstacle, solid and not stood on
	constexpr OverlayRgba kProjected{ 0.90f, 0.40f, 0.85f, 0.85f };	// magenta: NavUse::Projected, a 2D footprint cut through all heights
	constexpr OverlayRgba kBuilding{ 0.70f, 0.85f, 1.00f, 0.90f };	// bluish white: NavUse::Walkable, a wall / floor / roof
	constexpr OverlayRgba kTerrain{ 0.30f, 1.00f, 0.35f, 0.55f };	// green: the drawn terrain grid
	// navmesh edges
	constexpr OverlayRgba kNavInner{ 0.10f, 0.90f, 1.00f, 0.55f };	// cyan: an inner edge between two faces
	constexpr OverlayRgba kNavSeam{ 0.60f, 0.50f, 1.00f, 0.30f };	// violet: a tile seam
	constexpr OverlayRgba kNavSplit{ 0.10f, 0.90f, 1.00f, 0.22f };	// the cyan, fainter: an edge splitting one flat face (corner cap), a coplanar inner edge
	constexpr OverlayRgba kNavRim{ 1.00f, 0.95f, 0.40f, 0.90f };	// yellow: no neighbour (a wall, a cliff, a gap)
	constexpr OverlayRgba kNavStair{ 1.00f, 0.55f, 0.10f, 0.90f };	// orange: a stair or ramp
}

// The range (debug.overlay_range, the DEBUG SETTINGS slider; default 300 u, the one-room radius kinspect uses): both overlays draw only what lies within it
// of the centre (the selected character, else the camera focus), the fade easing from full at half of it to nothing at it. 0: no limit, the fade at 900 u.
float overlay_range();
