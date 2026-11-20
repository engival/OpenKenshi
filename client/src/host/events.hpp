#pragma once

// The host's window events (kenshi-k16.2.369.12; the slice plan on kenshi-k16.2.369.2): sokol's event callback and its queue, the input queue the
// logic frame drains, the bound key actions and the selection box. Moved out of main.cpp unchanged; the App state is host/app.hpp's.

#include <cstdint>
#include <string>

#include "input/inputqueue.hpp"
#include "sokol_app.h"

namespace host
{

extern InputQueue gInput;	// sokol_app's events, drained at the top of frame() (render-thread hook 1, inputqueue.hpp)
extern bool gDraining;	// logic thread: event() is running a queued event (queue_event stored its pointer already, newer ones may follow)
extern bool gFrameRan;	// sokol delivers the first loop's events before init(); those still go straight to event(), as before. Written and read on the sapp thread only (frame() sets it, kenshi-k16.2.205.12.23)
void queue_event(const sapp_event* e);	// sokol's event callback: queues after the first frame, else calls event() at once
void event(const sapp_event* e);	// one window event, applied now (the drain's and the script's injected events)
void debug_toggle(const std::string& id);	// a debug toggle by its key action's name (key_actions and the debug panel's toggle rows)

// Defined in main.cpp, used by the event handler
bool menu_shown();
bool held(const char* action);
void follow_retarget();
void ensure_weathers();
void apply_weather();
#ifdef KCLIENT_DEBUG_VIEW
void debug_tree_open(uint8_t kind, uint64_t id);
#endif

}
