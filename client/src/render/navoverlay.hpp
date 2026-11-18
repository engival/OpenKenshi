#pragma once
#include <functional>

namespace knav { class NavMesh; }

// The navmesh debug overlay (debug.navmesh, user 2026-11-03; docs/CLIENT.md "Navmesh overlay"): the ClientNav polygons' edges as lines over
// everything, fading out with the distance from the camera focus. Edges with no neighbour are bright (a gap or the mesh's rim), tile seams
// faint, and door polygons coloured by state. Draws with ovl:: (overlaylist.hpp: recorded on logic into the packet's overlay list, render sitting 11) in
// screen pixels; mvp is the camera's, relative to cam.
// Height bands (user 2026-11-04, multi-floor buildings): ref = the reference height (the selected character's feet, else the focus ground). Within
// band of it: full strength, its polygons filled faintly; easing over the next band to: below dimmed and greyed, fading out over kDepth bands; above
// clearly shown, fainter where cut (a floor or roof cut away: WorldBuildings::cutAway) but never hidden (stairs up stay clickable).
// The surface under `at` (user 2026-11-04): flooded from the polygon there across neighbours whose slope differs by under `flat` degrees, filled.
// Stairs and ramps (slope >= `stair` degrees) are orange, filled, at full strength within kStairRange bands of ref whatever the level or the cut.
// trace: print each stair / ramp polygon's centre, screen point and fill weight once (script `dump navdraw`).
// pruned: also the edges of polygons the bake pruned (flags 0: no seed reaches them), dim red; else they are not drawn.
// hover: hoverAt is the mouse ray's ground hit (the click point); its polygon is drawn white over everything.
struct NavBands { double ref = 0, band = 20, at[3] = {}, hoverAt[3] = {}; bool hover = false, pruned = false, trace = false; float stair = 30, flat = 10; std::function<bool(const double p[3])> cut; };
void draw_navmesh(const knav::NavMesh& mesh, const float mvp[16], const double cam[3], const double focus[3], float width, float height, const NavBands& bands);
// Debug (script `dump nav X Z`): every polygon holding (x, z) in XZ, any height: tile, index, centre, slope, area, flags, vertex count (stdout).
#ifdef KCLIENT_DEV_TOOLS
void dump_navmesh_at(const knav::NavMesh& mesh, double x, double z);
#endif
