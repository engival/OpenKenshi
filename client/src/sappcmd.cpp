#include "sappcmd.hpp"

#include <mutex>
#include <vector>

#include "sokol_app.h"

namespace sappcmd
{
namespace
{
enum class Kind { Lock, Show, Quit, SetClipboard, Fullscreen, Run };
struct Cmd
{
	Kind kind;
	bool flag = false;
	std::string text;
	std::function<void()> fn;	// Run
};

std::mutex gMutex;
std::vector<Cmd> gQueue;
std::vector<Cmd> gTaken;	// flush's buffer (flush runs on one thread only)
std::string gClipboard;		// the published copy, guarded by gMutex

void push(Cmd c)
{
	std::lock_guard<std::mutex> lock(gMutex);
	gQueue.push_back(std::move(c));
}
}

void lockMouse(bool lock) { push({ Kind::Lock, lock, {} }); }
void showMouse(bool show) { push({ Kind::Show, show, {} }); }
void requestQuit() { push({ Kind::Quit, false, {} }); }
void toggleFullscreen() { push({ Kind::Fullscreen, false, {} }); }
void post(std::function<void()> fn) { push({ Kind::Run, false, {}, std::move(fn) }); }
void setClipboard(const std::string& text)
{
	{
		std::lock_guard<std::mutex> lock(gMutex);
		gClipboard = text;
	}
	push({ Kind::SetClipboard, false, text });
}

void flush()
{
	{
		std::lock_guard<std::mutex> lock(gMutex);
		gTaken.swap(gQueue);
	}
	for (const Cmd& c : gTaken)
	{
		switch (c.kind)
		{
		case Kind::Lock: sapp_lock_mouse(c.flag); break;
		case Kind::Show: sapp_show_mouse(c.flag); break;
		case Kind::Quit: sapp_request_quit(); break;
		case Kind::SetClipboard: sapp_set_clipboard_string(c.text.c_str()); break;
		case Kind::Fullscreen: sapp_toggle_fullscreen(); break;
		case Kind::Run: c.fn(); break;
		}
	}
	gTaken.clear();
}

void publishClipboard()
{
	const char* c = sapp_get_clipboard_string();
	std::string s = c ? c : "";
	std::lock_guard<std::mutex> lock(gMutex);
	gClipboard = std::move(s);
}

std::string clipboard()
{
	std::lock_guard<std::mutex> lock(gMutex);
	return gClipboard;
}
}
