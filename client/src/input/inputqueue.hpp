#pragma once

// The window's input events as a queue (render-thread hook 1, kenshi-k16.2.205.12.4; docs/RENDER_THREAD.md §6 "Input events").
// sokol_app's event callback only pushes; the frame drains the queue into the client's event handler before any other work.
// Today one thread does both. sokol's loop delivers every pending event and then calls the frame callback, so the handlers
// run in the same order against the same state as when event_cb called them directly. With the render thread (P2), the push
// stays on sokol_app's thread and the drain moves to the top of the logic thread's frame. Hence the lock, which is uncontended
// on one thread. Events a script injects still call the handler directly; they are already on the logic side.
// sapp_event is a POD, so it is copied by value; the queue holds no pointers into sokol's state.
#include <atomic>
#include <mutex>
#include <vector>

#include "sokol_app.h"

class InputQueue
{
public:
	void push(const sapp_event& e)
	{
		std::lock_guard<std::mutex> lock(m_);
		pending_.push_back(e);
	}

	// Calls fn(const sapp_event&) for every event pushed so far, oldest first. The lock is released before the handlers run,
	// so a handler may push without deadlocking. An event pushed during the drain waits for the next one.
	template <typename Fn>
	void drain(Fn&& fn)
	{
		{
			std::lock_guard<std::mutex> lock(m_);
			taken_.swap(pending_);
		}
		for (const sapp_event& e : taken_) { fn(e); }
		taken_.clear();	// keeps its capacity: no allocation once the queue has seen its largest frame
	}

private:
	std::mutex m_;
	std::vector<sapp_event> pending_;
	std::vector<sapp_event> taken_;	// the drain's buffer (drain is called from one thread only)
};

// The pointer at its newest position (kenshi-k16.2.205.12.97, overlap review .85 section 1.5 / C2). queue_event stores it on sokol_app's thread the
// moment a mouse move arrives, before logic drains the event; a script's injected move and the menu's reposition store it from the logic side. The
// draw puts the cursor there (the packet's copy is a logic frame old in overlap, and a kept frame would freeze it). Off and lockstep: the same value
// event() puts in app.mouseX / mouseY by the time the frame draws, so nothing moves.
namespace pointer
{
inline std::atomic<float> x{ 0.0f }, y{ 0.0f };
inline void set(float px, float py) { x.store(px, std::memory_order_relaxed); y.store(py, std::memory_order_relaxed); }
}
