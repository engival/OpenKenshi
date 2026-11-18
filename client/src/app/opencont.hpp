#pragma once
#include <cstdint>

// The open item windows besides the character's (kenshi-k16.20.13.2): a box, a looted body, a trader's stock, a left-clicked box shown alone.
// Each is a window of ui::ItemWindows keyed by its id. `user` is the player character that reached it (the window's owner for its orders).
// App holds them (App::conts) and the dev `dump windows` op reads them (kenshi-k16.2.153.19), so the record lives here.
struct OpenCont
{
	uint64_t id = 0;
	uint64_t user = 0;
	bool view = false;	// a left-clicked container / deposit shown alone: no user, no order of its own
	bool body = false;	// a KO'd or dead character (loot open): a character window of its own
	bool trade = false;	// the open TRADE's stock
	bool pinned = false;	// its pin (a pinned window survives its user's next order)
	uint32_t task = 0;	// the user's SimView::taskOrder when it opened: a newer move order to the user closes it (never the distance, rwh.78)
};
