// Win32 half of the window geometry memory (windowgeom.hpp), kenshi-379.31.1. Compiled into kclient on Windows only (CMake), in place of the X11
// windowgeom.cpp. The same cfg entries as on Linux (window.<fingerprint>.rect / maximized / fullscreen / monitor, the rect being the outer frame
// in virtual-screen coordinates), but no windowgeom.log: savedSize() hands sokol_app nothing, restore() sets the frame (size and position
// together) once the window exists, as the macOS half does. Untested on a real Windows desktop.
#include "platform/windowgeom.hpp"

#include <cstdlib>

#include "app/config.hpp"
#include "app/sappcmd.hpp"
#include "sokol_app.h"

#include <windows.h>

namespace windowgeom
{
	namespace
	{
		const cfg::Bool kRemember{ cfg::Group{ "Windows" }, "window.remember", true, "Remember the game window's position, size and fullscreen state per monitor layout and restore them at the next start" };

		bool gArmed = false, gRestored = false;

		std::string key(const std::string& fp, const char* what) { return "window." + fp + "." + what; }
		HWND window() { return (HWND)sapp_win32_get_hwnd(); }

		std::string narrow(const wchar_t* w)
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1) { return {}; }
			std::string s((size_t)n - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
			return s;
		}

		BOOL CALLBACK addMonitor(HMONITOR hm, HDC, LPRECT, LPARAM user)
		{
			MONITORINFOEXW mi = {};
			mi.cbSize = sizeof mi;
			if (GetMonitorInfoW(hm, &mi))
			{
				const RECT& r = mi.rcMonitor;
				reinterpret_cast<std::vector<Monitor>*>(user)->push_back({ narrow(mi.szDevice), (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top) });
			}
			return TRUE;
		}
	}

	std::vector<Monitor> monitors()
	{
		std::vector<Monitor> out;
		EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&out));
		return out;
	}

	double refreshHz()
	{
		const HWND w = window();
		if (!w) { return 0; }
		MONITORINFOEXW mi = {};
		mi.cbSize = sizeof mi;
		if (!GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi)) { return 0; }
		DEVMODEW dm = {};
		dm.dmSize = sizeof dm;
		if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) { return 0; }	// 0 / 1: "the hardware default"
		return (double)dm.dmDisplayFrequency;
	}

	void arm(const std::vector<std::string>& disarming)
	{
		const char* c = getenv("KCLIENT_CONFIG");
		gArmed = disarming.empty() && !(c && std::string(c) == "none") && kRemember.get();
	}

	bool savedSize(int&, int&) { return false; }	// restore() sets size and position together once the window exists

	void restore()
	{
		if (gRestored || !gArmed) { return; }
		const HWND w = window();
		if (!w) { return; }
		gRestored = true;
		const std::vector<Monitor> mons = monitors();
		const std::string fp = fingerprint(mons);
		Rect r;
		if (!parseRect(cfg::getRaw(key(fp, "rect")), r) || !monitorAtCentre(mons, r)) { return; }	// no entry, or off every connected monitor
		SetWindowPos(w, nullptr, r.x, r.y, r.w, r.h, SWP_NOZORDER | SWP_NOACTIVATE);
		if (cfg::getRaw(key(fp, "maximized")) == "1") { ShowWindow(w, SW_MAXIMIZE); }
		else if (cfg::getRaw(key(fp, "fullscreen")) == "1" && !sapp_is_fullscreen()) { sappcmd::toggleFullscreen(); }
	}

	bool restoring() { return false; }	// restore() is one call, nothing arrives over later frames
	void applied(int, int) {}

	void save()
	{
		const HWND w = window();
		if (!gArmed || !w) { return; }
		const std::vector<Monitor> mons = monitors();
		if (mons.empty()) { return; }
		const std::string fp = fingerprint(mons);
		const bool fs = sapp_is_fullscreen();
		const bool maxed = !fs && IsZoomed(w);
		cfg::setRaw(key(fp, "fullscreen"), fs ? "1" : "0");
		cfg::setRaw(key(fp, "maximized"), maxed ? "1" : "0");
		RECT wr;
		if (!GetWindowRect(w, &wr)) { return; }
		const Rect r = { (int)wr.left, (int)wr.top, (int)(wr.right - wr.left), (int)(wr.bottom - wr.top) };
		if (const Monitor* m = monitorAtCentre(mons, r)) { cfg::setRaw(key(fp, "monitor"), m->name); }
		// Fullscreen or maximized: the rect is the monitor's or the work area's; keep the last normal one (the first save writes it anyway).
		if ((fs || maxed) && !cfg::getRaw(key(fp, "rect")).empty()) { return; }
		cfg::setRaw(key(fp, "rect"), formatRect(r));
	}

#ifdef KCLIENT_DEV_TOOLS
	bool resizeWindow(int w, int h)
	{
		const HWND hw = window();
		if (!hw || w < 1 || h < 1) { return false; }
		RECT r = { 0, 0, w, h };	// w x h is the client area: grow it by the frame
		AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(hw, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(hw, GWL_EXSTYLE));
		return SetWindowPos(hw, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
	}
#endif
}
