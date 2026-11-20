#pragma once

// The host's HUD: the over-head progress bars and floating texts, the world's labels and sim markers, and the developer text block's lines
// (kenshi-k16.2.369.8; the slice plan on kenshi-k16.2.369.2). The App state is host/app.hpp's.

#include <cstdint>
#include <string>
#include <vector>

#include "assets/ogremesh.hpp"
#include "render/camera.hpp"
#include "render/renderframe.hpp"
#include "app/config.hpp"

namespace host
{

// One line of the top-left developer text block: its key (the F12 sidecar's "hud.<key>:" line), text and colour (kenshi-k16.2.335.1)
struct HudLine { std::string key, text; float r, g, b; };

struct HudBox { float x, y, w, h; };

// Defined in main.cpp, used by the HUD code
bool world_to_screen(const double p[3], float& x, float& y);
bool char_cut_hidden(uint64_t id, const double pos[3]);
Camera overlay_camera(const FramePacket& pk);
uint64_t camera_subject();
const char* compass(double deg);
const ogre::Mesh* move_marker();
extern const cfg::Bool kCharacterLabels;
extern const cfg::Bool kSelectionRing;
extern const cfg::Bool kRouteLine;
extern uint64_t hoverCage;

// Defined in hud.cpp
bool order_route_end(double out[3]);
uint8_t progress_level();
void progress_frame();
void world_labels(const float mvp[16]);
void draw_sim_markers(const FramePacket& pk);
std::vector<HudLine> hud_lines();

}
