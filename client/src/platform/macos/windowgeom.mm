// Cocoa half of the window geometry memory (windowgeom.hpp), from the macOS tester's Metal port (kenshi-379.32.13). Compiled into kclient on
// Apple only (CMake), in place of the X11 windowgeom.cpp. AppKit keeps the frame itself (NSWindow's frame autosave under one name in the user
// defaults), so savedSize() has nothing to hand sokol_app before the window exists and restore() puts size and position back once it does.
#include "platform/windowgeom.hpp"
#include "sokol_app.h"
#import <AppKit/AppKit.h>

namespace windowgeom
{
	namespace
	{
		bool gArmed = false;
		NSString* const kFrameName = @"OpenKenshiMainWindow";
		NSWindow* window() { return (__bridge NSWindow*)sapp_macos_get_window(); }
	}

	std::vector<Monitor> monitors()
	{
		std::vector<Monitor> out;
		for (NSScreen* screen in NSScreen.screens)
		{
			const NSRect r = screen.frame;
			out.push_back({ screen.localizedName.UTF8String, (int)r.origin.x, (int)r.origin.y, (int)r.size.width, (int)r.size.height });
		}
		return out;
	}

	double refreshHz() { return window() ? (double)window().screen.maximumFramesPerSecond : 0; }
	void arm(const std::vector<std::string>& disarming) { gArmed = disarming.empty(); }
	bool savedSize(int&, int&) { return false; }	// AppKit restores size and position together once the window exists (restore)

	void restore()
	{
		static bool restored = false;
		if (gArmed && !restored && window())
		{
			[window() setFrameUsingName:kFrameName];
			restored = true;
		}
	}

	bool restoring() { return false; }
	void applied(int, int) {}
	void save()
	{
		if (gArmed && window()) { [window() saveFrameUsingName:kFrameName]; }
	}

#ifdef KCLIENT_DEV_TOOLS
	bool resizeWindow(int w, int h)
	{
		if (!window() || w < 1 || h < 1) { return false; }
		[window() setContentSize:NSMakeSize(w, h)];
		return true;
	}
#endif
}
