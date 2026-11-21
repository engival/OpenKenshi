#pragma once

// The host's script runner (kenshi-k16.2.369.6; the slice plan on kenshi-k16.2.369.2): exec_cmd, the one command of the scripted input language
// (docs/CLIENT.md) the logic frame runs, and the stall points it arms. Moved out of main.cpp unchanged. The dev-only ops stay inside exec_cmd's
// else-if chain, fenced by KCLIENT_DEV_TOOLS there: they share its locals (e, a, c), so they are not a self-contained block.

#include <cstdint>
#include <string>
#include <vector>

#include "dev/debughost.hpp"
#include "host/app.hpp"
#include "input/controlhints.hpp"
#include "sokol_app.h"

namespace host
{

void exec_cmd(const Cmd& c);	// run one script command now (a world frame's input ops, the console, the viewer)
void stall_point(int at);	// the `stall MS gate|b` op's sleep: 0 just before the world frame's gate, 1 just after it (main.cpp's frame calls it)

// main.cpp
[[noreturn]] void exit_now(int code);
void request_quit();
void script_error(int frame, const char* op);
void script_error(const Cmd& c);
sapp_keycode key_by_name(const std::string& n);
uint64_t named_character(const std::string& text);
void free_camera();
void teleport(double x, double z);
void set_weather(const std::string& name);
void creator_slider(const std::string& name, float v);
// The creator preview's orbit (kenshi-k16.2.433.1; the UI panel is the ui agent's, the camera is the host's): a drag of dx / dy pixels in the
// preview area turns the character and the platform about the pivot (creator_turn takes the same in radians), the wheel zooms toward the point
// under the pointer and makes it the pivot, and creator_point_at puts the pointer on a bone (the scripted shots).
void creator_drag(float dx, float dy);
void creator_turn(float yaw, float tilt);
void creator_zoom(float notches, double mx, double my);
bool creator_point_at(const char* what);
bool char_cut_hidden(uint64_t id, const double pos[3]);
uint64_t camera_subject();
dev::Context& dev_context();
hints::Context hints_context();
void portrait_gesture(uint64_t id, hints::Gesture g);
App::OpenCont* cont_scripted(bool trade);
bool title_shown();
enum class Mode { kTitle, kCreator, kGame };
Mode app_mode();
std::string title_status();
bool title_hold_dev(int button);

}
