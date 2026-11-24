#pragma once
// The road editor, step 1 (kenshi-ix9r, parent kenshi-k16.2.513; user 2026-11-22: "something akin to a unity editor"): dev-only, built with KCLIENT_DEV_TOOLS
// (src/dev/). debug.roads draws the original's road network (kworld::RoadNetwork::load, the sim's own loader) over the terrain: one line through each road's
// markers, a small handle on each marker, a distinct mark on each road's two junction ends. The suspect markers (a marker whose two legs are more than twice
// the chord of its neighbours and turn back more than 140 degrees: the rules of desktop-findings/roads_export/hub_sanity/outliers.py), ranked by the extra path
// they add, can be flown to one after the other (debug.road_next / debug.road_prev, the `roads next|prev` script ops). The marker under the cursor is
// highlighted and described. Straight segments only: the curve option waits for oracle kenshi-rwh.301. Editing and saving are a later task.
#include <cstdint>
#include <string>

#include "render/roadribbons.hpp"
#include "ui/dev/roadspanel.hpp"

namespace kworld { class Heightmap; }

namespace dev::roadedit
{
// A camera pose for the host to apply (the free camera: eye, yaw and pitch in radians as App holds them).
struct Pose
{
	double eye[3] = {};
	double yaw = 0, pitch = 0;
};

bool on();	// the settings key debug.roads
void toggle();
void setOn(bool v);	// the user setter (saved), for the panel's close
// Moves the selection to the next (dir > 0) or previous suspect and gives the camera pose that frames it from above at an angle, both neighbours in view.
// false: the network did not load or has no suspects (the debug line says which). The first call selects suspect 1 (next) or the last (prev).
bool step(int dir, const std::string& dataDir, const kworld::Heightmap& hm, Pose& out);
// The ribbons for the packet (kenshi-qjzb): the whole network's strip mesh, built at the first call and then the same pointer; an empty frame while the overlay is off.
roadribbons::Frame ribbons(const std::string& dataDir, const kworld::Heightmap& hm);
// The draw (every logic frame with the overlay on, into the caller's overlay list): the lines, handles, junction marks, suspects, the selection and the hover.
void draw(const std::string& dataDir, const kworld::Heightmap& hm, const float mvp[16], const double camPos[3], float width, float height, double mouseX, double mouseY);
// Step 1b (kenshi-tqtd; user 2026-11-22: "a list of suspect points to investigate ... click the 3 suspect points to get some info"): the selection is any marker
// (road, marker), a suspect or not. selectSuspect is a list row (the camera pose to apply, as step); selectMarker a click on a handle (no camera).
// Marks (ui::RoadMark) are kept in memory per (road, marker) for the fixup table of step 2 (kenshi-wsm5).
bool selectSuspect(int index, const std::string& dataDir, const kworld::Heightmap& hm, Pose& out);
bool selectMarker(int road, int marker, const std::string& dataDir);	// false: no such marker
void clearSelection();
void click();	// a left click in the editor: the marker handle under the pointer (as of the last draw) is selected, a click on empty ground clears
bool mark(int ui_RoadMark);	// ui::RoadMark of the selected marker (kRoadUnmarked clears); false: nothing selected
std::string coords();	// "x y z" of the selection ("" when none): COPY
bool screenOf(int road, int marker, double& x, double& y);	// the handle's screen position as of the last draw (scripted clicks); false: not on screen
const ui::RoadsPanelState& panelState(const std::string& dataDir);	// the panel's view, rebuilt in place (the same suspects vector between mark changes)
std::string list();	// `roads list`: every suspect, one line each ("roads list N/M: ...", with its kind)
std::string marksDump();	// the marks, one printable line each ("roads marks: N" first)
// The debug text's line (the hovered marker, else the selected suspect): false when the overlay is off.
bool legend(std::string& line);
// `roads dump`: the selected suspect (road index, junction ids, marker, xyz, ratio, turn, extra) as one printable line; "roads: none selected" when none.
std::string dump();
// Counts for the dump / tests: roads, markers, suspects (0 before the first load).
std::string summary();
}
