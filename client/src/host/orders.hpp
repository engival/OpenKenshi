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
std::string clicks_report();	// the F12 sidecar's "clicks:" block (dev/debughost.hpp's clicksReport)
bool click_replay(const std::vector<std::string>& words);	// the script op `click replay KEY=VALUE...`

bool set_follow_id(uint64_t id);
void leave_follow();
void update_follow(double dt);
void update_rts(double dt);

#ifdef KCLIENT_DEV_TOOLS
extern uint64_t g_forcePickThing;	// script `mouse force ID`: the next clicks pick thing ID (orders.cpp; the script op sets it)
#endif

// main.cpp
void post_event(uint32_t id, const char* name);
void forward_vec(double yaw, double pitch, double f[3]);
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
