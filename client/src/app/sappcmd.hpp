#pragma once

// Window commands from logic to sokol_app as a queue (render-thread hook 3, kenshi-k16.2.205.12.9; docs/RENDER_THREAD.md §6).
// Logic never calls sapp_lock_mouse / sapp_show_mouse / sapp_request_quit / sapp_set_clipboard_string / sapp_toggle_fullscreen
// itself: it queues here, and flush() applies them once at the end of frame(). With the render thread (P2) the flush moves to
// the sapp thread. Each command keeps its order; a later lockMouse overrides an earlier one in the same frame (only the last
// state matters to sokol), the others run in call order.
// Deferral: the window state a call changes is visible to logic a frame later (sapp_is_fullscreen after toggleFullscreen);
// the mouse lock was applied by sokol on the call, now at the frame's end, before the next event is read.
// The clipboard get is a published copy: sapp thread calls publishClipboard() on SAPP_EVENTTYPE_CLIPBOARD_PASTED,
// logic reads it with clipboard(). setClipboard also updates the copy, so a copy/paste inside the client round-trips at once.
#include <functional>
#include <string>

namespace sappcmd
{
void lockMouse(bool lock);
void showMouse(bool show);
void requestQuit();
void setClipboard(const std::string& text);
void toggleFullscreen();
// Any other call that must run on the sapp thread (an Xlib call such as windowgeom::resizeWindow: Xlib is not thread-safe, and sokol's loop
// owns the display), queued in order with the others (kenshi-k16.2.205.12.25).
void post(std::function<void()> fn);

// Applies every queued command in order and empties the queue. Call on the sapp thread (today the only thread), once per frame.
void flush();

// Refreshes the published clipboard copy from sokol (sapp thread only; call when the PASTED event arrives).
void publishClipboard();
// The published copy.
std::string clipboard();
}
