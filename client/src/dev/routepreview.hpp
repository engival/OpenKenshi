#pragma once
// debug.route_preview (kenshi-k16.2.331.2): dev-only, built with KCLIENT_DEV_TOOLS (src/dev/). With one character selected the client plans the
// move_to route the cursor's ground point would get, with the sim's own planner (knav::planRoute, the same call MeshNavigator::routeFrom makes) on
// the client's nav tiles, and draws it. A cursor that barely moved while the route length jumped, and a click whose server route differs from the
// preview for that goal, are WARN lines in the events file and on stderr (docs/PLAYER_GUIDE.md "Route preview").
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ClientNav;

namespace dev::routepreview
{
// The selected character as the click would send it: its interpolated wire position and its race's routing body.
struct Subject
{
	uint64_t id = 0;
	double from[3] = {};
	double radius = 0.0;
	std::optional<float> water;
};

bool on();	// the settings key debug.route_preview
// Once a frame after the hover pick. subject: null when not exactly one character is selected; hit: the cursor's ground point (null: none under it).
void update(const ClientNav& nav, const Subject* subject, const double* hit, double tick);
// A move_to click was sent (seq): plan for that goal now and keep the plan until the server's route for seq arrives.
void onOrder(const ClientNav& nav, const Subject& subject, const double goal[3], uint32_t seq, double tick);
// The server's CHAR_ROUTE for seq arrived (points as the wire has them; flags bit 1 approach); now: the character's wire position at that moment.
void onServerRoute(const ClientNav& nav, uint32_t seq, const std::vector<std::array<float, 3>>& points, uint8_t flags, const double now[3], double tick);
// What the overlay draws: the preview's points (empty: none) and whether it ends at an approach point.
const std::vector<std::array<float, 3>>& points();
bool approach();
// The debug line: "" with the preview off, else the length or the planner's failure check.
std::string status();
}
