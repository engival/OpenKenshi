// X11 half of the window geometry memory (windowgeom.hpp). Compiled into kclient only (CMake: X11 and xrandr).
#include "windowgeom.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "config.hpp"
#include "sappcmd.hpp"
#include "sokol_app.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#undef Bool	// Xlib's macro would hide cfg::Bool; the Bool-typed X calls below take plain ints

namespace windowgeom
{
	namespace
	{
		const cfg::Bool kRemember{ cfg::Group{ "Windows" }, "window.remember", true, "Remember the game window's position, size and fullscreen state per monitor layout and restore them at the next start (X11; best effort under Wayland)" };

		bool gArmed = false, gRestored = false, gLogged = false;
		Rect gSaved;	// savedSize()'s entry, for restore()
		std::string gSavedMonitor, gSavedFp;	// the entry's layout fingerprint, so restore() can name a changed layout
		bool gHaveSaved = false, gSavedFullscreen = false, gSavedMaximized = false;

		// Why load() found nothing: the startup log names the path it took (kenshi-k16.2.200.1). One false return used to mean both.
		enum class Load
		{
			kOk,
			kNoEntry,	// no window.<fp>.rect for this layout (absent, or it does not parse)
			kOffScreen	// the saved rect's centre is on no connected monitor
		};

		std::vector<Monitor> monitorsOf(Display* dpy)
		{
			std::vector<Monitor> out;
			if (!dpy) { return out; }
			int n = 0;
			XRRMonitorInfo* m = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &n);
			for (int i = 0; m && i < n; i++)
			{
				char* nm = XGetAtomName(dpy, m[i].name);
				out.push_back({ nm ? nm : "", m[i].x, m[i].y, m[i].width, m[i].height });
				if (nm) { XFree(nm); }
			}
			if (m) { XRRFreeMonitors(m); }
			return out;
		}

		std::string key(const std::string& fp, const char* what) { return "window." + fp + "." + what; }

		std::string monitorsText(const std::vector<Monitor>& mons)
		{
			std::string s;
			for (const Monitor& m : mons)
			{
				if (!s.empty()) { s += "; "; }
				s += m.name + " " + std::to_string(m.x) + "," + std::to_string(m.y) + " " + std::to_string(m.w) + "x" + std::to_string(m.h);
			}
			return s.empty() ? "(none)" : s;
		}

		// windowgeom.log beside the config file in use; empty under KCLIENT_CONFIG=none (headless runs write nothing).
		std::string logPathFile()
		{
			const char* c = getenv("KCLIENT_CONFIG");
			if (c && std::string(c) == "none") { return std::string(); }
			const std::string p = cfg::path();
			const size_t sl = p.find_last_of('/');
			return (sl == std::string::npos ? std::string() : p.substr(0, sl + 1)) + "windowgeom.log";
		}

		// Every windowgeom line goes to stderr and, stamped with the local time and pid, to windowgeom.log (kenshi-k16.2.200.3): the terminal
		// scrollback is gone by the time a 'small window' start is looked at.
		void note(const std::string& msg)
		{
			fprintf(stderr, "windowgeom: %s\n", msg.c_str());
			const std::string f = logPathFile();
			if (f.empty()) { return; }
			char ts[32] = "";
			const time_t t = time(nullptr);
			struct tm tmv;
			if (localtime_r(&t, &tmv)) { strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv); }
			std::ofstream o(f, std::ios::app);
			if (o) { o << ts << " pid " << getpid() << " " << msg << "\n"; }
		}

		// Keeps the last 200 lines; called once at startup.
		void trimLog()
		{
			const std::string f = logPathFile();
			if (f.empty()) { return; }
			std::vector<std::string> lines;
			{
				std::ifstream in(f);
				std::string l;
				while (std::getline(in, l)) { lines.push_back(l); }
			}
			if (lines.size() <= 200) { return; }
			std::ofstream o(f, std::ios::trunc);
			for (size_t i = lines.size() - 200; i < lines.size(); i++) { o << lines[i] << "\n"; }
		}

		// One line per launch, naming the restore path it took and the layout it saw. The silent paths (no entry, off screen, no display, gave up)
		// are the ones a launch that ignored the remembered position left no trace of (user 2026-11-14, kenshi-k16.2.200.1).
		void logPath(const std::string& path, const std::string& fp, const std::vector<Monitor>& mons)
		{
			if (gLogged) { return; }
			gLogged = true;
			note(path + ", fingerprint " + fp + ", monitors " + monitorsText(mons));
		}

		// Reads this layout's entry; kNoEntry when absent, kOffScreen when its centre is off every connected monitor.
		Load load(const std::vector<Monitor>& mons, Rect& r, bool& fullscreen, bool& maximized, std::string& monitor)
		{
			const std::string fp = fingerprint(mons);
			if (!parseRect(cfg::getRaw(key(fp, "rect")), r)) { return Load::kNoEntry; }
			if (!monitorAtCentre(mons, r)) { return Load::kOffScreen; }
			fullscreen = cfg::getRaw(key(fp, "fullscreen")) == "1";
			maximized = cfg::getRaw(key(fp, "maximized")) == "1";
			monitor = cfg::getRaw(key(fp, "monitor"));
			return Load::kOk;
		}

		// _NET_FRAME_EXTENTS (left, right, top, bottom); false until the window manager has framed the window.
		bool frameExtents(Display* dpy, Window win, long ext[4])
		{
			const Atom a = XInternAtom(dpy, "_NET_FRAME_EXTENTS", True);
			if (a == None) { return false; }
			Atom type;
			int fmt;
			unsigned long n, after;
			unsigned char* data = nullptr;
			bool ok = false;
			if (XGetWindowProperty(dpy, win, a, 0, 4, False, XA_CARDINAL, &type, &fmt, &n, &after, &data) == Success && data && n == 4)
			{
				for (int i = 0; i < 4; i++) { ext[i] = ((long*)data)[i]; }
				ok = true;
			}
			if (data) { XFree(data); }
			return ok;
		}

		// Both _NET_WM_STATE_MAXIMIZED_VERT and _HORZ present.
		bool isMaximized(Display* dpy, Window win)
		{
			const Atom st = XInternAtom(dpy, "_NET_WM_STATE", True), mv = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_VERT", True), mh = XInternAtom(dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", True);
			if (st == None) { return false; }
			Atom type;
			int fmt;
			unsigned long n = 0, after;
			unsigned char* data = nullptr;
			bool v = false, h = false;
			if (XGetWindowProperty(dpy, win, st, 0, 64, False, XA_ATOM, &type, &fmt, &n, &after, &data) == Success && data)
			{
				for (unsigned long i = 0; i < n; i++)
				{
					const Atom a = ((Atom*)data)[i];
					v = v || a == mv;
					h = h || a == mh;
				}
			}
			if (data) { XFree(data); }
			return v && h;
		}

		// The outer frame's top-left on the root window (client position minus the frame extents).
		Rect frameRect(Display* dpy, Window win)
		{
			XWindowAttributes at;
			Rect r;
			if (!XGetWindowAttributes(dpy, win, &at)) { return r; }
			Window child;
			int rx = 0, ry = 0;
			XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &rx, &ry, &child);
			long e[4] = { 0, 0, 0, 0 };
			frameExtents(dpy, win, e);
			return { rx - (int)e[0], ry - (int)e[2], at.width, at.height };
		}

		void sendState(Display* dpy, Window win, const char* a1, const char* a2)
		{
			XEvent ev = {};
			ev.xclient.type = ClientMessage;
			ev.xclient.window = win;
			ev.xclient.message_type = XInternAtom(dpy, "_NET_WM_STATE", False);
			ev.xclient.format = 32;
			ev.xclient.data.l[0] = 1;	// _NET_WM_STATE_ADD
			ev.xclient.data.l[1] = (long)XInternAtom(dpy, a1, False);
			ev.xclient.data.l[2] = a2 ? (long)XInternAtom(dpy, a2, False) : 0;
			ev.xclient.data.l[3] = 1;	// source: normal application
			XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
		}
	}

	void arm(const std::vector<std::string>& disarming)
	{
		// Name every cause exactly (user 2026-11-14, kenshi-k16.2.235.1): the old one line for both falses said only "a size, script,
		// screenshot, scene or tool option was given", so a play.sh launch (--debug alone fills the default scene file) read as if a scene
		// had been asked for and nothing said which option did it.
		std::vector<std::string> why = disarming;
		const char* c = getenv("KCLIENT_CONFIG");
		if (c && std::string(c) == "none") { why.push_back("KCLIENT_CONFIG=none"); }
		if (!kRemember.get()) { why.push_back("window.remember is off"); }
		gArmed = why.empty();
		trimLog();
		if (!gArmed)
		{
			std::string causes;
			for (const std::string& s : why) { causes += (causes.empty() ? "" : ", ") + s; }
			note("startup: not armed (" + causes + "); nothing restored, nothing saved");
		}
	}

	std::vector<Monitor> monitors()
	{
		Display* dpy = XOpenDisplay(nullptr);
		std::vector<Monitor> m = monitorsOf(dpy);
		if (dpy) { XCloseDisplay(dpy); }
		return m;
	}

	double refreshHz()
	{
		Display* dpy = (Display*)sapp_x11_get_display();
		const Window win = (Window)(uintptr_t)sapp_x11_get_window();
		if (!dpy || !win) { return 0; }
		static double hz = 0, logged = -1;
		static std::chrono::steady_clock::time_point next;
		const auto now = std::chrono::steady_clock::now();
		if (now < next) { return hz; }
		next = now + std::chrono::seconds(1);	// a window dragged to another monitor is seen within a second; one round trip per second otherwise
		const Window root = DefaultRootWindow(dpy);
		XWindowAttributes wa;
		int cx = 0, cy = 0;
		Window child;
		if (XGetWindowAttributes(dpy, win, &wa)) { XTranslateCoordinates(dpy, win, root, wa.width / 2, wa.height / 2, &cx, &cy, &child); }
		double under = 0, first = 0;
		if (XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root))
		{
			for (int i = 0; i < res->ncrtc; i++)
			{
				XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, res->crtcs[i]);
				if (!ci) { continue; }
				for (int k = 0; ci->mode != None && k < res->nmode; k++)
				{
					const XRRModeInfo& m = res->modes[k];
					if (m.id != ci->mode || !m.hTotal || !m.vTotal) { continue; }
					double r = (double)m.dotClock / ((double)m.hTotal * (double)m.vTotal);
					if (m.modeFlags & RR_DoubleScan) { r /= 2; }
					if (m.modeFlags & RR_Interlace) { r *= 2; }
					if (first == 0) { first = r; }
					if (under == 0 && cx >= ci->x && cx < ci->x + (int)ci->width && cy >= ci->y && cy < ci->y + (int)ci->height) { under = r; }
				}
				XRRFreeCrtcInfo(ci);
			}
			XRRFreeScreenResources(res);
		}
		hz = under > 0 ? under : first;
		if (hz != logged)
		{
			logged = hz;
			fprintf(stderr, "windowgeom: display refresh %.2f Hz (%s)\n", hz, under > 0 ? "the monitor under the window" : first > 0 ? "the first active monitor" : "unknown");
		}
		return hz;
	}

	bool savedSize(int& w, int& h)
	{
		if (!gArmed) { return false; }
		const std::vector<Monitor> mons = monitors();
		const Load why = load(mons, gSaved, gSavedFullscreen, gSavedMaximized, gSavedMonitor);
		gHaveSaved = why == Load::kOk;
		gSavedFp = fingerprint(mons);
		if (!gHaveSaved)
		{
			logPath(why == Load::kNoEntry ? "no saved geometry for this layout" : "saved rect's centre is on no connected monitor", gSavedFp, mons);
			return false;
		}
		w = gSaved.w;
		h = gSaved.h;
		return true;
	}

	// Called every frame until it is done (a few frames at most). A window manager that places a new window on the pointer's monitor undoes a move
	// made before it has mapped and framed the window, so: wait for _NET_FRAME_EXTENTS, move (USPosition hints, client position = frame corner +
	// extents), re-check the monitor under the window every 2 frames and retry, and only then maximize / fullscreen, so that it happens on the saved monitor.
	void restore()
	{
		if (gRestored || !gArmed) { return; }
		static int frames = 0, retries = 0, settle = 0;
		static bool stateSent = false;
		if (!gHaveSaved) { gRestored = true; return; }	// savedSize() already logged why: no entry, or off every monitor
		Display* dpy = (Display*)sapp_x11_get_display();
		const Window win = (Window)(uintptr_t)sapp_x11_get_window();
		if (!dpy || !win)
		{
			gRestored = true;
			const std::vector<Monitor> mons = monitors();	// no sokol X display to ask (Wayland built sokol, or no window yet): its own short connection
			logPath("no display or window", fingerprint(mons), mons);
			return;
		}
		const std::vector<Monitor> mons = monitorsOf(dpy);
		const Monitor* want = nullptr;
		for (const Monitor& m : mons) { if (m.name == gSavedMonitor) { want = &m; } }
		if (!want) { want = monitorAtCentre(mons, gSaved); }
		Rect target = gSaved;	// the frame's corner
		if (want && monitorAtCentre(mons, target) != want)	// maximized on another monitor than the last normal rect: same offset, clamped onto the saved one
		{
			target.x = want->x + std::max(0, std::min(want->w - target.w, 40));
			target.y = want->y + std::max(0, std::min(want->h - target.h, 40));
		}
		long e[4] = { 0, 0, 0, 0 };
		const bool framed = frameExtents(dpy, win, e);
		auto landed = [&]() { const Monitor* m = monitorAtCentre(mons, frameRect(dpy, win)); return m ? m->name : std::string("none"); };
		auto finish = [&]()
		{
			gRestored = true;
			if (!gLogged)
			{
				gLogged = true;
				const std::string fp = fingerprint(mons), layout = gSavedFp == fp ? std::string() : " (the entry was saved under " + gSavedFp + ")";
				char buf[64];
				snprintf(buf, sizeof buf, "%d", retries);
				note("saved monitor " + (want ? want->name : std::string("?")) + ", landed on " + landed() + ", " + (retries ? "retry was needed" : "no retry needed") + ", retries " + buf + ", fingerprint " + fp + layout + ", monitors " + monitorsText(mons) + (gSavedMaximized ? ", saved maximized" : gSavedFullscreen ? ", saved fullscreen" : ""));
			}
		};
		frames++;
		if (stateSent)
		{
			if (++settle >= 6) { finish(); }	// let the WM apply the state before the landing monitor is read
			return;
		}
		if (!framed && frames < 20) { return; }	// not framed yet
		static int moves = 0;
		if (frames > 60)	// give up on the monitor: the window stays where the WM put it, but the saved maximized / fullscreen state is still applied (kenshi-k16.2.200.4)
		{
			logPath("gave up after " + std::to_string(frames) + " frames, the window stays where the window manager put it (landed on " + landed() + ")" + (gSavedMaximized ? ", applying the saved maximized state there" : gSavedFullscreen ? ", applying the saved fullscreen state there" : ""), fingerprint(mons), mons);
			// Same path as the landed case below: stateSent keeps restoring() true through the settle frames, so windowgate applies the change at once (its bypass).
			if (gSavedMaximized) { sendState(dpy, win, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"); stateSent = true; }
			else if (gSavedFullscreen && !sapp_is_fullscreen()) { sappcmd::toggleFullscreen(); stateSent = true; }
			if (!stateSent) { gRestored = true; }
			return;
		}
		if (moves > 0 && landed() == (want ? want->name : std::string()))	// moved at least once and the WM left it on the saved monitor
		{
			if (gSavedMaximized) { sendState(dpy, win, "_NET_WM_STATE_MAXIMIZED_VERT", "_NET_WM_STATE_MAXIMIZED_HORZ"); stateSent = true; }
			else if (gSavedFullscreen && !sapp_is_fullscreen()) { sappcmd::toggleFullscreen(); stateSent = true; }
			if (!stateSent) { finish(); }
			return;
		}
		if (moves == 0 || frames % 2 == 0)
		{
			XSizeHints hints = {};
			long supplied = 0;
			XGetWMNormalHints(dpy, win, &hints, &supplied);
			hints.flags |= USPosition | PPosition | PWinGravity;
			hints.win_gravity = StaticGravity;
			hints.x = target.x + (int)e[0];
			hints.y = target.y + (int)e[2];
			XSetWMNormalHints(dpy, win, &hints);
			XMoveWindow(dpy, win, target.x + (int)e[0], target.y + (int)e[2]);
			XFlush(dpy);
			if (moves++ > 0) { retries++; }
		}
	}

	bool restoring() { return gArmed && !gRestored; }

	void applied(int w, int h)
	{
		if (!gArmed) { return; }	// a disarmed run (scripts, shots, headless) keeps the log quiet, as every other windowgeom note does
		note("gate: applied " + std::to_string(w) + "x" + std::to_string(h));
	}

	void save()
	{
		if (!gArmed) { note("exit: save skipped, not armed"); return; }
		// An exit before restore() finished (quit within the first frames) would write the window as the WM first placed it, normal size and not
		// maximized, over the good entry: the next start comes up small.
		if (gHaveSaved && !gRestored) { note("exit: save skipped, the exit came before the restore finished (the saved entry is kept)"); return; }
		Display* dpy = (Display*)sapp_x11_get_display();
		const Window win = (Window)(uintptr_t)sapp_x11_get_window();
		if (!dpy || !win) { note("exit: save skipped, no display or window"); return; }
		const std::vector<Monitor> mons = monitorsOf(dpy);
		if (mons.empty()) { note("exit: save skipped, no monitors"); return; }
		const std::string fp = fingerprint(mons);
		const bool fs = sapp_is_fullscreen();
		const bool maxed = !fs && isMaximized(dpy, win);
		cfg::setRaw(key(fp, "fullscreen"), fs ? "1" : "0");
		cfg::setRaw(key(fp, "maximized"), maxed ? "1" : "0");
		const Rect r = frameRect(dpy, win);
		const Monitor* m = monitorAtCentre(mons, r);
		if (m) { cfg::setRaw(key(fp, "monitor"), m->name); }
		const std::string head = "exit: save fingerprint " + fp + ", monitor " + (m ? m->name : std::string("none")) + ", fullscreen " + (fs ? "1" : "0") + ", maximized " + (maxed ? "1" : "0") + ", frame rect " + formatRect(r);
		// Fullscreen or maximized: the rect is the monitor's or the work area's; keep the last normal one (the first save writes it anyway, as an entry).
		if ((fs || maxed) && !cfg::getRaw(key(fp, "rect")).empty()) { note(head + ", rect kept (" + cfg::getRaw(key(fp, "rect")) + ")"); return; }
		cfg::setRaw(key(fp, "rect"), formatRect(r));
		note(head + ", rect written");
	}

#ifdef KCLIENT_DEV_TOOLS
	// The `resize` script op (k16.2.221): the client's own window to w x h. Not gated on gArmed: a dev check may run with geometry memory off.
	bool resizeWindow(int w, int h)
	{
		Display* dpy = (Display*)sapp_x11_get_display();
		const Window win = (Window)(uintptr_t)sapp_x11_get_window();
		if (!dpy || !win || w < 1 || h < 1) { return false; }
		XResizeWindow(dpy, win, (unsigned)w, (unsigned)h);
		XFlush(dpy);
		return true;
	}
#endif
}
