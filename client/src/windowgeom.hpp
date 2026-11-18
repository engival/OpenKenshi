#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

// Window geometry memory (kenshi-k16.2.109, docs/PLAYER_GUIDE.md "Window position"): kclient.cfg keeps the window's rect, monitor and fullscreen
// state under a fingerprint of the desktop's monitor layout, so a laptop on its own screen and the same laptop docked each get their own entry.
// sokol_app has no window-position or monitor API (and its headers are never modified), so the X11 half (windowgeom.cpp, always X11: also
// under XWayland) works on sapp_x11_get_display() / sapp_x11_get_window(). The pure half is here, header-only, so a test links nothing.
namespace windowgeom
{
	struct Monitor
	{
		std::string name;
		int x = 0, y = 0, w = 0, h = 0;
	};

	struct Rect
	{
		int x = 0, y = 0, w = 0, h = 0;
		bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
	};

	// FNV-1a over the monitors sorted by (name, w, h, x, y), so the order the server lists them in does not matter. 16 hex characters.
	inline std::string fingerprint(std::vector<Monitor> mons)
	{
		std::sort(mons.begin(), mons.end(), [](const Monitor& a, const Monitor& b) { return std::tie(a.name, a.w, a.h, a.x, a.y) < std::tie(b.name, b.w, b.h, b.x, b.y); });
		uint64_t h = 1469598103934665603ull;
		auto feed = [&h](const std::string& s) { for (unsigned char c : s) { h = (h ^ c) * 1099511628211ull; } };
		for (const Monitor& m : mons)
		{
			feed(m.name + " " + std::to_string(m.w) + "x" + std::to_string(m.h) + "+" + std::to_string(m.x) + "+" + std::to_string(m.y) + "\n");
		}
		char buf[17];
		snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
		return buf;
	}

	// "x,y,w,h" (x and y may be negative on a monitor left of or above the primary); false for anything else, or a size under 1x1.
	inline bool parseRect(const std::string& s, Rect& r)
	{
		int x, y, w, h, n = 0;
		if (sscanf(s.c_str(), "%d,%d,%d,%d%n", &x, &y, &w, &h, &n) != 4 || (size_t)n != s.size() || w < 1 || h < 1) { return false; }
		r = { x, y, w, h };
		return true;
	}

	inline std::string formatRect(const Rect& r) { return std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," + std::to_string(r.h); }

	// The monitor holding the rect's centre, or nullptr: a remembered window is restored only when it would still be on a connected screen.
	inline const Monitor* monitorAtCentre(const std::vector<Monitor>& mons, const Rect& r)
	{
		const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
		for (const Monitor& m : mons)
		{
			if (cx >= m.x && cx < m.x + m.w && cy >= m.y && cy < m.y + m.h) { return &m; }
		}
		return nullptr;
	}

	// ---- X11 half (windowgeom.cpp); every call is a no-op / empty when there is no X display (headless, Wayland-only builds) ----

	// Connected monitors (XRRGetMonitors); empty if the X server or RandR is unavailable.
	std::vector<Monitor> monitors();
	// The refresh rate of the monitor under the window's centre (else the first active one) from its RandR mode, in Hz; 0 when unknown (no X
	// display, no RandR mode). Sapp thread only (sokol's display); re-asks the server at most once a second (kenshi-k16.2.205.12.87).
	double refreshHz();
	// Before sapp_desc is filled (sokol_main): the saved size for the current layout, or false. Opens its own short X connection.
	bool savedSize(int& w, int& h);
	// First frame, once the window exists: XMoveWindow to the saved position and sapp_toggle_fullscreen if it was saved fullscreen.
	void restore();
	// restore() is still arriving over the first frames (k16.2.222): the window-state gate applies its changes at once meanwhile, since the
	// window manager's placement and the saved maximize would otherwise be held for the gate's cooldown.
	bool restoring();
	// The window-state gate (windowgate.hpp) applied this size (k16.2.222): one line in windowgeom.log beside the restore path, so a launch
	// reads which state the client rendered and saved, not which resize events arrived.
	void applied(int w, int h);
	// cleanup(): writes this layout's rect, monitor and fullscreen state to the config (cfg keys window.<fp>.*).
	void save();
	// Enable/disable the whole feature for this run (flags, scripts and screenshots turn it off). Default off until armed.
	// `disarming` names the argv options the start gave that turn it off, as text ("--size 1280x720"), and is logged verbatim;
	// empty = a plain start. The default scene file a --debug run fills in is not one of them (kenshi-k16.2.235.1).
	void arm(const std::vector<std::string>& disarming);

#ifdef KCLIENT_DEV_TOOLS
	// The game's own window to w x h, as a window-manager resize would (the `resize` script op: the loading screen's re-layout check,
	// k16.2.221). False when there is no X11 window, or the size is under 1x1. Dev builds only.
	bool resizeWindow(int w, int h);
#endif
}
