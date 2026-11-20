#pragma once

// The host's logic frame: the door poses, the world path's logic (logic_paths), logic_frame and the logic thread's start
// (kenshi-k16.2.369.9; the slice plan on kenshi-k16.2.369.2). The App state is host/app.hpp's.

#include <cstdint>
#include <vector>

#include "app/config.hpp"
#include "input/inputqueue.hpp"
#include "render/arrows.hpp"
#include "render/camera.hpp"
#include "render/renderframe.hpp"

namespace host
{

// Defined in main.cpp, used by the logic frame
extern FramePacket gPackets[2];
extern const double kDoorMatch;
extern const double kScriptDt;
extern const double kStallMs;
extern const int kLoadsPerFrame;
extern const float kCreatorNear, kCreatorFar;
extern const cfg::Float kDustMotes;
extern const cfg::Bool kEyeAdapt;
extern const cfg::Int kFpsCap;
extern const cfg::Float kInteriorLight;
extern const cfg::Bool kInteriorMask;
extern const cfg::Float kLampBeams;
extern const cfg::Bool kLampOcclusion;
extern const cfg::Bool kLightProbes;
extern const cfg::Float kNavBand;
extern const cfg::Bool kNavOverlay;
extern const cfg::Bool kNavPruned;
extern const cfg::Float kNavStair;
extern const cfg::Bool kOccluderFade;
extern const cfg::Bool kOccluderGhost;
extern const cfg::Bool kShadowCache;
extern const cfg::Float kShadowSoft;
extern const cfg::Int kShadowStaticBudget;
extern const cfg::Float kStreamMs;
extern const cfg::Bool kStreamScripted;
extern const cfg::Int kStreamSliceMb;
extern const cfg::Float kSunBounce;
extern const cfg::Int kSunShaftsDebug;
extern const cfg::Float kSunShafts;
void camera_restore_frame();
Camera current_camera(float nearZ, float farZ);
void draw_hud();
void frame_await_target(FramePacket& pk, double dt);
void frame_loading(FramePacket& pk);
void frame_title(FramePacket& pk, double dt);
void hover_cursor();
void live_profile();
void live_squads();
void loading_shot();
void pick_mask_update();
bool place_session_camera();
void poll_local_server();
void resize_burst_frame();
void run_script(bool post);
void screenshot_world_frame();
void shot_pending_frame();
double sim_time();
void sky_ramp(double dt);
std::vector<Arrows::Mark> stealth_arrow_marks();
void take_weathers();
void update_camera(double dt);
void update_peek(double dt);
void update_session_load(double dt);
void update_ui(double dt);
void camera_basis(double f[3], double r[3], double u[3]);
RenderState& render_state();

// Defined in logicframe.cpp
void update_doors(double dt);
void logic_frame(FramePacket& pk);
void start_logic_thread();

}
