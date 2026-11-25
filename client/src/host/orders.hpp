#pragma once

// The host's orders and camera modes (kenshi-k16.2.369.5; the slice plan on kenshi-k16.2.369.2): the per-click record and its replay, sim_click's
// order dispatch, the collision boom and the follow / direct drive and RTS cameras. Moved out of main.cpp unchanged; the state they read is
// host/app.hpp's. The camera helpers that stay (set_follow, set_rts, send_drive, ...) are declared here for orders.cpp.

#include <cstdint>
#include <string>
#include <vector>

#include "host/app.hpp"
#include "kworld/coords.hpp"

namespace host
{

// The two camera constants of main.cpp's camera block that update_camera and update_rts share.
constexpr double kHalfWorld = -kworld::kWorldMin;
constexpr double kFastFactor = 10.0;	// shift

void sim_click(double mx, double my, bool right, bool shift);
bool foreign_container_walk(uint64_t id, bool queue);	// a right click without ALT on someone else's container: the HUD hint and (orders.foreign_walk_to) the walk there
bool foreign_walk_on();	// orders.foreign_walk_to
std::string clicks_report();	// the F12 sidecar's "clicks:" block (dev/debughost.hpp's clicksReport)
bool click_replay(const std::vector<std::string>& words);	// the script op `click replay KEY=VALUE...`

// Camera follow is a lock on one of the player's own characters (user 2026-11-22, kenshi-k16.2.236.13 Q4). A non-squad target is refused (false) unless
// the session is a spectator's (no player faction: the watch is ours, it has no squad) or devRoute is set and the run is --debug / scripted (the
// --follow flag and the script's `follow char`: dev/test-only routes). The refused target keeps its other effects (the caller's inspect focus).
bool set_follow_id(uint64_t id, bool devRoute = false);
// A cast row's double click or the FOLLOW / STOP button (0 stops the camera, keeps the watch): the watch is always set, the camera follows only
// where set_follow_id allows it (the player's own squad; a spectator's cast).
void cast_follow(uint64_t id);
void leave_follow();
bool floor_step(int dir);	// R / F / the diamond: ignored in drive (D2); ends a follow (L4, L5) and steps the free floor; true when the level changed
void update_follow(double dt);
void update_rts(double dt);

Pick force_pick_thing(Pick p);	// script `mouse force ID`: p becomes that thing; p unchanged when none is forced (always in a dev-off build)

#ifdef KCLIENT_DEV_TOOLS
extern Pick g_forcePickGround;	// script `groundclick`: sim_click picks this ground point (orders.cpp)
extern uint64_t g_forcePickChar;	// script `charclick`: sim_click picks this character (orders.cpp)
extern bool g_forcePickLabel;	// script `thingclick label` (orders.cpp)
extern uint64_t g_forcePickThing;	// script `mouse force ID`: the next clicks pick thing ID (orders.cpp; the script op sets it)
#endif

// main.cpp
void post_event(uint32_t id, const char* name);
void forward_vec(double yaw, double pitch, double f[3]);
bool fly_owns_space();	// Space flies the free camera only with no sim (--fly); in a game it stays pause (kenshi-2n7o)
bool free_flying();	// the free camera is the one in use (no follow, RTS or walk): the fly keys steer it alone (kenshi-wlql)
void clamp_free_camera();	// lifts the free camera to the terrain + a small clearance
bool held(const char* action);
bool ctrl_held();
const char* camera_mode_name();
double wrap_angle(double a);
bool driving();
void send_drive(uint8_t drive, double heading, int strafe);
void set_follow(bool on);
void set_rts(uint64_t center);
App::OpenCont* cont_find(uint64_t id);
void cont_add(const App::OpenCont& c);

}
