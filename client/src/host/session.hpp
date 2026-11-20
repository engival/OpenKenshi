#pragma once

// The host's session: the sokol init / cleanup callbacks, the characters step and the load steps, the sim link's start (kenshi-k16.2.369.7; the slice
// plan on kenshi-k16.2.369.2). Moved out of main.cpp unchanged. The App state is host/app.hpp's.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace host
{

// Defined in session.cpp
void init();
void cleanup();
bool init_characters();
void sim_opened();
void place_characters(const std::vector<std::string>& specs, double x, double z, double yawRad);
void place_blood(const std::vector<std::string>& specs, double x, double z, double yawRad);
void run_load_step();

// Defined in main.cpp, used by the session code
void init_view();
void init_ui();
void open_creator(const std::string& what);
void log_cb(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char* file, void* user);
extern int g_exitCode;
extern std::chrono::steady_clock::time_point g_quitAsked;

}
